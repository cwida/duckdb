#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/main/statement_iterator.hpp"

#include <iostream>
#include <thread>

using namespace duckdb;

TEST_CASE("Test query profiler", "[api]") {
	duckdb::unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);
	string output;

	con.EnableProfiling();
	// don't pollute the console with profiler info - write it to a file in the test directory instead.
	con.context->config.profiler_save_location = TestCreatePath("test_query_profiler_output.txt");

	string query = "SELECT * FROM (SELECT 42) tbl1, (SELECT 33) tbl2";
	REQUIRE_NO_FAIL(con.Query(query));

	output = con.GetProfilingInformation();
	REQUIRE(output.size() > 0);
	// the text profiler output renders only the operator tree (the query SQL is no longer included)

	output = con.GetProfilingInformation(ProfilerPrintFormat::JSON());
	REQUIRE(output.size() > 0);
	bool query_found_in_output = output.find(query) != std::string::npos;
	REQUIRE(query_found_in_output);
}

TEST_CASE("Test query profiler, no query in the profiling output.", "[api]") {
	duckdb::unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);
	string output;

	con.EnableProfiling();
	// don't pollute the console with profiler info - write it to a file in the test directory instead.
	con.context->config.profiler_save_location = TestCreatePath("test_query_profiler_output.txt");

	// Disable `QUERY_SQL` in profiling output by only tracking other metrics.
	REQUIRE_NO_FAIL(
	    con.Query("SET tracked_metrics = ['query.cpu_time', 'query.total_time', 'operator.timing', 'operator.type']"));
	string query = "SELECT * FROM (SELECT 42) tbl1, (SELECT 33) tbl2";
	REQUIRE_NO_FAIL(con.Query(query));

	output = con.GetProfilingInformation();
	REQUIRE(output.size() > 0);
	bool query_not_found_in_output = output.find(query) == std::string::npos;
	REQUIRE(query_not_found_in_output);

	output = con.GetProfilingInformation(ProfilerPrintFormat::JSON());
	REQUIRE(output.size() > 0);
	query_not_found_in_output = output.find(query) == std::string::npos;
	REQUIRE(query_not_found_in_output);
}

TEST_CASE("Disabling profiling resets the active query profiler", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	con.EnableProfiling();
	con.context->config.profiler_print_format = "no_output";

	REQUIRE_NO_FAIL(con.Query("CALL disable_profiling()"));
	auto &profiler = QueryProfiler::Get(*con.context);
	CHECK(profiler.GetQuerySQL().empty());
	CHECK(profiler.GetQueryMetrics().GetStringMetricInSeconds("query.total_time") == 0);

	REQUIRE_NO_FAIL(con.Query("PRAGMA enable_profiling='no_output'"));
	REQUIRE_NO_FAIL(con.Query("SELECT 42"));
	auto output = con.GetProfilingInformation(ProfilerPrintFormat::JSON());
	REQUIRE(output.find("SELECT 42") != string::npos);
}

TEST_CASE("Test parser timing is reported per statement", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	con.EnableProfiling();
	con.context->config.profiler_save_location = TestCreatePath("test_query_profiler_parser_output.txt");
	con.context->config.tracked_metrics = {"parser.total_time", "query.sql"};

	REQUIRE_NO_FAIL(con.Query("SELECT 7;"));
	auto single_statement_output = con.GetProfilingInformation(ProfilerPrintFormat::JSON());
	REQUIRE(single_statement_output.find("\"parser\"") != std::string::npos);
	REQUIRE(single_statement_output.find("SELECT 7;") != std::string::npos);

	auto iterator = con.context->IterateStatements("SELECT 42; SELECT 43;");
	for (const auto expected_query : {"SELECT 42; ", "SELECT 43;"}) {
		REQUIRE(iterator.Peek());
		auto statement = iterator.GetStatementForExecution();
		REQUIRE(statement);
		REQUIRE(statement->query == expected_query);
		REQUIRE_NO_FAIL(con.Query(std::move(statement)));

		auto output = con.GetProfilingInformation(ProfilerPrintFormat::JSON());
		REQUIRE(output.find("\"parser\"") != std::string::npos);
		REQUIRE(output.find(expected_query) != std::string::npos);
	}
	REQUIRE_FALSE(iterator.Peek());
}

