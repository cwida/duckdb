//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/query_result_stream.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/query_result_state.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/main/result_format.hpp"
#include "duckdb/main/result_unit.hpp"

#include <functional>

namespace duckdb {

//! Whether a reader that picks the consumption mode on its consumer's behalf streams a statement: only a SELECT that
//! returns rows does. written_type is the statement as written, since a prepared execution reports the EXECUTE that
//! runs it; the result's properties are read only for a SELECT, since a statement that expands into several has none
//! before it runs. A reader that may stop reading also needs a statement that writes nothing, since a statement
//! abandoned before its end is rolled back
DUCKDB_API bool StreamsByDefault(StatementType written_type, const QueryResult &result, bool reads_to_the_end = true);

//! Opening a stream settles retention on draining and consumes the handle: no random access
class ResultStreamBase {
public:
	//! Opens a stream on a submitted query. Throws InvalidInputException when the handle carries an
	//! error, when its retention is already retained, or when the result's format is not the expected one.
	//! A throw consumes the handle: it is destroyed with this object, which ends the query
	DUCKDB_API ResultStreamBase(unique_ptr<QueryResult> result, const char *expected_format);
	DUCKDB_API virtual ~ResultStreamBase();
	ResultStreamBase(const ResultStreamBase &) = delete;
	ResultStreamBase &operator=(const ResultStreamBase &) = delete;

public:
	//! Reports READY while a unit is poppable, else where execution stands. Runs no task. An interrupt
	//! or an execution error is recorded on the stream and reported as EXECUTION_ERROR. Like ExecuteTask, it does
	//! not end the query at FINISHED; reading the end of the stream does
	DUCKDB_API QueryResultState Poll();
	//! Executes a single task of the query on the calling thread. An interrupt or an execution error
	//! is recorded on the stream and reported as EXECUTION_ERROR
	DUCKDB_API QueryResultState ExecuteTask();
	//! Blocks until a task is runnable or the engine is waiting on the caller. Runs no task
	DUCKDB_API void WaitForTask();
	//! Ends the stream. Idempotent; the destructor calls it
	DUCKDB_API void Close();
	//! Whether this stream is still the connection's open result
	DUCKDB_API bool IsOpen();
	//! Whether the result's schema and properties are known. Runs nothing
	DUCKDB_API bool MetadataAvailable() const;

	DUCKDB_API void SetError(ErrorData error);
	DUCKDB_API bool HasError() const;
	DUCKDB_API const string &GetError() const;
	DUCKDB_API const ErrorData &GetErrorObject() const;
	DUCKDB_API const ExceptionType &GetErrorType() const;

	DUCKDB_API const vector<LogicalType> &GetTypes() const;
	DUCKDB_API const vector<Identifier> &GetNames() const;
	DUCKDB_API const Identifier &ColumnName(idx_t index) const;
	DUCKDB_API idx_t ColumnCount() const;
	DUCKDB_API StatementType GetStatementType() const;
	DUCKDB_API const StatementProperties &GetStatementProperties() const;
	DUCKDB_API const ClientProperties &GetClientProperties() const;
	DUCKDB_API ClientProperties &GetClientProperties();

	//! Test hook: the buffer this stream drains
	BufferedData &GetBufferedData() {
		return handle->GetBufferedData();
	}

protected:
	DUCKDB_API unique_ptr<ResultUnit> FetchUnit();
	DUCKDB_API QueryResultState TryFetchUnit(unique_ptr<ResultUnit> &out_unit);
	DUCKDB_API const ResultFormatGlobalState &FormatStateInternal() const;

private:
	unique_ptr<ResultUnit> FetchUnitInternal(ClientContextLock &lock);
	//! Before the statement producing the result of a statement that expands into several started, the stream
	//! drives the statements before it through the query
	QueryResultState DriveWithoutBufferInternal(ClientContextLock &lock, bool execute_task);
	//! Runs a buffer call under the context lock and maps any failure onto the stream as
	//! EXECUTION_ERROR. Once the stream has ended it keeps reporting the terminal state
	QueryResultState GuardedInternal(const char *name,
	                                 const std::function<QueryResultState(ClientContextLock &lock)> &call);

private:
	//! The handle of the query this stream drains. Never handed out
	unique_ptr<QueryResult> handle;
};

template <class FORMAT = ChunkFormat>
class QueryResultStream : public ResultStreamBase {
public:
	explicit QueryResultStream(unique_ptr<QueryResult> result) : ResultStreamBase(std::move(result), FORMAT::NAME) {
	}

public:
	//! Pops a payload when one is observable, else reports where execution stands. Runs no task:
	//! payloads are produced by worker threads or by participating calls such as Fetch. After the end
	//! of the stream the terminal state keeps repeating
	QueryResultState TryFetch(unique_ptr<typename FORMAT::T> &out) {
		unique_ptr<ResultUnit> unit;
		auto state = TryFetchUnit(unit);
		out = FORMAT::UnpackUnit(std::move(unit));
		return state;
	}
	//! Runs tasks on the calling thread until a payload is buffered or the stream ends. Returns null
	//! at the end of the stream, and on an execution error, which is recorded on the stream. After a
	//! clean end it keeps returning null; after an error it throws
	unique_ptr<typename FORMAT::T> Fetch() {
		return FORMAT::UnpackUnit(FetchUnit());
	}
	const typename FORMAT::GlobalState &FormatState() const {
		return FormatStateInternal().template Cast<typename FORMAT::GlobalState>();
	}
};

} // namespace duckdb
