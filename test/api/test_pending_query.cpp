#include "catch.hpp"
#include "test_helpers.hpp"

#include <thread>
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/query_result_stream.hpp"

using namespace duckdb;

TEST_CASE("Test Submitted Query API", "[api][.]") {
	DuckDB db;
	Connection con(db);

	SECTION("Retained result") {
		auto handle = con.Submit("SELECT SUM(i) FROM range(1000000) tbl(i)");
		REQUIRE(!handle->HasError());
		handle->Complete();
		REQUIRE(CHECK_COLUMN(handle, 0, {Value::BIGINT(499999500000)}));

		// the retained result can be read again
		REQUIRE(CHECK_COLUMN(handle, 0, {Value::BIGINT(499999500000)}));

		// query the connection as normal after
		auto result = con.Query("SELECT 42");
		REQUIRE(CHECK_COLUMN(result, 0, {42}));
	}
	SECTION("Streamed result") {
		auto stream = OpenStream(con, "SELECT SUM(i) FROM range(1000000) tbl(i)");
		auto result = DrainStream(*stream);
		REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(499999500000)}));

		// query the connection as normal after
		result = con.Query("SELECT 42");
		REQUIRE(CHECK_COLUMN(result, 0, {42}));
	}
	SECTION("Execute tasks") {
		auto handle = con.Submit("SELECT SUM(i) FROM range(1000000) tbl(i)");
		while (handle->ExecuteTask() == QueryResultState::NOT_READY)
			;
		REQUIRE(!handle->HasError());
		handle->Complete();
		REQUIRE(CHECK_COLUMN(handle, 0, {Value::BIGINT(499999500000)}));

		// query the connection as normal after
		auto result = con.Query("SELECT 42");
		REQUIRE(CHECK_COLUMN(result, 0, {42}));
	}
	SECTION("Submit a query while another submitted query exists") {
		auto handle = con.Submit("SELECT SUM(i) FROM range(1000000) tbl(i)");

		// the connection has an open result, so the second submission is refused
		auto handle2 = con.Submit("SELECT SUM(i) FROM range(1000000) tbl(i)");
		REQUIRE(handle2->HasError());
		REQUIRE(handle2->GetErrorType() == ExceptionType::RESOURCE_IN_USE);

		// the first handle is untouched
		handle->Complete();
		REQUIRE(CHECK_COLUMN(handle, 0, {Value::BIGINT(499999500000)}));

		// once it has ended, the connection takes the next one
		handle2 = con.Submit("SELECT SUM(i) FROM range(1000000) tbl(i)");
		handle2->Complete();
		REQUIRE(CHECK_COLUMN(handle2, 0, {Value::BIGINT(499999500000)}));

		// query the connection as normal after
		auto result = con.Query("SELECT 42");
		REQUIRE(CHECK_COLUMN(result, 0, {42}));
	}
	SECTION("Binding error in submitted query") {
		auto handle = con.Submit("SELECT XXXSUM(i) FROM range(1000000) tbl(i)");
		REQUIRE(handle->HasError());
		REQUIRE_THROWS(handle->ExecuteTask());
		REQUIRE_THROWS(handle->Collection());

		// query the connection as normal after
		auto result = con.Query("SELECT 42");
		REQUIRE(CHECK_COLUMN(result, 0, {42}));
	}
	SECTION("Runtime error in submitted query (retained)") {
		// this succeeds initially
		auto handle = con.Submit("SELECT concat(SUM(i)::varchar, 'hello')::INT FROM range(1000000) tbl(i)");
		REQUIRE(!handle->HasError());
		// we only encounter the failure later on as we are executing the query
		handle->Complete();
		REQUIRE_FAIL(handle);

		// query the connection as normal after
		auto result = con.Query("SELECT 42");
		REQUIRE(CHECK_COLUMN(result, 0, {42}));
	}

	SECTION("Runtime error in submitted query (streamed)") {
		// this succeeds initially
		auto stream = OpenStream(con, "SELECT concat(SUM(i)::varchar, 'hello')::INT FROM range(1000000) tbl(i)");
		auto result = DrainStream(*stream);
		REQUIRE(result->HasError());

		// query the connection as normal after
		result = con.Query("SELECT 42");
		REQUIRE(CHECK_COLUMN(result, 0, {42}));
	}
	SECTION("Submission errors as JSON") {
		con.Query("SET errors_as_json = true;");
		auto handle = con.Submit("SELCT 32;");
		REQUIRE(handle->HasError());
		REQUIRE(duckdb::StringUtil::Contains(handle->GetError(), "SYNTAX_ERROR"));
	}
}

