//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression/expression_barrier.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/expression.hpp"

namespace duckdb {

//! __internal_barrier(x) returns x, but marks the expression as having an effect that is observable from outside the
//! query - it may throw an error, or it may have side effects. The optimizer treats it as a barrier: a barred
//! expression is never moved to a position where it would be evaluated on rows that the operators around it filter
//! out, and it is always evaluated after the filters that sit next to it.
struct ExpressionBarrier {
	//! Whether this expression node is a barrier
	static bool IsBarrier(const Expression &expr);
	//! Whether the expression tree contains a barrier
	static bool Contains(const Expression &expr);
	//! Whether evaluating this expression is observable from the outside, and it must therefore be wrapped in a
	//! barrier before it can be pushed into a secure view
	static bool Required(const Expression &expr);
	//! Wrap the given expression in a barrier. The level is the secure-view depth at which the expression originated:
	//! 0 for the caller, one more for every secure view the expression is defined inside of. Barriers in one filter
	//! are evaluated deepest level first, so that a view's predicate never runs after a caller's. The level lives in
	//! the barrier's bind data and cannot be written in SQL
	static unique_ptr<Expression> Wrap(unique_ptr<Expression> expr, idx_t level);
	//! Wrap the given expression in a barrier that has no level yet; filter pushdown gives it the level of the
	//! filter it ends up in
	static unique_ptr<Expression> Wrap(unique_ptr<Expression> expr);
	//! Give every barrier in the tree that has no level yet the given level
	static void SetLevel(Expression &expr, idx_t level);
	//! The level a barred expression is evaluated at: the lowest level of the barriers it contains
	static idx_t Level(const Expression &expr);
};

} // namespace duckdb
