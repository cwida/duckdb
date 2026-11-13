#include "duckdb/optimizer/relation_statistics/relation_statistics_helper.hpp"

#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/operator/add.hpp"
#include "duckdb/common/operator/subtract.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/function/partition_stats.hpp"
#include "duckdb/function/table/table_scan.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_delim_get.hpp"
#include "duckdb/planner/operator/logical_column_data_get.hpp"
#include "duckdb/planner/operator/logical_dummy_scan.hpp"
#include "duckdb/planner/operator/logical_expression_get.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"

namespace duckdb {

static idx_t CapMinMaxDistinctCount(uint64_t distinct_count, idx_t base_table_cardinality) {
	if (base_table_cardinality == 0 || distinct_count == 0) {
		return 0;
	}
	auto capped_distinct_count = MinValue<idx_t>(distinct_count, base_table_cardinality);
	return capped_distinct_count == NumericLimits<idx_t>::Maximum() ? 0 : capped_distinct_count;
}

static idx_t GetMinMaxSpanDistinctCount(uint64_t span, idx_t base_table_cardinality) {
	uint64_t distinct_count;
	if (!TryAddOperator::Operation<uint64_t, uint64_t, uint64_t>(span, 1, distinct_count)) {
		return 0;
	}
	return CapMinMaxDistinctCount(distinct_count, base_table_cardinality);
}

template <class T>
static idx_t GetSignedMinMaxDistinctCount(const BaseStatistics &base_stats, idx_t base_table_cardinality) {
	auto min_value = NumericStats::Min(base_stats).GetValueUnsafe<T>();
	auto max_value = NumericStats::Max(base_stats).GetValueUnsafe<T>();
	if (max_value < min_value) {
		return 0;
	}
	hugeint_t span;
	if (!TrySubtractOperator::Operation(hugeint_t(static_cast<int64_t>(max_value)),
	                                    hugeint_t(static_cast<int64_t>(min_value)), span)) {
		return 0;
	}
	uint64_t unsigned_span;
	if (!Hugeint::TryCast(span, unsigned_span)) {
		return 0;
	}
	return GetMinMaxSpanDistinctCount(unsigned_span, base_table_cardinality);
}

template <class T>
static idx_t GetUnsignedMinMaxDistinctCount(const BaseStatistics &base_stats, idx_t base_table_cardinality) {
	auto min_value = NumericStats::Min(base_stats).GetValueUnsafe<T>();
	auto max_value = NumericStats::Max(base_stats).GetValueUnsafe<T>();
	T span;
	if (!TrySubtractOperator::Operation(max_value, min_value, span)) {
		return 0;
	}
	return GetMinMaxSpanDistinctCount(static_cast<uint64_t>(span), base_table_cardinality);
}

static idx_t GetBooleanMinMaxDistinctCount(const BaseStatistics &base_stats, idx_t base_table_cardinality) {
	auto min_value = NumericStats::Min(base_stats).GetValueUnsafe<bool>();
	auto max_value = NumericStats::Max(base_stats).GetValueUnsafe<bool>();
	return CapMinMaxDistinctCount(min_value == max_value ? idx_t(1) : idx_t(2), base_table_cardinality);
}

static idx_t GetMinMaxDistinctCount(const BaseStatistics &base_stats, idx_t base_table_cardinality) {
	if (base_table_cardinality == 0 || base_stats.GetStatsType() != StatisticsType::NUMERIC_STATS ||
	    !NumericStats::HasMinMax(base_stats)) {
		return 0;
	}

	switch (base_stats.GetType().InternalType()) {
	case PhysicalType::BOOL:
		return GetBooleanMinMaxDistinctCount(base_stats, base_table_cardinality);
	case PhysicalType::INT8:
		return GetSignedMinMaxDistinctCount<int8_t>(base_stats, base_table_cardinality);
	case PhysicalType::INT16:
		return GetSignedMinMaxDistinctCount<int16_t>(base_stats, base_table_cardinality);
	case PhysicalType::INT32:
		return GetSignedMinMaxDistinctCount<int32_t>(base_stats, base_table_cardinality);
	case PhysicalType::INT64:
		return GetSignedMinMaxDistinctCount<int64_t>(base_stats, base_table_cardinality);
	case PhysicalType::UINT8:
		return GetUnsignedMinMaxDistinctCount<uint8_t>(base_stats, base_table_cardinality);
	case PhysicalType::UINT16:
		return GetUnsignedMinMaxDistinctCount<uint16_t>(base_stats, base_table_cardinality);
	case PhysicalType::UINT32:
		return GetUnsignedMinMaxDistinctCount<uint32_t>(base_stats, base_table_cardinality);
	case PhysicalType::UINT64:
		return GetUnsignedMinMaxDistinctCount<uint64_t>(base_stats, base_table_cardinality);
	default:
		return 0;
	}
}

static DistinctCount GetDistinctCountFromStats(BaseStatistics &base_stats, idx_t base_table_cardinality) {
	auto distinct_count = base_stats.GetDistinctCount();
	if (distinct_count > 0) {
		return DistinctCount(distinct_count, DistinctCountSource::HLL);
	}
	distinct_count = GetMinMaxDistinctCount(base_stats, base_table_cardinality);
	if (distinct_count > 0) {
		return DistinctCount(distinct_count, DistinctCountSource::MIN_MAX);
	}
	return DistinctCount(0, DistinctCountSource::CARDINALITY);
}

struct FilterEstimate {
	idx_t cardinality;
	bool supported;

