#include "duckdb/execution/operator/join/perfect_hash_join_executor.hpp"
#include "duckdb/execution/operator/join/physical_cross_product.hpp"
#include "duckdb/execution/operator/join/physical_hash_join.hpp"
#include "duckdb/execution/operator/join/physical_index_join.hpp"
#include "duckdb/execution/operator/join/physical_nested_loop_join.hpp"
#include "duckdb/execution/operator/join/physical_piecewise_merge_join.hpp"
#include "duckdb/execution/operator/scan/physical_table_scan.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/function/table/table_scan.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/storage/statistics/numeric_statistics.hpp"
#include "duckdb/transaction/transaction.hpp"

namespace duckdb {

static bool CanPlanIndexJoin(Transaction &transaction, TableScanBindData *bind_data, PhysicalTableScan &scan) {
	if (!bind_data) {
		// not a table scan
		return false;
	}
	auto table = bind_data->table;
	if (transaction.storage.Find(table->storage.get())) {
		// transaction local appends: skip index join
		return false;
	}
	if (scan.table_filters && !scan.table_filters->filters.empty()) {
		// table scan filters
		return false;
	}
	return true;
}

void CheckForPerfectJoinOpt(LogicalComparisonJoin &op, PerfectHashJoinStats &join_state) {
	// we only do this optimization for inner joins
	if (op.join_type != JoinType::INNER)
		return;
	// with one condition
	if (op.conditions.size() != 1)
		return;
	// with propagated statistics
	if (op.join_stats.size() <= 0)
		return;
	// with equality condition
	for (auto &&condition : op.conditions) {
		if (condition.comparison != ExpressionType::COMPARE_EQUAL) {
			return;
		}
	}
	// with integral types
	for (auto &&join_stat : op.join_stats) {
		if (!join_stat->type.IsIntegral() || join_stat->type == LogicalTypeId::HUGEINT) {
			// perfect join not possible for no integral types or hugeint
			return;
		}
	}

	// and when the build range is smaller than the threshold
	auto stats_build = reinterpret_cast<NumericStatistics *>(op.join_stats[0].get()); // lhs stats
	auto build_range = stats_build->max - stats_build->min;                           // Join Keys Range

	// Fill join_stats for invisible join
	auto stats_probe = reinterpret_cast<NumericStatistics *>(op.join_stats[1].get()); // rhs stats
	join_state.is_build_small = true;
	join_state.probe_min = stats_probe->min;
	join_state.probe_max = stats_probe->max;
	join_state.build_min = stats_build->min;
	join_state.build_max = stats_build->max;
	join_state.estimated_cardinality = op.estimated_cardinality;
	join_state.build_range = build_range.GetValue<idx_t>(); // cast integer types into idx_t

	if (stats_build->min <= stats_probe->min && stats_probe->max <= stats_build->max) {
		join_state.is_probe_in_domain = true;
	}

	return;
}

void TransformIndexJoin(ClientContext &context, LogicalComparisonJoin &op, Index **left_index, Index **right_index,
                        PhysicalOperator *left, PhysicalOperator *right) {
	auto &transaction = Transaction::GetTransaction(context);
	// check if one of the tables has an index on column
	if (op.join_type == JoinType::INNER && op.conditions.size() == 1) {
		// check if one of the children are table scans and if they have an index in the join attribute
		// (op.condition)
		if (left->type == PhysicalOperatorType::TABLE_SCAN) {
			auto &tbl_scan = (PhysicalTableScan &)*left;
			auto tbl = dynamic_cast<TableScanBindData *>(tbl_scan.bind_data.get());
			if (CanPlanIndexJoin(transaction, tbl, tbl_scan)) {
				tbl->table->storage->info->indexes.Scan([&](Index &index) {
					if (index.unbound_expressions[0]->alias == op.conditions[0].left->alias) {
						*left_index = &index;
						return true;
					}
					return false;
				});
			}
		}
		if (right->type == PhysicalOperatorType::TABLE_SCAN) {
			auto &tbl_scan = (PhysicalTableScan &)*right;
			auto tbl = dynamic_cast<TableScanBindData *>(tbl_scan.bind_data.get());
			if (CanPlanIndexJoin(transaction, tbl, tbl_scan)) {
				tbl->table->storage->info->indexes.Scan([&](Index &index) {
					if (index.unbound_expressions[0]->alias == op.conditions[0].right->alias) {
						*right_index = &index;
						return true;
					}
					return false;
				});
			}
		}
	}
}

unique_ptr<PhysicalOperator> PhysicalPlanGenerator::CreatePlan(LogicalComparisonJoin &op) {
	// now visit the children
	D_ASSERT(op.children.size() == 2);
	idx_t lhs_cardinality = op.children[0]->EstimateCardinality(context);
	idx_t rhs_cardinality = op.children[1]->EstimateCardinality(context);
	auto left = CreatePlan(*op.children[0]);
	auto right = CreatePlan(*op.children[1]);
	D_ASSERT(left && right);

	if (op.conditions.empty()) {
		// no conditions: insert a cross product
		return make_unique<PhysicalCrossProduct>(op.types, move(left), move(right), op.estimated_cardinality);
	}

	bool has_equality = false;
	bool has_inequality = false;
	bool has_null_equal_conditions = false;
	for (auto &cond : op.conditions) {
		if (cond.comparison == ExpressionType::COMPARE_EQUAL) {
			has_equality = true;
		}
		if (cond.comparison == ExpressionType::COMPARE_NOTEQUAL ||
		    cond.comparison == ExpressionType::COMPARE_DISTINCT_FROM) {
			has_inequality = true;
		}
		if (cond.null_values_are_equal) {
			has_null_equal_conditions = true;
			D_ASSERT(cond.comparison == ExpressionType::COMPARE_EQUAL);
		}
	}
	(void)has_null_equal_conditions;

	unique_ptr<PhysicalOperator> plan;
	if (has_equality) {
		Index *left_index {}, *right_index {};
		TransformIndexJoin(context, op, &left_index, &right_index, left.get(), right.get());
		if (left_index && (context.force_index_join || rhs_cardinality < 0.01 * lhs_cardinality)) {
			auto &tbl_scan = (PhysicalTableScan &)*left;
			swap(op.conditions[0].left, op.conditions[0].right);
			return make_unique<PhysicalIndexJoin>(op, move(right), move(left), move(op.conditions), op.join_type,
			                                      op.right_projection_map, op.left_projection_map, tbl_scan.column_ids,
			                                      left_index, false, op.estimated_cardinality);
		}
		if (right_index && (context.force_index_join || lhs_cardinality < 0.01 * rhs_cardinality)) {
			auto &tbl_scan = (PhysicalTableScan &)*right;
			return make_unique<PhysicalIndexJoin>(op, move(left), move(right), move(op.conditions), op.join_type,
			                                      op.left_projection_map, op.right_projection_map, tbl_scan.column_ids,
			                                      right_index, true, op.estimated_cardinality);
		}
		// Equality join with small number of keys : possible perfect join optimization
		PerfectHashJoinStats perfect_join_stats;
		CheckForPerfectJoinOpt(op, perfect_join_stats);
		plan = make_unique<PhysicalHashJoin>(op, move(left), move(right), move(op.conditions), op.join_type,
		                                     op.left_projection_map, op.right_projection_map, move(op.delim_types),
		                                     op.estimated_cardinality, perfect_join_stats);

	} else {
		D_ASSERT(!has_null_equal_conditions); // don't support this for anything but hash joins for now
		if (op.conditions.size() == 1 && !has_inequality) {
			// range join: use piecewise merge join
			plan = make_unique<PhysicalPiecewiseMergeJoin>(op, move(left), move(right), move(op.conditions),
			                                               op.join_type, op.estimated_cardinality);
		} else {
			// inequality join: use nested loop
			plan = make_unique<PhysicalNestedLoopJoin>(op, move(left), move(right), move(op.conditions), op.join_type,
			                                           op.estimated_cardinality);
		}
	}
	return plan;
}

} // namespace duckdb
