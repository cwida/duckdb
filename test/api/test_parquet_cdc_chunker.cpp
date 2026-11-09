#include "catch.hpp"
#include "writer/parquet_cdc_chunker.hpp"

#include <cstring>
#include <vector>

using namespace duckdb;

//! These tests pin exact cut positions so a chunker change that moves boundaries - the whole thing the option
//! exists for - fails here instead of silently drifting files. The stream is built from raw bytes, not from
//! native values, so the expectations hold on any endianness.

namespace {

struct SplitMix64 {
	uint64_t state = 0;

	uint64_t Next() {
		state += 0x9E3779B97F4A7C15ULL;
		uint64_t z = state;
		z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
		z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
		return z ^ (z >> 31);
	}
};

constexpr idx_t NUM_VALUES = 40000;
constexpr idx_t VAL_LEN = 32;

static std::vector<uint8_t> MakeValue(SplitMix64 &rng) {
	std::vector<uint8_t> value(VAL_LEN);
	for (idx_t b = 0; b < VAL_LEN; b += 8) {
		uint64_t mixed = rng.Next();
		for (idx_t j = 0; j < 8; j++) {
			value[b + j] = static_cast<uint8_t>((mixed >> (8 * j)) & 0xFF);
		}
	}
	return value;
}

static ParquetContentDefinedChunker::Options TestOptions() {
	ParquetContentDefinedChunker::Options options;
	options.min_chunk_size = 65536;
	options.max_chunk_size = 262144;
	options.norm_level = 0;
	return options;
}

//! The indices of the values that open a new chunk, feeding the whole stream in one run. When with_levels is
//! set, every seventh value is a null: the definition level is hashed and no value bytes are, like the writer
//! does for an OPTIONAL column.
static std::vector<idx_t> FeedCuts(const std::vector<std::vector<uint8_t>> &values, bool with_levels) {
	ParquetContentDefinedChunker chunker;
	chunker.Reset(TestOptions());
	std::vector<idx_t> cuts;
	for (idx_t i = 0; i < values.size(); i++) {
		const bool is_null = with_levels && (i % 7) == 3;
		if (with_levels) {
			chunker.RollLevel(is_null ? uint16_t(0) : uint16_t(1));
		}
		if (!is_null) {
			chunker.RollBytes(values[i].data(), values[i].size());
		}
		if (chunker.NeedNewChunk()) {
			cuts.push_back(i);
		}
	}
	return cuts;
}

} // namespace

TEST_CASE("Parquet CDC chunker pins Arrow's default mask", "[parquet_cdc]") {
	REQUIRE(ParquetContentDefinedChunker::CalculateMask(PARQUET_CDC_DEFAULT_MIN_CHUNK_SIZE,
	                                                    PARQUET_CDC_DEFAULT_MAX_CHUNK_SIZE, 0) == (~uint64_t(0) << 49));
}

TEST_CASE("Parquet CDC chunker cut positions are frozen", "[parquet_cdc]") {
	SplitMix64 rng;
	rng.state = 0x123456789ABCDEF0ULL;
	std::vector<std::vector<uint8_t>> values;
	for (idx_t i = 0; i < NUM_VALUES; i++) {
		values.push_back(MakeValue(rng));
	}

	// frozen on the stream above with { min_chunk_size: 65536, max_chunk_size: 262144 }
	const std::vector<idx_t> expected {{4699, 8735, 12390, 16655, 19989, 23462, 26960, 30632, 34413, 38616}};
	// the same stream as OPTIONAL: hashing the definition levels moves every boundary
	const std::vector<idx_t> expected_with_nulls {{3541, 7558, 11829, 15944, 21300, 25330, 31016, 35511, 38820}};

	auto cuts = FeedCuts(values, false);
	REQUIRE(cuts == expected);
	auto cuts_nulls = FeedCuts(values, true);
	REQUIRE(cuts_nulls == expected_with_nulls);
	REQUIRE(cuts_nulls != cuts);
}

TEST_CASE("Parquet CDC chunker state carries across segments", "[parquet_cdc]") {
	SplitMix64 rng;
	rng.state = 0x123456789ABCDEF0ULL;
	std::vector<std::vector<uint8_t>> values;
	for (idx_t i = 0; i < NUM_VALUES; i++) {
		values.push_back(MakeValue(rng));
	}

	// one run, the reference
	const auto reference = FeedCuts(values, false);

	// two segments with no Reset between them, as successive row groups of one column reach the chunker
	ParquetContentDefinedChunker chunker;
	chunker.Reset(TestOptions());
	std::vector<idx_t> carried;
	for (idx_t segment = 0; segment < 2; segment++) {
		const idx_t begin = segment * (NUM_VALUES / 2);
		const idx_t end = begin + NUM_VALUES / 2;
		for (idx_t i = begin; i < end; i++) {
			chunker.RollBytes(values[i].data(), values[i].size());
			if (chunker.NeedNewChunk()) {
				carried.push_back(i);
			}
		}
	}
	REQUIRE(carried == reference);
}

TEST_CASE("Parquet CDC chunker maximum is a hard cut", "[parquet_cdc]") {
	ParquetContentDefinedChunker chunker;
	ParquetContentDefinedChunker::Options options;
	options.min_chunk_size = 2048;
	options.max_chunk_size = 4096;
	chunker.Reset(options);

	// identical values: whatever the hash does, no chunk may span more than max_chunk_size of value bytes
	const std::vector<uint8_t> same(VAL_LEN, 'x');
	std::vector<idx_t> cuts;
	for (idx_t i = 0; i < 8192; i++) {
		chunker.RollBytes(same.data(), same.size());
		if (chunker.NeedNewChunk()) {
			cuts.push_back(i);
		}
	}
	REQUIRE(!cuts.empty());
	REQUIRE(cuts[0] >= options.min_chunk_size / VAL_LEN);
	idx_t previous = 0;
	for (auto cut : cuts) {
		// a value straddling the limit is hashed whole, so a gap can reach one value past the maximum
		REQUIRE(cut - previous <= options.max_chunk_size / VAL_LEN + 1);
		previous = cut;
	}
}

TEST_CASE("Parquet CDC chunker mask rejects unusable sizes", "[parquet_cdc]") {
	REQUIRE_THROWS_AS(ParquetContentDefinedChunker::CalculateMask(1024, 1024, 0), BinderException);
	REQUIRE_THROWS_AS(ParquetContentDefinedChunker::CalculateMask(2048, 1024, 0), BinderException);
	// (min + max) / 2 - min below one gear-table run of bytes leaves no usable bits
	REQUIRE_THROWS_AS(ParquetContentDefinedChunker::CalculateMask(8, 16, 0), BinderException);
	// raising norm_level below one bit is the same failure
	REQUIRE_THROWS_AS(ParquetContentDefinedChunker::CalculateMask(65536, 262144, 14), BinderException);
}
