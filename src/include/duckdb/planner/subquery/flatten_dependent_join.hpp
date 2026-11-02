//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/subquery/flatten_dependent_join.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/reference_map.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/subquery/rewrite_correlated_expressions.hpp"

namespace duckdb {

class LogicalAggregate;
class LogicalComparisonJoin;
class LogicalCTERef;
class LogicalExpressionGet;
class LogicalJoin;

//! The FlattenDependentJoins class is responsible for pushing the dependent join down into the plan to create a
//! flattened subquery
class FlattenDependentJoins {
public:
	static unique_ptr<LogicalOperator> DecorrelateIndependent(Binder &binder, unique_ptr<LogicalOperator> plan);

private:
	struct UnnestingState {
		explicit UnnestingState(vector<ColumnBinding> bindings_p) : bindings(std::move(bindings_p)) {
		}

		vector<ColumnBinding> bindings;
		BindingReplacementGraph replacement_graph;
	};
	struct DependentJoinOutput {
		vector<ColumnBinding> left_payload;
		vector<ColumnBinding> right_payload;
	};
	struct SubtreeAccess {
		bool correlated = false;
		bool volatile_expression = false;

		void Merge(const SubtreeAccess &other) {
			correlated = correlated || other.correlated;
			volatile_expression = volatile_expression || other.volatile_expression;
		}
	};

	FlattenDependentJoins(Binder &binder, const CorrelatedColumns &correlated, bool perform_delim = true,
	                      bool any_join = false, optional_ptr<FlattenDependentJoins> parent = nullptr);
	UnnestingState DecorrelateIndependentSubtree(unique_ptr<LogicalOperator> &plan, bool propagate_null_values = true);
	vector<ColumnBinding> CreateContiguousState(ColumnBinding base_binding) const;

	UnnestingState DecorrelateSubtree(unique_ptr<LogicalOperator> &plan, bool propagate_null_values,
	                                  vector<ColumnBinding> state);
	UnnestingState DecorrelateSubtreeInternal(unique_ptr<LogicalOperator> &plan, bool propagate_null_values,
	                                          vector<ColumnBinding> state);
	static void CreateDelimJoinConditions(vector<JoinCondition> &conditions,
	                                      const CorrelatedColumns &correlated_columns,
	                                      const vector<ColumnBinding> &state, bool perform_delim);
	column_binding_map_t<ColumnBinding> GetCurrentBindings(const vector<ColumnBinding> &state) const;
	void RewriteCorrelatedBindings(LogicalOperator &op, const vector<ColumnBinding> &state);
	void InvalidateAccessCache();
	//! Checks whether a subtree must be evaluated in this flattener's active domain.
	bool RequiresDomain(LogicalOperator &op) const;
	SubtreeAccess GetSubtreeAccess(LogicalOperator &op) const;
	idx_t GetDelimKeyIndex(idx_t index) const;

	//! propagate_null_values: everything above cannot distinguish one row with all-NULL original columns from
	//! no row at all. Must be cleared when descending below any operator that breaks that equivalence.
	UnnestingState PushDownCorrelatedNode(unique_ptr<LogicalOperator> &plan, bool propagate_null_values = true);
	UnnestingState PushDownCorrelatedNode(unique_ptr<LogicalOperator> &plan, bool propagate_null_values,
	                                      vector<ColumnBinding> state);
	UnnestingState AttachDomainToIndependentSubtree(unique_ptr<LogicalOperator> &plan, bool propagate_null_values);
	UnnestingState PushDownCorrelatedNodeInternal(unique_ptr<LogicalOperator> &plan, bool propagate_null_values,
	                                              vector<ColumnBinding> state);
	bool VerifyUnnestingState(LogicalOperator &plan, const UnnestingState &state) const;
	optional_ptr<const ColumnBinding> GetCorrelatedBase(const ColumnBinding &binding) const;
	optional_idx GetCorrelatedIndex(const ColumnBinding &binding) const;
	void MergeCorrelatedAliases(const FlattenDependentJoins &source);
	void AddReplacementAliases(const BindingReplacementGraph &replacements);
	Binder &binder;
	column_binding_map_t<ColumnBinding> correlated_aliases;
	column_binding_map_t<CorrelatedAggregateReplacement> replacement_map;
	const CorrelatedColumns &correlated_columns;
	vector<LogicalType> delim_types;

