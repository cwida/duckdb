//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/disjunctive_join_rewriter.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/planner/binder.hpp"

namespace duckdb {

struct JoinCondition;

class DisjunctiveJoinRewriter {
public:
	explicit DisjunctiveJoinRewriter(Binder &binder);

	unique_ptr<LogicalOperator> Optimize(unique_ptr<LogicalOperator> op);

private:
	struct Branch {
		unique_ptr<Expression> left_expr;
		unique_ptr<Expression> right_expr;
		ExpressionType comparison_type;
	};

	struct CTEInfo {
		TableIndex table_index;
		vector<LogicalType> output_types;
		vector<ColumnBinding> original_bindings;
	};

	Binder &binder;

	TableIndex NewTableIndex();

	unique_ptr<LogicalOperator> OptimizeInternal(unique_ptr<LogicalOperator> op, BindingReplacementGraph &replacements);

	bool ShouldRewrite(const LogicalOperator &join, const unordered_set<TableIndex> &left_tables,
	                   const unordered_set<TableIndex> &right_tables, vector<Branch> &out_branches) const;
	bool FlattenOR(const Expression &expr, const unordered_set<TableIndex> &left_tables,
	               const unordered_set<TableIndex> &right_tables, vector<Branch> &out) const;

	// Main builders for each join type
	unique_ptr<LogicalOperator> BuildInnerJoin(const CTEInfo &left_cte, const CTEInfo &right_cte,
	                                           const vector<Branch> &branches);
	unique_ptr<LogicalOperator> BuildLeftJoin(const CTEInfo &left_cte, const CTEInfo &right_cte,
	                                          const vector<Branch> &branches);
	unique_ptr<LogicalOperator> BuildRightJoin(const CTEInfo &left_cte, const CTEInfo &right_cte,
	                                           const vector<Branch> &branches);
	unique_ptr<LogicalOperator> BuildFullJoin(const CTEInfo &left_cte, const CTEInfo &right_cte,
	                                          const vector<Branch> &branches);
	unique_ptr<LogicalOperator> BuildSemiJoin(const CTEInfo &left_cte, const CTEInfo &right_cte,
	                                          const vector<Branch> &branches);
	unique_ptr<LogicalOperator> BuildAntiJoin(const CTEInfo &left_cte, const CTEInfo &right_cte,
	                                          const vector<Branch> &branches);

	// Helper builders
	vector<unique_ptr<LogicalOperator>> CreateMatchedBranches(const CTEInfo &left_cte, const CTEInfo &right_cte,
	                                                          const vector<Branch> &branches);
	unique_ptr<LogicalOperator> CreateBranchJoin(const CTEInfo &left_cte, const CTEInfo &right_cte,
	                                             const vector<Branch> &branches, idx_t branch_idx);
	unique_ptr<LogicalOperator> CreateUnmatchedProbeSide(const CTEInfo &probe_cte, const CTEInfo &build_cte,
	                                                     const vector<Branch> &branches);
	unique_ptr<LogicalOperator> CreateUnmatchedBuildSide(const CTEInfo &left_cte, const CTEInfo &right_cte,
	                                                     const vector<Branch> &branches);
	unique_ptr<LogicalOperator> CreateAntiJoinChain(const CTEInfo &probe_cte, const CTEInfo &build_cte,
	                                                const vector<Branch> &branches, TableIndex probe_ref_idx);

	unique_ptr<LogicalOperator> CreateUnionAll(vector<unique_ptr<LogicalOperator>> children, idx_t total_columns);
	unique_ptr<LogicalOperator> NormalizeOutput(unique_ptr<LogicalOperator> plan,
	                                            const vector<ColumnBinding> &orig_bindings,
	                                            const vector<LogicalType> &orig_types, const CTEInfo &left_cte,
	                                            const CTEInfo &right_cte, JoinType join_type,
	                                            BindingReplacementGraph &replacements);

	// Expression and Operator utilities
	JoinCondition CreateJoinCondition(const Branch &branch, ColumnBindingReplacer &left_replacer,
	                                  ColumnBindingReplacer &right_replacer);

	unique_ptr<LogicalOperator> CreateCTERef(const CTEInfo &cte, TableIndex ref_idx);
	unique_ptr<LogicalOperator> CreateProjection(unique_ptr<LogicalOperator> child,
	                                             vector<unique_ptr<Expression>> expressions);
	unique_ptr<Expression> CreateColRef(ColumnBinding binding, const LogicalType &type, const string &alias = "");
	unique_ptr<Expression> CreateNullExpr(const LogicalType &type);
	vector<unique_ptr<Expression>> CreateColRefs(TableIndex table_index, const vector<LogicalType> &types);
	vector<unique_ptr<Expression>> CreateNullExprs(const vector<LogicalType> &types);

	vector<Branch> SwapBranches(const vector<Branch> &branches);
};

} // namespace duckdb
