#include "duckdb/main/settings.hpp"
#include "duckdb/function/partition_stats.hpp"

#include "duckdb/execution/operator/aggregate/physical_hash_aggregate.hpp"
#include "duckdb/execution/operator/aggregate/physical_perfecthash_aggregate.hpp"
#include "duckdb/execution/operator/aggregate/physical_ungrouped_aggregate.hpp"
#include "duckdb/execution/operator/aggregate/physical_partitioned_aggregate.hpp"
#include "duckdb/execution/operator/projection/physical_projection.hpp"
#include "duckdb/execution/operator/scan/physical_table_scan.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"

namespace duckdb {

static uint32_t RequiredBitsForValue(uint32_t n) {
	idx_t required_bits = 0;
	while (n > 0) {
		n >>= 1;
		required_bits++;
	}
	return UnsafeNumericCast<uint32_t>(required_bits);
}

template <class T>
hugeint_t GetRangeHugeint(const BaseStatistics &nstats) {
	return Hugeint::Convert(NumericStats::GetMax<T>(nstats)) - Hugeint::Convert(NumericStats::GetMin<T>(nstats));
}

static bool CanUsePartitionedAggregate(ClientContext &context, LogicalAggregate &op, PhysicalOperator &child,
                                       vector<column_t> &partition_columns) {
	if (op.grouping_sets.size() > 1 || !op.grouping_functions.empty()) {
		return false;
	}
	for (auto &expression : op.expressions) {
		auto &aggregate = expression->Cast<BoundAggregateExpression>();
		if (aggregate.IsDistinct()) {
			// distinct aggregates are not supported in partitioned hash aggregates
			return false;
		}
	}

	return PhysicalPlanGenerator::HasSingleValuePartitions(context, op.groups, child, partition_columns);
}

bool PhysicalPlanGenerator::HasSingleValuePartitions(ClientContext &context,
                                                     const vector<unique_ptr<Expression>> &partitions,
                                                     PhysicalOperator &child, vector<column_t> &partition_columns) {
	// check if the source is partitioned by the aggregate columns
	// figure out the columns we are grouping by
	for (auto &group_expr : partitions) {
		// only support bound reference here
		if (group_expr->GetExpressionType() != ExpressionType::BOUND_REF) {
			return false;
		}
		auto &ref = group_expr->Cast<BoundReferenceExpression>();
		partition_columns.push_back(ref.Index());
	}
	// traverse the children of the aggregate to find the source operator
	reference<PhysicalOperator> child_ref(child);
	while (child_ref.get().type != PhysicalOperatorType::TABLE_SCAN) {
		auto &child_op = child_ref.get();
		switch (child_op.type) {
		case PhysicalOperatorType::PROJECTION: {
			// recompute partition columns
			auto &projection = child_op.Cast<PhysicalProjection>();
			vector<column_t> new_columns;
			for (auto &partition_col : partition_columns) {
				// we only support bound reference here
				auto &expr = projection.select_list[partition_col];
				if (expr->GetExpressionType() != ExpressionType::BOUND_REF) {
					return false;
				}
				auto &ref = expr->Cast<BoundReferenceExpression>();
				new_columns.push_back(ref.Index());
			}
			// continue into child node with new columns
			partition_columns = std::move(new_columns);
			child_ref = child_op.children[0];
			break;
		}
		case PhysicalOperatorType::FILTER:
			// continue into child operators
			child_ref = child_op.children[0];
			break;
		default:
			// unsupported operator for partition pass-through
			return false;
		}
	}
	auto &table_scan = child_ref.get().Cast<PhysicalTableScan>();
	if (!table_scan.function.get_partition_info) {
		// this source does not expose partition information - skip
		return false;
	}
	// get the base columns by projecting over the projection_ids/column_ids
	vector<column_t> base_columns;
	if (!table_scan.function.projection_pushdown) {
		// Non-pushdown scans output every base column in order. Any projection above the scan already maps references
		// into that base-column space.
		base_columns = partition_columns;
	} else {
		if (!table_scan.projection_ids.empty()) {
			for (auto &partition_col : partition_columns) {
				if (partition_col >= table_scan.projection_ids.size()) {
					return false;
				}
				partition_col = table_scan.projection_ids[partition_col];
			}
		}
		for (const auto &partition_idx : partition_columns) {
			if (partition_idx >= table_scan.column_ids.size()) {
				return false;
			}
			base_columns.push_back(table_scan.column_ids[partition_idx].GetPrimaryIndex());
		}
	}
	// check if the source operator is partitioned by the grouping columns
	TableFunctionPartitionInput input(table_scan.bind_data.get(), base_columns);
	auto partition_info = table_scan.function.get_partition_info(context, input);
	if (partition_info != TablePartitionInfo::SINGLE_VALUE_PARTITIONS) {
		// we only support single-value partitions currently
		return false;
	}
	// we have single value partitions!
	return true;
}

static bool CanUsePerfectHashAggregate(ClientContext &context, LogicalAggregate &op, vector<idx_t> &bits_per_group) {
	if (op.grouping_sets.size() > 1 || !op.grouping_functions.empty()) {
		return false;
	}
	idx_t perfect_hash_bits = 0;
	for (idx_t group_idx = 0; group_idx < op.groups.size(); group_idx++) {
		auto &group = op.groups[group_idx];
		auto &stats = op.group_stats[group_idx];

		switch (group->GetReturnType().InternalType()) {
		case PhysicalType::INT8:
		case PhysicalType::INT16:
		case PhysicalType::INT32:
		case PhysicalType::INT64:
		case PhysicalType::UINT8:
		case PhysicalType::UINT16:
		case PhysicalType::UINT32:
		case PhysicalType::UINT64:
			break;
		default:
			// we only support simple integer types for perfect hashing
			return false;
		}
		// check if the group has stats available
		auto &group_type = group->GetReturnType();
		if (!stats) {
			// no stats, but we might still be able to use perfect hashing if the type is small enough
			// for small types we can just set the stats to [type_min, type_max]
			switch (group_type.InternalType()) {
			case PhysicalType::INT8:
			case PhysicalType::INT16:
			case PhysicalType::UINT8:
			case PhysicalType::UINT16:
				break;
			default:
				// type is too large and there are no stats: skip perfect hashing
				return false;
			}
			// construct stats with the min and max value of the type
			stats = NumericStats::CreateUnknown(group_type).ToUnique();
			NumericStats::SetMin(*stats, Value::MinimumValue(group_type));
			NumericStats::SetMax(*stats, Value::MaximumValue(group_type));
		}
		auto &nstats = *stats;

		if (!NumericStats::HasMinMax(nstats)) {
			return false;
		}

		if (NumericStats::Max(*stats) < NumericStats::Min(*stats)) {
			// May result in underflow
			return false;
		}

		// we have a min and a max value for the stats: use that to figure out how many bits we have
		// we add two here, one for the NULL value, and one to make the computation one-indexed
		// (e.g. if min and max are the same, we still need one entry in total)
		hugeint_t range_h;
		switch (group_type.InternalType()) {
		case PhysicalType::INT8:
			range_h = GetRangeHugeint<int8_t>(nstats);
			break;
		case PhysicalType::INT16:
			range_h = GetRangeHugeint<int16_t>(nstats);
			break;
		case PhysicalType::INT32:
			range_h = GetRangeHugeint<int32_t>(nstats);
			break;
		case PhysicalType::INT64:
			range_h = GetRangeHugeint<int64_t>(nstats);
			break;
		case PhysicalType::UINT8:
			range_h = GetRangeHugeint<uint8_t>(nstats);
			break;
		case PhysicalType::UINT16:
			range_h = GetRangeHugeint<uint16_t>(nstats);
			break;
		case PhysicalType::UINT32:
			range_h = GetRangeHugeint<uint32_t>(nstats);
			break;
		case PhysicalType::UINT64:
			range_h = GetRangeHugeint<uint64_t>(nstats);
			break;
		default:
			throw InternalException("Unsupported type for perfect hash (should be caught before)");
		}

		uint64_t range;
		if (!Hugeint::TryCast(range_h, range)) {
			return false;
		}

		// bail out on any range bigger than 2^32
		if (range >= NumericLimits<int32_t>::Maximum()) {
			return false;
		}

		range += 2;
		// figure out how many bits we need
		idx_t required_bits = RequiredBitsForValue(UnsafeNumericCast<uint32_t>(range));
		bits_per_group.push_back(required_bits);
		perfect_hash_bits += required_bits;
		// check if we have exceeded the bits for the hash
		if (perfect_hash_bits > Settings::Get<PerfectHtThresholdSetting>(context)) {
			// too many bits for perfect hash
			return false;
		}
	}
	for (auto &expression : op.expressions) {
		auto &aggregate = expression->Cast<BoundAggregateExpression>();
		if (aggregate.IsDistinct() || !aggregate.Function().HasStateCombineCallback()) {
			// distinct aggregates are not supported in perfect hash aggregates
			return false;
		}
	}
	return true;
}

PhysicalOperator &PhysicalPlanGenerator::CreatePlan(LogicalAggregate &op) {
	D_ASSERT(op.children.size() == 1);

	reference<PhysicalOperator> plan = CreatePlan(*op.children[0]);
	plan = ExtractAggregateExpressions(plan, op.expressions, op.groups, op.grouping_sets);

	bool can_use_simple_aggregation = true;
	for (auto &expression : op.expressions) {
		auto &aggregate = expression->Cast<BoundAggregateExpression>();
		if (!aggregate.Function().GetStateClusterUpdateCallback()) {
			// unsupported aggregate for simple aggregation: use hash aggregation
			can_use_simple_aggregation = false;
			break;
		}
	}

	// Check if all groups are valid
	if (op.group_stats.empty()) {
		op.group_stats.resize(op.groups.size());
	}
	auto group_validity = TupleDataValidityType::CANNOT_HAVE_NULL_VALUES;
	for (const auto &stats : op.group_stats) {
		if (stats && !stats->CanHaveNull()) {
			continue;
		}
		group_validity = TupleDataValidityType::CAN_HAVE_NULL_VALUES;
		break;
	}

	if (op.groups.empty() && op.grouping_sets.size() <= 1) {
		// no groups: use the dedicated ungrouped aggregate path
		auto &group_by = Make<PhysicalUngroupedAggregate>(op.types, std::move(op.expressions), op.estimated_cardinality,
		                                                  op.distinct_validity);
		group_by.children.push_back(plan);
		return group_by;
	}

	// groups! create a GROUP BY aggregator
	// use a partitioned or perfect hash aggregate if possible
	vector<column_t> partition_columns;
	vector<idx_t> required_bits;
	if (can_use_simple_aggregation && CanUsePartitionedAggregate(context, op, plan, partition_columns)) {
		auto &group_by =
		    Make<PhysicalPartitionedAggregate>(context, op.types, std::move(op.expressions), std::move(op.groups),
		                                       std::move(partition_columns), op.estimated_cardinality);
		group_by.children.push_back(plan);
		return group_by;
	}

	if (CanUsePerfectHashAggregate(context, op, required_bits)) {
		auto &group_by = Make<PhysicalPerfectHashAggregate>(context, op.types, std::move(op.expressions),
		                                                    std::move(op.groups), std::move(op.group_stats),
		                                                    std::move(required_bits), op.estimated_cardinality);
		group_by.children.push_back(plan);
		return group_by;
	}

	auto &group_by = Make<PhysicalHashAggregate>(context, op.types, std::move(op.expressions), std::move(op.groups),
	                                             std::move(op.grouping_sets), std::move(op.grouping_functions),
	                                             op.estimated_cardinality, group_validity, op.distinct_validity);
	group_by.children.push_back(plan);
	return group_by;
}

PhysicalOperator &PhysicalPlanGenerator::ExtractAggregateExpressions(PhysicalOperator &child,
                                                                     vector<unique_ptr<Expression>> &aggregates,
                                                                     vector<unique_ptr<Expression>> &groups,
                                                                     optional_ptr<vector<GroupingSet>> grouping_sets) {
	vector<unique_ptr<Expression>> expressions;
	vector<LogicalType> types;

	// bind sorted aggregates
	for (auto &aggr : aggregates) {
		auto &bound_aggr = aggr->Cast<BoundAggregateExpression>();
		if (bound_aggr.GetOrderBys()) {
			// sorted aggregate!
			FunctionBinder::BindSortedAggregate(context, bound_aggr, groups, grouping_sets);
		}
	}
	for (auto &group : groups) {
		auto ref = make_uniq<BoundReferenceExpression>(group->GetReturnType(), expressions.size());
		types.push_back(group->GetReturnType());
		expressions.push_back(std::move(group));
		group = std::move(ref);
	}
	for (auto &aggr : aggregates) {
		auto &bound_aggr = aggr->Cast<BoundAggregateExpression>();
		for (auto &child_expr : bound_aggr.GetChildrenMutable()) {
			if (bound_aggr.GetFilter() && child_expr->CanThrow()) {
				// The child expressions are evaluated for every input row in the projection below the aggregate,
				// before the FILTER is applied. Guard expressions that can throw so that rows excluded by the FILTER
				// do not raise errors: child -> CASE WHEN <filter> THEN child ELSE NULL END
				auto null_value = make_uniq<BoundConstantExpression>(Value(child_expr->GetReturnType()));
				child_expr = make_uniq<BoundCaseExpression>(bound_aggr.GetFilter()->Copy(), std::move(child_expr),
				                                            std::move(null_value));
			}
			auto ref = make_uniq<BoundReferenceExpression>(child_expr->GetReturnType(), expressions.size());
			types.push_back(child_expr->GetReturnType());
			expressions.push_back(std::move(child_expr));
			child_expr = std::move(ref);
		}
		if (bound_aggr.GetFilter()) {
			auto &filter = bound_aggr.GetFilterMutable();
			auto ref = make_uniq<BoundReferenceExpression>(filter->GetReturnType(), expressions.size());
			types.push_back(filter->GetReturnType());
			expressions.push_back(std::move(filter));
			bound_aggr.GetFilterMutable() = std::move(ref);
		}
	}
	if (expressions.empty()) {
		return child;
	}
	auto &proj = Make<PhysicalProjection>(std::move(types), std::move(expressions), child.estimated_cardinality);
	proj.children.push_back(child);
	return proj;
}

} // namespace duckdb
