#include "catch.hpp"
#include "test_helpers.hpp"

using namespace duckdb;
using namespace std;

TEST_CASE("Test handling of overflows in basic types", "[overflowhandling]") {
	unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);
	con.EnableQueryVerification();

	result = con.Query("CREATE TABLE test (a INTEGER, b INTEGER);");

	// insert too large value for domain should cause error
	// FIXME new pg parser zeroes first param
	//REQUIRE_FAIL(con.Query("INSERT INTO test VALUES (-1099511627776, 3)"));

	REQUIRE_NO_FAIL(con.Query("INSERT INTO test VALUES (11, 22), (12, 21), (14, 22)"));

	//! Casting NULL should still work though
	REQUIRE_NO_FAIL(con.Query("SELECT ALL CAST ( - SUM ( DISTINCT - CAST ( "
	                          "NULL AS INTEGER ) ) AS INTEGER ) FROM test"));

	// proper upcasting of integer columns in AVG
	result = con.Query("SELECT b, AVG(a) FROM test GROUP BY b ORDER BY b;");
	REQUIRE(CHECK_COLUMN(result, 0, {21, 22}));

	// cast to bigger type if it will overflow
	result = con.Query("SELECT cast(200 AS TINYINT)");
	REQUIRE(CHECK_COLUMN(result, 0, {200}));

	// try to use the NULL value of a type
	result = con.Query("SELECT cast(-127 AS TINYINT)");
	REQUIRE(CHECK_COLUMN(result, 0, {-127}));

	// promote on addition overflow
	result = con.Query("SELECT cast(100 AS TINYINT) + cast(100 AS TINYINT)");
	REQUIRE(CHECK_COLUMN(result, 0, {200}));

	// also with tables
	result = con.Query("CREATE TABLE test2 (a INTEGER, b TINYINT);");
	result = con.Query("INSERT INTO test2 VALUES (200, 60), (12, 60), (14, 60)");

	// cast to bigger type if it will overflow
	result = con.Query("SELECT cast(a AS TINYINT) FROM test2");
	REQUIRE(CHECK_COLUMN(result, 0, {200, 12, 14}));

	// cast to bigger type if SUM overflows
	result = con.Query("SELECT SUM(b) FROM test2");
	REQUIRE(CHECK_COLUMN(result, 0, {180}));

	return;
	// promote overflows in more complicated expression chains
	// FIXME: need to fix statistics propagation
	result = con.Query("SELECT a + b FROM (SELECT cast(100 AS TINYINT) AS a, cast(100 AS TINYINT) AS b) tbl1");
	REQUIRE(CHECK_COLUMN(result, 0, {200}));
}
