//===----------------------------------------------------------------------===//
//
//                         DuckDB
//
// capi_tester.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "catch.hpp"
#include "duckdb.h"
#include "test_helpers.hpp"
#include "duckdb/common/arrow/arrow.hpp"
#include "duckdb/common/exception.hpp"

namespace duckdb {

class CAPIDataChunk {
public:
	CAPIDataChunk(duckdb_data_chunk chunk_p) : chunk(chunk_p) {
	}
	~CAPIDataChunk() {
		duckdb_destroy_data_chunk(&chunk);
	}

	idx_t ColumnCount() {
		return duckdb_data_chunk_get_column_count(chunk);
	}
	idx_t size() {
		return duckdb_data_chunk_get_size(chunk);
	}
	duckdb_vector GetVector(idx_t col) {
		return duckdb_data_chunk_get_vector(chunk, col);
	}
	void *GetData(idx_t col) {
		return duckdb_vector_get_data(GetVector(col));
	}
	uint64_t *GetValidity(idx_t col) {
		return duckdb_vector_get_validity(GetVector(col));
	}
	duckdb_vector GetListChildVector(idx_t col) {
		return duckdb_list_vector_get_child(GetVector(col));
	}
	void *GetListChildData(idx_t col) {
		return duckdb_vector_get_data(GetListChildVector(col));
	}
	duckdb_vector GetStructChildVector(idx_t col, idx_t idx) {
		return duckdb_struct_vector_get_child(GetVector(col), idx);
	}
	void *GetStructChildData(idx_t col, idx_t idx) {
		return duckdb_vector_get_data(GetStructChildVector(col, idx));
	}
	duckdb_vector GetArrayChildVector(idx_t col) {
		return duckdb_array_vector_get_child(GetVector(col));
	}
	void *GetArrayChildData(idx_t col) {
		return duckdb_vector_get_data(GetArrayChildVector(col));
	}
	duckdb_data_chunk GetChunk() {
		return chunk;
	}

private:
	duckdb_data_chunk chunk;
};

class CAPIResult {
public:
	CAPIResult() {
	}
	CAPIResult(duckdb_result result, bool success) : success(success), result(result) {
	}
	~CAPIResult() {
		duckdb_destroy_result(&result);
	}

public:
	bool HasError() const {
		return !success;
	}
	void Query(duckdb_connection connection, string query) {
		success = (duckdb_query(connection, query.c_str(), &result) == DuckDBSuccess);
		if (!success) {
			REQUIRE(ErrorMessage() != nullptr);
			REQUIRE(ErrorType() != DUCKDB_ERROR_INVALID);
		}
	}
	void QueryPrepared(duckdb_prepared_statement statement) {
		success = duckdb_execute_prepared(statement, &result) == DuckDBSuccess;
		if (!success) {
			REQUIRE(ErrorMessage() != nullptr);
			REQUIRE(ErrorType() != DUCKDB_ERROR_INVALID);
		}
	}

	duckdb_type ColumnType(idx_t col) {
		return duckdb_column_type(&result, col);
	}

	idx_t ChunkCount() {
		return duckdb_result_chunk_count(result);
	}

	unique_ptr<CAPIDataChunk> StreamChunk() {
		auto chunk = duckdb_stream_fetch_chunk(result);
		if (!chunk) {
			return nullptr;
		}
		return make_uniq<CAPIDataChunk>(chunk);
	}

	bool IsStreaming() {
		return duckdb_result_is_streaming(result);
	}

	unique_ptr<CAPIDataChunk> FetchChunk(idx_t chunk_idx) {
		auto chunk = duckdb_result_get_chunk(result, chunk_idx);
		if (!chunk) {
			return nullptr;
		}
		return make_uniq<CAPIDataChunk>(chunk);
	}

	unique_ptr<CAPIDataChunk> NextChunk() {
		auto chunk = duckdb_fetch_chunk(result);
		if (!chunk) {
			return nullptr;
		}
		return make_uniq<CAPIDataChunk>(chunk);
	}

	template <class T>
	T *ColumnData(idx_t col) {
		return (T *)duckdb_column_data(&result, col);
	}

	idx_t ColumnCount() {
		return duckdb_column_count(&result);
	}

	idx_t row_count() {
		return duckdb_row_count(&result);
	}

	idx_t rows_changed() {
		return duckdb_rows_changed(&result);
	}

	template <class T>
	T Fetch(idx_t col, idx_t row) {
		throw NotImplementedException("Unimplemented type for fetch");
	}

	bool IsNull(idx_t col, idx_t row) {
		auto nullmask_ptr = duckdb_nullmask_data(&result, col);
		REQUIRE(duckdb_value_is_null(&result, col, row) == nullmask_ptr[row]);
		return nullmask_ptr[row];
	}

	const char *ErrorMessage() {
		return duckdb_result_error(&result);
	}
	duckdb_error_type ErrorType() {
		return duckdb_result_error_type(&result);
	}

	string ColumnName(idx_t col) {
		auto colname = duckdb_column_name(&result, col);
		return colname ? string(colname) : string();
	}

