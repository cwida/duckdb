#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/execution/executor.hpp"
#include "duckdb/main/buffered_data/buffered_data.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/query_result_stream.hpp"
#include "result_wait_helpers.hpp"

#include <chrono>
#include <thread>

using namespace duckdb;

#ifndef DUCKDB_NO_THREADS

TEST_CASE("A stream drains in order and settles the retention on draining", "[api][query_result_stream]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET max_streaming_buffer_size='100KB'"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT range i FROM range(200000)"));

	// The unordered plan uses the simple store, the table scan the batched one
	for (auto query : {"SELECT i FROM range(200000) t(i)", "SELECT i FROM t"}) {
		auto stream = OpenStream(con, query);
		DrainWatchdog watchdog(con);
		REQUIRE(stream->GetBufferedData().Lifetime() == ResultLifetime::DRAINING);
		int64_t expected = 0;
		while (auto chunk = stream->Fetch()) {
			for (idx_t i = 0; i < chunk->size(); i++) {
				REQUIRE(chunk->GetValue(0, i).GetValue<int64_t>() == expected);
				expected++;
			}
		}
		REQUIRE(!stream->HasError());
		REQUIRE(expected == 200000);
		// The guarantee is the cap plus one chunk: an admission into an empty buffer may exceed it
		// by at most the admitted chunk
		REQUIRE(stream->GetBufferedData().PeakBufferedBytes() <= 100000 + 100000);
	}
}

TEST_CASE("The stream constructor refuses a handle it cannot drain", "[api][query_result_stream]") {
	DuckDB db(nullptr);
	Connection con(db);

	SECTION("an error handle") {
		auto handle = con.Submit("SELECT * FROM no_such_table");
		REQUIRE(handle->HasError());
		REQUIRE_THROWS_AS(QueryResultStream<>(std::move(handle)), InvalidInputException);
	}
	SECTION("a handle whose retention is already retained") {
		auto handle = con.Submit("SELECT i FROM range(1000) t(i)");
		handle->Materialize();
		REQUIRE_THROWS_AS(QueryResultStream<>(std::move(handle)), InvalidInputException);
	}
	// A refused stream released the query it consumed
	auto next = con.Query("SELECT 42");
	REQUIRE(CHECK_COLUMN(next, 0, {42}));
}

TEST_CASE("A query that produces no rows yields an empty stream", "[api][query_result_stream]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT range i FROM range(1000)"));

	auto stream = OpenStream(con, "SELECT i FROM t WHERE i < 0");
	unique_ptr<DataChunk> chunk;
	Deadline deadline;
	QueryResultState state = QueryResultState::NOT_READY;
	while (!IsTerminal(state)) {
		state = stream->TryFetch(chunk);
		REQUIRE(!chunk);
		REQUIRE(!deadline.Passed());
	}
	REQUIRE(state == QueryResultState::FINISHED);
	// The terminal state keeps repeating
	REQUIRE(stream->TryFetch(chunk) == QueryResultState::FINISHED);
	REQUIRE(stream->Poll() == QueryResultState::FINISHED);
	REQUIRE(!stream->Fetch());
}

TEST_CASE("A single-threaded consumer drains the stream itself", "[api][query_result_stream]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	REQUIRE_NO_FAIL(con.Query("SET max_streaming_buffer_size='16KB'"));

	auto stream = OpenStream(con, "SELECT i FROM range(100000) t(i)");
	DrainWatchdog watchdog(con);
	idx_t rows = 0;
	Deadline deadline;
	QueryResultState state = QueryResultState::NOT_READY;
	while (!IsTerminal(state)) {
		unique_ptr<DataChunk> chunk;
		state = stream->TryFetch(chunk);
		if (state == QueryResultState::READY) {
			rows += chunk->size();
			continue;
		}
		if (IsTerminal(state)) {
			break;
		}
		// No worker will do it for us: the consumer runs the tasks
		if (stream->ExecuteTask() == QueryResultState::BLOCKED) {
			stream->WaitForTask();
		}
		REQUIRE(!deadline.Passed());
	}
	REQUIRE(state == QueryResultState::FINISHED);
	REQUIRE(rows == 100000);
}

TEST_CASE("An interrupt terminates a running stream", "[api][query_result_stream]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET max_streaming_buffer_size='16KB'"));

	auto stream = OpenStream(con, "SELECT i FROM range(100000000000) t(i) WHERE i % 1000000 = 0");

	unique_ptr<DataChunk> chunk;
	stream->TryFetch(chunk);
	con.Interrupt();

	Deadline deadline;
	QueryResultState state;
	while (!IsTerminal(state = stream->TryFetch(chunk))) {
		REQUIRE(!deadline.Passed());
	}
	REQUIRE(state == QueryResultState::EXECUTION_ERROR);
	REQUIRE(StringUtil::Contains(stream->GetError(), "INTERRUPT"));

	con.context->ClearInterrupt();
	auto next = con.Query("SELECT 42");
	REQUIRE(CHECK_COLUMN(next, 0, {42}));
}

