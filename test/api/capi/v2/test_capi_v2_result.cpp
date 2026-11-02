#include "test_capi_v2.hpp"

#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/connection.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

//----------------------------------------------------------------------------------------------------------------------
// V2 query_result tests: streaming execution, schema, and metadata. Row-data decoding lives on the data_chunk + vector
// surfaces and is exercised separately.
//
// Tests drive the step primitive (result_step / result_wait, via the StepChunk / DrainRowCount helpers).
// The result_fetch_chunk convenience has its own focused tests.
//
// INVARIANTS:
//   - duckdb_v2_result_handle is a heap-allocated duckdb::ResultWrapperV2 cast to the handle type.
//     The bridge's ToResult cast and destructor depend on this layout.
//   - duckdb_v2_schema_handle returned by result_get_schema is owned (caller destroys via schema_destroy).
//     Its lifetime is independent of the result: it is a snapshot, so it stays valid after the result is destroyed.
//   - Names and types borrowed via schema_get_field are valid until the schema is destroyed.
//   - Chunks returned by step / fetch_chunk are owned and independent of the producing result / connection / database.
//----------------------------------------------------------------------------------------------------------------------

namespace test_capi_v2 {

// ===========================================================================
// SELECT round-trip, the canonical "SELECT 1" check.
// ===========================================================================

TEST_CASE("V2: statement_execute SELECT returns QUERY_RESULT with INTEGER column", "[capi_v2][query_result]") {
	EnvFixture fx;

	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "SELECT 1 AS one", &r, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(r != nullptr);

	DUCKDB_V2_RESULT_TYPE rt = DUCKDB_V2_RESULT_TYPE_NOTHING;
	REQUIRE(duckdb_v2_result_get_result_type(r, &rt, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(rt == DUCKDB_V2_RESULT_TYPE_QUERY_RESULT);

	DUCKDB_V2_STATEMENT_TYPE st = DUCKDB_V2_STATEMENT_TYPE_INVALID;
	REQUIRE(duckdb_v2_result_get_statement_type(r, &st, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(st == DUCKDB_V2_STATEMENT_TYPE_SELECT);

	REQUIRE(ColumnCount(r) == 1);
	RequireColumn(r, 0, "one", DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);

	REQUIRE(DrainRowCount(r) == 1);

	REQUIRE(duckdb_v2_result_destroy(&r) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(r == nullptr);
}

// ===========================================================================
// Multi-column SELECT: column count, names, and types.
// ===========================================================================

TEST_CASE("V2: statement_execute multi-column SELECT", "[capi_v2][query_result]") {
	EnvFixture fx;

	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "SELECT 1::INTEGER AS a, 'hi' AS b, 3.14::DOUBLE AS c", &r, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);

	REQUIRE(ColumnCount(r) == 3);

	struct {
		const char *name;
		DUCKDB_V2_LOGICAL_TYPE_ID id;
	} expected[] = {
	    {"a", DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER},
	    {"b", DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR},
	    {"c", DUCKDB_V2_LOGICAL_TYPE_ID_DOUBLE},
	};
	for (idx_t i = 0; i < 3; i++) {
		RequireColumn(r, i, expected[i].name, expected[i].id);
	}

	duckdb_v2_result_destroy(&r);
}

// ===========================================================================
// The step loop over a multi-chunk SELECT: WAITING is transient, chunks
// are caller-owned, FINISHED is sticky, and out_chunk is written iff the
// status is CHUNK.
// ===========================================================================
#if (STANDARD_VECTOR_SIZE == DEFAULT_STANDARD_VECTOR_SIZE)
TEST_CASE("V2: step loop drains a multi-chunk SELECT", "[capi_v2][query_result]") {
	EnvFixture fx;

	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "SELECT i FROM range(100000) t(i)", &r, nullptr) == DUCKDB_V2_ERROR_NONE);

	idx_t total_rows = 0;
	idx_t chunk_count = 0;
	// The WAITING rounds are timing-dependent, so the loop latches its findings
	// and the checks below run once: a per-round REQUIRE would make the suite's
	// assertion count differ between runs.
	auto step_rc = DUCKDB_V2_ERROR_NONE;
	auto final_status = DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
	bool chunks_valid = true;
	bool nulled_on_non_chunk = true;
	while (true) {
		// Poison the out-param to prove step nulls it on non-CHUNK
		// statuses.
		auto poison = reinterpret_cast<duckdb_v2_data_chunk_handle>(uintptr_t(0xdead));
		duckdb_v2_data_chunk_handle chunk = poison;
		DUCKDB_V2_RESULT_STEP_STATUS status = DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
		step_rc = duckdb_v2_result_step(r, &chunk, &status, nullptr);
		if (step_rc != DUCKDB_V2_ERROR_NONE) {
			final_status = status;
			break;
		}
		if (status == DUCKDB_V2_RESULT_STEP_STATUS_CHUNK) {
			if (!chunk || chunk == poison) {
				chunks_valid = false;
				final_status = status;
				break;
			}
			idx_t size = 0;
			duckdb_v2_data_chunk_get_size(chunk, &size, nullptr);
			chunks_valid = chunks_valid && size > 0;
			total_rows += size;
			chunk_count++;
			duckdb_v2_data_chunk_destroy(&chunk);
			continue;
		}
		nulled_on_non_chunk = nulled_on_non_chunk && chunk == nullptr;
		if (status == DUCKDB_V2_RESULT_STEP_STATUS_WAITING) {
			continue; // repeated stepping drives the query forward
		}
		final_status = status;
		break;
	}
	REQUIRE(step_rc == DUCKDB_V2_ERROR_NONE);
	REQUIRE(chunks_valid);
	REQUIRE(nulled_on_non_chunk); // nulled on every non-CHUNK status
	REQUIRE(final_status == DUCKDB_V2_RESULT_STEP_STATUS_FINISHED);
	REQUIRE(total_rows == 100000);
	REQUIRE(chunk_count > 1); // genuinely multi-chunk

	// FINISHED is sticky and keeps nulling out_chunk.
	for (int i = 0; i < 3; i++) {
		duckdb_v2_data_chunk_handle chunk = reinterpret_cast<duckdb_v2_data_chunk_handle>(uintptr_t(0xdead));
		DUCKDB_V2_RESULT_STEP_STATUS status = DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
		REQUIRE(duckdb_v2_result_step(r, &chunk, &status, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(status == DUCKDB_V2_RESULT_STEP_STATUS_FINISHED);
		REQUIRE(chunk == nullptr);
	}

	duckdb_v2_result_destroy(&r);
}
#endif

// ===========================================================================
// Metadata is valid before the first step, during consumption, and after
// the stream ends; it is prepare-time information.
// ===========================================================================
#if (STANDARD_VECTOR_SIZE == DEFAULT_STANDARD_VECTOR_SIZE)
TEST_CASE("V2: metadata valid before, during, and after consumption", "[capi_v2][query_result]") {
	EnvFixture fx;

	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "SELECT i AS steady FROM range(100000) t(i)", &r, nullptr) == DUCKDB_V2_ERROR_NONE);

	auto check_metadata = [&](const char *phase) {
		INFO("phase: " << phase);
		DUCKDB_V2_RESULT_TYPE rt = DUCKDB_V2_RESULT_TYPE_NOTHING;
		REQUIRE(duckdb_v2_result_get_result_type(r, &rt, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(rt == DUCKDB_V2_RESULT_TYPE_QUERY_RESULT);
		DUCKDB_V2_STATEMENT_TYPE st = DUCKDB_V2_STATEMENT_TYPE_INVALID;
		REQUIRE(duckdb_v2_result_get_statement_type(r, &st, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(st == DUCKDB_V2_STATEMENT_TYPE_SELECT);
		REQUIRE(ColumnCount(r) == 1);
		RequireColumn(r, 0, "steady", DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT);
	};

	// Before the first step: nothing has executed yet.
	check_metadata("before first step");

	// During: consume one chunk, then re-check.
	idx_t consumed = 0;
	auto chunk = StepChunk(r);
	REQUIRE(chunk != nullptr);
	idx_t size = 0;
	duckdb_v2_data_chunk_get_size(chunk, &size, nullptr);
	consumed += size;
	duckdb_v2_data_chunk_destroy(&chunk);
	check_metadata("mid-stream");

	// After FINISHED.
	consumed += DrainRowCount(r);
	REQUIRE(consumed == 100000);
	check_metadata("after FINISHED");

	duckdb_v2_result_destroy(&r);
}
#endif
// ===========================================================================
// DDL: CREATE / DROP report NOTHING; the stream yields no rows.
// ===========================================================================

TEST_CASE("V2: DDL returns NOTHING and an empty stream", "[capi_v2][query_result]") {
	EnvFixture fx;

	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "CREATE TABLE u (i INTEGER)", &r, nullptr) == DUCKDB_V2_ERROR_NONE);

	DUCKDB_V2_RESULT_TYPE rt = DUCKDB_V2_RESULT_TYPE_QUERY_RESULT;
	duckdb_v2_result_get_result_type(r, &rt, nullptr);
	REQUIRE(rt == DUCKDB_V2_RESULT_TYPE_NOTHING);

	DUCKDB_V2_STATEMENT_TYPE st = DUCKDB_V2_STATEMENT_TYPE_INVALID;
	duckdb_v2_result_get_statement_type(r, &st, nullptr);
	REQUIRE(st == DUCKDB_V2_STATEMENT_TYPE_CREATE);

	REQUIRE(DrainRowCount(r) == 0);

	duckdb_v2_result_destroy(&r);
}

// ===========================================================================
// DML: INSERT / UPDATE / DELETE report CHANGED_ROWS; the changed-row count
// arrives as the stream's single BIGINT "Count" chunk.
// ===========================================================================

TEST_CASE("V2: INSERT / UPDATE / DELETE stream the Count chunk", "[capi_v2][query_result]") {
	EnvFixture fx;

	ExecSQL(fx.conn, "CREATE TABLE t (i INTEGER)");

	// INSERT three rows.
	duckdb_v2_result_handle ins = nullptr;
	REQUIRE(Query(fx.conn, "INSERT INTO t VALUES (1), (2), (3)", &ins, nullptr) == DUCKDB_V2_ERROR_NONE);

	DUCKDB_V2_RESULT_TYPE rt = DUCKDB_V2_RESULT_TYPE_NOTHING;
	duckdb_v2_result_get_result_type(ins, &rt, nullptr);
	REQUIRE(rt == DUCKDB_V2_RESULT_TYPE_CHANGED_ROWS);

	DUCKDB_V2_STATEMENT_TYPE st = DUCKDB_V2_STATEMENT_TYPE_INVALID;
	duckdb_v2_result_get_statement_type(ins, &st, nullptr);
	REQUIRE(st == DUCKDB_V2_STATEMENT_TYPE_INSERT);

	REQUIRE(DrainChangedRows(ins) == 3);
	duckdb_v2_result_destroy(&ins);

	// UPDATE two rows.
	duckdb_v2_result_handle upd = nullptr;
	Query(fx.conn, "UPDATE t SET i = i + 10 WHERE i >= 2", &upd, nullptr);
	duckdb_v2_result_get_statement_type(upd, &st, nullptr);
	REQUIRE(st == DUCKDB_V2_STATEMENT_TYPE_UPDATE);
	REQUIRE(DrainChangedRows(upd) == 2);
	duckdb_v2_result_destroy(&upd);

	// DELETE one row.
	duckdb_v2_result_handle del = nullptr;
	Query(fx.conn, "DELETE FROM t WHERE i = 1", &del, nullptr);
	duckdb_v2_result_get_statement_type(del, &st, nullptr);
	REQUIRE(st == DUCKDB_V2_STATEMENT_TYPE_DELETE);
	REQUIRE(DrainChangedRows(del) == 1);
	duckdb_v2_result_destroy(&del);
}

TEST_CASE("V2: CHANGED_ROWS Count chunk reports 0 when WHERE matches nothing", "[capi_v2][query_result]") {
	EnvFixture fx;

	ExecSQL(fx.conn, "CREATE TABLE t (i INTEGER)");

	duckdb_v2_result_handle ins = nullptr;
	Query(fx.conn, "INSERT INTO t VALUES (1), (2)", &ins, nullptr);
	REQUIRE(DrainChangedRows(ins) == 2);
	duckdb_v2_result_destroy(&ins);

	duckdb_v2_result_handle upd = nullptr;
	REQUIRE(Query(fx.conn, "UPDATE t SET i = i WHERE 1=0", &upd, nullptr) == DUCKDB_V2_ERROR_NONE);
	DUCKDB_V2_RESULT_TYPE rt = DUCKDB_V2_RESULT_TYPE_NOTHING;
	duckdb_v2_result_get_result_type(upd, &rt, nullptr);
	REQUIRE(rt == DUCKDB_V2_RESULT_TYPE_CHANGED_ROWS);
	REQUIRE(DrainChangedRows(upd) == 0);
	duckdb_v2_result_destroy(&upd);

	duckdb_v2_result_handle del = nullptr;
	Query(fx.conn, "DELETE FROM t WHERE 1=0", &del, nullptr);
	REQUIRE(DrainChangedRows(del) == 0);
	duckdb_v2_result_destroy(&del);
}

// ===========================================================================
// Result schema on CHANGED_ROWS / NOTHING: the synthetic Count column is
// visible via the column accessors, before any step.
// ===========================================================================

TEST_CASE("V2: CHANGED_ROWS exposes synthetic Count column via column accessors", "[capi_v2][query_result]") {
	EnvFixture fx;

	ExecSQL(fx.conn, "CREATE TABLE t (i INTEGER)");

	duckdb_v2_result_handle ins = nullptr;
	Query(fx.conn, "INSERT INTO t VALUES (1)", &ins, nullptr);

	REQUIRE(ColumnCount(ins) == 1);
	// The synthetic column name is set by core; pin the current spelling
	// so a future rename surfaces here loud rather than silent.
	RequireColumn(ins, 0, "Count", DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT);

	duckdb_v2_result_destroy(&ins);
}

TEST_CASE("V2: NOTHING result exposes the synthetic Count column with zero rows", "[capi_v2][query_result]") {
	EnvFixture fx;

	duckdb_v2_result_handle r = nullptr;
	Query(fx.conn, "CREATE TABLE t (i INTEGER)", &r, nullptr);

	REQUIRE(ColumnCount(r) == 1);
	RequireColumn(r, 0, "Count", DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT);

	// The stream itself yields no rows.
	REQUIRE(DrainRowCount(r) == 0);

	duckdb_v2_result_destroy(&r);
}

// ===========================================================================
// result_fetch_chunk: the blocking convenience. Chunk, then NULL at
// end-of-stream, idempotently.
// ===========================================================================
#if (STANDARD_VECTOR_SIZE == DEFAULT_STANDARD_VECTOR_SIZE)
TEST_CASE("V2: result_fetch_chunk drains and reports end-of-stream as NULL", "[capi_v2][query_result]") {
	EnvFixture fx;

	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "SELECT i FROM range(100000) t(i)", &r, nullptr) == DUCKDB_V2_ERROR_NONE);

	idx_t total_rows = 0;
	idx_t chunk_count = 0;
	while (true) {
		duckdb_v2_data_chunk_handle chunk = reinterpret_cast<duckdb_v2_data_chunk_handle>(uintptr_t(0xdead));
		REQUIRE(duckdb_v2_result_fetch_chunk(r, &chunk, nullptr) == DUCKDB_V2_ERROR_NONE);
		if (!chunk) {
			break; // end-of-stream
		}
		idx_t size = 0;
		duckdb_v2_data_chunk_get_size(chunk, &size, nullptr);
		total_rows += size;
		chunk_count++;
		duckdb_v2_data_chunk_destroy(&chunk);
	}
	REQUIRE(total_rows == 100000);
	REQUIRE(chunk_count > 1);

	// End-of-stream is sticky.
	for (int i = 0; i < 3; i++) {
		duckdb_v2_data_chunk_handle chunk = reinterpret_cast<duckdb_v2_data_chunk_handle>(uintptr_t(0xdead));
		REQUIRE(duckdb_v2_result_fetch_chunk(r, &chunk, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(chunk == nullptr);
	}

	duckdb_v2_result_destroy(&r);
}
#endif
// ===========================================================================
// result_wait: unblocks stepping; no-op on terminal states.
// ===========================================================================
#if (STANDARD_VECTOR_SIZE == DEFAULT_STANDARD_VECTOR_SIZE)
TEST_CASE("V2: result_wait makes the step loop progress without busy-spinning", "[capi_v2][query_result]") {
	EnvFixture fx;

	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "SELECT i FROM range(100000) t(i)", &r, nullptr) == DUCKDB_V2_ERROR_NONE);

	// StepChunk waits between steps; draining the full stream through
	// it proves wait never wedges and stepping always becomes actionable.
	idx_t total_rows = 0;
	while (auto chunk = StepChunk(r)) {
		idx_t size = 0;
		duckdb_v2_data_chunk_get_size(chunk, &size, nullptr);
		total_rows += size;
		duckdb_v2_data_chunk_destroy(&chunk);
	}
	REQUIRE(total_rows == 100000);

	// Terminal state: wait is a no-op, never an error.
	for (int i = 0; i < 3; i++) {
		REQUIRE(duckdb_v2_result_wait(r, nullptr) == DUCKDB_V2_ERROR_NONE);
	}

	duckdb_v2_result_destroy(&r);
}
#endif

TEST_CASE("V2: result_wait before the first step is safe", "[capi_v2][query_result]") {
	EnvFixture fx;

	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "SELECT 1", &r, nullptr) == DUCKDB_V2_ERROR_NONE);
	// Nothing has executed yet; wait must return once a task is executable
	// (which is immediately, right after the pending query is created).
	REQUIRE(duckdb_v2_result_wait(r, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(DrainRowCount(r) == 1);
	duckdb_v2_result_destroy(&r);
}

// ===========================================================================
// Error paths at statement_execute time: prepare runs eagerly, so parser
// and binder errors surface here, not from the steps.
// ===========================================================================

TEST_CASE("V2: statement_execute surfaces parser error and leaves out_result null", "[capi_v2][query_result]") {
	EnvFixture fx;

	duckdb_v2_result_handle r = nullptr;
	duckdb_v2_error_info_handle err = nullptr;
	REQUIRE(Query(fx.conn, "SELEKT 1", &r, &err) == DUCKDB_V2_ERROR_QUERY_PARSER);
	REQUIRE(r == nullptr);
	REQUIRE(err != nullptr);

	duckdb_v2_str msg = {nullptr, 0};
	duckdb_v2_error_info_get_text(err, &msg);
	REQUIRE(msg.ptr != nullptr);
	REQUIRE(msg.len > 0); // detail propagated from the parser
	duckdb_v2_error_info_destroy(&err);
}

TEST_CASE("V2: statement_execute binder error (unknown table)", "[capi_v2][query_result]") {
	EnvFixture fx;

	duckdb_v2_result_handle r = nullptr;
	duckdb_v2_error_info_handle err = nullptr;
	REQUIRE(Query(fx.conn, "SELECT * FROM no_such_table", &r, &err) == DUCKDB_V2_ERROR_DATABASE_CATALOG);
	REQUIRE(r == nullptr);
	REQUIRE(err != nullptr);
	duckdb_v2_error_info_destroy(&err);
}

TEST_CASE("V2: statement_execute failure tolerates err == nullptr", "[capi_v2][query_result]") {
	EnvFixture fx;

	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "FROM BADSQL", &r, nullptr) == DUCKDB_V2_ERROR_DATABASE_CATALOG);
	REQUIRE(r == nullptr);
}

// ===========================================================================
// statement_execute takes exactly one parsed statement: a NULL statement
// handle (the iterator's end marker, or a no-statement input) is
// rejected. Multi-statement strings are handled by the iterator; see the
// sql_statement tests.
// ===========================================================================

TEST_CASE("V2: querying with a NULL statement is rejected", "[capi_v2][query_result]") {
	EnvFixture fx;

	// No-statement input parses to an immediately exhausted iterator; the
	// NULL statement it yields is rejected by statement_execute.
	for (const char *sql : {"", "   ", ";"}) {
		INFO("sql: '" << sql << "'");
		duckdb_v2_result_handle r = nullptr;
		REQUIRE(Query(fx.conn, sql, &r, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
		REQUIRE(r == nullptr);
	}
}

// ===========================================================================
// Error mid-stream: an execution-time failure surfaces from the steps as
// a sticky error with nulled out-params.
// ===========================================================================
#if (STANDARD_VECTOR_SIZE == DEFAULT_STANDARD_VECTOR_SIZE)
TEST_CASE("V2: execution error mid-stream is sticky", "[capi_v2][query_result]") {
	EnvFixture fx;

	// The cast only fails on the last row, so the failure is raised during
	// execution, after statement_execute has returned successfully.
	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn,
	              "SELECT (CASE WHEN i < 99999 THEN CAST(i AS VARCHAR) ELSE 'oops' END)::INT FROM range(100000) t(i)",
	              &r, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(r != nullptr);

	// Step until the error surfaces; chunks may legitimately arrive first.
	DUCKDB_V2_ERROR rc = DUCKDB_V2_ERROR_NONE;
	duckdb_v2_error_info_handle err = nullptr;
	// Latched, not asserted per round: the round count is timing-dependent.
	bool nulled_on_failure = true;
	bool finished_early = false;
	while (true) {
		duckdb_v2_data_chunk_handle chunk = reinterpret_cast<duckdb_v2_data_chunk_handle>(uintptr_t(0xdead));
		DUCKDB_V2_RESULT_STEP_STATUS status = DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
		rc = duckdb_v2_result_step(r, &chunk, &status, &err);
		if (rc != DUCKDB_V2_ERROR_NONE) {
			nulled_on_failure = chunk == nullptr;
			break;
		}
		if (status == DUCKDB_V2_RESULT_STEP_STATUS_FINISHED) {
			finished_early = true;
			break;
		}
		if (status == DUCKDB_V2_RESULT_STEP_STATUS_CHUNK) {
			duckdb_v2_data_chunk_destroy(&chunk);
		}
	}
	REQUIRE(nulled_on_failure);    // nulled on failure
	REQUIRE_FALSE(finished_early); // must fail before finishing
	REQUIRE(rc == DUCKDB_V2_ERROR_TYPE_CONVERSION);
	REQUIRE(err != nullptr);
	duckdb_v2_str msg = {nullptr, 0};
	duckdb_v2_error_info_get_text(err, &msg);
	REQUIRE(msg.ptr != nullptr);
	REQUIRE(msg.len > 0);
	duckdb_v2_error_info_destroy(&err);

	// Sticky: subsequent steps report the same code without touching the
	// (closed) internals; fetch_chunk reports it too.
	for (int i = 0; i < 2; i++) {
		duckdb_v2_data_chunk_handle chunk = reinterpret_cast<duckdb_v2_data_chunk_handle>(uintptr_t(0xdead));
		DUCKDB_V2_RESULT_STEP_STATUS status = DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
		REQUIRE(duckdb_v2_result_step(r, &chunk, &status, nullptr) == DUCKDB_V2_ERROR_TYPE_CONVERSION);
		REQUIRE(chunk == nullptr);
	}
	duckdb_v2_data_chunk_handle chunk = reinterpret_cast<duckdb_v2_data_chunk_handle>(uintptr_t(0xdead));
	REQUIRE(duckdb_v2_result_fetch_chunk(r, &chunk, nullptr) == DUCKDB_V2_ERROR_TYPE_CONVERSION);
	REQUIRE(chunk == nullptr);

	// Metadata still works after the failure (prepare-time information).
	REQUIRE(ColumnCount(r) == 1);

	duckdb_v2_result_destroy(&r);
}
#endif

// ===========================================================================
// Interrupt / cancellation. Decision 6: step reports the event as
// status CANCELLED; fetch_chunk reports it as ERROR_RUNTIME_INTERRUPT.
// ===========================================================================
#if (STANDARD_VECTOR_SIZE == DEFAULT_STANDARD_VECTOR_SIZE)
TEST_CASE("V2: interrupt between steps surfaces as sticky CANCELLED status", "[capi_v2][query_result]") {
	EnvFixture fx;

	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "SELECT i FROM range(10000000) t(i)", &r, nullptr) == DUCKDB_V2_ERROR_NONE);

	// Consume the first chunk so the stream is genuinely mid-flight.
	auto first = StepChunk(r);
	REQUIRE(first != nullptr);
	duckdb_v2_data_chunk_destroy(&first);

	REQUIRE(duckdb_v2_connection_interrupt(fx.conn, nullptr) == DUCKDB_V2_ERROR_NONE);

	// The next step that touches the engine observes the interrupt.
	DUCKDB_V2_RESULT_STEP_STATUS status = DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
	status = StepUntilCancelled(r);
	REQUIRE(status == DUCKDB_V2_RESULT_STEP_STATUS_CANCELLED);

	// Sticky, and out_chunk keeps being nulled.
	for (int i = 0; i < 3; i++) {
		duckdb_v2_data_chunk_handle chunk = reinterpret_cast<duckdb_v2_data_chunk_handle>(uintptr_t(0xdead));
		status = DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
		REQUIRE(duckdb_v2_result_step(r, &chunk, &status, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(status == DUCKDB_V2_RESULT_STEP_STATUS_CANCELLED);
		REQUIRE(chunk == nullptr);
	}

	duckdb_v2_result_destroy(&r);
}
#endif

#if (STANDARD_VECTOR_SIZE == DEFAULT_STANDARD_VECTOR_SIZE)
TEST_CASE("V2: interrupt during the pending phase maps to CANCELLED", "[capi_v2][query_result]") {
	EnvFixture fx;

	// Interrupt lands before the first step, i.e. while the result is
	// still in the pending phase (where the engine reports the interrupt
	// as an execution error of type INTERRUPT; the bridge maps it to the
	// CANCELLED status for consistency).
	QueryResult r;
	REQUIRE(Query(fx.conn, "SELECT i FROM range(10000000) t(i)", &r, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_interrupt(fx.conn, nullptr) == DUCKDB_V2_ERROR_NONE);

	DUCKDB_V2_RESULT_STEP_STATUS status = DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
	bool saw_chunk = false;
	status = StepUntilCancelled(r, &saw_chunk);
	REQUIRE(status == DUCKDB_V2_RESULT_STEP_STATUS_CANCELLED);
	REQUIRE_FALSE(saw_chunk); // no chunk can precede the interrupt

	duckdb_v2_result_destroy(&r);
}
#endif

#if (STANDARD_VECTOR_SIZE == DEFAULT_STANDARD_VECTOR_SIZE)
TEST_CASE("V2: fetch_chunk reports cancellation as ERROR_RUNTIME_INTERRUPT", "[capi_v2][query_result]") {
	EnvFixture fx;

	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "SELECT i FROM range(10000000) t(i)", &r, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_interrupt(fx.conn, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_data_chunk_handle chunk = reinterpret_cast<duckdb_v2_data_chunk_handle>(uintptr_t(0xdead));
	duckdb_v2_error_info_handle err = nullptr;
	REQUIRE(duckdb_v2_result_fetch_chunk(r, &chunk, &err) == DUCKDB_V2_ERROR_RUNTIME_INTERRUPT);
	REQUIRE(chunk == nullptr);
	REQUIRE(err != nullptr);
	duckdb_v2_error_info_destroy(&err);

	// Same event, two channels: the step channel reports the sticky
	// CANCELLED status, the fetch channel keeps reporting the error.
	DUCKDB_V2_RESULT_STEP_STATUS status = DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
	REQUIRE(duckdb_v2_result_step(r, &chunk, &status, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(status == DUCKDB_V2_RESULT_STEP_STATUS_CANCELLED);
	REQUIRE(duckdb_v2_result_fetch_chunk(r, &chunk, nullptr) == DUCKDB_V2_ERROR_RUNTIME_INTERRUPT);
	REQUIRE(chunk == nullptr);

	duckdb_v2_result_destroy(&r);
}
#endif

#if (STANDARD_VECTOR_SIZE == DEFAULT_STANDARD_VECTOR_SIZE)
TEST_CASE("V2: interrupt from a second thread cancels a blocked fetch", "[capi_v2][query_result]") {
	EnvFixture fx;

	// A heavy aggregation: a single result row that takes long enough to
	// compute that the interrupt lands mid-execution.
	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "SELECT sum(i) FROM range(2000000000) t(i) WHERE i % 3 <> 0", &r, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);

	std::thread interrupter([&]() {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		REQUIRE(duckdb_v2_connection_interrupt(fx.conn, nullptr) == DUCKDB_V2_ERROR_NONE);
	});

	duckdb_v2_data_chunk_handle chunk = nullptr;
	duckdb_v2_error_info_handle err = nullptr;
	auto rc = duckdb_v2_result_fetch_chunk(r, &chunk, &err);
	interrupter.join();

	// The interrupt must beat the multi-second aggregation. Capture the
	// error detail so a wrong code identifies the exception that escaped.
	duckdb_v2_str err_text = {nullptr, 0};
	if (err) {
		duckdb_v2_error_info_get_text(err, &err_text);
	}
	auto err_str = Convert(err_text);
	INFO("fetch_chunk error detail: " << (!err_str.empty() ? err_str : "(none)"));
	REQUIRE(rc == DUCKDB_V2_ERROR_RUNTIME_INTERRUPT);
	REQUIRE(chunk == nullptr);

	duckdb_v2_error_info_destroy(&err);
	duckdb_v2_result_destroy(&r);
}
#endif
TEST_CASE("V2: interrupt with no active query is a no-op", "[capi_v2][query_result]") {
	EnvFixture fx;

	REQUIRE(duckdb_v2_connection_interrupt(fx.conn, nullptr) == DUCKDB_V2_ERROR_NONE);

	// The stale interrupt flag must not poison the next query (the engine
	// resets it when a new query starts).
	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "SELECT 42", &r, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(DrainRowCount(r) == 1);
	duckdb_v2_result_destroy(&r);
}

// ===========================================================================
// connection_progress_get.
// ===========================================================================

TEST_CASE("V2: connection_progress_get reports idle values when no query is active", "[capi_v2][query_result]") {
	EnvFixture fx;

	auto progress = ReadProgress(fx.conn);
	REQUIRE(progress.percentage == -1.0);
	REQUIRE(progress.rows_processed == 0);
	REQUIRE(progress.total_rows_to_process == 0);
}

#if (STANDARD_VECTOR_SIZE == DEFAULT_STANDARD_VECTOR_SIZE)
TEST_CASE("V2: connection_progress_get advances while stepping a query", "[capi_v2][query_result]") {
	EnvFixture fx;

	// Single-threaded so all execution happens in our steps, and the
	// progress bar enabled so the engine publishes progress at all.
	for (const char *setup_sql : {"SET threads=1", "CREATE TABLE tbl AS SELECT range a FROM range(1000000)",
	                              "SET enable_progress_bar=true", "SET enable_progress_bar_print=false"}) {
		duckdb_v2_result_handle setup = nullptr;
		REQUIRE(Query(fx.conn, setup_sql, &setup, nullptr) == DUCKDB_V2_ERROR_NONE);
		DrainRowCount(setup);
		duckdb_v2_result_destroy(&setup);
	}

	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "SELECT sum(a) FROM tbl", &r, nullptr) == DUCKDB_V2_ERROR_NONE);

	// Progress restarts at 0 when the query begins and advances as steps
	// drive execution. Each call reads all fields from one snapshot.
	QueryProgress progress;
	bool saw_progress = false;
	// Each round reads a snapshot, and the round count is timing-dependent, so
	// both the step and the read latch instead of asserting.
	auto step_rc = DUCKDB_V2_ERROR_NONE;
	bool progress_reads_ok = true;
	while (true) {
		duckdb_v2_data_chunk_handle chunk = nullptr;
		DUCKDB_V2_RESULT_STEP_STATUS status = DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
		step_rc = duckdb_v2_result_step(r, &chunk, &status, nullptr);
		if (step_rc != DUCKDB_V2_ERROR_NONE) {
			break;
		}
		bool read_ok = false;
		progress = ReadProgress(fx.conn, &read_ok);
		progress_reads_ok = progress_reads_ok && read_ok;
		if (progress.percentage > 0.0) {
			saw_progress = true;
		}
		if (chunk) {
			duckdb_v2_data_chunk_destroy(&chunk);
		}
		if (status == DUCKDB_V2_RESULT_STEP_STATUS_FINISHED) {
			break;
		}
	}
	REQUIRE(step_rc == DUCKDB_V2_ERROR_NONE);
	REQUIRE(progress_reads_ok);
	REQUIRE(saw_progress);
	REQUIRE(progress.rows_processed <= progress.total_rows_to_process);

	duckdb_v2_result_destroy(&r);
}
#endif
// ===========================================================================
// Teardown discipline.
// ===========================================================================

#if (STANDARD_VECTOR_SIZE == DEFAULT_STANDARD_VECTOR_SIZE)
TEST_CASE("V2: destroying a never-stepped result is clean", "[capi_v2][query_result]") {
	EnvFixture fx;

	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "SELECT i FROM range(1000000) t(i)", &r, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_result_destroy(&r) == DUCKDB_V2_ERROR_NONE);

	// The connection remains fully usable.
	duckdb_v2_result_handle next = nullptr;
	REQUIRE(Query(fx.conn, "SELECT 1", &next, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(DrainRowCount(next) == 1);
	duckdb_v2_result_destroy(&next);
}

#endif
#if (STANDARD_VECTOR_SIZE == DEFAULT_STANDARD_VECTOR_SIZE)
TEST_CASE("V2: destroying a half-consumed result is clean", "[capi_v2][query_result]") {
	EnvFixture fx;

	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "SELECT i FROM range(1000000) t(i)", &r, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto chunk = StepChunk(r);
	REQUIRE(chunk != nullptr);
	duckdb_v2_data_chunk_destroy(&chunk);
	REQUIRE(duckdb_v2_result_destroy(&r) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_result_handle next = nullptr;
	REQUIRE(Query(fx.conn, "SELECT 1", &next, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(DrainRowCount(next) == 1);
	duckdb_v2_result_destroy(&next);
}
#endif
#if (STANDARD_VECTOR_SIZE == DEFAULT_STANDARD_VECTOR_SIZE)
TEST_CASE("V2: a fetched chunk outlives result, connection, and database", "[capi_v2][query_result]") {
	duckdb_v2_environment_handle env = nullptr;
	duckdb_v2_instance_handle instance = nullptr;
	duckdb_v2_connection_handle conn = nullptr;
	REQUIRE(duckdb_v2_environment_create(&env, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(OpenInstance(env, duckdb_v2_str {nullptr, 0}, &instance, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_create(instance, &conn, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(conn, "SELECT i, 'row-' || i AS s FROM range(100) t(i)", &r, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto chunk = StepChunk(r);
	REQUIRE(chunk != nullptr);

	duckdb_v2_result_destroy(&r);
	duckdb_v2_connection_destroy(&conn);
	duckdb_v2_instance_destroy(&instance);
	duckdb_v2_environment_destroy(&env);

	// The chunk owns its data; producers are all gone.
	idx_t size = 0;
	REQUIRE(duckdb_v2_data_chunk_get_size(chunk, &size, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(size == 100);
	duckdb_v2_vector_handle vec = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_vector_view view {};
	REQUIRE(duckdb_v2_vector_get_view(vec, &view, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(view.data != nullptr);
	REQUIRE(reinterpret_cast<const int64_t *>(view.data)[99] == 99);

	duckdb_v2_data_chunk_destroy(&chunk);
}
#endif
// ===========================================================================
// Lifetime: result_get_schema is a self-contained snapshot that outlives the
// result it came from. (Borrow stability and out-of-range indices are covered
// by the schema-type tests in test_capi_v2_schema.cpp.)
// ===========================================================================

TEST_CASE("V2: result_get_schema outlives the result", "[capi_v2][query_result]") {
	EnvFixture fx;

	duckdb_v2_result_handle r = nullptr;
	Query(fx.conn, "SELECT 1 AS only_column", &r, nullptr);

	duckdb_v2_schema_handle schema = nullptr;
	REQUIRE(duckdb_v2_result_get_schema(r, &schema, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_result_destroy(&r);

	// The schema is self-contained; it stays valid after the result is gone.
	duckdb_v2_str name = {nullptr, 0};
	duckdb_v2_logical_type_handle lt = nullptr;
	REQUIRE(duckdb_v2_schema_get_field(schema, 0, &name, &lt, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(Convert(name) == "only_column");
	DUCKDB_V2_LOGICAL_TYPE_ID id = DUCKDB_V2_LOGICAL_TYPE_ID_INVALID;
	REQUIRE(duckdb_v2_logical_type_get_id(lt, &id, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(id == DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);

	duckdb_v2_schema_destroy(&schema);
}

// ===========================================================================
// Null-arg validation across the surface.
// ===========================================================================

TEST_CASE("V2: statement_execute null-arg rejection", "[capi_v2][query_result]") {
	EnvFixture fx;

	// Exercise the real duckdb_v2_str signature. A {NULL, 0} sql is a valid
	// empty view; the malformed case is a null pointer with a nonzero length.
	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(nullptr, "SELECT 1", &r, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(Query(fx.conn, nullptr, &r, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(Query(fx.conn, "SELECT 1", nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
}

TEST_CASE("V2: result_destroy is null-safe", "[capi_v2][query_result]") {
	REQUIRE(duckdb_v2_result_destroy(nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_result_handle already_null = nullptr;
	REQUIRE(duckdb_v2_result_destroy(&already_null) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(already_null == nullptr);
}

TEST_CASE("V2: result accessors reject null handle and null out-params", "[capi_v2][query_result]") {
	EnvFixture fx;

	DUCKDB_V2_RESULT_TYPE rt;
	DUCKDB_V2_STATEMENT_TYPE st;
	duckdb_v2_schema_handle schema = nullptr;
	duckdb_v2_data_chunk_handle chunk = nullptr;
	DUCKDB_V2_RESULT_STEP_STATUS status;

	REQUIRE(duckdb_v2_result_get_result_type(nullptr, &rt, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_result_get_statement_type(nullptr, &st, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_result_get_schema(nullptr, &schema, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_result_step(nullptr, &chunk, &status, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_result_fetch_chunk(nullptr, &chunk, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_result_wait(nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_connection_interrupt(nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	double pct;
	uint64_t rows, total;
	REQUIRE(duckdb_v2_connection_progress_get(nullptr, &pct, &rows, &total, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	duckdb_v2_result_handle r = nullptr;
	Query(fx.conn, "SELECT 1", &r, nullptr);
	REQUIRE(duckdb_v2_result_get_result_type(r, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_result_get_statement_type(r, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_result_get_schema(r, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_result_step(r, nullptr, &status, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_result_step(r, &chunk, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_result_fetch_chunk(r, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_connection_progress_get(fx.conn, nullptr, &rows, &total, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_connection_progress_get(fx.conn, &pct, nullptr, &total, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_connection_progress_get(fx.conn, &pct, &rows, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_connection_progress_get(fx.conn, &pct, &rows, &total, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_result_destroy(&r);
}

// ===========================================================================
// Belt-and-braces: a succeeding call leaves a pre-existing *err untouched.
// ===========================================================================

TEST_CASE("V2: statement_execute leaves pre-existing err untouched on success", "[capi_v2][query_result]") {
	EnvFixture fx;

	duckdb_v2_result_handle r = nullptr;
	duckdb_v2_error_info_handle err = nullptr;
	REQUIRE(Query(fx.conn, "FROM BADSQL", &r, &err) == DUCKDB_V2_ERROR_DATABASE_CATALOG);
	REQUIRE(err != nullptr);

	REQUIRE(Query(fx.conn, "SELECT 1", &r, &err) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(err != nullptr);
	DUCKDB_V2_ERROR code = DUCKDB_V2_ERROR_NONE;
	duckdb_v2_error_info_get_code(err, &code);
	REQUIRE(code == DUCKDB_V2_ERROR_DATABASE_CATALOG);
	duckdb_v2_error_info_destroy(&err);
	duckdb_v2_result_destroy(&r);
}

// ===========================================================================
// Numeric round-trip for higher-numbered StatementType values.
//
// Earlier values (SELECT=1, INSERT=2, …, CREATE=4) happen to line up
// between the various candidate numberings, so the regression that catches
// a spec/core drift has to exercise the higher-numbered ids whose values
// only line up under the §4 "numerically identical to core" convention.
// ===========================================================================

TEST_CASE("V2: statement_type numeric round-trip for higher-numbered values", "[capi_v2][query_result]") {
	EnvFixture fx;

	ExecSQL(fx.conn, "CREATE TABLE t (i INTEGER)");

	struct Case {
		const char *sql;
		DUCKDB_V2_STATEMENT_TYPE expected;
	} cases[] = {
	    {"EXPLAIN SELECT 1", DUCKDB_V2_STATEMENT_TYPE_EXPLAIN},          // 14
	    {"DROP TABLE t", DUCKDB_V2_STATEMENT_TYPE_DROP},                 // 15
	    {"PRAGMA disable_profiling", DUCKDB_V2_STATEMENT_TYPE_PRAGMA},   // 17
	    {"SET memory_limit='1GB'", DUCKDB_V2_STATEMENT_TYPE_SET},        // 20
	    {"ATTACH ':memory:' AS other", DUCKDB_V2_STATEMENT_TYPE_ATTACH}, // 25
	};
	for (auto &c : cases) {
		duckdb_v2_result_handle r = nullptr;
		REQUIRE(Query(fx.conn, c.sql, &r, nullptr) == DUCKDB_V2_ERROR_NONE);
		DUCKDB_V2_STATEMENT_TYPE st = DUCKDB_V2_STATEMENT_TYPE_INVALID;
		duckdb_v2_result_get_statement_type(r, &st, nullptr);
		REQUIRE(st == c.expected);
		duckdb_v2_result_destroy(&r);
	}
}

// ===========================================================================
// Results don't alias each other's storage, and a terminal (drained)
// result keeps its metadata usable while later queries run.
// ===========================================================================

TEST_CASE("V2: results are independent; destroying one leaves the other usable", "[capi_v2][query_result]") {
	EnvFixture fx;

	duckdb_v2_result_handle a = nullptr;
	duckdb_v2_result_handle b = nullptr;
	Query(fx.conn, "SELECT 1 AS aa", &a, nullptr);
	// a must reach a terminal state before the connection accepts b.
	REQUIRE(DrainRowCount(a) == 1);
	Query(fx.conn, "SELECT 'hi' AS bb", &b, nullptr);

	RequireColumn(a, 0, "aa", DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	RequireColumn(b, 0, "bb", DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR);

	// Destroy a; b's schema is still fetchable.
	duckdb_v2_result_destroy(&a);
	RequireColumn(b, 0, "bb", DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR);

	duckdb_v2_result_destroy(&b);
}

// ===========================================================================
// One running statement per connection: statement_execute refuses with
// RESOURCE_IN_USE while a result's statement is running, and the connection
// is freed by drain, destroy, interrupt-then-step, or a sticky error.
// ===========================================================================
#if (STANDARD_VECTOR_SIZE == DEFAULT_STANDARD_VECTOR_SIZE)
TEST_CASE("V2: statement_execute refuses while a live result exists", "[capi_v2][query_result]") {
	EnvFixture fx;

	duckdb_v2_result_handle live = nullptr;
	REQUIRE(Query(fx.conn, "SELECT i FROM range(100000) t(i)", &live, nullptr) == DUCKDB_V2_ERROR_NONE);

	// Refused while live, with detail; the live result is untouched.
	duckdb_v2_result_handle second = reinterpret_cast<duckdb_v2_result_handle>(uintptr_t(0xdead));
	duckdb_v2_error_info_handle err = nullptr;
	REQUIRE(Query(fx.conn, "SELECT 1", &second, &err) == DUCKDB_V2_ERROR_RESOURCE_IN_USE);
	REQUIRE(second == nullptr);
	REQUIRE(err != nullptr);
	duckdb_v2_str msg = {nullptr, 0};
	duckdb_v2_error_info_get_text(err, &msg);
	REQUIRE(msg.ptr != nullptr);
	REQUIRE(Convert(msg).find("connection has an open result") != std::string::npos);
	duckdb_v2_error_info_destroy(&err);

	// Consuming the live result still works after the refusal.
	auto chunk = StepChunk(live);
	REQUIRE(chunk != nullptr);
	duckdb_v2_data_chunk_destroy(&chunk);

	// Draining to FINISHED frees the connection; the handle may still be
	// alive (metadata stays usable).
	while ((chunk = StepChunk(live))) {
		duckdb_v2_data_chunk_destroy(&chunk);
	}
	REQUIRE(Query(fx.conn, "SELECT 2", &second, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(DrainRowCount(second) == 1);
	duckdb_v2_result_destroy(&second);
	duckdb_v2_result_destroy(&live);
}
#endif
#if (STANDARD_VECTOR_SIZE == DEFAULT_STANDARD_VECTOR_SIZE)
TEST_CASE("V2: a statement that expands into a group holds the connection until it finishes",
          "[capi_v2][query_result]") {
	EnvFixture fx;
	duckdb_v2_result_handle setup = nullptr;
	REQUIRE(Query(fx.conn, "SET threads=1", &setup) == DUCKDB_V2_ERROR_NONE);
	DrainRowCount(setup);
	duckdb_v2_result_destroy(&setup);
	REQUIRE(Query(fx.conn, "CREATE TABLE t AS SELECT range i FROM range(100000)", &setup) == DUCKDB_V2_ERROR_NONE);
	DrainRowCount(setup);
	duckdb_v2_result_destroy(&setup);

	// A volatile default expands into ADD COLUMN, UPDATE and SET DEFAULT, which run as one query. Like a single
	// statement that completed before its rows are read, the group frees the connection once its query ended, and not
	// before: whenever another statement is let in, the whole group has applied
	duckdb_v2_result_handle group = nullptr;
	REQUIRE(Query(fx.conn, "ALTER TABLE t ADD COLUMN c DOUBLE DEFAULT random()", &group) == DUCKDB_V2_ERROR_NONE);
	// Latched, so the assertion count does not depend on how many steps the group takes
	bool freed_only_once_applied = true;
	idx_t refused_steps = 0;
	auto step_rc = DUCKDB_V2_ERROR_NONE;
	auto status = DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
	while (true) {
		duckdb_v2_data_chunk_handle chunk = nullptr;
		step_rc = duckdb_v2_result_step(group, &chunk, &status, nullptr);
		duckdb_v2_data_chunk_destroy(&chunk);
		if (step_rc != DUCKDB_V2_ERROR_NONE || status == DUCKDB_V2_RESULT_STEP_STATUS_FINISHED) {
			break;
		}
		duckdb_v2_result_handle other = nullptr;
		auto rc = Query(fx.conn, "SELECT c FROM t WHERE c IS NOT NULL", &other);
		if (rc == DUCKDB_V2_ERROR_RESOURCE_IN_USE) {
			refused_steps++;
			continue;
		}
		if (rc != DUCKDB_V2_ERROR_NONE || DrainRowCount(other) != 100000) {
			freed_only_once_applied = false;
		}
		duckdb_v2_result_destroy(&other);
	}
	REQUIRE(step_rc == DUCKDB_V2_ERROR_NONE);
	REQUIRE(status == DUCKDB_V2_RESULT_STEP_STATUS_FINISHED);
	REQUIRE(freed_only_once_applied);
	REQUIRE(refused_steps > 0);
	duckdb_v2_result_destroy(&group);

	duckdb_v2_result_handle filled = nullptr;
	REQUIRE(Query(fx.conn, "SELECT c FROM t WHERE c IS NOT NULL", &filled) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(DrainRowCount(filled) == 100000);
	duckdb_v2_result_destroy(&filled);
}

TEST_CASE("V2: a statement that expands into a group is refused while another statement runs",
          "[capi_v2][query_result]") {
	EnvFixture fx;
	duckdb_v2_result_handle setup = nullptr;
	REQUIRE(Query(fx.conn, "CREATE TABLE t AS SELECT range i FROM range(1000)", &setup) == DUCKDB_V2_ERROR_NONE);
	DrainRowCount(setup);
	duckdb_v2_result_destroy(&setup);

	duckdb_v2_result_handle live = nullptr;
	REQUIRE(Query(fx.conn, "SELECT i FROM range(100000) t(i)", &live) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_result_handle group = nullptr;
	REQUIRE(Query(fx.conn, "ALTER TABLE t ADD COLUMN c DOUBLE DEFAULT random()", &group) ==
	        DUCKDB_V2_ERROR_RESOURCE_IN_USE);
	REQUIRE(DrainRowCount(live) == 100000);
	duckdb_v2_result_destroy(&live);

	REQUIRE(Query(fx.conn, "ALTER TABLE t ADD COLUMN c DOUBLE DEFAULT random()", &group) == DUCKDB_V2_ERROR_NONE);
	DrainRowCount(group);
	duckdb_v2_result_destroy(&group);
	duckdb_v2_result_handle filled = nullptr;
	REQUIRE(Query(fx.conn, "SELECT c FROM t WHERE c IS NOT NULL", &filled) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(DrainRowCount(filled) == 1000);
	duckdb_v2_result_destroy(&filled);
}

//! Executes through the Arrow entry point, destroying any result
DUCKDB_V2_ERROR ExecuteArrow(duckdb_v2_connection_handle conn, const char *sql) {
	duckdb_v2_statement_iterator_handle iter = nullptr;
	REQUIRE(duckdb_v2_parse_sql(conn, sql, true, &iter, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_sql_statement_handle stmt = nullptr;
	REQUIRE(duckdb_v2_statement_iterator_next(iter, &stmt, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_arrow_result_handle result = nullptr;
	auto rc = duckdb_v2_statement_execute_arrow(conn, stmt, nullptr, nullptr, 0, 0, &result, nullptr);
	if (result) {
		duckdb_v2_arrow_result_destroy(&result);
	}
	duckdb_v2_sql_statement_destroy(&stmt);
	duckdb_v2_statement_iterator_destroy(&iter);
	return rc;
}

TEST_CASE("V2: a statement refused while another runs leaves the transaction intact", "[capi_v2][query_result]") {
	EnvFixture fx;
	ExecSQL(fx.conn, "CREATE TABLE t(i INTEGER)");
	ExecSQL(fx.conn, "BEGIN");
	ExecSQL(fx.conn, "INSERT INTO t VALUES (42)");

	duckdb_v2_result_handle live = nullptr;
	REQUIRE(Query(fx.conn, "SELECT i FROM range(100000) t(i)", &live) == DUCKDB_V2_ERROR_NONE);
	// A failing PRAGMA would invalidate the transaction while it is preprocessed: it is refused first
	duckdb_v2_result_handle refused = nullptr;
	REQUIRE(Query(fx.conn, "PRAGMA no_such_pragma", &refused) == DUCKDB_V2_ERROR_RESOURCE_IN_USE);
	REQUIRE(ExecuteArrow(fx.conn, "PRAGMA no_such_pragma") == DUCKDB_V2_ERROR_RESOURCE_IN_USE);
	REQUIRE(DrainRowCount(live) == 100000);
	duckdb_v2_result_destroy(&live);

	ExecSQL(fx.conn, "COMMIT");
	duckdb_v2_result_handle rows = nullptr;
	REQUIRE(Query(fx.conn, "SELECT i FROM t", &rows) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(DrainRowCount(rows) == 1);
	duckdb_v2_result_destroy(&rows);
}

//! Requests a cancellation while the statement is being planned, after the engine began the query
class CancelWhilePlanning : public duckdb::ClientContextState {
public:
	explicit CancelWhilePlanning(duckdb_v2_connection_handle conn) : conn(conn) {
	}
	bool CanRequestRebind() override {
		return true;
	}
	duckdb::RebindQueryInfo OnFinalizePrepare(duckdb::ClientContext &, duckdb::PreparedStatementData &,
	                                          duckdb::PreparedStatementMode) override {
		std::thread cancel([&] { duckdb_v2_connection_interrupt(conn, nullptr); });
		cancel.join();
		return duckdb::RebindQueryInfo::DO_NOT_REBIND;
	}

	duckdb_v2_connection_handle conn;
};

TEST_CASE("V2: a cancel requested while a statement is submitted cancels it", "[capi_v2][query_result]") {
	EnvFixture fx;
	auto &con = *reinterpret_cast<duckdb::Connection *>(fx.conn);
	con.context->registered_state->Insert("cancel_while_planning",
	                                      duckdb::make_shared_ptr<CancelWhilePlanning>(fx.conn));
	duckdb_v2_result_handle result = nullptr;
	REQUIRE(Query(fx.conn, "SELECT 42", &result) == DUCKDB_V2_ERROR_NONE);
	con.context->registered_state->Remove("cancel_while_planning");

	duckdb_v2_data_chunk_handle chunk = nullptr;
	auto status = DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
	REQUIRE(duckdb_v2_result_step(result, &chunk, &status, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(status == DUCKDB_V2_RESULT_STEP_STATUS_CANCELLED);
	duckdb_v2_result_destroy(&result);
}

TEST_CASE("V2: destroying a finished expanded statement leaves a later transaction alone", "[capi_v2][query_result]") {
	EnvFixture fx;
	ExecSQL(fx.conn, "CREATE TABLE t(i INTEGER)");
	ExecSQL(fx.conn, "INSERT INTO t VALUES (1)");
	duckdb_v2_result_handle group = nullptr;
	REQUIRE(Query(fx.conn, "ALTER TABLE t ADD COLUMN c DOUBLE DEFAULT random()", &group) == DUCKDB_V2_ERROR_NONE);
	DrainRowCount(group);

	ExecSQL(fx.conn, "BEGIN");
	ExecSQL(fx.conn, "INSERT INTO t(i) VALUES (2)");
	duckdb_v2_result_destroy(&group);
	ExecSQL(fx.conn, "COMMIT");
	duckdb_v2_result_handle rows = nullptr;
	REQUIRE(Query(fx.conn, "SELECT i FROM t", &rows) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(DrainRowCount(rows) == 2);
	duckdb_v2_result_destroy(&rows);
}

TEST_CASE("V2: an expanded statement abandoned at any step applies all or nothing", "[capi_v2][query_result]") {
	bool explicit_transaction = false;
	SECTION("autocommit") {
	}
	SECTION("inside a user transaction") {
		explicit_transaction = true;
	}
	// Latched, so the assertion count does not depend on how many steps the statement takes
	bool all_or_nothing = true;
	bool finished = false;
	for (idx_t steps = 0; !finished; steps++) {
		EnvFixture fx;
		ExecSQL(fx.conn, "SET threads=1");
		ExecSQL(fx.conn, "CREATE TABLE t AS SELECT range i FROM range(10)");
		if (explicit_transaction) {
			ExecSQL(fx.conn, "BEGIN");
		}
		duckdb_v2_result_handle group = nullptr;
		REQUIRE(Query(fx.conn, "ALTER TABLE t ADD COLUMN c DOUBLE DEFAULT random()", &group) == DUCKDB_V2_ERROR_NONE);
		for (idx_t step = 0; step < steps && !finished; step++) {
			duckdb_v2_data_chunk_handle chunk = nullptr;
			auto status = DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
			duckdb_v2_result_step(group, &chunk, &status, nullptr);
			duckdb_v2_data_chunk_destroy(&chunk);
			finished = status == DUCKDB_V2_RESULT_STEP_STATUS_FINISHED;
		}
		duckdb_v2_result_destroy(&group);
		if (explicit_transaction) {
			// An abandoned group invalidated the transaction, so this rolls it back
			ExecSQL(fx.conn, "COMMIT");
		}
		duckdb_v2_result_handle column = nullptr;
		REQUIRE(Query(fx.conn, "SELECT column_name FROM duckdb_columns() WHERE table_name = 't' AND column_name = 'c'",
		              &column) == DUCKDB_V2_ERROR_NONE);
		auto added = DrainRowCount(column) == 1;
		duckdb_v2_result_destroy(&column);
		if (added) {
			duckdb_v2_result_handle unfilled = nullptr;
			REQUIRE(Query(fx.conn, "SELECT i FROM t WHERE c IS NULL", &unfilled) == DUCKDB_V2_ERROR_NONE);
			if (DrainRowCount(unfilled) != 0) {
				all_or_nothing = false;
			}
			duckdb_v2_result_destroy(&unfilled);
		} else if (finished) {
			all_or_nothing = false;
		}
	}
	REQUIRE(all_or_nothing);
}

TEST_CASE("V2: a statement that completed before its rows are read does not hold the connection",
          "[capi_v2][query_result]") {
	EnvFixture fx;
	duckdb_v2_result_handle setup = nullptr;
	REQUIRE(Query(fx.conn, "CREATE TABLE t(i BIGINT)", &setup) == DUCKDB_V2_ERROR_NONE);
	DrainRowCount(setup);
	duckdb_v2_result_destroy(&setup);

	// An insert completes, and commits, before the rows it returns are read
	duckdb_v2_result_handle returning = nullptr;
	REQUIRE(Query(fx.conn, "INSERT INTO t SELECT range FROM range(100000) RETURNING i", &returning) ==
	        DUCKDB_V2_ERROR_NONE);
	auto first = StepChunk(returning);
	REQUIRE(first != nullptr);
	idx_t rows = 0;
	duckdb_v2_data_chunk_get_size(first, &rows, nullptr);
	duckdb_v2_data_chunk_destroy(&first);

	duckdb_v2_result_handle count = nullptr;
	REQUIRE(Query(fx.conn, "SELECT i FROM t", &count) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(DrainRowCount(count) == 100000);
	duckdb_v2_result_destroy(&count);

	rows += DrainRowCount(returning);
	REQUIRE(rows == 100000);
	duckdb_v2_result_destroy(&returning);
}
#endif
#if (STANDARD_VECTOR_SIZE == DEFAULT_STANDARD_VECTOR_SIZE)
TEST_CASE("V2: destroying an undrained result frees the connection", "[capi_v2][query_result]") {
	EnvFixture fx;

	duckdb_v2_result_handle live = nullptr;
	REQUIRE(Query(fx.conn, "SELECT i FROM range(100000) t(i)", &live, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_result_handle second = nullptr;
	REQUIRE(Query(fx.conn, "SELECT 1", &second, nullptr) == DUCKDB_V2_ERROR_RESOURCE_IN_USE);

	duckdb_v2_result_destroy(&live);
	REQUIRE(Query(fx.conn, "SELECT 1", &second, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(DrainRowCount(second) == 1);
	duckdb_v2_result_destroy(&second);
}

#endif
#if (STANDARD_VECTOR_SIZE == DEFAULT_STANDARD_VECTOR_SIZE)
TEST_CASE("V2: a cancelled result frees the connection once the step observes it", "[capi_v2][query_result]") {
	EnvFixture fx;

	QueryResult live;
	REQUIRE(Query(fx.conn, "SELECT i FROM range(10000000) t(i)", &live, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_interrupt(fx.conn, nullptr) == DUCKDB_V2_ERROR_NONE);
	// A refused execute leaves the cancellation to the running result
	QueryResult refused;
	REQUIRE(Query(fx.conn, "SELECT 1", &refused, nullptr) == DUCKDB_V2_ERROR_RESOURCE_IN_USE);

	// The connection is freed on the terminal transition, i.e. when a step
	// observes the cancellation, not by the interrupt itself.
	DUCKDB_V2_RESULT_STEP_STATUS status = DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
	status = StepUntilCancelled(live);
	REQUIRE(status == DUCKDB_V2_RESULT_STEP_STATUS_CANCELLED);

	QueryResult second;
	REQUIRE(Query(fx.conn, "SELECT 1", &second, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(DrainRowCount(second) == 1);
	duckdb_v2_result_destroy(&second);
	duckdb_v2_result_destroy(&live);
}

#endif
TEST_CASE("V2: a sticky execution error frees the connection", "[capi_v2][query_result]") {
	EnvFixture fx;

	duckdb_v2_result_handle live = nullptr;
	REQUIRE(Query(fx.conn, "SELECT 'oops'::INT FROM range(10) t(i)", &live, nullptr) == DUCKDB_V2_ERROR_NONE);
	DUCKDB_V2_ERROR rc = DUCKDB_V2_ERROR_NONE;
	while (rc == DUCKDB_V2_ERROR_NONE) {
		duckdb_v2_data_chunk_handle chunk = nullptr;
		DUCKDB_V2_RESULT_STEP_STATUS status = DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
		rc = duckdb_v2_result_step(live, &chunk, &status, nullptr);
		if (chunk) {
			duckdb_v2_data_chunk_destroy(&chunk);
		}
	}
	REQUIRE(rc == DUCKDB_V2_ERROR_TYPE_CONVERSION);

	duckdb_v2_result_handle second = nullptr;
	REQUIRE(Query(fx.conn, "SELECT 1", &second, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(DrainRowCount(second) == 1);
	duckdb_v2_result_destroy(&second);
	duckdb_v2_result_destroy(&live);
}

#if (STANDARD_VECTOR_SIZE == DEFAULT_STANDARD_VECTOR_SIZE)
TEST_CASE("V2: a busy connection does not affect a second connection", "[capi_v2][query_result]") {
	EnvFixture fx;

	duckdb_v2_connection_handle conn2 = nullptr;
	REQUIRE(duckdb_v2_connection_create(fx.instance, &conn2, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_result_handle live = nullptr;
	REQUIRE(Query(fx.conn, "SELECT i FROM range(100000) t(i)", &live, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_result_handle other = nullptr;
	REQUIRE(Query(conn2, "SELECT 1", &other, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(DrainRowCount(other) == 1);
	duckdb_v2_result_destroy(&other);

	duckdb_v2_result_destroy(&live);
	duckdb_v2_connection_destroy(&conn2);
}
#endif
// ===========================================================================
// result_drain: run to completion, report the changed-row count.
// ===========================================================================

namespace {
// Query + drain + destroy, returning the drained changed-row count.
idx_t QueryAndDrain(duckdb_v2_connection_handle conn, const char *sql) {
	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(conn, sql, &r, nullptr) == DUCKDB_V2_ERROR_NONE);
	idx_t rows_changed = 99;
	REQUIRE(duckdb_v2_result_drain(r, &rows_changed, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_result_destroy(&r);
	return rows_changed;
}
} // namespace

#if (STANDARD_VECTOR_SIZE == DEFAULT_STANDARD_VECTOR_SIZE)
TEST_CASE("V2: result_drain applies side effects and reports rows changed", "[capi_v2][query_result]") {
	EnvFixture fx;

	REQUIRE(QueryAndDrain(fx.conn, "CREATE TABLE t (i INTEGER)") == 0); // NOTHING result
	REQUIRE(QueryAndDrain(fx.conn, "INSERT INTO t VALUES (1), (2), (3)") == 3);
	REQUIRE(QueryAndDrain(fx.conn, "UPDATE t SET i = i WHERE 1=0") == 0);

	// Rows of a row-producing result are discarded; 0 rows changed.
	REQUIRE(QueryAndDrain(fx.conn, "SELECT i FROM range(100000) t(i)") == 0);

	// The side effects are visible: the drained INSERT really ran.
	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "SELECT i FROM t", &r, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(DrainRowCount(r) == 3);
	duckdb_v2_result_destroy(&r);
}
#endif

#if (STANDARD_VECTOR_SIZE == DEFAULT_STANDARD_VECTOR_SIZE)
TEST_CASE("V2: result_drain edge and error paths", "[capi_v2][query_result]") {
	EnvFixture fx;

	idx_t rows_changed = 99;
	duckdb_v2_error_info_handle err = nullptr;

	// Execution-time error propagates with its type and is sticky.
	duckdb_v2_result_handle bad = nullptr;
	REQUIRE(Query(fx.conn, "SELECT 'oops'::INT FROM range(10) t(i)", &bad, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_result_drain(bad, &rows_changed, &err) == DUCKDB_V2_ERROR_TYPE_CONVERSION);
	REQUIRE(err != nullptr);
	duckdb_v2_error_info_destroy(&err);
	REQUIRE(duckdb_v2_result_drain(bad, &rows_changed, nullptr) == DUCKDB_V2_ERROR_TYPE_CONVERSION);
	duckdb_v2_result_destroy(&bad);

	// Draining an already drained (FINISHED) result succeeds with 0: the
	// Count chunk was consumed by the first drain.
	duckdb_v2_result_handle ins = nullptr;
	REQUIRE(Query(fx.conn, "CREATE TABLE t (i INTEGER)", &ins, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_result_drain(ins, &rows_changed, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_result_destroy(&ins);
	REQUIRE(Query(fx.conn, "INSERT INTO t VALUES (1), (2)", &ins, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_result_drain(ins, &rows_changed, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(rows_changed == 2);
	REQUIRE(duckdb_v2_result_drain(ins, &rows_changed, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(rows_changed == 0);
	duckdb_v2_result_destroy(&ins);

	// Cancellation surfaces as RUNTIME_INTERRUPT, like fetch_chunk.
	duckdb_v2_result_handle live = nullptr;
	REQUIRE(Query(fx.conn, "SELECT i FROM range(10000000) t(i)", &live, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_interrupt(fx.conn, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_result_drain(live, &rows_changed, nullptr) == DUCKDB_V2_ERROR_RUNTIME_INTERRUPT);
	duckdb_v2_result_destroy(&live);

	// Null-arg rejection.
	REQUIRE(duckdb_v2_result_drain(nullptr, &rows_changed, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "SELECT 1", &r, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_result_drain(r, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_result_destroy(&r);

	// Draining released the connection: a fresh query still works.
	REQUIRE(Query(fx.conn, "SELECT 1", &r, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(DrainRowCount(r) == 1);
	duckdb_v2_result_destroy(&r);
}
#endif

// ===========================================================================
// result_wait is guarded against engine state the wrapper doesn't
// control: an interrupted-but-not-yet-observed result must not hang or
// surface an engine INTERNAL error.
// ===========================================================================

#if (STANDARD_VECTOR_SIZE == DEFAULT_STANDARD_VECTOR_SIZE)
TEST_CASE("V2: result_wait after interrupt is clean", "[capi_v2][query_result]") {
	EnvFixture fx;

	QueryResult r;
	REQUIRE(Query(fx.conn, "SELECT i FROM range(10000000) t(i)", &r, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_interrupt(fx.conn, nullptr) == DUCKDB_V2_ERROR_NONE);

	// Wait must return (the engine wakes waiters on interrupt) without an
	// INTERNAL error; the next steps then observe CANCELLED.
	duckdb_v2_error_info_handle err = nullptr;
	auto wait_rc = duckdb_v2_result_wait(r, &err);
	REQUIRE(wait_rc != DUCKDB_V2_ERROR_RUNTIME_INTERNAL);
	duckdb_v2_error_info_destroy(&err);

	DUCKDB_V2_RESULT_STEP_STATUS status = DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
	status = StepUntilCancelled(r);
	REQUIRE(status == DUCKDB_V2_RESULT_STEP_STATUS_CANCELLED);

	// And wait on the now-terminal result stays a no-op.
	REQUIRE(duckdb_v2_result_wait(r, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_result_destroy(&r);
}
#endif
// ===========================================================================
// result_fetch_chunk on a CHANGED_ROWS result: the convenience path
// yields the Count chunk, then end-of-stream.
// ===========================================================================

TEST_CASE("V2: result_fetch_chunk drains a CHANGED_ROWS result", "[capi_v2][query_result]") {
	EnvFixture fx;

	ExecSQL(fx.conn, "CREATE TABLE t (i INTEGER)");

	duckdb_v2_result_handle ins = nullptr;
	REQUIRE(Query(fx.conn, "INSERT INTO t VALUES (1), (2)", &ins, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_data_chunk_handle chunk = nullptr;
	REQUIRE(duckdb_v2_result_fetch_chunk(ins, &chunk, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(chunk != nullptr);
	duckdb_v2_vector_handle vec = nullptr;
	duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr);
	duckdb_v2_vector_view view {};
	duckdb_v2_vector_get_view(vec, &view, nullptr);
	REQUIRE(view.data != nullptr);
	REQUIRE(reinterpret_cast<const int64_t *>(view.data)[0] == 2);
	duckdb_v2_data_chunk_destroy(&chunk);

	REQUIRE(duckdb_v2_result_fetch_chunk(ins, &chunk, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(chunk == nullptr); // end-of-stream

	duckdb_v2_result_destroy(&ins);
}

// ===========================================================================
// Disconnecting while a result is alive but undrained: the result's
// internals keep the client context alive, so the handle stays safe to
// consume and destroy after the connection (and database) handles are
// gone.
// ===========================================================================
#if (STANDARD_VECTOR_SIZE == DEFAULT_STANDARD_VECTOR_SIZE)
TEST_CASE("V2: an undrained result survives disconnect and close", "[capi_v2][query_result]") {
	duckdb_v2_environment_handle env = nullptr;
	duckdb_v2_instance_handle instance = nullptr;
	duckdb_v2_connection_handle conn = nullptr;
	REQUIRE(duckdb_v2_environment_create(&env, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(OpenInstance(env, duckdb_v2_str {nullptr, 0}, &instance, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_create(instance, &conn, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(conn, "SELECT i FROM range(100000) t(i)", &r, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto chunk = StepChunk(r);
	REQUIRE(chunk != nullptr);
	duckdb_v2_data_chunk_destroy(&chunk);

	duckdb_v2_connection_destroy(&conn);
	duckdb_v2_instance_destroy(&instance);

	// Metadata still reads off the wrapper.
	REQUIRE(ColumnCount(r) == 1);

	// Consumption keeps working (the stream holds the context alive) or
	// fails cleanly; either way destroy must be safe afterwards.
	idx_t drained = 0;
	while (true) {
		duckdb_v2_data_chunk_handle c = nullptr;
		DUCKDB_V2_RESULT_STEP_STATUS status = DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
		auto rc = duckdb_v2_result_step(r, &c, &status, nullptr);
		if (rc != DUCKDB_V2_ERROR_NONE || status == DUCKDB_V2_RESULT_STEP_STATUS_CANCELLED) {
			break;
		}
		if (c) {
			idx_t size = 0;
			duckdb_v2_data_chunk_get_size(c, &size, nullptr);
			drained += size;
			duckdb_v2_data_chunk_destroy(&c);
		}
		if (status == DUCKDB_V2_RESULT_STEP_STATUS_FINISHED) {
			break;
		}
	}
	(void)drained;
	REQUIRE(duckdb_v2_result_destroy(&r) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_environment_destroy(&env);
}
#endif

// ===========================================================================
// Statement expansion: the engine can turn one user statement into a group
// of internal statements. The group executes as one query with one result:
// the rows of its first row-producing statement, and metadata deferred until
// that statement is bound.
// ===========================================================================

TEST_CASE("V2: PIVOT expands to a group and streams the pivoted rows", "[capi_v2][query_result]") {
	EnvFixture fx;

	ExecSQL(fx.conn, "CREATE TABLE sales (city VARCHAR, year INT, amount INT)");
	ExecSQL(fx.conn,
	        "INSERT INTO sales VALUES ('ams', 2023, 10), ('ams', 2024, 20), ('rtm', 2023, 30), ('rtm', 2024, 40)");

	// Auto-PIVOT parses to one raw statement (a MultiStatement), which the
	// engine expands into CREATE TYPE (enum) + SELECT.
	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "PIVOT sales ON year USING sum(amount)", &r, nullptr) == DUCKDB_V2_ERROR_NONE);

	// The statement type is the one written; the rest is deferred until the SELECT is bound.
	DUCKDB_V2_STATEMENT_TYPE st = DUCKDB_V2_STATEMENT_TYPE_INVALID;
	REQUIRE(duckdb_v2_result_get_statement_type(r, &st, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(st == DUCKDB_V2_STATEMENT_TYPE_MULTI);
	DUCKDB_V2_RESULT_TYPE rt = DUCKDB_V2_RESULT_TYPE_NOTHING;
	RequireSchemaDeferred(r);
	REQUIRE(duckdb_v2_result_get_result_type(r, &rt, nullptr) == DUCKDB_V2_ERROR_INPUT_METADATA_DEFERRED);

	// Only the pivoted SELECT's rows surface: one row per city.
	REQUIRE(DrainRowCount(r) == 2);

	REQUIRE(ColumnCount(r) == 3); // city + one column per year
	REQUIRE(duckdb_v2_result_get_result_type(r, &rt, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(rt == DUCKDB_V2_RESULT_TYPE_QUERY_RESULT);
	REQUIRE(duckdb_v2_result_get_statement_type(r, &st, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(st == DUCKDB_V2_STATEMENT_TYPE_MULTI);

	duckdb_v2_result_destroy(&r);
}
#if (STANDARD_VECTOR_SIZE == DEFAULT_STANDARD_VECTOR_SIZE)
TEST_CASE("V2: ALTER with a non-constant DEFAULT expands and executes fully", "[capi_v2][query_result]") {
	EnvFixture fx;

	ExecSQL(fx.conn, "CREATE TABLE t (i INTEGER)");
	ExecSQL(fx.conn, "INSERT INTO t SELECT * FROM range(1000)");

	// Expands into ALTER ADD (DEFAULT NULL) / UPDATE / ALTER SET DEFAULT, run as one query.
	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "ALTER TABLE t ADD COLUMN c DOUBLE DEFAULT random()", &r, nullptr) == DUCKDB_V2_ERROR_NONE);

	// Metadata deferred while the group runs.
	RequireSchemaDeferred(r);

	// No statement produces rows; the internal UPDATE's Count chunk is
	// discarded, exactly as ClientContext::Query discards it.
	REQUIRE(DrainRowCount(r) == 0);

	DUCKDB_V2_RESULT_TYPE rt = DUCKDB_V2_RESULT_TYPE_QUERY_RESULT;
	REQUIRE(duckdb_v2_result_get_result_type(r, &rt, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(rt == DUCKDB_V2_RESULT_TYPE_NOTHING);
	DUCKDB_V2_STATEMENT_TYPE st = DUCKDB_V2_STATEMENT_TYPE_INVALID;
	REQUIRE(duckdb_v2_result_get_statement_type(r, &st, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(st == DUCKDB_V2_STATEMENT_TYPE_MULTI);
	duckdb_v2_result_destroy(&r);

	// The whole group really ran: the column exists and is populated.
	REQUIRE(Query(fx.conn, "SELECT i FROM t WHERE c IS NOT NULL", &r, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(DrainRowCount(r) == 1000);
	duckdb_v2_result_destroy(&r);
}
#endif
TEST_CASE("V2: result_drain runs an expanded group to completion", "[capi_v2][query_result]") {
	EnvFixture fx;

	ExecSQL(fx.conn, "CREATE TABLE t (i INTEGER)");
	ExecSQL(fx.conn, "INSERT INTO t SELECT * FROM range(100)");

	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "ALTER TABLE t ADD COLUMN c INTEGER DEFAULT (1 + random())::INT", &r, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);

	// Drain applies all statements; the interior UPDATE's count is
	// suppressed (the group reports NOTHING).
	idx_t rows_changed = 99;
	REQUIRE(duckdb_v2_result_drain(r, &rows_changed, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(rows_changed == 0);
	duckdb_v2_result_destroy(&r);

	REQUIRE(Query(fx.conn, "SELECT i FROM t WHERE c IS NOT NULL", &r, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(DrainRowCount(r) == 100);
	duckdb_v2_result_destroy(&r);
}

TEST_CASE("V2: statement_bind on a statement that expands reports deferred metadata", "[capi_v2][query_result]") {
	EnvFixture fx;
	ExecSQL(fx.conn, "CREATE TABLE sales (city VARCHAR, year INT, amount INT)");
	ExecSQL(fx.conn, "INSERT INTO sales VALUES ('ams', 2023, 10), ('rtm', 2024, 40)");

	for (const char *sql :
	     {"PIVOT sales ON year USING sum(amount)", "ALTER TABLE sales ADD COLUMN c DOUBLE DEFAULT random()"}) {
		duckdb_v2_statement_iterator_handle iter = nullptr;
		REQUIRE(duckdb_v2_parse_sql(fx.conn, sql, true, &iter, nullptr) == DUCKDB_V2_ERROR_NONE);
		duckdb_v2_sql_statement_handle stmt = nullptr;
		REQUIRE(duckdb_v2_statement_iterator_next(iter, &stmt, nullptr) == DUCKDB_V2_ERROR_NONE);
		duckdb_v2_schema_handle out = nullptr;
		duckdb_v2_schema_handle params = nullptr;
		REQUIRE(duckdb_v2_statement_bind(fx.conn, stmt, &out, &params, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_METADATA_DEFERRED);
		REQUIRE(out == nullptr);
		REQUIRE(params == nullptr);
		duckdb_v2_sql_statement_destroy(&stmt);
		duckdb_v2_statement_iterator_destroy(&iter);
	}

	// Binding ran nothing: no enum type, no column
	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "SELECT type_name FROM duckdb_types() WHERE type_name LIKE '__pivot_enum%'", &r) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(DrainRowCount(r) == 0);
	duckdb_v2_result_destroy(&r);
	REQUIRE(Query(fx.conn, "SELECT c FROM sales", &r) == DUCKDB_V2_ERROR_QUERY_BINDER);
}

TEST_CASE("V2: a group that ended before its metadata was known reports how it ended", "[capi_v2][query_result]") {
	EnvFixture fx;
	ExecSQL(fx.conn, "CREATE TABLE sales (city VARCHAR, year INT, amount INT)");
	ExecSQL(fx.conn, "INSERT INTO sales VALUES ('ams', 2023, 10)");
	ExecSQL(fx.conn, "CREATE TABLE t AS SELECT range AS i FROM range(2000000)");

	SECTION("failed") {
		// The CREATE TYPE runs, then the SELECT fails to bind
		duckdb_v2_result_handle r = nullptr;
		REQUIRE(Query(fx.conn, "PIVOT sales ON year USING sum(amount) ORDER BY missing_column", &r) ==
		        DUCKDB_V2_ERROR_NONE);
		auto rc = DUCKDB_V2_ERROR_NONE;
		while (rc == DUCKDB_V2_ERROR_NONE) {
			duckdb_v2_data_chunk_handle chunk = nullptr;
			auto status = DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
			rc = duckdb_v2_result_step(r, &chunk, &status, nullptr);
			duckdb_v2_data_chunk_destroy(&chunk);
			REQUIRE(status != DUCKDB_V2_RESULT_STEP_STATUS_FINISHED);
		}
		REQUIRE(rc == DUCKDB_V2_ERROR_QUERY_BINDER);
		duckdb_v2_schema_handle schema = nullptr;
		REQUIRE(duckdb_v2_result_get_schema(r, &schema, nullptr) == DUCKDB_V2_ERROR_QUERY_BINDER);
		auto rt = DUCKDB_V2_RESULT_TYPE_NOTHING;
		REQUIRE(duckdb_v2_result_get_result_type(r, &rt, nullptr) == DUCKDB_V2_ERROR_QUERY_BINDER);
		duckdb_v2_result_destroy(&r);
	}
	SECTION("cancelled") {
		duckdb_v2_result_handle r = nullptr;
		REQUIRE(Query(fx.conn, "ALTER TABLE t ADD COLUMN c DOUBLE DEFAULT random()", &r) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_connection_interrupt(fx.conn, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(StepUntilCancelled(r) == DUCKDB_V2_RESULT_STEP_STATUS_CANCELLED);
		duckdb_v2_schema_handle schema = nullptr;
		REQUIRE(duckdb_v2_result_get_schema(r, &schema, nullptr) == DUCKDB_V2_ERROR_RUNTIME_INTERRUPT);
		duckdb_v2_result_destroy(&r);
	}
}

TEST_CASE("V2: query-backed PRAGMA reparses at statement_execute", "[capi_v2][query_result]") {
	EnvFixture fx;

	// Reparses one-to-one into a row-producing statement, so metadata is
	// available immediately, before the first step.
	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "PRAGMA database_size", &r, nullptr) == DUCKDB_V2_ERROR_NONE);

	REQUIRE(ColumnCount(r) > 0);

	REQUIRE(DrainRowCount(r) >= 1);
	duckdb_v2_result_destroy(&r);
}

TEST_CASE("V2: an error inside an expanded group is sticky and rolls back", "[capi_v2][query_result]") {
	EnvFixture fx;

	ExecSQL(fx.conn, "CREATE TABLE t (i INTEGER)");
	ExecSQL(fx.conn, "INSERT INTO t SELECT * FROM range(100)");

	// The materializing UPDATE fails at execution: '0.xxxx' || 'x' does
	// not convert to INT.
	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "ALTER TABLE t ADD COLUMN c INT DEFAULT ((random()::VARCHAR || 'x')::INT)", &r, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);

	DUCKDB_V2_ERROR rc = DUCKDB_V2_ERROR_NONE;
	duckdb_v2_error_info_handle err = nullptr;
	// Latched, not asserted per round: the round count is timing-dependent.
	bool finished_early = false;
	while (rc == DUCKDB_V2_ERROR_NONE) {
		duckdb_v2_data_chunk_handle chunk = nullptr;
		DUCKDB_V2_RESULT_STEP_STATUS status = DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
		rc = duckdb_v2_result_step(r, &chunk, &status, &err);
		finished_early = finished_early || status == DUCKDB_V2_RESULT_STEP_STATUS_FINISHED;
		if (chunk) {
			duckdb_v2_data_chunk_destroy(&chunk);
		}
	}
	REQUIRE_FALSE(finished_early); // must fail before finishing
	REQUIRE(rc == DUCKDB_V2_ERROR_TYPE_CONVERSION);
	REQUIRE(err != nullptr);
	duckdb_v2_error_info_destroy(&err);

	// Sticky.
	duckdb_v2_data_chunk_handle chunk = nullptr;
	DUCKDB_V2_RESULT_STEP_STATUS status = DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
	REQUIRE(duckdb_v2_result_step(r, &chunk, &status, nullptr) == DUCKDB_V2_ERROR_TYPE_CONVERSION);
	duckdb_v2_result_destroy(&r);

	// The group's transaction rolled back: no column, no partial data.
	REQUIRE(Query(fx.conn, "SELECT c FROM t", &r, nullptr) == DUCKDB_V2_ERROR_QUERY_BINDER);
	REQUIRE(r == nullptr);

	// The connection stays usable.
	REQUIRE(Query(fx.conn, "SELECT count(*) FROM t", &r, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(DrainRowCount(r) == 1);
	duckdb_v2_result_destroy(&r);
}
#if (STANDARD_VECTOR_SIZE == DEFAULT_STANDARD_VECTOR_SIZE)
TEST_CASE("V2: interrupt during an expanded group cancels and rolls back", "[capi_v2][query_result]") {
	EnvFixture fx;

	ExecSQL(fx.conn, "CREATE TABLE t (i INTEGER)");
	ExecSQL(fx.conn, "INSERT INTO t SELECT * FROM range(2000000)");

	QueryResult r;
	REQUIRE(Query(fx.conn, "ALTER TABLE t ADD COLUMN c DOUBLE DEFAULT random()", &r, nullptr) == DUCKDB_V2_ERROR_NONE);

	// Drive part of the group, then interrupt.
	for (int i = 0; i < 20; i++) {
		duckdb_v2_data_chunk_handle chunk = nullptr;
		DUCKDB_V2_RESULT_STEP_STATUS status = DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
		REQUIRE(duckdb_v2_result_step(r, &chunk, &status, nullptr) == DUCKDB_V2_ERROR_NONE);
		if (chunk) {
			duckdb_v2_data_chunk_destroy(&chunk);
		}
	}
	REQUIRE(duckdb_v2_connection_interrupt(fx.conn, nullptr) == DUCKDB_V2_ERROR_NONE);

	DUCKDB_V2_RESULT_STEP_STATUS status = DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
	status = StepUntilCancelled(r);
	REQUIRE(status == DUCKDB_V2_RESULT_STEP_STATUS_CANCELLED);
	duckdb_v2_result_destroy(&r);

	// Nothing of the group was committed.
	REQUIRE(Query(fx.conn, "SELECT c FROM t", &r, nullptr) == DUCKDB_V2_ERROR_QUERY_BINDER);

	// The connection stays usable.
	REQUIRE(Query(fx.conn, "SELECT 1", &r, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(DrainRowCount(r) == 1);
	duckdb_v2_result_destroy(&r);
}
#endif
#if (STANDARD_VECTOR_SIZE == DEFAULT_STANDARD_VECTOR_SIZE)
TEST_CASE("V2: destroying a half-executed expanded group is clean", "[capi_v2][query_result]") {
	EnvFixture fx;

	ExecSQL(fx.conn, "CREATE TABLE t (i INTEGER)");
	ExecSQL(fx.conn, "INSERT INTO t SELECT * FROM range(2000000)");

	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "ALTER TABLE t ADD COLUMN c DOUBLE DEFAULT random()", &r, nullptr) == DUCKDB_V2_ERROR_NONE);
	for (int i = 0; i < 10; i++) {
		duckdb_v2_data_chunk_handle chunk = nullptr;
		DUCKDB_V2_RESULT_STEP_STATUS status = DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
		REQUIRE(duckdb_v2_result_step(r, &chunk, &status, nullptr) == DUCKDB_V2_ERROR_NONE);
		if (chunk) {
			duckdb_v2_data_chunk_destroy(&chunk);
		}
	}
	REQUIRE(duckdb_v2_result_destroy(&r) == DUCKDB_V2_ERROR_NONE);

	// The abandoned group was not committed, and the connection works.
	REQUIRE(Query(fx.conn, "SELECT c FROM t", &r, nullptr) == DUCKDB_V2_ERROR_QUERY_BINDER);
	REQUIRE(Query(fx.conn, "SELECT count(*) FROM t", &r, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(DrainRowCount(r) == 1);
	duckdb_v2_result_destroy(&r);
}

TEST_CASE("V2: an insert that was only waited on is discarded when destroyed", "[capi_v2][query_result]") {
	EnvFixture fx;
	ExecSQL(fx.conn, "CREATE TABLE t (i INTEGER)");

	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "INSERT INTO t SELECT * FROM range(1000)", &r, nullptr) == DUCKDB_V2_ERROR_NONE);
	// Gives the workers time to finish the insert, which waiting must not keep
	for (int i = 0; i < 10; i++) {
		REQUIRE(duckdb_v2_result_wait(r, nullptr) == DUCKDB_V2_ERROR_NONE);
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	REQUIRE(duckdb_v2_result_destroy(&r) == DUCKDB_V2_ERROR_NONE);

	REQUIRE(Query(fx.conn, "SELECT * FROM t", &r, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(DrainRowCount(r) == 0);
	duckdb_v2_result_destroy(&r);
}
#endif

TEST_CASE("V2: a zero-row stream stepped to FINISHED keeps the transaction", "[capi_v2][query_result]") {
	EnvFixture fx;
	ExecSQL(fx.conn, "CREATE SEQUENCE s");
	ExecSQL(fx.conn, "BEGIN TRANSACTION");

	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "SELECT n FROM (SELECT nextval('s') n FROM range(5)) q WHERE n > 100", &r) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(StepChunk(r) == nullptr);
	duckdb_v2_result_destroy(&r);

	ExecSQL(fx.conn, "SELECT 42");
	ExecSQL(fx.conn, "COMMIT");
}

// ===========================================================================
// result_get_schema: a result's output schema as one schema handle.
// ===========================================================================

TEST_CASE("V2: result_get_schema mirrors a SELECT's columns", "[capi_v2][query_result]") {
	EnvFixture fx;
	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "SELECT 1 AS a, 'x' AS b", &r) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_schema_handle schema = nullptr;
	REQUIRE(duckdb_v2_result_get_schema(r, &schema, nullptr) == DUCKDB_V2_ERROR_NONE);
	idx_t count = 0;
	REQUIRE(duckdb_v2_schema_get_count(schema, &count, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(count == 2);
	duckdb_v2_str name = {nullptr, 0};
	duckdb_v2_logical_type_handle type = nullptr;
	REQUIRE(duckdb_v2_schema_get_field(schema, 0, &name, &type, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(std::string(name.ptr, name.len) == "a");
	REQUIRE(duckdb_v2_schema_get_field(schema, 1, &name, &type, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(std::string(name.ptr, name.len) == "b");

	duckdb_v2_schema_destroy(&schema);
	duckdb_v2_result_destroy(&r);
}

TEST_CASE("V2: result_get_schema of a CHANGED_ROWS statement is a BIGINT count", "[capi_v2][query_result]") {
	EnvFixture fx;
	ExecSQL(fx.conn, "CREATE TABLE t(a INTEGER)");
	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "INSERT INTO t VALUES (1), (2)", &r) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_schema_handle schema = nullptr;
	REQUIRE(duckdb_v2_result_get_schema(r, &schema, nullptr) == DUCKDB_V2_ERROR_NONE);
	idx_t count = 0;
	REQUIRE(duckdb_v2_schema_get_count(schema, &count, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(count == 1);
	duckdb_v2_str name = {nullptr, 0};
	duckdb_v2_logical_type_handle type = nullptr;
	REQUIRE(duckdb_v2_schema_get_field(schema, 0, &name, &type, nullptr) == DUCKDB_V2_ERROR_NONE);
	DUCKDB_V2_LOGICAL_TYPE_ID id;
	REQUIRE(duckdb_v2_logical_type_get_id(type, &id, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(id == DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT);

	duckdb_v2_schema_destroy(&schema);
	duckdb_v2_result_destroy(&r);
}

TEST_CASE("V2: result_get_schema null arguments", "[capi_v2][query_result]") {
	EnvFixture fx;
	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "SELECT 1", &r) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_schema_handle schema = reinterpret_cast<duckdb_v2_schema_handle>(0x1);
	REQUIRE(duckdb_v2_result_get_schema(nullptr, &schema, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(schema == nullptr); // pointer out-param nulled on failure
	REQUIRE(duckdb_v2_result_get_schema(r, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	duckdb_v2_result_destroy(&r);
}

// ===========================================================================
// result_render_box: engine-side box rendering, consuming the result by
// transfer (the to_arrow_stream adopt-by-transfer pattern). These tests pin
// the raw transfer semantics; higher-level rendering assertions live in the
// [cpp_api] suite.
// ===========================================================================

TEST_CASE("V2: result_render_box renders, consumes the result, and frees the connection", "[capi_v2][query_result]") {
	EnvFixture fx;
	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "SELECT 1 AS one", &r) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(r != nullptr);

	TextSinkTarget rendered;
	duckdb_v2_error_info_handle err = nullptr;
	auto null_value_str = duckdb_v2_str {nullptr, 0};
	REQUIRE(duckdb_v2_result_render_box(&r, 0, 0, 0, &null_value_str, 0, 0, AppendToString, &rendered, &err) ==
	        DUCKDB_V2_ERROR_NONE);
	// Adopt-by-transfer: the slot is nulled on success.
	REQUIRE(r == nullptr);
	REQUIRE_FALSE(rendered.text.empty());
	// A box-drawing glyph and the column name are present.
	REQUIRE(rendered.text.find("\342\224\202") != std::string::npos);
	REQUIRE(rendered.text.find("one") != std::string::npos);

	// The one-live-result slot was released: a new query runs on the same connection.
	duckdb_v2_result_handle r2 = nullptr;
	REQUIRE(Query(fx.conn, "SELECT 2 AS two", &r2) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(DrainRowCount(r2) == 1);
	REQUIRE(duckdb_v2_result_destroy(&r2) == DUCKDB_V2_ERROR_NONE);
}

TEST_CASE("V2: result_render_box on an execution error consumes the result and reports the error",
          "[capi_v2][query_result]") {
	EnvFixture fx;
	duckdb_v2_result_handle r = nullptr;
	// Execution is lazy: error() (volatile, so never folded) throws only when the
	// stream is stepped, i.e. inside render_box's materialize loop.
	REQUIRE(Query(fx.conn, "SELECT error('boom render') FROM range(5) t(i)", &r) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(r != nullptr);

	TextSinkTarget text; // the sink must not be invoked on the error path
	duckdb_v2_error_info_handle err = nullptr;
	auto null_value_str = duckdb_v2_str {nullptr, 0};
	auto rc = duckdb_v2_result_render_box(&r, 0, 0, 0, &null_value_str, 0, 0, AppendToString, &text, &err);
	REQUIRE(rc != DUCKDB_V2_ERROR_NONE);
	// Consumed by transfer on failure too, and the sink never ran.
	REQUIRE(r == nullptr);
	REQUIRE(text.text.empty());
	// The engine's error text propagated through the slot.
	REQUIRE(err != nullptr);
	duckdb_v2_str msg = {nullptr, 0};
	REQUIRE(duckdb_v2_error_info_get_text(err, &msg) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(std::string(msg.ptr ? msg.ptr : "", msg.len).find("boom render") != std::string::npos);
	duckdb_v2_error_info_destroy(&err);

	// The busy slot was released despite the error: a new query runs.
	duckdb_v2_result_handle r2 = nullptr;
	REQUIRE(Query(fx.conn, "SELECT 7", &r2) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(DrainRowCount(r2) == 1);
	REQUIRE(duckdb_v2_result_destroy(&r2) == DUCKDB_V2_ERROR_NONE);
}

TEST_CASE("V2: result_render_box rejects null arguments and leaves the result intact", "[capi_v2][query_result]") {
	EnvFixture fx;
	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "SELECT 1 AS one", &r) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(r != nullptr);

	// A NULL sink: rejected before adoption, so the result is NOT consumed.
	TextSinkTarget text;
	duckdb_v2_error_info_handle err = nullptr;
	auto null_value_str = duckdb_v2_str {nullptr, 0};
	REQUIRE(duckdb_v2_result_render_box(&r, 0, 0, 0, &null_value_str, 0, 0, nullptr, &text, &err) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(r != nullptr); // intact
	duckdb_v2_error_info_destroy(&err);

	// A NULL result-slot pointer is rejected without a crash.
	REQUIRE(duckdb_v2_result_render_box(nullptr, 0, 0, 0, &null_value_str, 0, 0, AppendToString, &text, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(text.text.empty());

	// The still-intact result renders normally now.
	REQUIRE(duckdb_v2_result_render_box(&r, 0, 0, 0, &null_value_str, 0, 0, AppendToString, &text, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(r == nullptr);
	REQUIRE_FALSE(text.text.empty());
}

// ===========================================================================
// The sink contract: invoked exactly once with the whole box, and its only
// influence on the outcome is the error slot.
// ===========================================================================

TEST_CASE("V2: result_render_box sink contract", "[capi_v2][query_result]") {
	EnvFixture fx;

	// Exactly one invocation, carrying the complete text: a caller can size off
	// text.len without waiting for more.
	{
		duckdb_v2_result_handle r = nullptr;
		REQUIRE(Query(fx.conn, "SELECT * FROM range(200) t(i)", &r) == DUCKDB_V2_ERROR_NONE);
		TextSinkTarget target;
		auto null_value_str = duckdb_v2_str {nullptr, 0};
		REQUIRE(duckdb_v2_result_render_box(&r, 0, 0, 0, &null_value_str, 0, 0, AppendToString, &target, nullptr) ==
		        DUCKDB_V2_ERROR_NONE);
		REQUIRE(r == nullptr);
		REQUIRE(target.calls == 1);
		REQUIRE_FALSE(target.text.empty());
		// A whole box, not a fragment: header rule, column name, and footer.
		REQUIRE(target.text.find("i") != std::string::npos);
		REQUIRE(target.text.find("rows") != std::string::npos);
	}

	// A sink that populates the slot fails the call with its own code, and the
	// message it set survives back out to the caller.
	{
		duckdb_v2_result_handle r = nullptr;
		REQUIRE(Query(fx.conn, "SELECT 1 AS one", &r) == DUCKDB_V2_ERROR_NONE);
		duckdb_v2_error_info_handle err = nullptr;
		auto null_value_str2 = duckdb_v2_str {nullptr, 0};
		REQUIRE(duckdb_v2_result_render_box(&r, 0, 0, 0, &null_value_str2, 0, 0, FailWithIOError, nullptr, &err) ==
		        DUCKDB_V2_ERROR_IO_GENERAL);
		REQUIRE(r == nullptr); // consumed by transfer on failure too
		REQUIRE(err != nullptr);
		duckdb_v2_str message {nullptr, 0};
		REQUIRE(duckdb_v2_error_info_get_text(err, &message) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(Convert(message).find("sink could not write") != std::string::npos);
		duckdb_v2_error_info_destroy(&err);
	}
}

TEST_CASE("V2: result_render_box null_value override and exact footer at the C boundary", "[capi_v2][query_result]") {
	EnvFixture fx;

	// Custom null text: appears instead of "NULL".
	{
		duckdb_v2_result_handle r = nullptr;
		REQUIRE(Query(fx.conn, "SELECT CAST(NULL AS INTEGER) AS b", &r) == DUCKDB_V2_ERROR_NONE);
		TextSinkTarget text;
		auto null_value_str = Convert("<nil>");
		REQUIRE(duckdb_v2_result_render_box(&r, 0, 0, 0, &null_value_str, 0, 0, AppendToString, &text, nullptr) ==
		        DUCKDB_V2_ERROR_NONE);
		const std::string &rendered = text.text;
		REQUIRE(rendered.find("<nil>") != std::string::npos);
		REQUIRE(rendered.find("NULL") == std::string::npos);
	}

	// max_rows bounds display but the footer counts every materialized row.
	{
		duckdb_v2_result_handle r = nullptr;
		REQUIRE(Query(fx.conn, "SELECT i FROM range(100) t(i)", &r) == DUCKDB_V2_ERROR_NONE);
		TextSinkTarget text;
		auto null_value_str2 = duckdb_v2_str {nullptr, 0};
		REQUIRE(duckdb_v2_result_render_box(&r, 4, 0, 0, &null_value_str2, 0, 0, AppendToString, &text, nullptr) ==
		        DUCKDB_V2_ERROR_NONE);
		const std::string &rendered = text.text;
		REQUIRE(rendered.find("100 rows") != std::string::npos); // exact total
		REQUIRE(rendered.find("4 shown") != std::string::npos);  // display bounded to max_rows
	}
}

TEST_CASE("V2: result_render_box validates by-value arguments before consuming the result", "[capi_v2][query_result]") {
	EnvFixture fx;

	// Malformed null_value (null pointer, nonzero length): rejected before
	// adopt-by-transfer, so the caller still owns the result.
	{
		duckdb_v2_result_handle r = nullptr;
		REQUIRE(Query(fx.conn, "SELECT CAST(NULL AS INTEGER) AS b", &r) == DUCKDB_V2_ERROR_NONE);
		TextSinkTarget text;
		duckdb_v2_error_info_handle err = nullptr;
		auto null_value_str = duckdb_v2_str {nullptr, 5};
		REQUIRE(duckdb_v2_result_render_box(&r, 0, 0, 0, &null_value_str, 0, 0, AppendToString, &text, &err) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		REQUIRE(r != nullptr); // intact
		REQUIRE(text.text.empty());
		duckdb_v2_error_info_destroy(&err);
		REQUIRE(duckdb_v2_result_destroy(&r) == DUCKDB_V2_ERROR_NONE);
	}

	// render_mode out of range: same rejection-before-adoption contract.
	{
		duckdb_v2_result_handle r = nullptr;
		REQUIRE(Query(fx.conn, "SELECT 1 AS one", &r) == DUCKDB_V2_ERROR_NONE);
		TextSinkTarget text;
		auto null_value_str2 = duckdb_v2_str {nullptr, 0};
		REQUIRE(duckdb_v2_result_render_box(&r, 0, 0, 0, &null_value_str2, 2, 0, AppendToString, &text, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		REQUIRE(r != nullptr); // intact
		REQUIRE(text.text.empty());
		REQUIRE(duckdb_v2_result_destroy(&r) == DUCKDB_V2_ERROR_NONE);
	}

	// The valid empty null_value forms both render the default "NULL":
	// {NULL, 0} (canonical empty view) and {ptr, 0} (non-null pointer, zero length).
	for (duckdb_v2_str empty_null : {duckdb_v2_str {nullptr, 0}, duckdb_v2_str {"", 0}}) {
		duckdb_v2_result_handle r = nullptr;
		REQUIRE(Query(fx.conn, "SELECT CAST(NULL AS INTEGER) AS b", &r) == DUCKDB_V2_ERROR_NONE);
		TextSinkTarget text;
		REQUIRE(duckdb_v2_result_render_box(&r, 0, 0, 0, &empty_null, 0, 0, AppendToString, &text, nullptr) ==
		        DUCKDB_V2_ERROR_NONE);
		REQUIRE(r == nullptr);
		const std::string &rendered = text.text;
		REQUIRE(rendered.find("NULL") != std::string::npos);
	}
}

TEST_CASE("V2: result_render_box limit renders an approximate footer when the result fills the bound",
          "[capi_v2][query_result]") {
	EnvFixture fx;

	// The caller applied LIMIT 21 upstream and passes it as limit. The result
	// fills the bound exactly, so the true total is unknown: the footer renders
	// "? rows" instead of reporting the truncated count as exact.
	{
		duckdb_v2_result_handle r = nullptr;
		REQUIRE(Query(fx.conn, "SELECT i FROM range(21) t(i)", &r) == DUCKDB_V2_ERROR_NONE);
		TextSinkTarget text;
		auto null_value_str = duckdb_v2_str {nullptr, 0};
		REQUIRE(duckdb_v2_result_render_box(&r, 0, 0, 0, &null_value_str, 0, 21, AppendToString, &text, nullptr) ==
		        DUCKDB_V2_ERROR_NONE);
		const std::string &rendered = text.text;
		REQUIRE(rendered.find("? rows") != std::string::npos);  // count is unknown
		REQUIRE(rendered.find("21 rows") == std::string::npos); // never claims the bound as an exact total
	}

	// The same result rendered with limit 0: the count is known and exact, so no
	// "? rows" approximation appears.
	{
		duckdb_v2_result_handle r = nullptr;
		REQUIRE(Query(fx.conn, "SELECT i FROM range(21) t(i)", &r) == DUCKDB_V2_ERROR_NONE);
		TextSinkTarget text;
		auto null_value_str2 = duckdb_v2_str {nullptr, 0};
		REQUIRE(duckdb_v2_result_render_box(&r, 0, 0, 0, &null_value_str2, 0, 0, AppendToString, &text, nullptr) ==
		        DUCKDB_V2_ERROR_NONE);
		const std::string &rendered = text.text;
		REQUIRE(rendered.find("? rows") == std::string::npos);
	}
}

} // namespace test_capi_v2
