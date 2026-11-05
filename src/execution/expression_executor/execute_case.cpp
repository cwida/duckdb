#include "duckdb/common/vector/array_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/uhugeint.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"

namespace duckdb {

struct CaseExpressionState : public ExpressionState {
	CaseExpressionState(const Expression &expr, ExpressionExecutorState &root)
	    : ExpressionState(expr, root), true_sel(STANDARD_VECTOR_SIZE), false_sel(STANDARD_VECTOR_SIZE) {
	}

	SelectionVector true_sel;
	SelectionVector false_sel;
};

unique_ptr<ExpressionState> ExpressionExecutor::InitializeState(const BoundCaseExpression &expr,
                                                                ExpressionExecutorState &root) {
	auto result = make_uniq<CaseExpressionState>(expr, root);
	for (auto &case_check : expr.CaseChecks()) {
		result->AddChild(*case_check.when_expr);
		result->AddChild(*case_check.then_expr);
	}
	result->AddChild(expr.Else());

	result->Finalize();
	return std::move(result);
}

void ExpressionExecutor::Execute(const BoundCaseExpression &expr, ExpressionState *state_p, const SelectionVector *sel,
                                 idx_t count, Vector &result) {
	auto &state = state_p->Cast<CaseExpressionState>();

	state.intermediate_chunk.Reset();

	// first execute the check expression
	auto current_true_sel = &state.true_sel;
	auto current_false_sel = &state.false_sel;
	auto current_sel = sel;
	idx_t current_count = count;
	for (idx_t i = 0; i < expr.CaseChecks().size(); i++) {
		auto &case_check = expr.CaseChecks()[i];
		auto &intermediate_result = state.intermediate_chunk.data[i * 2 + 1];
		auto check_state = state.child_states[i * 2].get();
		auto then_state = state.child_states[i * 2 + 1].get();

		idx_t tcount =
		    Select(*case_check.when_expr, check_state, current_sel, current_count, current_true_sel, current_false_sel);
		if (tcount == 0) {
			// everything is false: do nothing
			continue;
		}
		idx_t fcount = current_count - tcount;
		if (fcount == 0 && current_count == count) {
			// everything is true in the first CHECK statement
			// we can skip the entire case and only execute the TRUE side
			Execute(*case_check.then_expr, then_state, sel, count, result);
			return;
		} else {
			// we need to execute and then fill in the desired tuples in the result
			Execute(*case_check.then_expr, then_state, current_true_sel, tcount, intermediate_result);
			FillSwitch(intermediate_result, result, *current_true_sel, NumericCast<sel_t>(tcount));
		}
		// continue with the false tuples
		current_sel = current_false_sel;
		current_count = fcount;
		if (fcount == 0) {
			// everything is true: we are done
			break;
		}
	}
	if (current_count > 0) {
		auto else_state = state.child_states.back().get();
		if (current_count == count) {
			// everything was false, we can just evaluate the else expression directly
			Execute(expr.Else(), else_state, sel, count, result);
			return;
		} else {
			auto &intermediate_result = state.intermediate_chunk.data[expr.CaseChecks().size() * 2];

			D_ASSERT(current_sel);
			Execute(expr.Else(), else_state, current_sel, current_count, intermediate_result);
			FillSwitch(intermediate_result, result, *current_sel, NumericCast<sel_t>(current_count));
		}
	}
	if (sel) {
		result.Slice(*sel, count);
	}
}

static void FillNulls(ValidityMask &validity, const SelectionVector &sel, idx_t count) {
	if (count == 0) {
		return;
	}
	validity.EnsureWritable();
	idx_t current_entry = sel.get_index(0) / ValidityMask::BITS_PER_VALUE;
	validity_t invalid_bits = 0;
	for (idx_t i = 0; i < count; i++) {
		auto row_idx = sel.get_index(i);
		D_ASSERT(row_idx < validity.Capacity());
		auto entry_idx = row_idx / ValidityMask::BITS_PER_VALUE;
		if (entry_idx != current_entry) {
			validity.GetValidityEntryUnsafe(current_entry) &= ~invalid_bits;
			current_entry = entry_idx;
			invalid_bits = 0;
		}
		invalid_bits |= validity_t(1) << (row_idx % ValidityMask::BITS_PER_VALUE);
	}
	validity.GetValidityEntryUnsafe(current_entry) &= ~invalid_bits;
}

template <class T>
void TemplatedFillLoop(const Vector &vector, Vector &result, const SelectionVector &sel, sel_t count) {
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto res = FlatVector::GetDataMutable<T>(result);
	auto &result_mask = FlatVector::ValidityMutable(result);
	if (vector.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		auto data = ConstantVector::GetData<T>(vector);
		if (ConstantVector::IsNull(vector)) {
			FillNulls(result_mask, sel, count);
		} else {
			for (idx_t i = 0; i < count; i++) {
				res[sel.get_index(i)] = *data;
			}
		}
	} else {
		auto entries = vector.Values<T>();
		for (idx_t i = 0; i < count; i++) {
			auto entry = entries[i];
			auto res_idx = sel.get_index(i);
			if (entry.IsValid()) {
				res[res_idx] = entry.GetValue();
			} else {
				result_mask.SetInvalid(res_idx);
			}
		}
	}
}

void ValidityFillLoop(const Vector &vector, Vector &result, const SelectionVector &sel, sel_t count) {
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto &result_mask = FlatVector::ValidityMutable(result);
	if (vector.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		if (ConstantVector::IsNull(vector)) {
			FillNulls(result_mask, sel, count);
		}
	} else {
		auto entries = vector.Validity();
		if (!entries.CanHaveNull()) {
			return;
		}
		for (idx_t i = 0; i < count; i++) {
			if (!entries.IsValid(i)) {
				result_mask.SetInvalid(sel.get_index(i));
			}
		}
	}
}

void ExpressionExecutor::FillSwitch(const Vector &vector, Vector &result, const SelectionVector &sel, sel_t count) {
	switch (result.GetType().InternalType()) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		TemplatedFillLoop<int8_t>(vector, result, sel, count);
		break;
	case PhysicalType::INT16:
		TemplatedFillLoop<int16_t>(vector, result, sel, count);
		break;
	case PhysicalType::INT32:
		TemplatedFillLoop<int32_t>(vector, result, sel, count);
		break;
	case PhysicalType::INT64:
		TemplatedFillLoop<int64_t>(vector, result, sel, count);
		break;
	case PhysicalType::UINT8:
		TemplatedFillLoop<uint8_t>(vector, result, sel, count);
		break;
	case PhysicalType::UINT16:
		TemplatedFillLoop<uint16_t>(vector, result, sel, count);
		break;
	case PhysicalType::UINT32:
		TemplatedFillLoop<uint32_t>(vector, result, sel, count);
		break;
	case PhysicalType::UINT64:
		TemplatedFillLoop<uint64_t>(vector, result, sel, count);
		break;
	case PhysicalType::INT128:
		TemplatedFillLoop<hugeint_t>(vector, result, sel, count);
		break;
	case PhysicalType::UINT128:
		TemplatedFillLoop<uhugeint_t>(vector, result, sel, count);
		break;
	case PhysicalType::FLOAT:
		TemplatedFillLoop<float>(vector, result, sel, count);
		break;
	case PhysicalType::DOUBLE:
		TemplatedFillLoop<double>(vector, result, sel, count);
		break;
	case PhysicalType::INTERVAL:
		TemplatedFillLoop<interval_t>(vector, result, sel, count);
		break;
	case PhysicalType::VARCHAR:
		TemplatedFillLoop<string_t>(vector, result, sel, count);
		StringVector::AddHeapReference(result, vector);
		break;
	case PhysicalType::STRUCT: {
		if (vector.GetVectorType() != VectorType::CONSTANT_VECTOR) {
			// below code needs constant or flat structs
			vector.Flatten();
		}
		auto &vector_entries = StructVector::GetEntries(vector);
		auto &result_entries = StructVector::GetEntries(result);
		ValidityFillLoop(vector, result, sel, count);
		D_ASSERT(vector_entries.size() == result_entries.size());
		for (idx_t i = 0; i < vector_entries.size(); i++) {
			FillSwitch(vector_entries[i], result_entries[i], sel, count);
		}
		break;
	}
	case PhysicalType::LIST: {
		idx_t offset = ListVector::GetListSize(result);
		auto &list_child = ListVector::GetChild(vector);
		ListVector::Append(result, list_child, ListVector::GetListSize(vector));

		// all the false offsets need to be incremented by true_child.count
		TemplatedFillLoop<list_entry_t>(vector, result, sel, count);
		if (offset == 0) {
			break;
		}

		auto result_data = FlatVector::ScatterWriter<list_entry_t>(result);
		for (idx_t i = 0; i < count; i++) {
			auto result_idx = sel.get_index(i);
			result_data[result_idx].offset += offset;
		}

		result.Verify();
		break;
	}
	case PhysicalType::ARRAY: {
		const bool is_constant = vector.GetVectorType() == VectorType::CONSTANT_VECTOR;
		if (!is_constant) {
			// below code needs constant or flat arrays
			vector.Flatten();
		}
		ValidityFillLoop(vector, result, sel, count);
		auto array_size = ArrayType::GetSize(result.GetType());
		auto child_count = count * array_size;
		SelectionVector source_sel(child_count);
		SelectionVector result_sel(child_count);
		for (idx_t i = 0; i < count; i++) {
			auto source_idx = is_constant ? 0 : i;
			auto result_idx = sel.get_index(i);
			for (idx_t k = 0; k < array_size; k++) {
				source_sel.set_index(i * array_size + k, source_idx * array_size + k);
				result_sel.set_index(i * array_size + k, result_idx * array_size + k);
			}
		}
		Vector source_child(ArrayVector::GetChild(vector), source_sel, child_count);
		source_child.Flatten();
		FillSwitch(source_child, ArrayVector::GetChildMutable(result), result_sel, NumericCast<sel_t>(child_count));
		break;
	}
	default:
		throw NotImplementedException("Unimplemented type for case expression: %s", result.GetType().ToString());
	}
}

} // namespace duckdb
