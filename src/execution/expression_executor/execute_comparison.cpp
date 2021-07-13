#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/common/operator/comparison_operators.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"

#include <algorithm>

namespace duckdb {

unique_ptr<ExpressionState> ExpressionExecutor::InitializeState(const BoundComparisonExpression &expr,
                                                                ExpressionExecutorState &root) {
	auto result = make_unique<ExpressionState>(expr, root);
	result->AddChild(expr.left.get());
	result->AddChild(expr.right.get());
	result->Finalize();
	return result;
}

void ExpressionExecutor::Execute(const BoundComparisonExpression &expr, ExpressionState *state,
                                 const SelectionVector *sel, idx_t count, Vector &result) {
	// resolve the children
	state->intermediate_chunk.Reset();
	auto &left = state->intermediate_chunk.data[0];
	auto &right = state->intermediate_chunk.data[1];

	Execute(*expr.left, state->child_states[0].get(), sel, count, left);
	Execute(*expr.right, state->child_states[1].get(), sel, count, right);

	switch (expr.type) {
	case ExpressionType::COMPARE_EQUAL:
		VectorOperations::Equals(left, right, result, count);
		break;
	case ExpressionType::COMPARE_NOTEQUAL:
		VectorOperations::NotEquals(left, right, result, count);
		break;
	case ExpressionType::COMPARE_LESSTHAN:
		VectorOperations::LessThan(left, right, result, count);
		break;
	case ExpressionType::COMPARE_GREATERTHAN:
		VectorOperations::GreaterThan(left, right, result, count);
		break;
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		VectorOperations::LessThanEquals(left, right, result, count);
		break;
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		VectorOperations::GreaterThanEquals(left, right, result, count);
		break;
	case ExpressionType::COMPARE_DISTINCT_FROM:
		VectorOperations::DistinctFrom(left, right, result, count);
		break;
	case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
		VectorOperations::NotDistinctFrom(left, right, result, count);
		break;
	default:
		throw InternalException("Unknown comparison type!");
	}
}

template <typename OP>
static idx_t NestedSelectOperation(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                                   SelectionVector *true_sel, SelectionVector *false_sel);

template <class OP>
static idx_t TemplatedSelectOperation(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                                      SelectionVector *true_sel, SelectionVector *false_sel) {
	// the inplace loops take the result as the last parameter
	switch (left.GetType().InternalType()) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		return BinaryExecutor::Select<int8_t, int8_t, OP>(left, right, sel, count, true_sel, false_sel);
	case PhysicalType::INT16:
		return BinaryExecutor::Select<int16_t, int16_t, OP>(left, right, sel, count, true_sel, false_sel);
	case PhysicalType::INT32:
		return BinaryExecutor::Select<int32_t, int32_t, OP>(left, right, sel, count, true_sel, false_sel);
	case PhysicalType::INT64:
		return BinaryExecutor::Select<int64_t, int64_t, OP>(left, right, sel, count, true_sel, false_sel);
	case PhysicalType::UINT8:
		return BinaryExecutor::Select<uint8_t, uint8_t, OP>(left, right, sel, count, true_sel, false_sel);
	case PhysicalType::UINT16:
		return BinaryExecutor::Select<uint16_t, uint16_t, OP>(left, right, sel, count, true_sel, false_sel);
	case PhysicalType::UINT32:
		return BinaryExecutor::Select<uint32_t, uint32_t, OP>(left, right, sel, count, true_sel, false_sel);
	case PhysicalType::UINT64:
		return BinaryExecutor::Select<uint64_t, uint64_t, OP>(left, right, sel, count, true_sel, false_sel);
	case PhysicalType::INT128:
		return BinaryExecutor::Select<hugeint_t, hugeint_t, OP>(left, right, sel, count, true_sel, false_sel);
	case PhysicalType::POINTER:
		return BinaryExecutor::Select<uintptr_t, uintptr_t, OP>(left, right, sel, count, true_sel, false_sel);
	case PhysicalType::FLOAT:
		return BinaryExecutor::Select<float, float, OP>(left, right, sel, count, true_sel, false_sel);
	case PhysicalType::DOUBLE:
		return BinaryExecutor::Select<double, double, OP>(left, right, sel, count, true_sel, false_sel);
	case PhysicalType::INTERVAL:
		return BinaryExecutor::Select<interval_t, interval_t, OP>(left, right, sel, count, true_sel, false_sel);
	case PhysicalType::VARCHAR:
		return BinaryExecutor::Select<string_t, string_t, OP>(left, right, sel, count, true_sel, false_sel);
	case PhysicalType::LIST:
	case PhysicalType::MAP:
	case PhysicalType::STRUCT:
		return NestedSelectOperation<OP>(left, right, sel, count, true_sel, false_sel);
	default:
		throw InvalidTypeException(left.GetType(), "Invalid type for comparison");
	}
}

