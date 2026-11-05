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

//! The owning string map is equivalent to string_map_t, except that it manages ownership of all data using the
//! provided allocator
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
	explicit OwningStringMap(Allocator &allocator) : allocator(allocator), free_type(AllocatorFreeType::REQUIRES_FREE) {
	}
	explicit OwningStringMap(ArenaAllocator &allocator)
	    : allocator(allocator.GetAllocator()), free_type(AllocatorFreeType::DOES_NOT_REQUIRE_FREE) {
	}
	~OwningStringMap() {
		Destroy();
	}

	pair<iterator, bool> insert(value_type entry) { // NOLINT: match std style
		if (entry.first.IsInlined()) {
			return map.insert(std::move(entry));
		} else {
			return map.insert(make_pair(CopyString(entry.first), std::move(entry.second)));
		}
	}
	std::pair<iterator, bool> emplace(value_type &&entry) { // NOLINT: match std style
		auto it = find(entry.first);
		if (it != end()) {
			return std::make_pair(it, false);
		}
		return insert(std::move(entry));
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
		return GetOrCreate(k);
	}
	mapped_type &operator[](key_type &&k) {
		return GetOrCreate(std::move(k));
	}

private:
	inline string_t GetInsertionString(string_t input_str) {
		if (input_str.IsInlined()) {
			// inlined strings can be inserted directly
			return input_str;
		} else {
			return CopyString(input_str);
		}
	}

	inline string_t CopyString(string_t input_str) {
		D_ASSERT(!input_str.IsInlined());
		// if the string is not inlined we need to allocate space for it
		auto input_str_size = UnsafeNumericCast<uint32_t>(input_str.GetSize());
		auto string_memory = allocator.AllocateData(input_str_size);
		// copy over the string
		memcpy(string_memory, input_str.GetData(), input_str_size);
		// now return the final string_t
		return string_t(char_ptr_cast(string_memory), input_str_size);
	}

	mapped_type &GetOrCreate(key_type k) {
		auto entry = map.find(k);
		if (entry != map.end()) {
			return entry->second;
		}
		auto result = insert(make_pair(k, mapped_type()));
		return result.first->second;
	}

	void DestroyString(const string_t &str) const {
		if (free_type != AllocatorFreeType::REQUIRES_FREE) {
			return;
		}
		if (str.IsInlined()) {
			return;
		}
		allocator.FreeData(data_ptr_cast(str.GetDataWriteable()), str.GetSize());
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
	Allocator &allocator;
	STRING_MAP_TYPE map;
	AllocatorFreeType free_type;
};

template <class T>
using OrderedOwningStringMap = OwningStringMap<T, map<string_t, T>>;

} // namespace duckdb
