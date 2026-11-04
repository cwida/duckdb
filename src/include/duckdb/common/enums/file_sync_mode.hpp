//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/enums/file_sync_mode.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"

namespace duckdb {

enum class FileSyncMode : uint8_t { STANDARD, NONE, FULL };

} // namespace duckdb