TEST_CASE("An interrupt surfaces on an idle engine at the consumer's next call", "[api][query_result_stream]") {
	DuckDB db(nullptr);
	Connection con(db);
	// No worker threads: no task reaches an interrupt check, so the call itself must observe the flag
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));

	auto stream = OpenStream(con, "SELECT i FROM range(1000000) t(i)");
	con.Interrupt();

	unique_ptr<DataChunk> chunk;
	REQUIRE(stream->TryFetch(chunk) == QueryResultState::EXECUTION_ERROR);
	REQUIRE(StringUtil::Contains(stream->GetError(), "INTERRUPT"));
	REQUIRE(!stream->IsOpen());

	con.context->ClearInterrupt();
	auto next = con.Query("SELECT 42");
	REQUIRE(CHECK_COLUMN(next, 0, {42}));
}

TEST_CASE("An error after the first chunk surfaces at the consumer's next call", "[api][query_result_stream]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=2"));
	// A cap below one chunk: a chunk is always buffered when the failing row is produced
	REQUIRE_NO_FAIL(con.Query("SET max_streaming_buffer_size='1b'"));
	const auto rows = to_string(8 * STANDARD_VECTOR_SIZE);
	const auto boom = to_string(4 * STANDARD_VECTOR_SIZE);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT range i FROM range(" + rows + ")"));

	const string cast = "SELECT (CASE WHEN i = " + boom + " THEN 'boom' ELSE i::VARCHAR END)::INT ";
	for (auto source : {"FROM range(" + rows + ") t(i)", string("FROM t")}) {
		auto stream = OpenStream(con, cast + source);
		DrainWatchdog watchdog(con);
		// The submission itself succeeded: the failure is only found while producing
		REQUIRE(!stream->HasError());

		unique_ptr<DataChunk> chunk;
		Deadline deadline;
		QueryResultState state = QueryResultState::NOT_READY;
		while (!IsTerminal(state)) {
			state = stream->TryFetch(chunk);
			REQUIRE(!deadline.Passed());
		}
		REQUIRE(state == QueryResultState::EXECUTION_ERROR);
		REQUIRE(StringUtil::Contains(stream->GetError(), "boom"));
		// The buffer still held a chunk when the error landed; none of it is reported afterwards
		REQUIRE(!chunk);
		REQUIRE(stream->TryFetch(chunk) == QueryResultState::EXECUTION_ERROR);
		REQUIRE(!chunk);

		auto next = con.Query("SELECT 42");
		REQUIRE(CHECK_COLUMN(next, 0, {42}));
	}
}

TEST_CASE("A mid-stream fetch failure invalidates the open transaction", "[api][query_result_stream]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT range i FROM range(20000)"));
	REQUIRE_NO_FAIL(con.Query("BEGIN TRANSACTION"));

	auto stream = OpenStream(con, "SELECT (CASE WHEN i = 10000 THEN 'boom' ELSE i::VARCHAR END)::INT FROM t");
	DrainWatchdog watchdog(con);
	while (auto chunk = stream->Fetch()) {
	}
	REQUIRE(stream->HasError());

	// The failure invalidated the transaction, per the connection's invalidation policy
	auto next = con.Query("SELECT 42");
	REQUIRE(next->HasError());
	REQUIRE(StringUtil::Contains(next->GetError(), "aborted"));
	REQUIRE_NO_FAIL(con.Query("ROLLBACK"));
	auto after = con.Query("SELECT 42");
	REQUIRE(CHECK_COLUMN(after, 0, {42}));
}

TEST_CASE("Closing a partly drained stream leaves the connection usable", "[api][query_result_stream]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET max_streaming_buffer_size='16KB'"));

	auto stream = OpenStream(con, "SELECT i FROM range(1000000) t(i)");
	DrainWatchdog watchdog(con);
	REQUIRE(stream->Fetch());
	stream->Close();
	REQUIRE(!stream->HasError());

	auto next = con.Query("SELECT 42");
	REQUIRE(CHECK_COLUMN(next, 0, {42}));
}

