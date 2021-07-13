#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/execution_context.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"

namespace duckdb {

ExpressionExecutor::ExpressionExecutor() : random(0) {
}

ExpressionExecutor::ExpressionExecutor(const Expression *expression) : ExpressionExecutor() {
	D_ASSERT(expression);
	AddExpression(*expression);
}

ExpressionExecutor::ExpressionExecutor(const Expression &expression) : ExpressionExecutor() {
	AddExpression(expression);
}

ExpressionExecutor::ExpressionExecutor(const vector<unique_ptr<Expression>> &exprs) : ExpressionExecutor() {
	D_ASSERT(exprs.size() > 0);
	for (auto &expr : exprs) {
		AddExpression(*expr);
	}
}

void ExpressionExecutor::AddExpression(const Expression &expr) {
	expressions.push_back(&expr);
	auto state = make_unique<ExpressionExecutorState>(expr.ToString());
	Initialize(expr, *state);
	states.push_back(move(state));
}

void ExpressionExecutor::Initialize(const Expression &expression, ExpressionExecutorState &state) {
	state.root_state = InitializeState(expression, state);
	state.executor = this;
}

void ExpressionExecutor::Execute(DataChunk *input, DataChunk &result) {
	SetChunk(input);
	D_ASSERT(expressions.size() == result.ColumnCount());
	D_ASSERT(!expressions.empty());

	for (idx_t i = 0; i < expressions.size(); i++) {
		ExecuteExpression(i, result.data[i]);
	}
	result.SetCardinality(input ? input->size() : 1);
	result.Verify();
}

void ExpressionExecutor::ExecuteExpression(DataChunk &input, Vector &result) {
	SetChunk(&input);
	ExecuteExpression(result);
}

idx_t ExpressionExecutor::SelectExpression(DataChunk &input, SelectionVector &sel) {
	D_ASSERT(expressions.size() == 1);
	SetChunk(&input);
	states[0]->profiler.BeginSample();
	idx_t selected_tuples = Select(*expressions[0], states[0]->root_state.get(), nullptr, input.size(), &sel, nullptr);
	states[0]->profiler.EndSample(chunk ? chunk->size() : 0);
	return selected_tuples;
}

void ExpressionExecutor::ExecuteExpression(Vector &result) {
	D_ASSERT(expressions.size() == 1);
	ExecuteExpression(0, result);
}

void ExpressionExecutor::ExecuteExpression(idx_t expr_idx, Vector &result) {
	D_ASSERT(expr_idx < expressions.size());
	D_ASSERT(result.GetType() == expressions[expr_idx]->return_type);
	states[expr_idx]->profiler.BeginSample();
	Execute(*expressions[expr_idx], states[expr_idx]->root_state.get(), nullptr, chunk ? chunk->size() : 1, result);
	states[expr_idx]->profiler.EndSample(chunk ? chunk->size() : 0);
}

Value ExpressionExecutor::EvaluateScalar(const Expression &expr) {
	D_ASSERT(expr.IsFoldable());
	// use an ExpressionExecutor to execute the expression
	ExpressionExecutor executor(expr);

	Vector result(expr.return_type);
	executor.ExecuteExpression(result);

	D_ASSERT(result.GetVectorType() == VectorType::CONSTANT_VECTOR);
	auto result_value = result.GetValue(0);
	D_ASSERT(result_value.type() == expr.return_type);
	return result_value;
}

void ExpressionExecutor::Verify(const Expression &expr, Vector &vector, idx_t count) {
	D_ASSERT(expr.return_type == vector.GetType());
	vector.Verify(count);
	if (expr.stats) {
		expr.stats->Verify(vector, count);
	}
}

unique_ptr<ExpressionState> ExpressionExecutor::InitializeState(const Expression &expr,
                                                                ExpressionExecutorState &state) {
	switch (expr.expression_class) {
	case ExpressionClass::BOUND_REF:
		return InitializeState((const BoundReferenceExpression &)expr, state);
	case ExpressionClass::BOUND_BETWEEN:
		return InitializeState((const BoundBetweenExpression &)expr, state);
	case ExpressionClass::BOUND_CASE:
		return InitializeState((const BoundCaseExpression &)expr, state);
	case ExpressionClass::BOUND_CAST:
		return InitializeState((const BoundCastExpression &)expr, state);
	case ExpressionClass::BOUND_COMPARISON:
		return InitializeState((const BoundComparisonExpression &)expr, state);
	case ExpressionClass::BOUND_CONJUNCTION:
		return InitializeState((const BoundConjunctionExpression &)expr, state);
	case ExpressionClass::BOUND_CONSTANT:
		return InitializeState((const BoundConstantExpression &)expr, state);
	case ExpressionClass::BOUND_FUNCTION:
		return InitializeState((const BoundFunctionExpression &)expr, state);
	case ExpressionClass::BOUND_OPERATOR:
		return InitializeState((const BoundOperatorExpression &)expr, state);
	case ExpressionClass::BOUND_PARAMETER:
		return InitializeState((const BoundParameterExpression &)expr, state);
	default:
		throw InternalException("Attempting to initialize state of expression of unknown type!");
	}
}

void ExpressionExecutor::Execute(const Expression &expr, ExpressionState *state, const SelectionVector *sel,
                                 idx_t count, Vector &result) {
	if (count == 0) {
		return;
	}
	switch (expr.expression_class) {
	case ExpressionClass::BOUND_BETWEEN:
		Execute((const BoundBetweenExpression &)expr, state, sel, count, result);
		break;
	case ExpressionClass::BOUND_REF:
		Execute((const BoundReferenceExpression &)expr, state, sel, count, result);
		break;
	case ExpressionClass::BOUND_CASE:
		Execute((const BoundCaseExpression &)expr, state, sel, count, result);
		break;
	case ExpressionClass::BOUND_CAST:
		Execute((const BoundCastExpression &)expr, state, sel, count, result);
		break;
	case ExpressionClass::BOUND_COMPARISON:
		Execute((const BoundComparisonExpression &)expr, state, sel, count, result);
		break;
	case ExpressionClass::BOUND_CONJUNCTION:
		Execute((const BoundConjunctionExpression &)expr, state, sel, count, result);
		break;
	case ExpressionClass::BOUND_CONSTANT:
		Execute((const BoundConstantExpression &)expr, state, sel, count, result);
		break;
	case ExpressionClass::BOUND_FUNCTION:
		Execute((const BoundFunctionExpression &)expr, state, sel, count, result);
		break;
	case ExpressionClass::BOUND_OPERATOR:
		Execute((const BoundOperatorExpression &)expr, state, sel, count, result);
		break;
	case ExpressionClass::BOUND_PARAMETER:
		Execute((const BoundParameterExpression &)expr, state, sel, count, result);
		break;
	default:
		throw InternalException("Attempting to execute expression of unknown type!");
	}
	Verify(expr, result, count);
}

idx_t ExpressionExecutor::Select(const Expression &expr, ExpressionState *state, const SelectionVector *sel,
                                 idx_t count, SelectionVector *true_sel, SelectionVector *false_sel) {
	if (count == 0) {
		return 0;
	}
	D_ASSERT(true_sel || false_sel);
	D_ASSERT(expr.return_type.id() == LogicalTypeId::BOOLEAN);
	switch (expr.expression_class) {
	case ExpressionClass::BOUND_BETWEEN:
		return Select((BoundBetweenExpression &)expr, state, sel, count, true_sel, false_sel);
	case ExpressionClass::BOUND_COMPARISON:
		return Select((BoundComparisonExpression &)expr, state, sel, count, true_sel, false_sel);
	case ExpressionClass::BOUND_CONJUNCTION:
		return Select((BoundConjunctionExpression &)expr, state, sel, count, true_sel, false_sel);
	default:
		return DefaultSelect(expr, state, sel, count, true_sel, false_sel);
	}
}

template <bool NO_NULL, bool HAS_TRUE_SEL, bool HAS_FALSE_SEL>
static inline idx_t DefaultSelectLoop(const SelectionVector *bsel, uint8_t *__restrict bdata, ValidityMask &mask,
                                      const SelectionVector *sel, idx_t count, SelectionVector *true_sel,
                                      SelectionVector *false_sel) {
	idx_t true_count = 0, false_count = 0;
	for (idx_t i = 0; i < count; i++) {
		auto bidx = bsel->get_index(i);
		auto result_idx = sel->get_index(i);
		if (bdata[bidx] > 0 && (NO_NULL || mask.RowIsValid(bidx))) {
			if (HAS_TRUE_SEL) {
				true_sel->set_index(true_count++, result_idx);
			}
		} else {
			if (HAS_FALSE_SEL) {
				false_sel->set_index(false_count++, result_idx);
			}
		}
	}
	if (HAS_TRUE_SEL) {
		return true_count;
	} else {
		return count - false_count;
	}
}

template <bool NO_NULL>
static inline idx_t DefaultSelectSwitch(VectorData &idata, const SelectionVector *sel, idx_t count,
                                        SelectionVector *true_sel, SelectionVector *false_sel) {
	if (true_sel && false_sel) {
		return DefaultSelectLoop<NO_NULL, true, true>(idata.sel, (uint8_t *)idata.data, idata.validity, sel, count,
		                                              true_sel, false_sel);
	} else if (true_sel) {
		return DefaultSelectLoop<NO_NULL, true, false>(idata.sel, (uint8_t *)idata.data, idata.validity, sel, count,
		                                               true_sel, false_sel);
	} else {
		D_ASSERT(false_sel);
		return DefaultSelectLoop<NO_NULL, false, true>(idata.sel, (uint8_t *)idata.data, idata.validity, sel, count,
		                                               true_sel, false_sel);
	}
}

idx_t ExpressionExecutor::DefaultSelect(const Expression &expr, ExpressionState *state, const SelectionVector *sel,
                                        idx_t count, SelectionVector *true_sel, SelectionVector *false_sel) {
	// generic selection of boolean expression:
	// resolve the true/false expression first
	// then use that to generate the selection vector
	bool intermediate_bools[STANDARD_VECTOR_SIZE];
	Vector intermediate(LogicalType::BOOLEAN, (data_ptr_t)intermediate_bools);
	Execute(expr, state, sel, count, intermediate);

	VectorData idata;
	intermediate.Orrify(count, idata);
	if (!sel) {
		sel = &FlatVector::INCREMENTAL_SELECTION_VECTOR;
	}
	if (!idata.validity.AllValid()) {
		return DefaultSelectSwitch<false>(idata, sel, count, true_sel, false_sel);
	} else {
		return DefaultSelectSwitch<true>(idata, sel, count, true_sel, false_sel);
	}
}

vector<unique_ptr<ExpressionExecutorState>> &ExpressionExecutor::GetStates() {
	return states;
}

} // namespace duckdb
