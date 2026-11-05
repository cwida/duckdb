//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/join_filter_pushdown_optimizer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/execution/operator/join/join_filter_pushdown.hpp"

namespace duckdb {
class Optimizer;
class LogicalCTERef;

enum class EqualityPropagation { DISABLED, ENABLED };

//! The JoinFilterPushdownOptimizer links comparison joins to data sources to enable dynamic execution-time filter
//! pushdown
class JoinFilterPushdownOptimizer : public LogicalOperatorVisitor {
public:
	explicit JoinFilterPushdownOptimizer(Optimizer &optimizer);

public:
	void Optimize(LogicalOperator &op);
	void VisitOperator(LogicalOperator &op) override;
	//! Find scan targets independently for each column, allowing both NULL-retaining and NULL-rejecting filters.
	//! Equality propagation can return multiple equivalent columns per scan; single-column callers leave it disabled.
	//! Only the join filter pushdown continues into secure views (cross_secure_views): the other callers act on what
	//! they find, e.g. a target's estimated cardinality, which must not depend on a view's contents
	static void GetPushdownFilterTargets(LogicalOperator &op, vector<JoinFilterPushdownColumn> columns,
	                                     vector<PushdownFilterTarget> &targets,
	                                     EqualityPropagation equality_propagation = EqualityPropagation::DISABLED,
	                                     bool cross_secure_views = false);

	static bool IsFiltering(const unique_ptr<LogicalOperator> &op);

private:
	void CollectCTEFilters(LogicalOperator &op);
	void GenerateCTEFilters(LogicalMaterializedCTE &cte);
	void GenerateJoinFilters(LogicalComparisonJoin &join);
	static bool CanGenerateJoinFilters(const LogicalComparisonJoin &join);
	static void GetCTEFilterTargets(LogicalOperator &op, vector<JoinFilterPushdownColumn> columns,
	                                reference_map_t<LogicalCTERef, vector<JoinFilterPushdownColumn>> &targets);

private:
	Optimizer &optimizer;
	reference_map_t<LogicalComparisonJoin, vector<JoinFilterPushdownFilter>> cte_filters;
};
} // namespace duckdb
