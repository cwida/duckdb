//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/group_by_top_key.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/bound_query_node.hpp"
#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

class LogicalAggregate;
class Optimizer;

//! Bounds the input of a grouped aggregate whose Top-N orders by a prefix of the group keys:
//!
//!   TOP_N(n, ORDER BY k1 [, k2 ..] [, agg ..])        TOP_N(..)
//!     PROJECTION*                                       PROJECTION*
//!       AGGREGATE(k1 .. km; aggs)              ==>        AGGREGATE(k1 .. km; aggs)
//!         <input>                                           TOP_KEY_FILTER(n, keys: k1 [, k2 ..])
//!                                                             <input>      (+ a dynamic filter on k1 in the scans)
//!
//! The filter keeps the n smallest distinct key tuples seen so far and only passes rows that are not larger than the
//! largest of them. That bound is never smaller than the n-th smallest distinct key tuple of the whole input, so every
//! group that the Top-N can return still receives all of its rows and its aggregates are exact. A prefix of the
//! ORDER BY suffices because the order is lexicographic.
//!
//! The filter computes a key for every input row, the Top-N for one row of a group. A key therefore has to be a group
//! key (through projections that preserve the order), or an expression over group keys that has the same value for all
//! rows of a group: one that cannot throw and only reads group keys whose equal values cannot be told apart (not
//! FLOAT / DOUBLE, where -0.0 = 0.0). Key types whose sort keys do not agree with their comparison are declined.
//!
//! A LIMIT without an ORDER BY over a grouped aggregate may return any n groups. It is turned into a Top-N on a group
//! key first, so that the aggregate only has to aggregate the n groups that are returned:
//!
//!   LIMIT(n)                                 PROJECTION*
//!     PROJECTION*                   ==>        TOP_N(n, ORDER BY k)
//!       AGGREGATE(k1 .. km; aggs)                AGGREGATE(k1 .. km; aggs)
//!
//! The projections must not be volatile or able to throw: above the Top-N they only see the rows that are returned.
class GroupByTopKey {
public:
	explicit GroupByTopKey(Optimizer &optimizer);

	unique_ptr<LogicalOperator> Optimize(unique_ptr<LogicalOperator> op);

private:
	//! A matched TOP_N -> PROJECTION* -> AGGREGATE chain
	struct TopKeyMatch {
		//! LIMIT + OFFSET: the number of distinct key tuples that pass
		idx_t limit = 0;
		//! The number of projections between the Top-N and the aggregate
		idx_t depth = 0;
		//! The keys of the ORDER BY prefix as expressions over the input of the aggregate
		vector<BoundOrderByNode> prefix;
	};

private:
	//! LIMIT over a grouped aggregate -> Top-N over the aggregate
	unique_ptr<LogicalOperator> LimitToTopN(unique_ptr<LogicalOperator> op);
	bool TryMatch(LogicalOperator &op, TopKeyMatch &match);
	void Rewrite(LogicalAggregate &aggregate, TopKeyMatch &match);
	//! Computes the keys that are not columns of the input in a projection below the aggregate
	unique_ptr<LogicalOperator> MaterializeKeys(LogicalAggregate &aggregate, unique_ptr<LogicalOperator> input,
	                                            vector<BoundOrderByNode> &keys);

private:
	Optimizer &optimizer;
};

} // namespace duckdb
