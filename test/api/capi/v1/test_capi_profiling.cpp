#include "capi_tester.hpp"

using namespace duckdb;

// Find the first direct child of `info` that has a VALUE entry for `key`.
// Returns nullptr if not found.
static duckdb_profiling_info FindChildWithKey(duckdb_profiling_info info, const char *key) {
	idx_t n = duckdb_profiling_info_get_child_count(info);
	for (idx_t i = 0; i < n; i++) {
		auto child = duckdb_profiling_info_get_child(info, i);
		auto val = duckdb_profiling_info_get_value(child, key);
		if (val) {
			duckdb_destroy_value(&val);
			return child;
		}
	}
	return nullptr;
}

void RetrieveMetrics(duckdb_profiling_info info, duckdb::map<string, double> &cumulative_counter,
                     duckdb::map<string, double> &cumulative_result, const idx_t depth) {
	auto map = duckdb_profiling_info_get_metrics(info);
	REQUIRE(map);
	auto count = duckdb_get_map_size(map);

	// Test index out of bounds for MAP value.
	if (depth == 0) {
		auto invalid_key = duckdb_get_map_key(map, 10000000);
		REQUIRE(!invalid_key);
		auto invalid_value = duckdb_get_map_value(map, 10000000);
		REQUIRE(!invalid_value);
	}

	// The root node (depth=0) has no direct VALUE children — all query/system/etc.
	// metrics are stored in named OBJECT children accessible via get_child.
	// Nodes at depth>0 always carry at least one metric when profiling is enabled.
	if (depth > 0) {
		REQUIRE(count != 0);
	}

	for (idx_t i = 0; i < count; i++) {
		auto key = duckdb_get_map_key(map, i);
		REQUIRE(key);
		auto value = duckdb_get_map_value(map, i);
		REQUIRE(value);

		auto key_c_str = duckdb_get_varchar(key);
		auto value_c_str = duckdb_get_varchar(value);
		auto key_str = duckdb::string(key_c_str);
		auto value_str = duckdb::string(value_c_str);

		if (depth == 0) {
			// Root node has no direct VALUE children — these should never appear.
			REQUIRE(key_str != "intermediate_rows");
			REQUIRE(key_str != "rows_scanned");
			REQUIRE(key_str != "timing");
			REQUIRE(key_str != "name");
			REQUIRE(key_str != "type");
		} else {
			// Operator nodes and metric-group objects: keys are unprefixed.
			// Prefixed names (like "query.sql") must not appear here.
			REQUIRE(key_str != "query.sql");
			REQUIRE(key_str != "system.blocked_thread_time");
			REQUIRE(key_str != "query.total_time");
			REQUIRE(key_str != "query.rows_returned");
		}

		// String-valued metrics — skip numeric conversion for these.
		if (key_str == "name" || key_str == "type" || key_str == "extra_info" || key_str == "sql") {
			REQUIRE(!value_str.empty());
		} else {
			double result = 0;
			try {
				result = std::stod(value_str);
			} catch (std::invalid_argument &e) {
				REQUIRE(false);
			}

			if (cumulative_counter.find(key_str) != cumulative_counter.end()) {
				cumulative_counter[key_str] += result;
			}
			if (cumulative_result.find(key_str) != cumulative_result.end() && cumulative_result[key_str] == 0) {
				cumulative_result[key_str] = result;
			}
		}

		duckdb_destroy_value(&key);
		duckdb_destroy_value(&value);
		duckdb_free(key_c_str);
		duckdb_free(value_c_str);
	}

	duckdb_destroy_value(&map);
}

void TraverseTree(duckdb_profiling_info profiling_info, duckdb::map<string, double> &cumulative_counter,
                  duckdb::map<string, double> &cumulative_result, const idx_t depth) {
	RetrieveMetrics(profiling_info, cumulative_counter, cumulative_result, depth);

	// Recurse into children (operator nodes and metric-group OBJECT children).
	auto child_count = duckdb_profiling_info_get_child_count(profiling_info);
	if (depth == 0) {
		REQUIRE(child_count != 0);
	}

	for (idx_t i = 0; i < child_count; i++) {
		auto child = duckdb_profiling_info_get_child(profiling_info, i);
		TraverseTree(child, cumulative_counter, cumulative_result, depth + 1);
	}
}

