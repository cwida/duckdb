#include "duckdb/execution/expression_executor_state.hpp"

#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/storage/standard_buffer_manager.hpp"

namespace duckdb {

void ExpressionState::AddChild(const Expression &child_expr) {
	types.push_back(child_expr.GetReturnType());
	auto child_state = ExpressionExecutor::InitializeState(child_expr, root);
	child_states.push_back(std::move(child_state));

	auto expr_class = child_expr.GetExpressionClass();
	auto initialize_child = expr_class != ExpressionClass::BOUND_REF && expr_class != ExpressionClass::BOUND_CONSTANT &&
	                        expr_class != ExpressionClass::BOUND_PARAMETER;
	initialize.push_back(initialize_child);
}

void ExpressionState::Finalize() {
	if (types.empty()) {
		return;
	}
	auto &allocator = BufferAllocator::Get(GetContext());
	intermediate_chunk.Initialize(allocator, types, initialize);
}

Allocator &ExpressionState::GetAllocator() {
	return root.executor->GetAllocator();
}

Context &ExpressionState::GetContext() {
	return root.executor->GetContext();
}

bool ExpressionState::HasClientContext() {
	return root.executor->HasClientContext();
}

ClientContext &ExpressionState::GetClientContext() {
	if (!HasClientContext()) {
		throw BinderException("Cannot use %s in this context",
		                      (expr.Cast<BoundFunctionExpression>()).Function().GetName());
	}
	return root.executor->GetClientContext();
}

ExpressionState::ExpressionState(const Expression &expr, ExpressionExecutorState &root) : expr(expr), root(root) {
}

ExpressionExecutorState::ExpressionExecutorState() {
}

void ExpressionState::Verify(ExpressionExecutorState &root_executor) {
	D_ASSERT(&root_executor == &root);
	for (auto &entry : child_states) {
		entry->Verify(root_executor);
	}
}

void ExpressionState::ResetDictionaryStates() {
	for (const auto &child : child_states) {
		child->ResetDictionaryStates();
	}
}

void ExpressionExecutorState::Verify() {
	D_ASSERT(executor);
	root_state->Verify(*this);
}

} // namespace duckdb
