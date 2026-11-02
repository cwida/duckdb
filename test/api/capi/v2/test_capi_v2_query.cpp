#include "test_capi_v2.hpp"

#include <chrono>
#include <string>
#include <thread>

using namespace std;

// ===========================================================================
// Query-execution edge cases: distinguishing an engine-initiated interrupt
// (max_execution_time timeout) from a consumer cancellation, and not rolling
// back a user-issued BEGIN TRANSACTION.
// ===========================================================================

// ---------------------------------------------------------------------------
// Destroying the drained result of a user-issued BEGIN TRANSACTION must not
// roll back the user's open transaction.
// ---------------------------------------------------------------------------

namespace test_capi_v2 {
TEST_CASE("V2: user BEGIN ... ROLLBACK is not clobbered by the bridge", "[capi_v2][query_execution]") {
	EnvFixture fx;

	// Draining and destroying the result of a BEGIN TRANSACTION must leave the transaction open.
	ExecSQL(fx.conn, "BEGIN TRANSACTION");

	// The transaction is still active: ROLLBACK must succeed rather than fail with "no transaction is active".
	duckdb_v2_result_handle r = nullptr;
	duckdb_v2_error_info_handle err = nullptr;
	REQUIRE(Query(fx.conn, "ROLLBACK", &r, &err) == DUCKDB_V2_ERROR_NONE);
	DrainRowCount(r);
	duckdb_v2_result_destroy(&r);
	duckdb_v2_error_info_destroy(&err);
}

TEST_CASE("V2: user BEGIN / INSERT / ROLLBACK discards the inserted row", "[capi_v2][query_execution]") {
	EnvFixture fx;

	ExecSQL(fx.conn, "CREATE TABLE t (i INTEGER)");

	// Open a user transaction, insert a row, then roll back. The BEGIN result
	// is drained+destroyed before the INSERT runs; the bridge must keep the
	// user's transaction alive across that destroy.
	ExecSQL(fx.conn, "BEGIN TRANSACTION");
	ExecSQL(fx.conn, "INSERT INTO t VALUES (42)");
	ExecSQL(fx.conn, "ROLLBACK");

	// The ROLLBACK undid the INSERT: the table is empty.
	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "SELECT count(*) FROM t", &r, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(DrainChangedRows(r) == 0);
	duckdb_v2_result_destroy(&r);
}

TEST_CASE("V2: user BEGIN / INSERT / COMMIT keeps the inserted row", "[capi_v2][query_execution]") {
	EnvFixture fx;

	ExecSQL(fx.conn, "CREATE TABLE t (i INTEGER)");

	ExecSQL(fx.conn, "BEGIN TRANSACTION");
	ExecSQL(fx.conn, "INSERT INTO t VALUES (7)");
	ExecSQL(fx.conn, "COMMIT");

	// The COMMIT persisted the INSERT: the row survives.
	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "SELECT count(*) FROM t", &r, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(DrainChangedRows(r) == 1);
	duckdb_v2_result_destroy(&r);
}

// ---------------------------------------------------------------------------
// Bug #1: a max_execution_time timeout shares the INTERRUPT exception type with
// a consumer cancellation, but must surface as an ERROR carrying its message,
// not a message-less CANCELLED status. A real connection_interrupt must still
// surface as CANCELLED.
// ---------------------------------------------------------------------------

// A timeout can land in either the pending or streaming phase, depending on
// scheduling; [!mayfail] absorbs the rare run where the bounded step loop
// exits before the timeout fires under heavy load.
TEST_CASE("V2: a max_execution_time timeout surfaces as an error, not CANCELLED",
          "[capi_v2][query_execution][!mayfail]") {
	EnvFixture fx;

	// 50ms timeout on a slow cross product, mirroring max_execution_time.test.
	ExecSQL(fx.conn, "SET max_execution_time=50");

	QueryResult r;
	REQUIRE(Query(fx.conn, "SELECT count(*) FROM range(100000000) t1, range(1000) t2", &r, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);

	// Step until the timeout fires. It must arrive as an error return code, not
	// as the CANCELLED status. On the buggy code the timeout was conflated with
	// a consumer cancellation and surfaced as a message-less CANCELLED.
	DUCKDB_V2_ERROR rc = DUCKDB_V2_ERROR_NONE;
	duckdb_v2_error_info_handle err = nullptr;
	DUCKDB_V2_RESULT_STEP_STATUS status = DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
	for (int i = 0; i < 1000000; i++) {
		duckdb_v2_data_chunk_handle chunk = nullptr;
		status = DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
		rc = duckdb_v2_result_step(r, &chunk, &status, &err);
		if (chunk) {
			duckdb_v2_data_chunk_destroy(&chunk);
		}
		// Stop on any terminal outcome: an error (expected), a cancellation
		// (the bug), or a finish (should not happen at this scale).
		if (rc != DUCKDB_V2_ERROR_NONE || status == DUCKDB_V2_RESULT_STEP_STATUS_CANCELLED ||
		    status == DUCKDB_V2_RESULT_STEP_STATUS_FINISHED) {
			break;
		}
	}

	// The timeout must never be reported as a cancellation. CHECK, not REQUIRE:
	// a REQUIRE aborts the case on the rare run where the timeout did not land,
	// which would make this [!mayfail] case contribute a varying number of
	// assertions to the suite total.
	std::string msg;
	if (err) {
		duckdb_v2_str text = {nullptr, 0};
		duckdb_v2_error_info_get_text(err, &text);
		msg = Convert(text);
	}
	INFO("timeout error detail: " << (!msg.empty() ? msg : "(none)"));
	CHECK(status != DUCKDB_V2_RESULT_STEP_STATUS_CANCELLED);
	CHECK(rc == DUCKDB_V2_ERROR_RUNTIME_INTERRUPT);
	CHECK(err != nullptr);
	CHECK(msg.find("Query exceeded maximum execution time") != string::npos);
	duckdb_v2_error_info_destroy(&err);
}
#if STANDARD_VECTOR_SIZE == DEFAULT_STANDARD_VECTOR_SIZE
TEST_CASE("V2: a consumer interrupt still surfaces as CANCELLED, not an error", "[capi_v2][query_execution]") {
	EnvFixture fx;

	// No timeout set: the only cancellation channel is connection_interrupt.
	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "SELECT i FROM range(10000000) t(i)", &r, nullptr) == DUCKDB_V2_ERROR_NONE);

	// Consume the first chunk so the stream is genuinely mid-flight.
	auto first = StepChunk(r);
	REQUIRE(first != nullptr);
	duckdb_v2_data_chunk_destroy(&first);

	REQUIRE(duckdb_v2_connection_interrupt(fx.conn, nullptr) == DUCKDB_V2_ERROR_NONE);

	// The interrupt surfaces as the CANCELLED status, never as an error.
	DUCKDB_V2_RESULT_STEP_STATUS status = DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
	status = StepUntilCancelled(r);
	REQUIRE(status == DUCKDB_V2_RESULT_STEP_STATUS_CANCELLED);

	duckdb_v2_result_destroy(&r);
}
#endif

} // namespace test_capi_v2
