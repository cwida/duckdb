//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/enums/checkpoint_abort.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"

namespace duckdb {

enum class CheckpointAbort : uint8_t {
	NO_ABORT = 0,
	DEBUG_ABORT_BEFORE_TRUNCATE = 1,
	DEBUG_ABORT_BEFORE_HEADER = 2,
	DEBUG_ABORT_AFTER_FREE_LIST_WRITE = 3,
	DEBUG_ABORT_BEFORE_WAL_FINISH = 4,
	DEBUG_ABORT_BEFORE_MOVING_RECOVERY = 5,
	DEBUG_ABORT_BEFORE_DELETING_CHECKPOINT_WAL = 6,
	DEBUG_ABORT_BEFORE_HEADER_NON_FATAL = 7,
	DEBUG_ABORT_IN_MEMORY_CHECKPOINT = 8,
	DEBUG_ABORT_BEFORE_WAL_FLUSH = 9
};

} // namespace duckdb
