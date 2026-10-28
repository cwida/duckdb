//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/relation_statistics/relation_statistics.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/planner/column_binding_map.hpp"

namespace duckdb {

enum class DistinctCountSource : uint8_t { CARDINALITY, MIN_MAX, HLL, EXACT };

struct DistinctCount {
public:
	DistinctCount(idx_t distinct_count, DistinctCountSource source);

public:
	idx_t distinct_count;
	DistinctCountSource source;
};

struct SemiAntiJoinDomainEvidence {
public:
	void TightenFilterDomainBound(idx_t bound);
	void MarkUnique();
	void Invalidate();
	optional_idx GetSupportedDomainSize(idx_t current_domain) const;
	optional_idx GetFilterDomainBound() const;

private:
	optional_idx filter_domain_bound;
	bool is_unique = false;
};

struct RelationColumnStats {
public:
	RelationColumnStats(ColumnBinding binding, DistinctCount domain, Identifier name);
	RelationColumnStats(ColumnBinding binding, DistinctCount total_domain, DistinctCount current_domain,
	                    Identifier name,
	                    SemiAntiJoinDomainEvidence semi_anti_join_domain_evidence = SemiAntiJoinDomainEvidence());
	optional_idx GetSupportedSemiAntiDomainSize() const;

public:
	ColumnBinding binding;
	//! The estimated size of the complete value domain. Equality joins use this as their denominator.
	DistinctCount total_domain;
	//! The estimated number of values from the total domain that are currently present.
	DistinctCount current_domain;
	//! Structural evidence that permits using current_domain for SEMI/ANTI joins.
	SemiAntiJoinDomainEvidence semi_anti_join_domain_evidence;
	Identifier name;
};

struct RelationStats {
public:
	RelationStats();

public:
	optional_idx FindColumn(ColumnBinding binding) const;
	optional_ptr<const RelationColumnStats> GetColumnStats(ColumnBinding binding) const;
	bool MatchesBindings(const vector<ColumnBinding> &bindings) const;
	void CapCurrentDomainsToCardinality();
	void Verify(const vector<ColumnBinding> &bindings) const;

public:
	vector<RelationColumnStats> columns;
	idx_t cardinality;
	//! Cumulative estimated fraction of rows retained from the most recent statistics source.
	//! Aggregation and DISTINCT establish a new source.
	double row_retention = 1;
	bool stats_initialized = false;
	Identifier table_name;
};

//! Looks up column statistics by binding, the first relation and column with a binding wins
class RelationColumnStatsLookup {
public:
	RelationColumnStatsLookup() = default;
	explicit RelationColumnStatsLookup(const RelationStats &stats);

public:
	void AddRelation(const RelationStats &stats);
	optional_ptr<const RelationColumnStats> Find(ColumnBinding binding) const;

private:
	column_binding_map_t<reference<const RelationColumnStats>> columns;
};

} // namespace duckdb