idx_t ConvertToInt(double value) {
	return idx_t(value * 1000);
}

TEST_CASE("Test profiling with a single metric and get_value", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;
	REQUIRE(tester.OpenDatabase(nullptr));

	REQUIRE_NO_FAIL(tester.Query("PRAGMA enable_profiling = 'no_output'"));

	// test query.cpu_time profiling alongside operator.extra_info so the metrics map is non-empty
	REQUIRE_NO_FAIL(tester.Query("SET tracked_metrics = ['query.cpu_time', 'operator.extra_info']"));
	REQUIRE_NO_FAIL(tester.Query("SELECT 42"));

	auto info = duckdb_get_profiling_info(tester.connection);
	REQUIRE(info != nullptr);

	// Retrieve a metric that is not enabled — must return nullptr at every level.
	REQUIRE(duckdb_profiling_info_get_value(info, "operator.intermediate_rows") == nullptr);

	// query.cpu_time lives inside the "query" metric-group child, not at the root level.
	// Navigate to it via get_child, then retrieve the unprefixed key "cpu_time".
	auto query_group = FindChildWithKey(info, "cpu_time");
	REQUIRE(query_group != nullptr);
	auto cpu_time_val = duckdb_profiling_info_get_value(query_group, "cpu_time");
	REQUIRE(cpu_time_val != nullptr);
	duckdb_destroy_value(&cpu_time_val);

	duckdb::map<string, double> cumulative_counter;
	duckdb::map<string, double> cumulative_result;

	TraverseTree(info, cumulative_counter, cumulative_result, 0);
	tester.Cleanup();
}

TEST_CASE("Test profiling with cumulative metrics", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;
	REQUIRE(tester.OpenDatabase(nullptr));

	REQUIRE_NO_FAIL(tester.Query("PRAGMA enable_profiling = 'no_output'"));

	// test all profiling metrics
	REQUIRE_NO_FAIL(tester.Query(
	    "SET tracked_metrics = ['system.blocked_thread_time', 'query.cpu_time', 'query.total_intermediate_rows', "
	    "'operator.extra_info', 'operator.intermediate_rows', 'operator.timing']"));
	REQUIRE_NO_FAIL(tester.Query("SELECT 42"));

	auto info = duckdb_get_profiling_info(tester.connection);
	REQUIRE(info != nullptr);

	// All operator nodes (depth>0) have unprefixed keys: "timing", "intermediate_rows"
	duckdb::map<string, double> cumulative_counter = {{"timing", 0}, {"intermediate_rows", 0}};
	duckdb::map<string, double> cumulative_result;

	TraverseTree(info, cumulative_counter, cumulative_result, 0);

	// query.cpu_time and query.total_intermediate_rows live in the "query" metric-group child.
	// Navigate to it and retrieve the unprefixed keys.
	auto query_group = FindChildWithKey(info, "cpu_time");
	REQUIRE(query_group != nullptr);

	auto cpu_time_val = duckdb_profiling_info_get_value(query_group, "cpu_time");
	REQUIRE(cpu_time_val != nullptr);
	auto cpu_time_str = duckdb_get_varchar(cpu_time_val);
	double root_cpu_time = std::stod(cpu_time_str);
	duckdb_free(cpu_time_str);
	duckdb_destroy_value(&cpu_time_val);

	auto total_rows_val = duckdb_profiling_info_get_value(query_group, "total_intermediate_rows");
	REQUIRE(total_rows_val != nullptr);
	auto total_rows_str = duckdb_get_varchar(total_rows_val);
	double root_total_intermediate_rows = std::stod(total_rows_str);
	duckdb_free(total_rows_str);
	duckdb_destroy_value(&total_rows_val);

	REQUIRE(ConvertToInt(root_cpu_time) == ConvertToInt(cumulative_counter["timing"]));
	REQUIRE(ConvertToInt(root_total_intermediate_rows) == ConvertToInt(cumulative_counter["intermediate_rows"]));
	tester.Cleanup();
}

TEST_CASE("Test profiling without profiling enabled", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;
	REQUIRE(tester.OpenDatabase(nullptr));

	// Retrieve info without profiling enabled.
	auto info = duckdb_get_profiling_info(tester.connection);
	REQUIRE(info == nullptr);
	tester.Cleanup();
}

