//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/fixed_size_map.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/bit_utils.hpp"
#include "duckdb/common/perfect_map_set.hpp"
#include "duckdb/common/types/validity_mask.hpp"

namespace duckdb {

template <class T, bool is_const>
class fixed_size_map_iterator; // NOLINT: match stl case

//! Alternative to perfect_map_t when min/max keys are integral, small, and known
template <class T>
class fixed_size_map_t { // NOLINT: match stl case
	friend class fixed_size_map_iterator<T, false>;
	friend class fixed_size_map_iterator<T, true>;

public:
	using key_type = idx_t;
	using mapped_type = T;
	using occupied_mask = TemplatedValidityMask<uint8_t>;
	using iterator = fixed_size_map_iterator<mapped_type, false>;
	using const_iterator = fixed_size_map_iterator<mapped_type, true>;

public:
	explicit fixed_size_map_t(idx_t capacity_p = 0) : capacity(capacity_p) {
		resize(capacity);
	}

	idx_t size() const { // NOLINT: match stl case
		return count;
	}

	idx_t GetCapacity() const {
		return capacity;
	}

	void resize(idx_t capacity_p) { // NOLINT: match stl case
		capacity = capacity_p;
		occupied = occupied_mask(capacity);
		values = make_unsafe_uniq_array_uninitialized<mapped_type>(capacity + 1);
		clear();
	}

	void clear() { // NOLINT: match stl case
		count = 0;
		min_occupied_key = capacity;
		max_occupied_key = 0;
		occupied.SetAllInvalid(capacity);
	}

	mapped_type &operator[](const key_type &key) {
		D_ASSERT(key < capacity);
		if (!occupied.RowIsValidUnsafe(key)) {
			count++;
			min_occupied_key = MinValue(min_occupied_key, key);
			max_occupied_key = MaxValue(max_occupied_key, key);
			occupied.SetValidUnsafe(key);
		}
		return values[key];
	}

	mapped_type &GetOrInsert(const key_type &key, bool &inserted) {
		D_ASSERT(key < capacity);
		inserted = !occupied.RowIsValidUnsafe(key);
		if (inserted) {
			count++;
			min_occupied_key = MinValue(min_occupied_key, key);
			max_occupied_key = MaxValue(max_occupied_key, key);
			occupied.SetValidUnsafe(key);
		}
		return values[key];
	}

	const mapped_type &operator[](const key_type &key) const {
		D_ASSERT(key < capacity);
		return values[key];
	}

	iterator begin() { // NOLINT: match stl case
		if (count == 0) {
			return end();
		}
		return iterator(*this, min_occupied_key);
	}

	const_iterator begin() const { // NOLINT: match stl case
		if (count == 0) {
			return end();
		}
		return const_iterator(*this, min_occupied_key);
	}

	iterator end() { // NOLINT: match stl case
		return iterator(*this, capacity);
	}

	const_iterator end() const { // NOLINT: match stl case
		return const_iterator(*this, capacity);
	}

	iterator find(const key_type &index) { // NOLINT: match stl case
		return occupied.RowIsValidUnsafe(index) ? iterator(*this, index) : end();
	}

	const_iterator find(const key_type &index) const { // NOLINT: match stl case
		return occupied.RowIsValidUnsafe(index) ? const_iterator(*this, index) : end();
	}

private:
	idx_t capacity;
	idx_t count;
	idx_t min_occupied_key;
	idx_t max_occupied_key;

	occupied_mask occupied;
	unsafe_unique_array<mapped_type> values;
};

template <class T, bool is_const>
class fixed_size_map_iterator { // NOLINT: match stl case
public:
	using key_type = idx_t;
	using mapped_type = T;
	using fixed_size_map_type = fixed_size_map_t<mapped_type>;
	using map_type = typename std::conditional<is_const, const fixed_size_map_type, fixed_size_map_type>::type;
	using occupied_mask = typename fixed_size_map_t<mapped_type>::occupied_mask;

public:
	fixed_size_map_iterator(map_type &map_p, key_type index) : map(map_p) {
		occupied_mask::GetEntryIndex(index, entry_idx, idx_in_entry);
	}

	fixed_size_map_iterator &operator++() {
		// Prefix increment
		if (++idx_in_entry == occupied_mask::BITS_PER_VALUE) {
			NextEntry();
		}
		MoveToNextOccupied();
		return *this;
	}

	fixed_size_map_iterator operator++(int) {
		fixed_size_map_iterator tmp = *this;
		++(*this);
		return tmp;
	}

	key_type GetKey() const {
		return entry_idx * occupied_mask::BITS_PER_VALUE + idx_in_entry;
	}

