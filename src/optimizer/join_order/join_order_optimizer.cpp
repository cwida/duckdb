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
	auto &op = *plan;

	// extract the relations that go into the hyper graph.
	// We optimize the children of any non-reorderable operations we come across.
	bool reorderable = query_graph_manager.Build(*this, op);

	// get relation_stats here since the reconstruction process will move all relations.
	auto relation_stats = query_graph_manager.relation_manager.GetRelationStats();
	unique_ptr<LogicalOperator> new_logical_plan = nullptr;
	RelationStats output_stats;

	if (reorderable) {
		// query graph now has filters and relations
		auto cardinality_estimator =
		    CardinalityEstimator(query_graph_manager.set_manager, query_graph_manager.GetPredicateModel());
		auto cost_model = CostModel(cardinality_estimator);

		// Initialize a plan enumerator.
		auto plan_enumerator =
		    PlanEnumerator(query_graph_manager, cost_model, query_graph_manager.GetQueryGraphEdges());

		// Initialize the leaf/single node plans
		plan_enumerator.InitLeafPlans();
		if (plan_enumerator.SolveJoinOrder()) {
			// now reconstruct a logical plan from the query graph plan
			query_graph_manager.plans = plan_enumerator.GetPlans();
			auto reconstructed = query_graph_manager.Reconstruct(std::move(plan), relation_stats);
			new_logical_plan = std::move(reconstructed.op);
			output_stats = std::move(reconstructed.stats);
		}
	}
	if (!new_logical_plan) {
		// Keep the original tree when there is nothing to reorder or enumeration cannot complete.
		new_logical_plan = std::move(plan);
		if (!reorderable && relation_stats.size() == 1) {
			new_logical_plan->estimated_cardinality = relation_stats.at(0).cardinality;
			new_logical_plan->has_estimated_cardinality = true;
		}
		if (stats && query_graph_manager.relation_manager.HasCompleteStats()) {
			auto combined = query_graph_manager.CombineRelationStats(
			    new_logical_plan->GetColumnBindings(), relation_stats, new_logical_plan->EstimateCardinality(context));
			if (combined) {
				output_stats = std::move(*combined);
			}
		}
	}

	// Propagate up a stats object from the top of the new_logical_plan if stats exist.
	if (stats) {
		*stats = std::move(output_stats);
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
