#include "catch.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/db_instance_cache.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

TEST_CASE("Test that destroying a database instance that was never initialized does not terminate", "[api]") {
	// without Initialize the block allocator and the other members it creates do not exist
	{
		auto instance = make_shared_ptr<DatabaseInstance>();
		REQUIRE(instance);
	}
	REQUIRE(true);
}

TEST_CASE("Test that a failure while configuring a database reaches the caller", "[api]") {
	// Configure sets the file system and initializes the HTTP transport manager before it creates the block
	// allocator, so an invalid pool capacity leaves a partly configured instance behind for the destructor
	DBConfig config;
	config.options.http_client_pool_capacity = 0;
	REQUIRE_THROWS_WITH(DuckDB(nullptr, &config), Catch::Contains("HTTP client pool capacity must be at least 1"));

	// the failure leaves nothing behind that stops a later database from opening
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SELECT 42"));
}

TEST_CASE("Test that a failed cached database construction releases its cache entry", "[api]") {
	DBInstanceCache cache;
	DBConfig config;
	config.options.http_client_pool_capacity = 0;
	REQUIRE_THROWS_WITH(cache.GetOrCreateInstance(":memory:", config, true),
	                    Catch::Contains("HTTP client pool capacity must be at least 1"));
	// the entry the failed attempt reserved is gone, otherwise the next lookup waits for it forever
	REQUIRE(!config.db_cache_entry);

	config.options.http_client_pool_capacity = DConstants::INVALID_INDEX;
	auto db = cache.GetOrCreateInstance(":memory:", config, true);
	REQUIRE(db);
	Connection con(*db);
	REQUIRE_NO_FAIL(con.Query("SELECT 42"));
}
