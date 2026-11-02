//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/enums/constraint_check_mode.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"

namespace duckdb {

enum class ConstraintCheckMode : uint8_t { DEFAULT, IMMEDIATE, DEFERRED };

} // namespace duckdb
