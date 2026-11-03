//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/subquery/rewrite_correlated_expressions.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

struct CorrelatedAggregateReplacement {
	ColumnBinding marker;
	ColumnBinding empty_result;
};

//! Rewrites correlated expressions through converted algebra, stopping at unconverted dependent-join children.
class RewriteCorrelatedExpressions : public LogicalOperatorVisitor {
public:
	static void Rewrite(LogicalOperator &op, column_binding_map_t<ColumnBinding> current_binding_map,
	                    column_binding_map_t<ColumnBinding> &correlated_aliases);

private:
	RewriteCorrelatedExpressions(column_binding_map_t<ColumnBinding> current_binding_map,
	                             column_binding_map_t<ColumnBinding> &correlated_aliases);
	void RegisterCorrelatedBinding(const ColumnBinding &source_binding, const ColumnBinding &target_binding);
	void VisitOperator(LogicalOperator &op) override;
	unique_ptr<Expression> VisitReplace(BoundColumnRefExpression &expr, unique_ptr<Expression> *expr_ptr) override;

	column_binding_map_t<ColumnBinding> current_binding_map;
	column_binding_map_t<ColumnBinding> &correlated_aliases;
};

//! Replaces the NULLs introduced for missing aggregate groups with the aggregate's actual empty-input result.
class RewriteCorrelatedAggregates : public LogicalOperatorVisitor {
public:
	static void Rewrite(LogicalOperator &op, column_binding_map_t<CorrelatedAggregateReplacement> &replacement_map);

private:
	explicit RewriteCorrelatedAggregates(column_binding_map_t<CorrelatedAggregateReplacement> &replacement_map);
	unique_ptr<Expression> VisitReplace(BoundColumnRefExpression &expr, unique_ptr<Expression> *expr_ptr) override;
	column_binding_map_t<CorrelatedAggregateReplacement> &replacement_map;
};

} // namespace duckdb
