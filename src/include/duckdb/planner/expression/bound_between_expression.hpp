//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression/bound_between_expression.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/expression.hpp"

namespace duckdb {
class BoundFunctionExpression;

//! The BoundBetweenExpression has been moved to a function expression
//! This class exists only as a set of helper methods
struct BoundBetweenExpression {
	static unique_ptr<Expression> Create(unique_ptr<Expression> input, unique_ptr<Expression> lower,
	                                     unique_ptr<Expression> upper, bool lower_inclusive, bool upper_inclusive);

	static bool LowerInclusive(const BoundFunctionExpression &between_expr);
	static bool UpperInclusive(const BoundFunctionExpression &between_expr);
	//! Whether the expression contains the bind data owned by the built-in BETWEEN implementation
	static bool HasValidBindData(const BoundFunctionExpression &between_expr);

	static const Expression &Input(const BoundFunctionExpression &between_expr);
	static const Expression &LowerBound(const BoundFunctionExpression &between_expr);
	static const Expression &UpperBound(const BoundFunctionExpression &between_expr);

	static unique_ptr<Expression> &InputMutable(BoundFunctionExpression &between_expr);
	static unique_ptr<Expression> &LowerBoundMutable(BoundFunctionExpression &between_expr);
	static unique_ptr<Expression> &UpperBoundMutable(BoundFunctionExpression &between_expr);

	static ExpressionType LowerComparisonType(const BoundFunctionExpression &between_expr) {
		return LowerInclusive(between_expr) ? ExpressionType::COMPARE_GREATERTHANOREQUALTO
		                                    : ExpressionType::COMPARE_GREATERTHAN;
	}
	static ExpressionType UpperComparisonType(const BoundFunctionExpression &between_expr) {
		return UpperInclusive(between_expr) ? ExpressionType::COMPARE_LESSTHANOREQUALTO
		                                    : ExpressionType::COMPARE_LESSTHAN;
	}
};
} // namespace duckdb