	void Combine(const FilterEstimate &other) {
		cardinality = MinValue(cardinality, other.cardinality);
		supported &= other.supported;
	}
};

static idx_t FilterFraction(idx_t cardinality, double fraction) {
	if (cardinality == 0) {
		return 0;
	}
	return MinValue(cardinality, MaxValue<idx_t>(1, RelationStatisticsHelper::CardinalityFromDouble(
	                                                    double(cardinality) * fraction)));
}

class IntegerRangeEstimate {
public:
	explicit IntegerRangeEstimate(const BaseStatistics &statistics) : statistics(statistics) {
	}

	bool AddBound(ExpressionType comparison, const Value &constant) {
		if (constant.IsNull() || !constant.type().EqualsIncludingCollation(statistics.GetType()) ||
		    statistics.GetStatsType() != StatisticsType::NUMERIC_STATS || !NumericStats::HasMinMax(statistics)) {
			return false;
		}
		switch (constant.type().id()) {
		case LogicalTypeId::TINYINT:
		case LogicalTypeId::SMALLINT:
		case LogicalTypeId::INTEGER:
		case LogicalTypeId::BIGINT:
		case LogicalTypeId::UTINYINT:
		case LogicalTypeId::USMALLINT:
		case LogicalTypeId::UINTEGER:
		case LogicalTypeId::UBIGINT:
			break;
		default:
			return false;
		}
		if (!has_bounds) {
			lower = NumericStats::Min(statistics).GetValue<hugeint_t>();
			upper = NumericStats::Max(statistics).GetValue<hugeint_t>();
			if (lower > upper) {
				return false;
			}
			// Widen before endpoint arithmetic: a complete 64-bit interval has 2^64 values.
			count = upper - lower + 1;
			has_bounds = true;
		}
		auto value = constant.GetValue<hugeint_t>();
		switch (comparison) {
		case ExpressionType::COMPARE_LESSTHAN:
			upper = MinValue(upper, value - 1);
			break;
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
			upper = MinValue(upper, value);
			break;
		case ExpressionType::COMPARE_GREATERTHAN:
			lower = MaxValue(lower, value + 1);
			break;
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			lower = MaxValue(lower, value);
			break;
		default:
			throw InternalException("Expected a range comparison");
		}
		return true;
	}

	optional<double> Fraction() const {
		if (!has_bounds) {
			return {};
		}
		auto matching = MaxValue(hugeint_t(0), upper - lower + 1);
		return Hugeint::Cast<double>(matching) / Hugeint::Cast<double>(count);
	}

private:
	const BaseStatistics &statistics;
	hugeint_t lower;
	hugeint_t upper;
	hugeint_t count;
	bool has_bounds = false;
};

static optional_ptr<const RelationColumnStats> FilterColumn(const Expression &expr,
                                                            const RelationColumnStatsLookup &columns,
                                                            const vector<ColumnBinding> &positional_bindings) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &ref = expr.Cast<BoundColumnRefExpression>();
		return ref.Depth() == 0 ? columns.Find(ref.Binding()) : nullptr;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_REF) {
		auto index = expr.Cast<BoundReferenceExpression>().Index();
		if (index < positional_bindings.size()) {
			return columns.Find(positional_bindings[index]);
		}
	}
	return nullptr;
}

static FilterEstimate EstimateNullCheckRows(idx_t cardinality, const BoundOperatorExpression &expr,
                                            const RelationColumnStatsLookup &columns,
                                            const vector<ColumnBinding> &positional_bindings) {
	auto &children = expr.GetChildren();
	if (children.size() != 1 || !FilterColumn(*children[0], columns, positional_bindings)) {
		return {cardinality, false};
	}
	const auto selectivity = RelationStatisticsHelper::DEFAULT_SELECTIVITY;
	return {FilterFraction(cardinality, expr.GetExpressionType() == ExpressionType::OPERATOR_IS_NULL ? selectivity
	                                                                                                 : 1 - selectivity),
	        true};
}

