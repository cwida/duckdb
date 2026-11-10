//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/topn_window_elimination.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/common/enums/order_type.hpp"

namespace duckdb {
class ClientContext;

enum class TopNPayloadType { SINGLE_COLUMN, STRUCT_PACK };

struct TopNWindowEliminationParameters {
	//! Whether the sort is ASCENDING or DESCENDING
	OrderType order_type;
	//! The number of values in the LIMIT clause
	int64_t limit;
	//! How we fetch the payload columns
	TopNPayloadType payload_type;
	//! Whether to include row numbers
	bool include_row_number;
	//! Whether the val or arg column contains null values
	bool can_be_null = false;
	//! The number of semantic window partitions
	idx_t partition_count = 0;
	//! Whether operators below the window can produce the same base-table row ID more than once
	bool row_ids_may_have_duplicates = false;
};

class TopNWindowElimination : public LogicalOperatorVisitor {
public:
	explicit TopNWindowElimination(ClientContext &context, Optimizer &optimizer,
	                               optional_ptr<column_binding_map_t<unique_ptr<BaseStatistics>>> stats_p);

	unique_ptr<LogicalOperator> Optimize(unique_ptr<LogicalOperator> op);

	//! Whether a rewrite used late materialization, which leaves the input of its aggregate to be pruned afterwards
	bool UsedLateMaterialization() const {
		return any_late_materialization;
	}

	//	Returns the <= index for TopN, if any
	static optional_idx CanOptimize(LogicalOperator &op);

private:
	unique_ptr<LogicalOperator> OptimizeInternal(unique_ptr<LogicalOperator> op, ColumnBindingReplacer &replacer);

	unique_ptr<LogicalOperator> CreateAggregateOperator(LogicalWindow &window, vector<unique_ptr<Expression>> args,
	                                                    const TopNWindowEliminationParameters &params) const;
	unique_ptr<LogicalOperator> TryCreateUnnestOperator(unique_ptr<LogicalOperator> op,
	                                                    const TopNWindowEliminationParameters &params) const;
	unique_ptr<LogicalOperator> CreateProjectionOperator(unique_ptr<LogicalOperator> op,
	                                                     const TopNWindowEliminationParameters &params,
	                                                     const map<idx_t, idx_t> &group_idxs) const;

	vector<unique_ptr<Expression>> GenerateAggregatePayload(const vector<ColumnBinding> &bindings,
	                                                        const LogicalWindow &window, map<idx_t, idx_t> &group_idxs);
	bool TraverseProjectionBindings(const vector<ColumnBinding> &old_bindings, reference<LogicalOperator> &op,
	                                vector<ColumnBinding> &new_bindings);
	unique_ptr<Expression> CreateAggregateExpression(vector<unique_ptr<Expression>> aggregate_params, bool requires_arg,
	                                                 const TopNWindowEliminationParameters &params) const;
	unique_ptr<Expression> CreateRowNumberGenerator(unique_ptr<Expression> aggregate_column_ref) const;
	void AddStructExtractExprs(vector<unique_ptr<Expression>> &exprs, const LogicalType &struct_type,
	                           const unique_ptr<BoundColumnRefExpression> &aggregate_column_ref) const;
	unique_ptr<LogicalOperator>
	UpdateTopmostBindings(TableIndex window_idx, unique_ptr<LogicalOperator> op, const vector<LogicalType> &types,
	                      const map<idx_t, idx_t> &group_idxs, const vector<ColumnBinding> &topmost_bindings,
	                      vector<ColumnBinding> &new_bindings, ColumnBindingReplacer &replacer,
	                      const TopNWindowEliminationParameters &params);
	TopNWindowEliminationParameters ExtractOptimizerParameters(const LogicalWindow &window, const LogicalFilter &filter,
	                                                           const vector<ColumnBinding> &bindings,
	                                                           vector<unique_ptr<Expression>> &aggregate_payload);

	// Semi-join reduction methods
	unique_ptr<LogicalOperator> TryPrepareLateMaterialization(const LogicalWindow &window,
	                                                          vector<unique_ptr<Expression>> &args,
	                                                          TopNWindowEliminationParameters &params);
	unique_ptr<LogicalOperator> ConstructLHS(LogicalGet &rhs, vector<ProjectionIndex> &projections) const;
	unique_ptr<LogicalOperator> ConstructJoin(unique_ptr<LogicalOperator> lhs, unique_ptr<LogicalOperator> rhs,
	                                          idx_t aggregate_offset, const TopNWindowEliminationParameters &params);
	bool CanUseLateMaterialization(const LogicalWindow &window, vector<unique_ptr<Expression>> &args,
	                               vector<ProjectionIndex> &projections, vector<reference<LogicalOperator>> &stack,
	                               TopNWindowEliminationParameters &params);
	bool ExtractSingleBinding(unique_ptr<Expression> *expr, ColumnBinding &binding,
	                          bool require_direct_column_ref = false);

private:
	ClientContext &context;
	Optimizer &optimizer;
	optional_ptr<column_binding_map_t<unique_ptr<BaseStatistics>>> stats;
	bool any_late_materialization = false;
};
} // namespace duckdb
