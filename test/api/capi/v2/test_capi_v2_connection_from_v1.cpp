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

} // namespace

TEST_CASE("V2: connection_create_from_v1_database", "[capi_v2][connection]") {
	duckdb_database v1_db = nullptr;
	duckdb_connection v1_conn = nullptr;
	REQUIRE(duckdb_open(nullptr, &v1_db) == DuckDBSuccess);
	REQUIRE(duckdb_connect(v1_db, &v1_conn) == DuckDBSuccess);

	duckdb_result v1_result;
	REQUIRE(duckdb_query(v1_conn, "CREATE TABLE t AS SELECT range AS i FROM range(5)", &v1_result) == DuckDBSuccess);
	duckdb_destroy_result(&v1_result);
	REQUIRE(duckdb_query(v1_conn, "ATTACH ':memory:' AS other", &v1_result) == DuckDBSuccess);
	duckdb_destroy_result(&v1_result);
	REQUIRE(duckdb_query(v1_conn, "CREATE TABLE other.u AS SELECT 1 AS j", &v1_result) == DuckDBSuccess);
	duckdb_destroy_result(&v1_result);
	REQUIRE(duckdb_query(v1_conn, "SET threads = 1", &v1_result) == DuckDBSuccess);
	duckdb_destroy_result(&v1_result);

	duckdb_v2_connection_handle conn = nullptr;
	REQUIRE(duckdb_v2_connection_create_from_v1_database(reinterpret_cast<duckdb_v2_opaque *>(v1_db), &conn,
	                                                     nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(conn);

	// the catalog and the attached databases are the V1 connection's
	REQUIRE(V2Count(conn, "SELECT * FROM t") == 5);
	REQUIRE(V2Count(conn, "SELECT * FROM other.u") == 1);

	// what the V2 connection writes, the V1 connection reads
	V2Count(conn, "INSERT INTO t VALUES (5), (6)");
	REQUIRE(duckdb_query(v1_conn, "SELECT count(*) FROM t", &v1_result) == DuckDBSuccess);
	REQUIRE(duckdb_value_int64(&v1_result, 0, 0) == 7);
	duckdb_destroy_result(&v1_result);

	// session settings are each connection's own
	REQUIRE(duckdb_query(v1_conn, "SET search_path = 'other'", &v1_result) == DuckDBSuccess);
	duckdb_destroy_result(&v1_result);
	REQUIRE(V2Count(conn, "SELECT * FROM t") == 7);

	duckdb_v2_connection_destroy(&conn);
	duckdb_disconnect(&v1_conn);
	duckdb_close(&v1_db);
}

TEST_CASE("V2: connection_create_from_v1_database error paths", "[capi_v2][connection]") {
	duckdb_v2_connection_handle conn = nullptr;
	duckdb_v2_error_info_handle err = nullptr;
	REQUIRE(duckdb_v2_connection_create_from_v1_database(nullptr, &conn, &err) != DUCKDB_V2_ERROR_NONE);
	REQUIRE(!conn);
	duckdb_v2_error_info_destroy(&err);

	duckdb_database v1_db = nullptr;
	REQUIRE(duckdb_open(nullptr, &v1_db) == DuckDBSuccess);
	REQUIRE(duckdb_v2_connection_create_from_v1_database(reinterpret_cast<duckdb_v2_opaque *>(v1_db), nullptr,
	                                                     nullptr) != DUCKDB_V2_ERROR_NONE);
	duckdb_close(&v1_db);
}
