//===----------------------------------------------------------------------===//
//                         DuckDB
//
// parquet_int96.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"

namespace duckdb {

class ClientContext;
class Expression;

//! Builds an expression that converts a raw 12-byte INT96 value (read as BLOB) into
//! STRUCT(date DATE, time TIME_NS). Used for the int96_as='struct' read option.
unique_ptr<Expression> CreateInt96AsStructExpression(ClientContext &context);

//! Builds an expression that returns the child at child_index of the struct conversion
//! (0 = date, 1 = time). Used for pushdown struct extracts on the int96_as='struct' column.
unique_ptr<Expression> CreateInt96AsStructChildExpression(ClientContext &context, idx_t child_index);

} // namespace duckdb
