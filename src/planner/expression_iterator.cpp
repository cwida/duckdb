#include "duckdb/planner/expression_iterator.hpp"

#include "duckdb/planner/expression/list.hpp"
#include "duckdb/planner/tableref/list.hpp"
#include "duckdb/common/enum_util.hpp"

namespace duckdb {

void ExpressionIterator::EnumerateChildren(const Expression &expr,
                                           const std::function<void(const Expression &child)> &callback) {
	EnumerateChildren((Expression &)expr, [&](unique_ptr<Expression> &child) { callback(*child); });
}

void ExpressionIterator::EnumerateChildren(Expression &expr, const std::function<void(Expression &child)> &callback) {
	EnumerateChildren(expr, [&](unique_ptr<Expression> &child) { callback(*child); });
}

void ExpressionIterator::EnumerateChildren(Expression &expr,
                                           const std::function<void(unique_ptr<Expression> &child)> &callback) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_AGGREGATE: {
		auto &aggr_expr = expr.Cast<BoundAggregateExpression>();
		for (auto &child : aggr_expr.GetChildrenMutable()) {
			callback(child);
		}
		if (aggr_expr.GetFilter()) {
			callback(aggr_expr.GetFilterMutable());
		}
		if (aggr_expr.GetOrderBys()) {
			for (auto &order : aggr_expr.GetOrderBysMutable()->orders) {
				callback(order.expression);
			}
		}
		break;
	}
	case ExpressionClass::BOUND_CASE: {
		auto &case_expr = expr.Cast<BoundCaseExpression>();
		for (auto &case_check : case_expr.CaseChecksMutable()) {
			callback(case_check.when_expr);
			callback(case_check.then_expr);
		}
		callback(case_expr.ElseMutable());
		break;
	}
	case ExpressionClass::BOUND_CONJUNCTION: {
		auto &conj_expr = expr.Cast<BoundConjunctionExpression>();
		for (auto &child : conj_expr.GetChildrenMutable()) {
			callback(child);
		}
		break;
	}
	case ExpressionClass::BOUND_FUNCTION: {
		auto &func_expr = expr.Cast<BoundFunctionExpression>();
		for (auto &child : func_expr.GetChildrenMutable()) {
			callback(child);
		}
		break;
	}
	case ExpressionClass::BOUND_OPERATOR: {
		auto &op_expr = expr.Cast<BoundOperatorExpression>();
		for (auto &child : op_expr.GetChildrenMutable()) {
			callback(child);
		}
		break;
	}
	case ExpressionClass::BOUND_SUBQUERY: {
		auto &subquery_expr = expr.Cast<BoundSubqueryExpression>();
		for (auto &child : subquery_expr.GetChildrenMutable()) {
			callback(child);
		}
		break;
	}
	case ExpressionClass::BOUND_WINDOW: {
		auto &window_expr = expr.Cast<BoundWindowExpression>();
		for (auto &partition : window_expr.PartitionsMutable()) {
			callback(partition);
		}
		for (auto &order : window_expr.OrderByMutable()) {
			callback(order.expression);
		}
		for (auto &child : window_expr.GetChildrenMutable()) {
			callback(child);
		}
		if (window_expr.FilterMutable()) {
			callback(window_expr.FilterMutable());
		}
		if (window_expr.StartExprMutable()) {
			callback(window_expr.StartExprMutable());
		}
		if (window_expr.EndExprMutable()) {
			callback(window_expr.EndExprMutable());
		}
		for (auto &order : window_expr.ArgOrdersMutable()) {
			callback(order.expression);
		}
		break;
	}
	case ExpressionClass::BOUND_UNNEST: {
		auto &unnest_expr = expr.Cast<BoundUnnestExpression>();
		callback(unnest_expr.ChildMutable());
		break;
	}
	case ExpressionClass::BOUND_COLUMN_REF:
	// TODO: enumerate the lambda body here. That would give volatility, CanThrow and Hash for free,
	// but the body's BoundReferenceExpressions index the lambda's own input chunk rather than the
	// outer one, so every pass that rewrites references must first be shown to leave them alone.
	case ExpressionClass::BOUND_LAMBDA:
	case ExpressionClass::BOUND_LAMBDA_REF:
	case ExpressionClass::BOUND_CONSTANT:
	case ExpressionClass::BOUND_DEFAULT:
	case ExpressionClass::BOUND_PARAMETER:
	case ExpressionClass::BOUND_REF:
		// these node types have no children
		break;
	default:
		throw InternalException("ExpressionIterator used on unbound expression");
	}
}

void ExpressionIterator::EnumerateExpression(unique_ptr<Expression> &expr,
                                             const std::function<void(Expression &child)> &callback) {
	if (!expr) {
		return;
	}
	callback(*expr);
	ExpressionIterator::EnumerateChildren(*expr,
	                                      [&](unique_ptr<Expression> &child) { EnumerateExpression(child, callback); });
}

void ExpressionIterator::EnumerateExpression(unique_ptr<Expression> &expr,
                                             const std::function<void(unique_ptr<Expression> &child)> &callback) {
	if (!expr) {
		return;
	}
	callback(expr);
	ExpressionIterator::EnumerateChildren(*expr,
	                                      [&](unique_ptr<Expression> &child) { EnumerateExpression(child, callback); });
}

void ExpressionIterator::VisitExpressionClass(const Expression &expr, ExpressionClass expr_class,
                                              const std::function<void(const Expression &child)> &callback) {
	if (expr.GetExpressionClass() == expr_class) {
		callback(expr);
		return;
	}
	ExpressionIterator::EnumerateChildren(
	    expr, [&](const Expression &child) { VisitExpressionClass(child, expr_class, callback); });
}

void ExpressionIterator::VisitExpressionClassMutable(
    unique_ptr<Expression> &expr, ExpressionClass expr_class,
    const std::function<void(unique_ptr<Expression> &child)> &callback) {
	if (expr->GetExpressionClass() == expr_class) {
		callback(expr);
		return;
	}
	ExpressionIterator::EnumerateChildren(
	    *expr, [&](unique_ptr<Expression> &child) { VisitExpressionClassMutable(child, expr_class, callback); });
}

static void ReplaceExpressionInPlace(unique_ptr<Expression> &expr, const Expression &target,
                                     const Expression &replacement) {
	if (expr->Equals(target)) {
		expr = replacement.Copy();
		return;
	}
	ExpressionIterator::EnumerateChildren(
	    *expr, [&](unique_ptr<Expression> &child) { ReplaceExpressionInPlace(child, target, replacement); });
}

unique_ptr<Expression> ExpressionIterator::ReplaceExpression(const Expression &expr, const Expression &target,
                                                             const Expression &replacement) {
	auto result = expr.Copy();
	ReplaceExpressionInPlace(result, target, replacement);
	return result;
}

} // namespace duckdb