TEST_CASE("Extracting statements does not start the query profiler", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	con.EnableProfiling();
	con.context->config.profiler_save_location = TestCreatePath("test_query_profiler_extract_output.txt");
	con.context->config.tracked_metrics = {"parser.total_time", "query.sql"};

	auto statements = con.ExtractStatements("SELECT 44;");
	REQUIRE(statements.size() == 1);
	REQUIRE(QueryProfiler::Get(*con.context).GetQuerySQL().empty());

	REQUIRE_NO_FAIL(con.Query("SELECT 44;"));
	auto output = con.GetProfilingInformation(ProfilerPrintFormat::JSON());
	REQUIRE(output.find("\"parser\"") != std::string::npos);
	REQUIRE(output.find("SELECT 44;") != std::string::npos);
}

TEST_CASE("Calls that run no statement do not leave the query profiler running", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	con.EnableProfiling();
	con.context->config.profiler_print_format = "no_output";
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT 1 AS i"));

	auto check_next_query = [&]() {
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		REQUIRE_NO_FAIL(con.Query("SELECT 42"));
		auto &profiler = QueryProfiler::Get(*con.context);
		REQUIRE(profiler.GetQuerySQL() == "SELECT 42");
		// the idle time before the query is not counted towards its latency
		REQUIRE(profiler.GetQueryMetrics().GetStringMetricInSeconds("query.total_time") < 0.4);
	};

	SECTION("GetTableNames fails") {
		REQUIRE_THROWS(con.GetTableNames("SELECT no_such_function(i) FROM t"));
		check_next_query();
	}
	SECTION("GetTableNames succeeds") {
		REQUIRE(con.GetTableNames("SELECT * FROM t").size() == 1);
		check_next_query();
	}
	SECTION("ExtractPlan fails") {
		REQUIRE_THROWS(con.ExtractPlan("SELECT * FROM table_that_does_not_exist"));
		check_next_query();
	}
	SECTION("ExtractPlan succeeds") {
		REQUIRE(con.ExtractPlan("SELECT * FROM t"));
		check_next_query();
	}
	SECTION("a query fails to parse") {
		REQUIRE_FAIL(con.Query("SELEC 42"));
		check_next_query();
	}
	SECTION("a prepared statement fails to parse") {
		REQUIRE(con.Prepare("SELEC 42")->HasError());
		check_next_query();
	}
	SECTION("a submitted query fails to parse") {
		REQUIRE(con.Submit("SELEC 42")->HasError());
		check_next_query();
	}
	SECTION("a query has no statements") {
		REQUIRE_NO_FAIL(con.Query(""));
		check_next_query();
	}
}

TEST_CASE("Test latency when interrupting query", "[api]") {
	// FIXME
	// duckdb::unique_ptr<QueryResult> result;
	// DuckDB db(nullptr);
	// Connection con(db);
	//
	// con.EnableProfiling();
	//
	// con.context->config.profiler_save_location = TestCreatePath("test_query_profiler_output.txt");
	//
	// // Test interupting a query and running a new one afterward.
	// // The latency should reflect the new one.
	// std::thread t([&con]() {
	// 	string query = "explain analyze select sum(range) from range(1_000_000_000);";
	// 	con.Query(query);
	// });
	//
	// std::this_thread::sleep_for(std::chrono::milliseconds(100));
	// con.Interrupt();
	// t.join();
	//
	// string query = "explain analyze select 42;";
	// REQUIRE_NO_FAIL(con.Query(query));
	//
	// auto profiling_info = con.GetProfilingTree()->GetProfilingInfo();
	// auto latency = profiling_info.GetMetricValue<double>(MetricType::LATENCY);
	// auto query_name = profiling_info.GetMetricValue<string>(MetricType::QUERY_NAME);
	// REQUIRE(query == query_name);
	// REQUIRE(latency > 0);
	// REQUIRE(latency < 0.1);
}

