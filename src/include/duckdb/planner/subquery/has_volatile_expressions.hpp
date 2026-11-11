//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/subquery/has_volatile_expressions.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

namespace duckdb {
class LogicalOperator;

//! Check subquery volatility, excluding the built-in error() but including its arguments.
bool HasVolatileExpressionsIgnoringError(const LogicalOperator &op);

} // namespace duckdb
