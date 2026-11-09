//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/join_order/join_order_source_stats.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/unordered_map.hpp"
#include "duckdb/optimizer/relation_statistics/relation_statistics.hpp"

namespace duckdb {

class ClientContext;
class LogicalGet;
class LogicalOperator;

struct JoinColumnSource {
	ColumnBinding binding;
	idx_t source_cardinality;
	idx_t scan_cardinality;
	DistinctCount domain;
};

struct JoinColumnSources {
	explicit JoinColumnSources(JoinColumnSource direct) : direct(std::move(direct)) {
	}

	JoinColumnSource direct;
	vector<JoinColumnSource> equivalents;
};

using join_column_sources_t = column_binding_map_t<JoinColumnSources>;

//! Immutable source evidence captured before any join-order subtree is optimized.
class JoinOrderSourceStats {
public:
	JoinOrderSourceStats(ClientContext &context, LogicalOperator &root);

public:
	//! Capture additional read-only roots, such as externally supplied CTE definitions.
	void AddPlan(ClientContext &context, LogicalOperator &root);
	optional_ptr<const RelationStats> FindScanStats(const LogicalGet &get) const;
	//! Resolve the visible columns of one relation, respecting statistics boundaries.
	join_column_sources_t Resolve(LogicalOperator &root) const;

private:
	struct ScanStats {
		idx_t cardinality;
		RelationStats stats;
	};

	unordered_map<TableIndex, ScanStats> scans;
	unordered_map<TableIndex, join_column_sources_t> projections;
};

} // namespace duckdb