	mapped_type &GetValue() {
		return map.values[GetKey()];
	}

	const mapped_type &GetValue() const {
		return map.values[GetKey()];
	}

	friend bool operator==(const fixed_size_map_iterator &a, const fixed_size_map_iterator &b) {
		return a.entry_idx == b.entry_idx && a.idx_in_entry == b.idx_in_entry;
	}

	friend bool operator!=(const fixed_size_map_iterator &a, const fixed_size_map_iterator &b) {
		return !(a == b);
	}

	friend bool operator<(const fixed_size_map_iterator &a, const fixed_size_map_iterator &b) {
		if (a.entry_idx < b.entry_idx) {
			return true;
		}
		if (a.entry_idx == b.entry_idx) {
			return a.idx_in_entry < b.idx_in_entry;
		}
		return false;
	}

private:
	void NextEntry() {
		entry_idx++;
		idx_in_entry = 0;
	}

	void MoveToNextOccupied() {
		const auto iterator_end = map.capacity;
		const auto scan_end = map.count == 0 ? iterator_end : map.max_occupied_key + 1;
		idx_t end_entry_idx;
		idx_t end_idx_in_entry;
		occupied_mask::GetEntryIndex(scan_end, end_entry_idx, end_idx_in_entry);
		while (entry_idx < end_entry_idx || (entry_idx == end_entry_idx && idx_in_entry < end_idx_in_entry)) {
			const auto entry_end = entry_idx == end_entry_idx ? end_idx_in_entry : occupied_mask::BITS_PER_VALUE;
			const auto entry = map.occupied.GetValidityEntryUnsafe(entry_idx);
			const auto valid_until_end = static_cast<uint8_t>(entry & occupied_mask::EntryWithValidBits(entry_end));
			const auto remaining =
			    static_cast<uint8_t>(valid_until_end & ~occupied_mask::EntryWithValidBits(idx_in_entry));
			if (remaining) {
				idx_in_entry = CountZeros<uint64_t>::Trailing(remaining);
				return;
			}
			NextEntry();
		}
		occupied_mask::GetEntryIndex(iterator_end, entry_idx, idx_in_entry);
	}

private:
	map_type &map;
	idx_t entry_idx;
	idx_t idx_in_entry;
};

//! A helper functor so we can template functions to use either a perfect map or a fixed size map

// LCOV_EXCL_START
template <class T, bool fixed>
struct TemplatedMapGetter {
private:
	using key_type = idx_t;
	using mapped_type = T;
	using fixed_size_map_type = fixed_size_map_t<mapped_type>;
	using perfect_map_type = perfect_map_t<mapped_type>;
	using map_type = typename std::conditional<fixed, fixed_size_map_type, perfect_map_type>::type;
	using iterator = typename map_type::iterator;
	using const_iterator = typename map_type::const_iterator;

public:
	static key_type GetKey(const iterator &it) {
		return GetKeyInternal(it);
	}

	static key_type GetKey(const const_iterator &it) {
		return GetKeyInternal(it);
	}

	static mapped_type &GetValue(iterator &it) {
		return GetValueInternal(it);
	}

	static const mapped_type &GetValue(const const_iterator &it) {
		return GetValueInternal(it);
	}

private:
	// Down here we overload instead of templating to circumvent this error:
	// "Explicit specialization of struct 'Functor<fixed>' in non-namespace scope"
	// Alternatively, we could define these outside of TemplatedMapGetter
	// However, then we no longer have access to the MAPPED_TYPE template and the code becomes unreadable
	static key_type GetKeyInternal(const typename perfect_map_type::iterator &it) {
		return it->first;
	}

	static key_type GetKeyInternal(const typename perfect_map_type::const_iterator &it) {
		return it->first;
	}

	static mapped_type &GetValueInternal(typename perfect_map_type::iterator &it) {
		return it->second;
	}

	static const mapped_type &GetValueInternal(const typename perfect_map_type::const_iterator &it) {
		return it->second;
	}

	static key_type GetKeyInternal(const typename fixed_size_map_type::iterator &it) {
		return it.GetKey();
	}

	static key_type GetKeyInternal(const typename fixed_size_map_type::const_iterator &it) {
		return it.GetKey();
	}

	static mapped_type &GetValueInternal(typename fixed_size_map_type::iterator &it) {
		return it.GetValue();
	}

	static const mapped_type &GetValueInternal(const typename fixed_size_map_type::const_iterator &it) {
		return it.GetValue();
	}
};
// LCOV_EXCL_STOP

} // namespace duckdb
