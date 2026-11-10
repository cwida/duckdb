//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/relation_statistics/relation_statistics_extractor.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/optimizer/relation_statistics/relation_statistics.hpp"

#include <functional>

namespace duckdb {

class ClientContext;
class LogicalCTERef;
class LogicalOperator;
class JoinOrderSourceStats;

using relation_stats_cte_callback_t = std::function<optional_ptr<LogicalOperator>(TableIndex)>;

class RelationStatsExtractor {
public:
	explicit RelationStatsExtractor(ClientContext &context);
	RelationStatsExtractor(ClientContext &context, relation_stats_cte_callback_t cte_callback);
	RelationStatsExtractor(ClientContext &context, shared_ptr<const JoinOrderSourceStats> source_stats);

public:
	optional_ptr<const RelationStats> Extract(LogicalOperator &op);
	idx_t ExtractedOperatorCount() const;
	shared_ptr<const JoinOrderSourceStats> GetSourceStats(LogicalOperator &root);

private:
	optional<RelationStats> ExtractInternal(LogicalOperator &op);
	optional<RelationStats> ExtractJoinStats(LogicalOperator &op);
	optional<RelationStats> ExtractCTERef(LogicalCTERef &cte_ref);

private:
	ClientContext &context;
	relation_stats_cte_callback_t cte_callback;
	reference_map_t<LogicalOperator, RelationStats> cache;
	reference_set_t<LogicalOperator> active_operators;
	reference_set_t<LogicalOperator> failed_operators;
	idx_t extracted_operator_count = 0;
	shared_ptr<JoinOrderSourceStats> source_stats;
	//! A join-order caller has already captured all sources before rewriting the plan.
	shared_ptr<const JoinOrderSourceStats> inherited_source_stats;
};

} // namespace duckdb
