#include "capi_tester.hpp"
#include "duckdb.h"

using namespace duckdb;

TEST_CASE("Test streaming results in C API", "[capi]") {
	CAPITester tester;
	CAPIPrepared prepared;
	CAPIPending pending;
	duckdb::unique_ptr<CAPIResult> result;

	// open the database in in-memory mode
	REQUIRE(tester.OpenDatabase(nullptr));
	REQUIRE(prepared.Prepare(tester, "SELECT i::UINT32 FROM range(1000000) tbl(i)"));
	REQUIRE(pending.PendingStreaming(prepared));

	while (true) {
		auto state = pending.ExecuteTask();
		REQUIRE(state != DUCKDB_PENDING_ERROR);
		if (state == DUCKDB_PENDING_RESULT_READY) {
			break;
		}
	}

	result = pending.Execute();
	REQUIRE(result);
	REQUIRE(!result->HasError());
	auto chunk = result->StreamChunk();

	idx_t value = duckdb::DConstants::INVALID_INDEX;
	idx_t result_count = 0;
	while (chunk) {
		auto old_value = value;

		auto vector = chunk->GetVector(0);
		uint32_t *data = (uint32_t *)duckdb_vector_get_data(vector);
		value = data[0];
		if (old_value != duckdb::DConstants::INVALID_INDEX) {
			// We select from a range, so we can expect every starting value of a new chunk to be higher than the last
			// one.
			REQUIRE(value > old_value);
		}
		REQUIRE(chunk->size() > 0);
		result_count += chunk->size();
		REQUIRE(result_count <= 1000000);
		chunk = result->StreamChunk();
	}
}

TEST_CASE("Test other methods on streaming results in C API", "[capi]") {
	CAPITester tester;
	CAPIPrepared prepared;
	CAPIPending pending;
	duckdb::unique_ptr<CAPIResult> result;

	// open the database in in-memory mode
	REQUIRE(tester.OpenDatabase(nullptr));
	REQUIRE(prepared.Prepare(tester, "SELECT i::UINT32 FROM range(1000000) tbl(i)"));
	REQUIRE(pending.PendingStreaming(prepared));

	while (true) {
		auto state = pending.ExecuteTask();
		REQUIRE(state != DUCKDB_PENDING_ERROR);
		if (state == DUCKDB_PENDING_RESULT_READY) {
			break;
		}
	}

	// Once we've done this, the stream is opened
	result = pending.Execute();
	REQUIRE(result);
	REQUIRE(!result->HasError());
	REQUIRE(result->IsStreaming());

	// interrogate the result with various methods
	auto chunk_count = result->ChunkCount();
	REQUIRE(chunk_count == 0);
	auto column_count = result->ColumnCount();
	(void)column_count;
	auto column_name = result->ColumnName(0);
	(void)column_name;
	auto column_type = result->ColumnType(0);
	(void)column_type;
	auto error_message = result->ErrorMessage();
	REQUIRE(error_message == nullptr);
	auto fetched_chunk = result->FetchChunk(0);
	REQUIRE(fetched_chunk == nullptr);
	auto has_error = result->HasError();
	REQUIRE(has_error == false);
	auto row_count = result->row_count();
	REQUIRE(row_count == 0);
	auto rows_changed = result->rows_changed();
	REQUIRE(rows_changed == 0);

	// this succeeds because the result is materialized if a stream-result method hasn't being used yet
	auto column_data = result->ColumnData<uint32_t>(0);
	REQUIRE(column_data != nullptr);

	// this materializes the result
	auto is_null = result->IsNull(0, 0);
	REQUIRE(is_null == false);
}

