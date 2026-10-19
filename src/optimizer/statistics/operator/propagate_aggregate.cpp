#include "duckdb/common/assert.hpp"
#include "duckdb/common/column_index.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/function/partition_stats.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/optimizer/statistics_propagator.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_dummy_scan.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_expression_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/statistics/node_statistics.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "duckdb/storage/statistics/string_stats.hpp"
#include "duckdb/storage/storage_index.hpp"
#include "duckdb/common/types/hugeint.hpp"

namespace duckdb {

namespace {

struct MinMaxColumnInfo {
	ColumnBinding binding;
	LogicalType input_type;
	LogicalType result_type;
};

struct ValueComparator {
	virtual ~ValueComparator() = default;
	virtual bool Compare(Value &lhs, Value &rhs) const = 0;
	virtual Value GetVal(BaseStatistics &stats) const = 0;
};

template <typename StatsType>
struct MinValueComp : public ValueComparator {
	bool Compare(Value &lhs, Value &rhs) const override {
		return lhs < rhs;
	}
	Value GetVal(BaseStatistics &stats) const override {
		return StatsType::Min(stats);
	}
};

template <typename StatsType>
struct MaxValueComp : public ValueComparator {
	bool Compare(Value &lhs, Value &rhs) const override {
		return lhs > rhs;
	}
	Value GetVal(BaseStatistics &stats) const override {
		return StatsType::Max(stats);
	}
};

template <typename StatsType>
unique_ptr<ValueComparator> GetComparator(const Identifier &fun_name) {
	if (fun_name == "min") {
		return make_uniq<MinValueComp<StatsType>>();
	}
	D_ASSERT(fun_name == "max");
	return make_uniq<MaxValueComp<StatsType>>();
}

unique_ptr<ValueComparator> GetComparator(const Identifier &fun_name, const LogicalType &type) {
	if (type == LogicalType::VARCHAR) {
		return GetComparator<StringStats>(fun_name);
	} else if (type.IsNumeric() || type.IsTemporal()) {
		return GetComparator<NumericStats>(fun_name);
	}
	return nullptr;
}

bool IsSafeMinMaxCast(const LogicalType &source, const LogicalType &target) {
	if (source == target) {
		return true;
	}
	if (!source.IsIntegral() || !target.IsIntegral()) {
		return false;
	}
	LogicalType max_type;
	return LogicalType::DefaultTryGetMaxLogicalTypeUnchecked(source, target, max_type) && max_type == target;
}

bool TryGetMinMaxColumnInfo(const Expression &expr, MinMaxColumnInfo &info) {
	if (expr.GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
		const auto &col_ref = expr.Cast<BoundColumnRefExpression>();
		info.binding = col_ref.Binding();
		info.input_type = col_ref.GetReturnType();
		info.result_type = col_ref.GetReturnType();
		return true;
	}
	if (!BoundCastExpression::IsCast(expr)) {
		return false;
	}
	const auto &cast = expr.Cast<BoundFunctionExpression>();
	const auto &cast_child = BoundCastExpression::Child(cast);
	if (cast_child.GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
		return false;
	}
	const auto &col_ref = cast_child.Cast<BoundColumnRefExpression>();
	if (!IsSafeMinMaxCast(col_ref.GetReturnType(), BoundCastExpression::TargetType(cast))) {
		return false;
	}
	info.binding = col_ref.Binding();
	info.input_type = col_ref.GetReturnType();
	info.result_type = BoundCastExpression::TargetType(cast);
	return true;
}

bool TryGetValueFromStats(const PartitionStatistics &stats, const StorageIndex &storage_index,
                          const ValueComparator &comparator, const LogicalType &result_type, Value &result) {
	if (!stats.partition_row_group) {
		return false;
	}
	auto column_stats = stats.partition_row_group->GetColumnStatistics(storage_index);
	if (!column_stats) {
		return false;
	}
	if (!stats.partition_row_group->MinMaxIsExact(storage_index) || stats.partition_row_group->HasPendingWrites()) {
		return false;
	}
	if (column_stats->GetStatsType() == StatisticsType::NUMERIC_STATS) {
		if (!NumericStats::HasMinMax(*column_stats)) {
			// TODO: This also returns if an entire row group is null. In that case, we could skip/compare null
			return false;
		}
	} else {
		D_ASSERT(column_stats->GetStatsType() == StatisticsType::STRING_STATS);
		if (!StringStats::HasMinMax(*column_stats)) {
			return false;
		}
		if (StringStats::GetMinType(*column_stats) != StringStatsType::EXACT_STATS ||
		    StringStats::GetMaxType(*column_stats) != StringStatsType::EXACT_STATS) {
			// string stats are not exact - we cannot use the value from the stats
			return false;
		}
	}
	result = comparator.GetVal(*column_stats);
	if (result.type() == result_type) {
		return true;
	}
	auto cast = result.DefaultTryCastAs(result_type);
	if (!cast) {
		return false;
	}
	result = std::move(*cast);
	return true;
}

bool GroupingSetCanIntroduceNull(const LogicalAggregate &aggr, idx_t group_idx) {
	if (aggr.grouping_sets.empty()) {
		return false;
	}
	const auto projection_idx = ProjectionIndex(group_idx);
	for (const auto &grouping_set : aggr.grouping_sets) {
		if (grouping_set.find(projection_idx) == grouping_set.end()) {
			return true;
		}
	}
	return false;
}

template <class T>
hugeint_t GroupKeyRangeHugeint(const BaseStatistics &nstats) {
	return Hugeint::Convert(NumericStats::GetMax<T>(nstats)) - Hugeint::Convert(NumericStats::GetMin<T>(nstats));
}

// Upper bound on the number of distinct values a single group key can take on.
//
// This lifts the group-key domain bound that the physical planner already derives in
// CanUsePerfectHashAggregate (src/execution/physical_plan/plan_aggregate.cpp) up into the logical
// cardinality estimate, which otherwise ignores it. Two sources, in order of preference:
//   1. min/max statistics for integer and temporal keys: the key takes at most (max - min + 1) values;
//   2. the finite domain of a few narrow types when there are no usable statistics (BOOLEAN, 1- and
//      2-byte integers, ENUM - whose dictionary size is an exact bound on its non-NULL values).
// A NULL value forms its own group, so one extra slot is added. Returns false when no finite bound can
// be derived (e.g. a wide integer key without statistics, or a VARCHAR key) - the caller then leaves
// the estimate untouched. Note: CHAR(n) is normalized to width-less VARCHAR in DuckDB, so fixed-width
// character keys produce no bound here.
bool TryGetGroupKeyDistinctUpperBound(const Expression &group, optional_ptr<BaseStatistics> stats, hugeint_t &result) {
	const auto &type = group.GetReturnType();

	// 1) min/max range for integer and temporal keys
	if (stats && (type.IsIntegral() || type.IsTemporal()) && stats->GetStatsType() == StatisticsType::NUMERIC_STATS &&
	    NumericStats::HasMinMax(*stats)) {
		bool have_range = true;
		hugeint_t range;
		switch (type.InternalType()) {
		case PhysicalType::INT8:
			range = GroupKeyRangeHugeint<int8_t>(*stats);
			break;
		case PhysicalType::INT16:
			range = GroupKeyRangeHugeint<int16_t>(*stats);
			break;
		case PhysicalType::INT32:
			range = GroupKeyRangeHugeint<int32_t>(*stats);
			break;
		case PhysicalType::INT64:
			range = GroupKeyRangeHugeint<int64_t>(*stats);
			break;
		case PhysicalType::UINT8:
			range = GroupKeyRangeHugeint<uint8_t>(*stats);
			break;
		case PhysicalType::UINT16:
			range = GroupKeyRangeHugeint<uint16_t>(*stats);
			break;
		case PhysicalType::UINT32:
			range = GroupKeyRangeHugeint<uint32_t>(*stats);
			break;
		case PhysicalType::UINT64:
			range = GroupKeyRangeHugeint<uint64_t>(*stats);
			break;
		default:
			// e.g. HUGEINT (INT128) - too wide to be a useful bound
			have_range = false;
			break;
		}
		if (have_range) {
			result = range + 1; // the range is inclusive of both endpoints
			if (stats->CanHaveNull()) {
				result += 1; // NULL forms its own group
			}
			return true;
		}
	}

	// 2) finite type domain for narrow types (no statistics required); the conservative +1 covers a NULL group
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		result = 2 + 1;
		return true;
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::UTINYINT:
		result = 256 + 1;
		return true;
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::USMALLINT:
		result = 65536 + 1;
		return true;
	case LogicalTypeId::ENUM:
		result = hugeint_t(NumericCast<int64_t>(EnumType::GetSize(type))) + 1;
		return true;
	default:
		return false;
	}
}

// Tighten a plain GROUP BY's estimated cardinality using an upper bound on the number of distinct
// group-key combinations: the saturating product over the group keys of each key's distinct-value
// upper bound (see TryGetGroupKeyDistinctUpperBound). The estimate is only ever lowered - we take the
// min of the bound, the existing estimate and the input's maximum cardinality when that is known.
void CapGroupedAggregateCardinalityByKeyDomain(LogicalAggregate &aggr, optional_ptr<NodeStatistics> input_stats) {
	if (aggr.groups.empty()) {
		return; // ungrouped aggregate: handled separately (always one row)
	}
	if (aggr.grouping_sets.size() > 1) {
		// multiple grouping sets emit a key combination per set - the simple product bound does not apply
		return;
	}
	if (aggr.group_stats.size() != aggr.groups.size()) {
		return;
	}

	// saturate the product at the maximum idx_t so it can never overflow or raise the estimate.
	// NB: hugeint_t's implicit constructor takes int64_t, so build UINT64_MAX via the {upper, lower} form -
	// passing the uint64 value directly would wrap to -1.
	const hugeint_t ceiling(0, NumericLimits<idx_t>::Maximum());
	hugeint_t domain_bound = 1;
	for (idx_t group_idx = 0; group_idx < aggr.groups.size(); group_idx++) {
		hugeint_t key_bound;
		if (!TryGetGroupKeyDistinctUpperBound(*aggr.groups[group_idx], aggr.group_stats[group_idx].get(), key_bound)) {
			// one key with an unbounded domain makes the whole product unbounded: no useful cap
			return;
		}
		if (key_bound > ceiling) {
			key_bound = ceiling;
		}
		if (!Hugeint::TryMultiply(domain_bound, key_bound, domain_bound) || domain_bound > ceiling) {
			domain_bound = ceiling;
		}
	}

	idx_t bound;
	if (!Hugeint::TryCast<idx_t>(domain_bound, bound)) {
		return;
	}
	// the result of an aggregate can never have more rows than its input: clamp with the input's maximum
	// cardinality, not its estimate - an estimate is not an upper bound (a cross product is estimated as the
	// larger of its inputs, not their product)
	if (input_stats && input_stats->has_max_cardinality) {
		bound = MinValue<idx_t>(bound, input_stats->max_cardinality);
	}
	// only ever tighten the estimate
	if (!aggr.has_estimated_cardinality || bound < aggr.estimated_cardinality) {
		aggr.SetEstimatedCardinality(bound);
	}
}

} // namespace

