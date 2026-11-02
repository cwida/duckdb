//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/query_result.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/query_result_state.hpp"
#include "duckdb/common/enums/statement_type.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/winapi.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/main/client_properties.hpp"
#include "duckdb/main/result_format.hpp"
#include "duckdb/main/result_unit.hpp"
#include "duckdb/main/retained_result_collection.hpp"

#include <type_traits>

namespace duckdb {
class BoxRendererContext;
struct BoxRendererConfig;
class BufferedData;
class ClientContext;
class ClientContextLock;
class PreparedStatementData;
class QueryResult;

class BaseQueryResult {
public:
	//! Creates a successful query result with the specified names and types
	DUCKDB_API BaseQueryResult(StatementType statement_type, StatementProperties properties, vector<LogicalType> types,
	                           vector<Identifier> names);
	//! Creates an unsuccessful query result with error condition
	DUCKDB_API explicit BaseQueryResult(ErrorData error);
	DUCKDB_API virtual ~BaseQueryResult();

public:
	//! Whether the result's schema and properties are known. False for a statement that expands into several until
	//! the one producing the result is bound, unless it failed. Runs nothing
	DUCKDB_API bool MetadataAvailable() const;
	//! Returns the type of the statement that created this result
	DUCKDB_API StatementType GetStatementType() const;
	//! Returns the properties of the statement that created this result
	DUCKDB_API const StatementProperties &GetStatementProperties() const;
	//! Returns the SQL types of the result
	DUCKDB_API const vector<LogicalType> &GetTypes() const;
	//! Returns the names of the result
	DUCKDB_API const vector<Identifier> &GetNames() const;
	//! Returns the number of columns in the result
	DUCKDB_API idx_t ColumnCount() const;

	[[noreturn]] DUCKDB_API void ThrowError(const string &prepended_message = "") const;
	DUCKDB_API void ThrowIfError(const string &prepended_message = "") const;
	DUCKDB_API void SetError(ErrorData error);
	DUCKDB_API bool HasError() const;
	DUCKDB_API const ExceptionType &GetErrorType() const;
	DUCKDB_API const std::string &GetError() const;
	DUCKDB_API ErrorData &GetErrorObject();
	DUCKDB_API const ErrorData &GetErrorObject() const;

protected:
	//! Creates a successful result whose metadata the statement producing it will provide
	explicit BaseQueryResult(StatementType statement_type);
	void SetMetadata(StatementProperties properties, vector<LogicalType> types, vector<Identifier> names);
	void RequireMetadata() const;

private:
	//! The type of the statement that created this result
	StatementType statement_type;
	//! Properties of the statement
	StatementProperties properties;
	//! The SQL types of the result
	vector<LogicalType> types;
	//! The names of the result
	vector<Identifier> names;
	bool metadata_available = true;
	//! Whether or not execution was successful
	bool success;
	//! The error (in case execution was not successful)
	ErrorData error;
};

//! A query result. The format is fixed at submission; calling Materialize, Collection, TakeCollection, Fetch or
//! RowCount will materialize the result's data into that format's collection. If instead the caller wants a
//! streaming interface, it can be moved into a QueryResultStream<FORMAT>.
//! An accessor taking FORMAT throws InvalidInputException when FORMAT is not the result's format
class QueryResult : public BaseQueryResult {
	friend class BufferedData;
	friend class ClientContext;
	friend class ResultStreamBase;

public:
	//! Creates the handle of a freshly submitted query
	DUCKDB_API QueryResult(shared_ptr<ClientContext> context, PreparedStatementData &statement,
	                       vector<LogicalType> types, ClientProperties client_properties,
	                       shared_ptr<BufferedData> buffer);
	//! Creates the handle of a freshly submitted statement that expands into several: its metadata and buffer come
	//! from the statement that produces the result, once that is bound
	QueryResult(shared_ptr<ClientContext> context, StatementType statement_type, shared_ptr<ResultFormat> format,
	            ClientProperties client_properties);
	//! Creates a detached result over an existing, finalized collection
	DUCKDB_API QueryResult(StatementType statement_type, StatementProperties properties, vector<Identifier> names,
	                       unique_ptr<ColumnDataCollection> collection, ClientProperties client_properties);
	//! Creates a detached result over an existing, finalized retained collection of any format
	DUCKDB_API QueryResult(StatementType statement_type, StatementProperties properties, vector<LogicalType> types,
	                       vector<Identifier> names, unique_ptr<RetainedResultCollection> collection,
	                       shared_ptr<ResultFormat> format, shared_ptr<ResultFormatGlobalState> format_state,
	                       ClientProperties client_properties);
	//! Creates an unsuccessful query result with error condition
	DUCKDB_API explicit QueryResult(ErrorData error);
	DUCKDB_API ~QueryResult() override;

