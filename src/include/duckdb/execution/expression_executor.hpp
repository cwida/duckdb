//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/expression_executor.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/unordered_map.hpp"
#include "duckdb/execution/expression_executor_state.hpp"
#include "duckdb/planner/bound_tokens.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/common/enums/debug_vector_verification.hpp"

namespace duckdb {
class Allocator;
class ClientContext;
class ExecutionContext;

//! ExpressionExecutor is responsible for executing a set of expressions and storing the result in a data chunk
class ExpressionExecutor {
	friend class BoundIndex;

public:
	DUCKDB_API explicit ExpressionExecutor(ClientContext &context);
	DUCKDB_API ExpressionExecutor(ClientContext &context, const Expression *expression);
	DUCKDB_API ExpressionExecutor(ClientContext &context, const Expression &expression);
	DUCKDB_API ExpressionExecutor(ClientContext &context, const vector<unique_ptr<Expression>> &expressions);
	ExpressionExecutor(ExpressionExecutor &&) = delete;

	//! The expressions of the executor
	vector<const Expression *> expressions;
	//! The data chunk of the current physical operator, used to resolve
	//! column references and determines the output cardinality
	DataChunk *chunk = nullptr;

public:
	bool HasContext();
	ClientContext &GetContext();
	optional_ptr<ClientContext> GetContextPtr();
	Allocator &GetAllocator();

	//! Add an expression to the set of to-be-executed expressions of the executor
	DUCKDB_API void AddExpression(const Expression &expr);
	void ClearExpressions();

	//! Execute the set of expressions with the given input chunk and store the result in the output chunk
	DUCKDB_API void Execute(DataChunk *input, DataChunk &result);
	inline void Execute(DataChunk &input, DataChunk &result) {
		Execute(&input, result);
	}
	inline void Execute(DataChunk &result) {
		Execute(nullptr, result);
	}

	//! Execute the ExpressionExecutor and put the result in the result vector; this should only be used for expression
	//! executors with a single expression
	DUCKDB_API void ExecuteExpression(DataChunk &input, Vector &result);
	//! Execute the expression only on rows described by the 'sel', write to those same row indices in 'result'
	DUCKDB_API void ExecuteExpression(DataChunk &input, Vector &result, const SelectionVector &sel, idx_t count);
	//! Execute the ExpressionExecutor and put the result in the result vector; this should only be used for expression
	//! executors with a single expression
	DUCKDB_API void ExecuteExpression(Vector &result);
	//! Execute the ExpressionExecutor and generate a selection vector from all true values in the result; this should
	//! only be used with a single boolean expression
	DUCKDB_API idx_t SelectExpression(DataChunk &input, SelectionVector &sel);

	DUCKDB_API idx_t SelectExpression(DataChunk &input, SelectionVector &result_sel,
	                                  optional_ptr<SelectionVector> current_sel, idx_t current_count);

	DUCKDB_API idx_t SelectExpression(DataChunk &input, optional_ptr<SelectionVector> true_sel,
	                                  optional_ptr<SelectionVector> false_sel,
	                                  optional_ptr<SelectionVector> current_sel, idx_t current_count);

	//! Execute the expression with index `expr_idx` and store the result in the result vector
	DUCKDB_API void ExecuteExpression(idx_t expr_idx, Vector &result);
	//! Evaluate a scalar expression and fold it into a single value
	DUCKDB_API static Value EvaluateScalar(ClientContext &context, const Expression &expr,
	                                       bool allow_unfoldable = false);
	//! Try to evaluate a scalar expression and fold it into a single value, returns false if an exception is thrown
	DUCKDB_API static bool TryEvaluateScalar(ClientContext &context, const Expression &expr, Value &result);

	//! Initialize the state of a given expression
	static unique_ptr<ExpressionState> InitializeState(const Expression &expr, ExpressionExecutorState &state);

	inline void SetChunk(DataChunk *chunk) {
		this->chunk = chunk;
	}
	inline void SetChunk(DataChunk &chunk) {
		SetChunk(&chunk);
	}

	DUCKDB_API vector<unique_ptr<ExpressionExecutorState>> &GetStates();

protected:
	void Initialize(const Expression &expr, ExpressionExecutorState &state);

