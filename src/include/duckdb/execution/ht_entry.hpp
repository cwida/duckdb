//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/ht_entry.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/assert.hpp"
#include "duckdb/common/typedefs.hpp"

namespace duckdb {

#if !defined(DUCKDB_DISABLE_POINTER_SALT) && defined(__ANDROID__)
// Google, why does Android need 18446744 TB of address space?
#define DUCKDB_DISABLE_POINTER_SALT
#endif

//! The ht_entry_t struct represents an individual entry within a hash table.
/*!
    This struct is used by the JoinHashTable and AggregateHashTable to store entries within the hash table. It stores
    a pointer to the data and a salt value in a single hash_t and can return or modify the pointer and salt
    individually.
*/
struct ht_entry_t { // NOLINT
public:
#ifdef DUCKDB_DISABLE_POINTER_SALT
	//! No salt, all pointer
	static constexpr const hash_t SALT_MASK = 0x0000000000000000;
	static constexpr const hash_t POINTER_MASK = 0xFFFFFFFFFFFFFFFF;
#else
	//! Upper 16 bits are salt, lower 48 bits are the pointer
	static constexpr const hash_t SALT_MASK = 0xFFFF000000000000;
	static constexpr const hash_t POINTER_MASK = 0x0000FFFFFFFFFFFF;
#endif

	ht_entry_t() noexcept : value(0) {
	}

	explicit ht_entry_t(hash_t value_p) noexcept : value(value_p) {
	}

	ht_entry_t(const hash_t &salt, const data_ptr_t &pointer)
	    : value(cast_pointer_to_uint64(pointer) | (salt & SALT_MASK)) {
	}

	inline bool IsOccupied() const {
		return value != 0;
	}

	//! Returns a pointer based on the stored value (asserts if the cell is occupied)
	inline data_ptr_t GetPointer() const {
		D_ASSERT(IsOccupied());
		return GetPointerOrNull();
	}

	//! Returns a pointer based on the stored value
	inline data_ptr_t GetPointerOrNull() const {
		return cast_uint64_to_pointer(value & POINTER_MASK);
	}

	inline void SetPointer(const data_ptr_t &pointer) {
		// Pointer shouldn't use upper bits
		D_ASSERT((cast_pointer_to_uint64(pointer) & SALT_MASK) == 0);
		// Value should have all 1's in the pointer area
		D_ASSERT((value & POINTER_MASK) == POINTER_MASK);
		// Set upper bits to 1 in pointer so the salt stays intact
		value &= cast_pointer_to_uint64(pointer) | SALT_MASK;
	}

	// Returns the salt, leaves upper salt bits intact, sets lower bits to all 1's
	static inline hash_t ExtractSalt(const hash_t &hash) {
		return hash | POINTER_MASK;
	}

	inline hash_t GetSalt() const {
		return ExtractSalt(value);
	}

	inline hash_t GetSaltWithNulls() const {
		return value & SALT_MASK;
	}

	inline void SetSalt(const hash_t &salt) {
		// Shouldn't be occupied when we set this
		D_ASSERT(!IsOccupied());
		// Salt should have all 1's in the pointer field
		D_ASSERT((salt & POINTER_MASK) == POINTER_MASK);
		// No need to mask, just put the whole thing there
		value = salt;
	}

private:
	hash_t value;
};

//! Prefetch a future bucket, evaluating its address only within the loop bounds.
template <class GET_ENTRY>
inline void PrefetchHashTableEntry(idx_t row_idx, idx_t row_count, GET_ENTRY get_entry) {
#if defined(__GNUC__) || defined(__clang__)
	constexpr idx_t PREFETCH_DISTANCE = 64;
	if (row_idx + PREFETCH_DISTANCE < row_count) {
		__builtin_prefetch(get_entry(row_idx + PREFETCH_DISTANCE), 0, 3);
	}
#endif
}

// uses an AND operation to apply the modulo operation instead of an if condition that could be branch mispredicted
inline void IncrementAndWrap(idx_t &offset, const uint64_t &capacity_mask) {
	++offset &= capacity_mask;
}

} // namespace duckdb