TEST_CASE("Abandoned submitted query must release the active query", "[api]") {
	// A query submitted but never executed must not leak the active-query state (executor, plan,
	// autocommit transaction). We observe the autocommit transaction it opens, which is created and
	// released together with the active query: abandoning the handle must release it immediately, not
	// defer it to the next query or context teardown.
	DuckDB db;
	Connection con(db);

	REQUIRE(!con.context->transaction.HasActiveTransaction());

	SECTION("Abandon via Close()") {
		auto handle = con.Submit("SELECT 42");
		REQUIRE(!handle->HasError());
		REQUIRE(con.context->transaction.HasActiveTransaction());

		handle->Close();
		REQUIRE(!con.context->transaction.HasActiveTransaction());
	}
	SECTION("Abandon an ATTACH via Close()") {
		auto handle = con.Submit("ATTACH ':memory:' AS abandoned_db");
		REQUIRE(!handle->HasError());
		REQUIRE(con.context->transaction.HasActiveTransaction());

		handle->Close();
		REQUIRE(!con.context->transaction.HasActiveTransaction());
	}
	SECTION("Abandon a prepared submitted query") {
		auto prepared = con.Prepare("SELECT 42");
		REQUIRE(!prepared->HasError());
		auto handle = prepared->Submit();
		REQUIRE(!handle->HasError());
		REQUIRE(con.context->transaction.HasActiveTransaction());

		handle->Close();
		REQUIRE(!con.context->transaction.HasActiveTransaction());
	}
	// the connection must remain usable after abandoning submitted queries
	auto result = con.Query("SELECT 42");
	REQUIRE(CHECK_COLUMN(result, 0, {42}));
}

TEST_CASE("Abandoned stream must release the active query", "[api]") {
	// A stream keeps the active-query state alive to feed it; it is normally released when the stream
	// is fully consumed. A stream abandoned before being drained must still release that state, not
	// leak it until the next query or context teardown.
	DuckDB db;
	Connection con(db);

	REQUIRE(!con.context->transaction.HasActiveTransaction());

	SECTION("Abandon via Close() before consuming") {
		auto stream = OpenStream(con, "SELECT * FROM range(10000)");
		REQUIRE(!stream->HasError());
		// the stream is in flight: the active query is still open
		REQUIRE(con.context->transaction.HasActiveTransaction());

		stream->Close();
		REQUIRE(!con.context->transaction.HasActiveTransaction());
	}
	SECTION("Abandon via Close() after a partial fetch") {
		auto stream = OpenStream(con, "SELECT * FROM range(10000)");
		REQUIRE(!stream->HasError());
		auto chunk = stream->Fetch(); // consume one chunk; the stream is not drained
		REQUIRE(chunk);
		REQUIRE(con.context->transaction.HasActiveTransaction());

		stream->Close();
		REQUIRE(!con.context->transaction.HasActiveTransaction());
	}
	// the connection must remain usable after abandoning streams
	auto check = con.Query("SELECT 42");
	REQUIRE(CHECK_COLUMN(check, 0, {42}));
}

