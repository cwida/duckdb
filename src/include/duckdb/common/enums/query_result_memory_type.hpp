//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/enums/query_result_memory_type.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"

namespace duckdb {

//! Where a retained result keeps its rows
enum class QueryResultMemoryType : uint8_t { IN_MEMORY, BUFFER_MANAGED };

} // namespace duckdb
