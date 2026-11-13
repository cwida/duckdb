//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/perfect_hash_join_table.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/allocator.hpp"
#include "duckdb/common/array_ptr.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/unique_ptr.hpp"

namespace duckdb {

class TupleDataLayout;

//! Direct integer lookup for JoinHashTable.
class PerfectHashJoinTable {
public:
	enum class Type { BITMAP, ROWS, DICTIONARY };

	PerfectHashJoinTable(Type type, bool nulls_equal, bool track_matches)
	    : nulls_equal(nulls_equal), track_matches(track_matches), type(type) {
		D_ASSERT(type != Type::DICTIONARY);
		D_ASSERT(!track_matches || type == Type::BITMAP);
	}
	virtual ~PerfectHashJoinTable();

	static unique_ptr<PerfectHashJoinTable> Create(PhysicalType key_type, Type type, bool nulls_equal,
	                                               bool track_matches);

	//! Accumulate bounds before allocating the directory, independently of runtime filters.
	virtual void UpdateBounds(const Vector &keys) = 0;
	virtual void MergeBounds(const PerfectHashJoinTable &other) = 0;
	virtual bool Initialize(Allocator &allocator, idx_t build_count, idx_t pointer_table_size) = 0;
	//! Returns whether duplicate keys produced a chain.
	virtual bool Insert(const Vector &rows, const TupleDataLayout &layout, idx_t pointer_offset, bool parallel) = 0;
	virtual void GetRowPointers(const Vector &keys, optional_ptr<const SelectionVector> sel, idx_t &count,
	                            Vector &pointers, SelectionVector &matches) const = 0;
	virtual idx_t ProbeMembership(const Vector &keys, unsafe_unique_array<bool> &found_match) const = 0;
	virtual idx_t ProbeDictionary(const Vector &keys, SelectionVector &probe_sel, SelectionVector &build_sel) const = 0;
	//! Select retained rows by their equality group's match state.
	virtual idx_t SelectMatches(array_ptr<const data_ptr_t> rows, const TupleDataLayout &layout, idx_t &offset,
	                            array_ptr<data_ptr_t> result, bool matched) const = 0;

	void InitializeEntries(idx_t from, idx_t to);
	void ConvertToDictionary(idx_t pointer_offset);
	idx_t EntryCount() const {
		return entries.GetSize() / sizeof(uint64_t);
	}
	idx_t SizeInBytes() const {
		return entries.GetSize() + match_entries.GetSize();
	}
	Type GetType() const {
		return type;
	}
	bool TracksMatches() const {
		return track_matches;
	}

protected:
	bool IsBitmap() const {
		return type == Type::BITMAP;
	}
	array_ptr<uint64_t> GetEntries() {
		return {reinterpret_cast<uint64_t *>(entries.get()), EntryCount()};
	}
	array_ptr<const uint64_t> GetEntries() const {
		return {reinterpret_cast<const uint64_t *>(entries.get()), EntryCount()};
	}
	array_ptr<atomic<uint64_t>> GetAtomicEntries() {
		return {reinterpret_cast<atomic<uint64_t> *>(entries.get()), EntryCount()};
	}
	array_ptr<atomic<uint64_t>> GetMatchEntries() const {
		D_ASSERT(track_matches);
		return {reinterpret_cast<atomic<uint64_t> *>(match_entries.get()), EntryCount()};
	}

	static constexpr idx_t MAX_BYTES = 8 * 1024 * 1024;
	AllocatedData entries;
	mutable AllocatedData match_entries;
	const bool nulls_equal;
	const bool track_matches;

private:
	Type type;
};

} // namespace duckdb
