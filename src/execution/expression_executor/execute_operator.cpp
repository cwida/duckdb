#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/bit_utils.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/vector_writer.hpp"

namespace duckdb {

unique_ptr<ExpressionState> ExpressionExecutor::InitializeState(const BoundOperatorExpression &expr,
                                                                ExpressionExecutorState &root) {
	auto result = make_uniq<ExpressionState>(expr, root);
	const auto save_inside_try = root.inside_try;
	const auto expression_type = expr.GetExpressionType();
	if (expression_type == ExpressionType::OPERATOR_TRY || expression_type == ExpressionType::OPERATOR_HAS_ERROR) {
		root.inside_try = true;
	}
	for (auto &child : expr.GetChildren()) {
		result->AddChild(*child);
	}
	root.inside_try = save_inside_try;

	result->Finalize();
	return result;
}

static idx_t SelectFlatNulls(const ValidityMask &validity, idx_t count, SelectionVector &result) {
	if (validity.CannotHaveNull()) {
		return 0;
	}
	idx_t result_count = 0;
	for (idx_t offset = 0; offset < count; offset += ValidityMask::BITS_PER_VALUE) {
		const auto valid_bits = MinValue<idx_t>(ValidityMask::BITS_PER_VALUE, count - offset);
		const auto mask = ValidityMask::EntryWithValidBits(valid_bits);
		auto nulls = ~validity.GetValidityEntryUnsafe(offset / ValidityMask::BITS_PER_VALUE) & mask;
		if (nulls == mask) {
			for (idx_t i = 0; i < valid_bits; i++) {
				result.set_index(result_count++, offset + i);
			}
			continue;
		}
		while (nulls) {
			result.set_index(result_count++, offset + CountZeros<validity_t>::Trailing(nulls));
			nulls &= nulls - 1;
		}
	}
	return result_count;
}

template <bool HAS_VALID_SEL, bool HAS_NULL_SEL>
static idx_t SelectNullLoop(const VectorValidityIterator &entries, const SelectionVector &sel, idx_t count,
                            optional_ptr<SelectionVector> valid_sel, optional_ptr<SelectionVector> null_sel) {
	idx_t valid_count = 0, null_count = 0;
	for (idx_t i = 0; i < count; i++) {
		const auto row_idx = sel.get_index(i);
		const auto valid = entries.IsValid(i);
		if (HAS_VALID_SEL) {
			valid_sel->set_index(valid_count, row_idx);
			valid_count += valid;
		}
		if (HAS_NULL_SEL) {
			null_sel->set_index(null_count, row_idx);
			null_count += !valid;
		}
	}
	return HAS_VALID_SEL ? valid_count : count - null_count;
}

static idx_t SelectNulls(const Vector &input, const SelectionVector &sel, idx_t count,
                         optional_ptr<SelectionVector> valid_sel, optional_ptr<SelectionVector> null_sel) {
	if (!valid_sel && null_sel && !sel.IsSet() && input.GetVectorType() == VectorType::FLAT_VECTOR) {
		return count - SelectFlatNulls(FlatVector::Validity(input), count, *null_sel);
	}
	auto entries = input.Validity();
	if (entries.CannotHaveNull() || input.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		const auto valid = entries.CannotHaveNull() || entries.IsValid(0);
		auto result_sel = valid ? valid_sel : null_sel;
		if (result_sel) {
			for (idx_t i = 0; i < count; i++) {
				result_sel->set_index(i, sel.get_index(i));
			}
		}
		return valid ? count : 0;
	}
	if (valid_sel && null_sel) {
		return SelectNullLoop<true, true>(entries, sel, count, valid_sel, null_sel);
	}
	if (valid_sel) {
		return SelectNullLoop<true, false>(entries, sel, count, valid_sel, null_sel);
	}
	return SelectNullLoop<false, true>(entries, sel, count, valid_sel, null_sel);
}

idx_t ExpressionExecutor::Select(const BoundOperatorExpression &expr, ExpressionState &state,
                                 optional_ptr<const SelectionVector> sel, idx_t count,
                                 optional_ptr<SelectionVector> true_sel, optional_ptr<SelectionVector> false_sel) {
	const auto type = expr.GetExpressionType();
	if (type != ExpressionType::OPERATOR_IS_NULL && type != ExpressionType::OPERATOR_IS_NOT_NULL) {
		return DefaultSelect(expr, &state, sel.get(), count, true_sel.get(), false_sel.get());
	}
	D_ASSERT(expr.GetChildren().size() == 1);
	state.intermediate_chunk.Reset();
	auto &child = state.intermediate_chunk.data[0];
	Execute(*expr.GetChildren()[0], state.child_states[0].get(), sel.get(), count, child);

	const SelectionVector identity;
	const auto &input_sel = sel ? *sel : identity;
	if (type == ExpressionType::OPERATOR_IS_NOT_NULL) {
		return SelectNulls(child, input_sel, count, true_sel, false_sel);
	}
	return count - SelectNulls(child, input_sel, count, false_sel, true_sel);
}

bool ExpressionExecutor::ExecuteErrorCapture(const Expression &child, ExpressionState &child_state,
                                             const SelectionVector *sel, idx_t count, Vector &values,
                                             optional_ptr<Vector> error_flags) {
	try {
		Execute(child, &child_state, sel, count, values);
		return true;
	} catch (std::exception &ex) {
		ErrorData error(ex);
		if (!Exception::IsExecutionError(error.Type())) {
			throw;
		}
	}

	// an error was raised: evaluate per row to capture the failing rows
	// the failed evaluation may have left values referencing buffers of its inputs, so reinitialize it
	// before writing into it
	values.Initialize();
	// CASE/COALESCE write their result at the physical row index, so the intermediate must fit that index
	SelectionVector selvec(1);
	DataChunk intermediate;
	intermediate.Initialize(GetAllocator(), {values.GetType()}, STANDARD_VECTOR_SIZE);
	unique_ptr<VectorWriter<bool>> flag_writer;
	if (error_flags) {
		flag_writer = make_uniq<VectorWriter<bool>>(*error_flags, count, 0ULL);
	}
	for (idx_t i = 0; i < count; i++) {
		intermediate.Reset();
		intermediate.SetChildCardinality(1);

		// clear any dictionary states in the child expression, so that it is executed anew for every row
		child_state.ResetDictionaryStates();

		selvec.set_index(0, sel ? sel->get_index(i) : i);
		Value val(values.GetType());
		bool has_error = false;
		try {
			Execute(child, &child_state, &selvec, 1, intermediate.data[0]);
			val = intermediate.GetValue(0, 0);
		} catch (std::exception &ex) {
			ErrorData error(ex);
			if (!Exception::IsExecutionError(error.Type())) {
				throw;
			}
			has_error = true;
		}
		values.SetValue(i, val);
		if (flag_writer) {
			flag_writer->WriteValue(has_error);
		}
	}
	return false;
}

void ExpressionExecutor::Execute(const BoundOperatorExpression &expr, ExpressionState *state,
                                 const SelectionVector *sel, idx_t count, Vector &result) {
	// special handling for special snowflake 'IN'
	// IN has n children
	auto expression_type = expr.GetExpressionType();
	if (expression_type == ExpressionType::COMPARE_IN || expression_type == ExpressionType::COMPARE_NOT_IN) {
		if (expr.GetChildren().size() < 2) {
			throw InvalidInputException("IN needs at least two children");
		}

		Vector left(expr.GetChildren()[0]->GetReturnType());
		// eval left side
		Execute(*expr.GetChildren()[0], state->child_states[0].get(), sel, count, left);

		// init result to false
		Vector intermediate(LogicalType::BOOLEAN);
		intermediate.Reference(Value::BOOLEAN(false), count_t(count));

		// in rhs is a list of constants
		// for every child, OR the result of the comparison with the left
		// to get the overall result.
		for (idx_t child = 1; child < expr.GetChildren().size(); child++) {
			Vector vector_to_check(expr.GetChildren()[child]->GetReturnType());
			Vector comp_res(LogicalType::BOOLEAN);

			Execute(*expr.GetChildren()[child], state->child_states[child].get(), sel, count, vector_to_check);
			VectorOperations::Equals(left, vector_to_check, comp_res);

			if (child == 1) {
				// first child: move to result
				intermediate.Reference(comp_res);
			} else {
				// otherwise OR together
				Vector new_result(LogicalType::BOOLEAN);
				VectorOperations::Or(intermediate, comp_res, new_result);
				intermediate.Reference(new_result);
			}
		}
		if (expression_type == ExpressionType::COMPARE_NOT_IN) {
			// NOT IN: invert result
			VectorOperations::Not(intermediate, result);
		} else {
			// directly use the result
			result.Reference(intermediate);
		}
	} else if (expression_type == ExpressionType::OPERATOR_COALESCE) {
		SelectionVector sel_a(count);
		SelectionVector sel_b(count);
		SelectionVector slice_sel(count);
		SelectionVector result_sel(count);
		SelectionVector *next_sel = &sel_a;
		const SelectionVector *current_sel = sel;
		idx_t remaining_count = count;
		idx_t next_count;
		for (idx_t child = 0; child < expr.GetChildren().size(); child++) {
			Vector vector_to_check(expr.GetChildren()[child]->GetReturnType());
			Execute(*expr.GetChildren()[child], state->child_states[child].get(), current_sel, remaining_count,
			        vector_to_check);

			auto entries = vector_to_check.Validity();
			idx_t result_count = 0;
			next_count = 0;
			for (idx_t i = 0; i < remaining_count; i++) {
				auto base_idx = current_sel ? current_sel->get_index(i) : i;
				if (entries.IsValid(i)) {
					slice_sel.set_index(result_count, i);
					result_sel.set_index(result_count++, base_idx);
				} else {
					next_sel->set_index(next_count++, base_idx);
				}
			}
			if (result_count > 0) {
				vector_to_check.Slice(slice_sel, result_count);
				FillSwitch(vector_to_check, result, result_sel, NumericCast<sel_t>(result_count));
			}
			current_sel = next_sel;
			next_sel = next_sel == &sel_a ? &sel_b : &sel_a;
			remaining_count = next_count;
			if (next_count == 0) {
				break;
			}
		}
		if (remaining_count > 0) {
			for (idx_t i = 0; i < remaining_count; i++) {
				FlatVector::SetNull(result, current_sel->get_index(i), true);
			}
		}
		if (sel) {
			result.Slice(*sel, count);
		} else if (count == 1) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	} else if (expression_type == ExpressionType::OPERATOR_TRY) {
		auto &child_state = *state->child_states[0];
		Vector try_result(result.GetType());
		if (ExecuteErrorCapture(*expr.GetChildren()[0], child_state, sel, count, try_result, nullptr)) {
			// no error, forward the child result
			if (try_result.GetVectorType() == VectorType::CONSTANT_VECTOR) {
				result.Reference(try_result);
				return;
			}
			VectorOperations::Copy(try_result, result, count, 0, 0);
			return;
		}
		// the failing rows were captured as NULL
		VectorOperations::Copy(try_result, result, count, 0, 0);
		if (count == 1) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	} else if (expression_type == ExpressionType::OPERATOR_HAS_ERROR) {
		auto &child_state = *state->child_states[0];
		Vector probe_result(expr.GetChildren()[0]->GetReturnType());
		if (ExecuteErrorCapture(*expr.GetChildren()[0], child_state, sel, count, probe_result, &result)) {
			// the child evaluated without an error for every row
			result.Reference(Value::BOOLEAN(false), count_t(count));
			return;
		}
		if (count == 1) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	} else if (expr.GetChildren().size() == 1) {
		state->intermediate_chunk.Reset();
		auto &child = state->intermediate_chunk.data[0];

		Execute(*expr.GetChildren()[0], state->child_states[0].get(), sel, count, child);
		switch (expr.GetExpressionType()) {
		case ExpressionType::OPERATOR_NOT: {
			VectorOperations::Not(child, result);
			break;
		}
		case ExpressionType::OPERATOR_IS_NULL: {
			VectorOperations::IsNull(child, result);
			break;
		}
		case ExpressionType::OPERATOR_IS_NOT_NULL: {
			VectorOperations::IsNotNull(child, result);
			break;
		}
		default:
			throw NotImplementedException("Unsupported operator type with 1 child!");
		}
	} else {
		throw NotImplementedException("operator");
	}
}

} // namespace duckdb
