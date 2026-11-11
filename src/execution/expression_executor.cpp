#include "duckdb/execution/expression_executor.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/main/context.hpp"
#include "duckdb/main/database.hpp"

#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/execution_context.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/planner/expression/list.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"
#include "duckdb/common/type_visitor.hpp"
#include "duckdb/storage/table/variant_column_data.hpp"

namespace duckdb {

ExpressionExecutor::ExpressionExecutor(Context &context_p) : context(context_p) {
	debug_vector_verification = Settings::Get<DebugVerifyVectorSetting>(context);
}

ExpressionExecutor::ExpressionExecutor(Context &context, const Expression *expression) : ExpressionExecutor(context) {
	D_ASSERT(expression);
	AddExpression(*expression);
}

ExpressionExecutor::ExpressionExecutor(Context &context, const Expression &expression) : ExpressionExecutor(context) {
	AddExpression(expression);
}

ExpressionExecutor::ExpressionExecutor(Context &context, const vector<unique_ptr<Expression>> &exprs)
    : ExpressionExecutor(context) {
	D_ASSERT(exprs.size() > 0);
	for (auto &expr : exprs) {
		AddExpression(*expr);
	}
}

ExpressionExecutor::ExpressionExecutor(ClientContext &context) : ExpressionExecutor(context.GetContext()) {
}

ExpressionExecutor::ExpressionExecutor(ClientContext &context, const Expression *expression)
    : ExpressionExecutor(context.GetContext(), expression) {
}

ExpressionExecutor::ExpressionExecutor(ClientContext &context, const Expression &expression)
    : ExpressionExecutor(context.GetContext(), expression) {
}

ExpressionExecutor::ExpressionExecutor(ClientContext &context, const vector<unique_ptr<Expression>> &exprs)
    : ExpressionExecutor(context.GetContext(), exprs) {
}

ExpressionExecutor::ExpressionExecutor(DatabaseInstance &db) : ExpressionExecutor(db.GetContext()) {
}

ExpressionExecutor::ExpressionExecutor(DatabaseInstance &db, const Expression &expression) : ExpressionExecutor(db) {
	AddExpression(expression);
}

ExpressionExecutor::ExpressionExecutor(DatabaseInstance &db, const vector<unique_ptr<Expression>> &exprs)
    : ExpressionExecutor(db) {
	D_ASSERT(exprs.size() > 0);
	for (auto &expr : exprs) {
		AddExpression(*expr);
	}
}

Context &ExpressionExecutor::GetContext() {
	return context;
}

bool ExpressionExecutor::HasClientContext() {
	return context.IsClient();
}

ClientContext &ExpressionExecutor::GetClientContext() {
	auto client_context = context.TryGetClientContext();
	if (!client_context) {
		throw InternalException(
		    "Calling ExpressionExecutor::GetClientContext on an expression executor without a client context");
	}
	return *client_context;
}

optional_ptr<ClientContext> ExpressionExecutor::TryGetClientContext() {
	return context.TryGetClientContext();
}

Allocator &ExpressionExecutor::GetAllocator() {
	return Allocator::Get(context.GetDatabase());
}

void ExpressionExecutor::AddExpression(const Expression &expr) {
	expressions.push_back(&expr);
	auto state = make_uniq<ExpressionExecutorState>();
	Initialize(expr, *state);
	state->Verify();
	states.push_back(std::move(state));
}

void ExpressionExecutor::ClearExpressions() {
	states.clear();
	expressions.clear();
}

void ExpressionExecutor::Initialize(const Expression &expression, ExpressionExecutorState &state) {
	state.executor = this;
	state.root_state = InitializeState(expression, state);
}

void ExpressionExecutor::Execute(DataChunk *input, DataChunk &result) {
	SetChunk(input);
	D_ASSERT(expressions.size() == result.ColumnCount());
	D_ASSERT(!expressions.empty());

	for (idx_t i = 0; i < expressions.size(); i++) {
		ExecuteExpression(i, result.data[i]);
	}
	result.SetChildCardinality(input ? input->size() : 1);
	result.Verify(context.GetDatabase());
}

void ExpressionExecutor::ExecuteExpression(DataChunk &input, Vector &result) {
	SetChunk(&input);
	ExecuteExpression(result);
}

void ExpressionExecutor::ExecuteExpression(DataChunk &input, Vector &result, const SelectionVector &sel, idx_t count) {
	SetChunk(&input);
	D_ASSERT(!expressions.empty());
	auto &expression = expressions[0];
	auto &state = states[0];
	D_ASSERT(result.GetType().id() == expression->GetReturnType().id());
	Execute(*expression, state->root_state.get(), &sel, count, result);
}

idx_t ExpressionExecutor::SelectExpression(DataChunk &input, SelectionVector &sel) {
	return SelectExpression(input, sel, nullptr, input.size());
}

idx_t ExpressionExecutor::SelectExpression(DataChunk &input, SelectionVector &result_sel,
                                           optional_ptr<SelectionVector> current_sel, idx_t current_count) {
	return SelectExpression(input, result_sel, nullptr, current_sel, current_count);
}

idx_t ExpressionExecutor::SelectExpression(DataChunk &input, optional_ptr<SelectionVector> true_sel,
                                           optional_ptr<SelectionVector> false_sel,
                                           optional_ptr<SelectionVector> current_sel, idx_t current_count) {
	D_ASSERT(expressions.size() == 1);
	D_ASSERT(current_count <= input.size());
	SetChunk(&input);
	idx_t selected_tuples = Select(*expressions[0], states[0]->root_state.get(), current_sel.get(), current_count,
	                               true_sel.get(), false_sel.get());
	return selected_tuples;
}

void ExpressionExecutor::ExecuteExpression(Vector &result) {
	D_ASSERT(expressions.size() == 1);
	ExecuteExpression(0, result);
}

void ExpressionExecutor::ExecuteExpression(idx_t expr_idx, Vector &result) {
	D_ASSERT(expr_idx < expressions.size());
	D_ASSERT(result.GetType().id() == expressions[expr_idx]->GetReturnType().id());
	Execute(*expressions[expr_idx], states[expr_idx]->root_state.get(), nullptr, chunk ? chunk->size() : 1, result);
}

void ExpressionExecutor::ExecuteExpression(idx_t expr_idx, Vector &result, const SelectionVector &sel, idx_t count) {
	D_ASSERT(expr_idx < expressions.size());
	Execute(*expressions[expr_idx], states[expr_idx]->root_state.get(), &sel, count, result);
}

Value ExpressionExecutor::EvaluateScalar(ClientContext &context, const Expression &expr, bool allow_unfoldable) {
	return EvaluateScalar(context.GetContext(), expr, allow_unfoldable);
}

Value ExpressionExecutor::EvaluateScalar(Context &context, const Expression &expr, bool allow_unfoldable) {
	D_ASSERT(allow_unfoldable || expr.IsFoldable());
	D_ASSERT(expr.IsScalar());
	// use an ExpressionExecutor to execute the expression
	ExpressionExecutor executor(context, expr);

	Vector result(expr.GetReturnType());
	executor.ExecuteExpression(result);

	D_ASSERT(allow_unfoldable || result.GetVectorType() == VectorType::CONSTANT_VECTOR);
	auto result_value = result.GetValue(0);
	D_ASSERT(result_value.type().InternalType() == expr.GetReturnType().InternalType());
	return result_value.WithType(expr.GetReturnType());
}

bool ExpressionExecutor::TryEvaluateScalar(ClientContext &context, const Expression &expr, Value &result) {
	return TryEvaluateScalar(context.GetContext(), expr, result);
}

bool ExpressionExecutor::TryEvaluateScalar(Context &context, const Expression &expr, Value &result) {
	try {
		result = EvaluateScalar(context, expr);
		return true;
	} catch (InternalException &ex) {
		throw;
	} catch (...) {
		return false;
	}
}

void ExpressionExecutor::Verify(const Expression &expr, Vector &vector, idx_t count) {
	D_ASSERT(expr.GetReturnType().id() == vector.GetType().id());
	vector.Verify();
	if (expr.GetVerificationStats()) {
		expr.GetVerificationStats()->Verify(vector, count);
	}
	if (debug_vector_verification == DebugVectorVerification::DICTIONARY_EXPRESSION) {
		Vector::DebugTransformToDictionary(vector);
	}
	if (debug_vector_verification == DebugVectorVerification::VARIANT_VECTOR) {
		// LAMBDA is not a value - its slot only holds a placeholder, so there is nothing to round-trip
		if (TypeVisitor::Contains(vector.GetType(), [](const LogicalType &type) {
			    if (type.IsJSONType() || type.id() == LogicalTypeId::VARIANT || type.id() == LogicalTypeId::UNION ||
			        type.id() == LogicalTypeId::ENUM || type.id() == LogicalTypeId::TYPE ||
			        type.id() == LogicalTypeId::TUPLE || type.id() == LogicalTypeId::LAMBDA) {
				    return true;
			    }
			    return false;
		    })) {
			return;
		}

		// preserve the input vector type if it was constant - the cast roundtrip below would otherwise flatten it,
		// breaking callers that rely on constant-value defaults staying constant (e.g. remap_struct)
		const bool input_is_constant = vector.GetVectorType() == VectorType::CONSTANT_VECTOR;
		const idx_t cast_count = input_is_constant ? 1 : count;

		Vector intermediate(LogicalType::VARIANT(), cast_count);

		//! First cast to VARIANT
		VectorOperations::Cast(GetContext(), vector, intermediate, cast_count, true);
		intermediate.Verify();
		//! FIXME: this is probably also where we want to test 'variant_normalize'

		Vector result(vector.GetType(), cast_count);
		//! Then cast back into the original type
		VectorOperations::Cast(GetContext(), intermediate, result, cast_count, true);
		if (input_is_constant) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
			FlatVector::SetSize(result, count_t(count));
		}
		vector.Reference(result);
		vector.Verify();
	}
	if (debug_vector_verification == DebugVectorVerification::SHREDDED_VECTOR) {
		//! Shred (top-level) VARIANT vectors based on the schema of their first value, so downstream
		//! operators are exercised against shredded (and partially-shredded) variant vectors.
		//! A SHREDDED_VECTOR is never a constant vector - skip constant vectors so we don't break callers
		//! that require a constant result (e.g. scalar expression folding in EvaluateScalar).
		if (vector.GetType().id() == LogicalTypeId::VARIANT && vector.GetVectorType() != VectorType::CONSTANT_VECTOR) {
			VariantColumnData::DebugShred(vector, count);
			vector.Verify();
		}
	}
}

