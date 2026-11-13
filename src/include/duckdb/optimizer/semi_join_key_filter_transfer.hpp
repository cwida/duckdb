//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/semi_join_key_filter_transfer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/table_index.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

class BoundColumnRefExpression;

//! Copies a predicate that every row of the right side of a SEMI join (or of a MARK join under a filter on its mark)
//! satisfies on an equality key onto the key of the left side, when the left side is a reference of a materialized
//! CTE and the new filter makes every reference of that CTE a filtered one. The CTEFilterPusher then moves the
//! predicate into the CTE definition.
//!
//!   SEMI[l = r](CTE_REF c, R)                 ->  SEMI[l = r](FILTER[p(l)](CTE_REF c), R)
//!   FILTER[mark](MARK[l = r](CTE_REF c, R))   ->  FILTER[mark](MARK[l = r](FILTER[p(l)](CTE_REF c), R))
//!
//! when p(r) is TRUE for every row of R
class SemiJoinKeyFilterTransfer {
public:
	void Optimize(unique_ptr<LogicalOperator> &plan);

private:
	//! The references of a materialized CTE, and how many of them sit directly below a filter
	struct CTEReferences {
		idx_t total = 0;
		idx_t filtered = 0;
	};

private:
	void CountReferences(const LogicalOperator &op);
	void VisitOperator(unique_ptr<LogicalOperator> &op);
	void Transfer(LogicalOperator &join_op);

	//! The predicates over the column `key` that hold for every row the operator produces
	static void CollectKeyPredicates(LogicalOperator &op, const BoundColumnRefExpression &key, idx_t &steps,
	                                 vector<unique_ptr<Expression>> &result);

private:
	unordered_map<TableIndex, CTEReferences> references;
};

} // namespace duckdb
