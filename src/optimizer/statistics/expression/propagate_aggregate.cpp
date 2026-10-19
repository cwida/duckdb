#include "duckdb/optimizer/statistics_propagator.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"

namespace duckdb {

unique_ptr<BaseStatistics> StatisticsPropagator::PropagateExpression(BoundAggregateExpression &aggr,
                                                                     unique_ptr<Expression> &expr_ptr) {
	vector<BaseStatistics> stats;
	stats.reserve(aggr.GetChildren().size());
	for (auto &child : aggr.GetChildrenMutable()) {
		auto stat = PropagateExpression(child);
		if (!stat) {
			stats.push_back(BaseStatistics::CreateUnknown(child->GetReturnType()));
		} else {
			stats.push_back(stat->Copy());
		}
	}
	if (!aggr.Function().GetCallbacks().HasStatisticsCallback()) {
		return nullptr;
	}
	AggregateStatisticsInput input(aggr.BindInfo(), stats, node_stats.get());
	input.allow_plan_rewrite = !SuppressPlanFolds();
	const idx_t child_count = aggr.GetChildren().size();
	auto result = aggr.Function().GetCallbacks().GetStatisticsCallback()(context, aggr, input);
	removed_expressions |= aggr.GetChildren().size() < child_count;
	return result;
}

} // namespace duckdb
