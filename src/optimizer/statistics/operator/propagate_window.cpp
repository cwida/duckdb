#include "duckdb/optimizer/statistics_propagator.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/operator/logical_window.hpp"
#include "duckdb/function/aggregate_function.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"

namespace duckdb {

unique_ptr<NodeStatistics> StatisticsPropagator::PropagateStatistics(LogicalWindow &window,
                                                                     unique_ptr<LogicalOperator> &node_ptr) {
	// first propagate to the child
	node_stats = PropagateStatistics(window.children[0]);

	// then propagate to each of the order expressions
	for (auto &window_expr : window.expressions) {
		auto &over_expr = window_expr->Cast<BoundWindowExpression>();
		for (auto &expr : over_expr.PartitionsMutable()) {
			over_expr.PartitionsStatsMutable().push_back(PropagateExpression(expr));
		}
		for (auto &bound_order : over_expr.OrderByMutable()) {
			bound_order.stats = PropagateExpression(bound_order.expression);
		}

		if (over_expr.StartExpr()) {
			over_expr.ExprStatsMutable().push_back(PropagateExpression(over_expr.StartExprMutable()));
		} else {
			over_expr.ExprStatsMutable().push_back(nullptr);
		}

		if (over_expr.EndExpr()) {
			over_expr.ExprStatsMutable().push_back(PropagateExpression(over_expr.EndExprMutable()));
		} else {
			over_expr.ExprStatsMutable().push_back(nullptr);
		}

		for (auto &bound_order : over_expr.ArgOrdersMutable()) {
			bound_order.stats = PropagateExpression(bound_order.expression);
		}

		// Aggregate window functions need their statistics callback invoked so that
		// stats-dependent aggregates such as bitstring_agg receive their required statistics.
		if (over_expr.AggregateFunction()) {
			auto &agg_children = over_expr.GetChildrenMutable();
			vector<BaseStatistics> child_stats;
			child_stats.reserve(agg_children.size());
			for (auto &child : agg_children) {
				auto stat = PropagateExpression(child);
				if (!stat) {
					child_stats.push_back(BaseStatistics::CreateUnknown(child->GetReturnType()));
				} else {
					child_stats.push_back(stat->Copy());
				}
			}
			auto &aggregate = *over_expr.AggregateFunctionMutable();
			if (aggregate.GetCallbacks().HasStatisticsCallback()) {
				AggregateStatisticsInput input(over_expr.BindInfo(), child_stats, node_stats.get());
	input.allow_plan_rewrite = !SuppressPlanFolds();
				const idx_t child_count = agg_children.size();
				// The callback takes a BoundAggregateExpression; a window aggregate has none, so
				// synthesise one over the same function and children. Callbacks may rewrite both
				// (count replaces itself with count_star and drops its children), so write both back.
				BoundAggregateExpression synthetic(aggregate, std::move(agg_children), nullptr, nullptr,
				                                   over_expr.Distinct() ? AggregateType::DISTINCT
				                                                        : AggregateType::NON_DISTINCT);
				aggregate.GetCallbacks().GetStatisticsCallback()(context, synthetic, input);
				agg_children = std::move(synthetic.GetChildrenMutable());
				aggregate = synthetic.FunctionMutable();
				removed_expressions |= agg_children.size() < child_count;
			}
		}
	}
	return std::move(node_stats);
}

} // namespace duckdb