struct NestedSelector {
	// Select the matching rows for the values of a nested type that are not both NULL.
	// Those semantics are the same as the corresponding non-distinct comparator
	template <typename OP>
	static idx_t Select(Vector &left, Vector &right, idx_t vcount, const SelectionVector &sel, idx_t count,
	                    SelectionVector *true_sel, SelectionVector *false_sel) {
		throw InvalidTypeException(left.GetType(), "Invalid operation for nested SELECT");
	}
};

template <>
idx_t NestedSelector::Select<duckdb::Equals>(Vector &left, Vector &right, idx_t vcount, const SelectionVector &sel,
                                             idx_t count, SelectionVector *true_sel, SelectionVector *false_sel) {
	return VectorOperations::NestedEquals(left, right, vcount, sel, count, true_sel, false_sel);
}

template <>
idx_t NestedSelector::Select<duckdb::NotEquals>(Vector &left, Vector &right, idx_t vcount, const SelectionVector &sel,
                                                idx_t count, SelectionVector *true_sel, SelectionVector *false_sel) {
	return VectorOperations::NestedNotEquals(left, right, vcount, sel, count, true_sel, false_sel);
}

template <>
idx_t NestedSelector::Select<duckdb::LessThan>(Vector &left, Vector &right, idx_t vcount, const SelectionVector &sel,
                                               idx_t count, SelectionVector *true_sel, SelectionVector *false_sel) {
	return VectorOperations::NestedLessThan(left, right, vcount, sel, count, true_sel, false_sel);
}

template <>
idx_t NestedSelector::Select<duckdb::LessThanEquals>(Vector &left, Vector &right, idx_t vcount,
                                                     const SelectionVector &sel, idx_t count, SelectionVector *true_sel,
                                                     SelectionVector *false_sel) {
	return VectorOperations::NestedLessThanEquals(left, right, vcount, sel, count, true_sel, false_sel);
}

template <>
idx_t NestedSelector::Select<duckdb::GreaterThan>(Vector &left, Vector &right, idx_t vcount, const SelectionVector &sel,
                                                  idx_t count, SelectionVector *true_sel, SelectionVector *false_sel) {
	return VectorOperations::NestedGreaterThan(left, right, vcount, sel, count, true_sel, false_sel);
}

template <>
idx_t NestedSelector::Select<duckdb::GreaterThanEquals>(Vector &left, Vector &right, idx_t vcount,
                                                        const SelectionVector &sel, idx_t count,
                                                        SelectionVector *true_sel, SelectionVector *false_sel) {
	return VectorOperations::NestedGreaterThanEquals(left, right, vcount, sel, count, true_sel, false_sel);
}

static inline idx_t SelectNotNull(VectorData &lvdata, VectorData &rvdata, const idx_t count,
                                  OptionalSelection &false_vec, SelectionVector &maybe_vec) {

	// For top-level comparisons, NULL semantics are in effect,
	// so filter out any NULLs
	if (!lvdata.validity.AllValid() || !rvdata.validity.AllValid()) {
		idx_t true_count = 0;
		idx_t false_count = 0;
		for (idx_t i = 0; i < count; ++i) {
			const auto lidx = lvdata.sel->get_index(i);
			const auto ridx = rvdata.sel->get_index(i);
			if (!lvdata.validity.RowIsValid(lidx) || !rvdata.validity.RowIsValid(ridx)) {
				false_vec.Append(false_count, i);
			} else {
				maybe_vec.set_index(true_count++, i);
			}
		}
		false_vec.Advance(false_count);

		return true_count;
	} else {
		for (idx_t i = 0; i < count; ++i) {
			maybe_vec.set_index(i, i);
		}
		return count;
	}
}

static void ScatterSelection(SelectionVector *target, const idx_t count, const SelectionVector *sel,
                             const SelectionVector &dense_vec) {
	if (target) {
		for (idx_t i = 0; i < count; ++i) {
			target->set_index(i, sel->get_index(dense_vec.get_index(i)));
		}
	}
}

