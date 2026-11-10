//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/subquery/delim_join_cte_rewriter.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

//! Rewrites fully decorrelated DelimJoins into materialized CTEs.
class DelimJoinCTERewriter {
public:
	static void Rewrite(Binder &binder, unique_ptr<LogicalOperator> &plan);

private:
	explicit DelimJoinCTERewriter(Binder &binder);

	void Rewrite(unique_ptr<LogicalOperator> &plan);
	BindingReplacementGraph RewriteDelimJoinsToCTEs(unique_ptr<LogicalOperator> &plan, LogicalOperator &rewrite_root,
	                                                bool null_rejecting_filter_above = false,
	                                                bool preserve_evidence_side = false);
	BindingReplacementGraph MaterializeDelimJoinAsCTE(unique_ptr<LogicalOperator> &plan, LogicalOperator &rewrite_root,
	                                                  bool null_rejecting_filter_above, bool preserve_evidence_side,
	                                                  bool preserve_nested_evidence_side);

private:
	Binder &binder;
	bool cte_deliminator_enabled;
	vector<TableIndex> generated_dedup_cte_indexes;
	set<TableIndex> marker_indexes;
	set<TableIndex> preserve_marker_indexes;
	set<TableIndex> preserve_nested_marker_indexes;
};

} // namespace duckdb