void StatisticsPropagator::TryExecuteAggregates(LogicalAggregate &aggr, unique_ptr<LogicalOperator> &node_ptr) {
	if (SuppressPlanFolds()) {
		// the aggregate results would be constants computed from data observations
		return;
	}
	if (!aggr.groups.empty()) {
		// not possible with groups
		return;
	}
	if (aggr.expressions.empty()) {
		return;
	}
	// check if all aggregates are COUNT(*), MIN or MAX
	vector<idx_t> count_star_idxs;
	vector<MinMaxColumnInfo> min_max_columns;
	vector<unique_ptr<ValueComparator>> comparators;

	for (idx_t i = 0; i < aggr.expressions.size(); i++) {
		auto &aggr_ref = aggr.expressions[i];
		if (aggr_ref->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
			// not an aggregate
			return;
		}
		auto &aggr_expr = aggr_ref->Cast<BoundAggregateExpression>();
		if (aggr_expr.GetFilter()) {
			// aggregate has a filter - bail
			return;
		}
		if (aggr_expr.StateExportMode() == AggregateStateExportMode::STATE_EXPORT) {
			// aggregate is in state export mode - cannot replace with a constant
			return;
		}
		auto &fun_name = aggr_expr.Function().GetName();
		if (fun_name == "min" || fun_name == "max") {
			if (aggr_expr.GetChildren().size() != 1) {
				return;
			}
			MinMaxColumnInfo column_info;
			if (!TryGetMinMaxColumnInfo(*aggr_expr.GetChildren()[0], column_info)) {
				return;
			}
			min_max_columns.push_back(column_info);
			auto comparator = GetComparator(fun_name, column_info.input_type);
			if (!comparator) {
				// Type has no min max statistics
				return;
			}
			comparators.push_back(std::move(comparator));
		} else if (fun_name == "count_star") {
			count_star_idxs.push_back(i);
		} else {
			// aggregate is not count star, min or max - bail
			return;
		}
	}

	// skip any projections
	reference<LogicalOperator> child_ref = *aggr.children[0];
	while (child_ref.get().type == LogicalOperatorType::LOGICAL_PROJECTION) {
		for (auto &column_info : min_max_columns) {
			auto &proj = child_ref.get().Cast<LogicalProjection>();
			auto &expr = proj.GetExpression(column_info.binding);
			MinMaxColumnInfo projection_info;
			if (!TryGetMinMaxColumnInfo(expr, projection_info)) {
				return;
			}
			if (!IsSafeMinMaxCast(projection_info.result_type, column_info.input_type)) {
				return;
			}
			column_info.binding = projection_info.binding;
			column_info.input_type = projection_info.input_type;
		}
		child_ref = *child_ref.get().children[0];
	}

	if (child_ref.get().type != LogicalOperatorType::LOGICAL_GET) {
		// child must be a LOGICAL_GET
		return;
	}
	auto &get = child_ref.get().Cast<LogicalGet>();
	if (!get.function.get_partition_stats) {
		// GET does not support getting the partition stats
		return;
	}
	if (get.extra_info.sample_options) {
		// only use row group statistics if we query the whole table
		return;
	}

	// we can do the rewrite! get the stats
	GetPartitionStatsInput input(get.function, get.bind_data.get());
	auto partition_stats = get.function.get_partition_stats(context, input);
	if (partition_stats.empty()) {
		// no partition stats found
		return;
	}

	vector<StorageIndex> min_max_storage_indexes(min_max_columns.size());
	for (idx_t i = 0; i < min_max_columns.size(); i++) {
		auto &binding = min_max_columns[i].binding;
		auto &column_index = get.GetColumnIndex(binding);
		if (!get.TryGetStorageIndex(column_index, min_max_storage_indexes[i])) {
			//! Can't get a storage index for this column, so it doesn't have stats we can use
			//! This happens when we're dealing with a generated column for example
			return;
		}
	}

	vector<LogicalType> types;
	vector<unique_ptr<Expression>> agg_results;
	bool need_to_scan = false;
	vector<idx_t> scan_partition_indices;
	// we can keep execute eager aggregate if all partitions could be either filtered entirely or remained entirely
	if (get.table_filters.HasFilters()) {
		map<StorageIndex, reference<TableFilter>> filter_storage_index_map;
		for (auto &entry : get.table_filters) {
			auto filter_idx = entry.GetIndex();
			auto &filter = entry.Filter();
			auto &column_index = get.GetColumnIndex(filter_idx);
			StorageIndex storage_index;
			if (!get.TryGetStorageIndex(column_index, storage_index)) {
				return;
			}
			filter_storage_index_map.emplace(storage_index, filter);
		}
		vector<PartitionStatistics> precomputed_partition_stats;
		for (idx_t partition_idx = 0; partition_idx < partition_stats.size(); partition_idx++) {
			auto &stats = partition_stats[partition_idx];
			if (!stats.partition_row_group) {
				return;
			}
			auto filter_result = FilterPropagateResult::FILTER_ALWAYS_TRUE;
			for (auto &entry : filter_storage_index_map) {
				auto &storage_index = entry.first;
				auto &filter = entry.second;
				auto prg = stats.partition_row_group;
				if (!prg) {
					return;
				}
				auto column_stats = prg->GetColumnStatistics(storage_index);
				if (!column_stats) {
					return;
				}
				if (!prg->MinMaxIsExact(storage_index) || prg->HasPendingWrites()) {
					filter_result = FilterPropagateResult::NO_PRUNING_POSSIBLE;
					break;
				}
				auto &expr_filter =
				    ExpressionFilter::GetExpressionFilter(filter.get(), "AggregateStats::CheckPartitionFilters");
				auto col_filter_result = expr_filter.CheckStatistics(context, *column_stats);
				if (col_filter_result == FilterPropagateResult::FILTER_ALWAYS_FALSE) {
					// all data in this partition is filtered out, remove this partition entirely
					filter_result = FilterPropagateResult::FILTER_ALWAYS_FALSE;
					break;
				}
				if (col_filter_result != FilterPropagateResult::FILTER_ALWAYS_TRUE) {
					filter_result = col_filter_result;
				}
			}
			switch (filter_result) {
			case FilterPropagateResult::FILTER_ALWAYS_TRUE:
				precomputed_partition_stats.push_back(std::move(stats));
				break;
			case FilterPropagateResult::FILTER_ALWAYS_FALSE:
				break;
			default:
				need_to_scan = true;
				scan_partition_indices.push_back(partition_idx);
				break;
			}
		}
		if (precomputed_partition_stats.empty()) {
			// no partitions can be pre-computed
			return;
		}
		partition_stats = std::move(precomputed_partition_stats);
	}

	if (partition_stats.empty()) {
		// no partitions can be pre-computed
		return;
	}

	if (!min_max_columns.empty()) {
		// Execute min/max aggregates on partition statistics
		for (idx_t agg_idx = 0; agg_idx < min_max_storage_indexes.size(); agg_idx++) {
			const auto &storage_index = min_max_storage_indexes[agg_idx];
			const auto &result_type = min_max_columns[agg_idx].result_type;
			auto &comparator = comparators[agg_idx];

			Value agg_result;
			if (!TryGetValueFromStats(partition_stats[0], storage_index, *comparator, result_type, agg_result)) {
				return;
			}
			for (idx_t partition_idx = 1; partition_idx < partition_stats.size(); partition_idx++) {
				Value rhs;
				if (!TryGetValueFromStats(partition_stats[partition_idx], storage_index, *comparator, result_type,
				                          rhs)) {
					return;
				}
				if (!comparator->Compare(agg_result, rhs)) {
					agg_result = rhs;
				}
			}
			types.push_back(agg_result.type());
			auto expr = make_uniq<BoundConstantExpression>(agg_result);
			agg_results.push_back(std::move(expr));
		}
	}
	if (!count_star_idxs.empty()) {
		// Execute count_star aggregates on partition statistics
		idx_t count = 0;
		for (const auto &stats : partition_stats) {
			if (stats.count_type == CountType::COUNT_APPROXIMATE) {
				// we cannot get an exact count
				return;
			}
			count += stats.count;
		}
		for (const auto count_star_idx : count_star_idxs) {
			auto count_result = make_uniq<BoundConstantExpression>(Value::BIGINT(NumericCast<int64_t>(count)));
			agg_results.emplace(agg_results.begin() + NumericCast<int64_t>(count_star_idx), std::move(count_result));
			types.insert(types.begin() + NumericCast<int64_t>(count_star_idx), LogicalType::BIGINT);
		}
	}

	if (need_to_scan) {
		// Partial precomputation combines plan-time partition statistics with an execution-time scan that
		// skips partitions by their index in the row-group list. That list can change in between
		// (concurrent appends, checkpoints), in which case a skipped partition is scanned again and its
		// rows are counted twice. Only the full precomputation (no scan) is safe.
		return;
	}
	if (need_to_scan) {
		// Partial precomputation: some partitions need scanning
		// Insert a LogicalProjection above the aggregate that combines pre-computed constants with scan results
		if (!get.function.set_partitions_to_scan) {
			// scan does not support partition filtering - bail out
			return;
		}

		// Build projection expressions that merge pre-computed values with aggregate results
		auto proj_index = optimizer.binder.GenerateTableIndex();
		vector<unique_ptr<Expression>> proj_expressions;
		for (idx_t i = 0; i < aggr.expressions.size(); i++) {
			auto &aggr_expr = aggr.expressions[i]->Cast<BoundAggregateExpression>();
			auto &fun_name = aggr_expr.Function().GetName();

			// Reference to the aggregate output column
			auto agg_col_ref = make_uniq<BoundColumnRefExpression>(
			    aggr_expr.GetReturnType(), ColumnBinding(aggr.aggregate_index, ProjectionIndex(i)));

			if (fun_name == "count_star") {
				// pre_count + count_star_from_scan
				auto &pre_count_expr = agg_results[i];
				auto add_expr = optimizer.BindScalarFunction("+", pre_count_expr->Copy(), std::move(agg_col_ref));
				add_expr->SetAlias(aggr.expressions[i]->GetAlias());
				proj_expressions.push_back(std::move(add_expr));
			} else if (fun_name == "min" || fun_name == "max") {
				// For min: COALESCE(least(pre_min, agg_min), pre_min)
				// For max: COALESCE(greatest(pre_max, agg_max), pre_max)
				auto &pre_val_expr = agg_results[i];
				Identifier merge_func((fun_name == "min") ? "least" : "greatest");
				auto merged = optimizer.BindScalarFunction(merge_func, pre_val_expr->Copy(), std::move(agg_col_ref));
				auto coalesce =
				    make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_COALESCE, aggr_expr.GetReturnType());
				coalesce->GetChildrenMutable().push_back(std::move(merged));
				coalesce->GetChildrenMutable().push_back(pre_val_expr->Copy());
				coalesce->SetAlias(aggr.expressions[i]->GetAlias());
				proj_expressions.push_back(std::move(coalesce));
			}
		}

		// Tell the scan to only scan partitions whose aggregates were NOT pre-computed
		get.SetPartitionsToScan(std::move(scan_partition_indices));

		// Create LogicalProjection above the aggregate
		auto projection = make_uniq<LogicalProjection>(proj_index, std::move(proj_expressions));
		projection->children.push_back(std::move(node_ptr));

		ColumnBindingReplacer replacer;
		for (idx_t i = 0; i < aggr.expressions.size(); i++) {
			auto old_binding = ColumnBinding(aggr.aggregate_index, ProjectionIndex(i));
			auto new_binding = ColumnBinding(proj_index, ProjectionIndex(i));
			replacer.replacement_bindings.emplace_back(old_binding, new_binding);
		}

		replacer.stop_operator = projection.get();
		node_ptr = std::move(projection);
		replacer.VisitOperator(*root);
		return;
	}

	// Set column names
	for (idx_t expr_idx = 0; expr_idx < agg_results.size(); expr_idx++) {
		agg_results[expr_idx]->SetAlias(aggr.expressions[expr_idx]->GetAlias());
	}

	vector<vector<unique_ptr<Expression>>> expressions;
	expressions.push_back(std::move(agg_results));
	auto expression_get =
	    make_uniq<LogicalExpressionGet>(aggr.aggregate_index, std::move(types), std::move(expressions));
	expression_get->children.push_back(make_uniq<LogicalDummyScan>(aggr.group_index));
	node_ptr = std::move(expression_get);
}

