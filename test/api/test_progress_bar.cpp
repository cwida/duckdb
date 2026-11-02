#ifndef DUCKDB_NO_THREADS

#include "catch.hpp"
#include "duckdb/common/progress_bar/display/terminal_progress_bar_display.hpp"
#include "duckdb/common/progress_bar/progress_bar.hpp"
#include "duckdb/main/client_context.hpp"
#include "test_helpers.hpp"

#include <duckdb/execution/executor.hpp>
#include <future>
#include <thread>

using namespace duckdb;

TEST_CASE("Test terminal progress ETA estimate", "[progress-bar]") {
	REQUIRE(TerminalProgressBarDisplay::EstimateRemainingSeconds(0, 10) == Approx(2147483647.0));
	REQUIRE(TerminalProgressBarDisplay::EstimateRemainingSeconds(25, 10) == Approx(30));
	REQUIRE(TerminalProgressBarDisplay::EstimateRemainingSeconds(50, 100) == Approx(100));
	REQUIRE(TerminalProgressBarDisplay::EstimateRemainingSeconds(100, 10) == Approx(0));
	REQUIRE(TerminalProgressBarDisplay::EstimateRemainingSeconds(50, 100, 1) == Approx(50));
	REQUIRE(TerminalProgressBarDisplay::EstimateRemainingSeconds(50, 100, 4) == Approx(50));
	REQUIRE(TerminalProgressBarDisplay::EstimateRemainingSeconds(50, 100, 0.1) == Approx(200));
}

class TestProgressBar {
	class TestFailure {
		using failure_callback = std::function<void()>;

	public:
		TestFailure() : callback(nullptr) {
		}

	public:
		bool IsSet() {
			return callback != nullptr;
		}
		void SetError(failure_callback failure) {
			if (!callback) {
				callback = failure;
			}
		}
		void ThrowError() {
			D_ASSERT(IsSet());
			callback();
		}

	private:
		failure_callback callback;
	};

public:
	explicit TestProgressBar(ClientContext *context) : context(context) {
	}

	ClientContext *context;
	atomic<bool> stop;
	std::thread check_thread;
	TestFailure error;

	void CheckProgressThread() {
		double prev_percentage = -1;
		uint64_t total_cardinality = 0;
		uint64_t cur_rows_read = 0;
		while (!stop) {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			auto query_progress = context->GetQueryProgress();
			double new_percentage = query_progress.GetPercentage();
			if (new_percentage < prev_percentage && new_percentage != -1) {
				error.SetError([new_percentage, prev_percentage]() { REQUIRE(new_percentage >= prev_percentage); });
			}
			if (new_percentage > 100) {
				error.SetError([new_percentage]() { REQUIRE(new_percentage <= 100); });
			}
			cur_rows_read = query_progress.GetRowsProcessed();
			total_cardinality = query_progress.GetTotalRowsToProcess();
			if (cur_rows_read > total_cardinality) {
				error.SetError([cur_rows_read, total_cardinality]() { REQUIRE(cur_rows_read <= total_cardinality); });
			}
		}
		if (cur_rows_read != total_cardinality) {
			if (std::getenv("FORCE_ASYNC_SINK_SOURCE") != nullptr) {
				return;
			}
			error.SetError([cur_rows_read, total_cardinality]() { REQUIRE(cur_rows_read == total_cardinality); });
		}
	}
	void Start() {
		stop = false;
		check_thread = std::thread(&TestProgressBar::CheckProgressThread, this);
	}
	void End() {
		stop = true;
		check_thread.join();
		if (error.IsSet()) {
			error.ThrowError();
			// This should never be reached, ThrowError() should contain a failing REQUIRE statement
			REQUIRE(false);
		}
	}
};

TEST_CASE("Test Progress Bar Fast", "[progress-bar]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NOTHROW(con.context->GetQueryProgress());

	TestProgressBar test_progress(con.context.get());

	REQUIRE_NOTHROW(con.context->GetQueryProgress());

	REQUIRE_NO_FAIL(con.Query("create  table tbl as select range a, mod(range,10) b from range(10000);"));
	REQUIRE_NO_FAIL(con.Query("create  table tbl_2 as select range a from range(10000);"));

	REQUIRE_NO_FAIL(con.Query("PRAGMA progress_bar_time=10"));
	REQUIRE_NO_FAIL(con.Query("PRAGMA disable_print_progress_bar"));
	// Simple Aggregation
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl"));
	test_progress.End();

	// Simple Join
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl inner join tbl_2 on (tbl.a = tbl_2.a)"));
	test_progress.End();

	// Subquery
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl where a = (select min(a) from tbl_2)"));
	test_progress.End();

	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl where a = (select min(b) from tbl)"));
	test_progress.End();

	// Stream result
	test_progress.Start();
	auto stream = OpenStream(con, "select count(*) from tbl inner join tbl_2 on (tbl.a = tbl_2.a)");
	// The query only completes once the stream is consumed
	while (stream->Fetch()) {
	}
	test_progress.End();
	REQUIRE(!stream->HasError());

	// Test Multiple threads
	REQUIRE_NO_FAIL(con.Query("PRAGMA threads=2"));
	REQUIRE_NO_FAIL(con.Query("PRAGMA verify_parallelism"));

	// Simple Aggregation
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl"));
	test_progress.End();

	// Simple Join
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl inner join tbl_2 on (tbl.a = tbl_2.a)"));
	test_progress.End();

	// Subquery
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl where a = (select min(a) from tbl_2)"));
	test_progress.End();

	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl where a = (select min(b) from tbl)"));
	test_progress.End();

	// Stream result
	test_progress.Start();
	stream = OpenStream(con, "select count(*) from tbl inner join tbl_2 on (tbl.a = tbl_2.a)");
	while (stream->Fetch()) {
	}
	test_progress.End();
	REQUIRE(!stream->HasError());
}

