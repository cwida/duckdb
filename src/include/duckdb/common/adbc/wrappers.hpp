//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/adbc/wrappers.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.h"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb_adbc {
struct DuckDBAdbcStreamWrapper;
struct DuckDBAdbcStatementWrapper;
} // namespace duckdb_adbc

namespace duckdb {

struct DuckDBAdbcConnectionWrapper {
	duckdb_connection connection;
	unordered_map<string, string> options;
	//! A cancel and a max_execution_time timeout raise the same exception type, and errors_as_json rewrites the
	//! message that would tell them apart. Cleared when a statement starts executing
	atomic<bool> cancel_requested {false};

	//! Register a stream wrapper so it can be materialized if another query runs on this connection.
	void RegisterStream(duckdb_adbc::DuckDBAdbcStreamWrapper *stream);
	//! Unregister a stream wrapper.
	void UnregisterStream(duckdb_adbc::DuckDBAdbcStreamWrapper *stream);
	//! Materialize all active streams, fetching remaining data into memory.
	void MaterializeStreams();
	//! Ends every open stream on this connection, which then reports `reason` once its materialized arrays are read
	void CloseStreams(const char *reason);
	//! Detach all streams from this connection and clear the list (called on connection release).
	void DetachAndClearStreams();

	//! Numbers the executions on this connection. DuckDB runs one query per connection at a time, so the connection
	//! has a single query progress; `running_execution` says which execution it belongs to, 0 when none is running.
	//! A statement reads that progress only for its own execution, so one that has finished keeps reporting its own
	//! completion rather than the progress of whatever ran next. These are atomics because ADBC requires
	//! AdbcStatementGetOptionDouble to be thread-safe, and it is read while another thread executes.
	atomic<uint64_t> execution_counter {0};
	atomic<uint64_t> running_execution {0};

	//! Take the connection's query progress for a new execution, and return that execution's number (never 0).
	uint64_t BeginExecution() {
		auto execution = ++execution_counter;
		running_execution = execution;
		return execution;
	}
	//! Whether `execution` is the one running on this connection.
	bool IsRunning(uint64_t execution) const {
		return execution != 0 && running_execution == execution;
	}
	//! End `execution`, and record on the statement that ran it whether it completed. Does nothing once the execution
	//! has ended, so the first ending wins and a later execution keeps the progress. `statement` is null for a query
	//! that no statement owns, and is only used while the registry below still holds it.
	void FinishExecution(duckdb_adbc::DuckDBAdbcStatementWrapper *statement, uint64_t execution, bool completed);

	//! The statements created on this connection. A result stream reaches the statement that produced it through this
	//! registry rather than by following a pointer, because a stream can outlive its statement.
	void RegisterStatement(duckdb_adbc::DuckDBAdbcStatementWrapper *statement);
	void UnregisterStatement(duckdb_adbc::DuckDBAdbcStatementWrapper *statement);
	//! Tell the statements that this connection is gone, so they stop reading its query progress.
	void DetachAndClearStatements();

private:
	mutex stream_mutex;
	vector<duckdb_adbc::DuckDBAdbcStreamWrapper *> active_streams;
	mutex statement_mutex;
	vector<duckdb_adbc::DuckDBAdbcStatementWrapper *> statements;
};
} // namespace duckdb
