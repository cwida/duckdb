//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/query_parameters.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/identifier.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/planner/expression/bound_parameter_data.hpp"

#include <functional>

namespace duckdb {

class ResultFormat;

struct QueryParameters {
	QueryParameters() = default;
	//! So a caller that wants only a format does not have to spell out the other fields
	explicit QueryParameters(shared_ptr<ResultFormat> format_p) : format(std::move(format_p)) {
	}

	//! Arguments for a parameterized statement (may be null)
	optional_ptr<identifier_map_t<BoundParameterData>> statement_args;
	shared_ptr<ResultFormat> format;
	//! Called when the result's state may have changed, until the query ends, at the latest when its result is closed
	//! or destroyed. If ending the query itself fails for lack of memory, it can still be called after that. It runs on
	//! any thread, possibly while engine locks are held, so it must not block, throw or call into DuckDB, and the
	//! consumer must not hold a lock it takes while calling into DuckDB. Calls carry nothing and may be spurious or
	//! merged: answer each one by polling and acting on READY (fetch from a stream, or decide how an undecided result
	//! is read) until the answer is no longer READY. A statement that runs as several internal statements also calls it
	//! as each one ends, and the next one starts only inside a call of the consumer, such as the poll that answers it.
	//! It is destroyed with the connection locked, so its captures must not call into DuckDB when destroyed. Without
	//! worker threads nothing changes between the consumer's own calls
	std::function<void()> notify_callback;
};

} // namespace duckdb
