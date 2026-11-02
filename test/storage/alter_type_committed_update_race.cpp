#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/sync_point.hpp"
#include "duckdb/common/types.hpp"
#include "test_helpers.hpp"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace duckdb;

#ifdef D_ASSERT_IS_ENABLED

//! The ALTER TYPE rewrite swaps in a new column that does not inherit the old
//! column's update segments, so an UPDATE committing while the rewrite runs must
//! not be silently dropped (PR #25008).
//! The invariant is "no silent loss": either the UPDATE commits and its new value
//! is visible, or it aborts loudly.
TEST_CASE("Test ALTER TYPE does not drop a concurrently committed update", "[storage][sync_point]") {
	duckdb::DuckDB db(nullptr);
	Connection conn(db);
	REQUIRE_NO_FAIL(conn.Query("CREATE TABLE t(id INTEGER, v INTEGER)"));
	REQUIRE_NO_FAIL(conn.Query("INSERT INTO t VALUES (1, 0)"));

	std::string alter_error;
	std::atomic<bool> update_succeeded(false);
	// the sync point guard below lives in an inner scope that ends before the join: an
	// alter thread parking after the wait timed out is released instead of deadlocking it
	ThreadJoiner joiner;
	{
		// the alter thread parks at the sync point until released below
		auto guard = SyncPointCtl::EnableInScope("alter_type.rewrite_scan_complete");
		joiner.thread = std::thread([&] {
			Connection alter(db);
			auto res = alter.Query("ALTER TABLE t ALTER COLUMN v SET DATA TYPE BIGINT");
			if (res->HasError()) {
				alter_error = res->GetError();
			}
		});

		bool rewrite_parked = true;
		try {
			guard.WaitAndPause(3000);
		} catch (const InternalException &) {
			WARN("The ALTER TYPE rewrite did not park at the sync point");
			rewrite_parked = false;
		}
		if (rewrite_parked) {
			// the rewrite is parked mid-flight: commit the update in a separate connection
			Connection updater(db);
			auto res = updater.Query("UPDATE t SET v = 7");
			update_succeeded = !res->HasError();
			guard.Next();
		}
	} // ends the sync point scope: disables the point before the join below
	joiner.thread.join();
	REQUIRE(alter_error.empty());

	// check the final value
	double final_value = -1;
	auto check = conn.Query("SELECT v FROM t");
	REQUIRE(!check->HasError());
	final_value = check->Fetch()->GetValue(0, 0).GetValue<double>();
	INFO(StringUtil::Format("update_succeeded=%d final_value=%f", (int)update_succeeded.load(), final_value));
	REQUIRE((!update_succeeded || final_value == 7.0));
}

#endif
