#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/function/cast/cast_function_set.hpp"
#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

using namespace duckdb;

namespace {

idx_t registered_cast_calls = 0;

bool CountingBigintToSmallint(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	registered_cast_calls++;
	for (idx_t i = 0; i < count; i++) {
		auto value = source.GetValue(i);
		result.SetValue(i, value.IsNull() ? Value(LogicalType::SMALLINT) : value.DefaultCastAs(LogicalType::SMALLINT));
	}
	return true;
}

} // namespace

TEST_CASE("Compressed materialization uses registered casts", "[optimizer][compressed_materialization]") {
	DuckDB db;
	Connection con(db);
	ExtensionLoader loader(*db.instance, "compressed_materialization_cast_extension");
	loader.RegisterCastFunction(LogicalType::BIGINT, LogicalType::SMALLINT, BoundCastInfo(CountingBigintToSmallint), 0);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT (range % 1000)::BIGINT AS i FROM range(100000)"));

	registered_cast_calls = 0;
	auto result = con.Query("SELECT i, count(*) FROM t GROUP BY i ORDER BY i LIMIT 3");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(CHECK_COLUMN(result, 0, {0, 1, 2}));
	REQUIRE(CHECK_COLUMN(result, 1, {100, 100, 100}));
	REQUIRE(registered_cast_calls > 0);
}
