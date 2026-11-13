//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/distinctness.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/reference_map.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/planner/column_binding_map.hpp"

namespace duckdb {

class LogicalComparisonJoin;
class LogicalOperator;

//! Structural proofs that the output of an operator has at most one row per value of a set of columns, where NULLs
//! count as one value (like for DISTINCT and GROUP BY).
//! The sources are the groups of an aggregate and the PRIMARY KEY / UNIQUE NOT NULL constraints of a scanned table.
//! They are forwarded through the operators that never duplicate rows: filters, limits, orderings, samples, windows,
//! projections (plain column references only), the left side of SEMI / ANTI / MARK / SINGLE joins, and a side of an
//! INNER join (the left side of a LEFT join) whose other side is itself distinct on its equality keys.
//! Statistics (distinct counts) are estimates and are never used.
//! The unique sets of an operator are computed once and cached, so an instance must not outlive changes to the
//! operators it has been asked about.
class Distinctness {
public:
	//! The maximum number of unique sets that is tracked per operator
	static constexpr idx_t MAX_UNIQUE_SETS = 32;

public:
	//! Whether the output of the operator has at most one row per value of the keys
	bool IsDistinctOn(LogicalOperator &op, const column_binding_set_t &keys);
	//! Gets the equality keys (= or IS NOT DISTINCT FROM) of the left (child_idx 0) or right (child_idx 1) side of a
	//! join. Returns false if there is no equality, or if the key of an equality is not a plain column reference
	static bool GetJoinEqualityKeys(const LogicalComparisonJoin &join, idx_t child_idx, column_binding_set_t &keys);

private:
	//! The sets of columns the output of the operator is distinct on (cached)
	const vector<column_binding_set_t> &GetUniqueSets(LogicalOperator &op);
	vector<column_binding_set_t> ComputeUniqueSets(LogicalOperator &op);
	vector<column_binding_set_t> ComputeJoinUniqueSets(LogicalComparisonJoin &join);
	bool IsDistinctOnJoinKeys(LogicalComparisonJoin &join, idx_t child_idx);

private:
	reference_map_t<LogicalOperator, vector<column_binding_set_t>> unique_sets;
};

} // namespace duckdb
