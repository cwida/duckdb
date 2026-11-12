//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/partial_aggregate_pushdown.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"

namespace duckdb {
class Optimizer;

//! The PartialAggregatePushdown optimizer pushes partial aggregate states below joins when this can reduce join work
class PartialAggregatePushdown : public LogicalOperatorVisitor {
public:
	explicit PartialAggregatePushdown(Optimizer &optimizer);

	void VisitOperator(unique_ptr<LogicalOperator> &op) override;

private:
	//! Double-eager (Yan & Larson "eager group-by-count"): pre-aggregate BOTH join inputs by the join key and
	//! reconstruct aggregates above by repeating each side's state by the other side's row count.
	bool TryDoubleEagerPushdown(unique_ptr<LogicalOperator> &op);
	//! Pre-aggregate count on the non-preserved side of a LEFT or RIGHT join.
	bool TryOwnerOuterCountPushdown(unique_ptr<LogicalOperator> &op);
	//! One-sided eager group-by: push a partial aggregate (via state export) below the join on one side only.
	bool TryPushdownAggregate(unique_ptr<LogicalOperator> &op);
	unique_ptr<Expression> VisitReplace(BoundColumnRefExpression &expr, unique_ptr<Expression> *expr_ptr) override;

private:
	Optimizer &optimizer;
	column_binding_map_t<ColumnBinding> replacement_map;
};
} // namespace duckdb
