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
#include "duckdb/common/enums/filter_propagate_result.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include <cstdint>
#include <utility>

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

enum class PartitionAction : uint8_t { PRECOMPUTE, SCAN, PRUNE };

struct AggregateValues {
	vector<Value> min_max_values;
	idx_t count = 0;
};

struct PartitionAggregateEligibility {
	LogicalGet *get = nullptr;
	vector<idx_t> count_star_idxs;
	vector<idx_t> min_max_expression_idxs;
	vector<MinMaxColumnInfo> min_max_columns;
	vector<unique_ptr<ValueComparator>> comparators;
	vector<StorageIndex> min_max_storage_indexes;
	map<StorageIndex, reference<TableFilter>> filters;
};

PartitionAction ClassifyPartition(ClientContext &context, const PartitionStatistics &stats,
                                  const PartitionAggregateEligibility &eligibility, AggregateValues &result) {
	auto &row_group = stats.partition_row_group;
	if (!eligibility.filters.empty() && !row_group) {
		return PartitionAction::SCAN;
	}
	bool partial_match = false;
	for (auto &[storage_index, filter] : eligibility.filters) {
		auto column_stats = row_group->GetColumnStatistics(storage_index);
		if (!column_stats || !row_group->MinMaxIsExact(storage_index) || row_group->HasPendingWrites()) {
			partial_match = true;
			continue;
		}
		auto &expr_filter = ExpressionFilter::GetExpressionFilter(filter.get(), "ClassifyPartition");
		switch (expr_filter.CheckStatistics(context, *column_stats)) {
		case FilterPropagateResult::FILTER_ALWAYS_FALSE:
			return PartitionAction::PRUNE;
		case FilterPropagateResult::FILTER_ALWAYS_TRUE:
			break;
		default:
			partial_match = true;
			break;
		}
	}
	if (partial_match) {
		return PartitionAction::SCAN;
	}
	if (!eligibility.count_star_idxs.empty()) {
		if (stats.count_type != CountType::COUNT_EXACT) {
			return PartitionAction::SCAN;
		}
		result.count = stats.count;
	}
	for (idx_t i = 0; i < eligibility.min_max_storage_indexes.size(); i++) {
		Value value;
		if (!TryGetValueFromStats(stats, eligibility.min_max_storage_indexes[i], *eligibility.comparators[i],
		                          eligibility.min_max_columns[i].result_type, value)) {
			return PartitionAction::SCAN;
		}
		result.min_max_values.push_back(std::move(value));
	}
	return PartitionAction::PRECOMPUTE;
}