static FilterEstimate EstimateEqualityRows(idx_t cardinality, ExpressionType type, const RelationColumnStats &column) {
	const auto selectivity = RelationStatisticsHelper::DEFAULT_SELECTIVITY;
	bool equal = type == ExpressionType::COMPARE_EQUAL || type == ExpressionType::COMPARE_NOT_DISTINCT_FROM;
	if (cardinality == 0) {
		return {0, true};
	}
	if (column.total_domain.source == DistinctCountSource::CARDINALITY) {
		return {FilterFraction(cardinality, equal ? selectivity : 1 - selectivity), true};
	}
	auto domain = MinValue(cardinality, column.current_domain.distinct_count);
	if (domain == 0) {
		return {cardinality, false};
	}
	return {equal ? cardinality / domain + (cardinality % domain != 0)
	              : FilterFraction(cardinality, 1 - 1.0 / double(domain)),
	        true};
}

static FilterEstimate EstimateComparisonRows(idx_t cardinality, const BoundFunctionExpression &comparison,
                                             const RelationColumnStatsLookup &columns,
                                             const vector<ColumnBinding> &positional_bindings,
                                             FilterPropagateResult pruning, optional_ptr<IntegerRangeEstimate> range) {
	const auto selectivity = RelationStatisticsHelper::DEFAULT_SELECTIVITY;
	auto &children = comparison.GetChildren();
	auto type = comparison.GetExpressionType();
	auto column = FilterColumn(*children[0], columns, positional_bindings);
	optional_ptr<const Expression> value = *children[1];
	if (!column) {
		column = FilterColumn(*children[1], columns, positional_bindings);
		value = *children[0];
		type = FlipComparisonExpression(type);
	}
	if (!column || (value->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT &&
	                value->GetExpressionClass() != ExpressionClass::BOUND_PARAMETER)) {
		return {cardinality, false};
	}
	if (value->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT &&
	    value->Cast<BoundConstantExpression>().GetValue().IsNull()) {
		if (type == ExpressionType::COMPARE_NOT_DISTINCT_FROM || type == ExpressionType::COMPARE_DISTINCT_FROM) {
			return {FilterFraction(cardinality,
			                       type == ExpressionType::COMPARE_NOT_DISTINCT_FROM ? selectivity : 1 - selectivity),
			        true};
		}
		return {0, true};
	}
	if (pruning == FilterPropagateResult::FILTER_TRUE_OR_NULL && type != ExpressionType::COMPARE_DISTINCT_FROM &&
	    type != ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
		return {FilterFraction(cardinality, 1 - selectivity), true};
	}
	switch (type) {
	case ExpressionType::COMPARE_EQUAL:
	case ExpressionType::COMPARE_NOTEQUAL:
	case ExpressionType::COMPARE_DISTINCT_FROM:
	case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
		return EstimateEqualityRows(cardinality, type, *column);
	case ExpressionType::COMPARE_LESSTHAN:
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
	case ExpressionType::COMPARE_GREATERTHAN:
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		if (range && value->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT &&
		    range->AddBound(type, value->Cast<BoundConstantExpression>().GetValue())) {
			return {cardinality, true};
		}
		return {FilterFraction(cardinality, selectivity), true};
	default:
		return {cardinality, false};
	}
}

static FilterEstimate EstimateFilterRows(idx_t cardinality, const Expression &expr,
                                         const RelationColumnStatsLookup &columns,
                                         const vector<ColumnBinding> &positional_bindings,
                                         optional_ptr<ClientContext> context = nullptr,
                                         optional_ptr<const BaseStatistics> scan_statistics = nullptr,
                                         optional_ptr<IntegerRangeEstimate> range = nullptr) {
	if (ExpressionFilter::IsRootOptionalExpression(expr)) {
		return {cardinality, true};
	}
	if (expr.GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
		FilterEstimate result {cardinality, true};
		for (auto &child : expr.Cast<BoundConjunctionExpression>().GetChildren()) {
			result.Combine(
			    EstimateFilterRows(cardinality, *child, columns, positional_bindings, context, scan_statistics, range));
		}
		return result;
	}
	auto pruning = FilterPropagateResult::NO_PRUNING_POSSIBLE;
	if (scan_statistics && !expr.HasParameter()) {
		pruning = ExpressionFilter::CheckExpressionStatistics(context, expr, *scan_statistics);
		if (pruning == FilterPropagateResult::FILTER_ALWAYS_TRUE) {
			return {cardinality, true};
		}
		if (pruning == FilterPropagateResult::FILTER_ALWAYS_FALSE ||
		    pruning == FilterPropagateResult::FILTER_FALSE_OR_NULL) {
			return {0, true};
		}
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT && expr.GetReturnType() == LogicalType::BOOLEAN) {
		auto &value = expr.Cast<BoundConstantExpression>().GetValue();
		return {value.IsNull() || !BooleanValue::Get(value) ? 0 : cardinality, true};
	}
	// Preserve the existing scan-specific non-null NOT LIKE estimate.
	if (scan_statistics && expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION &&
	    expr.Cast<BoundFunctionExpression>().Function().GetName() == "!~~" && !scan_statistics->CanHaveNull()) {
		return {FilterFraction(cardinality, 1 - RelationStatisticsHelper::DEFAULT_SELECTIVITY), true};
	}
	if (expr.GetExpressionType() == ExpressionType::OPERATOR_IS_NULL ||
	    expr.GetExpressionType() == ExpressionType::OPERATOR_IS_NOT_NULL) {
		return EstimateNullCheckRows(cardinality, expr.Cast<BoundOperatorExpression>(), columns, positional_bindings);
	}
	if (BoundComparisonExpression::IsComparison(expr)) {
		return EstimateComparisonRows(cardinality, expr.Cast<BoundFunctionExpression>(), columns, positional_bindings,
		                              pruning, range);
	}
	return {cardinality, false};
}