TEST_CASE("Test streaming arrow results in C API", "[capi][arrow]") {
	CAPITester tester;
	CAPIPrepared prepared;
	CAPIPending pending;
	duckdb::unique_ptr<CAPIResult> result;

	// open the database in in-memory mode
	REQUIRE(tester.OpenDatabase(nullptr));
	REQUIRE(prepared.Prepare(tester, "SELECT i::UINT32 FROM range(1000000) tbl(i)"));
	REQUIRE(pending.PendingStreaming(prepared));

	while (true) {
		auto state = pending.ExecuteTask();
		REQUIRE(state != DUCKDB_PENDING_ERROR);
		if (state == DUCKDB_PENDING_RESULT_READY) {
			break;
		}
	}

	result = pending.Execute();
	REQUIRE(result);
	REQUIRE(!result->HasError());
	auto chunk = result->StreamChunk();

	// Check handle null out_array
	duckdb_result_arrow_array(result->InternalResult(), chunk->GetChunk(), nullptr);

	int nb_row = 0;
	while (chunk) {
		ArrowArray *arrow_array = new ArrowArray();
		duckdb_result_arrow_array(result->InternalResult(), chunk->GetChunk(), (duckdb_arrow_array *)&arrow_array);
		nb_row += arrow_array->length;
		chunk = result->StreamChunk();
		arrow_array->release(arrow_array);
		delete arrow_array;
	}
	REQUIRE(nb_row == 1000000);
}

TEST_CASE("Test query progress and interrupt in C API", "[capi]") {
	CAPITester tester;
	CAPIPrepared prepared;
	CAPIPending pending;
	duckdb::unique_ptr<CAPIResult> result;

	// test null handling
	REQUIRE(duckdb_query_progress(nullptr).percentage == -1.0);
	duckdb_interrupt(nullptr);

	// open the database in in-memory mode
	REQUIRE(tester.OpenDatabase(nullptr));
	REQUIRE_NO_FAIL(tester.Query("SET threads=1"));
	REQUIRE_NO_FAIL(tester.Query("create table tbl as select range a, mod(range,10) b from range(10000);"));
	REQUIRE_NO_FAIL(tester.Query("create table tbl_2 as select range a from range(10000);"));
	REQUIRE_NO_FAIL(tester.Query("set enable_progress_bar=true;"));
	REQUIRE_NO_FAIL(tester.Query("set enable_progress_bar_print=false;"));
	// test no progress before query
	REQUIRE(duckdb_query_progress(tester.connection).percentage == -1.0);
	// test zero progress with query
	REQUIRE(prepared.Prepare(tester, "select count(*) from tbl where a = (select min(a) from tbl_2)"));
	REQUIRE(pending.PendingStreaming(prepared));
	REQUIRE(duckdb_query_progress(tester.connection).percentage == 0.0);

	// test progress
	while (duckdb_query_progress(tester.connection).percentage == 0.0) {
		auto state = pending.ExecuteTask();
		REQUIRE(state == DUCKDB_PENDING_RESULT_NOT_READY);
	}
	REQUIRE(duckdb_query_progress(tester.connection).percentage >= 0.0);

	// test interrupt
	duckdb_interrupt(tester.connection);
	while (true) {
		auto state = pending.ExecuteTask();
		REQUIRE(state != DUCKDB_PENDING_RESULT_READY);
		if (state == DUCKDB_PENDING_ERROR) {
			break;
		}
	}
}

TEST_CASE("Test destroying a partly read streaming result inside a transaction", "[capi]") {
	CAPITester tester;
	REQUIRE(tester.OpenDatabase(nullptr));
	REQUIRE_NO_FAIL(tester.Query("CREATE SEQUENCE s"));
	REQUIRE_NO_FAIL(tester.Query("CREATE TABLE t AS SELECT range i FROM range(1000000)"));

	string query;
	bool invalidates = false;
	SECTION("a query that writes invalidates the transaction") {
		query = "SELECT nextval('s') FROM range(1000000)";
		invalidates = true;
	}
	SECTION("a read-only query keeps the transaction") {
		query = "SELECT i FROM t";
	}
	REQUIRE_NO_FAIL(tester.Query("BEGIN TRANSACTION"));
	{
		CAPIPrepared prepared;
		CAPIPending pending;
		REQUIRE(prepared.Prepare(tester, query));
		REQUIRE(pending.PendingStreaming(prepared));
		auto result = pending.Execute();
		REQUIRE(result);
		REQUIRE(!result->HasError());
		REQUIRE(result->StreamChunk());
	}

	auto next = tester.Query("SELECT 42");
	REQUIRE(next->HasError() == invalidates);
	if (invalidates) {
		REQUIRE(string(next->ErrorMessage()).find("aborted") != string::npos);
	}
	REQUIRE_NO_FAIL(tester.Query("ROLLBACK"));
}