	duckdb_result &InternalResult() {
		return result;
	}

public:
	bool success = false;

protected:
	duckdb_result result;
};

template <>
bool CAPIResult::Fetch(idx_t col, idx_t row);
template <>
int8_t CAPIResult::Fetch(idx_t col, idx_t row);
template <>
int16_t CAPIResult::Fetch(idx_t col, idx_t row);
template <>
int32_t CAPIResult::Fetch(idx_t col, idx_t row);
template <>
int64_t CAPIResult::Fetch(idx_t col, idx_t row);
template <>
uint8_t CAPIResult::Fetch(idx_t col, idx_t row);
template <>
uint16_t CAPIResult::Fetch(idx_t col, idx_t row);
template <>
uint32_t CAPIResult::Fetch(idx_t col, idx_t row);
template <>
uint64_t CAPIResult::Fetch(idx_t col, idx_t row);
template <>
float CAPIResult::Fetch(idx_t col, idx_t row);
template <>
double CAPIResult::Fetch(idx_t col, idx_t row);
template <>
duckdb_decimal CAPIResult::Fetch(idx_t col, idx_t row);
template <>
duckdb_date CAPIResult::Fetch(idx_t col, idx_t row);
template <>
duckdb_time CAPIResult::Fetch(idx_t col, idx_t row);
template <>
duckdb_time_ns CAPIResult::Fetch(idx_t col, idx_t row);
template <>
duckdb_timestamp CAPIResult::Fetch(idx_t col, idx_t row);
template <>
duckdb_timestamp_s CAPIResult::Fetch(idx_t col, idx_t row);
template <>
duckdb_timestamp_ms CAPIResult::Fetch(idx_t col, idx_t row);
template <>
duckdb_timestamp_ns CAPIResult::Fetch(idx_t col, idx_t row);
template <>
duckdb_interval CAPIResult::Fetch(idx_t col, idx_t row);
template <>
duckdb_blob CAPIResult::Fetch(idx_t col, idx_t row);
template <>
string CAPIResult::Fetch(idx_t col, idx_t row);
template <>
duckdb_date_struct CAPIResult::Fetch(idx_t col, idx_t row);
template <>
duckdb_time_struct CAPIResult::Fetch(idx_t col, idx_t row);
template <>
duckdb_timestamp_struct CAPIResult::Fetch(idx_t col, idx_t row);
template <>
duckdb_hugeint CAPIResult::Fetch(idx_t col, idx_t row);
template <>
duckdb_uhugeint CAPIResult::Fetch(idx_t col, idx_t row);

class CAPITester {
public:
	CAPITester() : database(nullptr), connection(nullptr) {
	}
	~CAPITester() {
		Cleanup();
	}

	void Cleanup() {
		if (connection) {
			duckdb_disconnect(&connection);
			connection = nullptr;
		}
		if (database) {
			duckdb_close(&database);
			database = nullptr;
		}
	}

	bool OpenDatabase(const char *path) {
		Cleanup();
		if (duckdb_open(path, &database) != DuckDBSuccess) {
			return false;
		}
		return duckdb_connect(database, &connection) == DuckDBSuccess;
	}

	bool ChangeConnection() {
		duckdb_disconnect(&connection);
		return duckdb_connect(database, &connection) == DuckDBSuccess;
	}

	duckdb::unique_ptr<CAPIResult> Query(string query) {
		D_ASSERT(connection);
		auto result = make_uniq<CAPIResult>();
		result->Query(connection, query);
		return result;
	}
	duckdb::unique_ptr<CAPIResult> QueryPrepared(duckdb_prepared_statement prepared) {
		D_ASSERT(connection);
		auto result = make_uniq<CAPIResult>();
		result->QueryPrepared(prepared);
		return result;
	}

	duckdb_database database = nullptr;
	duckdb_connection connection = nullptr;
};

struct CAPIPrepared {
	CAPIPrepared() {
	}
	~CAPIPrepared() {
		if (!prepared) {
			return;
		}
		duckdb_destroy_prepare(&prepared);
	}

	bool Prepare(CAPITester &tester, const string &query) {
		auto state = duckdb_prepare(tester.connection, query.c_str(), &prepared);
		return state == DuckDBSuccess;
	}

	duckdb_prepared_statement prepared = nullptr;
};

struct CAPIPending {
	CAPIPending() {
	}
	~CAPIPending() {
		if (!pending) {
			return;
		}
		duckdb_destroy_pending(&pending);
	}

	bool Pending(CAPIPrepared &prepared) {
		auto state = duckdb_pending_prepared(prepared.prepared, &pending);
		return state == DuckDBSuccess;
	}

	bool PendingStreaming(CAPIPrepared &prepared) {
		auto state = duckdb_pending_prepared_streaming(prepared.prepared, &pending);
		return state == DuckDBSuccess;
	}

	duckdb_pending_state ExecuteTask() {
		REQUIRE(pending);
		return duckdb_pending_execute_task(pending);
	}

	duckdb_pending_state CheckState() {
		REQUIRE(pending);
		return duckdb_pending_execute_check_state(pending);
	}

	unique_ptr<CAPIResult> Execute() {
		duckdb_result result;
		auto success = duckdb_execute_pending(pending, &result) == DuckDBSuccess;
		return make_uniq<CAPIResult>(result, success);
	}

	duckdb_pending_result pending = nullptr;
};

} // namespace duckdb

bool NO_FAIL(duckdb::CAPIResult &result);
bool NO_FAIL(duckdb::unique_ptr<duckdb::CAPIResult> result);