bool TryGetPartitionAggregateEligibility(LogicalAggregate &aggr, PartitionAggregateEligibility &eligibility) {
	if (!aggr.groups.empty()) {
		return false;
	}
	if (aggr.expressions.empty()) {
		return false;
	}
	auto &count_star_idxs = eligibility.count_star_idxs;
	auto &min_max_columns = eligibility.min_max_columns;
	auto &comparators = eligibility.comparators;

	for (idx_t i = 0; i < aggr.expressions.size(); i++) {
		auto &aggr_ref = aggr.expressions[i];
		if (aggr_ref->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
			return false;
		}
		auto &aggr_expr = aggr_ref->Cast<BoundAggregateExpression>();
		if (aggr_expr.GetFilter()) {
			return false;
		}
		if (aggr_expr.StateExportMode() == AggregateStateExportMode::STATE_EXPORT) {
			return false;
		}
		auto &fun_name = aggr_expr.Function().GetName();
		if (fun_name == "min" || fun_name == "max") {
			if (aggr_expr.GetChildren().size() != 1) {
				return false;
			}
			MinMaxColumnInfo column_info;
			if (!TryGetMinMaxColumnInfo(*aggr_expr.GetChildren()[0], column_info)) {
				return false;
			}
			min_max_columns.push_back(column_info);
			eligibility.min_max_expression_idxs.push_back(i);
			auto comparator = GetComparator(fun_name, column_info.input_type);
			if (!comparator) {
				return false;
			}
			comparators.push_back(std::move(comparator));
		} else if (fun_name == "count_star") {
			count_star_idxs.push_back(i);
		} else {
			return false;
		}
	}

	reference<LogicalOperator> child_ref = *aggr.children[0];
	while (child_ref.get().type == LogicalOperatorType::LOGICAL_PROJECTION) {
		for (auto &column_info : min_max_columns) {
			auto &proj = child_ref.get().Cast<LogicalProjection>();
			auto &expr = proj.GetExpression(column_info.binding);
			MinMaxColumnInfo projection_info;
			if (!TryGetMinMaxColumnInfo(expr, projection_info)) {
				return false;
			}
			if (!IsSafeMinMaxCast(projection_info.result_type, column_info.input_type)) {
				return false;
			}
			column_info.binding = projection_info.binding;
			column_info.input_type = projection_info.input_type;
		}
		child_ref = *child_ref.get().children[0];
	}

	if (child_ref.get().type != LogicalOperatorType::LOGICAL_GET) {
		return false;
	}
	auto &get = child_ref.get().Cast<LogicalGet>();
	if (!get.function.get_partition_stats) {
		return false;
	}
	if (get.extra_info.sample_options) {
		return false;
	}

	auto &min_max_storage_indexes = eligibility.min_max_storage_indexes;
	min_max_storage_indexes.resize(min_max_columns.size());
	for (idx_t i = 0; i < min_max_columns.size(); i++) {
		auto &binding = min_max_columns[i].binding;
		auto &column_index = get.GetColumnIndex(binding);
		if (!get.TryGetStorageIndex(column_index, min_max_storage_indexes[i])) {
			return false;
		}
	}

	auto &filters = eligibility.filters;
	for (auto &entry : get.table_filters) {
		auto &column_index = get.GetColumnIndex(entry.GetIndex());
		StorageIndex storage_index;
		if (!get.TryGetStorageIndex(column_index, storage_index)) {
			return false;
		}
		filters.emplace(storage_index, entry.Filter());
	}

	eligibility.get = &get;
	return true;
}

} // namespace