TEST_CASE("Test Progress Bar", "[progress-bar][.]") {
	DuckDB db(nullptr);
	Connection con(db);
	TestProgressBar test_progress(con.context.get());
	REQUIRE_NO_FAIL(con.Query("create  table tbl as select range a, mod(range,10) b from range(10000000);"));
	REQUIRE_NO_FAIL(con.Query("create  table tbl_2 as select range a from range(10000000);"));

	REQUIRE_NO_FAIL(con.Query("PRAGMA progress_bar_time=10"));
	REQUIRE_NO_FAIL(con.Query("PRAGMA disable_print_progress_bar"));
	// Simple Aggregation
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl"));
	test_progress.End();

	// Simple Join
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl inner join tbl_2 on (tbl.a = tbl_2.a)"));
	test_progress.End();

	// Subquery
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl where a = (select min(a) from tbl_2)"));
	test_progress.End();

	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl where a = (select min(b) from tbl)"));
	test_progress.End();

	// Stream result
	test_progress.Start();
	auto stream = OpenStream(con, "select count(*) from tbl inner join tbl_2 on (tbl.a = tbl_2.a)");
	// The query only completes once the stream is consumed
	while (stream->Fetch()) {
	}
	test_progress.End();
	REQUIRE(!stream->HasError());

	// Test Multiple threads
	REQUIRE_NO_FAIL(con.Query("PRAGMA threads=4"));
	REQUIRE_NO_FAIL(con.Query("PRAGMA verify_parallelism"));

	// Simple Aggregation
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl"));
	test_progress.End();

	// Simple Join
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl inner join tbl_2 on (tbl.a = tbl_2.a)"));
	test_progress.End();

	// Subquery
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl where a = (select min(a) from tbl_2)"));
	test_progress.End();

	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl where a = (select min(b) from tbl)"));
	test_progress.End();

	// Stream result
	test_progress.Start();
	stream = OpenStream(con, "select count(*) from tbl inner join tbl_2 on (tbl.a = tbl_2.a)");
	while (stream->Fetch()) {
	}
	test_progress.End();
	REQUIRE(!stream->HasError());
}

TEST_CASE("Test Progress Bar CSV", "[progress-bar][.]") {
	DuckDB db(nullptr);
	Connection con(db);

	TestProgressBar test_progress(con.context.get());
	REQUIRE_NO_FAIL(con.Query("PRAGMA progress_bar_time=1"));
	REQUIRE_NO_FAIL(con.Query("PRAGMA disable_print_progress_bar"));

	// Create Tables From CSVs
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE test AS SELECT * FROM read_csv_auto ('data/csv/test/test.csv')"));
	test_progress.End();

	test_progress.Start();
	REQUIRE_NO_FAIL(
	    con.Query("CREATE TABLE test_2 AS SELECT * FROM  read_csv('data/csv/test/test.csv', columns=STRUCT_PACK(a "
	              ":= 'INTEGER', b := 'INTEGER', c := 'VARCHAR'), sep=',', auto_detect='false')"));
	test_progress.End();

	// Insert into existing tables
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("INSERT INTO test SELECT * FROM read_csv_auto('data/csv/test/test.csv')"));
	test_progress.End();

	test_progress.Start();
	REQUIRE_NO_FAIL(
	    con.Query("INSERT INTO test SELECT * FROM  read_csv('data/csv/test/test.csv', columns=STRUCT_PACK(a := "
	              "'INTEGER', b := 'INTEGER', c := 'VARCHAR'), sep=',', auto_detect='false')"));
	test_progress.End();

	// copy from
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("COPY test FROM 'data/csv/test/test.csv'"));
	test_progress.End();

	// Repeat but in parallel
	REQUIRE_NO_FAIL(con.Query("DROP TABLE test"));
	REQUIRE_NO_FAIL(con.Query("DROP TABLE test_2"));

	// Test Multiple threads
	REQUIRE_NO_FAIL(con.Query("PRAGMA threads=4"));
	REQUIRE_NO_FAIL(con.Query("PRAGMA verify_parallelism"));
	// Create Tables From CSVs
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE test AS SELECT * FROM read_csv_auto ('data/csv/test/test.csv')"));
	test_progress.End();

	test_progress.Start();
	REQUIRE_NO_FAIL(
	    con.Query("CREATE TABLE test_2 AS SELECT * FROM  read_csv('data/csv/test/test.csv', columns=STRUCT_PACK(a "
	              ":= 'INTEGER', b := 'INTEGER', c := 'VARCHAR'), sep=',', auto_detect='false')"));
	test_progress.End();

	// Insert into existing tables
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("INSERT INTO test SELECT * FROM read_csv_auto('data/csv/test/test.csv')"));
	test_progress.End();

	test_progress.Start();
	REQUIRE_NO_FAIL(
	    con.Query("INSERT INTO test SELECT * FROM  read_csv('data/csv/test/test.csv', columns=STRUCT_PACK(a := "
	              "'INTEGER', b := 'INTEGER', c := 'VARCHAR'), sep=',', auto_detect='false')"));
	test_progress.End();

	// copy from
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("COPY test FROM 'data/csv/test/test.csv'"));
	test_progress.End();
}
#endif