TEST_CASE("PROBE cancel a streaming producer parked on a full buffer", "[api][.]") {
	// Force the producer to park on a full buffer (result >> streaming_buffer_size), abandon the
	// stream mid-flight, then run another query so InitialCleanup -> CleanupInternal -> CancelTasks
	// runs against the parked producer. If CancelTasks cannot reap a parked result-collector task,
	// this hangs (busy-spins in `while (executor_tasks > 0) WorkOnTasks()`).
	DuckDB db;
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET streaming_buffer_size='16KB'"));

	SECTION("abandon by dropping, then run another query") {
		auto stream = OpenStream(con, "SELECT * FROM range(10000000)");
		REQUIRE(!stream->HasError());
		auto chunk = stream->Fetch(); // ensure the pipeline is actually streaming and re-parks
		REQUIRE(chunk);
		stream.reset(); // abandon while the producer is parked on the full buffer

		auto check = con.Query("SELECT 42");
		REQUIRE(CHECK_COLUMN(check, 0, {42}));
	}
	SECTION("abandon via Close(), then run another query") {
		auto stream = OpenStream(con, "SELECT * FROM range(10000000)");
		REQUIRE(!stream->HasError());
		auto chunk = stream->Fetch();
		REQUIRE(chunk);
		stream->Close();

		auto check = con.Query("SELECT 42");
		REQUIRE(CHECK_COLUMN(check, 0, {42}));
	}
}

TEST_CASE("Interrupt is observed by QueryResult::ExecuteTask", "[api]") {
	DuckDB db;
	Connection con(db);

	// Single thread + tiny streaming buffer make the parked-collector READY state reachable fast.
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	REQUIRE_NO_FAIL(con.Query("SET streaming_buffer_size='16KB'"));

	auto handle = con.Submit("SELECT * FROM range(10000000)");
	REQUIRE(!handle->HasError());

	QueryResultState state = QueryResultState::NOT_READY;
	for (idx_t i = 0; i < 1000000; i++) {
		state = handle->ExecuteTask();
		if (state == QueryResultState::READY || state == QueryResultState::EXECUTION_ERROR) {
			break;
		}
	}
	REQUIRE(state == QueryResultState::READY);

	con.Interrupt();

	// Without the fix the parked collector keeps reporting READY and the interrupt is never seen.
	bool saw_error = false;
	for (idx_t j = 0; j < 1000; j++) {
		if (handle->ExecuteTask() == QueryResultState::EXECUTION_ERROR) {
			saw_error = true;
			break;
		}
	}
	REQUIRE(saw_error);
}

