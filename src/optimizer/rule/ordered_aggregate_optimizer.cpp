#include "duckdb/optimizer/matcher/expression_matcher.hpp"
#include "duckdb/optimizer/aggregate_rewrite.hpp"
#include "duckdb/optimizer/expression_rewriter.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/optimizer/rule/ordered_aggregate_optimizer.hpp"

namespace duckdb {

OrderedAggregateOptimizer::OrderedAggregateOptimizer(ExpressionRewriter &rewriter) : Rule(rewriter) {
	// we match on an OR expression within a LogicalFilter node
	root = make_uniq<ExpressionMatcher>();
	root->expr_class = ExpressionClass::BOUND_AGGREGATE;
}

unique_ptr<Expression> OrderedAggregateOptimizer::Apply(ClientContext &context, BoundAggregateExpression &aggr,
                                                        vector<unique_ptr<Expression>> &groups,
                                                        optional_ptr<vector<GroupingSet>> grouping_sets,
                                                        bool &changes_made) {
	if (!aggr.GetOrderBys()) {
		// no ORDER BYs defined
		return nullptr;
	}
	if (aggr.Function().GetOrderDependent() == AggregateOrderDependent::NOT_ORDER_DEPENDENT) {
		// not an order dependent aggregate but we have an ORDER BY clause - remove it
		aggr.GetOrderBysMutable().reset();
		changes_made = true;
		return nullptr;
	}

	// Remove unnecessary ORDER BY clauses and return if nothing remains
	if (aggr.GetOrderBys()->Simplify(groups, grouping_sets)) {
		aggr.GetOrderBysMutable().reset();
		changes_made = true;
		return nullptr;
	}

	AggregateRewriteInput input(context, aggr);
	auto rewrite = TryDirectAggregateRewrite(input);
	changes_made |= rewrite != nullptr;
	return rewrite;
}

unique_ptr<Expression> OrderedAggregateOptimizer::Apply(LogicalOperator &op, vector<reference<Expression>> &bindings,
                                                        bool &changes_made, bool is_root) {
	auto &aggr = bindings[0].get().Cast<BoundAggregateExpression>();

	// only apply to LogicalAggregate nodes
	if (op.type != LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		return nullptr;
	}
	// don't rewrite state-export aggregates - the rewrite would lose the STATE_EXPORT mode
	if (aggr.StateExportMode() == AggregateStateExportMode::STATE_EXPORT) {
		return nullptr;
	}

	return Apply(rewriter.context, aggr, op.Cast<LogicalAggregate>().groups, op.Cast<LogicalAggregate>().grouping_sets,
	             changes_made);
}

} // namespace duckdb
