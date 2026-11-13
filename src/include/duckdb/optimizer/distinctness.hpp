//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/distinctness.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

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
class Distinctness {
public:
	//! Whether the output of the operator has at most one row per value of the keys
	static bool IsDistinctOn(LogicalOperator &op, const column_binding_set_t &keys);
	//! Gets the equality keys (= or IS NOT DISTINCT FROM) of the left (child_idx 0) or right (child_idx 1) side of a
	//! join. Returns false if there is no equality, or if the key of an equality is not a plain column reference
	static bool GetJoinEqualityKeys(const LogicalComparisonJoin &join, idx_t child_idx, column_binding_set_t &keys);

private:
	static vector<column_binding_set_t> GetUniqueSets(LogicalOperator &op);
	static vector<column_binding_set_t> GetJoinUniqueSets(LogicalComparisonJoin &join);
	static bool IsDistinctOnJoinKeys(LogicalComparisonJoin &join, idx_t child_idx);
};

} // namespace duckdb
