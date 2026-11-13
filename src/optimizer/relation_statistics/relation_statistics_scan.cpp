#include "duckdb/optimizer/relation_statistics/relation_statistics_helper.hpp"

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/operator/add.hpp"
#include "duckdb/common/operator/subtract.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/function/table/table_scan.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/operator/logical_delim_get.hpp"
#include "duckdb/planner/operator/logical_column_data_get.hpp"
#include "duckdb/planner/operator/logical_dummy_scan.hpp"
#include "duckdb/planner/operator/logical_expression_get.hpp"
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
	auto cardinality_after_filters = base_table_cardinality;
	result.table_name = get.GetTable() ? get.GetTable()->name : Identifier(get.GetName());

	if (get.table_filters.HasFilters()) {
		bool has_non_optional_filters = false;
		for (auto &entry : get.table_filters) {
			auto &column_index = get.GetColumnIndex(entry.GetIndex());
			auto column_statistics = GetColumnStatistics(get, context, column_index);
			if (column_statistics) {
				auto &filter = ExpressionFilter::GetExpressionFilter(entry.Filter(), "ExtractGetStats");
				if (filter.column_indexes.size() <= 1 &&
				    filter.CheckStatistics(context, *column_statistics) == FilterPropagateResult::FILTER_ALWAYS_TRUE) {
					continue;
				}
				cardinality_after_filters =
				    MinValue(cardinality_after_filters,
				             InspectTableFilter(base_table_cardinality, entry.Filter(), *column_statistics));
			}
			if (!ExpressionFilter::IsOptionalFilter(entry.Filter())) {
				has_non_optional_filters = true;
			}
		}
		if (cardinality_after_filters == base_table_cardinality && has_non_optional_filters) {
			cardinality_after_filters =
			    MaxValue<idx_t>(LossyNumericCast<idx_t>(double(base_table_cardinality) * DEFAULT_SELECTIVITY), 1);
		}
		if (base_table_cardinality == 0) {
			cardinality_after_filters = 0;
		}
	}

	result.cardinality = cardinality_after_filters;
	result.row_retention = base_table_cardinality == 0
	                           ? 0
	                           : MinValue(1.0, double(cardinality_after_filters) / double(base_table_cardinality));
	for (auto &binding : get.GetColumnBindings()) {
		if (binding.table_index != get.table_index) {
			continue;
		}
		ColumnIndex fallback_column(get.GetAnyColumn());
		auto column_id = get.GetColumnIds().empty() ? fallback_column : get.GetColumnIndex(binding);
		auto column_statistics = GetColumnStatistics(get, context, column_id);
		auto total_domain = column_statistics ? GetDistinctCountFromStats(*column_statistics, base_table_cardinality)
		                                      : DistinctCount(0, DistinctCountSource::CARDINALITY);
		if (total_domain.distinct_count == 0) {
			total_domain = DistinctCount(base_table_cardinality, DistinctCountSource::CARDINALITY);
		}
		auto current_domain = EstimateCurrentDomain(total_domain, base_table_cardinality, cardinality_after_filters);
		SemiAntiJoinDomainEvidence semi_anti_join_domain_evidence;
		for (auto &entry : get.table_filters) {
			if (ExpressionFilter::IsOptionalFilter(entry.Filter()) ||
			    get.GetColumnIndex(entry.GetIndex()) != column_id) {
				continue;
			}
			auto &filter =
			    ExpressionFilter::GetExpressionFilter(entry.Filter(), "RelationStatisticsHelper::ExtractGetStats");
			auto direct_bound = EstimateDirectFilterDomain(*filter.expr, {}, total_domain.distinct_count);
			if (direct_bound.IsValid()) {
				current_domain = DistinctCount(MinValue(current_domain.distinct_count, direct_bound.GetIndex()),
				                               DistinctCountSource::CARDINALITY);
				semi_anti_join_domain_evidence.TightenFilterDomainBound(direct_bound.GetIndex());
			}
		}
		result.columns.emplace_back(binding, total_domain, current_domain,
		                            Identifier(get.GetName() + "." + get.GetColumnName(column_id)),
		                            semi_anti_join_domain_evidence);
	}
	result.stats_initialized = true;
	D_ASSERT(base_table_cardinality >= cardinality_after_filters);
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

idx_t RelationStatisticsHelper::InspectTableFilter(idx_t cardinality, const TableFilter &filter,
                                                   BaseStatistics &base_stats) {
	auto cardinality_after_filters = cardinality;
	auto &expr_filter = ExpressionFilter::GetExpressionFilter(filter, "RelationStatisticsHelper::InspectTableFilter");
	auto &expr = *expr_filter.expr;
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &function = expr.Cast<BoundFunctionExpression>();
		if (function.Function().GetName() == "!~~" && !base_stats.CanHaveNull()) {
			if (cardinality == 0) {
				return 0;
			}
			return MaxValue<idx_t>(
			    LossyNumericCast<idx_t>(static_cast<double>(cardinality) * (1.0 - DEFAULT_SELECTIVITY)), 1);
		}
	}
	if (expr.GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
		auto &conjunction = expr.Cast<BoundConjunctionExpression>();
		for (auto &child : conjunction.GetChildren()) {
			ExpressionFilter child_filter(child->Copy());
			cardinality_after_filters =
			    MinValue(cardinality_after_filters, InspectTableFilter(cardinality, child_filter, base_stats));
		}
		return cardinality_after_filters;
	}
	if (!BoundComparisonExpression::IsComparison(expr)) {
		return cardinality_after_filters;
	}
	auto &comparison = expr.Cast<BoundFunctionExpression>();
	if (comparison.GetExpressionType() != ExpressionType::COMPARE_EQUAL) {
		return cardinality_after_filters;
	}
	auto column_count = GetDistinctCountFromStats(base_stats, cardinality).distinct_count;
	if (column_count > 0) {
		cardinality_after_filters = (cardinality + column_count - 1) / column_count;
	}
	return cardinality_after_filters;
}

} // namespace duckdb