template <typename OP>
static idx_t NestedSelectOperation(Vector &left, Vector &right, const SelectionVector *sel, idx_t vcount,
                                   SelectionVector *true_sel, SelectionVector *false_sel) {
	// The Select operations all use a dense pair of input vectors to partition
	// a selection vector in a single pass. But to implement progressive comparisons,
	// we have to make multiple passes, so we need to keep track of the original input positions
	// and then scatter the output selections when we are done.
	if (!sel) {
		sel = &FlatVector::INCREMENTAL_SELECTION_VECTOR;
	}

	VectorData lvdata, rvdata;
	left.Orrify(vcount, lvdata);
	right.Orrify(vcount, rvdata);

	// Make real selections for progressive comparisons
	SelectionVector true_vec(vcount);
	OptionalSelection true_opt(&true_vec);

	SelectionVector false_vec(vcount);
	OptionalSelection false_opt(&false_vec);

	SelectionVector maybe_vec(vcount);
	auto count = SelectNotNull(lvdata, rvdata, vcount, false_opt, maybe_vec);
	auto no_match_count = vcount - count;

	// If everything was NULL, fill in false_sel with sel
	if (count == 0) {
		ScatterSelection(false_sel, no_match_count, sel, FlatVector::INCREMENTAL_SELECTION_VECTOR);
		return count;
	}

	//	Now that we have handled the NULLs, we can use the recursive nested comparator for the rest.
	auto match_count = NestedSelector::Select<OP>(left, right, vcount, maybe_vec, count, true_opt, false_opt);
	no_match_count += (count - match_count);

	// Sort the optional selections if we would overwrite.
	if (true_sel == sel) {
		std::sort(true_vec.data(), true_vec.data() + match_count);
	}
	if (false_sel == sel) {
		std::sort(false_vec.data(), false_vec.data() + no_match_count);
	}

	// Scatter the original selection to the output selections
	ScatterSelection(true_sel, match_count, sel, true_vec);
	ScatterSelection(false_sel, no_match_count, sel, false_vec);

	return match_count;
}

idx_t VectorOperations::Equals(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                               SelectionVector *true_sel, SelectionVector *false_sel) {
	return TemplatedSelectOperation<duckdb::Equals>(left, right, sel, count, true_sel, false_sel);
}

idx_t VectorOperations::NotEquals(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                                  SelectionVector *true_sel, SelectionVector *false_sel) {
	return TemplatedSelectOperation<duckdb::NotEquals>(left, right, sel, count, true_sel, false_sel);
}

idx_t VectorOperations::GreaterThan(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                                    SelectionVector *true_sel, SelectionVector *false_sel) {
	return TemplatedSelectOperation<duckdb::GreaterThan>(left, right, sel, count, true_sel, false_sel);
}

idx_t VectorOperations::GreaterThanEquals(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                                          SelectionVector *true_sel, SelectionVector *false_sel) {
	return TemplatedSelectOperation<duckdb::GreaterThanEquals>(left, right, sel, count, true_sel, false_sel);
}

idx_t VectorOperations::LessThan(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                                 SelectionVector *true_sel, SelectionVector *false_sel) {
	return TemplatedSelectOperation<duckdb::LessThan>(left, right, sel, count, true_sel, false_sel);
}

idx_t VectorOperations::LessThanEquals(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                                       SelectionVector *true_sel, SelectionVector *false_sel) {
	return TemplatedSelectOperation<duckdb::LessThanEquals>(left, right, sel, count, true_sel, false_sel);
}

idx_t ExpressionExecutor::Select(const BoundComparisonExpression &expr, ExpressionState *state,
                                 const SelectionVector *sel, idx_t count, SelectionVector *true_sel,
                                 SelectionVector *false_sel) {
	// resolve the children
	state->intermediate_chunk.Reset();
	auto &left = state->intermediate_chunk.data[0];
	auto &right = state->intermediate_chunk.data[1];

	Execute(*expr.left, state->child_states[0].get(), sel, count, left);
	Execute(*expr.right, state->child_states[1].get(), sel, count, right);

	switch (expr.type) {
	case ExpressionType::COMPARE_EQUAL:
		return VectorOperations::Equals(left, right, sel, count, true_sel, false_sel);
	case ExpressionType::COMPARE_NOTEQUAL:
		return VectorOperations::NotEquals(left, right, sel, count, true_sel, false_sel);
	case ExpressionType::COMPARE_LESSTHAN:
		return VectorOperations::LessThan(left, right, sel, count, true_sel, false_sel);
	case ExpressionType::COMPARE_GREATERTHAN:
		return VectorOperations::GreaterThan(left, right, sel, count, true_sel, false_sel);
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return VectorOperations::LessThanEquals(left, right, sel, count, true_sel, false_sel);
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return VectorOperations::GreaterThanEquals(left, right, sel, count, true_sel, false_sel);
	case ExpressionType::COMPARE_DISTINCT_FROM:
		return VectorOperations::DistinctFrom(left, right, sel, count, true_sel, false_sel);
	case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
		return VectorOperations::NotDistinctFrom(left, right, sel, count, true_sel, false_sel);
	default:
		throw InternalException("Unknown comparison type!");
	}
}

} // namespace duckdb
