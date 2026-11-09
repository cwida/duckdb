//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/column_lifetime_analyzer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/vector.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"

namespace duckdb {

class Optimizer;
class BoundColumnRefExpression;

//! The ColumnLifetimeAnalyzer optimizer traverses the logical operator tree and ensures that columns are removed from
//! the plan when no longer required
class ColumnLifetimeAnalyzer : public LogicalOperatorVisitor {
public:
	explicit ColumnLifetimeAnalyzer(Optimizer &optimizer_p, LogicalOperator &root_p, bool is_root = false)
	    : optimizer(optimizer_p), root(root_p), everything_referenced(is_root) {
	}

	void VisitOperator(LogicalOperator &op) override;

public:
	static void ExtractColumnBindings(const Expression &expr, vector<ColumnBinding> &bindings);

protected:
	unique_ptr<Expression> VisitReplace(BoundColumnRefExpression &expr, unique_ptr<Expression> *expr_ptr) override;
	unique_ptr<Expression> VisitReplace(BoundReferenceExpression &expr, unique_ptr<Expression> *expr_ptr) override;

private:
	Optimizer &optimizer;
	LogicalOperator &root;
	//! Whether or not all the columns are referenced. This happens in the case of the root expression (because the
	//! output implicitly refers all the columns below it)
	bool everything_referenced;
	//! The set of column references
	column_binding_set_t column_references;

private:
	void VisitOperatorInternal(LogicalOperator &op);
	void StandardVisitOperator(LogicalOperator &op);
	void ExtractUnusedColumnBindings(const vector<ColumnBinding> &bindings, column_binding_set_t &unused_bindings);
	static void GenerateProjectionMap(vector<ColumnBinding> bindings, column_binding_set_t &unused_bindings,
	                                  vector<ProjectionIndex> &map);
	void Verify(LogicalOperator &op);
	void AddVerificationProjection(unique_ptr<LogicalOperator> &child, vector<ProjectionIndex> &projection_map);
};
} // namespace duckdb