void StatisticsPropagator::ReplaceWithPartialPrecompute(LogicalAggregate &aggr, LogicalGet &get,
                                                        vector<unique_ptr<Expression>> aggr_results,
                                                        vector<idx_t> scan_partition_indices,
                                                        unique_ptr<LogicalOperator> &node_ptr) {
	auto proj_index = optimizer.binder.GenerateTableIndex();
	vector<unique_ptr<Expression>> proj_expressions;
	for (idx_t i = 0; i < aggr.expressions.size(); i++) {
		auto &aggr_expr = aggr.expressions[i]->Cast<BoundAggregateExpression>();
		auto &fun_name = aggr_expr.Function().GetName();

		auto agg_col_ref = make_uniq<BoundColumnRefExpression>(aggr_expr.GetReturnType(),
		                                                       ColumnBinding(aggr.aggregate_index, ProjectionIndex(i)));

		if (fun_name == "count_star") {
			auto &pre_count_expr = aggr_results[i];
			auto add_expr = optimizer.BindScalarFunction("+", pre_count_expr->Copy(), std::move(agg_col_ref));
			add_expr->SetAlias(aggr.expressions[i]->GetAlias());
			proj_expressions.push_back(std::move(add_expr));
		} else if (fun_name == "min" || fun_name == "max") {
			auto &pre_val_expr = aggr_results[i];
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

	get.SetPartitionsToScan(std::move(scan_partition_indices));

	auto projection = make_uniq<LogicalProjection>(proj_index, std::move(proj_expressions));
	projection->children.push_back(std::move(node_ptr));

	ColumnBindingReplacer replacer;
	for (idx_t i = 0; i < aggr.expressions.size(); i++) {
		replacer.replacement_bindings.emplace_back(ColumnBinding(aggr.aggregate_index, ProjectionIndex(i)),
		                                           ColumnBinding(proj_index, ProjectionIndex(i)));
	}

	replacer.stop_operator = projection.get();
	node_ptr = std::move(projection);
	replacer.VisitOperator(*root);
}

void StatisticsPropagator::ReplaceWithFullPrecompute(LogicalAggregate &aggr,
                                                     vector<unique_ptr<Expression>> aggr_results,
                                                     vector<LogicalType> types, unique_ptr<LogicalOperator> &node_ptr) {
	for (idx_t expr_idx = 0; expr_idx < aggr_results.size(); expr_idx++) {
		aggr_results[expr_idx]->SetAlias(aggr.expressions[expr_idx]->GetAlias());
	}

	vector<vector<unique_ptr<Expression>>> expressions;
	expressions.push_back(std::move(aggr_results));
	auto expression_get =
	    make_uniq<LogicalExpressionGet>(aggr.aggregate_index, std::move(types), std::move(expressions));
	expression_get->children.push_back(make_uniq<LogicalDummyScan>(aggr.group_index));
	node_ptr = std::move(expression_get);
}

void StatisticsPropagator::TryExecuteAggregates(LogicalAggregate &aggr, unique_ptr<LogicalOperator> &node_ptr) {
	PartitionAggregateEligibility eligibility;
	if (!TryGetPartitionAggregateEligibility(aggr, eligibility)) {
		return;
	}
	auto &get = *eligibility.get;

	GetPartitionStatsInput input(get.function, get.bind_data.get());
	auto partition_stats = get.function.get_partition_stats(context, input);
	if (partition_stats.empty()) {
		return;
	}

	idx_t precompute_count = 0;
	idx_t total_count = 0;
	vector<Value> min_max_values;
	for (auto &column_info : eligibility.min_max_columns) {
		min_max_values.emplace_back(column_info.result_type);
	}
	vector<idx_t> scan_partition_indices;

	for (idx_t partition_idx = 0; partition_idx < partition_stats.size(); partition_idx++) {
		AggregateValues values;
		switch (ClassifyPartition(context, partition_stats[partition_idx], eligibility, values)) {
		case PartitionAction::PRECOMPUTE:
			precompute_count++;
			total_count += values.count;
			for (idx_t i = 0; i < min_max_values.size(); i++) {
				if (min_max_values[i].IsNull() ||
				    !eligibility.comparators[i]->Compare(min_max_values[i], values.min_max_values[i])) {
					min_max_values[i] = values.min_max_values[i];
				}
			}
			break;
		case PartitionAction::SCAN:
			scan_partition_indices.push_back(partition_idx);
			break;
		case PartitionAction::PRUNE:
			break;
		}
	}

	if (precompute_count == 0) {
		return;
	}
	const bool need_to_scan = !scan_partition_indices.empty();
	if (need_to_scan && !get.function.set_partitions_to_scan) {
		return;
	}

	vector<unique_ptr<Expression>> aggr_results(aggr.expressions.size());
	for (idx_t i = 0; i < eligibility.min_max_expression_idxs.size(); i++) {
		aggr_results[eligibility.min_max_expression_idxs[i]] = make_uniq<BoundConstantExpression>(min_max_values[i]);
	}
	for (const auto count_idx : eligibility.count_star_idxs) {
		aggr_results[count_idx] = make_uniq<BoundConstantExpression>(Value::BIGINT(NumericCast<int64_t>(total_count)));
	}

	if (need_to_scan) {
		ReplaceWithPartialPrecompute(aggr, get, std::move(aggr_results), std::move(scan_partition_indices), node_ptr);
		return;
	}

	vector<LogicalType> types;
	types.reserve(aggr_results.size());
	for (auto &expr : aggr_results) {
		types.push_back(expr->GetReturnType());
	}
	ReplaceWithFullPrecompute(aggr, std::move(aggr_results), std::move(types), node_ptr);
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
