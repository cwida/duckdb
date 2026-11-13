//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/object_cache.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/memory_tag.hpp"
#include "duckdb/common/lru_cache.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/storage/buffer/buffer_pool_reservation.hpp"

namespace duckdb {
class ClientContext;

struct BufferPoolPayload {
	explicit BufferPoolPayload(unique_ptr<TempBufferPoolReservation> &&res) : reservation(std::move(res)) {
	}
	~BufferPoolPayload() {
		reservation->Resize(0);
	}
	idx_t GetWeight() const {
		return reservation->size;
	}
	unique_ptr<BufferPoolReservation> reservation;
};

// Forward declaration.
class BufferPool;

//! ObjectCache is the base class for objects caches in DuckDB
class ObjectCacheEntry {
public:
	virtual ~ObjectCacheEntry() {
	}

	virtual string GetObjectType() = 0;

	//! Get the rough cache memory usage in bytes for this entry.
	//! Used for eviction decisions. Return invalid index to prevent eviction.
	virtual optional_idx GetEstimatedCacheMemory() const = 0;
};

struct CleanupBufferPool {
	void operator()(unique_ptr<BufferPoolReservation> &buffer) {
		D_ASSERT(buffer);
		buffer->Resize(0);
	}
};

class ObjectCache {
public:
	//! Default max memory 8GiB for non-evictable cache entries.
	static constexpr idx_t DEFAULT_MAX_MEMORY = 8ULL * 1024 * 1024 * 1024;

	explicit ObjectCache(BufferPool &buffer_pool_p) : ObjectCache(DEFAULT_MAX_MEMORY, buffer_pool_p) {
	}

	ObjectCache(idx_t max_memory, BufferPool &buffer_pool_p) : lru_cache(max_memory), buffer_pool(buffer_pool_p) {
	}

	shared_ptr<ObjectCacheEntry> GetObject(const string &key) {
		const lock_guard<mutex> lock(lock_mutex);
		auto non_evictable_it = non_evictable_entries.find(key);
		if (non_evictable_it != non_evictable_entries.end()) {
			return non_evictable_it->second;
		}
		return lru_cache.Get(key);
	}

	template <class T>
	shared_ptr<T> Get(const string &key) {
		shared_ptr<ObjectCacheEntry> object = GetObject(key);
		if (!object || object->GetObjectType() != T::ObjectType()) {
			return nullptr;
		}
		return shared_ptr_cast<ObjectCacheEntry, T>(object);
	}

	template <class T, class... ARGS>
	shared_ptr<T> GetOrCreate(const string &key, ARGS &&... args) {
		const lock_guard<mutex> lock(lock_mutex);

		// Check non-evictable entries first
		auto non_evictable_it = non_evictable_entries.find(key);
		if (non_evictable_it != non_evictable_entries.end()) {
			auto &existing = non_evictable_it->second;
			if (existing->GetObjectType() != T::ObjectType()) {
				return nullptr;
			}
			return shared_ptr_cast<ObjectCacheEntry, T>(existing);
		}

		// Check evictable cache
		auto existing = lru_cache.Get(key);
		if (existing) {
			if (existing->GetObjectType() != T::ObjectType()) {
				return nullptr;
			}
			return shared_ptr_cast<ObjectCacheEntry, T>(existing);
		}

		// Create new entry while holding lock
		auto value = make_shared_ptr<T>(std::forward<ARGS>(args)...);
		const auto estimated_memory = value->GetEstimatedCacheMemory();
		const bool is_evictable = estimated_memory.IsValid();
		if (!is_evictable) {
			non_evictable_entries[key] = value;
			return value;
		}

		auto reservation =
		    make_uniq<TempBufferPoolReservation>(MemoryTag::OBJECT_CACHE, buffer_pool, estimated_memory.GetIndex());
		lru_cache.Put(key, value, std::move(reservation));
		return value;
	}

	void Put(string key, shared_ptr<ObjectCacheEntry> value) {
		if (!value) {
			return;
		}

		const lock_guard<mutex> lock(lock_mutex);
		const auto estimated_memory = value->GetEstimatedCacheMemory();
		const bool is_evictable = estimated_memory.IsValid();
		if (!is_evictable) {
			non_evictable_entries[std::move(key)] = std::move(value);
			return;
		}

		auto reservation =
		    make_uniq<TempBufferPoolReservation>(MemoryTag::OBJECT_CACHE, buffer_pool, estimated_memory.GetIndex());
		lru_cache.Put(std::move(key), std::move(value), std::move(reservation));
	}

	void Delete(const string &key) {
		const lock_guard<mutex> lock(lock_mutex);
		auto iter = non_evictable_entries.find(key);
		if (iter != non_evictable_entries.end()) {
			non_evictable_entries.erase(iter);
			return;
		}
		lru_cache.Delete(key);
	}

	//! Type-prefixed variants of the methods above. These namespace the caller-provided key with the entry's
	//! ObjectType so that callers can pass a natural key (e.g. a file path) without having to build a unique
	//! cache key themselves.
	template <class T>
	shared_ptr<T> GetWithTypePrefix(const string &key) {
		return Get<T>(MakeCacheKey<T>(key));
	}

	template <class T, class... ARGS>
	shared_ptr<T> GetOrCreateWithTypePrefix(const string &key, ARGS &&... args) {
		return GetOrCreate<T>(MakeCacheKey<T>(key), std::forward<ARGS>(args)...);
	}

	template <class T>
	void PutWithTypePrefix(const string &key,
	                       shared_ptr<ObjectCacheEntry> value) { // NOLINT(performance-unnecessary-value-param)
		Put(MakeCacheKey<T>(key), std::move(value));
	}

	template <class T>
	void DeleteWithTypePrefix(const string &key) {
		Delete(MakeCacheKey<T>(key));
	}

	DUCKDB_API static ObjectCache &GetObjectCache(ClientContext &context);

	idx_t GetMaxMemory() const {
		const lock_guard<mutex> lock(lock_mutex);
		return lru_cache.Capacity();
	}
	idx_t GetCurrentMemory() const {
		const lock_guard<mutex> lock(lock_mutex);
		return lru_cache.CurrentTotalWeight();
	}
	size_t GetEntryCount() const {
		const lock_guard<mutex> lock(lock_mutex);
		return lru_cache.Size() + non_evictable_entries.size();
	}
	bool IsEmpty() const {
		const lock_guard<mutex> lock(lock_mutex);
		return lru_cache.IsEmpty() && non_evictable_entries.empty();
	}

	idx_t EvictToReduceMemory(idx_t target_bytes) {
		const lock_guard<mutex> lock(lock_mutex);
		return lru_cache.EvictToReduceAtLeast(target_bytes);
	}

	//! Evict LRU entries until the buffer pool fits memory_limit.
	//! Return false if evictable entries run out before reaching the limit.
	bool EvictToMemoryLimit(idx_t memory_limit);

private:
	//! Build the internal cache key for a typed entry by namespacing the caller-provided key with the entry's
	//! ObjectType.
	template <class T>
	static string MakeCacheKey(const string &key) {
		return StringUtil::Format("%s-%s", T::ObjectType(), key);
	}

private:
	mutable mutex lock_mutex;
	//! LRU cache for evictable entries

	SharedLruCache<string, ObjectCacheEntry, duckdb::BufferPoolPayload> lru_cache;
	//! Separate storage for non-evictable entries (i.e., encryption keys)
	unordered_map<string, shared_ptr<ObjectCacheEntry>> non_evictable_entries;
	//! Used to create buffer pool reservation on entries creation.
	BufferPool &buffer_pool;
};

} // namespace duckdb