TEST_CASE("Test profiling with detailed profiling mode enabled", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;
	REQUIRE(tester.OpenDatabase(nullptr));

	REQUIRE_NO_FAIL(tester.Query("PRAGMA enable_profiling = 'no_output'"));
	REQUIRE_NO_FAIL(tester.Query("PRAGMA profiling_mode = 'detailed'"));
	REQUIRE_NO_FAIL(tester.Query("SELECT 42"));

	auto info = duckdb_get_profiling_info(tester.connection);
	REQUIRE(info != nullptr);

	duckdb::map<string, double> cumulative_counter;
	duckdb::map<string, double> cumulative_result;
	TraverseTree(info, cumulative_counter, cumulative_result, 0);
	tester.Cleanup();
}

TEST_CASE("Test invalid use of profiling API", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;
	REQUIRE(tester.OpenDatabase(nullptr));

	REQUIRE_NO_FAIL(tester.Query("PRAGMA enable_profiling = 'no_output'"));
	REQUIRE_NO_FAIL(tester.Query("PRAGMA profiling_mode = 'detailed'"));
	REQUIRE_NO_FAIL(tester.Query("SELECT 42"));

	auto info = duckdb_get_profiling_info(tester.connection);
	REQUIRE(info != nullptr);

	// Incorrect usage tests.

	auto map = duckdb_profiling_info_get_metrics(nullptr);
	REQUIRE(map == nullptr);
	map = duckdb_profiling_info_get_metrics(info);

	auto dummy_value = duckdb_create_bool(true);
	auto count = duckdb_get_map_size(nullptr);
	REQUIRE(count == 0);
	count = duckdb_get_map_size(dummy_value);
	REQUIRE(count == 0);
	count = duckdb_get_map_size(map);

	for (idx_t i = 0; i < count; i++) {
		auto key = duckdb_get_map_key(nullptr, i);
		REQUIRE(key == nullptr);
		key = duckdb_get_map_key(map, DConstants::INVALID_INDEX);
		REQUIRE(key == nullptr);
		key = duckdb_get_map_key(dummy_value, i);
		REQUIRE(key == nullptr);

		auto value = duckdb_get_map_value(nullptr, i);
		REQUIRE(value == nullptr);
		value = duckdb_get_map_value(map, DConstants::INVALID_INDEX);
		REQUIRE(value == nullptr);
		value = duckdb_get_map_value(dummy_value, i);
		REQUIRE(value == nullptr);
		break;
	}

	duckdb_destroy_value(&dummy_value);
	duckdb_destroy_value(&map);
	tester.Cleanup();
}

TEST_CASE("Test profiling after throwing an error", "[capi]") {
	CAPITester tester;
	auto main_db = TestCreatePath("profiling_error.db");
	REQUIRE(tester.OpenDatabase(main_db.c_str()));

	auto path = TestCreatePath("profiling_error.db");
	REQUIRE_NO_FAIL(tester.Query("ATTACH IF NOT EXISTS '" + path + "' (TYPE DUCKDB)"));
	REQUIRE_NO_FAIL(tester.Query("CREATE TABLE profiling_error.tbl AS SELECT range AS id FROM range(10)"));

	REQUIRE_NO_FAIL(tester.Query("SET enable_profiling = 'no_output'"));
	REQUIRE_NO_FAIL(tester.Query("SET profiling_mode = 'standard'"));

	CAPIPrepared prepared_q1;
	CAPIPending pending_q1;
	REQUIRE(prepared_q1.Prepare(tester, "SELECT * FROM profiling_error.tbl"));
	REQUIRE(pending_q1.Pending(prepared_q1));
	auto result = pending_q1.Execute();
	REQUIRE(result);
	REQUIRE(!result->HasError());

	auto info = duckdb_get_profiling_info(tester.connection);
	REQUIRE(info != nullptr);

	CAPIPrepared prepared_q2;
	REQUIRE(!prepared_q2.Prepare(tester, "SELECT * FROM profiling_error.does_not_exist"));
	info = duckdb_get_profiling_info(tester.connection);
	REQUIRE(info == nullptr);

	tester.Cleanup();
}

