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
	//! Wrap the given expression in a barrier
	static unique_ptr<Expression> Wrap(unique_ptr<Expression> expr);
};

} // namespace duckdb
