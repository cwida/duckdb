#include "catch.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

TEST_CASE("Test operator_memory_limit session isolation", "[api][memory]") {
	DuckDB db(nullptr);
	Connection con_a(db);
	Connection con_b(db);

	// default is NULL for both
	auto result_a = con_a.Query("SELECT current_setting('operator_memory_limit')");
	REQUIRE_NO_FAIL(*result_a);
	REQUIRE(result_a->Collection().GetValue(0, 0).IsNull());

	auto result_b = con_b.Query("SELECT current_setting('operator_memory_limit')");
	REQUIRE_NO_FAIL(*result_b);
	REQUIRE(result_b->Collection().GetValue(0, 0).IsNull());

	// set on connection B only
	REQUIRE_NO_FAIL(con_b.Query("SET operator_memory_limit='64MB'"));

	// connection A still has NULL
	result_a = con_a.Query("SELECT current_setting('operator_memory_limit')");
	REQUIRE_NO_FAIL(*result_a);
	REQUIRE(result_a->Collection().GetValue(0, 0).IsNull());

	// connection B has the new value
	result_b = con_b.Query("SELECT current_setting('operator_memory_limit')");
	REQUIRE_NO_FAIL(*result_b);
	REQUIRE(!result_b->Collection().GetValue(0, 0).IsNull());

	// reset on B
	REQUIRE_NO_FAIL(con_b.Query("RESET operator_memory_limit"));
	result_b = con_b.Query("SELECT current_setting('operator_memory_limit')");
	REQUIRE_NO_FAIL(*result_b);
	REQUIRE(result_b->Collection().GetValue(0, 0).IsNull());
}
