#include "catch.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "test_helpers.hpp"

#include <atomic>
#include <thread>

using namespace duckdb;

// Under TSan: DETACH must synchronize with a database reference another thread already dropped
TEST_CASE("DETACH synchronizes with a dropped database reference", "[interquery]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("ATTACH ':memory:' AS other"));
	auto other = DatabaseManager::Get(*db.instance).GetDatabase("other");
	REQUIRE(other);

	std::atomic<bool> released {false};
	std::thread reader([&]() {
		other->GetCatalog();
		other.reset();
		// relaxed on purpose: only the reference count may order the read above before the DETACH
		released.store(true, std::memory_order_relaxed);
	});
	while (!released.load(std::memory_order_relaxed)) {
		std::this_thread::yield();
	}
	auto result = con.Query("DETACH other");
	reader.join();
	REQUIRE_NO_FAIL(*result);
}
