//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression/bound_comparison_expression.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {
//! The BoundComparisonExpression has been moved to a function expression
//! This class exists only as a set of helper methods
struct BoundComparisonExpression {
	static bool IsComparison(ExpressionType type);
	static bool IsComparison(const Expression &expr);

	static const Expression &Left(const BoundFunctionExpression &comparison_expr);
	static const Expression &Right(const BoundFunctionExpression &comparison_expr);

	static unique_ptr<Expression> &LeftMutable(BoundFunctionExpression &between_expr);
	static unique_ptr<Expression> &RightMutable(BoundFunctionExpression &between_expr);
	static unique_ptr<Expression> Create(ExpressionType type, unique_ptr<Expression> left,
	                                     unique_ptr<Expression> right);
	//! Create a comparison, casting both sides to a common type - throws if they cannot be compared
	static unique_ptr<Expression> Bind(ClientContext &context, ExpressionType type, unique_ptr<Expression> left,
	                                   unique_ptr<Expression> right);

	//! Flip the comparison type, updating both the expression type and the bind data.
	//! Use instead of SetExpressionTypeUnsafe to keep the two in sync.
	static void FlipType(BoundFunctionExpression &comparison_expr);
	//! Set the comparison type, updating both the expression type and the bind data.
	//! Use instead of SetExpressionTypeUnsafe to keep the two in sync.
	static void SetType(BoundFunctionExpression &comparison_expr, ExpressionType new_type);

	static LogicalType BindComparison(ClientContext &context, const LogicalType &left_type,
	                                  const LogicalType &right_type, ExpressionType comparison_type);
	static bool TryBindComparison(ClientContext &context, const LogicalType &left_type, const LogicalType &right_type,
	                              LogicalType &result_type, ExpressionType comparison_type);
};
} // namespace duckdb
