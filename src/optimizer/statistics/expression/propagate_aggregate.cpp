#include "duckdb/optimizer/statistics_propagator.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"

namespace duckdb {

unique_ptr<BaseStatistics> StatisticsPropagator::PropagateExpression(BoundAggregateExpression &aggr,
                                                                     unique_ptr<Expression> &expr_ptr) {
	if (aggr.GetFilter()) {
		// a filter that is true for every row is no filter
		auto &filter = aggr.GetFilterMutable();
		auto filter_stats = PropagateExpression(filter);
		if (filter_stats && filter_stats->GetType().id() == LogicalTypeId::BOOLEAN && !filter_stats->CanHaveNull() &&
		    filter_stats->GetStatsType() == StatisticsType::NUMERIC_STATS && NumericStats::HasMinMax(*filter_stats) &&
		    BooleanValue::Get(NumericStats::Min(*filter_stats))) {
			filter.reset();
			// the columns that only the filter read can be pruned from the scan
			removed_expressions = true;
		}
	}
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
	const idx_t child_count = aggr.GetChildren().size();
	auto result = aggr.Function().GetCallbacks().GetStatisticsCallback()(context, aggr, input);
	removed_expressions |= aggr.GetChildren().size() < child_count;
	return result;
}

} // namespace duckdb