static void ApplyFilterEstimate(RelationStats &stats, FilterEstimate estimate) {
	if (!estimate.supported) {
		// An unsupported conjunct retains its fallback even when another conjunct has a weaker estimate.
		estimate.cardinality = MinValue(
		    estimate.cardinality, FilterFraction(stats.cardinality, RelationStatisticsHelper::DEFAULT_SELECTIVITY));
	}
	RelationStatisticsHelper::ApplyCardinality(stats, estimate.cardinality);
}

static void ApplyFilterDomainCap(RelationColumnStats &column, optional_idx cap) {
	if (!cap.IsValid()) {
		return;
	}
	column.current_domain =
	    DistinctCount(MinValue(column.current_domain.distinct_count, cap.GetIndex()), DistinctCountSource::CARDINALITY);
	column.semi_anti_join_domain_evidence.TightenDomainBound(cap.GetIndex());
}

static void ApplyDirectFilterDomain(RelationColumnStats &column, const Expression &expression,
                                    optional<ColumnBinding> binding) {
	ApplyFilterDomainCap(column, RelationStatisticsHelper::EstimateDirectFilterDomain(
	                                 expression, binding, column.total_domain.distinct_count));
}

//===--------------------------------------------------------------------===//
// Zonemap bound
//===--------------------------------------------------------------------===//
//! The [min, max] of a numeric column, over the table or over one row group
struct ZonemapSpan {
	double min;
	double max;
	//! Integral types count values, floating point types measure lengths
	bool integral;

	static bool TryGetNumber(const Value &value, double &result, bool &integral) {
		if (value.IsNull()) {
			return false;
		}
		integral = true;
		switch (value.type().InternalType()) {
		case PhysicalType::INT8:
			result = static_cast<double>(value.GetValueUnsafe<int8_t>());
			return true;
		case PhysicalType::INT16:
			result = static_cast<double>(value.GetValueUnsafe<int16_t>());
			return true;
		case PhysicalType::INT32:
			result = static_cast<double>(value.GetValueUnsafe<int32_t>());
			return true;
		case PhysicalType::INT64:
			result = static_cast<double>(value.GetValueUnsafe<int64_t>());
			return true;
		case PhysicalType::UINT8:
			result = static_cast<double>(value.GetValueUnsafe<uint8_t>());
			return true;
		case PhysicalType::UINT16:
			result = static_cast<double>(value.GetValueUnsafe<uint16_t>());
			return true;
		case PhysicalType::UINT32:
			result = static_cast<double>(value.GetValueUnsafe<uint32_t>());
			return true;
		case PhysicalType::UINT64:
			result = static_cast<double>(value.GetValueUnsafe<uint64_t>());
			return true;
		case PhysicalType::INT128:
			result = Hugeint::Cast<double>(value.GetValueUnsafe<hugeint_t>());
			return true;
		case PhysicalType::FLOAT:
			integral = false;
			result = static_cast<double>(value.GetValueUnsafe<float>());
			return Value::IsFinite(result);
		case PhysicalType::DOUBLE:
			integral = false;
			result = value.GetValueUnsafe<double>();
			return Value::IsFinite(result);
		default:
			return false;
		}
	}

	static optional<ZonemapSpan> FromStatistics(const BaseStatistics &stats) {
		if (stats.GetStatsType() != StatisticsType::NUMERIC_STATS || !NumericStats::HasMinMax(stats)) {
			return {};
		}
		ZonemapSpan result;
		bool max_integral;
		if (!TryGetNumber(NumericStats::Min(stats), result.min, result.integral) ||
		    !TryGetNumber(NumericStats::Max(stats), result.max, max_integral) || result.min > result.max) {
			return {};
		}
		return result;
	}

	double Width() const {
		return max - min;
	}
	//! A row group is clustered on a column if it covers at most half of the span of the column over the table:
	//! two row groups that each cover more than half overlap
	bool IsClusteredIn(const ZonemapSpan &table) const {
		return Width() * 2 <= table.Width();
	}
};

//! The comparisons of a filter that only consists of range comparisons between the column and constants
struct ZonemapRangeFilter {
	struct Bound {
		ExpressionType comparison;
		double value;
	};
	vector<Bound> bounds;