	//! Properties from the client context
	ClientProperties client_properties;
	//! The next result (if any)
	unique_ptr<QueryResult> next;

public:
	//! Deduplicate column names for interop with external libraries
	static void DeduplicateColumns(vector<Identifier> &names);
	static void DeduplicateColumns(vector<string> &names);

public:
	//! Returns the query's current state. Runs no task; for a statement that expands into several, it starts the
	//! next statement once the running one finished.
	DUCKDB_API QueryResultState Poll();
	//! Executes a single task of the query on the calling thread. Decides nothing: READY means the engine is
	//! waiting for the retention decision, and every further call returns READY, running nothing, until a
	//! stream is opened or a retained-side call (Materialize, Complete, Collection, ...) is made. Unless a
	//! stream drains the result, the call that reports FINISHED ends the query and collects the rows.
	DUCKDB_API QueryResultState ExecuteTask();
	//! Blocks until a task is runnable or the engine is waiting on the caller. Runs no task.
	DUCKDB_API void WaitForTask();
	DUCKDB_API const ResultFormat &Format() const;
	//! Non-blocking. Tells the engine to fully materialize the result. Call Collection(), Fetch[Raw](), or
	//! ExecuteTask() to execute tasks, or (if multithreaded) Poll until the result is complete.
	DUCKDB_API void Materialize();
	//! Blocking. Tells the engine to fully materialize the result. Participates in execution of the query.
	DUCKDB_API void Complete();
	template <class FORMAT = ChunkFormat>
	RetainedPayloadsOf<FORMAT> &Collection() {
		PrepareCollected(FORMAT::NAME);
		return collection->Cast<RetainedCollectionOf<FORMAT>>().Get();
	}
	//! Blocking. Same as Collection() but takes ownership of the collection. The QueryResult is empty afterward.
	template <class FORMAT = ChunkFormat>
	unique_ptr<RetainedPayloadsOf<FORMAT>> TakeCollection() {
		PrepareCollected(FORMAT::NAME);
		auto taken = collection->Cast<RetainedCollectionOf<FORMAT>>().Take();
		collection.reset();
		return taken;
	}
	//! Throws when FORMAT is not the result's format
	template <class FORMAT>
	const typename FORMAT::GlobalState &FormatState() const {
		return CheckedFormatState(FORMAT::NAME).template Cast<typename FORMAT::GlobalState>();
	}
	//! Get the rowcount of the result. Will materialize the full result if it hadn't yet.
	DUCKDB_API idx_t RowCount();
	//! Aborts the query unless a call already ended it: autocommit rolls it back, and inside a transaction a
	//! statement that may write invalidates the transaction. Idempotent.
	DUCKDB_API void Close();
	//! Whether this result is still the connection's open result.
	DUCKDB_API bool IsOpen();

	//! Returns the name of the column for the given index
	DUCKDB_API const Identifier &ColumnName(idx_t index) const;
	//! Copies the next unit, leaving the collection intact, and materializes the result first if needed
	template <class FORMAT = ChunkFormat>
	unique_ptr<typename FORMAT::T> Fetch() {
		if constexpr (std::is_same<FORMAT, ChunkFormat>::value) {
			auto chunk = FetchRaw();
			if (!chunk) {
				return nullptr;
			}
			chunk->Flatten();
			return chunk;
		} else {
			PrepareCollected(FORMAT::NAME);
			return collection->Cast<RetainedCollectionOf<FORMAT>>().Fetch();
		}
	}
	//! Fetches a DataChunk from the query result. The vectors are not normalized and hence any vector types can be
	//! returned. Will materialize the full result into a CDC if it hadn't yet. Chunk format only
	DUCKDB_API unique_ptr<DataChunk> FetchRaw();
	//! Converts the QueryResult to a string
	DUCKDB_API string ToString();
	//! Converts the QueryResult to a box-rendered string
	DUCKDB_API string ToBox(BoxRendererContext &context, const BoxRendererConfig &config);
	//! Prints the QueryResult to the console
	DUCKDB_API void Print();
	//! Returns true if the two results are identical; false otherwise. Note that this method is destructive; it calls
	//! Fetch() until both results are exhausted. The data in the results will be lost.
	DUCKDB_API bool Equals(QueryResult &other, bool compare_names = true);

	bool TryFetchOrError(unique_ptr<DataChunk> &result, ErrorData &error) {
		try {
			result = Fetch();
			return !HasError();
		} catch (std::exception &ex) {
			error = ErrorData(ex);
			return false;
		} catch (...) {
			error = ErrorData("Unknown error in Fetch");
			return false;
		}
	}

