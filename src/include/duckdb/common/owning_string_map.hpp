//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/owning_string_map.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/string_map_set.hpp"
#include "duckdb/common/allocator.hpp"
#include "duckdb/common/pair.hpp"
#include "duckdb/common/map.hpp"
#include "duckdb/storage/arena_allocator.hpp"
#include <cstring>

namespace duckdb {

//! The owning string map is equivalent to string_map_t, except that it manages ownership of the keys.
//! Owning maps allocate and free keys through the allocator they are constructed with. Arena-backed maps retain no
//! allocator: inserting calls take the arena of the caller, so a map can be filled from several threads' arenas.
template <class VALUE_TYPE, class STRING_MAP_TYPE = string_map_t<VALUE_TYPE>>
class OwningStringMap {
public:
	using size_type = typename STRING_MAP_TYPE::size_type;
	using key_type = typename STRING_MAP_TYPE::key_type;
	using mapped_type = typename STRING_MAP_TYPE::mapped_type;
	using value_type = typename STRING_MAP_TYPE::value_type;
	using iterator = typename STRING_MAP_TYPE::iterator;
	using const_iterator = typename STRING_MAP_TYPE::const_iterator;

public:
	OwningStringMap() : free_type(AllocatorFreeType::DOES_NOT_REQUIRE_FREE) {
	}
	explicit OwningStringMap(Allocator &allocator)
	    : allocator(&allocator), free_type(AllocatorFreeType::REQUIRES_FREE) {
	}
	~OwningStringMap() {
		Destroy();
	}

	pair<iterator, bool> insert(value_type entry) { // NOLINT: match std style
		D_ASSERT(allocator);
		if (entry.first.IsInlined()) {
			return map.insert(std::move(entry));
		}
		auto key = CopyString(entry.first, allocator->AllocateData(entry.first.GetSize()));
		return map.insert(make_pair(key, std::move(entry.second)));
	}
	pair<iterator, bool> insert(value_type entry, ArenaAllocator &arena) { // NOLINT: match std style
		D_ASSERT(!allocator);
		if (entry.first.IsInlined()) {
			return map.insert(std::move(entry));
		}
		auto key = CopyString(entry.first, arena.Allocate(entry.first.GetSize()));
		return map.insert(make_pair(key, std::move(entry.second)));
	}
	std::pair<iterator, bool> emplace(value_type &&entry) { // NOLINT: match std style
		auto it = find(entry.first);
		if (it != end()) {
			return std::make_pair(it, false);
		}
		return insert(std::move(entry));
	}
	std::pair<iterator, bool> emplace(value_type &&entry, ArenaAllocator &arena) { // NOLINT: match std style
		auto it = find(entry.first);
		if (it != end()) {
			return std::make_pair(it, false);
		}
		return insert(std::move(entry), arena);
	}
	size_type erase(const key_type &k) { // NOLINT: match std style
		auto entry = map.find(k);
		if (entry == map.end()) {
			return 0;
		}
		string_t erase_string = entry->first;
		map.erase(entry);
		DestroyString(erase_string);
		return 1;
	}

	void clear() { // NOLINT: match std style
		Destroy();
	}

	size_type size() const noexcept { // NOLINT: match std style
		return map.size();
	}

	bool empty() const noexcept { // NOLINT: match std style
		return map.empty();
	}
	iterator begin() noexcept { // NOLINT: match std style
		return map.begin();
	}
	const_iterator begin() const noexcept { // NOLINT: match std style
		return map.begin();
	}
	iterator end() noexcept { // NOLINT: match std style
		return map.end();
	}
	const_iterator end() const noexcept { // NOLINT: match std style
		return map.end();
	}
	iterator find(const key_type &k) { // NOLINT: match std style
		return map.find(k);
	}
	const_iterator find(const key_type &k) const { // NOLINT: match std style
		return map.find(k);
	}

	mapped_type &operator[](const key_type &k) {
		auto entry = map.find(k);
		if (entry != map.end()) {
			return entry->second;
		}
		return insert(make_pair(k, mapped_type())).first->second;
	}
	mapped_type &GetOrCreate(const key_type &k, ArenaAllocator &arena) {
		auto entry = map.find(k);
		if (entry != map.end()) {
			return entry->second;
		}
		return insert(make_pair(k, mapped_type()), arena).first->second;
	}

private:
	static string_t CopyString(string_t input_str, data_ptr_t string_memory) {
		D_ASSERT(!input_str.IsInlined());
		auto input_str_size = UnsafeNumericCast<uint32_t>(input_str.GetSize());
		memcpy(string_memory, input_str.GetData(), input_str_size);
		return string_t(char_ptr_cast(string_memory), input_str_size);
	}

	void DestroyString(const string_t &str) {
		if (free_type != AllocatorFreeType::REQUIRES_FREE) {
			return;
		}
		if (str.IsInlined()) {
			return;
		}
		allocator->FreeData(data_ptr_cast(str.GetDataWriteable()), str.GetSize());
	}

	void Destroy() {
		if (free_type == AllocatorFreeType::REQUIRES_FREE) {
			for (auto &str : map) {
				DestroyString(str.first);
			}
		}
		map.clear();
	}

private:
	//! Only set for owning maps
	optional_ptr<Allocator> allocator;
	STRING_MAP_TYPE map;
	AllocatorFreeType free_type;
};

template <class T>
using OrderedOwningStringMap = OwningStringMap<T, map<string_t, T>>;

} // namespace duckdb