	static bool IsFilterColumn(const Expression &expr) {
		return expr.GetExpressionClass() == ExpressionClass::BOUND_REF ||
		       expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF;
	}

	bool AddComparison(const Expression &expr, const LogicalType &column_type) {
		if (!BoundComparisonExpression::IsComparison(expr)) {
			return false;
		}
		auto &children = expr.Cast<BoundFunctionExpression>().GetChildren();
		auto comparison = expr.GetExpressionType();
		optional_ptr<const Expression> constant = *children[1];
		if (!IsFilterColumn(*children[0])) {
			if (!IsFilterColumn(*children[1])) {
				return false;
			}
			constant = *children[0];
			comparison = FlipComparisonExpression(comparison);
		}
		switch (comparison) {
		case ExpressionType::COMPARE_LESSTHAN:
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		case ExpressionType::COMPARE_GREATERTHAN:
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			break;
		default:
			return false;
		}
		if (constant->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
			return false;
		}
		auto &value = constant->Cast<BoundConstantExpression>().GetValue();
		Bound bound;
		bound.comparison = comparison;
		bool integral;
		if (!value.type().EqualsIncludingCollation(column_type) ||
		    !ZonemapSpan::TryGetNumber(value, bound.value, integral)) {
			return false;
		}
		bounds.push_back(bound);
		return true;
	}

	//! Reads a filter that is a range comparison, or a conjunction of range comparisons
	static optional<ZonemapRangeFilter> FromExpression(const Expression &expr, const LogicalType &column_type) {
		ZonemapRangeFilter result;
		if (expr.GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
			for (auto &child : expr.Cast<BoundConjunctionExpression>().GetChildren()) {
				if (!result.AddComparison(*child, column_type)) {
					return {};
				}
			}
		} else if (!result.AddComparison(expr, column_type)) {
			return {};
		}
		if (result.bounds.empty()) {
			return {};
		}
		return result;
	}

	//! The share of [min, max] of a row group that the range covers, assuming the values are spread evenly
	double CoveredShare(const ZonemapSpan &span) const {
		auto low = span.min;
		auto high = span.max;
		for (auto &bound : bounds) {
			switch (bound.comparison) {
			case ExpressionType::COMPARE_LESSTHAN:
				high = MinValue(high, span.integral ? bound.value - 1 : bound.value);
				break;
			case ExpressionType::COMPARE_LESSTHANOREQUALTO:
				high = MinValue(high, bound.value);
				break;
			case ExpressionType::COMPARE_GREATERTHAN:
				low = MaxValue(low, span.integral ? bound.value + 1 : bound.value);
				break;
			default:
				low = MaxValue(low, bound.value);
				break;
			}
		}
		if (high < low) {
			return 0;
		}
		const double unit = span.integral ? 1 : 0;
		return MinValue(1.0, (high - low + unit) / (span.Width() + unit));
	}
};

struct ZonemapFilter {
	ZonemapFilter(const Expression &expr, const LogicalType &column_type, StorageIndex storage_index_p,
	              optional<ZonemapSpan> table_span_p, optional<ZonemapRangeFilter> range_p)
	    : expr(expr), column_type(column_type), storage_index(std::move(storage_index_p)), table_span(table_span_p),
	      range(std::move(range_p)) {
	}

	const Expression &expr;
	const LogicalType &column_type;
	StorageIndex storage_index;
	//! The [min, max] of the column over the table
	optional<ZonemapSpan> table_span;
	//! Set if the filter is a range whose covered share of a clustered row group can be interpolated
	optional<ZonemapRangeFilter> range;
	//! Whether a row group was counted partially for this filter
	bool refined = false;
};