TEST_CASE("Test the running total of bytes scanned", "[api][parquet]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto path = TestCreatePath("bytes_scanned_running_total.parquet");
	REQUIRE_NO_FAIL(
	    con.Query("COPY (SELECT range AS i FROM range(6144)) TO '" + path + "' (FORMAT parquet, ROW_GROUP_SIZE 2048)"));
	auto chunks = con.Query("SELECT sum(total_compressed_size)::UBIGINT FROM parquet_metadata('" + path + "')");
	REQUIRE_NO_FAIL(*chunks);
	auto chunk_bytes = chunks->Collection().GetValue(0, 0).GetValue<uint64_t>();
	REQUIRE(chunk_bytes > 0);

	// tracked with profiling disabled
	REQUIRE_NO_FAIL(con.Query("SELECT sum(i) FROM read_parquet('" + path + "')"));
	REQUIRE(QueryProfiler::Get(*con.context).GetBytesScanned() == chunk_bytes);

	// and for a scan inside a secure view, which query.total_bytes_scanned leaves out
	REQUIRE_NO_FAIL(con.Query("CREATE SECURE VIEW secure_scan AS SELECT i FROM read_parquet('" + path + "')"));
	REQUIRE_NO_FAIL(con.Query("SELECT sum(i) FROM secure_scan"));
	REQUIRE(QueryProfiler::Get(*con.context).GetBytesScanned() == chunk_bytes);
}

TEST_CASE("Test the running total of bytes scanned after a failed query", "[api][parquet]") {
	DuckDB db(nullptr);
	Connection con(db);
	// read-ahead schedules the later row groups early; only those whose scan started may count
	REQUIRE_NO_FAIL(con.Query("SET threads = 1"));
	REQUIRE_NO_FAIL(con.Query("SET read_ahead_depth = 8"));
	auto path = TestCreatePath("bytes_scanned_failed_query.parquet");
	REQUIRE_NO_FAIL(
	    con.Query("COPY (SELECT range AS i FROM range(6144)) TO '" + path + "' (FORMAT parquet, ROW_GROUP_SIZE 2048)"));
	auto chunks = con.Query("SELECT sum(total_compressed_size)::UBIGINT FROM parquet_metadata('" + path + "')");
	REQUIRE_NO_FAIL(*chunks);
	auto chunk_bytes = chunks->Collection().GetValue(0, 0).GetValue<uint64_t>();

	// fails in the second of three row groups, after scanning the first
	REQUIRE_FAIL(
	    con.Query("SELECT sum(CASE WHEN i = 3000 THEN error('boom') ELSE i END) FROM read_parquet('" + path + "')"));
	auto scanned_before_failing = QueryProfiler::Get(*con.context).GetBytesScanned();
	REQUIRE(scanned_before_failing > 0);
	REQUIRE(scanned_before_failing < chunk_bytes);

	// and the next query starts from zero
	REQUIRE_NO_FAIL(con.Query("SELECT sum(i) FROM read_parquet('" + path + "')"));
	REQUIRE(QueryProfiler::Get(*con.context).GetBytesScanned() == chunk_bytes);

	// a statement that fails before it starts executing does not report the previous one's total
	auto missing_parameter = con.ExtractStatements("SELECT $1::INTEGER");
	REQUIRE_FAIL(con.Query(std::move(missing_parameter[0])));
	REQUIRE(QueryProfiler::Get(*con.context).GetBytesScanned() == 0);
}

TEST_CASE("Test bytes scanned by a LIMIT with read-ahead", "[api][parquet]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads = 1"));
	REQUIRE_NO_FAIL(con.Query("SET read_ahead_depth = 8"));
	auto path = TestCreatePath("bytes_scanned_limit.parquet");
	REQUIRE_NO_FAIL(con.Query("COPY (SELECT range AS i FROM range(10 * 2048)) TO '" + path +
	                          "' (FORMAT parquet, ROW_GROUP_SIZE 2048)"));
	auto first =
	    con.Query("SELECT total_compressed_size::UBIGINT FROM parquet_metadata('" + path + "') WHERE row_group_id = 0");
	REQUIRE_NO_FAIL(*first);
	auto first_row_group = first->Collection().GetValue(0, 0).GetValue<uint64_t>();

	// the row groups read ahead are never scanned, so only the first counts
	REQUIRE_NO_FAIL(con.Query("SELECT * FROM read_parquet('" + path + "') LIMIT 10"));
	REQUIRE(QueryProfiler::Get(*con.context).GetBytesScanned() == first_row_group);
}