	static unique_ptr<ExpressionState> InitializeState(const BoundReferenceExpression &expr,
	                                                   ExpressionExecutorState &state);
	static unique_ptr<ExpressionState> InitializeState(const BoundCaseExpression &expr, ExpressionExecutorState &state);
	static unique_ptr<ExpressionState> InitializeState(const BoundConjunctionExpression &expr,
	                                                   ExpressionExecutorState &state);
	static unique_ptr<ExpressionState> InitializeState(const BoundConstantExpression &expr,
	                                                   ExpressionExecutorState &state);
	static unique_ptr<ExpressionState> InitializeState(const BoundFunctionExpression &expr,
	                                                   ExpressionExecutorState &state);
	static unique_ptr<ExpressionState> InitializeState(const BoundLambdaExpression &expr,
	                                                   ExpressionExecutorState &state);
	static unique_ptr<ExpressionState> InitializeState(const BoundOperatorExpression &expr,
	                                                   ExpressionExecutorState &state);
	static unique_ptr<ExpressionState> InitializeState(const BoundParameterExpression &expr,
	                                                   ExpressionExecutorState &state);

	void Execute(const Expression &expr, ExpressionState *state, const SelectionVector *sel, idx_t count,
	             Vector &result);

	void Execute(const BoundCaseExpression &expr, ExpressionState *state, const SelectionVector *sel, idx_t count,
	             Vector &result);
	void Execute(const BoundConjunctionExpression &expr, ExpressionState *state, const SelectionVector *sel,
	             idx_t count, Vector &result);
	void Execute(const BoundConstantExpression &expr, ExpressionState *state, const SelectionVector *sel, idx_t count,
	             Vector &result);
	void Execute(const BoundFunctionExpression &expr, ExpressionState *state, const SelectionVector *sel, idx_t count,
	             Vector &result);
	void Execute(const BoundLambdaExpression &expr, ExpressionState *state, const SelectionVector *sel, idx_t count,
	             Vector &result);
	void Execute(const BoundOperatorExpression &expr, ExpressionState *state, const SelectionVector *sel, idx_t count,
	             Vector &result);
	void Execute(const BoundParameterExpression &expr, ExpressionState *state, const SelectionVector *sel, idx_t count,
	             Vector &result);
	void Execute(const BoundReferenceExpression &expr, ExpressionState *state, const SelectionVector *sel, idx_t count,
	             Vector &result);

	//! Execute the (boolean-returning) expression and generate a selection vector with all entries that are "true" in
	//! the result
	idx_t Select(const Expression &expr, ExpressionState *state, const SelectionVector *sel, idx_t count,
	             SelectionVector *true_sel, SelectionVector *false_sel);
	idx_t DefaultSelect(const Expression &expr, ExpressionState *state, const SelectionVector *sel, idx_t count,
	                    SelectionVector *true_sel, SelectionVector *false_sel);

	idx_t Select(const BoundConjunctionExpression &expr, ExpressionState *state, const SelectionVector *sel,
	             idx_t count, SelectionVector *true_sel, SelectionVector *false_sel);
	idx_t Select(const BoundFunctionExpression &expr, ExpressionState *state, const SelectionVector *sel, idx_t count,
	             SelectionVector *true_sel, SelectionVector *false_sel);
	idx_t Select(const BoundOperatorExpression &expr, ExpressionState &state, optional_ptr<const SelectionVector> sel,
	             idx_t count, optional_ptr<SelectionVector> true_sel, optional_ptr<SelectionVector> false_sel);

	//! Verify that the output of a step in the ExpressionExecutor is correct
	void Verify(const Expression &expr, Vector &result, idx_t count);

	void FillSwitch(const Vector &vector, Vector &result, const SelectionVector &sel, sel_t count);

private:
	//! Client context
	optional_ptr<ClientContext> context;
	//! The states of the expression executor; this holds any intermediates and temporary states of expressions
	vector<unique_ptr<ExpressionExecutorState>> states;
	//! The vector verification (debug setting)
	DebugVectorVerification debug_vector_verification = DebugVectorVerification::NONE;

private:
	// it is possible to create an expression executor without a ClientContext - but it should be avoided
	DUCKDB_API ExpressionExecutor();
	DUCKDB_API explicit ExpressionExecutor(const vector<unique_ptr<Expression>> &exprs);
};
} // namespace duckdb
