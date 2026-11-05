//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/enums/debug_progress_verification.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"
#include "duckdb/common/windows_undefs.hpp"

namespace duckdb {

//! How operator progress reporting is verified (debug setting)
enum class DebugProgressVerification : uint8_t {
	//! No verification (default)
	NONE = 0,
	//! Violations are written to the log (log type "ProgressVerification")
	LOG = 1,
	//! Violations that are not ignored fail the query
	ERROR = 2
};

} // namespace duckdb
