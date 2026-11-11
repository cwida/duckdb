#include "duckdb/optimizer/statistics_propagator.hpp"
#include "duckdb/planner/expression/bound_unnest_expression.hpp"
#include "duckdb/planner/operator/logical_unnest.hpp"
#include "duckdb/storage/statistics/array_stats.hpp"
#include "duckdb/storage/statistics/list_stats.hpp"

namespace duckdb {

static unique_ptr<BaseStatistics> GetUnnestElementStats(const BaseStatistics &input_stats,
                                                        const LogicalType &element_type) {
	optional_ptr<const BaseStatistics> element_stats;
	switch (input_stats.GetStatsType()) {
	case StatisticsType::LIST_STATS:
		element_stats = ListStats::GetChildStats(input_stats);
		break;
	case StatisticsType::ARRAY_STATS:
		element_stats = ArrayStats::GetChildStats(input_stats);
		break;
	default:
		return nullptr;
	}
	if (element_stats->GetType() != element_type) {
		return nullptr;
	}
	return element_stats->ToUnique();
}

unique_ptr<NodeStatistics> StatisticsPropagator::PropagateStatistics(LogicalUnnest &unnest,
                                                                     unique_ptr<LogicalOperator> &node_ptr) {
	PropagateStatistics(unnest.children[0]);
	if (unnest.children[0]->type == LogicalOperatorType::LOGICAL_EMPTY_RESULT) {
		ReplaceWithEmptyResult(node_ptr);
		return nullptr;
	}
	// unnest emits its input row once per unnested entry
	ResetAdditiveStatistics(unnest.children[0]->GetColumnBindings());
	// several unnests in one operator pad the shorter lists with NULL
	const bool can_pad_with_null = unnest.expressions.size() > 1;
	for (idx_t expr_idx = 0; expr_idx < unnest.expressions.size(); expr_idx++) {
		auto &expr = unnest.expressions[expr_idx];
		if (expr->GetExpressionClass() != ExpressionClass::BOUND_UNNEST) {
			PropagateExpression(expr);
			continue;
		}
		auto &unnest_expr = expr->Cast<BoundUnnestExpression>();
		auto input_stats = PropagateExpression(unnest_expr.ChildMutable());
		if (!input_stats) {
			continue;
		}
		auto element_stats = GetUnnestElementStats(*input_stats, expr->GetReturnType());
		if (!element_stats) {
			continue;
		}
		element_stats->ResetAdditiveStatistics();
		if (can_pad_with_null) {
			element_stats->Set(StatsInfo::CAN_HAVE_NULL_VALUES);
		}
		statistics_map[ColumnBinding(unnest.unnest_index, ProjectionIndex(expr_idx))] = std::move(element_stats);
	}
	return nullptr;
}

} // namespace duckdb
