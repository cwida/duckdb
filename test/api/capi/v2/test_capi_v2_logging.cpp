#include "test_capi_v2.hpp"

// ---------------------------------------------------------------------------
// V2 logging tests: entries written through an instance or a connection land
// in duckdb_logs under the matching scope.
// ---------------------------------------------------------------------------

namespace test_capi_v2 {

namespace {

idx_t CountLogs(duckdb_v2_connection_handle conn, const std::string &where) {
	duckdb_v2_result_handle r = nullptr;
	auto sql = "SELECT * FROM duckdb_logs WHERE " + where;
	REQUIRE(Query(conn, sql.c_str(), &r, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto count = DrainRowCount(r);
	duckdb_v2_result_destroy(&r);
	return count;
}

} // namespace

TEST_CASE("V2 logging: instance and connection entries carry their scope", "[capi_v2][logging]") {
	EnvFixture fx;
	ExecSQL(fx.conn, "SET enable_logging = true");

	auto type = Convert("");
	auto instance_message = Convert("v2 instance log");
	auto conn_message = Convert("v2 connection log");
	REQUIRE(duckdb_v2_instance_log(fx.instance, DUCKDB_V2_LOG_LEVEL_INFO, &type, &instance_message, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_log(fx.conn, DUCKDB_V2_LOG_LEVEL_INFO, &type, &conn_message, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);

	REQUIRE(CountLogs(fx.conn, "message = 'v2 instance log' AND scope = 'DATABASE'") == 1);
	REQUIRE(CountLogs(fx.conn,
	                  "message = 'v2 connection log' AND scope = 'CONNECTION' AND connection_id IS NOT NULL") == 1);

	// Below the threshold is dropped without error.
	auto trace_message = Convert("v2 trace log");
	REQUIRE(duckdb_v2_connection_log(fx.conn, DUCKDB_V2_LOG_LEVEL_TRACE, &type, &trace_message, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(CountLogs(fx.conn, "message = 'v2 trace log'") == 0);

	REQUIRE(duckdb_v2_instance_log(nullptr, DUCKDB_V2_LOG_LEVEL_INFO, &type, &instance_message, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_connection_log(nullptr, DUCKDB_V2_LOG_LEVEL_INFO, &type, &conn_message, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_connection_log(fx.conn, static_cast<DUCKDB_V2_LOG_LEVEL>(0), &type, &conn_message, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
}

} // namespace test_capi_v2
