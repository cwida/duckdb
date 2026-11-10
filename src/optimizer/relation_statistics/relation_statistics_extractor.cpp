#include "duckdb/optimizer/relation_statistics/relation_statistics_extractor.hpp"

#include "duckdb/optimizer/relation_statistics/relation_statistics_helper.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/optimizer/join_order/query_graph_manager.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

namespace duckdb {

RelationStatsExtractor::RelationStatsExtractor(ClientContext &context) : context(context) {
}

RelationStatsExtractor::RelationStatsExtractor(ClientContext &context, relation_stats_cte_callback_t cte_callback)
    : context(context), cte_callback(std::move(cte_callback)) {
}

RelationStatsExtractor::RelationStatsExtractor(ClientContext &context,
                                               shared_ptr<const JoinOrderSourceStats> source_stats)
    : context(context), inherited_source_stats(std::move(source_stats)) {
}

optional_ptr<const RelationStats> RelationStatsExtractor::Extract(LogicalOperator &op) {
	auto cache_entry = cache.find(op);
	if (cache_entry != cache.end()) {
		return cache_entry->second;
	}
	GetSourceStats(op);
	if (failed_operators.find(op) != failed_operators.end() || !active_operators.insert(op).second) {
		return nullptr;
	}

	extracted_operator_count++;
	auto result = ExtractInternal(op);
	active_operators.erase(op);
	if (!result || !result->stats_initialized || !result->MatchesBindings(op.GetColumnBindings())) {
		failed_operators.insert(op);
		return nullptr;
	}
	result->Verify(op.GetColumnBindings());
	auto inserted = cache.emplace(op, std::move(*result));
	return inserted.first->second;
}

shared_ptr<const JoinOrderSourceStats> RelationStatsExtractor::GetSourceStats(LogicalOperator &root) {
	if (inherited_source_stats) {
		return inherited_source_stats;
	}
	if (!source_stats) {
		source_stats = make_shared_ptr<JoinOrderSourceStats>(context, root);
	} else if (active_operators.empty()) {
		source_stats->AddPlan(context, root);
	}
	return source_stats;
}

idx_t RelationStatsExtractor::ExtractedOperatorCount() const {
	return extracted_operator_count;
}

static bool IsInnerJoinRegion(LogicalOperator &op) {
	optional_ptr<LogicalOperator> root = op;
	while (root->type == LogicalOperatorType::LOGICAL_FILTER) {
		root = *root->children[0];
	}
	return root->type == LogicalOperatorType::LOGICAL_CROSS_PRODUCT ||
	       (root->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN &&
	        root->Cast<LogicalComparisonJoin>().join_type == JoinType::INNER);
}

optional<RelationStats> RelationStatsExtractor::ExtractInternal(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_GET && op.children.empty()) {
		auto captured = GetSourceStats(op)->FindScanStats(op.Cast<LogicalGet>());
		D_ASSERT(captured);
		return *captured;
	}
	if (op.type == LogicalOperatorType::LOGICAL_CTE_REF) {
		return ExtractCTERef(op.Cast<LogicalCTERef>());
	}
	if (op.type == LogicalOperatorType::LOGICAL_EXPLAIN) {
		return RelationStatisticsHelper::ExtractExplainStats(op);
	}

	if (IsInnerJoinRegion(op)) {
		auto stats = ExtractJoinStats(op);
		if (stats) {
			return stats;
		}
	}

	vector<reference<const RelationStats>> child_stats;
	child_stats.reserve(op.children.size());
	for (auto &child : op.children) {
		auto stats = Extract(*child);
		if (!stats) {
			return {};
		}
		child_stats.push_back(*stats);
	}
	if (op.type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY && child_stats.size() == 1) {
		return RelationStatisticsHelper::ExtractAggregationStats(op.Cast<LogicalAggregate>(), child_stats[0], *this);
	}
	return RelationStatisticsHelper::ExtractOperatorStats(op, context, child_stats);
}

optional<RelationStats> RelationStatsExtractor::ExtractJoinStats(LogicalOperator &op) {
	QueryGraphManager graph(context);
	if (!graph.BuildForEstimation(*this, op)) {
		return {};
	}
	auto relation_stats = graph.relation_manager.GetRelationStats();
	CardinalityEstimator estimator(graph.set_manager, graph.GetPredicateModel());
	estimator.Initialize(relation_stats, graph.GetJoinOperators(), graph.relation_manager.GetColumnSources());
	unordered_set<RelationIndex> relations;
	for (idx_t i = 0; i < relation_stats.size(); i++) {
		relations.insert(RelationIndex(i));
	}
	auto &set = graph.set_manager.GetJoinRelation(relations);
	auto estimate = estimator.EstimateCardinality(set);
	if (estimate.GetDouble() >= static_cast<double>(NumericLimits<idx_t>::Maximum())) {
		return {};
	}
	return graph.CombineRelationStats(op.GetColumnBindings(), relation_stats, estimate.GetIndex());
}

optional<RelationStats> RelationStatsExtractor::ExtractCTERef(LogicalCTERef &cte_ref) {
	if (cte_ref.is_recurring || !cte_callback) {
		return {};
	}
	auto definition = cte_callback(cte_ref.cte_index);
	if (!definition) {
		return {};
	}
	if (cache.find(*definition) == cache.end()) {
		source_stats->AddPlan(context, *definition);
	}
	auto definition_stats = Extract(*definition);
	if (!definition_stats) {
		return {};
	}
	return RelationStatisticsHelper::RebindOutputStats(*definition_stats, cte_ref);
}

} // namespace duckdb