TEST_CASE("Test profiling with Extra Info enabled", "[capi]") {
	CAPITester tester;
	REQUIRE(tester.OpenDatabase(nullptr));

	REQUIRE_NO_FAIL(tester.Query("PRAGMA enable_profiling = 'no_output'"));
	REQUIRE_NO_FAIL(tester.Query("SET tracked_metrics = ['operator.extra_info']"));
	REQUIRE_NO_FAIL(tester.Query("SELECT unnest([1,2,3]) ORDER BY 1"));

	auto info = duckdb_get_profiling_info(tester.connection);
	REQUIRE(info);

	// get_child(root, 0) returns the root operator (LIST items come before OBJECT children).
	auto child_info = duckdb_profiling_info_get_child(info, 0);
	REQUIRE(duckdb_profiling_info_get_child_count(child_info) != 0);

	auto map = duckdb_profiling_info_get_metrics(child_info);
	REQUIRE(map);
	auto count = duckdb_get_map_size(map);
	REQUIRE(count != 0);

	bool found_extra_info = false;
	for (idx_t i = 0; i < count; i++) {
		auto key = duckdb_get_map_key(map, i);
		REQUIRE(key);
		auto key_c_str = duckdb_get_varchar(key);
		auto key_str = duckdb::string(key_c_str);

		auto value = duckdb_get_map_value(map, i);
		REQUIRE(value);
		auto value_c_str = duckdb_get_varchar(value);
		auto value_str = duckdb::string(value_c_str);

		if (key_str == "extra_info") {
			REQUIRE(!value_str.empty());
			found_extra_info = true;
		}

		if (key) {
			duckdb_destroy_value(&key);
			duckdb_free(key_c_str);
		}
		if (value) {
			duckdb_destroy_value(&value);
			duckdb_free(value_c_str);
		}
	}

	REQUIRE(found_extra_info);

	duckdb_destroy_value(&map);
	tester.Cleanup();
}

TEST_CASE("Test profiling with the appender", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;
	REQUIRE(tester.OpenDatabase(nullptr));

	tester.Query("CREATE TABLE tbl (i INT PRIMARY KEY, value VARCHAR)");
	REQUIRE_NO_FAIL(tester.Query("PRAGMA enable_profiling = 'no_output'"));
	REQUIRE_NO_FAIL(tester.Query("SET profiling_coverage='ALL'"));
	duckdb_appender appender;

	string query = "INSERT INTO tbl FROM my_appended_data";
	duckdb_logical_type types[2];
	types[0] = duckdb_create_logical_type(DUCKDB_TYPE_INTEGER);
	types[1] = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);

	auto status = duckdb_appender_create_query(tester.connection, query.c_str(), 2, types, "my_appended_data", nullptr,
	                                           &appender);
	duckdb_destroy_logical_type(&types[0]);
	duckdb_destroy_logical_type(&types[1]);
	REQUIRE(status == DuckDBSuccess);
	REQUIRE(duckdb_appender_error(appender) == nullptr);

	REQUIRE(duckdb_appender_begin_row(appender) == DuckDBSuccess);
	REQUIRE(duckdb_append_int32(appender, 1) == DuckDBSuccess);
	REQUIRE(duckdb_append_varchar(appender, "hello world") == DuckDBSuccess);
	REQUIRE(duckdb_appender_end_row(appender) == DuckDBSuccess);

	REQUIRE(duckdb_appender_flush(appender) == DuckDBSuccess);
	REQUIRE(duckdb_appender_close(appender) == DuckDBSuccess);
	REQUIRE(duckdb_appender_destroy(&appender) == DuckDBSuccess);

	auto info = duckdb_get_profiling_info(tester.connection);
	REQUIRE(info);

	// query.sql lives in the "query" metric-group child. Navigate to it and
	// retrieve the unprefixed key "sql".
	auto query_group = FindChildWithKey(info, "sql");
	REQUIRE(query_group != nullptr);
	auto query_name = duckdb_profiling_info_get_value(query_group, "sql");
	REQUIRE(query_name);
	auto query_name_c_str = duckdb_get_varchar(query_name);
	auto query_name_str = duckdb::string(query_name_c_str);
	REQUIRE(query_name_str == query);
	duckdb_destroy_value(&query_name);
	duckdb_free(query_name_c_str);

	duckdb::map<string, double> cumulative_counter;
	duckdb::map<string, double> cumulative_result;
	TraverseTree(info, cumulative_counter, cumulative_result, 0);
	tester.Cleanup();
}