//! An upper bound on the rows that a scan of a DuckDB table returns: the rows of the row groups whose zonemaps
//! can satisfy every table filter of the scan. A row group that a range filter cuts counts for the share of its
//! own [min, max] that the range covers, but only where the column is stored in runs, which the zonemaps show: the
//! row group covers at most half of the span of the column, and the row groups together cover the span at most
//! ZONEMAP_MAX_SPAN_COVERAGE times. Everywhere else a row group that cannot be pruned counts whole.
//! Returns an invalid index if the bound is not below the estimate that the scan already has.
static optional_idx ZonemapFilterBound(LogicalGet &get, ClientContext &context,
                                       const vector<BaseStatistics> &scan_statistics, idx_t estimate) {
	//! The work per plan is limited to the row groups of a table of this many row groups (2 billion rows at the
	//! default row group size). A larger table keeps its estimate: checking only some of its row groups would not
	//! give a bound, as the row groups that are not checked can hold all of the matching rows
	static constexpr idx_t ZONEMAP_MAX_ROW_GROUPS = 16384;
	//! Row groups of a sorted column tile its span (a coverage of 1, slightly more with shared boundaries). At 2, a
	//! value lies in two row groups on average: the row groups overlap, and [min, max] of a row group says nothing
	//! about where its rows lie
	static constexpr double ZONEMAP_MAX_SPAN_COVERAGE = 2.0;

	auto table = get.GetTable();
	if (!table || !table->IsDuckTable() || !get.function.get_partition_stats) {
		return optional_idx();
	}
	vector<ZonemapFilter> filters;
	for (auto &entry : get.table_filters) {
		if (ExpressionFilter::IsOptionalFilter(entry.Filter())) {
			// optional filters do not restrict the result of the scan
			continue;
		}
		auto &filter = ExpressionFilter::GetExpressionFilter(entry.Filter(), "ZonemapFilterBound");
		if (filter.expr->HasParameter()) {
			continue;
		}
		const auto column_idx = entry.GetIndex().GetIndex();
		if (column_idx >= get.GetColumnIds().size() || column_idx >= scan_statistics.size()) {
			continue;
		}
		StorageIndex storage_index;
		if (!get.TryGetStorageIndex(get.GetColumnIds()[column_idx], storage_index)) {
			continue;
		}
		auto &column_statistics = scan_statistics[column_idx];
		auto table_span = ZonemapSpan::FromStatistics(column_statistics);
		optional<ZonemapRangeFilter> range;
		if (table_span && table_span->Width() > 0) {
			range = ZonemapRangeFilter::FromExpression(*filter.expr, column_statistics.GetType());
		}
		filters.emplace_back(*filter.expr, column_statistics.GetType(), std::move(storage_index), table_span,
		                     std::move(range));
	}
	if (filters.empty()) {
		return optional_idx();
	}
	// the limit is checked before any row group is loaded or collected, and covers the row groups of the table
	// and the ones that the transaction has added
	auto &storage = table->Cast<DuckTableEntry>().GetStorage();
	if (storage.GetRowGroupCountWithoutLoading(context) > ZONEMAP_MAX_ROW_GROUPS) {
		return optional_idx();
	}

	GetPartitionStatsInput input(get.function, get.bind_data.get());
	auto partitions = get.function.get_partition_stats(context, input);
	if (partitions.empty()) {
		return optional_idx();
	}
	if (partitions.size() > ZONEMAP_MAX_ROW_GROUPS) {
		return optional_idx();
	}
	// rows of the row groups that no filter prunes, and the same rows with the row groups that a range cuts
	// counted for their covered share
	idx_t matching_rows = 0;
	double refined_rows = 0;
	for (auto &partition : partitions) {
		bool pruned = false;
		double share = 1;
		for (auto &filter : filters) {
			if (!partition.partition_row_group) {
				break;
			}
			auto stats = partition.partition_row_group->GetColumnStatistics(filter.storage_index);
			if (!stats || !stats->GetType().EqualsIncludingCollation(filter.column_type)) {
				// without statistics the row group counts whole for this filter
				continue;
			}
			// the check that the scan uses to skip the row group
			auto pruning = ExpressionFilter::CheckExpressionStatistics(context, filter.expr, *stats);
			if (pruning == FilterPropagateResult::FILTER_ALWAYS_FALSE ||
			    pruning == FilterPropagateResult::FILTER_FALSE_OR_NULL) {
				pruned = true;
				break;
			}
			if (pruning != FilterPropagateResult::NO_PRUNING_POSSIBLE || !filter.range || !filter.table_span) {
				continue;
			}
			// the range cuts the row group. A row group that holds a single value is not cut, and a row group
			// that is not clustered on the column says nothing that the statistics of the table do not say
			auto row_group_span = ZonemapSpan::FromStatistics(*stats);
			if (!row_group_span || row_group_span->Width() <= 0 || !row_group_span->IsClusteredIn(*filter.table_span)) {
				continue;
			}
			// the rows inside a row group are not counted by any zonemap: the interpolated share keeps the
			// default selectivity as its floor, like the estimate of a range over the table, and several filters
			// combine like their estimates over the table do (the smallest one)
			const auto covered =
			    MaxValue(RelationStatisticsHelper::DEFAULT_SELECTIVITY, filter.range->CoveredShare(*row_group_span));
			if (covered < share) {
				filter.refined = true;
				share = covered;
			}
		}
		if (pruned) {
			continue;
		}
		matching_rows += partition.count;
		refined_rows += static_cast<double>(partition.count) * share;
		if (refined_rows >= static_cast<double>(estimate)) {
			// the bound only grows from here on: it cannot improve on the estimate, so the remaining row groups
			// need not be read
			return optional_idx();
		}
	}
	// the interpolation is only kept if every column it was applied to is stored in runs: the spans of all of its
	// row groups, also of the ones that another filter skips, cover the span of the column about once
	bool in_runs = true;
	for (auto &filter : filters) {
		if (!filter.refined || !filter.table_span) {
			continue;
		}
		const auto table_width = filter.table_span->Width();
		double span_coverage = 0;
		for (auto &partition : partitions) {
			if (!partition.partition_row_group) {
				continue;
			}
			auto stats = partition.partition_row_group->GetColumnStatistics(filter.storage_index);
			auto row_group_span = stats ? ZonemapSpan::FromStatistics(*stats) : optional<ZonemapSpan>();
			if (row_group_span) {
				span_coverage += MinValue(1.0, row_group_span->Width() / table_width);
			}
		}
		if (span_coverage > ZONEMAP_MAX_SPAN_COVERAGE) {
			in_runs = false;
			break;
		}
	}
	auto bound = static_cast<double>(matching_rows);
	if (in_runs) {
		bound = MinValue(bound, std::ceil(refined_rows));
	}
	return MaxValue<idx_t>(1, RelationStatisticsHelper::CardinalityFromDouble(bound));
}

