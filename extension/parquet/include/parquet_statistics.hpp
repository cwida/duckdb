//===----------------------------------------------------------------------===//
//                         DuckDB
//
// parquet_statistics.hpp
//
//
//===----------------------------------------------------------------------===/

#pragma once

#include <stdint.h>
#include <string>

#include "duckdb.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "parquet_types.h"
#include "resizable_buffer.hpp"
#include "duckdb/common/assert.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/mutex.hpp"

#include <functional>

namespace duckdb_apache {
namespace thrift {
namespace protocol {
class TProtocol;
} // namespace protocol
} // namespace thrift
} // namespace duckdb_apache
namespace duckdb_parquet {
class ColumnChunk;
class ColumnMetaData;
class FileMetaData;
class SchemaElement;
class Statistics;
} // namespace duckdb_parquet

namespace duckdb {
class Allocator;
class TableFilter;
enum class LogicalTypeId : uint8_t;

using duckdb_parquet::ColumnChunk;
using duckdb_parquet::SchemaElement;

struct LogicalType;
struct ParquetColumnSchema;
class ResizeableBuffer;

enum class ParquetIntervalBloomFilterVersion : uint8_t { NONE, NORMALIZED_V1 };

enum class ParquetBloomFilterHashStrategy : uint8_t { STANDARD, NORMALIZED_INTERVAL_V1 };

struct ParquetStatisticsUtils {
	static constexpr const char *INTERVAL_BLOOM_FILTER_KEY = "duckdb.interval_bloom_filter";
	static constexpr const char *INTERVAL_BLOOM_FILTER_VALUE = "normalized-v1";

	static unique_ptr<BaseStatistics> TransformColumnStatistics(const ParquetColumnSchema &reader,
	                                                            const duckdb_parquet::ColumnChunkList &columns,
	                                                            bool can_have_nan);

	static unique_ptr<BaseStatistics>
	TransformParquetStatistics(const LogicalType &type, const ParquetColumnSchema &schema,
	                           const duckdb_parquet::Statistics &parquet_stats, bool can_have_nan,
	                           optional_ptr<const ColumnChunk> column_chunk = nullptr);

	//! Whether the column can contain NaN values - uses nan_count if present, otherwise the can_have_nan option
	static bool CanHaveNaN(const duckdb_parquet::Statistics &parquet_stats, bool can_have_nan);

	static Value ConvertValue(const LogicalType &type, const ParquetColumnSchema &schema_ele, const std::string &stats);

	static ParquetIntervalBloomFilterVersion
	GetIntervalBloomFilterVersion(const duckdb_parquet::FileMetaData &file_meta_data);

	static optional<ParquetBloomFilterHashStrategy>
	GetBloomFilterHashStrategy(const ParquetColumnSchema &schema,
	                           ParquetIntervalBloomFilterVersion interval_bloom_filter_version);

	static bool BloomFilterExcludes(const TableFilter &filter, const duckdb_parquet::ColumnMetaData &column_meta_data,
	                                duckdb_apache::thrift::protocol::TProtocol &file_proto, Allocator &allocator,
	                                const ParquetColumnSchema &schema, ParquetBloomFilterHashStrategy hash_strategy);

	static unique_ptr<BaseStatistics> CreateNumericStats(const LogicalType &type, const ParquetColumnSchema &schema_ele,
	                                                     const duckdb_parquet::Statistics &parquet_stats);

private:
	static Value ConvertValueInternal(const LogicalType &type, const ParquetColumnSchema &schema_ele,
	                                  const std::string &stats);
};

class ParquetBloomFilterMemory {
public:
	static constexpr idx_t DEFAULT_LIMIT = 64ULL * 1024 * 1024;

	ParquetBloomFilterMemory(idx_t limit_p, std::function<void()> make_room_p)
	    : limit(limit_p), make_room(std::move(make_room_p)) {
	}
	//! When the budget is exhausted, calls make_room once to free buffered filters and retries
	bool TryReserve(idx_t bytes);
	void Release(idx_t bytes);

private:
	bool TryReserveInternal(idx_t bytes);

private:
	mutex lock;
	idx_t used = 0;
	idx_t limit;
	std::function<void()> make_room;
};

class ParquetBloomFilter {
	static constexpr const idx_t DEFAULT_BLOCK_COUNT = 32; // 4k filter

public:
	ParquetBloomFilter(unique_ptr<ResizeableBuffer> data_p);
	~ParquetBloomFilter();
	static unique_ptr<ParquetBloomFilter> Create(Allocator &allocator, idx_t num_entries, double target);
	static optional_idx EstimateSize(idx_t num_entries, double target);
	static unique_ptr<ParquetBloomFilter> TryCreate(Allocator &allocator, idx_t num_entries, double target,
	                                                shared_ptr<ParquetBloomFilterMemory> memory);
	void FilterInsert(uint64_t x);
	bool FilterCheck(uint64_t x);
	void Shrink(Allocator &allocator, idx_t new_block_count);
	void FoldToTarget(Allocator &allocator, double target);
	void ReleaseReservation();
	double OneRatio();
	ResizeableBuffer *Get();

private:
	unique_ptr<ResizeableBuffer> data;
	idx_t block_count;
	shared_ptr<ParquetBloomFilterMemory> memory;
	idx_t reserved_bytes = 0;
};

// see https://github.com/apache/parquet-format/blob/master/BloomFilter.md

struct ParquetBloomBlock {
	struct ParquetBloomMaskResult {
		uint8_t bit_set[8] = {0};
	};

	uint32_t block[8] = {0};

	static bool check_bit(uint32_t &x, const uint8_t i) {
		D_ASSERT(i < 32);
		return (x >> i) & (uint32_t)1;
	}

	static void set_bit(uint32_t &x, const uint8_t i) {
		D_ASSERT(i < 32);
		x |= (uint32_t)1 << i;
		D_ASSERT(check_bit(x, i));
	}

	static ParquetBloomMaskResult Mask(uint32_t x) {
		static const uint32_t parquet_bloom_salt[8] = {0x47b6137bU, 0x44974d91U, 0x8824ad5bU, 0xa2b7289dU,
		                                               0x705495c7U, 0x2df1424bU, 0x9efc4947U, 0x5c6bfb31U};
		ParquetBloomMaskResult result;
		for (idx_t i = 0; i < 8; i++) {
			result.bit_set[i] = (x * parquet_bloom_salt[i]) >> 27;
		}
		return result;
	}

	static void BlockInsert(ParquetBloomBlock &b, uint32_t x) {
		auto masked = Mask(x);
		for (idx_t i = 0; i < 8; i++) {
			set_bit(b.block[i], masked.bit_set[i]);
			D_ASSERT(check_bit(b.block[i], masked.bit_set[i]));
		}
	}

	static bool BlockCheck(ParquetBloomBlock &b, uint32_t x) {
		auto masked = Mask(x);
		for (idx_t i = 0; i < 8; i++) {
			if (!check_bit(b.block[i], masked.bit_set[i])) {
				return false;
			}
		}
		return true;
	}
};

} // namespace duckdb
