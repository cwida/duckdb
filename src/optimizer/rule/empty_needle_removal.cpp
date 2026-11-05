
#include "duckdb/optimizer/rule/empty_needle_removal.hpp"

#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/optimizer/expression_rewriter.hpp"

namespace duckdb {

static unique_ptr<Expression> TryRemoveEmptyStringConcat(ClientContext &context, BoundFunctionExpression &root) {
	if (root.GetReturnType().id() != LogicalTypeId::VARCHAR) {
		return nullptr;
	}
	auto &children = root.GetChildrenMutable();
	for (idx_t child_idx = 0; child_idx < children.size(); child_idx++) {
		if (!children[child_idx]->IsFoldable()) {
			continue;
		}
		Value value;
		if (!ExpressionExecutor::TryEvaluateScalar(context, *children[child_idx], value)) {
			continue;
		}
		if (value.IsNull() || value.type().id() != LogicalTypeId::VARCHAR || !StringValue::Get(value).empty()) {
			continue;
		}
		return Expression::PreserveReturnType(root.GetReturnType(), std::move(children[1 - child_idx]));
	}
	return nullptr;
}

EmptyNeedleRemovalRule::EmptyNeedleRemovalRule(ExpressionRewriter &rewriter) : Rule(rewriter) {
	// match on a FunctionExpression that has a foldable ConstantExpression
	auto func = make_uniq<FunctionExpressionMatcher>();
	func->matchers.push_back(make_uniq<ExpressionMatcher>());
	func->matchers.push_back(make_uniq<ExpressionMatcher>());
	func->policy = SetMatcher::Policy::SOME;

	identifier_set_t functions = {"prefix", "contains", "suffix", "||"};
	func->function = make_uniq<ManyFunctionMatcher>(functions);
	root = std::move(func);
}

unique_ptr<Expression> EmptyNeedleRemovalRule::Apply(LogicalOperator &op, vector<reference<Expression>> &bindings,
                                                     bool &changes_made, bool is_root) {
	auto &root = bindings[0].get().Cast<BoundFunctionExpression>();
	D_ASSERT(root.GetChildren().size() == 2);
	if (root.Function().GetName() == "||") {
		return TryRemoveEmptyStringConcat(GetContext(), root);
	}
	auto &prefix_expr = bindings[2].get();

	// the constant_expr is a scalar expression that we have to fold
	if (!prefix_expr.IsFoldable()) {
		return nullptr;
	}
	D_ASSERT(root.GetReturnType().id() == LogicalTypeId::BOOLEAN);

	Value prefix_value;
	if (!ExpressionExecutor::TryEvaluateScalar(GetContext(), prefix_expr, prefix_value)) {
		return nullptr;
	}

	if (prefix_value.IsNull()) {
		return make_uniq<BoundConstantExpression>(Value(LogicalType::BOOLEAN));
	}

	D_ASSERT(prefix_value.type() == prefix_expr.GetReturnType());
	if (prefix_value.type().InternalType() != PhysicalType::VARCHAR) {
		return nullptr;
	}
	auto &needle_string = StringValue::Get(prefix_value);

	// PREFIX('xyz', '') is TRUE
	// PREFIX(NULL, '') is NULL
	// so rewrite PREFIX(x, '') to TRUE_OR_NULL(x)
	if (needle_string.empty()) {
		return ExpressionRewriter::ConstantOrNull(GetContext(), std::move(root.GetChildrenMutable()[0]),
		                                          Value::BOOLEAN(true));
	}
	return nullptr;
}

NoopReplaceRemovalRule::NoopReplaceRemovalRule(ExpressionRewriter &rewriter) : Rule(rewriter) {
	auto func = make_uniq<FunctionExpressionMatcher>();
	func->matchers.push_back(make_uniq<ExpressionMatcher>());
	func->matchers.push_back(make_uniq<ExpressionMatcher>());
	func->matchers.push_back(make_uniq<ExpressionMatcher>());
	func->policy = SetMatcher::Policy::ORDERED;
	func->function = make_uniq<SpecificFunctionMatcher>("replace");
	root = std::move(func);
}

unique_ptr<Expression> NoopReplaceRemovalRule::Apply(LogicalOperator &op, vector<reference<Expression>> &bindings,
                                                     bool &changes_made, bool is_root) {
	auto &root = bindings[0].get().Cast<BoundFunctionExpression>();
	D_ASSERT(root.GetChildren().size() == 3);
	auto &needle_expr = bindings[2].get();
	auto &replacement_expr = bindings[3].get();
	if (!needle_expr.IsFoldable() || !replacement_expr.IsFoldable()) {
		return nullptr;
	}

	Value needle;
	Value replacement;
	if (!ExpressionExecutor::TryEvaluateScalar(GetContext(), needle_expr, needle) ||
	    !ExpressionExecutor::TryEvaluateScalar(GetContext(), replacement_expr, replacement)) {
		return nullptr;
	}
	if (needle.IsNull() || replacement.IsNull() || needle.type().id() != LogicalTypeId::VARCHAR ||
	    replacement.type().id() != LogicalTypeId::VARCHAR) {
		return nullptr;
	}
	if (StringValue::Get(needle) != StringValue::Get(replacement)) {
		return nullptr;
	}
	return Expression::PreserveReturnType(root.GetReturnType(), std::move(root.GetChildrenMutable()[0]));
}

} // namespace duckdb
