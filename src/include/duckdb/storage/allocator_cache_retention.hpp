//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/allocator_cache_retention.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/array.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/typedefs.hpp"

namespace duckdb {

//! Approximate recent returns minus reuse, decaying linearly after each time bucket ends.
class AllocatorCacheRetention {
	friend class BlockAllocatorTestHelper;

public:
	static constexpr idx_t INTERVAL_MS = 500;
	static constexpr idx_t DECAY_MS = 1000;
	static constexpr idx_t BUCKET_COUNT = DECAY_MS / INTERVAL_MS + 1;

	AllocatorCacheRetention() {
		Reset(0);
	}

	//! Reset only before publication or when callers are quiescent.
	void Reset(idx_t now_ms);
	void Return(idx_t count, idx_t capacity, idx_t now_ms);
	void Reuse(idx_t count, idx_t now_ms);
	idx_t Target(idx_t now_ms) const;

private:
	//! Generation and count share one atomic word so slot reuse cannot race a stale update.
	array<atomic<uint64_t>, BUCKET_COUNT> buckets;
};

} // namespace duckdb
