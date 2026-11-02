#include "duckdb/optimizer/rule/contains_to_in_clause.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/optimizer/expression_rewriter.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"

namespace duckdb {

ContainsToInClauseRule::ContainsToInClauseRule(ExpressionRewriter &rewriter) : Rule(rewriter) {
	auto func = make_uniq<FunctionExpressionMatcher>();
	identifier_set_t functions = {"contains", "list_contains", "list_has", "array_contains", "array_has"};
	func->function = make_uniq<ManyFunctionMatcher>(functions);
	func->matchers.push_back(make_uniq<FoldableConstantMatcher>());
	func->matchers.push_back(make_uniq<ExpressionMatcher>());
	func->policy = SetMatcher::Policy::ORDERED;
	root = std::move(func);
}

unique_ptr<Expression> ContainsToInClauseRule::Apply(LogicalOperator &op, vector<reference<Expression>> &bindings,
                                                     bool &changes_made, bool is_root) {
	auto &expr = bindings[0].get().Cast<BoundFunctionExpression>();
	auto &list_arg = expr.GetChildren()[0];
	auto &probe_arg = expr.GetChildren()[1];

	Value list_val;
	if (!ExpressionExecutor::TryEvaluateScalar(GetContext(), *list_arg, list_val)) {
		return nullptr;
	}

	// Null list: result is always NULL regardless of the probe value.
	if (list_val.IsNull()) {
		changes_made = true;
		return make_uniq<BoundConstantExpression>(Value(LogicalType::BOOLEAN));
	}

	// For other types (i.e., string/map/struct) leave them alone.
	if (list_val.type().id() != LogicalTypeId::LIST) {
		return nullptr;
	}

	// Collect non-NULL elements from the list.
	const auto &child_type = ListType::GetChildType(list_val.type());
	vector<Value> non_null_elements;
	for (const auto &elem : ListValue::GetChildren(list_val)) {
		if (!elem.IsNull()) {
			non_null_elements.emplace_back(elem.DefaultCastAs(child_type));
		}
	}

	// No non-NULL elements: never contains any value.
	if (non_null_elements.empty()) {
		changes_made = true;
		return ExpressionRewriter::ConstantOrNull(GetContext(), probe_arg->Copy(), Value::BOOLEAN(false));
	}

	// Fully constant probe: let constant folding handle it.
	if (probe_arg->IsFoldable()) {
		return nullptr;
	}

	auto in_expr = make_uniq<BoundOperatorExpression>(ExpressionType::COMPARE_IN, LogicalType::BOOLEAN);
	in_expr->GetChildrenMutable().push_back(probe_arg->Copy());
	for (auto &v : non_null_elements) {
		in_expr->GetChildrenMutable().push_back(make_uniq<BoundConstantExpression>(std::move(v)));
	}
	changes_made = true;
	return std::move(in_expr);
}

} // namespace duckdb
