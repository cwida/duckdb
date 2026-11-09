//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/logical_operator_visitor.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/planner/bound_tokens.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/planner/logical_tokens.hpp"
#include "duckdb/common/projection_index.hpp"

#include <functional>

namespace duckdb {
//! The LogicalOperatorVisitor is an abstract base class that implements the
//! Visitor pattern on LogicalOperator.
class LogicalOperatorVisitor {
public:
	virtual ~LogicalOperatorVisitor() {
	}

	virtual void VisitOperator(unique_ptr<LogicalOperator> &op);
	virtual void VisitOperator(LogicalOperator &op);
	virtual void VisitExpression(unique_ptr<Expression> *expression);

	static void EnumerateExpressions(LogicalOperator &op,
	                                 const std::function<void(unique_ptr<Expression> *child)> &callback);
	static void EnumerateExpressions(const LogicalOperator &op,
	                                 const std::function<void(const unique_ptr<Expression> *child)> &callback);
	//! Return the projection map owned by an operator for the given child, if any
	static optional_ptr<vector<ProjectionIndex>> GetProjectionMap(LogicalOperator &op, idx_t child_index);
	//! Preserve the selected binding identities after a child rewrite changes its output layout
	static void RemapProjectionMap(vector<ProjectionIndex> &projection_map,
	                               const vector<ColumnBinding> &child_bindings_before,
	                               const vector<ColumnBinding> &child_bindings_after);

protected:
	//! Automatically calls the Visit method for LogicalOperator children of the current operator. Can be overloaded to
	//! change this behavior.
	void VisitOperatorChildren(LogicalOperator &op);
	//! Automatically calls the Visit method for Expression children of the current operator. Can be overloaded to
	//! change this behavior.
	void VisitOperatorExpressions(LogicalOperator &op);
	//! Alternatives for VisitOperatorChildren for operators that have a projection map
	void VisitOperatorWithProjectionMapChildren(LogicalOperator &op);
	void VisitChildOfOperatorWithProjectionMap(unique_ptr<LogicalOperator> &child,
	                                           vector<ProjectionIndex> &projection_map);

	// The VisitExpressionChildren method is called at the end of every call to VisitExpression to recursively visit all
	// expressions in an expression tree. It can be overloaded to prevent automatically visiting the entire tree.
	virtual void VisitExpressionChildren(Expression &expression);

	virtual unique_ptr<Expression> VisitReplace(BoundAggregateExpression &expr, unique_ptr<Expression> *expr_ptr);
	virtual unique_ptr<Expression> VisitReplace(BoundCaseExpression &expr, unique_ptr<Expression> *expr_ptr);
	virtual unique_ptr<Expression> VisitReplace(BoundColumnRefExpression &expr, unique_ptr<Expression> *expr_ptr);
	virtual unique_ptr<Expression> VisitReplace(BoundConjunctionExpression &expr, unique_ptr<Expression> *expr_ptr);
	virtual unique_ptr<Expression> VisitReplace(BoundConstantExpression &expr, unique_ptr<Expression> *expr_ptr);
	virtual unique_ptr<Expression> VisitReplace(BoundDefaultExpression &expr, unique_ptr<Expression> *expr_ptr);
	virtual unique_ptr<Expression> VisitReplace(BoundFunctionExpression &expr, unique_ptr<Expression> *expr_ptr);
	virtual unique_ptr<Expression> VisitReplace(BoundLambdaExpression &expr, unique_ptr<Expression> *expr_ptr);
	virtual unique_ptr<Expression> VisitReplace(BoundOperatorExpression &expr, unique_ptr<Expression> *expr_ptr);
	virtual unique_ptr<Expression> VisitReplace(BoundReferenceExpression &expr, unique_ptr<Expression> *expr_ptr);
	virtual unique_ptr<Expression> VisitReplace(BoundSubqueryExpression &expr, unique_ptr<Expression> *expr_ptr);
	virtual unique_ptr<Expression> VisitReplace(BoundParameterExpression &expr, unique_ptr<Expression> *expr_ptr);
	virtual unique_ptr<Expression> VisitReplace(BoundWindowExpression &expr, unique_ptr<Expression> *expr_ptr);
	virtual unique_ptr<Expression> VisitReplace(BoundUnnestExpression &expr, unique_ptr<Expression> *expr_ptr);
};
} // namespace duckdb
