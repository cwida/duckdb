//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/scalar_aggregate_merger.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

class Optimizer;

//! The ScalarAggregateMerger computes sibling ungrouped aggregates over the same table(s) in one pass.
//!
//! An uncorrelated scalar subquery that returns exactly one row, and a one-row derived table in a FROM list, are
//! planned as a cross product with the rest of the query, and each of them reads its tables on its own. Siblings of
//! the form [projection ->] ungrouped aggregate -> filters -> plain scan(s) of the same table(s) are rewritten into one
//! scan, filtered by the predicates every sibling shares and the OR of what each has beyond them, feeding one aggregate
//! that holds every sibling's aggregates, each with a FILTER (WHERE ...) of its own predicate. A projection then hands
//! every sibling's columns back under new bindings (TPC-DS q9, q28, q88 and q90).
class ScalarAggregateMerger {
public:
	explicit ScalarAggregateMerger(Optimizer &optimizer);

	//! Rewrites every eligible group of siblings in the plan
	void Optimize(unique_ptr<LogicalOperator> &plan);

public:
	struct Branch;
	struct MergeTarget;

private:
	void VisitOperator(unique_ptr<LogicalOperator> &op);
	//! Merges the eligible leaves of the cross-product tree rooted at `op`
	void MergeCrossProduct(unique_ptr<LogicalOperator> &op);
	//! Appends `leaf` to `branches` when it has the shape of a branch
	static void TryMatchBranch(unique_ptr<LogicalOperator> &leaf, vector<Branch> &branches);
	//! The merged leaf that replaces the branches of `group`, or null when the group cannot be merged
	unique_ptr<LogicalOperator> MergeBranches(vector<Branch> &branches, const vector<idx_t> &group);

private:
	Optimizer &optimizer;
	//! The bindings of the branches that were merged away, pointed at the merged projection's columns
	vector<ReplacementBinding> replacements;
};

} // namespace duckdb
