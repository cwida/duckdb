#ifndef DUCKDB_NO_THREADS

#include <duckdb/execution/executor.hpp>
#include <future>
#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/common/progress_bar.hpp"

#include <thread>
#include <future>

using namespace duckdb;
using namespace std;

class TestProgressBar {
public:
	explicit TestProgressBar(ClientContext *context) : context(context), correct(true) {
	}

	ClientContext *context;
	atomic<bool> stop;
	std::thread check_thread;
	atomic<bool> correct;

	void CheckProgressThread() {
		int prev_percentage = -1;
		while (!stop) {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			int new_percentage = context->GetProgress();
			if (!(new_percentage >= prev_percentage || new_percentage == -1)) {
				correct = false;
			}
			if (!(new_percentage <= 100)) {
				correct = false;
			}
		}
	}
	void Start() {
		stop = false;
		check_thread = std::thread(&TestProgressBar::CheckProgressThread, this);
	}
	void End() {
		stop = true;
		check_thread.join();
		REQUIRE(correct);
	}
};

TEST_CASE("Test Progress Bar Fast", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	TestProgressBar test_progress(con.context.get());

	REQUIRE_NOTHROW(con.context->GetProgress());

	REQUIRE_NO_FAIL(con.Query("create  table tbl as select range a, mod(range,10) b from range(10000);"));
	REQUIRE_NO_FAIL(con.Query("create  table tbl_2 as select range a from range(10000);"));

	REQUIRE_NO_FAIL(con.Query("PRAGMA set_progress_bar_time=10"));
	REQUIRE_NO_FAIL(con.Query("PRAGMA disable_print_progress_bar"));
	//! Simple Aggregation
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl"));
	test_progress.End();

	//! Simple Join
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl inner join tbl_2 on (tbl.a = tbl_2.a)"));
	test_progress.End();

	//! Subquery
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl where a = (select min(a) from tbl_2)"));
	test_progress.End();

	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl where a = (select min(b) from tbl)"));
	test_progress.End();

	// Stream result
	test_progress.Start();
	auto result = con.SendQuery("select count(*) from tbl inner join tbl_2 on (tbl.a = tbl_2.a)");
	test_progress.End();
	REQUIRE_NO_FAIL(*result);

	//! Test Multiple threads
	REQUIRE_NO_FAIL(con.Query("PRAGMA threads=4"));
	REQUIRE_NO_FAIL(con.Query("PRAGMA force_parallelism"));

	//! Simple Aggregation
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl"));
	test_progress.End();

	//! Simple Join
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl inner join tbl_2 on (tbl.a = tbl_2.a)"));
	test_progress.End();

	//! Subquery
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl where a = (select min(a) from tbl_2)"));
	test_progress.End();

	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl where a = (select min(b) from tbl)"));
	test_progress.End();

	// Stream result
	test_progress.Start();
	result = con.SendQuery("select count(*) from tbl inner join tbl_2 on (tbl.a = tbl_2.a)");
	test_progress.End();
	REQUIRE_NO_FAIL(*result);
}

TEST_CASE("Test Progress Bar", "[api][.]") {
	DuckDB db(nullptr);
	Connection con(db);
	TestProgressBar test_progress(con.context.get());
	REQUIRE_NO_FAIL(con.Query("create  table tbl as select range a, mod(range,10) b from range(10000000);"));
	REQUIRE_NO_FAIL(con.Query("create  table tbl_2 as select range a from range(10000000);"));

	REQUIRE_NO_FAIL(con.Query("PRAGMA set_progress_bar_time=10"));
	REQUIRE_NO_FAIL(con.Query("PRAGMA disable_print_progress_bar"));
	//! Simple Aggregation
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl"));
	test_progress.End();

	//! Simple Join
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl inner join tbl_2 on (tbl.a = tbl_2.a)"));
	test_progress.End();

	//! Subquery
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl where a = (select min(a) from tbl_2)"));
	test_progress.End();

	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl where a = (select min(b) from tbl)"));
	test_progress.End();

	// Stream result
	test_progress.Start();
	auto result = con.SendQuery("select count(*) from tbl inner join tbl_2 on (tbl.a = tbl_2.a)");
	test_progress.End();
	REQUIRE_NO_FAIL(*result);

	//! Test Multiple threads
	REQUIRE_NO_FAIL(con.Query("PRAGMA threads=4"));
	REQUIRE_NO_FAIL(con.Query("PRAGMA force_parallelism"));

	//! Simple Aggregation
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl"));
	test_progress.End();

	//! Simple Join
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl inner join tbl_2 on (tbl.a = tbl_2.a)"));
	test_progress.End();

	//! Subquery
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl where a = (select min(a) from tbl_2)"));
	test_progress.End();

	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl where a = (select min(b) from tbl)"));
	test_progress.End();

	// Stream result
	test_progress.Start();
	result = con.SendQuery("select count(*) from tbl inner join tbl_2 on (tbl.a = tbl_2.a)");
	test_progress.End();
	REQUIRE_NO_FAIL(*result);
}

