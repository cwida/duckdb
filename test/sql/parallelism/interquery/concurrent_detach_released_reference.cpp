#include "catch.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "test_helpers.hpp"

#include <atomic>
#include <thread>

using namespace duckdb;

// Under the thread sanitizer: closing on DETACH must synchronize with a reference another thread already dropped
TEST_CASE("DETACH synchronizes with a dropped GetDatabases reference", "[interquery]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("ATTACH ':memory:' AS other"));

	std::atomic<bool> released {false};
	bool found_catalog = false;
	std::thread reader([&]() {
		// look up the catalog by iterating every attached database, as storage extension scanners do
		shared_ptr<AttachedDatabase> other;
		{
			auto databases = DatabaseManager::Get(*db.instance).GetDatabases();
			for (auto &database : databases) {
				if (database->GetName() == "other") {
					other = database;
				}
			}
		}
		if (other) {
			auto &catalog = other->GetCatalog();
			found_catalog = catalog.GetCatalogType() == "duckdb" && catalog.GetName() == "other";
		}
		other.reset();
		// relaxed on purpose: only the reference count may order the reads above before the DETACH
		released.store(true, std::memory_order_relaxed);
	});
	while (!released.load(std::memory_order_relaxed)) {
		std::this_thread::yield();
	}
	auto result = con.Query("DETACH other");
	reader.join();
	REQUIRE_NO_FAIL(*result);
	REQUIRE(found_catalog);
}
