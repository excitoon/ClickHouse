#include <Storages/MergeTree/MergeTreeIndexConditionBloomFilter.h>
#include <Interpreters/QueryNormalizer.h>
#include <Interpreters/BloomFilterHash.h>
#include <Common/HashTable/ClearableHashMap.h>
#include <Storages/MergeTree/RPNBuilder.h>
#include <Storages/MergeTree/MergeTreeIndexGranuleBloomFilter.h>
#include <DataTypes/DataTypeTuple.h>
#include <Columns/ColumnConst.h>
#include <ext/bit_cast.h>
#include <Parsers/ASTSubquery.h>
#include <Parsers/ASTIdentifier.h>
#include <Columns/ColumnTuple.h>


namespace DB
{

namespace
{

PreparedSetKey getPreparedSetKey(const ASTPtr & node, const DataTypePtr & data_type)
{
    /// If the data type is tuple, let's try unbox once
    if (node->as<ASTSubquery>() || node->as<ASTIdentifier>())
        return PreparedSetKey::forSubquery(*node);

    if (const auto * date_type_tuple = typeid_cast<const DataTypeTuple *>(&*data_type))
        return PreparedSetKey::forLiteral(*node, date_type_tuple->getElements());

    return PreparedSetKey::forLiteral(*node, DataTypes(1, data_type));
}

bool maybeTrueOnBloomFilter(const IColumn * hash_column, const BloomFilterPtr & bloom_filter, size_t hash_functions)
{
    const auto const_column = typeid_cast<const ColumnConst *>(hash_column);
    const auto non_const_column = typeid_cast<const ColumnUInt64 *>(hash_column);

    if (!const_column && !non_const_column)
        throw Exception("LOGICAL ERROR: hash column must be Const Column or UInt64 Column.", ErrorCodes::LOGICAL_ERROR);

    if (const_column)
    {
        for (size_t index = 0; index < hash_functions; ++index)
            if (!bloom_filter->containsWithSeed(const_column->getValue<UInt64>(), BloomFilterHash::bf_hash_seed[index]))
                return false;
        return true;
    }
    else
    {
        bool missing_rows = true;
        const ColumnUInt64::Container & data = non_const_column->getData();

        for (size_t index = 0, size = data.size(); missing_rows && index < size; ++index)
        {
            bool match_row = true;
            for (size_t hash_index = 0; match_row && hash_index < hash_functions; ++hash_index)
                match_row = bloom_filter->containsWithSeed(data[index], BloomFilterHash::bf_hash_seed[hash_index]);

            missing_rows = !match_row;
        }

        return !missing_rows;
    }
}

}

MergeTreeIndexConditionBloomFilter::MergeTreeIndexConditionBloomFilter(
    const SelectQueryInfo & info, const Context & context, const Block & header, size_t hash_functions)
    : header(header), query_info(info), hash_functions(hash_functions)
{
    auto atomFromAST = [this](auto & node, auto &, auto & constants, auto & out) { return traverseAtomAST(node, constants, out); };
    rpn = std::move(RPNBuilder<RPNElement>(info, context, atomFromAST).extractRPN());
}

bool MergeTreeIndexConditionBloomFilter::alwaysUnknownOrTrue() const
{
    std::vector<bool> rpn_stack;

    for (const auto & element : rpn)
    {
        if (element.function == RPNElement::FUNCTION_UNKNOWN
            || element.function == RPNElement::ALWAYS_TRUE)
        {
            rpn_stack.push_back(true);
        }
        else if (element.function == RPNElement::FUNCTION_EQUALS
                 || element.function == RPNElement::FUNCTION_NOT_EQUALS
                 || element.function == RPNElement::FUNCTION_IN
                 || element.function == RPNElement::FUNCTION_NOT_IN
                 || element.function == RPNElement::ALWAYS_FALSE)
        {
            rpn_stack.push_back(false);
        }
        else if (element.function == RPNElement::FUNCTION_NOT)
        {
            // do nothing
        }
        else if (element.function == RPNElement::FUNCTION_AND)
        {
            auto arg1 = rpn_stack.back();
            rpn_stack.pop_back();
            auto arg2 = rpn_stack.back();
            rpn_stack.back() = arg1 && arg2;
        }
        else if (element.function == RPNElement::FUNCTION_OR)
        {
            auto arg1 = rpn_stack.back();
            rpn_stack.pop_back();
            auto arg2 = rpn_stack.back();
            rpn_stack.back() = arg1 || arg2;
        }
        else
            throw Exception("Unexpected function type in KeyCondition::RPNElement", ErrorCodes::LOGICAL_ERROR);
    }

    return rpn_stack[0];
}

bool MergeTreeIndexConditionBloomFilter::mayBeTrueOnGranule(const MergeTreeIndexGranuleBloomFilter * granule) const
{
    std::vector<BoolMask> rpn_stack;
    const auto & filters = granule->getFilters();

    for (const auto & element : rpn)
    {
        if (element.function == RPNElement::FUNCTION_UNKNOWN)
        {
            rpn_stack.emplace_back(true, true);
        }
        else if (element.function == RPNElement::FUNCTION_IN
            || element.function == RPNElement::FUNCTION_NOT_IN
            || element.function == RPNElement::FUNCTION_EQUALS
            || element.function == RPNElement::FUNCTION_NOT_EQUALS)
        {
            bool match_rows = true;
            const auto & predicate = element.predicate;
            for (size_t index = 0; match_rows && index < predicate.size(); ++index)
            {
                const auto & query_index_hash = predicate[index];
                const auto & filter = filters[query_index_hash.first];
                const ColumnPtr & hash_column = query_index_hash.second;
                match_rows = maybeTrueOnBloomFilter(&*hash_column, filter, hash_functions);
            }

            rpn_stack.emplace_back(match_rows, !match_rows);
            if (element.function == RPNElement::FUNCTION_NOT_EQUALS || element.function == RPNElement::FUNCTION_NOT_IN)
                rpn_stack.back() = !rpn_stack.back();
        }
        else if (element.function == RPNElement::FUNCTION_NOT)
        {
            rpn_stack.back() = !rpn_stack.back();
        }
        else if (element.function == RPNElement::FUNCTION_OR)
        {
            auto arg1 = rpn_stack.back();
            rpn_stack.pop_back();
            auto arg2 = rpn_stack.back();
            rpn_stack.back() = arg1 | arg2;
        }
        else if (element.function == RPNElement::FUNCTION_AND)
        {
            auto arg1 = rpn_stack.back();
            rpn_stack.pop_back();
            auto arg2 = rpn_stack.back();
            rpn_stack.back() = arg1 & arg2;
        }
        else if (element.function == RPNElement::ALWAYS_TRUE)
        {
            rpn_stack.emplace_back(true, false);
        }
        else if (element.function == RPNElement::ALWAYS_FALSE)
        {
            rpn_stack.emplace_back(false, true);
        }
        else
            throw Exception("Unexpected function type in KeyCondition::RPNElement", ErrorCodes::LOGICAL_ERROR);
    }

    if (rpn_stack.size() != 1)
        throw Exception("Unexpected stack size in KeyCondition::mayBeTrueInRange", ErrorCodes::LOGICAL_ERROR);

    return rpn_stack[0].can_be_true;
}

bool MergeTreeIndexConditionBloomFilter::traverseAtomAST(const ASTPtr & node, Block & block_with_constants, RPNElement & out)
{
    {
        Field const_value;
        DataTypePtr const_type;
        if (KeyCondition::getConstant(node, block_with_constants, const_value, const_type))
        {
            if (const_value.getType() == Field::Types::UInt64 || const_value.getType() == Field::Types::Int64 ||
                const_value.getType() == Field::Types::Float64)
            {
                /// Zero in all types is represented in memory the same way as in UInt64.
                out.function = const_value.get<UInt64>() ? RPNElement::ALWAYS_TRUE : RPNElement::ALWAYS_FALSE;
                return true;
            }
        }
    }

    if (const auto * function = node->as<ASTFunction>())
    {
        const ASTs & arguments = function->arguments->children;

        if (arguments.size() != 2)
            return false;

        if (functionIsInOrGlobalInOperator(function->name))
            return processInOrNotInOperator(function->name, arguments[0], arguments[1], out);

        if (function->name == "equals" || function->name  == "notEquals")
        {
            Field const_value;
            DataTypePtr const_type;
            if (KeyCondition::getConstant(arguments[1], block_with_constants, const_value, const_type))
                return processEqualsOrNotEquals(function->name, arguments[0], const_type, const_value, out);
            else if (KeyCondition::getConstant(arguments[0], block_with_constants, const_value, const_type))
                return processEqualsOrNotEquals(function->name, arguments[1], const_type, const_value, out);
        }
    }

    return false;
}

bool MergeTreeIndexConditionBloomFilter::processInOrNotInOperator(
    const String & function_name, const ASTPtr & key_ast, const ASTPtr & expr_list, RPNElement & out)
{
    if (header.has(key_ast->getColumnName()))
    {
        const auto & column_and_type = header.getByName(key_ast->getColumnName());
        const auto & prepared_set_it = query_info.sets.find(getPreparedSetKey(expr_list, column_and_type.type));

        if (prepared_set_it != query_info.sets.end() && prepared_set_it->second->hasExplicitSetElements())
        {
            const IDataType * type = &*column_and_type.type;
            const auto & prepared_set = prepared_set_it->second;

            if (!typeid_cast<const DataTypeTuple *>(type))
            {
                const Columns & columns = prepared_set->getSetElements();

                if (columns.size() != 1)
                    throw Exception("LOGICAL ERROR: prepared_set columns size must be 1.", ErrorCodes::LOGICAL_ERROR);

                ColumnPtr column = columns[0];
                size_t position = header.getPositionByName(key_ast->getColumnName());
                out.predicate.emplace_back(std::make_pair(position, BloomFilterHash::hashWithColumn(type, &*column, 0, column->size())));
            }
            else
            {
                size_t position = header.getPositionByName(key_ast->getColumnName());
                const auto & tuple_column = ColumnTuple::create(prepared_set->getSetElements());
                const auto & bf_hash_column = BloomFilterHash::hashWithColumn(type, &*tuple_column, 0, prepared_set->getTotalRowCount());
                out.predicate.emplace_back(std::make_pair(position, bf_hash_column));
            }

            if (function_name == "in"  || function_name == "globalIn")
                out.function = RPNElement::FUNCTION_IN;

            if (function_name == "notIn"  || function_name == "globalNotIn")
                out.function = RPNElement::FUNCTION_NOT_IN;

            return true;
        }
    }

    return false;
}

bool MergeTreeIndexConditionBloomFilter::processEqualsOrNotEquals(
    const String & function_name, const ASTPtr & key_ast, const DataTypePtr & value_type, const Field & value_field, RPNElement & out)
{
    if (header.has(key_ast->getColumnName()))
    {
        size_t position = header.getPositionByName(key_ast->getColumnName());
        out.predicate.emplace_back(std::make_pair(position, BloomFilterHash::hashWithField(&*value_type, value_field)));
        out.function = function_name == "equals" ? RPNElement::FUNCTION_EQUALS : RPNElement::FUNCTION_NOT_EQUALS;
        return true;
    }

    if (const auto * function = key_ast->as<ASTFunction>())
    {
        WhichDataType which(value_type);

        /// TODO: support SQL: where array(index_column_x, column_y) = [1, 2]
        if (which.isTuple() && function->name == "tuple")
        {
            const TupleBackend & tuple = get<const Tuple &>(value_field).toUnderType();
            const auto value_tuple_data_type = typeid_cast<const DataTypeTuple *>(value_type.get());
            const ASTs & arguments = typeid_cast<const ASTExpressionList &>(*function->arguments).children;

            if (tuple.size() != arguments.size())
                throw Exception("Illegal types of arguments of function " + function_name, ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);

            bool match_with_subtype = false;
            const DataTypes & subtypes = value_tuple_data_type->getElements();

            for (size_t index = 0; index < tuple.size(); ++index)
                match_with_subtype |= processEqualsOrNotEquals(function_name, arguments[index], subtypes[index], tuple[index], out);

            return match_with_subtype;
        }
    }

    return false;
}

}
