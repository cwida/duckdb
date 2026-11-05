#include "duckdb/optimizer/rule/not_comparison_simplification.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/common/enums/expression_type.hpp"

namespace duckdb {

NotComparisonSimplificationRule::NotComparisonSimplificationRule(ExpressionRewriter &rewriter) : Rule(rewriter) {
	root = make_uniq<NotExpressionMatcher>();
}

unique_ptr<Expression> NotComparisonSimplificationRule::Apply(LogicalOperator &op,
                                                              vector<reference<Expression>> &bindings,
                                                              bool &changes_made, bool is_root) {
	auto &not_expr = bindings[0].get().Cast<BoundOperatorExpression>();
	D_ASSERT(not_expr.GetExpressionType() == ExpressionType::OPERATOR_NOT);
	D_ASSERT(not_expr.GetChildren().size() == 1);

	auto &child = not_expr.GetChildrenMutable()[0];

	if (!BoundComparisonExpression::IsComparison(*child)) {
		return nullptr;
	}

	auto &comparison = child->Cast<BoundFunctionExpression>();
	auto negated_type = NegateComparisonExpression(comparison.GetExpressionType());

	BoundComparisonExpression::SetType(comparison, negated_type);
	changes_made = true;

	return std::move(child);
}

} // namespace duckdb