unique_ptr<ExpressionState> ExpressionExecutor::InitializeState(const Expression &expr,
                                                                ExpressionExecutorState &state) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_REF:
		return InitializeState(expr.Cast<BoundReferenceExpression>(), state);
	case ExpressionClass::BOUND_CASE:
		return InitializeState(expr.Cast<BoundCaseExpression>(), state);
	case ExpressionClass::BOUND_CONJUNCTION:
		return InitializeState(expr.Cast<BoundConjunctionExpression>(), state);
	case ExpressionClass::BOUND_CONSTANT:
		return InitializeState(expr.Cast<BoundConstantExpression>(), state);
	case ExpressionClass::BOUND_FUNCTION:
		return InitializeState(expr.Cast<BoundFunctionExpression>(), state);
	case ExpressionClass::BOUND_LAMBDA:
		return InitializeState(expr.Cast<BoundLambdaExpression>(), state);
	case ExpressionClass::BOUND_OPERATOR:
		return InitializeState(expr.Cast<BoundOperatorExpression>(), state);
	case ExpressionClass::BOUND_PARAMETER:
		return InitializeState(expr.Cast<BoundParameterExpression>(), state);
	default:
		throw InternalException("Attempting to initialize state of expression of unknown type!");
	}
}

void ExpressionExecutor::Execute(const Expression &expr, ExpressionState *state, const SelectionVector *sel,
                                 idx_t count, Vector &result) {
#ifdef DEBUG
	// The result vector must be used for the first time, or must be reset.
	// Otherwise, the validity mask can contain previous (now incorrect) data.
	if (result.GetVectorType() == VectorType::FLAT_VECTOR) {
		// We do not initialize vector caches for these expressions.
		if (expr.GetExpressionClass() != ExpressionClass::BOUND_REF &&
		    expr.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT &&
		    expr.GetExpressionClass() != ExpressionClass::BOUND_PARAMETER) {
			D_ASSERT(FlatVector::Validity(result).CheckAllValid(count));
		}
	}
#endif

	if (count == 0) {
		return;
	}
	if (result.GetType().id() != expr.GetReturnType().id()) {
		throw InternalException(
		    "ExpressionExecutor::Execute called with a result vector of type %s that does not match expression type %s",
		    result.GetType(), expr.GetReturnType());
	}
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_REF:
		Execute(expr.Cast<BoundReferenceExpression>(), state, sel, count, result);
		break;
	case ExpressionClass::BOUND_CASE:
		Execute(expr.Cast<BoundCaseExpression>(), state, sel, count, result);
		break;
	case ExpressionClass::BOUND_CONJUNCTION:
		Execute(expr.Cast<BoundConjunctionExpression>(), state, sel, count, result);
		break;
	case ExpressionClass::BOUND_CONSTANT:
		Execute(expr.Cast<BoundConstantExpression>(), state, sel, count, result);
		break;
	case ExpressionClass::BOUND_FUNCTION:
		Execute(expr.Cast<BoundFunctionExpression>(), state, sel, count, result);
		break;
	case ExpressionClass::BOUND_LAMBDA:
		Execute(expr.Cast<BoundLambdaExpression>(), state, sel, count, result);
		break;
	case ExpressionClass::BOUND_OPERATOR:
		Execute(expr.Cast<BoundOperatorExpression>(), state, sel, count, result);
		break;
	case ExpressionClass::BOUND_PARAMETER:
		Execute(expr.Cast<BoundParameterExpression>(), state, sel, count, result);
		break;
	default:
		throw InternalException("Attempting to execute expression of unknown type!");
	}
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_REF && result.size() != count_t(count)) {
		// BoundReferenceExpression shares buffer with its source - we cannot resize it
		// all other expressions produce a fresh result vector that we own
		FlatVector::SetSize(result, count_t(count));
	}
	Verify(expr, result, count);
}

