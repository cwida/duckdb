#include "duckdb/optimizer/matcher/expression_matcher.hpp"

#include "duckdb/planner/expression/list.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"

namespace duckdb {

bool ExpressionMatcher::Match(Expression &expr, vector<reference<Expression>> &bindings) {
	if (type && !type->Match(expr.GetReturnType())) {
		return false;
	}
	if (expr_type && !expr_type->Match(expr.GetExpressionType())) {
		return false;
	}
	if (expr_class != ExpressionClass::INVALID && expr_class != expr.GetExpressionClass()) {
		return false;
	}
	bindings.push_back(expr);
	return true;
}

bool ExpressionEqualityMatcher::Match(Expression &expr, vector<reference<Expression>> &bindings) {
	if (!expr.Equals(expression)) {
		return false;
	}
	bindings.push_back(expr);
	return true;
}

bool CaseExpressionMatcher::Match(Expression &expr_p, vector<reference<Expression>> &bindings) {
	if (!ExpressionMatcher::Match(expr_p, bindings)) {
		return false;
	}
	return true;
}

bool ComparisonExpressionMatcher::Match(Expression &expr_p, vector<reference<Expression>> &bindings) {
	if (!ExpressionMatcher::Match(expr_p, bindings)) {
		return false;
	}
	auto &expr = expr_p.Cast<BoundFunctionExpression>();
	if (!BoundComparisonExpression::IsComparison(expr.GetExpressionType())) {
		return false;
	}
	return SetMatcher::Match(matchers, expr.GetChildrenMutable(), bindings, policy);
}

bool CastExpressionMatcher::Match(Expression &expr_p, vector<reference<Expression>> &bindings) {
	if (!ExpressionMatcher::Match(expr_p, bindings)) {
		return false;
	}
	if (!BoundCastExpression::IsCast(expr_p)) {
		return false;
	}
	if (!matcher) {
		return true;
	}
	auto &expr = expr_p.Cast<BoundFunctionExpression>();
	return matcher->Match(*BoundCastExpression::ChildMutable(expr), bindings);
}

bool NotExpressionMatcher::Match(Expression &expr_p, vector<reference<Expression>> &bindings) {
	if (!ExpressionMatcher::Match(expr_p, bindings)) {
		return false;
	}
	auto &children = expr_p.Cast<BoundOperatorExpression>().GetChildrenMutable();
	if (children.size() != 1) {
		return false;
	}
	if (!child) {
		return true;
	}
	return child->Match(*children[0], bindings);
}

bool InClauseExpressionMatcher::Match(Expression &expr_p, vector<reference<Expression>> &bindings) {
	if (!ExpressionMatcher::Match(expr_p, bindings)) {
		return false;
	}
	auto &expr = expr_p.Cast<BoundOperatorExpression>();
	if (expr.GetExpressionType() != ExpressionType::COMPARE_IN ||
	    expr.GetExpressionType() == ExpressionType::COMPARE_NOT_IN) {
		return false;
	}
	return SetMatcher::Match(matchers, expr.GetChildrenMutable(), bindings, policy);
}

InUniformExpressionMatcher::InUniformExpressionMatcher() : ExpressionMatcher(ExpressionClass::BOUND_OPERATOR) {
	vector<ExpressionType> types;
	types.emplace_back(ExpressionType::COMPARE_IN);
	types.emplace_back(ExpressionType::COMPARE_NOT_IN);
	expr_type = make_uniq<ManyExpressionTypeMatcher>(types);
}

bool InUniformExpressionMatcher::Match(Expression &expr_p, vector<reference<Expression>> &bindings) {
	if (!ExpressionMatcher::Match(expr_p, bindings)) {
		return false;
	}
	auto &expr = expr_p.Cast<BoundOperatorExpression>();

	auto &entries = expr.GetChildrenMutable();
	if (entries.size() < 2) {
		return false;
	}

	if (!probe_matcher->Match(*entries[0], bindings)) {
		return false;
	}

	for (idx_t i = 1; i < entries.size(); ++i) {
		if (!child_matcher->Match(*entries[i], bindings)) {
			return false;
		}
	}

	return true;
}

bool ConjunctionExpressionMatcher::Match(Expression &expr_p, vector<reference<Expression>> &bindings) {
	if (!ExpressionMatcher::Match(expr_p, bindings)) {
		return false;
	}
	auto &expr = expr_p.Cast<BoundConjunctionExpression>();
	if (!SetMatcher::Match(matchers, expr.GetChildrenMutable(), bindings, policy)) {
		return false;
	}
	return true;
}

bool FunctionExpressionMatcher::Match(Expression &expr_p, vector<reference<Expression>> &bindings) {
	if (!ExpressionMatcher::Match(expr_p, bindings)) {
		return false;
	}
	auto &expr = expr_p.Cast<BoundFunctionExpression>();
	if (!FunctionMatcher::Match(function, expr.Function().GetName())) {
		return false;
	}
	if (!SetMatcher::Match(matchers, expr.GetChildrenMutable(), bindings, policy)) {
		return false;
	}
	return true;
}

bool AggregateExpressionMatcher::Match(Expression &expr_p, vector<reference<Expression>> &bindings) {
	if (!ExpressionMatcher::Match(expr_p, bindings)) {
		return false;
	}
	auto &expr = expr_p.Cast<BoundAggregateExpression>();
	if (expr.StateExportMode() == AggregateStateExportMode::STATE_EXPORT) {
		return false;
	}
	if (!FunctionMatcher::Match(function, expr.Function().GetName())) {
		return false;
	}
	// we should create matchers for these in the future
	if (expr.GetFilter() || expr.GetOrderBys() || expr.GetAggregateType() != AggregateType::NON_DISTINCT) {
		return false;
	}
	if (!SetMatcher::Match(matchers, expr.GetChildrenMutable(), bindings, policy)) {
		return false;
	}
	return true;
}

bool FoldableConstantMatcher::Match(Expression &expr, vector<reference<Expression>> &bindings) {
	// we match on ANY expression that is a scalar expression
	if (!expr.IsFoldable()) {
		return false;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_LAMBDA) {
		// a lambda has no value of its own, so it cannot be replaced by a constant - it does not stop the
		// function it belongs to from being folded though
		return false;
	}
	bindings.push_back(expr);
	return true;
}

bool StableExpressionMatcher::Match(Expression &expr, vector<reference<Expression>> &bindings) {
	// we match on ANY expression that is a stable expression
	if (expr.IsVolatile()) {
		return false;
	}
	bindings.push_back(expr);
	return true;
}

} // namespace duckdb
