#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/function/scalar/nested_functions.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_parameter_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/function/scalar/list_functions.hpp"

namespace duckdb {

namespace {

struct SetSelectionVectorSelect {
	using CHILD_TYPE = int64_t;

	static void SetSelectionVector(SelectionVector &selection_vector, ValidityMask &validity_mask,
	                               const VectorValidityIterator &input_validity,
	                               const VectorIterator<int64_t> &child_data, idx_t child_idx, idx_t &target_offset,
	                               idx_t selection_offset, idx_t input_offset, idx_t target_length) {
		auto child_entry = child_data[selection_offset + child_idx];
		int64_t sel_idx = -1;
		if (child_entry.IsValid()) {
			int64_t value_idx = child_entry.GetValue();
			if (value_idx > 0) {
				sel_idx = value_idx - 1;
			}
		}

		if (sel_idx >= 0 && sel_idx < UnsafeNumericCast<int64_t>(target_length)) {
			auto sel_idx_unsigned = UnsafeNumericCast<idx_t>(sel_idx);
			selection_vector.set_index(target_offset, input_offset + sel_idx_unsigned);
			if (!input_validity.IsValid(input_offset + sel_idx_unsigned)) {
				validity_mask.SetInvalid(target_offset);
			}
		} else {
			selection_vector.set_index(target_offset, 0);
			validity_mask.SetInvalid(target_offset);
		}
		target_offset++;
	}

	static void GetResultLength(const VectorIterator<int64_t> &child_data, idx_t &result_length,
	                            list_entry_t selection_list) {
		result_length += selection_list.length;
	}
};

struct SetSelectionVectorWhere {
	using CHILD_TYPE = bool;

	static void SetSelectionVector(SelectionVector &selection_vector, ValidityMask &validity_mask,
	                               const VectorValidityIterator &input_validity, const VectorIterator<bool> &child_data,
	                               idx_t child_idx, idx_t &target_offset, idx_t selection_offset, idx_t input_offset,
	                               idx_t target_length) {
		auto child_val = child_data[selection_offset + child_idx];
		if (!child_val.GetValue()) {
			return;
		}

		if (child_idx >= target_length) {
			selection_vector.set_index(target_offset, 0);
			validity_mask.SetInvalid(target_offset);
			target_offset++;
			return;
		}

		selection_vector.set_index(target_offset, input_offset + child_idx);
		if (!input_validity.IsValid(input_offset + child_idx)) {
			validity_mask.SetInvalid(target_offset);
		}

		target_offset++;
	}

	static void GetResultLength(const VectorIterator<bool> &child_data, idx_t &result_length,
	                            list_entry_t selection_list) {
		for (idx_t child_idx = 0; child_idx < selection_list.length; child_idx++) {
			auto child_val = child_data[selection_list.offset + child_idx];
			if (!child_val.IsValid()) {
				throw InvalidInputException("NULLs are not allowed as list elements in the second input parameter.");
			}
			if (child_val.GetValue()) {
				result_length++;
			}
		}
	}
};

template <class OP>
void ListSelectFunction(const DataChunk &args, ExpressionState &state, Vector &result) {
	D_ASSERT(args.data.size() == 2);
	const auto &list = args.data[0];
	const auto &selection_list = args.data[1];
	idx_t count = args.size();

	auto selection_list_data = selection_list.Values<list_entry_t>();
	auto &selection_entry = ListVector::GetChild(selection_list);
	auto input_lists_data = list.Values<list_entry_t>();
	auto &input_entry = ListVector::GetChild(list);
	auto input_validity = input_entry.Validity();
	auto selection_entry_data = selection_entry.Values<typename OP::CHILD_TYPE>();

	idx_t result_length = 0;
	for (idx_t r = 0; r < count; r++) {
		auto input_list_entry = input_lists_data[r];
		auto selection_list_entry = selection_list_data[r];
		if (input_list_entry.IsValid() && selection_list_entry.IsValid()) {
			OP::GetResultLength(selection_entry_data, result_length, selection_list_entry.GetValue());
		}
	}

	ListVector::Reserve(result, result_length);
	SelectionVector result_selection_vec = SelectionVector(result_length);
	ValidityMask entry_validity_mask = ValidityMask(result_length);

	auto result_data = FlatVector::Writer<list_entry_t>(result, count);
	auto &result_entry = ListVector::GetChildMutable(result);

	idx_t offset = 0;
	for (idx_t r = 0; r < count; r++) {
		// Get length and offset of selection list for current output row
		auto selection_list_entry = selection_list_data[r];
		idx_t selection_len = 0;
		idx_t selection_offset = 0;
		if (selection_list_entry.IsValid()) {
			auto selection_list_val = selection_list_entry.GetValue();
			selection_len = selection_list_val.length;
			selection_offset = selection_list_val.offset;
		} else {
			result_data.WriteNull();
			continue;
		}
		// Get length and offset of input list for current output row
		auto input_list_entry = input_lists_data[r];
		idx_t input_length = 0;
		idx_t input_offset = 0;
		if (input_list_entry.IsValid()) {
			auto input_list_val = input_list_entry.GetValue();
			input_length = input_list_val.length;
			input_offset = input_list_val.offset;
		} else {
			result_data.WriteNull();
			continue;
		}
		const idx_t entry_offset = offset;
		// Set all selected values in the result
		for (idx_t child_idx = 0; child_idx < selection_len; child_idx++) {
			if (selection_entry.GetValue(selection_offset + child_idx).IsNull()) {
				throw InvalidInputException("NULLs are not allowed as list elements in the second input parameter.");
			}
			OP::SetSelectionVector(result_selection_vec, entry_validity_mask, input_validity, selection_entry_data,
			                       child_idx, offset, selection_offset, input_offset, input_length);
		}
		result_data.WriteValue(list_entry_t(entry_offset, offset - entry_offset));
	}
	ListVector::SetListSize(result, offset);

	if (result_length > 0) {
		result_entry.Slice(input_entry, result_selection_vec, offset);
		result_entry.Flatten();

		FlatVector::SetValidity(result_entry, entry_validity_mask);
	}
}

} // namespace

ScalarFunction ListWhereFun::GetFunction() {
	auto fun =
	    ScalarFunction({}, LogicalType::LIST(LogicalType::TEMPLATE("T")), ListSelectFunction<SetSelectionVectorWhere>);
	fun.GetSignature()
	    .AddParameter("value_list", LogicalType::LIST(LogicalType::TEMPLATE("T")))
	    .AddParameter("mask_list", LogicalType::LIST(LogicalType::BOOLEAN));
	// throws if the selection list contains NULL values
	fun.SetFallible();
	return fun;
}

ScalarFunction ListSelectFun::GetFunction() {
	auto fun =
	    ScalarFunction({}, LogicalType::LIST(LogicalType::TEMPLATE("T")), ListSelectFunction<SetSelectionVectorSelect>);
	fun.GetSignature()
	    .AddParameter("value_list", LogicalType::LIST(LogicalType::TEMPLATE("T")))
	    .AddParameter("index_list", LogicalType::LIST(LogicalType::BIGINT));
	// throws if the selection list contains NULL values
	fun.SetFallible();
	return fun;
}

} // namespace duckdb
