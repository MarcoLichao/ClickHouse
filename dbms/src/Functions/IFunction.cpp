#include <Common/config.h>
#include <Common/typeid_cast.h>
#include <Columns/ColumnConst.h>
#include <Columns/ColumnNullable.h>
#include <DataTypes/DataTypeNothing.h>
#include <DataTypes/DataTypeNullable.h>
#include <DataTypes/Native.h>
#include <DataTypes/DataTypeWithDictionary.h>
#include <DataTypes/getLeastSupertype.h>
#include <Columns/ColumnWithDictionary.h>
#include <Functions/FunctionHelpers.h>
#include <Functions/IFunction.h>
#include <Interpreters/ExpressionActions.h>
#include <IO/WriteHelpers.h>
#include <ext/range.h>
#include <ext/collection_cast.h>
#include <cstdlib>
#include <memory>
#include <optional>

#if USE_EMBEDDED_COMPILER
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <llvm/IR/IRBuilder.h> // Y_IGNORE
#pragma GCC diagnostic pop
#endif


namespace DB
{

namespace ErrorCodes
{
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
    extern const int ILLEGAL_COLUMN;
}


namespace
{


/** Return ColumnNullable of src, with null map as OR-ed null maps of args columns in blocks.
  * Or ColumnConst(ColumnNullable) if the result is always NULL or if the result is constant and always not NULL.
  */
ColumnPtr wrapInNullable(const ColumnPtr & src, Block & block, const ColumnNumbers & args, size_t result, size_t input_rows_count)
{
    ColumnPtr result_null_map_column;

    /// If result is already nullable.
    ColumnPtr src_not_nullable = src;

    if (src->onlyNull())
        return src;
    else if (src->isColumnNullable())
    {
        src_not_nullable = static_cast<const ColumnNullable &>(*src).getNestedColumnPtr();
        result_null_map_column = static_cast<const ColumnNullable &>(*src).getNullMapColumnPtr();
    }

    for (const auto & arg : args)
    {
        const ColumnWithTypeAndName & elem = block.getByPosition(arg);
        if (!elem.type->isNullable())
            continue;

        /// Const Nullable that are NULL.
        if (elem.column->onlyNull())
            return block.getByPosition(result).type->createColumnConst(input_rows_count, Null());

        if (elem.column->isColumnConst())
            continue;

        if (elem.column->isColumnNullable())
        {
            const ColumnPtr & null_map_column = static_cast<const ColumnNullable &>(*elem.column).getNullMapColumnPtr();
            if (!result_null_map_column)
            {
                result_null_map_column = null_map_column;
            }
            else
            {
                MutableColumnPtr mutable_result_null_map_column = (*std::move(result_null_map_column)).mutate();

                NullMap & result_null_map = static_cast<ColumnUInt8 &>(*mutable_result_null_map_column).getData();
                const NullMap & src_null_map = static_cast<const ColumnUInt8 &>(*null_map_column).getData();

                for (size_t i = 0, size = result_null_map.size(); i < size; ++i)
                    if (src_null_map[i])
                        result_null_map[i] = 1;

                result_null_map_column = std::move(mutable_result_null_map_column);
            }
        }
    }

    if (!result_null_map_column)
        return makeNullable(src);

    if (src_not_nullable->isColumnConst())
        return ColumnNullable::create(src_not_nullable->convertToFullColumnIfConst(), result_null_map_column);
    else
        return ColumnNullable::create(src_not_nullable, result_null_map_column);
}


struct NullPresence
{
    bool has_nullable = false;
    bool has_null_constant = false;
};

NullPresence getNullPresense(const Block & block, const ColumnNumbers & args)
{
    NullPresence res;

    for (const auto & arg : args)
    {
        const auto & elem = block.getByPosition(arg);

        if (!res.has_nullable)
            res.has_nullable = elem.type->isNullable();
        if (!res.has_null_constant)
            res.has_null_constant = elem.type->onlyNull();
    }

    return res;
}

NullPresence getNullPresense(const ColumnsWithTypeAndName & args)
{
    NullPresence res;

    for (const auto & elem : args)
    {
        if (!res.has_nullable)
            res.has_nullable = elem.type->isNullable();
        if (!res.has_null_constant)
            res.has_null_constant = elem.type->onlyNull();
    }

    return res;
}

bool allArgumentsAreConstants(const Block & block, const ColumnNumbers & args)
{
    for (auto arg : args)
        if (!block.getByPosition(arg).column->isColumnConst())
            return false;
    return true;
}
}

bool PreparedFunctionImpl::defaultImplementationForConstantArguments(Block & block, const ColumnNumbers & args, size_t result,
                                                                     size_t input_rows_count)
{
    ColumnNumbers arguments_to_remain_constants = getArgumentsThatAreAlwaysConstant();

    /// Check that these arguments are really constant.
    for (auto arg_num : arguments_to_remain_constants)
        if (arg_num < args.size() && !block.getByPosition(args[arg_num]).column->isColumnConst())
            throw Exception("Argument at index " + toString(arg_num) + " for function " + getName() + " must be constant", ErrorCodes::ILLEGAL_COLUMN);

    if (args.empty() || !useDefaultImplementationForConstants() || !allArgumentsAreConstants(block, args))
        return false;

    Block temporary_block;
    bool have_converted_columns = false;

    size_t arguments_size = args.size();
    for (size_t arg_num = 0; arg_num < arguments_size; ++arg_num)
    {
        const ColumnWithTypeAndName & column = block.getByPosition(args[arg_num]);

        if (arguments_to_remain_constants.end() != std::find(arguments_to_remain_constants.begin(), arguments_to_remain_constants.end(), arg_num))
            temporary_block.insert(column);
        else
        {
            have_converted_columns = true;
            temporary_block.insert({ static_cast<const ColumnConst *>(column.column.get())->getDataColumnPtr(), column.type, column.name });
        }
    }

    /** When using default implementation for constants, the function requires at least one argument
      *  not in "arguments_to_remain_constants" set. Otherwise we get infinite recursion.
      */
    if (!have_converted_columns)
        throw Exception("Number of arguments for function " + getName() + " doesn't match: the function requires more arguments",
            ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);

    temporary_block.insert(block.getByPosition(result));

    ColumnNumbers temporary_argument_numbers(arguments_size);
    for (size_t i = 0; i < arguments_size; ++i)
        temporary_argument_numbers[i] = i;

    executeWithoutColumnsWithDictionary(temporary_block, temporary_argument_numbers, arguments_size, temporary_block.rows());

    block.getByPosition(result).column = ColumnConst::create(temporary_block.getByPosition(arguments_size).column, input_rows_count);
    return true;
}


bool PreparedFunctionImpl::defaultImplementationForNulls(Block & block, const ColumnNumbers & args, size_t result,
                                                         size_t input_rows_count)
{
    if (args.empty() || !useDefaultImplementationForNulls())
        return false;

    NullPresence null_presence = getNullPresense(block, args);

    if (null_presence.has_null_constant)
    {
        block.getByPosition(result).column = block.getByPosition(result).type->createColumnConst(input_rows_count, Null());
        return true;
    }

    if (null_presence.has_nullable)
    {
        Block temporary_block = createBlockWithNestedColumns(block, args, result);
        executeWithoutColumnsWithDictionary(temporary_block, args, result, temporary_block.rows());
        block.getByPosition(result).column = wrapInNullable(temporary_block.getByPosition(result).column, block, args,
                                                            result, input_rows_count);
        return true;
    }

    return false;
}

void PreparedFunctionImpl::executeWithoutColumnsWithDictionary(Block & block, const ColumnNumbers & args, size_t result, size_t input_rows_count)
{
    if (defaultImplementationForConstantArguments(block, args, result, input_rows_count))
        return;

    if (defaultImplementationForNulls(block, args, result, input_rows_count))
        return;

    executeImpl(block, args, result, input_rows_count);
}

static ColumnPtr replaceColumnsWithDictionaryByNestedAndGetDictionaryIndexes(Block & block, const ColumnNumbers & args, 
                                                                             bool can_be_executed_on_default_arguments)
{
    size_t num_rows = 0;
    ColumnPtr indexes;

    for (auto arg : args)
    {
        ColumnWithTypeAndName & column = block.getByPosition(arg);
        if (auto * column_with_dict = checkAndGetColumn<ColumnWithDictionary>(column.column.get()))
        {
            if (indexes)
                throw Exception("Expected single dictionary argument for function.", ErrorCodes::LOGICAL_ERROR);

            indexes = column_with_dict->getIndexesPtr();
            num_rows = column_with_dict->getDictionary().size();
        }
    }

    if (!indexes)
        throw Exception("Expected column with dictionary for any function argument.", ErrorCodes::LOGICAL_ERROR);

    for (auto arg : args)
    {
        ColumnWithTypeAndName & column = block.getByPosition(arg);
        if (auto * column_const = checkAndGetColumn<ColumnConst>(column.column.get()))
            column.column = column_const->cloneResized(num_rows);
        else if (auto * column_with_dict = checkAndGetColumn<ColumnWithDictionary>(column.column.get()))
        {
            auto * type_with_dict = checkAndGetDataType<DataTypeWithDictionary>(column.type.get());

            if (!type_with_dict)
                throw Exception("Incompatible type for column with dictionary: " + column.type->getName(),
                                ErrorCodes::LOGICAL_ERROR);

            if (can_be_executed_on_default_arguments)
                column.column = column_with_dict->getDictionary().getNestedColumn();
            else
            {
                auto dict_encoded = column_with_dict->getMinimalDictionaryEncodedColumn(0, column_with_dict->size());
                column.column = dict_encoded.dictionary;
                indexes = dict_encoded.indexes;
            }
            column.type = type_with_dict->getDictionaryType();
        }
    }

    return indexes;
}

static void convertColumnsWithDictionaryToFull(Block & block, const ColumnNumbers & args)
{
    for (auto arg : args)
    {
        ColumnWithTypeAndName & column = block.getByPosition(arg);
        if (auto * column_with_dict = checkAndGetColumn<ColumnWithDictionary>(column.column.get()))
        {
            auto * type_with_dict = checkAndGetDataType<DataTypeWithDictionary>(column.type.get());

            if (!type_with_dict)
                throw Exception("Incompatible type for column with dictionary: " + column.type->getName(),
                                ErrorCodes::LOGICAL_ERROR);

            column.column = column_with_dict->convertToFullColumn();
            column.type = type_with_dict->getDictionaryType();
        }
    }
}

void PreparedFunctionImpl::execute(Block & block, const ColumnNumbers & args, size_t result, size_t input_rows_count)
{
    if (useDefaultImplementationForColumnsWithDictionary())
    {
        auto & res = block.safeGetByPosition(result);
        Block block_without_dicts = block.cloneEmpty();

        for (auto arg : args)
            block_without_dicts.safeGetByPosition(arg).column = block.safeGetByPosition(arg).column;

        if (res.type->withDictionary())
        {
            ColumnPtr indexes = replaceColumnsWithDictionaryByNestedAndGetDictionaryIndexes(
                    block_without_dicts, args, canBeExecutedOnDefaultArguments());

            executeWithoutColumnsWithDictionary(block_without_dicts, args, result, block_without_dicts.rows());

            auto res_column = res.type->createColumn();
            auto * column_with_dictionary = typeid_cast<ColumnWithDictionary *>(res_column.get());

            if (!column_with_dictionary)
                throw Exception("Expected ColumnWithDictionary, got" + res_column->getName(), ErrorCodes::LOGICAL_ERROR);

            const auto & keys = block_without_dicts.safeGetByPosition(result).column;
            column_with_dictionary->insertRangeFromDictionaryEncodedColumn(*keys, *indexes);

            res.column = std::move(res_column);
        }
        else
        {
            convertColumnsWithDictionaryToFull(block_without_dicts, args);
            executeWithoutColumnsWithDictionary(block_without_dicts, args, result, block_without_dicts.rows());
            res.column = block_without_dicts.safeGetByPosition(result).column;
        }
    }
    else
        executeWithoutColumnsWithDictionary(block, args, result, input_rows_count);
}

void FunctionBuilderImpl::checkNumberOfArguments(size_t number_of_arguments) const
{
    if (isVariadic())
        return;

    size_t expected_number_of_arguments = getNumberOfArguments();

    if (number_of_arguments != expected_number_of_arguments)
        throw Exception("Number of arguments for function " + getName() + " doesn't match: passed "
                        + toString(number_of_arguments) + ", should be " + toString(expected_number_of_arguments),
                        ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);
}

DataTypePtr FunctionBuilderImpl::getReturnTypeWithoutDictionary(const ColumnsWithTypeAndName & arguments) const
{
    checkNumberOfArguments(arguments.size());

    if (!arguments.empty() && useDefaultImplementationForNulls())
    {
        NullPresence null_presence = getNullPresense(arguments);

        if (null_presence.has_null_constant)
        {
            return makeNullable(std::make_shared<DataTypeNothing>());
        }
        if (null_presence.has_nullable)
        {
            Block nested_block = createBlockWithNestedColumns(Block(arguments), ext::collection_cast<ColumnNumbers>(ext::range(0, arguments.size())));
            auto return_type = getReturnTypeImpl(ColumnsWithTypeAndName(nested_block.begin(), nested_block.end()));
            return makeNullable(return_type);

        }
    }

    return getReturnTypeImpl(arguments);
}

#if USE_EMBEDDED_COMPILER

static std::optional<DataTypes> removeNullables(const DataTypes & types)
{
    for (const auto & type : types)
    {
        if (!typeid_cast<const DataTypeNullable *>(type.get()))
            continue;
        DataTypes filtered;
        for (const auto & type : types)
            filtered.emplace_back(removeNullable(type));
        return filtered;
    }
    return {};
}

bool IFunction::isCompilable(const DataTypes & arguments) const
{
    if (useDefaultImplementationForNulls())
        if (auto denulled = removeNullables(arguments))
            return isCompilableImpl(*denulled);
    return isCompilableImpl(arguments);
}

llvm::Value * IFunction::compile(llvm::IRBuilderBase & builder, const DataTypes & arguments, ValuePlaceholders values) const
{
    if (useDefaultImplementationForNulls())
    {
        if (auto denulled = removeNullables(arguments))
        {
            /// FIXME: when only one column is nullable, this can actually be slower than the non-jitted version
            ///        because this involves copying the null map while `wrapInNullable` reuses it.
            auto & b = static_cast<llvm::IRBuilder<> &>(builder);
            auto * fail = llvm::BasicBlock::Create(b.GetInsertBlock()->getContext(), "", b.GetInsertBlock()->getParent());
            auto * join = llvm::BasicBlock::Create(b.GetInsertBlock()->getContext(), "", b.GetInsertBlock()->getParent());
            auto * zero = llvm::Constant::getNullValue(toNativeType(b, makeNullable(getReturnTypeImpl(*denulled))));
            for (size_t i = 0; i < arguments.size(); i++)
            {
                if (!arguments[i]->isNullable())
                    continue;
                /// Would be nice to evaluate all this lazily, but that'd change semantics: if only unevaluated
                /// arguments happen to contain NULLs, the return value would not be NULL, though it should be.
                auto * value = values[i]();
                auto * ok = llvm::BasicBlock::Create(b.GetInsertBlock()->getContext(), "", b.GetInsertBlock()->getParent());
                b.CreateCondBr(b.CreateExtractValue(value, {1}), fail, ok);
                b.SetInsertPoint(ok);
                values[i] = [value = b.CreateExtractValue(value, {0})]() { return value; };
            }
            auto * result = b.CreateInsertValue(zero, compileImpl(builder, *denulled, std::move(values)), {0});
            auto * result_block = b.GetInsertBlock();
            b.CreateBr(join);
            b.SetInsertPoint(fail);
            auto * null = b.CreateInsertValue(zero, b.getTrue(), {1});
            b.CreateBr(join);
            b.SetInsertPoint(join);
            auto * phi = b.CreatePHI(result->getType(), 2);
            phi->addIncoming(result, result_block);
            phi->addIncoming(null, fail);
            return phi;
        }
    }
    return compileImpl(builder, arguments, std::move(values));
}

#endif


DataTypePtr FunctionBuilderImpl::getReturnType(const ColumnsWithTypeAndName & arguments) const
{
    if (useDefaultImplementationForColumnsWithDictionary())
    {
        bool has_type_with_dictionary = false;
        bool can_run_function_on_dictionary = true;

        ColumnsWithTypeAndName args_without_dictionary(arguments);

        for (ColumnWithTypeAndName & arg : args_without_dictionary)
        {
            if (arg.column && arg.column->isColumnConst())
                continue;

            if (auto * type_with_dictionary = typeid_cast<const DataTypeWithDictionary *>(arg.type.get()))
            {
                if (has_type_with_dictionary)
                    can_run_function_on_dictionary = false;

                has_type_with_dictionary = true;
                arg.type = type_with_dictionary->getDictionaryType();
            }
            else
                can_run_function_on_dictionary = false;
        }

        if (has_type_with_dictionary && can_run_function_on_dictionary)
            return std::make_shared<DataTypeWithDictionary>(getReturnTypeWithoutDictionary(args_without_dictionary));
        else
            return getReturnTypeWithoutDictionary(args_without_dictionary);
    }

    return getReturnTypeWithoutDictionary(arguments);
}
}
