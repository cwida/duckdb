#include "catch.hpp"
#include "duckdb/common/time_point.hpp"

#include <thread>

using namespace duckdb; // NOLINT

// A clock that steps backwards (duckdb#27790) must never yield a negative interval: callers cast them to unsigned.
TEST_CASE("TimePoint intervals are never negative", "[time_point]") {
	auto earlier = TimePoint::Tick();
	std::this_thread::sleep_for(std::chrono::milliseconds(2));
	auto later = TimePoint::Tick();

	REQUIRE(TimePoint::ElapsedMicros(earlier, later) > 0);

	// Measuring from the later point to the earlier one is what a backwards clock step produces.
	REQUIRE(TimePoint::ElapsedSeconds(later, earlier) == 0);
	REQUIRE(TimePoint::ElapsedMillis(later, earlier) == 0);
	REQUIRE(TimePoint::ElapsedMicros(later, earlier) == 0);
	REQUIRE(TimePoint::ElapsedNanos(later, earlier) == 0);
}
