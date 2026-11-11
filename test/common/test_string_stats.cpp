#include "catch.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/statistics/string_stats.hpp"

using namespace duckdb; // NOLINT

//! Build string stats data that only carries length information, as a disjoint data set for MERGE_STATS
static StringStatsData LengthStats(uint32_t min_length, uint32_t max_length, uint64_t total_length) {
	StringStatsData data;
	data.min_type = StringStatsType::EMPTY_STATS;
	data.max_type = StringStatsType::EMPTY_STATS;
	data.has_unicode = false;
	data.has_max_string_length = true;
	data.max_string_length = max_length;
	data.has_min_string_length = true;
	data.min_string_length = min_length;
	data.has_total_string_length = true;
	data.total_string_length = total_length;
	return data;
}

TEST_CASE("Total string length statistics do not wrap on overflow", "[statistics]") {
	const auto max_total = NumericLimits<uint64_t>::Maximum();

	SECTION("Merge sums totals and invalidates the total on overflow") {
		auto stats = StringStats::CreateEmpty(LogicalType::VARCHAR);
		StringStats::Merge(stats, LengthStats(2, 4, 6));
		StringStats::Merge(stats, LengthStats(1, 8, 10));
		REQUIRE(StringStats::MinStringLength(stats).GetIndex() == 1);
		REQUIRE(StringStats::MaxStringLength(stats) == 8);
		REQUIRE(StringStats::TotalStringLength(stats).GetIndex() == 16);

		// summing up to the maximum keeps the total known
		StringStats::Merge(stats, LengthStats(1, 8, max_total - 16));
		REQUIRE(StringStats::TotalStringLength(stats).GetIndex() == max_total);

		// one more byte overflows - the total becomes unknown, the bounds remain
		StringStats::Merge(stats, LengthStats(1, 8, 1));
		REQUIRE(!StringStats::TotalStringLength(stats).IsValid());
		REQUIRE(StringStats::MinStringLength(stats).GetIndex() == 1);
		REQUIRE(StringStats::MaxStringLength(stats) == 8);

		// the total stays unknown for subsequent merges
		StringStats::Merge(stats, LengthStats(1, 8, 42));
		REQUIRE(!StringStats::TotalStringLength(stats).IsValid());
	}

	SECTION("Merge keeps a total that lands exactly on the maximum") {
		auto stats = StringStats::CreateEmpty(LogicalType::VARCHAR);
		StringStats::Merge(stats, LengthStats(2, 4, max_total - 11));
		StringStats::Merge(stats, LengthStats(3, 7, 11));
		REQUIRE(StringStats::TotalStringLength(stats).GetIndex() == max_total);
	}

	SECTION("Merge with an unknown total keeps the total unknown") {
		auto stats = StringStats::CreateEmpty(LogicalType::VARCHAR);
		StringStats::Merge(stats, LengthStats(2, 4, 100));
		auto unknown_total = LengthStats(3, 7, 0);
		unknown_total.has_total_string_length = false;
		StringStats::Merge(stats, unknown_total);
		REQUIRE(!StringStats::TotalStringLength(stats).IsValid());
		REQUIRE(StringStats::MinStringLength(stats).GetIndex() == 2);
		REQUIRE(StringStats::MaxStringLength(stats) == 7);

		// merging a known total into an unknown one stays unknown
		StringStats::Merge(stats, LengthStats(1, 9, 5));
		REQUIRE(!StringStats::TotalStringLength(stats).IsValid());
	}

	SECTION("Merge invalidates the total on overflow and keeps the bounds") {
		auto stats = StringStats::CreateEmpty(LogicalType::VARCHAR);
		StringStats::Merge(stats, LengthStats(2, 4, max_total - 10));
		StringStats::Merge(stats, LengthStats(3, 7, 11));
		REQUIRE(!StringStats::TotalStringLength(stats).IsValid());
		REQUIRE(StringStats::MinStringLength(stats).GetIndex() == 2);
		REQUIRE(StringStats::MaxStringLength(stats) == 7);
	}
}