TEST_CASE("Stream results from materialized CTE exchanges", "[api]") {
	DuckDB db;
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query("SET threads=4"));
	REQUIRE_NO_FAIL(con.Query("SET streaming_buffer_size='16KB'"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE integers AS SELECT i FROM range(1000000) t(i)"));

	SECTION("Direct batch-indexed exchange") {
		auto stream = OpenStream(con, "WITH c AS MATERIALIZED (SELECT i FROM integers) SELECT i FROM c");
		idx_t count = 0;
		while (auto chunk = stream->Fetch()) {
			REQUIRE(chunk->GetValue(0, 0).GetValue<int64_t>() == NumericCast<int64_t>(count));
			count += chunk->size();
		}
		REQUIRE(!stream->HasError());
		REQUIRE(count == 1000000);
	}
	SECTION("Buffered unordered exchange") {
		REQUIRE_NO_FAIL(con.Query("SET preserve_insertion_order=false"));
		auto stream = OpenStream(con, "WITH c AS MATERIALIZED (SELECT i FROM integers) SELECT i FROM c");
		idx_t count = 0;
		while (auto chunk = stream->Fetch()) {
			count += chunk->size();
		}
		REQUIRE(count == 1000000);
	}
	SECTION("Buffered batch-indexed exchange") {
		auto stream = OpenStream(con, "WITH c AS MATERIALIZED ("
		                              "SELECT i FROM integers WHERE i < 500000 "
		                              "UNION ALL "
		                              "SELECT i FROM integers WHERE i >= 500000) "
		                              "SELECT i FROM c");
		idx_t count = 0;
		while (auto chunk = stream->Fetch()) {
			REQUIRE(chunk->GetValue(0, 0).GetValue<int64_t>() == NumericCast<int64_t>(count));
			count += chunk->size();
		}
		REQUIRE(!stream->HasError());
		REQUIRE(count == 1000000);
	}
	SECTION("Abandon buffered batch-indexed exchange") {
		auto stream = OpenStream(con, "WITH c AS MATERIALIZED ("
		                              "SELECT i FROM integers WHERE i < 500000 "
		                              "UNION ALL "
		                              "SELECT i FROM integers WHERE i >= 500000) "
		                              "SELECT i FROM c");
		REQUIRE(!stream->HasError());
		REQUIRE(stream->Fetch());

		stream->Close();
		auto check = con.Query("SELECT 42");
		REQUIRE(CHECK_COLUMN(check, 0, {42}));
	}
	SECTION("Ordered sink pipeline sequencing") {
		auto stream = OpenStream(con, "WITH c1 AS MATERIALIZED (SELECT i FROM range(10000) t(i)), "
		                              "c2 AS MATERIALIZED (SELECT i FROM c1), "
		                              "c3 AS MATERIALIZED (SELECT i + 10000 AS i FROM c1) "
		                              "SELECT i FROM c2 UNION ALL SELECT i FROM c3");
		idx_t count = 0;
		while (auto chunk = stream->Fetch()) {
			REQUIRE(chunk->GetValue(0, 0).GetValue<int64_t>() == NumericCast<int64_t>(count));
			count += chunk->size();
		}
		REQUIRE(count == 20000);
	}
}

static void parallel_submitted_query(Connection *conn, bool *correct, size_t threadnr) {
	correct[threadnr] = true;
	for (size_t i = 0; i < 100; i++) {
		// submit a query and then run it to completion
		auto handle = conn->Submit("SELECT * FROM integers ORDER BY i");
		try {
			// another thread submitting first cancels this one
			handle->Complete();
			if (handle->HasError()) {
				continue;
			}
			if (!CHECK_COLUMN(handle, 0, {1, 2, 3, Value()})) {
				correct[threadnr] = false;
			}
		} catch (...) {
			continue;
		}
	}
}

TEST_CASE("Test parallel usage of the submit API", "[api][.]") {
	auto db = make_uniq<DuckDB>(nullptr);
	auto conn = make_uniq<Connection>(*db);

	REQUIRE_NO_FAIL(conn->Query("CREATE TABLE integers(i INTEGER)"));
	REQUIRE_NO_FAIL(conn->Query("INSERT INTO integers VALUES (1), (2), (3), (NULL)"));

	bool correct[20];
	std::thread threads[20];
	for (size_t i = 0; i < 20; i++) {
		threads[i] = std::thread(parallel_submitted_query, conn.get(), correct, i);
	}
	for (size_t i = 0; i < 20; i++) {
		threads[i].join();
		REQUIRE(correct[i]);
	}
}

TEST_CASE("Test Submit Prepared Statements API", "[api][.]") {
	DuckDB db;
	Connection con(db);

	SECTION("Standard prepared") {
		auto prepare = con.Prepare("SELECT SUM(i) FROM range(1000000) tbl(i) WHERE i>=$1");
		REQUIRE(!prepare->HasError());

		auto handle = prepare->Submit(0);
		REQUIRE(!handle->HasError());

		handle->Complete();
		REQUIRE(CHECK_COLUMN(handle, 0, {Value::BIGINT(499999500000)}));

		// we can use the prepared statement again
		handle = prepare->Submit(500000);
		REQUIRE(!handle->HasError());

		handle->Complete();
		REQUIRE(CHECK_COLUMN(handle, 0, {Value::BIGINT(374999750000)}));
	}
	SECTION("Error during prepare") {
		auto prepare = con.Prepare("SELECT SUM(i+X) FROM range(1000000) tbl(i) WHERE i>=$1");
		REQUIRE(prepare->HasError());

		REQUIRE_FAIL(prepare->Submit(0));
	}
	SECTION("Error during execution") {
		duckdb::vector<Value> parameters;
		auto prepared = con.Prepare("SELECT concat(SUM(i)::varchar, CASE WHEN SUM(i) IS NULL THEN 0 ELSE 'hello' "
		                            "END)::INT FROM range(1000000) tbl(i) WHERE i>$1");
		// this succeeds initially
		parameters = {Value::INTEGER(0)};
		auto handle = prepared->Submit(parameters);
		REQUIRE(!handle->HasError());
		// still succeeds...
		handle->Complete();
		REQUIRE(handle->HasError());

		// query the connection as normal after
		auto result = con.Query("SELECT 42");
		REQUIRE(CHECK_COLUMN(result, 0, {42}));

		// if we change the parameter this works
		parameters = {Value::INTEGER(2000000)};
		handle = prepared->Submit(parameters);

		handle->Complete();
		REQUIRE(!handle->HasError());
		REQUIRE(CHECK_COLUMN(handle, 0, {Value::BIGINT(0)}));
	}
	SECTION("Multiple prepared statements") {
		auto prepare1 = con.Prepare("SELECT SUM(i) FROM range(1000000) tbl(i) WHERE i>=$1");
		auto prepare2 = con.Prepare("SELECT SUM(i) FROM range(1000000) tbl(i) WHERE i<=$1");
		REQUIRE(!prepare1->HasError());
		REQUIRE(!prepare2->HasError());

		// we can execute from both prepared statements individually
		auto handle = prepare1->Submit(500000);
		REQUIRE(!handle->HasError());

		handle->Complete();
		REQUIRE(CHECK_COLUMN(handle, 0, {Value::BIGINT(374999750000)}));

		handle = prepare2->Submit(500000);
		REQUIRE(!handle->HasError());

		handle->Complete();
		REQUIRE(CHECK_COLUMN(handle, 0, {Value::BIGINT(125000250000)}));

		// a result that has ended does not hold the connection, so submission after submission works
		for (idx_t i = 0; i < 10; i++) {
			handle = prepare1->Submit(500000);
			handle->Complete();
			handle = prepare2->Submit(500000);
			handle->Complete();
		}
		REQUIRE(CHECK_COLUMN(handle, 0, {Value::BIGINT(125000250000)}));

		// a result that is still open does hold it
		handle = prepare1->Submit(500000);
		auto handle2 = prepare2->Submit(500000);
		REQUIRE(handle2->HasError());
		REQUIRE(handle2->GetErrorType() == ExceptionType::RESOURCE_IN_USE);

		handle->Complete();
		REQUIRE(CHECK_COLUMN(handle, 0, {Value::BIGINT(374999750000)}));
	}
}

TEST_CASE("Auto-rollback of a failed statement that expands into several via the submit API", "[api]") {
	// A statement that expands into several runs in one transaction, and a failure inside it must roll that back so
	// the connection stays usable without a manual ROLLBACK, on the submit API path too (used by client drivers).
	DuckDB db;
	Connection con(db);
	const char *pivot = "CREATE TEMP TABLE t AS PIVOT (SELECT 'x' AS k, 1 AS v) ON k USING first(does_not_exist)";

	auto handle = con.Submit(string(pivot));
	if (!handle->HasError()) {
		handle->Complete();
	}
	REQUIRE(handle->HasError());
	handle.reset();

	// The connection must be usable again WITHOUT a manual ROLLBACK.
	auto result = con.Query("SELECT 42");
	REQUIRE(CHECK_COLUMN(result, 0, {42}));
}
