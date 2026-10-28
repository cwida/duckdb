#include "duckdb/optimizer/join_order/join_order_optimizer.hpp"

#include "duckdb/common/enums/join_type.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/pair.hpp"
#include "duckdb/optimizer/join_order/cardinality_estimator.hpp"
#include "duckdb/optimizer/join_order/cost_model.hpp"
#include "duckdb/optimizer/join_order/plan_enumerator.hpp"
#include "duckdb/planner/expression/list.hpp"
#include "duckdb/planner/operator/list.hpp"

namespace duckdb {

static optional_ptr<const RelationColumnStats> GetReorderableColumn(const vector<RelationStats> &relation_stats,
                                                                    const ColumnBinding &binding) {
	if (!binding.table_index.IsValid() || !binding.column_index.IsValid() ||
	    binding.table_index.index >= relation_stats.size()) {
		return nullptr;
	}
	auto &relation = relation_stats[binding.table_index.index];
	auto column_index = binding.column_index.GetIndex();
	if (column_index >= relation.columns.size()) {
		return nullptr;
	}
	return relation.columns[column_index];
}

static optional<RelationStats> CombineReorderableStats(const vector<ColumnBinding> &bindings,
                                                       const vector<RelationStats> &relation_stats,
                                                       const RelationManager &relation_manager,
                                                       const JoinPredicateModel &predicate_model,
                                                       idx_t root_cardinality) {
	RelationStats result;
	result.cardinality = root_cardinality;
	if (relation_stats.size() == 1) {
		result.row_retention = relation_stats[0].row_retention;
	}
	result.stats_initialized = true;
	for (auto &stats : relation_stats) {
		if (!stats.stats_initialized) {
			return {};
		}
		if (!result.table_name.empty()) {
			result.table_name = Identifier(result.table_name + " joined with ");
		}
		result.table_name = Identifier(result.table_name + stats.table_name);
	}

	column_binding_map_t<idx_t> equality_bounds;
	for (auto &equality_class : predicate_model.GetEqualityClasses()) {
		idx_t current_domain = NumericLimits<idx_t>::Maximum();
		bool complete = !equality_class.columns.empty();
		for (auto &binding : equality_class.columns) {
			auto column = GetReorderableColumn(relation_stats, binding);
			if (!column) {
				complete = false;
				break;
			}
			current_domain = MinValue(current_domain, column->current_domain.distinct_count);
		}
		if (!complete) {
			continue;
		}
		for (auto &binding : equality_class.columns) {
			auto entry = equality_bounds.find(binding);
			if (entry == equality_bounds.end()) {
				equality_bounds.emplace(binding, current_domain);
			} else {
				entry->second = MinValue(entry->second, current_domain);
			}
		}
	}

	for (auto &binding : bindings) {
		ColumnBinding normalized;
		if (!relation_manager.TryNormalizeBinding(binding, normalized)) {
			return {};
		}
		auto source = GetReorderableColumn(relation_stats, normalized);
		if (!source) {
			return {};
		}
		SemiAntiJoinDomainEvidence semi_anti_evidence;
		if (relation_stats.size() == 1) {
			semi_anti_evidence = source->semi_anti_join_domain_evidence;
		}
		result.columns.emplace_back(binding, source->total_domain, source->current_domain, source->name,
		                            semi_anti_evidence);
		auto &column = result.columns.back();
		auto equality_entry = equality_bounds.find(normalized);
		if (equality_entry != equality_bounds.end()) {
			column.current_domain =
			    DistinctCount(MinValue(column.current_domain.distinct_count, equality_entry->second),
			                  DistinctCountSource::CARDINALITY);
		}
		column.current_domain.distinct_count =
		    MinValue(column.current_domain.distinct_count, column.total_domain.distinct_count);
	}
	result.CapCurrentDomainsToCardinality();
	result.Verify(bindings);
	return result;
}

JoinOrderOptimizer::JoinOrderOptimizer(ClientContext &context)
    : context(context), query_graph_manager(context), depth(1) {
}

JoinOrderOptimizer JoinOrderOptimizer::CreateChildOptimizer() {
	JoinOrderOptimizer child_optimizer(context);
	child_optimizer.materialized_cte_stats = materialized_cte_stats;
	child_optimizer.delim_scan_stats = delim_scan_stats;
	child_optimizer.depth = depth + 1;
	child_optimizer.recursive_cte_indexes = recursive_cte_indexes;
	return child_optimizer;
}

unique_ptr<LogicalOperator> JoinOrderOptimizer::Optimize(unique_ptr<LogicalOperator> plan,
                                                         optional_ptr<RelationStats> stats) {
	if (depth > MAX_RECURSION_DEPTH) {
		// Very deep plans will eventually consume quite some stack space
		// Returning the current plan is always a valid choice
		return plan;
	}

	// make sure query graph manager has not extracted a relation graph already
	LogicalOperator *op = plan.get();

	// extract the relations that go into the hyper graph.
	// We optimize the children of any non-reorderable operations we come across.
	bool reorderable = query_graph_manager.Build(*this, *op);

	// get relation_stats here since the reconstruction process will move all relations.
	auto relation_stats = query_graph_manager.relation_manager.GetRelationStats();
	unique_ptr<LogicalOperator> new_logical_plan = nullptr;

	if (reorderable) {
		// query graph now has filters and relations
		auto cardinality_estimator =
		    CardinalityEstimator(query_graph_manager.set_manager, query_graph_manager.GetPredicateModel());
		auto cost_model = CostModel(query_graph_manager, cardinality_estimator);

		// Initialize a plan enumerator.
		auto plan_enumerator =
		    PlanEnumerator(query_graph_manager, cost_model, query_graph_manager.GetQueryGraphEdges());

		// Initialize the leaf/single node plans
		plan_enumerator.InitLeafPlans();
		if (plan_enumerator.SolveJoinOrder()) {
			// now reconstruct a logical plan from the query graph plan
			query_graph_manager.plans = plan_enumerator.GetPlans();
			new_logical_plan = query_graph_manager.Reconstruct(std::move(plan), relation_stats);
		} else {
			// Approximate enumeration can conservatively reject every remaining partition. The original tree is still
			// intact and is always a valid fallback.
			new_logical_plan = std::move(plan);
		}
	} else {
		new_logical_plan = std::move(plan);
		if (relation_stats.size() == 1) {
			new_logical_plan->estimated_cardinality = relation_stats.at(0).cardinality;
			new_logical_plan->has_estimated_cardinality = true;
		}
	}

	auto root_cardinality = new_logical_plan->EstimateCardinality(context);

	// Propagate up a stats object from the top of the new_logical_plan if stats exist.
	if (stats) {
		auto new_stats = query_graph_manager.relation_manager.HasCompleteStats()
		                     ? CombineReorderableStats(new_logical_plan->GetColumnBindings(), relation_stats,
		                                               query_graph_manager.relation_manager,
		                                               query_graph_manager.GetPredicateModel(), root_cardinality)
		                     : optional<RelationStats>();
		if (new_stats) {
			*stats = std::move(*new_stats);
		} else {
			*stats = RelationStats();
		}
	}

	if (new_logical_plan->type == LogicalOperatorType::LOGICAL_EXPLAIN) {
		new_logical_plan->SetEstimatedCardinality(3);
	}

	return new_logical_plan;
}

void JoinOrderOptimizer::AddMaterializedCTEStats(TableIndex index, RelationStats &&stats) {
	materialized_cte_stats.emplace(index, std::move(stats));
}

RelationStats JoinOrderOptimizer::GetMaterializedCTEStats(TableIndex index) {
	auto it = materialized_cte_stats.find(index);
	if (it == materialized_cte_stats.end()) {
		throw InternalException("Unable to find materialized CTE stats with index %llu", index.index);
	}
	return it->second;
}

void JoinOrderOptimizer::AddDelimScanStats(RelationStats &stats) {
	delim_scan_stats = &stats;
}

RelationStats JoinOrderOptimizer::GetDelimScanStats() {
	if (!delim_scan_stats) {
		throw InternalException("Unable to find delim scan stats!");
	}
	return *delim_scan_stats;
}

} // namespace duckdb
