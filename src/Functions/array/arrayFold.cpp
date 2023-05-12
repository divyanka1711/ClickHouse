#include "FunctionArrayMapped.h"
#include <Functions/FunctionFactory.h>
#include <Common/Exception.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int ILLEGAL_COLUMN;
    extern const int ILLEGAL_TYPE_OF_ARGUMENT;
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
    extern const int SIZES_OF_ARRAYS_DONT_MATCH;
    extern const int TYPE_MISMATCH;
}


/** arrayFold(x1,...,xn,accum -> expression, array1,...,arrayn, init_accum) - apply the expression to each element of the array (or set of parallel arrays).
  */
class ArrayFold : public IFunction
{
public:
    static constexpr auto name = "arrayFold";
    static FunctionPtr create(ContextPtr) { return std::make_shared<ArrayFold>(); }

    bool isVariadic() const override { return true; }
    size_t getNumberOfArguments() const override { return 0; }
    bool isSuitableForShortCircuitArgumentsExecution(const DataTypesWithConstInfo & /*arguments*/) const override { return true; }

    void getLambdaArgumentTypes(DataTypes & arguments) const override
    {
        if (arguments.size() < 3)
            throw Exception(ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH, "Function {} needs lambda function, at least one array argument and one accumulator argument.", getName());
        DataTypes nested_types(arguments.size() - 1);
        for (size_t i = 0; i < nested_types.size() - 1; ++i)
        {
            const DataTypeArray * array_type = checkAndGetDataType<DataTypeArray>(&*arguments[i + 1]);
            if (!array_type)
                throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT, "Argument {} of function {} must be array. Found {} instead.", toString(i + 2), getName(), arguments[i + 1]->getName());
            nested_types[i] = recursiveRemoveLowCardinality(array_type->getNestedType());
        }
        nested_types[nested_types.size() - 1] = arguments[arguments.size() - 1];

