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

namespace duckdb {
class Optimizer;
struct JoinFilterPushdownColumn;
struct PushdownFilterTarget;

enum class EqualityPropagation { DISABLED, ENABLED };

//! The JoinFilterPushdownOptimizer links comparison joins to data sources to enable dynamic execution-time filter
//! pushdown
class JoinFilterPushdownOptimizer : public LogicalOperatorVisitor {
public:
	explicit JoinFilterPushdownOptimizer(Optimizer &optimizer);

public:
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
	void GenerateJoinFilters(LogicalComparisonJoin &join);

private:
	Optimizer &optimizer;
};
} // namespace duckdb
