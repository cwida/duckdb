#include "duckdb/planner/subquery/has_volatile_expressions.hpp"

#include "duckdb/function/scalar/generic_functions.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"

namespace duckdb {

static bool IsVolatileIgnoringError(const Expression &expr) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_FUNCTION: {
		auto &function = expr.Cast<BoundFunctionExpression>().Function();
		if (function.HasBindLambdaCallback() ||
		    (function.GetStability() == FunctionStability::VOLATILE && !ErrorFun::IsError(function))) {
			return expr.IsVolatile();
		}
		break;
	}
	case ExpressionClass::BOUND_CASE:
	case ExpressionClass::BOUND_CONJUNCTION:
	case ExpressionClass::BOUND_OPERATOR:
		break;
	default:
		return expr.IsVolatile();
	}
	bool is_volatile = false;
	ExpressionIterator::EnumerateChildren(
	    expr, [&](const Expression &child) { is_volatile = is_volatile || IsVolatileIgnoringError(child); });
	return is_volatile;
}

bool HasVolatileExpressionsIgnoringError(const LogicalOperator &op) {
	bool is_volatile = false;
	LogicalOperatorVisitor::EnumerateExpressions(op, [&](const unique_ptr<Expression> *expression) {
		if (IsVolatileIgnoringError(**expression)) {
			is_volatile = true;
		}
	});
	if (is_volatile) {
		return true;
	}
	for (auto &child : op.children) {
		if (child && HasVolatileExpressionsIgnoringError(*child)) {
			return true;
		}
	}
	return false;
}

} // namespace duckdb
