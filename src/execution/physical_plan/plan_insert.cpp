#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/execution/operator/helper/physical_execute.hpp"
#include "duckdb/execution/operator/persistent/physical_insert.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/execution/operator/persistent/physical_batch_insert.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/execution/operator/projection/physical_projection.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"

namespace duckdb {

OrderPreservationType PhysicalPlanGenerator::OrderPreservationRecursive(PhysicalOperator &op) {
	if (op.type == PhysicalOperatorType::EXECUTE) {
		// PhysicalExecute has no children: look into the executed plan, or an ORDER BY in it is missed.
		return OrderPreservationRecursive(op.Cast<PhysicalExecute>().plan);
	}
	if (op.IsSource()) {
		return op.SourceOrder();
	}

	idx_t child_idx = 0;
	for (auto &child : op.children) {
		// Do not take the materialization phase of physical CTEs into account
		if (op.type == PhysicalOperatorType::CTE && child_idx == 0) {
			child_idx++;
			continue;
		}
		auto child_preservation = OrderPreservationRecursive(child);
		if (child_preservation != OrderPreservationType::INSERTION_ORDER) {
			return child_preservation;
		}
		child_idx++;
	}
	return OrderPreservationType::INSERTION_ORDER;
}

bool PhysicalPlanGenerator::PreserveInsertionOrder(ClientContext &context, PhysicalOperator &plan) {
	auto preservation_type = OrderPreservationRecursive(plan);
	if (preservation_type == OrderPreservationType::FIXED_ORDER) {
		// always need to maintain preservation order
		return true;
	}
	if (preservation_type == OrderPreservationType::NO_ORDER) {
		// never need to preserve order
		return false;
	}
	// preserve insertion order - check flags
	if (!Settings::Get<PreserveInsertionOrderSetting>(context)) {
		// preserving insertion order is disabled by config
		return false;
	}
	return true;
}

bool PhysicalPlanGenerator::PreserveInsertionOrder(PhysicalOperator &plan) {
	return PreserveInsertionOrder(context, plan);
}

bool PhysicalPlanGenerator::UseBatchIndex(ClientContext &context, PhysicalOperator &plan) {
	auto &scheduler = TaskScheduler::GetScheduler(context);
	if (scheduler.NumberOfThreads() == 1) {
		// batch index usage only makes sense if we are using multiple threads
		return false;
	}
	if (!plan.AllSourcesSupportBatchIndex()) {
		// batch index is not supported
		return false;
	}
	return true;
}

bool PhysicalPlanGenerator::UseBatchIndex(PhysicalOperator &plan) {
	return UseBatchIndex(context, plan);
}

PhysicalOperator &PhysicalPlanGenerator::ResolveDefaultsProjection(LogicalInsert &op, PhysicalOperator &child) {
	if (op.column_index_map.empty()) {
		throw InternalException("No defaults to push");
	}
	// columns specified by the user, push a projection
	vector<LogicalType> types;
	vector<unique_ptr<Expression>> select_list;
	for (auto &col : op.table.GetColumns().Physical()) {
		auto storage_idx = col.StorageOid();
		auto mapped_index = op.column_index_map[col.Physical()];
		if (mapped_index == DConstants::INVALID_INDEX) {
			// push default value
			select_list.push_back(std::move(op.bound_defaults[storage_idx]));
		} else {
			// push reference
			select_list.push_back(make_uniq<BoundReferenceExpression>(col.Type(), mapped_index));
		}
		types.push_back(col.Type());
	}
	auto &proj = Make<PhysicalProjection>(std::move(types), std::move(select_list), child.estimated_cardinality);
	proj.children.push_back(child);
	return proj;
}

PhysicalOperator &DuckCatalog::PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
                                          optional_ptr<PhysicalOperator> plan) {
	D_ASSERT(plan);
	bool parallel_streaming_insert = !PhysicalPlanGenerator::PreserveInsertionOrder(context, *plan);
	bool use_batch_index = PhysicalPlanGenerator::UseBatchIndex(context, *plan);
	auto num_threads = TaskScheduler::GetScheduler(context).NumberOfThreads();
	if (op.return_chunk) {
		// not supported for RETURNING (yet?)
		parallel_streaming_insert = false;
		use_batch_index = false;
	}
	if (op.on_conflict_info.action_type != OnConflictAction::THROW) {
		// We don't support ON CONFLICT clause in batch insertion operation currently
		use_batch_index = false;
	}
	if (op.on_conflict_info.action_type == OnConflictAction::UPDATE) {
		// When we potentially need to perform updates, we have to check that row is not updated twice
		// that currently needs to be done for every chunk, which would add a huge bottleneck to parallelized insertion
		parallel_streaming_insert = false;
	}
	if (!op.column_index_map.empty()) {
		//! Deprecated: The column_index_map is only populated by older versions.
		plan = planner.ResolveDefaultsProjection(op, *plan);
	}
	if (use_batch_index && !parallel_streaming_insert) {
		auto &insert = planner.Make<PhysicalBatchInsert>(op.types, op.table.Cast<DuckTableEntry>(),
		                                                 std::move(op.bound_constraints), op.estimated_cardinality);
		insert.children.push_back(*plan);
		return insert;
	}

	auto &insert = planner.Make<PhysicalInsert>(
	    op.types, op.table.Cast<DuckTableEntry>(), std::move(op.bound_constraints), std::move(op.expressions),
	    std::move(op.on_conflict_info.set_columns), std::move(op.on_conflict_info.set_types), op.estimated_cardinality,
	    op.return_chunk, parallel_streaming_insert && num_threads > 1, op.on_conflict_info.action_type,
	    std::move(op.on_conflict_info.on_conflict_condition), std::move(op.on_conflict_info.do_update_condition),
	    std::move(op.on_conflict_info.on_conflict_filter), std::move(op.on_conflict_info.columns_to_fetch),
	    op.on_conflict_info.update_is_del_and_insert);
	insert.children.push_back(*plan);
	return insert;
}

PhysicalOperator &PhysicalPlanGenerator::CreatePlan(LogicalInsert &op) {
	optional_ptr<PhysicalOperator> plan;
	if (!op.children.empty()) {
		D_ASSERT(op.children.size() == 1);
		plan = CreatePlan(*op.children[0]);
	}
	dependencies.AddDependency(op.table);
	return op.table.catalog.PlanInsert(context, *this, op, plan);
}

} // namespace duckdb
