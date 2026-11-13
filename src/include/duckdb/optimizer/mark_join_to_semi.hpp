//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/mark_join_to_semi.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/planner/column_binding.hpp"

namespace duckdb {

class LogicalOperator;
class Optimizer;

//! Converts a MARK join whose mark is only consumed by the filter directly above it into a SEMI or ANTI join:
//!   FILTER[mark](MARK[c](L, R))     -> SEMI[c](L, R)
//!   FILTER[NOT mark](MARK[c](L, R)) -> ANTI[c](L, R), if every condition is IS [NOT] DISTINCT FROM
//! FilterPushdown performs this conversion already, but only for a mark that nothing references when it runs.
//! This pass runs after unused columns have been removed and picks up the joins that had a dead reference then.
class MarkJoinToSemi {
public:
	explicit MarkJoinToSemi(Optimizer &optimizer);

	//! Returns whether any join was converted
	bool Optimize(unique_ptr<LogicalOperator> &op);

private:
	void VisitOperator(unique_ptr<LogicalOperator> &op, const unordered_set<TableIndex> &referenced_p);
	void TryConvert(unique_ptr<LogicalOperator> &op, const unordered_set<TableIndex> &referenced);

private:
	Optimizer &optimizer;
	bool changed = false;
};

} // namespace duckdb