unique_ptr<NodeStatistics> StatisticsPropagator::PropagateStatistics(LogicalAggregate &aggr,
                                                                     unique_ptr<LogicalOperator> &node_ptr) {
	// first propagate statistics in the child node
	node_stats = PropagateStatistics(aggr.children[0]);

	// handle the groups: simply propagate statistics and assign the stats to the group binding
	aggr.group_stats.resize(aggr.groups.size());
	// with multiple grouping sets a key appearing in several sets is emitted once per set
	const bool group_rows_can_repeat = aggr.grouping_sets.size() > 1;
	for (idx_t group_idx = 0; group_idx < aggr.groups.size(); group_idx++) {
		auto stats = PropagateExpression(aggr.groups[group_idx]);
		if (stats && GroupingSetCanIntroduceNull(aggr, group_idx)) {
			stats->Set(StatsInfo::CAN_HAVE_NULL_VALUES);
		}
		if (stats && group_rows_can_repeat) {
			stats->ResetAdditiveStatistics();
		}
		aggr.group_stats[group_idx] = stats ? stats->ToUnique() : nullptr;
		if (!stats) {
			continue;
		}
		ColumnBinding group_binding(aggr.group_index, ProjectionIndex(group_idx));
		statistics_map[group_binding] = std::move(stats);
	}

	// propagate statistics in the aggregates
	for (idx_t aggregate_idx = 0; aggregate_idx < aggr.expressions.size(); aggregate_idx++) {
		auto &expr = aggr.expressions[aggregate_idx];

		auto stats = PropagateExpression(expr);
		if (!stats) {
			continue;
		}
		ColumnBinding aggregate_binding(aggr.aggregate_index, ProjectionIndex(aggregate_idx));
		statistics_map[aggregate_binding] = std::move(stats);
	}

	// check whether all inputs to the aggregate functions are valid
	TupleDataValidityType distinct_validity = TupleDataValidityType::CANNOT_HAVE_NULL_VALUES;
	for (const auto &aggr_ref : aggr.expressions) {
		if (distinct_validity == TupleDataValidityType::CAN_HAVE_NULL_VALUES) {
			break;
		}
		if (aggr_ref->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
			// Bail if it's not a bound aggregate
			distinct_validity = TupleDataValidityType::CAN_HAVE_NULL_VALUES;
			break;
		}
		auto &aggr_expr = aggr_ref->Cast<BoundAggregateExpression>();
		for (const auto &child : aggr_expr.GetChildren()) {
			if (child->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
				// Bail if bound aggregate child is not a colref
				distinct_validity = TupleDataValidityType::CAN_HAVE_NULL_VALUES;
				break;
			}
			const auto &col_ref = child->Cast<BoundColumnRefExpression>();
			auto it = statistics_map.find(col_ref.Binding());
			if (it == statistics_map.end() || !it->second || it->second->CanHaveNull()) {
				// Bail if no stats or if there can be a NULL
				distinct_validity = TupleDataValidityType::CAN_HAVE_NULL_VALUES;
				break;
			}
		}
	}
	aggr.distinct_validity = distinct_validity;

	const auto is_ungrouped = aggr.groups.empty();
	// after we propagate statistics - try to directly execute aggregates using statistics
	TryExecuteAggregates(aggr, node_ptr);

	if (is_ungrouped) {
		// Ungrouped aggregate always returns exactly one row, even over empty input
		// node_stats holds the input cardinality: do not leak it to the parent operator
		node_stats.reset();
		return make_uniq<NodeStatistics>(1U, 1U);
	}
	// the max cardinality of an aggregate is the max cardinality of the input (i.e. when every row is a unique
	// group)
	// The join-order optimizer bounds a group key's distinct count by its table's min/max only for keys that are
	// a direct base-table column reference; a key that is an expression or arrives through another operator falls
	// back to an input-proportional guess. Now that the propagated group statistics are available, tighten the
	// estimate by an upper bound on the number of distinct group-key combinations - the same group-key domain the
	// physical perfect-hash-aggregate path derives from min/max and type.
	CapGroupedAggregateCardinalityByKeyDomain(aggr, node_stats.get());
	return std::move(node_stats);
}

} // namespace duckdb
