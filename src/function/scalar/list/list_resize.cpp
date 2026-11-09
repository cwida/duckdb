#include "duckdb/common/checked_integer.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/function/scalar/nested_functions.hpp"
#include "duckdb/function/scalar/list_functions.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/built_in_functions.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"

namespace duckdb {

static void ListResizeFunction(DataChunk &args, ExpressionState &, Vector &result) {
	// Early-out, if the return value is a constant NULL.
	if (result.GetType().id() == LogicalTypeId::SQLNULL) {
		ConstantVector::SetNull(result, count_t(args.size()));
		return;
	}

	const auto &lists = args.data[0];
	const auto &new_sizes = args.data[1];
	auto row_count = args.size();
	D_ASSERT(result.GetType().id() == LogicalTypeId::LIST);
	D_ASSERT(new_sizes.GetType().id() == LogicalTypeId::UBIGINT);

	auto list_entries = lists.Values<list_entry_t>();
	auto new_size_entries = new_sizes.Values<ubigint_t>();
	auto &child_vector = ListVector::GetChild(lists);

	// Sum up the total child capacity
	ubigint_t total_child_size(0);
	for (idx_t row_idx = 0; row_idx < row_count; row_idx++) {
		auto list_entry = list_entries[row_idx];
		auto new_size_entry = new_size_entries[row_idx];
		if (list_entry.IsValid() && new_size_entry.IsValid()) {
			total_child_size += new_size_entry.GetValue();
		}
	}

	bool has_default_vector = args.ColumnCount() == 3 && args.data[2].GetType().id() != LogicalTypeId::SQLNULL;

	ListVector::Reserve(result, total_child_size.GetValue());
	auto result_entries = FlatVector::Writer<list_entry_t>(result, row_count);

	for (idx_t row_idx = 0; row_idx < row_count; row_idx++) {
		auto list_entry = list_entries[row_idx];
		// Set to NULL, if the list is NULL.
		if (!list_entry.IsValid()) {
			result_entries.WriteNull();
			continue;
		}

		auto new_size_entry = new_size_entries[row_idx];
		ubigint_t new_size = new_size_entry.IsValid() ? new_size_entry.GetValue() : ubigint_t(0);

		// If new_size >= length, then we copy [0, length) values.
		// If new_size < length, then we copy [0, new_size) values.
		const auto &source_list = list_entry.GetValue();
		auto copy_count = MinValue<ubigint_t>(source_list.length, new_size);

		auto list = result_entries.WriteDynamicList();
		ubigint_t source_offset = source_list.offset;
		ubigint_t source_count = source_offset + copy_count;
		list.Append(child_vector, *FlatVector::IncrementalSelectionVector(), source_count.GetValue(),
		            source_offset.GetValue(), copy_count.GetValue());

		if (copy_count >= new_size) {
			continue;
		}
		ubigint_t remaining_count = new_size - copy_count;

		// if a default value is provided fill the list with the default value
		if (has_default_vector) {
			SelectionVector sel(remaining_count.GetValue());
			for (idx_t j = 0; j < remaining_count.GetValue(); j++) {
				sel.set_index(j, row_idx);
			}
			const auto &default_vector = args.data[2];
			// the selection only has an entry for every padded row
			list.Append(default_vector, sel, remaining_count.GetValue(), 0, remaining_count.GetValue());
			continue;
		}

		// Fill the remaining space with NULL.
		list.AppendNulls(remaining_count.GetValue());
	}
}

static void ListResizeResolveTypes(ResolveScalarFunctionTypesInput &input) {
	auto &bound_function = input.GetBoundFunction();
	D_ASSERT(bound_function.GetArguments().size() == 2 || input.GetArgumentCount() == 3);
	bound_function.GetArguments()[1] = LogicalType::UBIGINT;

	// If the first argument is an array, cast it to a list.
	auto list_type = input.GetArgumentType(0);
	if (list_type.id() == LogicalTypeId::ARRAY) {
		list_type = LogicalType::LIST(ArrayType::GetChildType(list_type));
		bound_function.GetArguments()[0] = list_type;
	}

	// Early-out, if the first argument is a constant NULL.
	if (list_type == LogicalType::SQLNULL) {
		bound_function.GetArguments()[0] = LogicalType::SQLNULL;
		bound_function.SetReturnType(LogicalType::SQLNULL);
		return;
	}

	// Early-out, if the first argument is a prepared statement.
	if (list_type == LogicalType::UNKNOWN) {
		bound_function.SetReturnType(list_type);
		return;
	}

	if (list_type.id() != LogicalTypeId::LIST) {
		throw BinderException("%s can only operate on LISTs", bound_function.GetName());
	}

	// Attempt implicit casting, if the default type does not match list the list child type.
	if (bound_function.GetArguments().size() == 3 && ListType::GetChildType(list_type) != input.GetArgumentType(2) &&
	    input.GetArgumentType(2) != LogicalTypeId::SQLNULL) {
		bound_function.GetArguments()[2] = ListType::GetChildType(list_type);
	}

	bound_function.SetReturnType(list_type);
}

ScalarFunctionSet ListResizeFun::GetFunctions() {
	ScalarFunction simple_fun({}, LogicalType::LIST(LogicalTypeId::ANY), ListResizeFunction,
	                          VariableReturnBindData::Bind);
	simple_fun.GetSignature()
	    .AddParameter("list", LogicalType::LIST(LogicalTypeId::ANY))
	    .AddParameter("size", LogicalTypeId::ANY);
	simple_fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	simple_fun.SetFallible();
	simple_fun.SetResolveTypesCallback(ListResizeResolveTypes);
	ScalarFunction default_value_fun({}, LogicalType::LIST(LogicalTypeId::ANY), ListResizeFunction,
	                                 VariableReturnBindData::Bind);
	default_value_fun.GetSignature()
	    .AddParameter("list", LogicalType::LIST(LogicalTypeId::ANY))
	    .AddParameter("size", LogicalTypeId::ANY)
	    .AddParameter("value", LogicalTypeId::ANY);
	default_value_fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	default_value_fun.SetFallible();
	default_value_fun.SetResolveTypesCallback(ListResizeResolveTypes);
	ScalarFunctionSet list_resize_set("list_resize");
	list_resize_set.AddFunction(simple_fun);
	list_resize_set.AddFunction(default_value_fun);
	return list_resize_set;
}

} // namespace duckdb