	//! False for a detached result and for an error result, which never had a buffer
	bool HasBufferedData() const {
		return buffer != nullptr;
	}
	//! Test helper: the buffer created for this query.
	BufferedData &GetBufferedData() const {
		D_ASSERT(buffer);
		return *buffer;
	}

private:
	unique_ptr<ClientContextLock> LockContext();
	void CheckExecutableInternal(ClientContextLock &lock);
	bool IsOpenInternal(ClientContextLock &lock);
	//! Records that this result is no longer the connection's active query, the way an interrupt is
	//! recorded, and reports it as an error state
	QueryResultState Cancelled();
	void CompleteInternal(ClientContextLock &lock);
	//! Ends the query as failed after an exception escaped a call that holds the context lock, and releases the
	//! context
	void AbandonAfterThrowInternal(ClientContextLock &lock, ErrorData error);
	void SettleRetainedInternal(ClientContextLock &lock);
	//! Ends a query whose retained execution finished and collects its rows
	QueryResultState EndFinishedInternal(ClientContextLock &lock);
	void HandleFetchFailure(ClientContextLock &lock, ErrorData error);
	//! Ends the query and records a commit failure on this result without throwing
	void EndQuery(ClientContextLock &lock, bool invalidate_transaction = false);
	[[noreturn]] void ThrowNoCollection() const;
	bool IsCollected() const {
		return collection != nullptr;
	}
	//! A result without a buffer is in the chunk format, with a state built from its own metadata
	void InitializeChunkFormatState();
	//! Takes the rows only: the handle already holds the format and state its buffer settled at submission
	void AdoptCollected(QueryResult &produced);
	//! Takes the metadata and buffer of the statement that produces the result of a statement that expands
	void AdoptResultStatement(StatementProperties properties, PreparedStatementData &statement,
	                          shared_ptr<BufferedData> buffer);
	//! Takes the metadata, format state and rows of a finished statement that expands into several and returns no
	//! rows
	void AdoptRowlessResult(QueryResult &produced);
	DUCKDB_API void PrepareCollected(const char *expected);
	DUCKDB_API const ResultFormatGlobalState &CheckedFormatState(const char *expected) const;
	[[noreturn]] void ThrowFormatMismatch(const char *expected) const;

private:
	//! The client context this result belongs to. Null once the query has ended
	shared_ptr<ClientContext> context;
	//! The buffer created for this query at submission. It carries the retention decision and, for
	//! a stream, the units (null for a detached or an error result, and for a statement that expands until the one
	//! producing the result has started)
	shared_ptr<BufferedData> buffer;
	//! Set at construction; never null for a non-error result
	shared_ptr<ResultFormat> format;
	//! Never null for a non-error result whose metadata is available
	shared_ptr<ResultFormatGlobalState> format_state;
	//! In the format's own storage, which also holds the fetch cursor
	unique_ptr<RetainedResultCollection> collection;

private:
	class QueryResultIterator;
	class QueryResultRow {
		friend class QueryResultIterator;

	public:
		explicit QueryResultRow() : row(0) {
		}

		bool IsNull(idx_t col_idx) const {
			return chunk->GetValue(col_idx, row).IsNull();
		}
		template <class T>
		T GetValue(idx_t col_idx) const {
			return chunk->GetValue(col_idx, row).GetValue<T>();
		}
		Value GetBaseValue(idx_t col_idx) const {
			return chunk->GetValue(col_idx, row);
		}
		DataChunk &GetChunk() const {
			return *chunk;
		}
		idx_t GetRowInChunk() const {
			return row;
		}

	private:
		shared_ptr<DataChunk> chunk;
		idx_t row;
	};
	//! The row-based query result iterator. Invoking the
	class QueryResultIterator {
	public:
		explicit QueryResultIterator(optional_ptr<QueryResult> result_p = nullptr) : result(result_p), base_row(0) {
			if (result) {
				current_row.chunk = shared_ptr<DataChunk>(result->Fetch().release());
				if (!current_row.chunk) {
					result = nullptr;
				}
			}
		}

		QueryResultRow current_row;
		optional_ptr<QueryResult> result;
		idx_t base_row;

	public:
		void Next() {
			if (!current_row.chunk) {
				return;
			}
			current_row.row++;
			if (current_row.row >= current_row.chunk->size()) {
				base_row += current_row.chunk->size();
				current_row.chunk = shared_ptr<DataChunk>(result->Fetch().release());
				current_row.row = 0;
				if (!current_row.chunk || current_row.chunk->size() == 0) {
					// exhausted all rows
					base_row = 0;
					result = nullptr;
					current_row.chunk.reset();
				}
			}
		}

		QueryResultIterator &operator++() {
			Next();
			return *this;
		}
		bool operator!=(const QueryResultIterator &other) const {
			return result != other.result || base_row != other.base_row || current_row.row != other.current_row.row;
		}
		bool operator==(const QueryResultIterator &other) const {
			return !(*this != other);
		}
		const QueryResultRow &operator*() const {
			return current_row;
		}
	};

public:
	using iterator = QueryResultIterator;

	iterator begin() { // NOLINT: match stl API
		return QueryResultIterator(this);
	}
	iterator end() { // NOLINT: match stl API
		return QueryResultIterator(nullptr);
	}

protected:
	DUCKDB_API string HeaderToString();

private:
	QueryResult(const QueryResult &) = delete;
};

} // namespace duckdb
