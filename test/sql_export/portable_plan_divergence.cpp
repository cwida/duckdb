#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/string_util.hpp"

using namespace duckdb;

// The demonstration needs to capture the generated SQL text and execute it later, after the data
// changed. SQL cannot capture EXPLAIN output for re-execution (query() accepts a single SELECT),
// so this lives in C++. The textual and snapshot-identity coverage is SQL-reachable and lives in
// test/sql/sql_export/portable_plan_profile.test

static string ExplainSQL(Connection &connection, const string &query) {
	auto result = connection.Query("EXPLAIN (SQL) " + query);
	REQUIRE_NO_FAIL(*result);
	return result->Collection().GetValue(1, 0).ToString();
}

TEST_CASE("Profiled export text re-derives data observations, folded text does not",
          "[sql_export][portable_plan]") {
	DuckDB db(nullptr);
	Connection connection(db);
	REQUIRE_NO_FAIL(connection.Query("CREATE TABLE obs(i INTEGER)"));
	REQUIRE_NO_FAIL(connection.Query("INSERT INTO obs SELECT range FROM range(100)"));

	const string query = "SELECT count(*) FROM obs WHERE i < 1000";
	auto folded = ExplainSQL(connection, query);
	REQUIRE_NO_FAIL(connection.Query("SET portable_plan=true"));
	auto profiled = ExplainSQL(connection, query);
	REQUIRE_NO_FAIL(connection.Query("SET portable_plan=false"));

	// the folded text bakes the observed count, the profiled text keeps the aggregate
	REQUIRE(StringUtil::Contains(folded, "100"));
	REQUIRE(StringUtil::Contains(profiled, "count"));

	// on the planning snapshot both texts give the planned answer
	for (auto &sql : {folded, profiled}) {
		auto result = connection.Query(sql);
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->Collection().GetValue(0, 0) == Value::BIGINT(100));
	}

	// after the data changes, the profiled text sees the change while the folded text repeats
	// the stale observation - the profile is a semantic choice, not a cosmetic one.
	// the row at 1500 fails the predicate, so a text that dropped the predicate gives 102
	REQUIRE_NO_FAIL(connection.Query("INSERT INTO obs VALUES (500), (1500)"));
	auto profiled_result = connection.Query(profiled);
	REQUIRE_NO_FAIL(*profiled_result);
	REQUIRE(profiled_result->Collection().GetValue(0, 0) == Value::BIGINT(101));
	auto folded_result = connection.Query(folded);
	REQUIRE_NO_FAIL(*folded_result);
	REQUIRE(folded_result->Collection().GetValue(0, 0) == Value::BIGINT(100));
}
