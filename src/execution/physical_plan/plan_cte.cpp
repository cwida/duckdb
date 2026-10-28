#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/execution/operator/scan/physical_column_data_scan.hpp"
#include "duckdb/execution/operator/set/physical_cte.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/parallel/pipeline_broadcast_exchange.hpp"
#include "duckdb/planner/operator/logical_materialized_cte.hpp"

namespace duckdb {

static bool ContainsRecursiveCTE(const LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_RECURSIVE_CTE ||
	    op.type == LogicalOperatorType::LOGICAL_TRAMPOLINE) {
		return true;
	}
	for (auto &child : op.children) {
		if (ContainsRecursiveCTE(*child)) {
			return true;
		}
	}
	return false;
}

PhysicalOperator &PhysicalPlanGenerator::CreatePlan(LogicalMaterializedCTE &op) {
	D_ASSERT(op.children.size() == 2);

	auto cte_body_has_side_effects = op.children[0]->HasSideEffects();
	auto use_exchange = planning_recursive_cte_depth == 0 && !ContainsRecursiveCTE(*op.children[0]) &&
	                    !ContainsRecursiveCTE(*op.children[1]);

	// Create the working_table that the PhysicalCTE will use for evaluation.
	auto working_table = make_shared_ptr<ColumnDataCollection>(context, op.children[0]->types);

	// Add the ColumnDataCollection to the context of this PhysicalPlanGenerator
	recursive_cte_tables[op.table_index] = working_table;
	materialized_ctes[op.table_index] = vector<const_reference<PhysicalOperator>>();

	// Create the plan for the left side. This is the materialization.
	auto &left = CreatePlan(*op.children[0]);
	const auto cte_body_order = OrderPreservationRecursive(left);
	const auto preserve_cte_order = PreserveInsertionOrder(left);
	const auto use_batch_index = preserve_cte_order && UseBatchIndex(left);
	const auto source_order = preserve_cte_order ? cte_body_order : OrderPreservationType::NO_ORDER;
	materialized_cte_orders[op.table_index] = source_order;

	shared_ptr<PipelineBroadcastExchange> exchange;
	if (use_exchange) {
		auto completion_mode = cte_body_has_side_effects
		                           ? PipelineBroadcastExchangeCompletionMode::RUN_TO_COMPLETION
		                           : PipelineBroadcastExchangeCompletionMode::STOP_WHEN_UNCONSUMED;
		exchange = make_shared_ptr<PipelineBroadcastExchange>(context, op.children[0]->types, completion_mode,
		                                                      source_order, use_batch_index);
		materialized_cte_exchanges[op.table_index] = exchange;
	}

	// Initialize an empty vector to collect the scan operators.
	auto &right = CreatePlan(*op.children[1]);

	auto &cte = Make<PhysicalCTE>(op.ctename, op.table_index, right.types, left, right, op.estimated_cardinality);
	auto &cast_cte = cte.Cast<PhysicalCTE>();
	cast_cte.working_table = working_table;
	cast_cte.exchange = exchange;
	cast_cte.cte_scans = materialized_ctes[op.table_index];
	cast_cte.cte_body_has_side_effects = cte_body_has_side_effects;
	cast_cte.preserve_order = preserve_cte_order;
	cast_cte.use_batch_index = use_batch_index;
	cast_cte.parallel = !preserve_cte_order || use_batch_index;
	return cte;
}

} // namespace duckdb
