//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/named_parameter_map.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/common/insertion_order_preserving_map.hpp"
#include "duckdb/common/types.hpp"
namespace duckdb {

using named_parameter_type_map_t = identifier_map_t<LogicalType>;
using named_parameter_map_t = identifier_map_t<Value>;
//! The named arguments of a call, in the order they were passed
using named_argument_map_t = InsertionOrderPreservingMap<Value, Identifier, identifier_map_t<idx_t>>;

} // namespace duckdb
