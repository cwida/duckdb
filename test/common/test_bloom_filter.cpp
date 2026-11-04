#include "catch.hpp"
#include "duckdb/common/types/hash.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "test_helpers.hpp"

using namespace duckdb; // NOLINT

//! Inserts the hashes of the keys [start, start + count) into the filter
static void InsertKeyRange(BloomFilter &filter, idx_t start, idx_t count) {
	Vector hashes(LogicalType::HASH);
	for (idx_t offset = 0; offset < count; offset += STANDARD_VECTOR_SIZE) {
		const auto n = MinValue<idx_t>(STANDARD_VECTOR_SIZE, count - offset);
		auto writer = FlatVector::Writer<hash_t>(hashes, n);
		for (idx_t i = 0; i < n; i++) {
			writer.WriteValue(Hash(start + offset + i));
		}
		filter.InsertHashes(hashes, *FlatVector::IncrementalSelectionVector(), n);
	}
}

//! Returns how many of the keys [start, start + count) the filter contains
static idx_t CountKeyRange(const BloomFilter &filter, idx_t start, idx_t count) {
	idx_t found = 0;
	for (idx_t i = 0; i < count; i++) {
		found += filter.LookupOne(Hash(start + i));
	}
	return found;
}

TEST_CASE("Sealing an oversized bloom filter folds it without losing membership", "[bloom_filter]") {
	DuckDB db;
	Connection con(db);
	auto &context = *con.context;

	// sized for a million keys but filled with a thousand, so Seal folds the filter down aggressively
	static constexpr idx_t ESTIMATED_COUNT = 1000000;
	static constexpr idx_t ACTUAL_COUNT = 1000;

	BloomFilter filter;
	filter.Initialize(context, ESTIMATED_COUNT);
	InsertKeyRange(filter, 0, ACTUAL_COUNT);
	filter.Seal(context);

	REQUIRE(filter.IsSealed());
	REQUIRE(filter.IsUseful());

	// a bloom filter can produce false positives but never false negatives
	REQUIRE(CountKeyRange(filter, 0, ACTUAL_COUNT) == ACTUAL_COUNT);
}

TEST_CASE("Sealing marks a saturated bloom filter as not useful", "[bloom_filter]") {
	DuckDB db;
	Connection con(db);
	auto &context = *con.context;

	// sized for a thousand keys but filled with a million, so the filter saturates
	static constexpr idx_t ESTIMATED_COUNT = 1000;
	static constexpr idx_t ACTUAL_COUNT = 1000000;

	BloomFilter filter;
	filter.Initialize(context, ESTIMATED_COUNT);
	InsertKeyRange(filter, 0, ACTUAL_COUNT);
	filter.Seal(context);

	REQUIRE(filter.IsSealed());
	REQUIRE_FALSE(filter.IsUseful());

	// even a saturated filter must keep every inserted key
	REQUIRE(CountKeyRange(filter, 0, ACTUAL_COUNT) == ACTUAL_COUNT);
}

TEST_CASE("Bloom vector lookup respects counts and selected source rows", "[bloom_filter]") {
	DuckDB db;
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET memory_limit='512MB'"));
	auto &context = *con.context;
	const auto vector_rows = MaxValue<idx_t>(160, STANDARD_VECTOR_SIZE + 17);

	Vector hashes(LogicalType::HASH, vector_rows);
	for (idx_t i = 0; i < vector_rows; i++) {
		hashes.SetValue(i, Value::HASH(Hash(i % 3 == 0 ? i % 8 : (1ULL << 21) + i)));
	}
	FlatVector::SetSize(hashes, count_t(vector_rows));

	// Accurate build cardinality keeps the large filter above the prefetch threshold after sealing.
	for (idx_t build_count : {idx_t(8), idx_t(1) << 20}) {
		BloomFilter filter;
		filter.Initialize(context, build_count);
		InsertKeyRange(filter, 0, build_count);
		filter.Seal(context);
		REQUIRE(filter.IsUseful());

		for (idx_t count : {idx_t(0), idx_t(1), idx_t(63), idx_t(64), idx_t(65), idx_t(127), idx_t(128), idx_t(129),
		                    vector_rows - 1}) {
			for (bool mapped : {false, true}) {
				CAPTURE(build_count, count, mapped);
				SelectionVector input_sel(count), result_sel(count);
				vector<idx_t> expected;
				for (idx_t i = 0; i < count; i++) {
					input_sel.set_index(i, vector_rows - i - 1);
					const auto row = mapped ? input_sel.get_index(i) : i;
					if (filter.LookupOne(hashes.GetValue(row).GetValue<hash_t>())) {
						expected.push_back(i);
					}
				}
				const auto found_count = mapped ? filter.LookupHashes(hashes, input_sel, result_sel, count)
				                                : filter.LookupHashes(hashes, result_sel, count);
				REQUIRE(found_count == expected.size());
				for (idx_t i = 0; i < found_count; i++) {
					REQUIRE(result_sel.get_index(i) == expected[i]);
				}
			}
		}
	}
}