TEST_CASE("Closing a partly drained read-only stream keeps the open transaction", "[api][query_result_stream]") {
	DuckDB db(nullptr);
	Connection con(db);
	Connection observer(db);
	REQUIRE_NO_FAIL(con.Query("SET max_streaming_buffer_size='16KB'"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i BIGINT)"));
	REQUIRE_NO_FAIL(con.Query("BEGIN TRANSACTION"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t VALUES (42)"));

	auto stream = OpenStream(con, "SELECT i FROM range(1000000) t(i)");
	DrainWatchdog watchdog(con);
	REQUIRE(stream->Fetch());
	stream->Close();

	REQUIRE_NO_FAIL(con.Query("SELECT 42"));
	REQUIRE_NO_FAIL(con.Query("COMMIT"));
	auto rows = observer.Query("SELECT i FROM t");
	REQUIRE(CHECK_COLUMN(rows, 0, {42}));
}

TEST_CASE("Closing a partly drained stream over a temporary table keeps the open transaction",
          "[api][query_result_stream]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET max_streaming_buffer_size='16KB'"));
	REQUIRE_NO_FAIL(con.Query("CREATE TEMP TABLE t AS SELECT range i FROM range(1000000)"));
	REQUIRE_NO_FAIL(con.Query("BEGIN TRANSACTION"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t VALUES (-1)"));

	auto stream = OpenStream(con, "SELECT i FROM t");
	DrainWatchdog watchdog(con);
	REQUIRE(stream->Fetch());
	stream->Close();

	REQUIRE_NO_FAIL(con.Query("COMMIT"));
	auto count = con.Query("SELECT count(*) FROM t");
	REQUIRE(CHECK_COLUMN(count, 0, {1000001}));
}

TEST_CASE("Closing a stream of a writing SELECT before its end invalidates the open transaction",
          "[api][query_result_stream]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=2"));
	REQUIRE_NO_FAIL(con.Query("SET max_streaming_buffer_size='1MB'"));
	REQUIRE_NO_FAIL(con.Query("CREATE SEQUENCE s"));
	REQUIRE_NO_FAIL(con.Query("BEGIN TRANSACTION"));

	auto stream = OpenStream(con, "SELECT nextval('s') FROM range(10000)");
	REQUIRE(WaitForExecution(con));
	// Execution is done and the buffer holds the rest, but the consumer never reached the end
	REQUIRE(stream->Fetch());
	REQUIRE(stream->GetBufferedData().HasObservableUnit());
	stream->Close();

	auto next = con.Query("SELECT 42");
	REQUIRE(next->HasError());
	REQUIRE(StringUtil::Contains(next->GetError(), "aborted"));
	REQUIRE_NO_FAIL(con.Query("ROLLBACK"));
	auto after = con.Query("SELECT 42");
	REQUIRE(CHECK_COLUMN(after, 0, {42}));
}

TEST_CASE("Abandoning a streamed RETURNING insert discards its writes", "[api][query_result_stream]") {
	DuckDB db(nullptr);
	Connection con(db);
	Connection observer(db);
	REQUIRE_NO_FAIL(con.Query("SET max_streaming_buffer_size='16KB'"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i BIGINT)"));

	auto stream = OpenStream(con, "INSERT INTO t SELECT i FROM range(1000000) t(i) RETURNING i");
	DrainWatchdog watchdog(con);
	REQUIRE(stream->Fetch());
	stream->Close();
	REQUIRE(!stream->HasError());

	auto count = observer.Query("SELECT count(*) FROM t");
	REQUIRE(CHECK_COLUMN(count, 0, {0}));
}

TEST_CASE("Abandoning a streamed RETURNING insert invalidates the open transaction", "[api][query_result_stream]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET max_streaming_buffer_size='16KB'"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i BIGINT)"));
	REQUIRE_NO_FAIL(con.Query("BEGIN TRANSACTION"));

	auto stream = OpenStream(con, "INSERT INTO t SELECT i FROM range(1000000) t(i) RETURNING i");
	DrainWatchdog watchdog(con);
	REQUIRE(stream->Fetch());
	stream->Close();

	auto next = con.Query("SELECT 42");
	REQUIRE(next->HasError());
	REQUIRE(StringUtil::Contains(next->GetError(), "aborted"));
	REQUIRE_NO_FAIL(con.Query("ROLLBACK"));
	auto count = con.Query("SELECT count(*) FROM t");
	REQUIRE(CHECK_COLUMN(count, 0, {0}));
}

TEST_CASE("A fully drained RETURNING insert keeps its rows", "[api][query_result_stream]") {
	DuckDB db(nullptr);
	Connection con(db);
	Connection observer(db);
	REQUIRE_NO_FAIL(con.Query("SET max_streaming_buffer_size='16KB'"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i BIGINT)"));

	auto stream = OpenStream(con, "INSERT INTO t SELECT i FROM range(1000000) t(i) RETURNING i");
	DrainWatchdog watchdog(con);
	idx_t rows = 0;
	while (auto chunk = stream->Fetch()) {
		rows += chunk->size();
	}
	REQUIRE(!stream->HasError());
	REQUIRE(rows == 1000000);
	stream->Close();

	auto count = observer.Query("SELECT count(*) FROM t");
	REQUIRE(CHECK_COLUMN(count, 0, {1000000}));
}

TEST_CASE("A CALL statement streams its rows", "[api][query_result_stream]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET max_streaming_buffer_size='16KB'"));

	auto stream = OpenStream(con, "CALL range(5000)");
	DrainWatchdog watchdog(con);
	idx_t rows = 0;
	while (auto chunk = stream->Fetch()) {
		rows += chunk->size();
	}
	REQUIRE(!stream->HasError());
	REQUIRE(rows == 5000);
}

#endif
