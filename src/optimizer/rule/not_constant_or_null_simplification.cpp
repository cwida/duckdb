#include "duckdb/optimizer/rule/not_constant_or_null_simplification.hpp"

#include "duckdb/function/scalar/generic_common.hpp"
#include "duckdb/optimizer/expression_rewriter.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"

namespace duckdb {

NotConstantOrNullSimplificationRule::NotConstantOrNullSimplificationRule(ExpressionRewriter &rewriter)
    : Rule(rewriter) {
	auto op = make_uniq<ExpressionMatcher>(ExpressionClass::BOUND_OPERATOR);
	op->expr_type = make_uniq<SpecificExpressionTypeMatcher>(ExpressionType::OPERATOR_NOT);
	root = std::move(op);
}

unique_ptr<Expression> NotConstantOrNullSimplificationRule::Apply(LogicalOperator &op,
                                                                  vector<reference<Expression>> &bindings,
                                                                  bool &changes_made, bool is_root) {
	auto &not_expr = bindings[0].get().Cast<BoundOperatorExpression>();
	D_ASSERT(not_expr.GetExpressionType() == ExpressionType::OPERATOR_NOT);
	D_ASSERT(not_expr.GetChildren().size() == 1);

	auto &child = not_expr.GetChildrenMutable()[0];

	// NOT(constant_or_null(v, e)) => constant_or_null(!v, e)
	// the NULL check on e is preserved, only the constant is negated
	if (child->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION ||
	    child->GetReturnType().id() != LogicalTypeId::BOOLEAN) {
		return nullptr;
	}

	auto &func = child->Cast<BoundFunctionExpression>();
	optional<bool> value;
	if (ConstantOrNull::IsConstantOrNull(func, Value::BOOLEAN(true))) {
		value = true;
	} else if (ConstantOrNull::IsConstantOrNull(func, Value::BOOLEAN(false))) {
		value = false;
	} else {
		return nullptr;
	}

	// the first child is the constant value; the remaining children carry the NULL check
	auto &func_children = func.GetChildrenMutable();
	D_ASSERT(func_children.size() >= 2);
	vector<unique_ptr<Expression>> children;
	children.reserve(func_children.size() - 1);
	for (idx_t child_idx = 1; child_idx < func_children.size(); ++child_idx) {
		children.push_back(std::move(func_children[child_idx]));
	}

	changes_made = true;
	return rewriter.ConstantOrNull(GetContext(), std::move(children), Value::BOOLEAN(!value.value()));
}

} // namespace duckdb