	bool perform_delim;
	bool any_join;
	optional_ptr<FlattenDependentJoins> parent;
	mutable reference_map_t<LogicalOperator, SubtreeAccess> access_cache;
	void AppendCorrelatedColumns(vector<unique_ptr<Expression>> &expressions, const vector<ColumnBinding> &state,
	                             bool include_names) const;
	void AddDelimColumnsToGroup(LogicalAggregate &aggr, const vector<ColumnBinding> &state) const;
	void AddCorrelatedFirstAggregates(LogicalAggregate &aggr, const vector<ColumnBinding> &state) const;
	vector<optional_idx> GetCTERefCorrelatedPositions(const LogicalCTERef &cteref) const;
	void AddCTERefJoinConditions(LogicalComparisonJoin &join, const LogicalCTERef &cteref,
	                             const vector<optional_idx> &positions, const vector<ColumnBinding> &state) const;
	void AddCorrelatedJoinConditions(LogicalJoin &join, const vector<ColumnBinding> &left_state,
	                                 const vector<ColumnBinding> &right_state) const;
	vector<ColumnBinding> CreateDelimCrossProduct(unique_ptr<LogicalOperator> &plan,
	                                              unique_ptr<LogicalOperator> delim_scan,
	                                              vector<ColumnBinding> state) const;
	void PatchAccessingOperators(LogicalOperator &subtree_root, TableIndex table_index,
	                             const CorrelatedColumns &correlated_columns);
	UnnestingState PrepareDependentJoinLeft(unique_ptr<LogicalOperator> &plan, bool propagate_null_values,
	                                        vector<ColumnBinding> state);
	UnnestingState PushDownChild(unique_ptr<LogicalOperator> &plan, bool propagate_null_values,
	                             vector<ColumnBinding> state, bool rewrite_parent = true, idx_t child_idx = 0);
	UnnestingState FinalizeDependentJoin(unique_ptr<LogicalOperator> &plan, UnnestingState outer_state,
	                                     const UnnestingState &right_state, DependentJoinOutput output);
	UnnestingState AttachDelimToIndependentJoinLeft(unique_ptr<LogicalOperator> &plan);
	UnnestingState PushDownSingleCorrelatedChild(unique_ptr<LogicalOperator> &plan, bool propagate_null_values,
	                                             vector<ColumnBinding> state, bool correlated_left);
	UnnestingState PushDownProjection(unique_ptr<LogicalOperator> &plan, bool propagate_null_values,
	                                  vector<ColumnBinding> state);
	UnnestingState PushDownAggregate(unique_ptr<LogicalOperator> &plan, bool propagate_null_values,
	                                 vector<ColumnBinding> state);
	UnnestingState PushDownCrossProduct(unique_ptr<LogicalOperator> &plan, bool propagate_null_values,
	                                    vector<ColumnBinding> state);
	UnnestingState PushDownFullOuterJoin(unique_ptr<LogicalOperator> &plan, bool propagate_null_values,
	                                     vector<ColumnBinding> state);
	UnnestingState PushDownJoin(unique_ptr<LogicalOperator> &plan, bool propagate_null_values,
	                            vector<ColumnBinding> state);
	UnnestingState PushDownLimit(unique_ptr<LogicalOperator> &plan, bool propagate_null_values,
	                             vector<ColumnBinding> state);
	UnnestingState PushDownWindow(unique_ptr<LogicalOperator> &plan, bool propagate_null_values,
	                              vector<ColumnBinding> state);
	UnnestingState PushDownSetOperation(unique_ptr<LogicalOperator> &plan, vector<ColumnBinding> state);
	UnnestingState PushDownDistinct(unique_ptr<LogicalOperator> &plan, bool propagate_null_values,
	                                vector<ColumnBinding> state);
	UnnestingState PushDownExpressionGet(unique_ptr<LogicalOperator> &plan, bool propagate_null_values,
	                                     vector<ColumnBinding> state);
	UnnestingState PushDownDML(unique_ptr<LogicalOperator> &plan, bool propagate_null_values,
	                           vector<ColumnBinding> state);
	UnnestingState PushDownGet(unique_ptr<LogicalOperator> &plan, vector<ColumnBinding> state);
	UnnestingState PushDownCTE(unique_ptr<LogicalOperator> &plan, bool propagate_null_values,
	                           vector<ColumnBinding> state);
	UnnestingState PushDownCTERef(unique_ptr<LogicalOperator> &plan);
};

} // namespace duckdb
