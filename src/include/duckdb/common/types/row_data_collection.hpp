//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/row_chunk.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/string_heap.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/storage/buffer_manager.hpp"

namespace duckdb {

struct RowDataBlock {
	RowDataBlock(BufferManager &buffer_manager, idx_t capacity, idx_t entry_size)
	    : capacity(capacity), entry_size(entry_size), count(0), byte_offset(0) {
		block = buffer_manager.RegisterMemory(capacity * entry_size, false);
	}
	//! The buffer block handle
	shared_ptr<BlockHandle> block;
	//! Capacity (number of entries) and entry size that fit in this block
	idx_t capacity;
	const idx_t entry_size;
	//! Number of entries currently in this block
	idx_t count;
	//! Write offset (if variable size entries)
	idx_t byte_offset;
};

struct BlockAppendEntry {
	BlockAppendEntry(data_ptr_t baseptr, idx_t count) : baseptr(baseptr), count(count) {
	}
	data_ptr_t baseptr;
	idx_t count;
};

class RowDataCollection {
public:
	RowDataCollection(BufferManager &buffer_manager, idx_t block_capacity, idx_t entry_size, bool keep_pinned = false);

	mutex rc_lock;

	//! BufferManager
	BufferManager &buffer_manager;
	//! The total number of stored entries
	idx_t count;
	//! The number of entries per block
	idx_t block_capacity;
	//! Size of entries in the blocks
	idx_t entry_size;
	//! The blocks holding the main data
	vector<RowDataBlock> blocks;
	//! The blocks that this collection currently has pinned
	vector<unique_ptr<BufferHandle>> pinned_blocks;

public:
	void SerializeVectorSortable(Vector &v, idx_t vcount, const SelectionVector &sel, idx_t ser_count,
	                             data_ptr_t key_locations[], bool desc, bool has_null, bool nulls_first,
	                             idx_t prefix_len, idx_t width, idx_t offset = 0);

	static void ComputeEntrySizes(Vector &v, VectorData &vdata, idx_t entry_sizes[], idx_t vcount, idx_t ser_count,
	                              const SelectionVector &sel, idx_t offset = 0);
	static void ComputeEntrySizes(Vector &v, idx_t entry_sizes[], idx_t vcount, idx_t ser_count,
	                              const SelectionVector &sel, idx_t offset = 0);
	static void ComputeEntrySizes(DataChunk &input, idx_t entry_sizes[], idx_t entry_size, const SelectionVector &sel,
	                              idx_t ser_count);

	static void SerializeVectorData(VectorData &vdata, PhysicalType type, const SelectionVector &sel, idx_t ser_count,
	                                idx_t col_idx, data_ptr_t key_locations[], data_ptr_t validitymask_locations[],
	                                idx_t offset = 0);
	static void SerializeVector(Vector &v, idx_t vcount, const SelectionVector &sel, idx_t ser_count, idx_t col_idx,
	                            data_ptr_t key_locations[], data_ptr_t validitymask_locations[], idx_t offset = 0);
	idx_t AppendToBlock(RowDataBlock &block, BufferHandle &handle, vector<BlockAppendEntry> &append_entries,
	                    idx_t remaining, idx_t entry_sizes[]);
	void Build(idx_t added_count, data_ptr_t key_locations[], idx_t entry_sizes[]);

	void Merge(RowDataCollection &other);

	static void DeserializeIntoVector(Vector &v, const idx_t &vcount, const SelectionVector &sel, const idx_t &col_idx,
	                                  data_ptr_t key_locations[], data_ptr_t validitymask_locations[]);

private:
	template <class T>
	void TemplatedSerializeVectorSortable(VectorData &vdata, const SelectionVector &sel, idx_t count,
	                                      data_ptr_t key_locations[], bool desc, bool has_null, bool invert,
	                                      const idx_t offset);
	void SerializeStringVectorSortable(VectorData &vdata, const SelectionVector &sel, idx_t add_count,
	                                   data_ptr_t key_locations[], const bool desc, const bool has_null,
	                                   const bool nulls_first, const idx_t prefix_len, const idx_t offset);
	void SerializeListVectorSortable(Vector &v, VectorData &vdata, const SelectionVector &sel, idx_t add_count,
	                                 data_ptr_t key_locations[], const bool desc, const bool has_null,
	                                 const bool nulls_first, const idx_t prefix_len, const idx_t width,
	                                 const idx_t offset);
	void SerializeStructVectorSortable(Vector &v, VectorData &vdata, idx_t vcount, const SelectionVector &sel,
	                                   idx_t add_count, data_ptr_t key_locations[], const bool desc,
	                                   const bool has_null, const bool nulls_first, const idx_t prefix_len, idx_t width,
	                                   const idx_t offset);

	static void ComputeStringEntrySizes(VectorData &col, idx_t entry_sizes[], const idx_t ser_count,
	                                    const SelectionVector &sel, const idx_t offset);
	static void ComputeStructEntrySizes(Vector &v, idx_t entry_sizes[], idx_t vcount, idx_t ser_count,
	                                    const SelectionVector &sel, idx_t offset);
	static void ComputeListEntrySizes(Vector &v, VectorData &vdata, idx_t entry_sizes[], idx_t ser_count,
	                                  const SelectionVector &sel, idx_t offset);

	static void SerializeStringVector(Vector &v, idx_t vcount, const SelectionVector &sel, idx_t ser_count,
	                                  idx_t col_idx, data_ptr_t key_locations[], data_ptr_t validitymask_locations[],
	                                  idx_t offset);
	static void SerializeStructVector(Vector &v, idx_t vcount, const SelectionVector &sel, idx_t ser_count,
	                                  idx_t col_idx, data_ptr_t key_locations[], data_ptr_t validitymask_locations[],
	                                  idx_t offset);
	static void SerializeListVector(Vector &v, idx_t vcount, const SelectionVector &sel, idx_t ser_count, idx_t col_idx,
	                                data_ptr_t key_locations[], data_ptr_t validitymask_locations[], idx_t offset);

	//! Whether the system is little endian
	const bool is_little_endian;
	//! Whether the blocks should stay pinned (necessary for e.g. a heap)
	bool keep_pinned;
};

} // namespace duckdb
