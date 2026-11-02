//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/relation_statistics/relation_statistics_helper.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional.hpp"
#include "duckdb/common/enums/join_type.hpp"
#include "duckdb/optimizer/relation_statistics/relation_statistics.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

class ClientContext;
class Expression;
class LogicalAggregate;
class LogicalColumnDataGet;
class LogicalComparisonJoin;
class LogicalDelimGet;
class LogicalDistinct;
class LogicalDummyScan;
class LogicalEmptyResult;
class LogicalExpressionGet;
class LogicalFilter;
class LogicalGet;
class LogicalLimit;
class LogicalProjection;
class LogicalWindow;

struct SemiAntiJoinDomain {
	idx_t total_domain;
	idx_t rhs_current_domain;
	ColumnBinding preserved_binding;
	ColumnBinding rhs_binding;
};

struct SemiAntiJoinCardinalityEstimate {
	double cardinality;
	bool used_domain_evidence;
};

class RelationStatisticsHelper {
public:
	static constexpr double DEFAULT_SELECTIVITY = 0.2;

public:
	static idx_t InspectTableFilter(idx_t cardinality, const TableFilter &filter, BaseStatistics &base_stats);
	static RelationStats ExtractGetStats(LogicalGet &get, ClientContext &context);
	static RelationStats ExtractDelimGetStats(LogicalDelimGet &delim_get, ClientContext &context);
	static RelationStats ExtractDummyScanStats(LogicalDummyScan &dummy_scan, ClientContext &context);
	static RelationStats ExtractExpressionGetStats(LogicalExpressionGet &expression_get, ClientContext &context);
	static RelationStats ExtractColumnDataGetStats(LogicalColumnDataGet &column_data_get, ClientContext &context);
	static RelationStats ExtractExplainStats(LogicalOperator &op);
	static optional<RelationStats> ExtractOperatorStats(LogicalOperator &op, ClientContext &context,
	                                                    const vector<reference<const RelationStats>> &child_stats);
	static optional<RelationStats> ExtractProjectionStats(LogicalProjection &projection,
	                                                      const RelationStats &child_stats);
	static optional<RelationStats> ExtractAggregationStats(LogicalAggregate &aggregate,
	                                                       const RelationStats &child_stats, ClientContext &context);
	static optional<RelationStats> ExtractWindowStats(LogicalWindow &window, const RelationStats &child_stats);
	static optional<RelationStats> ExtractDistinctStats(LogicalDistinct &distinct, const RelationStats &child_stats);
	static optional<RelationStats> ExtractFilterStats(LogicalFilter &filter, const RelationStats &child_stats);
	static optional<RelationStats> ExtractLimitStats(LogicalLimit &limit, const RelationStats &child_stats);
	static RelationStats ExtractEmptyResultStats(LogicalEmptyResult &empty);
	static optional<RelationStats> ProjectOutputStats(const RelationStats &stats, LogicalOperator &op);
	static optional<RelationStats> RebindOutputStats(const RelationStats &stats, LogicalOperator &op);
	static optional<DistinctCount> SelectTotalDomain(const vector<DistinctCount> &domains);
	static idx_t EstimateDistinctCardinality(const vector<DistinctCount> &distinct_counts, idx_t input_cardinality);
	static DistinctCount EstimateCurrentDomain(const DistinctCount &current_domain, idx_t input_cardinality,
	                                           idx_t output_cardinality);
	static void ApplyCardinality(RelationStats &stats, idx_t output_cardinality);
	static SemiAntiJoinCardinalityEstimate EstimateSemiAntiJoinFallback(double preserved_cardinality,
	                                                                    idx_t rhs_cardinality, JoinType join_type);
	static SemiAntiJoinCardinalityEstimate
	EstimateSemiAntiJoinCardinality(double preserved_cardinality, idx_t rhs_cardinality, double rhs_row_retention,
	                                JoinType join_type, const vector<SemiAntiJoinDomain> &domains, bool has_residual);
	static SemiAntiJoinCardinalityEstimate
	EstimateSemiAntiJoinCardinality(LogicalComparisonJoin &join, const RelationStats &left, const RelationStats &right);
	static optional_idx EstimateDirectFilterDomain(const Expression &expression, optional<ColumnBinding> target_binding,
	                                               idx_t maximum_domain);

private:
	static unique_ptr<BaseStatistics> GetColumnStatistics(LogicalGet &get, ClientContext &context,
	                                                      const ColumnIndex &column_id);
};

} // namespace duckdb
