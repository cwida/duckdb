#include "catch.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

// This test triggers an edge case where data that is flushed from a caching operator in pulled into an operator
// that needs to see this chunk more that once. handling this case requires temporarily caching the flushed result.
TEST_CASE("Streaming result with a filter and a cross product", "[filter][.]") {
	DuckDB db(nullptr);
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query("PRAGMA disable_optimizer"));

	REQUIRE_NO_FAIL(con.Query("CREATE TABLE test as SELECT * FROM RANGE(0, 2000000) tbl(i)"));
	auto result =
	    con.Query("SELECT * FROM (SELECT * FROM test where i%1000=0) t1(i), (SELECT * FROM test where i%1000=0) t2(j)");
	REQUIRE_NO_FAIL(*result);

	idx_t expected_count = 4000000;
	idx_t got_count = 0;
	uint64_t expected_sum = 3998000000000l;
	uint64_t i_sum = 0;
	uint64_t j_sum = 0;

	while (true) {
		auto chunk = result->Fetch();
		if (chunk) {
			got_count += chunk->size();
			for (idx_t i = 0; i < chunk->size(); i++) {
				i_sum += chunk->GetValue(0, i).GetValue<uint64_t>();
				j_sum += chunk->GetValue(1, i).GetValue<uint64_t>();
			}
		} else {
			break;
		}
	}
	REQUIRE(got_count == expected_count);
	REQUIRE(i_sum == expected_sum);
	REQUIRE(j_sum == expected_sum);
}
