#include "catch.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/statistics/list_stats.hpp"

using namespace duckdb; // NOLINT

TEST_CASE("Total list length statistics do not wrap on overflow", "[statistics]") {
	const auto max_total = NumericLimits<uint64_t>::Maximum();
	auto list_type = LogicalType::LIST(LogicalType::INTEGER);

	SECTION("MergeInLengths sums totals and invalidates the total on overflow") {
		auto stats = ListStats::CreateEmpty(list_type);
		ListStats::MergeInLengths(stats, 2, 4, 6);
		ListStats::MergeInLengths(stats, 1, 8, 10);
		REQUIRE(ListStats::MinListLength(stats).GetIndex() == 1);
		REQUIRE(ListStats::MaxListLength(stats).GetIndex() == 8);
		REQUIRE(ListStats::TotalListLength(stats).GetIndex() == 16);

		// summing up to the maximum keeps the total known
		ListStats::MergeInLengths(stats, 1, 8, max_total - 16);
		REQUIRE(ListStats::TotalListLength(stats).GetIndex() == max_total);

		// one more element overflows - the total becomes unknown, the bounds remain
		ListStats::MergeInLengths(stats, 1, 8, 1);
		REQUIRE(!ListStats::TotalListLength(stats).IsValid());
		REQUIRE(ListStats::MinListLength(stats).GetIndex() == 1);
		REQUIRE(ListStats::MaxListLength(stats).GetIndex() == 8);

		// the total stays unknown for subsequent merges
		ListStats::MergeInLengths(stats, 1, 8, 42);
		REQUIRE(!ListStats::TotalListLength(stats).IsValid());
	}

	SECTION("Merge sums totals that stay in range") {
		auto left = ListStats::CreateEmpty(list_type);
		auto right = ListStats::CreateEmpty(list_type);
		ListStats::MergeInLengths(left, 2, 4, 100);
		ListStats::MergeInLengths(right, 3, 7, 11);
		ListStats::Merge(left, right);
		REQUIRE(ListStats::MinListLength(left).GetIndex() == 2);
		REQUIRE(ListStats::MaxListLength(left).GetIndex() == 7);
		REQUIRE(ListStats::TotalListLength(left).GetIndex() == 111);
	}

	SECTION("Merge keeps a total that lands exactly on the maximum") {
		auto left = ListStats::CreateEmpty(list_type);
		auto right = ListStats::CreateEmpty(list_type);
		ListStats::MergeInLengths(left, 2, 4, max_total - 11);
		ListStats::MergeInLengths(right, 3, 7, 11);
		ListStats::Merge(left, right);
		REQUIRE(ListStats::TotalListLength(left).GetIndex() == max_total);
	}

	SECTION("Merge with an unknown total keeps the total unknown") {
		auto left = ListStats::CreateEmpty(list_type);
		auto right = ListStats::CreateEmpty(list_type);
		ListStats::MergeInLengths(left, 2, 4, 100);
		ListStats::MergeInLengths(right, 3, 7, 11);
		ListStats::ResetTotalListLength(right);
		ListStats::Merge(left, right);
		REQUIRE(!ListStats::TotalListLength(left).IsValid());
		REQUIRE(ListStats::MinListLength(left).GetIndex() == 2);
		REQUIRE(ListStats::MaxListLength(left).GetIndex() == 7);

		// merging a known total into an unknown one stays unknown
		auto known = ListStats::CreateEmpty(list_type);
		ListStats::MergeInLengths(known, 1, 9, 5);
		ListStats::Merge(left, known);
		REQUIRE(!ListStats::TotalListLength(left).IsValid());
	}

	SECTION("Merge invalidates the total on overflow and keeps the bounds") {
		auto left = ListStats::CreateEmpty(list_type);
		auto right = ListStats::CreateEmpty(list_type);
		ListStats::MergeInLengths(left, 2, 4, max_total - 10);
		ListStats::MergeInLengths(right, 3, 7, 11);
		ListStats::Merge(left, right);
		REQUIRE(!ListStats::TotalListLength(left).IsValid());
		REQUIRE(ListStats::MinListLength(left).GetIndex() == 2);
		REQUIRE(ListStats::MaxListLength(left).GetIndex() == 7);
	}
}