TEST_CASE("Test profiling with the non-query appender", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;

	REQUIRE(tester.OpenDatabase(nullptr));
	tester.Query("CREATE TABLE test (i INTEGER)");
	REQUIRE_NO_FAIL(tester.Query("PRAGMA enable_profiling = 'no_output'"));
	REQUIRE_NO_FAIL(tester.Query("SET profiling_coverage='ALL'"));

	duckdb_appender appender;
	REQUIRE(duckdb_appender_create(tester.connection, nullptr, "test", &appender) == DuckDBSuccess);
	REQUIRE(duckdb_appender_error(appender) == nullptr);

	// Appending a row.
	REQUIRE(duckdb_appender_begin_row(appender) == DuckDBSuccess);
	REQUIRE(duckdb_append_int32(appender, 42) == DuckDBSuccess);
	// Finish and flush.
	REQUIRE(duckdb_appender_end_row(appender) == DuckDBSuccess);
	REQUIRE(duckdb_appender_flush(appender) == DuckDBSuccess);
	REQUIRE(duckdb_appender_close(appender) == DuckDBSuccess);
	REQUIRE(duckdb_appender_destroy(&appender) == DuckDBSuccess);

	auto info = duckdb_get_profiling_info(tester.connection);
	REQUIRE(info);

	// Navigate to the "query" metric-group child and check the sql value.
	auto query_group = FindChildWithKey(info, "sql");
	REQUIRE(query_group != nullptr);
	auto query_name = duckdb_profiling_info_get_value(query_group, "sql");
	REQUIRE(query_name);

	auto query_name_c_str = duckdb_get_varchar(query_name);
	auto query_name_str = duckdb::string(query_name_c_str);

	auto query = "INSERT INTO memory.main.test FROM __duckdb_internal_appended_data";
	REQUIRE(query_name_str == query);

	duckdb_destroy_value(&query_name);
	duckdb_free(query_name_c_str);

	duckdb::map<string, double> cumulative_counter;
	duckdb::map<string, double> cumulative_result;
	TraverseTree(info, cumulative_counter, cumulative_result, 0);
	tester.Cleanup();
}

extern "C" {
DUCKDB_C_API duckdb_profiling_info duckdb_get_profiling_info_snapshot(duckdb_connection connection);
DUCKDB_C_API void duckdb_destroy_profiling_info_snapshot(duckdb_profiling_info *info);
}

TEST_CASE("Take an owned profiling snapshot during streaming", "[capi]") {
	CAPITester tester;
	REQUIRE(tester.OpenDatabase(nullptr));
	REQUIRE_NO_FAIL(tester.Query("SET threads = 4"));
	REQUIRE_NO_FAIL(tester.Query("PRAGMA enable_profiling = 'no_output'"));
	REQUIRE_NO_FAIL(tester.Query("SET tracked_metrics = ['operator.extra_info', 'operator.name']"));

	duckdb_prepared_statement prepared = nullptr;
	REQUIRE(duckdb_prepare(tester.connection, "SELECT range FROM range(10000000)", &prepared) == DuckDBSuccess);

	duckdb_result result;
	REQUIRE(duckdb_execute_prepared_streaming(prepared, &result) == DuckDBSuccess);
	REQUIRE(duckdb_result_is_streaming(result));

	auto chunk = duckdb_stream_fetch_chunk(result);
	REQUIRE(chunk);
	idx_t row_count = duckdb_data_chunk_get_size(chunk);
	duckdb_destroy_data_chunk(&chunk);

	auto snapshot = duckdb_get_profiling_info_snapshot(tester.connection);
	REQUIRE(snapshot);

	while ((chunk = duckdb_stream_fetch_chunk(result))) {
		row_count += duckdb_data_chunk_get_size(chunk);
		duckdb_destroy_data_chunk(&chunk);
	}
	REQUIRE(row_count == 10000000);

	duckdb_destroy_result(&result);
	duckdb_destroy_prepare(&prepared);
	REQUIRE_NO_FAIL(tester.Query("SELECT 42"));

	duckdb::map<string, double> cumulative_counter;
	duckdb::map<string, double> cumulative_result;
	TraverseTree(snapshot, cumulative_counter, cumulative_result, 0);

	duckdb_destroy_profiling_info_snapshot(&snapshot);
	REQUIRE(!snapshot);
}
