#include "duckdb/optimizer/expression_rewriter.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/function/builtin_function_lookup.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/function/scalar/generic_functions.hpp"
#include "duckdb/function/scalar/generic_common.hpp"
#include "duckdb/optimizer/rule/contains_to_in_clause.hpp"
#include "duckdb/optimizer/rule/distinct_aggregate_optimizer.hpp"
#include "duckdb/optimizer/rule/empty_needle_removal.hpp"
#include "duckdb/optimizer/rule/equal_or_null_simplification.hpp"
#include "duckdb/optimizer/rule/in_clause_simplification.hpp"
#include "duckdb/optimizer/rule/join_dependent_filter.hpp"
#include "duckdb/optimizer/rule/list.hpp"
#include "duckdb/optimizer/rule/monotone_preimage.hpp"
#include "duckdb/optimizer/rule/predicate_factoring.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"

namespace duckdb {

const vector<unique_ptr<Rule>> &ExpressionRewriter::GetRules() {
	if (!rules.empty()) {
		return rules;
	}
	rules.push_back(make_uniq<ConstantOrderNormalizationRule>(*this));
	rules.push_back(make_uniq<ConstantFoldingRule>(*this));
	rules.push_back(make_uniq<StructExtractStructPackFoldingRule>(*this));
	rules.push_back(make_uniq<DistributivityRule>(*this));
	rules.push_back(make_uniq<ArithmeticSimplificationRule>(*this));
	rules.push_back(make_uniq<LeastGreatestSimplificationRule>(*this));
	rules.push_back(make_uniq<CaseSimplificationRule>(*this));
	rules.push_back(make_uniq<ConjunctionSimplificationRule>(*this));
	rules.push_back(make_uniq<DatePartSimplificationRule>(*this));
	rules.push_back(make_uniq<DateTruncSimplificationRule>(*this));
	rules.push_back(make_uniq<ComparisonSimplificationRule>(*this));
	rules.push_back(make_uniq<RowComparisonSimplificationRule>(*this));
	rules.push_back(make_uniq<InClauseSimplificationRule>(*this));
	rules.push_back(make_uniq<InEnumSimplificationRule>(*this));
	rules.push_back(make_uniq<EnumCompareSimplificationRule>(*this));
	rules.push_back(make_uniq<EqualOrNullSimplification>(*this));
	rules.push_back(make_uniq<MoveConstantsRule>(*this));
	rules.push_back(make_uniq<MoveUnaryMinusRule>(*this));
	rules.push_back(make_uniq<MonotonePreimageRule>(*this));
	rules.push_back(make_uniq<LikeOptimizationRule>(*this));
	rules.push_back(make_uniq<StringPrefixRule>(*this));
	rules.push_back(make_uniq<InstrPrefixRule>(*this));
	rules.push_back(make_uniq<OrderedAggregateOptimizer>(*this));
	rules.push_back(make_uniq<DistinctAggregateOptimizer>(*this));
	rules.push_back(make_uniq<DistinctWindowedOptimizer>(*this));
	rules.push_back(make_uniq<RegexOptimizationRule>(*this));
	rules.push_back(make_uniq<RegexpReplaceExtractRule>(*this));
	rules.push_back(make_uniq<EmptyNeedleRemovalRule>(*this));
	rules.push_back(make_uniq<NoopReplaceRemovalRule>(*this));
	rules.push_back(make_uniq<EnumComparisonRule>(*this));
	rules.push_back(make_uniq<JoinDependentFilterRule>(*this));
	rules.push_back(make_uniq<TimeStampComparison>(*this));
	rules.push_back(make_uniq<PredicateFactoringRule>(*this));
	rules.push_back(make_uniq<ListComprehensionRewriteRule>(*this));
	rules.push_back(make_uniq<ContainsToInClauseRule>(*this));
	rules.push_back(make_uniq<NotComparisonSimplificationRule>(*this));
	rules.push_back(make_uniq<NotConjunctionSimplificationRule>(*this));

#ifdef DEBUG
	for (auto &rule : rules) {
		// root not defined in rule
		D_ASSERT(rule->root);
	}
#endif
	return rules;
}

void ExpressionRewriter::AddRule(unique_ptr<Rule> rule) {
	D_ASSERT(rule->root);
	rules.push_back(std::move(rule));
}

struct RewriteFrame {
	reference<unique_ptr<Expression>> expr;
	bool is_root;
	bool children_visited;
};

static unique_ptr<Expression> ApplyRule(LogicalOperator &op, const vector<reference<Rule>> &rules,
                                        unique_ptr<Expression> expr, bool &changes_made, bool is_root) {
	for (auto &rule : rules) {
		vector<reference<Expression>> bindings;
		if (rule.get().root->Match(*expr, bindings)) {
			// the rule matches! try to apply it
			bool rule_made_change = false;
			auto alias = expr->GetAlias();
#ifdef D_ASSERT_IS_ENABLED
			auto return_type = expr->GetReturnType();
#endif
			auto result = rule.get().Apply(op, bindings, rule_made_change, is_root);
			if (result) {
				D_ASSERT(result->GetReturnType() != return_type ||
				         result->GetReturnType().EqualsIncludingCollation(return_type));
				changes_made = true;
				// the base node changed: the rule applied changes
				if (!alias.empty()) {
					result->SetAlias(std::move(alias));
				}
				return result;
			} else if (rule_made_change) {
				changes_made = true;
				return expr;
			}
			// else nothing changed, continue to the next rule
			continue;
		}
	}
	return expr;
}

static void CollectChildren(Expression &expr, vector<reference<unique_ptr<Expression>>> &children) {
	children.clear();
	ExpressionIterator::EnumerateChildren(expr, [&](unique_ptr<Expression> &child) { children.push_back(child); });
}

unique_ptr<Expression> ExpressionRewriter::ApplyRules(LogicalOperator &op, const vector<reference<Rule>> &rules,
                                                      unique_ptr<Expression> expr, bool &changes_made, bool is_root) {
	vector<RewriteFrame> stack;
	vector<reference<unique_ptr<Expression>>> children;
	stack.push_back({expr, is_root, false});

	while (!stack.empty()) {
		auto &frame = stack.back();
		auto &current_expr = frame.expr.get();
		D_ASSERT(current_expr);
		if (!frame.children_visited) {
			frame.children_visited = true;
			// Rewrite the subtree bottom-up so parent rules see already-simplified children.
			CollectChildren(*current_expr, children);
			for (idx_t i = children.size(); i > 0; i--) {
				stack.push_back({children[i - 1], false, false});
			}
			continue;
		}
		bool node_made_change = false;
		current_expr = ApplyRule(op, rules, std::move(current_expr), node_made_change, frame.is_root);
		if (node_made_change) {
			changes_made = true;
			frame.children_visited = false;
			continue;
		}
		stack.pop_back();
	}
	return expr;
}

unique_ptr<Expression> ExpressionRewriter::ConstantOrNull(ClientContext &context, unique_ptr<Expression> child,
                                                          Value value) {
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(child));
	return ConstantOrNull(context, std::move(children), std::move(value));
}

