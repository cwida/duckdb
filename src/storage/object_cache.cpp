#include "duckdb/storage/object_cache.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/storage/buffer/buffer_pool.hpp"

namespace duckdb {

ObjectCache &ObjectCache::GetObjectCache(ClientContext &context) {
	return context.db->GetObjectCache();
}

bool ObjectCache::EvictToMemoryLimit(idx_t memory_limit) {
	constexpr idx_t EVICTION_BATCH_SIZE = 16;
	while (true) {
		// Release the cache lock between batches to let lookups make progress.
		const lock_guard<mutex> lock(lock_mutex);
		for (idx_t i = 0; i < EVICTION_BATCH_SIZE; i++) {
			// Entries can own reservations beyond their cache weight, or remain alive after eviction.
			if (buffer_pool.GetUsedMemory() <= memory_limit) {
				return true;
			}
			if (!lru_cache.EvictLeastRecentlyUsed()) {
				return false;
			}
		}
	}
}

} // namespace duckdb
