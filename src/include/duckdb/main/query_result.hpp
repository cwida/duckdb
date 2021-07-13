//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/query_result.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/statement_type.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/winapi.hpp"

struct ArrowSchema;

namespace duckdb {

enum class QueryResultType : uint8_t { MATERIALIZED_RESULT, STREAM_RESULT };

//! The QueryResult object holds the result of a query. It can either be a MaterializedQueryResult, in which case the
//! result contains the entire result set, or a StreamQueryResult in which case the Fetch method can be called to
//! incrementally fetch data from the database.
class QueryResult {
public:
	//! Creates an successful empty query result
	DUCKDB_API QueryResult(QueryResultType type, StatementType statement_type);
	//! Creates a successful query result with the specified names and types
	DUCKDB_API QueryResult(QueryResultType type, StatementType statement_type, vector<LogicalType> types,
	                       vector<string> names);
	//! Creates an unsuccessful query result with error condition
	DUCKDB_API QueryResult(QueryResultType type, string error);
	DUCKDB_API virtual ~QueryResult() {
	}

	//! The type of the result (MATERIALIZED or STREAMING)
	QueryResultType type;
	//! The type of the statement that created this result
	StatementType statement_type;
	//! The SQL types of the result
	vector<LogicalType> types;
	//! The names of the result
	vector<string> names;
	//! Whether or not execution was successful
	bool success;
	//! The error string (in case execution was not successful)
	string error;
	//! The next result (if any)
	unique_ptr<QueryResult> next;

public:
	//! Fetches a DataChunk of normalized (flat) vectors from the query result.
	//! Returns nullptr if there are no more results to fetch.
	DUCKDB_API virtual unique_ptr<DataChunk> Fetch();
	//! Fetches a DataChunk from the query result. The vector types
	DUCKDB_API virtual unique_ptr<DataChunk> FetchRaw() = 0;
	// Converts the QueryResult to a string
	DUCKDB_API virtual string ToString() = 0;
	//! Prints the QueryResult to the console
	DUCKDB_API void Print();
	//! Returns true if the two results are identical; false otherwise. Note that this method is destructive; it calls
	//! Fetch() until both results are exhausted. The data in the results will be lost.
	DUCKDB_API bool Equals(QueryResult &other);

	DUCKDB_API idx_t ColumnCount() {
		return types.size();
	}

	DUCKDB_API bool TryFetch(unique_ptr<DataChunk> &result, string &error) {
		try {
			result = Fetch();
			return success;
		} catch (std::exception &ex) {
			error = ex.what();
			return false;
		} catch (...) {
			error = "Unknown error in Fetch";
			return false;
		}
	}

	DUCKDB_API void ToArrowSchema(ArrowSchema *out_array);

private:
	//! The current chunk used by the iterator
	unique_ptr<DataChunk> iterator_chunk;

private:
	class QueryResultIterator;
	class QueryResultRow {
	public:
		explicit QueryResultRow(QueryResultIterator &iterator) : iterator(iterator), row(0) {
		}

		QueryResultIterator &iterator;
		idx_t row;

		template <class T>
		T GetValue(idx_t col_idx) const {
			return iterator.result->iterator_chunk->GetValue(col_idx, iterator.row_idx).GetValue<T>();
		}
	};
	//! The row-based query result iterator. Invoking the
	class QueryResultIterator {
	public:
		explicit QueryResultIterator(QueryResult *result) : current_row(*this), result(result), row_idx(0) {
			if (result) {
				result->iterator_chunk = result->Fetch();
			}
		}

		QueryResultRow current_row;
		QueryResult *result;
		idx_t row_idx;

	public:
		void Next() {
			if (!result->iterator_chunk) {
				return;
			}
			current_row.row++;
			row_idx++;
			if (row_idx >= result->iterator_chunk->size()) {
				result->iterator_chunk = result->Fetch();
				row_idx = 0;
			}
		}

		QueryResultIterator &operator++() {
			Next();
			return *this;
		}
		bool operator!=(const QueryResultIterator &other) const {
			return result->iterator_chunk && result->iterator_chunk->ColumnCount() > 0;
		}
		const QueryResultRow &operator*() const {
			return current_row;
		}
	};

public:
	DUCKDB_API QueryResultIterator begin() {
		return QueryResultIterator(this);
	}
	DUCKDB_API QueryResultIterator end() {
		return QueryResultIterator(nullptr);
	}

protected:
	DUCKDB_API string HeaderToString();

private:
	QueryResult(const QueryResult &) = delete;
};

} // namespace duckdb