unique_ptr<Expression> ExpressionRewriter::ConstantOrNull(ClientContext &context,
                                                          vector<unique_ptr<Expression>> children, Value value) {
	children.insert(children.begin(), make_uniq<BoundConstantExpression>(std::move(value)));
	// constant_or_null has the type of the leading constant, also when a typed NULL child folds the call
	return BindBuiltinScalarExpression(context, ConstantOrNullFun::Name, std::move(children));
}

void ExpressionRewriter::VisitOperator(LogicalOperator &op) {
	VisitOperatorChildren(op);
	this->op = &op;

	to_apply_rules.clear();
	for (auto &rule : GetRules()) {
		to_apply_rules.push_back(*rule);
	}

	if (op.type == LogicalOperatorType::LOGICAL_FILTER) {
		// For FILTER we want to ensure we always visit the split predicates
		// Rewrites can add more predicates, so we need to loop
		auto &filter = op.Cast<LogicalFilter>();
		idx_t visited_until = 0;
		do {
			for (idx_t i = visited_until; i < filter.expressions.size(); i++) {
				VisitExpression(&filter.expressions[i]);
			}
			visited_until = filter.expressions.size();
		} while (LogicalFilter::SplitPredicates(filter.expressions));
	} else {
		VisitOperatorExpressions(op);
	}
}

void ExpressionRewriter::VisitExpression(unique_ptr<Expression> *expression) {
	bool changes_made = false;
	*expression = ExpressionRewriter::ApplyRules(*op, to_apply_rules, std::move(*expression), changes_made, true);
}

ClientContext &Rule::GetContext() const {
	return rewriter.context;
}

} // namespace duckdb