unique_ptr<BaseStatistics> RelationStatisticsHelper::GetColumnStatistics(LogicalGet &get, ClientContext &context,
                                                                         const ColumnIndex &column_id) {
	if (!get.bind_data || (!get.function.statistics && !get.function.statistics_extended)) {
		return nullptr;
	}
	if (get.function.statistics_extended) {
		TableFunctionGetStatisticsInput input(get.bind_data.get(), column_id);
		return get.function.statistics_extended(context, input);
	}
	return get.function.statistics(context, get.bind_data.get(), column_id.GetPrimaryIndex());
}

RelationStats RelationStatisticsHelper::ExtractGetStats(LogicalGet &get, ClientContext &context) {
	return ExtractGetStats(get, context, get.EstimateSourceCardinality(context));
}

RelationStats RelationStatisticsHelper::ExtractGetStats(LogicalGet &get, ClientContext &context,
                                                        idx_t base_table_cardinality) {
	RelationStats result;
	result.cardinality = base_table_cardinality;
	result.row_retention = base_table_cardinality == 0 ? 0 : 1;
	result.table_name = get.GetTable() ? get.GetTable()->name : Identifier(get.GetName());

	// Include hidden filter columns, and acquire each scanned column's provider statistics once.
	vector<BaseStatistics> scan_statistics;
	for (idx_t i = 0; i < MaxValue<idx_t>(1, get.GetColumnIds().size()); i++) {
		ColumnBinding binding(get.table_index, ProjectionIndex(i));
		ColumnIndex fallback_column(get.GetAnyColumn());
		auto column_id = get.GetColumnIds().empty() ? fallback_column : get.GetColumnIndex(binding);
		auto column_statistics = GetColumnStatistics(get, context, column_id);
		auto total_domain = column_statistics ? GetDistinctCountFromStats(*column_statistics, base_table_cardinality)
		                                      : DistinctCount(0, DistinctCountSource::CARDINALITY);
		if (total_domain.distinct_count == 0) {
			total_domain = DistinctCount(base_table_cardinality, DistinctCountSource::CARDINALITY);
		}
		result.columns.emplace_back(binding, total_domain,
		                            Identifier(get.GetName() + "." + get.GetColumnName(column_id)));
		if (column_statistics && column_statistics->GetType().EqualsIncludingCollation(get.GetColumnType(column_id))) {
			scan_statistics.push_back(std::move(*column_statistics));
		} else {
			scan_statistics.push_back(BaseStatistics::CreateUnknown(get.GetColumnType(column_id)));
		}
	}
	RelationColumnStatsLookup columns(result);
	FilterEstimate estimate {base_table_cardinality, true};
	for (auto &entry : get.table_filters) {
		auto &filter = ExpressionFilter::GetExpressionFilter(entry.Filter(), "ExtractGetStats");
		vector<ColumnBinding> bindings {ColumnBinding(get.table_index, entry.GetIndex())};
		D_ASSERT(filter.column_indexes.size() <= 1);
		auto &statistics = scan_statistics[entry.GetIndex().GetIndex()];
		IntegerRangeEstimate range(statistics);
		estimate.Combine(
		    EstimateFilterRows(base_table_cardinality, *filter.expr, columns, bindings, context, statistics, range));
		auto fraction = range.Fraction();
		if (fraction) {
			// Intersect same-column bounds before applying the default-selectivity floor.
			auto range_cardinality =
			    *fraction == 0 ? 0 : FilterFraction(base_table_cardinality, MaxValue(DEFAULT_SELECTIVITY, *fraction));
			estimate.Combine({range_cardinality, true});
		}
	}
	if (get.table_filters.HasFilters() &&
	    !Optimizer::OptimizerDisabled(context, OptimizerType::SCAN_SELECTIVITY_ZONEMAPS)) {
		// the scan skips the row groups whose zonemaps cannot satisfy the filters: their rows bound the estimate
		auto current_estimate = estimate.cardinality;
		if (!estimate.supported) {
			current_estimate = MinValue(current_estimate, FilterFraction(base_table_cardinality, DEFAULT_SELECTIVITY));
		}
		// an estimate of at most one row cannot be improved
		if (current_estimate > 1) {
			auto zonemap_bound = ZonemapFilterBound(get, context, scan_statistics, current_estimate);
			if (zonemap_bound.IsValid()) {
				estimate.Combine({zonemap_bound.GetIndex(), true});
			}
		}
	}
	ApplyFilterEstimate(result, estimate);
	for (auto &column : result.columns) {
		for (auto &entry : get.table_filters) {
			if (ExpressionFilter::IsOptionalFilter(entry.Filter()) || entry.GetIndex() != column.binding.column_index) {
				continue;
			}
			auto &filter = ExpressionFilter::GetExpressionFilter(entry.Filter(), "ExtractGetStats");
			ApplyDirectFilterDomain(column, *filter.expr, {});
		}
	}
	vector<RelationColumnStats> output_columns;
	for (auto binding : get.GetColumnBindings()) {
		if (binding.table_index == get.table_index) {
			output_columns.push_back(result.columns[binding.column_index.GetIndex()]);
		}
	}
	result.columns = std::move(output_columns);
	result.stats_initialized = true;
	D_ASSERT(base_table_cardinality >= result.cardinality);
	return result;
}