        const DataTypeFunction * function_type = checkAndGetDataType<DataTypeFunction>(arguments[0].get());
        if (!function_type || function_type->getArgumentTypes().size() != nested_types.size())
            throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT, "First argument for this overload of {} must be a function with {} arguments. Found {} instead.",
                            getName(), toString(nested_types.size()), arguments[0]->getName());

        arguments[0] = std::make_shared<DataTypeFunction>(nested_types);
    }

    DataTypePtr getReturnTypeImpl(const ColumnsWithTypeAndName & arguments) const override
    {
        if (arguments.size() < 2)
            throw Exception(ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH, "Function {} needs at least 2 arguments; passed {}.", getName(), toString(arguments.size()));
        const auto * data_type_function = checkAndGetDataType<DataTypeFunction>(arguments[0].type.get());
        if (!data_type_function)
            throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT, "First argument for function {} must be a function.", getName());

        auto const accumulator_type = arguments.back().type;
        auto const lambda_type = data_type_function->getReturnType();
        if (! accumulator_type->equals(*lambda_type))
            throw Exception(ErrorCodes::TYPE_MISMATCH, "Return type of lambda function must be the same as the accumulator type. "
                            "Inferred type of lambda {}, inferred type of accumulator {}.", lambda_type->getName(), accumulator_type->getName());

        return DataTypePtr(accumulator_type);
    }

    ColumnPtr executeImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr &, size_t input_rows_count) const override
    {
        const auto & column_with_type_and_name = arguments[0];

        if (!column_with_type_and_name.column)
            throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT, "First argument for function {} must be a function.", getName());

        const auto * column_function = typeid_cast<const ColumnFunction *>(column_with_type_and_name.column.get());

        if (!column_function)
            throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT, "First argument for function {} must be a function.", getName());

        ColumnPtr offsets_column;
        ColumnPtr column_first_array_ptr;
        const ColumnArray * column_first_array = nullptr;
        ColumnsWithTypeAndName arrays;
        arrays.reserve(arguments.size() - 1);

        for (size_t i = 1; i < arguments.size() - 1; ++i)
        {
            const auto & array_with_type_and_name = arguments[i];
            ColumnPtr column_array_ptr = array_with_type_and_name.column;
            const auto * column_array = checkAndGetColumn<ColumnArray>(column_array_ptr.get());
            const DataTypePtr & array_type_ptr = array_with_type_and_name.type;
            const auto * array_type = checkAndGetDataType<DataTypeArray>(array_type_ptr.get());
            if (!column_array)
            {
                const ColumnConst * column_const_array = checkAndGetColumnConst<ColumnArray>(column_array_ptr.get());
                if (!column_const_array)
                    throw Exception(ErrorCodes::ILLEGAL_COLUMN, "Expected array column, found {}", column_array_ptr->getName());
                column_array_ptr = recursiveRemoveLowCardinality(column_const_array->convertToFullColumn());
                column_array = checkAndGetColumn<ColumnArray>(column_array_ptr.get());
            }
            if (!array_type)
                throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT, "Expected array type, found {}", array_type_ptr->getName());
            if (!offsets_column)
            {
                offsets_column = column_array->getOffsetsPtr();
            }
            else
            {
                /// The first condition is optimization: do not compare data if the pointers are equal.
                if (column_array->getOffsetsPtr() != offsets_column
                    && column_array->getOffsets() != typeid_cast<const ColumnArray::ColumnOffsets &>(*offsets_column).getData())
                    throw Exception(ErrorCodes::SIZES_OF_ARRAYS_DONT_MATCH, "Arrays passed to {} must have equal size", getName());
            }
            if (i == 1)
            {
                column_first_array_ptr = column_array_ptr;
                column_first_array = column_array;
            }
            arrays.emplace_back(ColumnWithTypeAndName(column_array->getDataPtr(),
                                                      recursiveRemoveLowCardinality(array_type->getNestedType()),
                                                      array_with_type_and_name.name));
        }

        ssize_t rows_count = input_rows_count;
        ssize_t data_row_count = arrays[0].column->size();
        auto array_count = arrays.size();

        if (rows_count == 0)
            return arguments.back().column->convertToFullColumnIfConst()->cloneEmpty();
        
        ColumnPtr current_column;
        current_column = arguments.back().column->convertToFullColumnIfConst();
        MutableColumnPtr result_data = arguments.back().column->convertToFullColumnIfConst()->cloneEmpty();
        
        size_t max_array_size = 0;
        auto& offsets = column_first_array->getOffsets();

        //get columns of Nth array elements
        IColumn::Selector selector(data_row_count);
        size_t cur_ind = 0;
        ssize_t cur_arr = 0;

        if (data_row_count)
            while (offsets[cur_arr] == 0)
                ++cur_arr;

        for (ssize_t i = 0; i < data_row_count; ++i)
        {
            selector[i] = cur_ind++;
            if (cur_ind > max_array_size)
                max_array_size = cur_ind;
            while (cur_arr < rows_count && cur_ind >= offsets[cur_arr] - offsets[cur_arr - 1])
            {
                ++cur_arr;
                cur_ind = 0;
            }
        }

        std::vector<MutableColumns> data_arrays;
        data_arrays.resize(array_count);

        for (size_t i = 0; i < array_count; ++i)
            data_arrays[i] = arrays[i].column->scatter(max_array_size, selector);
        
        size_t prev_size = rows_count;
        IColumn::Permutation inverse_permutation(rows_count);
        size_t inverse_permutation_count = 0;

        for (size_t ind = 0; ind < max_array_size; ++ind)
        {
            IColumn::Selector prev_selector(prev_size);
            size_t prev_ind = 0;
            for (ssize_t irow = 0; irow < rows_count; ++irow)
            {
                if (offsets[irow] - offsets[irow - 1] > ind)
                {
                    prev_selector[prev_ind++] = 1;
                }
                else if (offsets[irow] - offsets[irow - 1] == ind)
                {
                    inverse_permutation[inverse_permutation_count++] = irow;
                    prev_selector[prev_ind++] = 0;
                }
            }
            auto prev = current_column->scatter(2, prev_selector);

            result_data->insertRangeFrom(*(prev[0]), 0, prev[0]->size());

            auto res_lambda = column_function->cloneResized(prev[1]->size());
            auto * res_lambda_ptr = typeid_cast<ColumnFunction *>(res_lambda.get());
            
            for (size_t i = 0; i < array_count; i++)
                res_lambda_ptr->appendArguments(std::vector({ColumnWithTypeAndName(std::move(data_arrays[i][ind]), arrays[i].type, arrays[i].name)}));
            res_lambda_ptr->appendArguments(std::vector({ColumnWithTypeAndName(std::move(prev[1]), arguments.back().type, arguments.back().name)}));

            current_column = IColumn::mutate(res_lambda_ptr->reduce().column);
            prev_size = current_column->size();
        }

        result_data->insertRangeFrom(*current_column, 0, current_column->size());
        for (ssize_t irow = 0; irow < rows_count; ++irow)
            if (offsets[irow] - offsets[irow - 1] == max_array_size)
                inverse_permutation[inverse_permutation_count++] = irow;

        IColumn::Permutation perm(rows_count);
        for (ssize_t i = 0; i < rows_count; i++)
            perm[inverse_permutation[i]] = i;
        return result_data->permute(perm, 0);
    }

private:
    String getName() const override
    {
        return name;
    }
};

REGISTER_FUNCTION(ArrayFold)
{
    {
        R"(
Calculates BLAKE3 hash string and returns the resulting set of bytes as FixedString.
This cryptographic hash-function is integrated into ClickHouse with BLAKE3 Rust library.
The function is rather fast and shows approximately two times faster performance compared to SHA-2, while generating hashes of the same length as SHA-256.
It returns a BLAKE3 hash as a byte array with type FixedString(32).
)",
        Documentation::Examples{
            {"hash", "SELECT hex(BLAKE3('ABC'))"}},
        Documentation::Categories{"Hash"}
    },
    factory.registerFunction<ArrayFold>("arrayFold", {R"(
        Function arrayFold(x1,...,xn,accum -> expression, array1,...,arrayn, init_accum) applies lambda function to a number of same sized array columns
        and collects result in accumulator. Accumulator can be either constant or column.
        )",
        Documentation::Examples{
            {"sum", "SELECT arrayFold(x,acc -> acc + x, [1,2,3,4], toInt64(0));"}},
        Documentation::Categories{}
    });
}
}
