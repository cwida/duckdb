#include "duckdb/optimizer/rule/not_conjunction_simplification.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/common/enums/expression_type.hpp"

namespace duckdb {

NotConjunctionSimplificationRule::NotConjunctionSimplificationRule(ExpressionRewriter &rewriter) : Rule(rewriter) {
	root = make_uniq<NotExpressionMatcher>();
}

unique_ptr<Expression> NotConjunctionSimplificationRule::Apply(LogicalOperator &op,
                                                               vector<reference<Expression>> &bindings,
                                                               bool &changes_made, bool is_root) {
	auto &not_expr = bindings[0].get().Cast<BoundOperatorExpression>();
	D_ASSERT(not_expr.GetExpressionType() == ExpressionType::OPERATOR_NOT);
	D_ASSERT(not_expr.GetChildren().size() == 1);

	auto &child = not_expr.GetChildrenMutable()[0];
	if (child->GetExpressionClass() != ExpressionClass::BOUND_CONJUNCTION) {
		return nullptr;
	}

	auto &conjunction = child->Cast<BoundConjunctionExpression>();
	D_ASSERT(conjunction.GetExpressionType() == ExpressionType::CONJUNCTION_AND ||
	         conjunction.GetExpressionType() == ExpressionType::CONJUNCTION_OR);
	if (conjunction.IsVolatile() || conjunction.CanThrow()) {
		return nullptr;
	}
	auto negated_type = conjunction.GetExpressionType() == ExpressionType::CONJUNCTION_AND
	                        ? ExpressionType::CONJUNCTION_OR
	                        : ExpressionType::CONJUNCTION_AND;

	auto result = make_uniq<BoundConjunctionExpression>(negated_type);
	result->GetChildrenMutable().reserve(conjunction.GetChildren().size());
	for (auto &conj_child : conjunction.GetChildrenMutable()) {
		auto negated_child = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_NOT, LogicalType::BOOLEAN);
		negated_child->GetChildrenMutable().push_back(std::move(conj_child));
		result->GetChildrenMutable().push_back(std::move(negated_child));
	}

	changes_made = true;
	return std::move(result);
}

} // namespace duckdb
