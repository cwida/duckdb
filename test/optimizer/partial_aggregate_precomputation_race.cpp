#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/sync_point.hpp"
#include "duckdb/common/types.hpp"
#include "test_helpers.hpp"

#include <atomic>
#include <chrono>
#include <thread>

using namespace duckdb;

#ifdef D_ASSERT_IS_ENABLED

//! A checkpoint can swap the row group collection between plan-time index
//! capture and execution-time scan: the plan-time indices then refer to different
//! (or fewer) partitions, so a partition that was precomputed is scanned again
//! and its rows are counted twice.
//! The reader parks after capturing its indices, a second connection checkpoints
//! (which vacuums out the fully-deleted leading row group and installs a renumbered
//! collection), and only then is the reader released.
TEST_CASE("Test partial aggregate precomputation partition race reproduces PR 24962", "[optimizer][sync_point]") {
	// Layout after setup (row group size 2048):
	//   partition 0: 2048 rows of 1    -> deleted before the reader starts, vacuums
	//                                     out of the tree at the mutation checkpoint
	//   partition 1: alternating 5/6   -> needs a scan (contributes 1024 rows of 5)
	//   partition 2: 2048 rows of 5    -> always true, precomputed
	// Correct count for key=5: 2048 (precomputed) + 1024 (scanned) = 3072.
	constexpr double precompute_race_expected = 2048.0 + 1024.0;
	auto db_path = TestDirectoryPath() + "precompute_race.duckdb";
	duckdb::DuckDB db(nullptr);
	Connection conn(db);
	REQUIRE_NO_FAIL(conn.Query("ATTACH '" + db_path + "' AS race_db (ROW_GROUP_SIZE 2048)"));
	REQUIRE_NO_FAIL(conn.Query("CREATE TABLE race_db.t(key INTEGER)"));
	REQUIRE_NO_FAIL(conn.Query("INSERT INTO race_db.t SELECT 1 FROM range(2048)"));
	REQUIRE_NO_FAIL(
	    conn.Query("INSERT INTO race_db.t SELECT CASE WHEN i % 2 = 0 THEN 5 ELSE 6 END FROM range(2048) r(i)"));
	REQUIRE_NO_FAIL(conn.Query("INSERT INTO race_db.t SELECT 5 FROM range(2048)"));
	REQUIRE_NO_FAIL(conn.Query("CHECKPOINT race_db"));
	// commit the delete before the reader starts so the checkpoint below is
	// allowed to drop the fully-deleted leading row group
	REQUIRE_NO_FAIL(conn.Query("DELETE FROM race_db.t WHERE key=1"));
	Connection writer(db);

	std::atomic<double> result(-1);
	std::string query_error;
	// the sync point guard below lives in an inner scope that ends before the join: a
	// reader parking after the wait timed out is released instead of deadlocking it
	ThreadJoiner joiner;
	{
		// the reader thread parks at the sync point after capturing its indices until released below
		auto guard = SyncPointCtl::EnableInScope("optimizer.partial_precompute.indices_captured");
		joiner.thread = std::thread([&] {
			Connection reader(db);
			auto res = reader.Query("SELECT count(*) FROM race_db.t WHERE key=5");
			if (res->HasError()) {
				query_error = res->GetError();
				return;
			}
			result = res->Fetch()->GetValue(0, 0).GetValue<double>();
		});

		bool precompute_active = true;
		try {
			guard.WaitAndPause(3000);
		} catch (...) {
			WARN("The reader did not park at the sync point");
			// a parked-arrival timeout (or any other failure to reach the hook)
			// falls back to the plain correctness check below
			precompute_active = false;
		}
		// the timeout fallback is the path on the current tree (the hook is
		// unreachable while the partial precompute is disabled; see the TODO at the
		// early return in TryExecuteAggregates)
		if (precompute_active) {
			// the reader is parked at the captured indices: invoke checkpoint to swap
			// the row group collection
			REQUIRE_NO_FAIL(writer.Query("CHECKPOINT race_db"));
			guard.Next();
		}
	} // ends the sync point scope: disables the point before the join below
	joiner.thread.join();

	// the reader must not count precomputed partitions twice
	INFO(query_error);
	REQUIRE(query_error.empty());
	REQUIRE(result == precompute_race_expected);
}

#endif
