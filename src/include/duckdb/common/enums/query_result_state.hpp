//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/enums/query_result_state.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"

namespace duckdb {

//! Where a submitted query stands. READY means the engine is waiting on the consumer: producers are
//! parked for a retention decision, or parked for buffer space with a chunk to pop
enum class QueryResultState : uint8_t { READY, NOT_READY, BLOCKED, NO_TASKS_AVAILABLE, FINISHED, EXECUTION_ERROR };

//! Whether execution reached a terminal state
constexpr bool IsTerminal(QueryResultState state) {
	return state == QueryResultState::FINISHED || state == QueryResultState::EXECUTION_ERROR;
}

//! Whether the result is ready to be observed by a consumer. Execution is in ready state or in a final state.
constexpr bool IsObservable(QueryResultState state) {
	return IsTerminal(state) || state == QueryResultState::READY;
}

} // namespace duckdb
