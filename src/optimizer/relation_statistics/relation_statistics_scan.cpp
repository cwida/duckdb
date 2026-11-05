#include "duckdb/optimizer/relation_statistics/relation_statistics_helper.hpp"

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/operator/add.hpp"
#include "duckdb/common/operator/subtract.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/function/table/table_scan.hpp"
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
	column.semi_anti_join_domain_evidence.TightenFilterDomainBound(cap.GetIndex());
}

static void ApplyDirectFilterDomain(RelationColumnStats &column, const Expression &expression,
                                    optional<ColumnBinding> binding) {
	ApplyFilterDomainCap(column, RelationStatisticsHelper::EstimateDirectFilterDomain(
	                                 expression, binding, column.total_domain.distinct_count));
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
	RelationStats result;
	auto base_table_cardinality = get.EstimateSourceCardinality(context);
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
	}
	result.Verify(column_data_get.GetColumnBindings());
	return result;
}

} // namespace duckdb
