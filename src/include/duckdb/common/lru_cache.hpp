//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/lru_cache.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include <functional>

#include "duckdb/common/common.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/list.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/unordered_map.hpp"

namespace duckdb {

struct DefaultPayload {
	idx_t GetWeight() const {
		return 1;
	}
};

// A LRU cache implementation, whose value could be accessed in a shared manner with shared pointer.
// Notice, it's not thread-safe.
template <typename Key, typename Val, typename Payload = DefaultPayload, typename KeyHash = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>>
class SharedLruCache {
public:
	using key_type = Key;
	using mapped_type = shared_ptr<Val>;
	using hasher = KeyHash;
	using key_equal = KeyEqual;

	// @param max_total_weight_p: Maximum total weight (in relevant unit) of entries. 0 means unlimited.
	explicit SharedLruCache(idx_t max_total_weight_p) : max_total_weight(max_total_weight_p), current_total_weight(0) {
	}

	// Disable copy and move
	SharedLruCache(const SharedLruCache &) = delete;
	SharedLruCache &operator=(const SharedLruCache &) = delete;

	~SharedLruCache() = default;

	// Insert `value` with key `key` and explicit memory size. This will replace any previous entry with the same key.
	template <typename... Types>
	void Put(Key key, shared_ptr<Val> value, Types... constructor_args) {
		// Remove existing entry if present
		auto existing_it = entry_map.find(key);
		if (existing_it != entry_map.end()) {
			DeleteImpl(existing_it);
		}

		auto payload = make_uniq<Payload>(std::forward<Types>(constructor_args)...);
		auto payload_weight = payload->GetWeight();

		// Evict entries if needed to make room
		if (max_total_weight > 0 && payload_weight > 0) {
			EvictIfNeeded(payload_weight);
		}

		// Add new entry
		lru_list.emplace_front(std::move(key));
		Entry new_entry;
		new_entry.value = std::move(value);
		new_entry.lru_iterator = lru_list.begin();
		new_entry.payload = std::move(payload);
		new_entry.payload_weight = payload_weight;

		entry_map.emplace(std::cref(lru_list.front()), std::move(new_entry));
		current_total_weight += payload_weight;
	}

	// Delete the entry with key `key`.
	// Return whether the requested `key` is found in the cache.
	bool Delete(const Key &key) {
		auto it = entry_map.find(key);
		if (it == entry_map.end()) {
			return false;
		}
		DeleteImpl(it);
		return true;
	}

	// Look up the entry with key `key`. Return nullptr if not found.
	shared_ptr<Val> Get(const Key &key) {
		auto entry_map_iter = entry_map.find(key);
		if (entry_map_iter == entry_map.end()) {
			return nullptr;
		}

		// Move to front, which indicates most recently used.
		lru_list.splice(lru_list.begin(), lru_list, entry_map_iter->second.lru_iterator);
		return entry_map_iter->second.value;
	}

	// Clear the whole cache.
	void Clear() {
		entry_map.clear();
		lru_list.clear();
		current_total_weight = 0;
	}

	//! Evict the least recently used entry. Return false if the cache is empty.
	bool EvictLeastRecentlyUsed() {
		if (lru_list.empty()) {
			return false;
		}
		auto stale_it = entry_map.find(lru_list.back());
		D_ASSERT(stale_it != entry_map.end());
		DeleteImpl(stale_it);
		return true;
	}

	//! Evict entries until at least target_weight is removed or the cache is empty. Return the removed weight.
	idx_t EvictToReduceAtLeast(idx_t target_weight) {
		const auto initial_weight = current_total_weight;
		while (initial_weight - current_total_weight < target_weight) {
			if (!EvictLeastRecentlyUsed()) {
				break;
			}
		}
		return initial_weight - current_total_weight;
	}

	idx_t Capacity() const {
		return max_total_weight;
	}
	idx_t CurrentTotalWeight() const {
		return current_total_weight;
	}
	size_t Size() const {
		return entry_map.size();
	}
	bool IsEmpty() const {
		return entry_map.empty();
	}

private:
	struct Entry {
		shared_ptr<Val> value;
		idx_t memory;
		typename list<Key>::iterator lru_iterator;
		// Record payload weight, which is used for global weight control.
		unique_ptr<Payload> payload;
		idx_t payload_weight;
	};

	using KeyConstReference = std::reference_wrapper<const Key>;

	struct KeyReferenceHash {
		size_t operator()(KeyConstReference key) const {
			return KeyHash()(key.get());
		}
	};

	struct KeyReferenceEqual {
		bool operator()(KeyConstReference lhs, KeyConstReference rhs) const {
			return KeyEqual()(lhs.get(), rhs.get());
		}
	};

	using EntryMap = unordered_map<KeyConstReference, Entry, KeyReferenceHash, KeyReferenceEqual>;

	void DeleteImpl(typename EntryMap::iterator iter) {
		auto lru_iterator = iter->second.lru_iterator;
		current_total_weight -= iter->second.payload_weight;
		entry_map.erase(iter);
		lru_list.erase(lru_iterator);
	}

	void EvictIfNeeded(idx_t required_weight) {
		if (max_total_weight == 0) {
			return;
		}

		// Evict LRU entries until we have enough space
		while (current_total_weight + required_weight > max_total_weight) {
			if (!EvictLeastRecentlyUsed()) {
				break;
			}
		}
	}

	const idx_t max_total_weight;
	idx_t current_total_weight;
	EntryMap entry_map;
	list<Key> lru_list;
};

} // namespace duckdb
