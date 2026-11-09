#include "test_capi_v2.hpp"

#include "duckdb.h"

#include <string>

using namespace test_capi_v2;

// A V2 connection on the database a V1 duckdb_database opened.

namespace {

idx_t V2Count(duckdb_v2_connection_handle conn, const std::string &sql) {
	duckdb_v2_result_handle result = nullptr;
	REQUIRE(Query(conn, sql, &result, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto rows = DrainRowCount(result);
	duckdb_v2_result_destroy(&result);
	return rows;
}

void V1Exec(duckdb_connection conn, const char *sql) {
	duckdb_result result;
	REQUIRE(duckdb_query(conn, sql, &result) == DuckDBSuccess);
	duckdb_destroy_result(&result);
}

int64_t V1Int(duckdb_connection conn, const char *sql) {
	duckdb_result result;
	REQUIRE(duckdb_query(conn, sql, &result) == DuckDBSuccess);
	auto value = duckdb_value_int64(&result, 0, 0);
	duckdb_destroy_result(&result);
	return value;
}

} // namespace

TEST_CASE("V2: connection_create_from_v1_database", "[capi_v2][connection]") {
	duckdb_database v1_db = nullptr;
	duckdb_connection v1_conn = nullptr;
	REQUIRE(duckdb_open(nullptr, &v1_db) == DuckDBSuccess);
	REQUIRE(duckdb_connect(v1_db, &v1_conn) == DuckDBSuccess);

	V1Exec(v1_conn, "CREATE TABLE t AS SELECT range AS i FROM range(5)");
	V1Exec(v1_conn, "ATTACH ':memory:' AS other");
	V1Exec(v1_conn, "CREATE TABLE other.t AS SELECT range AS i FROM range(3)");

	duckdb_v2_connection_handle conn = nullptr;
	REQUIRE(duckdb_v2_connection_create_from_v1_database(v1_db, &conn, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(conn);

	// the catalog and the attached databases are shared
	REQUIRE(V2Count(conn, "SELECT * FROM main.t") == 5);
	REQUIRE(V2Count(conn, "SELECT * FROM other.t") == 3);

	// writes through either connection are visible to the other
	V2Count(conn, "INSERT INTO main.t VALUES (5), (6)");
	REQUIRE(V1Int(v1_conn, "SELECT count(*) FROM main.t") == 7);

	// session settings are each connection's own
	V1Exec(v1_conn, "SET search_path = 'other'");
	REQUIRE(V1Int(v1_conn, "SELECT count(*) FROM t") == 3);
	REQUIRE(V2Count(conn, "SELECT * FROM t") == 7);
	V2Count(conn, "SET search_path = 'other'");
	REQUIRE(V2Count(conn, "SELECT * FROM t") == 3);
	V1Exec(v1_conn, "RESET search_path");
	REQUIRE(V1Int(v1_conn, "SELECT count(*) FROM t") == 7);
	REQUIRE(V2Count(conn, "SELECT * FROM t") == 3);

	// global settings are shared
	V1Exec(v1_conn, "SET GLOBAL threads = 3");
	REQUIRE(V2Count(conn, "SELECT 1 WHERE current_setting('threads') = 3") == 1);

	duckdb_v2_connection_destroy(&conn);
	REQUIRE(!conn);
	duckdb_disconnect(&v1_conn);
	duckdb_close(&v1_db);
}

TEST_CASE("V2: connection_create_from_v1_database outlives duckdb_close", "[capi_v2][connection]") {
	duckdb_database v1_db = nullptr;
	REQUIRE(duckdb_open(nullptr, &v1_db) == DuckDBSuccess);
	duckdb_v2_connection_handle conn = nullptr;
	REQUIRE(duckdb_v2_connection_create_from_v1_database(v1_db, &conn, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_close(&v1_db);

	V2Count(conn, "CREATE TABLE t AS SELECT range AS i FROM range(4)");
	REQUIRE(V2Count(conn, "SELECT * FROM t") == 4);
	duckdb_v2_connection_destroy(&conn);
}

TEST_CASE("V2: connection_create_from_v1_database null arguments", "[capi_v2][connection]") {
	duckdb_v2_connection_handle conn = nullptr;
	duckdb_v2_error_info_handle err = nullptr;
	REQUIRE(duckdb_v2_connection_create_from_v1_database(nullptr, &conn, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(err);
	duckdb_v2_error_info_destroy(&err);

	duckdb_database v1_db = nullptr;
	REQUIRE(duckdb_open(nullptr, &v1_db) == DuckDBSuccess);
	REQUIRE(duckdb_v2_connection_create_from_v1_database(v1_db, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_close(&v1_db);
}