TEST_CASE("Test Progress Bar CSV", "[api][.]") {
	DuckDB db(nullptr);
	Connection con(db);

	TestProgressBar test_progress(con.context.get());
	REQUIRE_NO_FAIL(con.Query("PRAGMA set_progress_bar_time=1"));
	REQUIRE_NO_FAIL(con.Query("PRAGMA disable_print_progress_bar"));

	//! Create Tables From CSVs
	test_progress.Start();
	REQUIRE_NO_FAIL(
	    con.Query("CREATE TABLE test AS SELECT * FROM read_csv_auto ('test/sql/copy/csv/data/test/test.csv')"));
	test_progress.End();

	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query(
	    "CREATE TABLE test_2 AS SELECT * FROM  read_csv('test/sql/copy/csv/data/test/test.csv', columns=STRUCT_PACK(a "
	    ":= 'INTEGER', b := 'INTEGER', c := 'VARCHAR'), sep=',', auto_detect='false')"));
	test_progress.End();

	//! Insert into existing tables
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("INSERT INTO test SELECT * FROM read_csv_auto('test/sql/copy/csv/data/test/test.csv')"));
	test_progress.End();

	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query(
	    "INSERT INTO test SELECT * FROM  read_csv('test/sql/copy/csv/data/test/test.csv', columns=STRUCT_PACK(a := "
	    "'INTEGER', b := 'INTEGER', c := 'VARCHAR'), sep=',', auto_detect='false')"));
	test_progress.End();

	//! copy from
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("COPY test FROM 'test/sql/copy/csv/data/test/test.csv'"));
	test_progress.End();

	//! Repeat but in parallel
	REQUIRE_NO_FAIL(con.Query("DROP TABLE test"));
	REQUIRE_NO_FAIL(con.Query("DROP TABLE test_2"));

	//! Test Multiple threads
	REQUIRE_NO_FAIL(con.Query("PRAGMA threads=4"));
	REQUIRE_NO_FAIL(con.Query("PRAGMA force_parallelism"));
	//! Create Tables From CSVs
	test_progress.Start();
	REQUIRE_NO_FAIL(
	    con.Query("CREATE TABLE test AS SELECT * FROM read_csv_auto ('test/sql/copy/csv/data/test/test.csv')"));
	test_progress.End();

	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query(
	    "CREATE TABLE test_2 AS SELECT * FROM  read_csv('test/sql/copy/csv/data/test/test.csv', columns=STRUCT_PACK(a "
	    ":= 'INTEGER', b := 'INTEGER', c := 'VARCHAR'), sep=',', auto_detect='false')"));
	test_progress.End();

	//! Insert into existing tables
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("INSERT INTO test SELECT * FROM read_csv_auto('test/sql/copy/csv/data/test/test.csv')"));
	test_progress.End();

	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query(
	    "INSERT INTO test SELECT * FROM  read_csv('test/sql/copy/csv/data/test/test.csv', columns=STRUCT_PACK(a := "
	    "'INTEGER', b := 'INTEGER', c := 'VARCHAR'), sep=',', auto_detect='false')"));
	test_progress.End();

	//! copy from
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("COPY test FROM 'test/sql/copy/csv/data/test/test.csv'"));
	test_progress.End();
}
#endif