idx_t ExpressionExecutor::Select(const Expression &expr, ExpressionState *state, const SelectionVector *sel,
                                 idx_t count, SelectionVector *true_sel, SelectionVector *false_sel) {
	if (count == 0) {
		return 0;
	}
	D_ASSERT(true_sel || false_sel);
	D_ASSERT(expr.GetReturnType().id() == LogicalTypeId::BOOLEAN);
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_CONJUNCTION:
		return Select(expr.Cast<BoundConjunctionExpression>(), state, sel, count, true_sel, false_sel);
	case ExpressionClass::BOUND_FUNCTION:
		return Select(expr.Cast<BoundFunctionExpression>(), state, sel, count, true_sel,
		              false_sel); // NOLINT: c-style cast
	case ExpressionClass::BOUND_OPERATOR:
		return Select(expr.Cast<BoundOperatorExpression>(), *state, sel, count, true_sel, false_sel);
	default:
		return DefaultSelect(expr, state, sel, count, true_sel, false_sel);
	}
}

idx_t ExpressionExecutor::DefaultSelect(const Expression &expr, ExpressionState *state, const SelectionVector *sel,
                                        idx_t count, SelectionVector *true_sel, SelectionVector *false_sel) {
	// generic selection of boolean expression:
	// resolve the true/false expression first
	// then use that to generate the selection vector
	bool intermediate_bools[STANDARD_VECTOR_SIZE];
	Vector intermediate(LogicalType::BOOLEAN, data_ptr_cast(intermediate_bools), STANDARD_VECTOR_SIZE);
	Execute(expr, state, sel, count, intermediate);

	if (!sel && intermediate.GetVectorType() == VectorType::FLAT_VECTOR &&
	    !FlatVector::Validity(intermediate).CanHaveNull()) {
		optional<array_ptr<sel_t>> true_output;
		optional<array_ptr<sel_t>> false_output;
		if (true_sel) {
			true_output = array_ptr<sel_t>(true_sel->data(), true_sel->Capacity());
		}
		if (false_sel) {
			false_output = array_ptr<sel_t>(false_sel->data(), false_sel->Capacity());
		}
		return SelectionVector::SelectFromBoolean(FlatVector::GetData<bool>(intermediate), nullptr, count, true_output,
		                                          false_output);
	}

	return UnaryExecutor::Select<uint8_t>(
	    intermediate, sel, count, [](uint8_t value) { return value > 0; }, true_sel, false_sel);
}

vector<unique_ptr<ExpressionExecutorState>> &ExpressionExecutor::GetStates() {
	return states;
}

} // namespace duckdb