optional<RelationStats> RelationStatisticsHelper::ExtractFilterStats(LogicalFilter &filter,
                                                                     const RelationStats &child_stats) {
	auto result = ProjectOutputStats(child_stats, filter);
	if (!result) {
		return {};
	}
	RelationColumnStatsLookup columns(child_stats);
	auto bindings = filter.children[0]->GetColumnBindings();
	FilterEstimate estimate {child_stats.cardinality, true};
	for (auto &expression : filter.expressions) {
		estimate.Combine(EstimateFilterRows(child_stats.cardinality, *expression, columns, bindings));
	}
	ApplyFilterEstimate(*result, estimate);
	for (auto &column : result->columns) {
		for (auto &expression : filter.expressions) {
			ApplyDirectFilterDomain(column, *expression, column.binding);
		}
	}
	return result;
}

RelationStats RelationStatisticsHelper::ExtractDelimGetStats(LogicalDelimGet &delim_get, ClientContext &context) {
	RelationStats result;
	result.table_name = Identifier(delim_get.GetName());
	result.cardinality = delim_get.EstimateCardinality(context);
	result.stats_initialized = true;
	for (auto &binding : delim_get.GetColumnBindings()) {
		result.columns.emplace_back(binding, DistinctCount(1, DistinctCountSource::CARDINALITY),
		                            Identifier("column" + to_string(binding.column_index)));
	}
	result.CapCurrentDomainsToCardinality();
	result.Verify(delim_get.GetColumnBindings());
	return result;
}

RelationStats RelationStatisticsHelper::ExtractDummyScanStats(LogicalDummyScan &dummy_scan, ClientContext &context) {
	RelationStats result;
	result.cardinality = dummy_scan.EstimateCardinality(context);
	result.table_name = "dummy scan";
	result.stats_initialized = true;
	for (auto &binding : dummy_scan.GetColumnBindings()) {
		result.columns.emplace_back(binding, DistinctCount(result.cardinality, DistinctCountSource::CARDINALITY),
		                            Identifier("dummy_scan_column"));
	}
	result.Verify(dummy_scan.GetColumnBindings());
	return result;
}

RelationStats RelationStatisticsHelper::ExtractExpressionGetStats(LogicalExpressionGet &expression_get,
                                                                  ClientContext &context) {
	RelationStats result;
	result.cardinality = expression_get.EstimateCardinality(context);
	result.table_name = "expression_get";
	result.stats_initialized = true;
	for (auto &binding : expression_get.GetColumnBindings()) {
		result.columns.emplace_back(binding, DistinctCount(result.cardinality, DistinctCountSource::CARDINALITY),
		                            Identifier("expression_get_column"));
		if (expression_get.children.size() == 1 &&
		    expression_get.children[0]->type == LogicalOperatorType::LOGICAL_DUMMY_SCAN) {
			result.columns.back().semi_anti_join_domain_evidence.TightenDomainBound(expression_get.expressions.size());
		}
	}
	result.Verify(expression_get.GetColumnBindings());
	return result;
}

RelationStats RelationStatisticsHelper::ExtractColumnDataGetStats(LogicalColumnDataGet &column_data_get,
                                                                  ClientContext &) {
	RelationStats result;
	result.cardinality = column_data_get.collection->Count();
	result.table_name = Identifier(column_data_get.GetName());
	result.stats_initialized = true;
	for (auto &binding : column_data_get.GetColumnBindings()) {
		result.columns.emplace_back(binding, DistinctCount(result.cardinality, DistinctCountSource::CARDINALITY),
		                            Identifier("column_data"));
		result.columns.back().semi_anti_join_domain_evidence.TightenDomainBound(result.cardinality);
	}
	result.Verify(column_data_get.GetColumnBindings());
	return result;
}

} // namespace duckdb
