//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/external_file_cache/external_file_cache_stats.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/typedefs.hpp"

namespace duckdb {

//! Read statistics of the external file cache, cumulative since database start and updated lock-free by readers
struct ExternalFileCacheStats {
	//! Bytes requested by readers through the cache, clamped to the file size
	atomic<idx_t> read_request_bytes {0};
	//! Requested bytes served from cached blocks
	atomic<idx_t> cache_hit_request_bytes {0};
	//! Bytes read from the underlying file system, including alignment and prefetch beyond the request
	atomic<idx_t> actual_io_bytes {0};
	//! Requested blocks served from the cache (memory, spill, or a concurrent reader's fetch)
	atomic<idx_t> block_hit_count {0};
	//! Requested blocks fetched from the underlying file system
	atomic<idx_t> block_miss_count {0};
	//! Evicted cache blocks that had to be fetched again
	atomic<idx_t> eviction_refetch_count {0};
};

} // namespace duckdb
