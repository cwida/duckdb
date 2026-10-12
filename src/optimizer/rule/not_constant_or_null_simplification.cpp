#include "duckdb/optimizer/rule/not_constant_or_null_simplification.hpp"

#include "duckdb/function/scalar/generic_functions.hpp"
#include "duckdb/optimizer/expression_rewriter.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"

namespace duckdb {

NotConstantOrNullSimplificationRule::NotConstantOrNullSimplificationRule(ExpressionRewriter &rewriter)
    : Rule(rewriter) {
	// the operand is a BOOLEAN constant_or_null(<value>, <args...>)
	auto cornull = make_uniq<FunctionExpressionMatcher>();
	cornull->type = make_uniq<SpecificTypeMatcher>(LogicalType::BOOLEAN);
	cornull->function = make_uniq<SpecificFunctionMatcher>(Identifier(ConstantOrNullFun::Name));
	cornull->policy = SetMatcher::Policy::SOME_ORDERED;
	cornull->matchers.push_back(make_uniq<ConstantExpressionMatcher>()); // the leading value argument

	auto not_matcher = make_uniq<NotExpressionMatcher>();
	not_matcher->child = std::move(cornull);
	root = std::move(not_matcher);
}

// NOT(constant_or_null(v, e)) => constant_or_null(!v, e)
// the NULL check on e is preserved, only the constant is negated
unique_ptr<Expression> NotConstantOrNullSimplificationRule::Apply(LogicalOperator &op,
                                                                  vector<reference<Expression>> &bindings,
                                                                  bool &changes_made, bool is_root) {
	auto &not_expr = bindings[0].get().Cast<BoundOperatorExpression>();
	auto &func = bindings[1].get().Cast<BoundFunctionExpression>();
	auto &value_expr = bindings[2].get().Cast<BoundConstantExpression>();
	D_ASSERT(not_expr.GetExpressionType() == ExpressionType::OPERATOR_NOT);

	auto value = value_expr.GetValue();
	if (value.IsNull()) {
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
	return rewriter.ConstantOrNull(GetContext(), std::move(children), Value::BOOLEAN(!value.GetValue<bool>()));
}

} // namespace duckdb
