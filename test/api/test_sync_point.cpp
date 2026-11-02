#include "catch.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/sync_point.hpp"
#include "duckdb/common/types.hpp"

#include <atomic>
#include <chrono>
#include <thread>

using namespace duckdb;

#ifdef D_ASSERT_IS_ENABLED

namespace {

//! Joins the thread even when a REQUIRE fails before the expected handshake
//! completed. The sync point guard must be declared after this joiner so that
//! on stack unwinding the point is disabled (releasing the thread) first.
struct SyncPointThreadJoiner {
	std::thread thread;
	~SyncPointThreadJoiner() {
		if (thread.joinable()) {
			thread.join();
		}
	}
};

} // namespace

TEST_CASE("Test sync point handshake ordering", "[api]") {
	SyncPointThreadJoiner joiner;
	auto guard = SyncPointCtl::EnableInScope("test.handshake_order");
	std::atomic<bool> advanced(false);
	joiner.thread = std::thread([&] {
		SYNC_POINT("test.handshake_order");
		advanced = true;
	});
	// WaitAndPause only returns once the business thread is suspended at the point
	guard.WaitAndPause();
	REQUIRE(!advanced);
	guard.Next();
	joiner.thread.join();
	REQUIRE(advanced);
}

TEST_CASE("Test sync point is a no-op when disabled", "[api]") {
	// the point never existed - Sync must return immediately
	SYNC_POINT("test.never_enabled");
	// the same point must become a no-op again after disable
	auto guard = SyncPointCtl::EnableInScope("test.enabled_then_disabled");
	guard.Disable();
	SYNC_POINT("test.enabled_then_disabled");
}

TEST_CASE("Test sync point disable releases a suspended thread", "[api]") {
	SyncPointThreadJoiner joiner;
	auto guard = SyncPointCtl::EnableInScope("test.disable_release");
	std::atomic<bool> finished(false);
	joiner.thread = std::thread([&] {
		SYNC_POINT("test.disable_release");
		finished = true;
	});
	guard.WaitAndPause();
	REQUIRE(!finished);
	guard.Disable();
	joiner.thread.join();
	REQUIRE(finished);
}

TEST_CASE("Test sync point disable aborts a blocked WaitAndPause", "[api]") {
	SyncPointThreadJoiner joiner;
	auto guard = SyncPointCtl::EnableInScope("test.disable_abort");
	std::atomic<bool> threw(false);
	joiner.thread = std::thread([&] {
		try {
			SyncPointCtl::WaitAndPause("test.disable_abort");
		} catch (...) {
			// any exception proves the abort took effect
			threw = true;
		}
	});
	// give the waiter a moment to block on the arrival channel
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	guard.Disable();
	joiner.thread.join();
	REQUIRE(threw);
}

TEST_CASE("Test sync point wait times out", "[api]") {
	auto guard = SyncPointCtl::EnableInScope("test.timeout");
	REQUIRE_THROWS(SyncPointCtl::WaitAndPause("test.timeout", 25));
}

TEST_CASE("Test sync point can be re-enabled on a single name", "[api]") {
	for (idx_t i = 0; i < 2; i++) {
		SyncPointThreadJoiner joiner;
		auto guard = SyncPointCtl::EnableInScope("test.repeated");
		std::atomic<bool> advanced(false);
		joiner.thread = std::thread([&] {
			SYNC_POINT("test.repeated");
			advanced = true;
		});
		guard.WaitAndPause();
		guard.Next();
		joiner.thread.join();
		REQUIRE(advanced);
	}
}

TEST_CASE("Test sync point queues multiple arrivals on one point", "[api]") {
	SyncPointThreadJoiner joiner_a;
	SyncPointThreadJoiner joiner_b;
	auto guard = SyncPointCtl::EnableInScope("test.multiple");
	joiner_a.thread = std::thread([] { SYNC_POINT("test.multiple"); });
	joiner_b.thread = std::thread([] { SYNC_POINT("test.multiple"); });
	guard.WaitAndPause();
	guard.Next();
	guard.WaitAndPause();
	guard.Next();
	joiner_a.thread.join();
	joiner_b.thread.join();
}

#endif
