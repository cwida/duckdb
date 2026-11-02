//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression/bound_cast_expression.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/function/cast/default_casts.hpp"

namespace duckdb {

//! The BoundCastExpression has been moved to a function expression (the __cast scalar function).
//! This class exists only as a set of helper methods.
struct BoundCastExpression {
	//! Whether or not the given expression is a bound cast (a function expression of type OPERATOR_CAST)
	static bool IsCast(const Expression &expr);

	//! Create a bound cast function expression around the given child using the provided bound cast
	static unique_ptr<Expression> Create(unique_ptr<Expression> child, const LogicalType &target_type,
	                                     BoundCastInfo bound_cast, bool try_cast = false);
	//! Create a bound cast function expression that is rebound using only the built-in SQL casts
	static unique_ptr<Expression> CreateDefault(unique_ptr<Expression> child, const LogicalType &target_type,
	                                            BoundCastInfo bound_cast, bool try_cast = false);

	//! The expression that is being cast
	static const Expression &Child(const BoundFunctionExpression &cast_expr);
	static unique_ptr<Expression> &ChildMutable(BoundFunctionExpression &cast_expr);
	//! The type the expression is cast to (equal to the return type of the expression)
	static const LogicalType &TargetType(const BoundFunctionExpression &cast_expr);
	//! The type of the cast source (equal to the return type of the child)
	static LogicalType SourceType(const BoundFunctionExpression &cast_expr);
	//! Whether or not this is a try_cast (cast failures become NULL instead of throwing an error)
	static bool IsTryCast(const BoundFunctionExpression &cast_expr);
	//! Whether the expression contains the bind data owned by the built-in cast implementation
	static bool HasValidBindData(const BoundFunctionExpression &cast_expr);
	//! Whether or not this cast is bound using only the built-in SQL casts
	static bool IsDefaultCast(const BoundFunctionExpression &cast_expr);
	static const BoundCastInfo &GetBoundCast(const BoundFunctionExpression &cast_expr);
	static BoundCastInfo &GetBoundCastMutable(BoundFunctionExpression &cast_expr);
	static unique_ptr<BaseStatistics> PropagateStatistics(BoundFunctionExpression &cast_expr,
	                                                      const BaseStatistics &child_stats,
	                                                      optional_ptr<ClientContext> context = nullptr);

	//! Cast an expression to the specified SQL type, using only the built-in SQL casts
	static unique_ptr<Expression> AddDefaultCastToType(unique_ptr<Expression> expr, const LogicalType &target_type,
	                                                   bool try_cast = false);
	//! Cast an expression to the specified SQL type if required
	DUCKDB_API static unique_ptr<Expression> AddCastToType(ClientContext &context, unique_ptr<Expression> expr,
	                                                       const LogicalType &target_type, bool try_cast = false);

	//! If the expression returns an array, cast it to return a list with the same child type. Otherwise do nothing.
	DUCKDB_API static unique_ptr<Expression> AddArrayCastToList(ClientContext &context, unique_ptr<Expression> expr);

	//! Returns true if a cast is invertible (i.e. CAST(s -> t -> s) = s for all values of s). This is not true for e.g.
	//! boolean casts, because that can be e.g. -1 -> TRUE -> 1. This is necessary to prevent some optimizer bugs.
	static bool CastIsInvertible(const LogicalType &source_type, const LogicalType &target_type);

	//! Returns true if a cast from source_type to target_type can throw a runtime error
	static bool CastCanThrow(const LogicalType &source_type, const LogicalType &target_type, bool try_cast);
};
} // namespace duckdb
