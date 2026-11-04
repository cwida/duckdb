//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/column/column_data_collection_segment.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/types/column/column_data_allocator.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"

namespace duckdb {

struct VectorChildIndex {
	explicit VectorChildIndex(idx_t index = DConstants::INVALID_INDEX) : index(index) {
	}

	idx_t index;

	bool IsValid() const {
		return index != DConstants::INVALID_INDEX;
	}
};

struct VectorDataIndex {
	explicit VectorDataIndex(idx_t index = DConstants::INVALID_INDEX) : index(index) {
	}

	idx_t index;

	bool IsValid() const {
		return index != DConstants::INVALID_INDEX;
	}
};

//! The representation class of a vector data entry
enum class ColumnDataRepresentation : uint8_t {
	//! Row values stored in block data (the default)
	FLAT,
	//! A single value plus a row count
	CONSTANT,
	//! Per-row dictionary codes in block data referencing a per-segment dictionary
	DICTIONARY
};

//! The value of a CONSTANT entry
struct ColumnDataConstantValue {
	//! The value bytes (fixed-size values and string_t entries are stored inline)
	uint8_t data[16] = {};
	//! Whether the constant is non-NULL
	bool is_valid = false;
	//! Entry holding the bytes of a non-inlined string (buffer-managed collections only)
	VectorDataIndex heap_index;
};

//! A dictionary materialized once per segment, shared by all DICTIONARY entries with the same id
struct SegmentDictionaryData {
	//! The id of the source dictionary
	string id;
	//! The type of the dictionary values
	LogicalType type;
	//! The entry holding the materialized dictionary values
	VectorDataIndex data_index;
	//! The blocks holding the dictionary values and string bytes
	unordered_set<uint32_t> block_ids;
};

struct SwizzleMetaData {
	SwizzleMetaData(VectorDataIndex child_index_p, data_ptr_t ptr_p, uint16_t offset_p, uint16_t count_p)
	    : child_index(child_index_p), ptr(ptr_p), offset(offset_p), count(count_p) {
	}
	SwizzleMetaData(const SwizzleMetaData &other) noexcept
	    : SwizzleMetaData(other.child_index, other.ptr.load(std::memory_order_relaxed), other.offset, other.count) {
	}
	//! Index of block storing heap
	VectorDataIndex child_index;
	//! Last-known pointer
	atomic<data_ptr_t> ptr;
	//! Offset into the string_t vector
	uint16_t offset;
	//! Number of strings starting at 'offset' that have strings stored in the block with index 'child_index'
	uint16_t count;
};

struct VectorMetaData {
	//! Where the vector data lives
	uint32_t block_id = 0;
	uint32_t offset = 0;
	//! The number of entries present in this vector
	uint16_t count;
	//! Whether block space has been allocated for this entry
	bool is_allocated = false;
	//! The representation class of this entry
	ColumnDataRepresentation representation = ColumnDataRepresentation::FLAT;
	//! The constant value (only used for CONSTANT entries)
	ColumnDataConstantValue constant;
	//! The dictionary slot in the segment (only used for DICTIONARY entries)
	idx_t dictionary_slot = DConstants::INVALID_INDEX;
	//! Meta data about string pointers
	vector<SwizzleMetaData> swizzle_data;

	//! Child data of this vector (used only for lists and structs)
	//! Note: child indices are stored with one layer of indirection
	//! The child_index here refers to the `child_indices` array in the ColumnDataCollectionSegment
	//! The entry in the child_indices array then refers to the actual `VectorMetaData` index
	//! In case of structs, the child_index refers to the FIRST child in the `child_indices` array
	//! Subsequent children are stored consecutively, i.e.
	//! first child: segment.child_indices[child_index + 0]
	//! nth child  : segment.child_indices[child_index + (n - 1)]
	VectorChildIndex child_index;
	//! Next vector entry (in case there is more data - used only in case of children of lists)
	VectorDataIndex next_data;
};

struct ChunkMetaData {
	//! The set of vectors of the chunk
	vector<VectorDataIndex> vector_data;
	//! Allocated bytes per column, including its child vectors and buffer-managed string heaps
	vector<idx_t> allocation_sizes;
	//! The block ids referenced by the chunk
	unordered_set<uint32_t> block_ids;
	//! The number of entries in the chunk
	uint16_t count;
	//! Total allocation size for this chunk
	optional_idx allocation_size;
};

class ColumnDataCollectionSegment {
public:
	ColumnDataCollectionSegment(shared_ptr<ColumnDataAllocator> allocator, vector<LogicalType> types_p);

public:
	//! The allocator and allocation statistics
	shared_ptr<ColumnDataAllocator> allocator;
	idx_t total_allocated;
	idx_t last_chunk_total_allocated;
	//! Metadata-resident bytes of entries appended as CONSTANT, counted in SizeInBytes and AllocationSize so
	//! that collections holding only compact entries report the bytes that actually store their values.
	//! Charges stay after a materialization, like superseded block regions: sizes never shrink while appending
	idx_t constant_metadata_size;

	//! The types of the chunks
	vector<LogicalType> types;
	//! The number of entries in the internal column data
	idx_t count;
	//! Set of chunk meta data
	vector<ChunkMetaData> chunk_data;
	//! Set of vector meta data
	vector<VectorMetaData> vector_data;
	//! The set of child indices
	vector<VectorDataIndex> child_indices;
	//! Per-segment dictionaries for DICTIONARY entries, deduplicated by dictionary id
	vector<SegmentDictionaryData> dictionaries;
	//! Map from dictionary id to a slot in "dictionaries"
	unordered_map<string, idx_t> dictionary_map;
	//! The string heap for the column data collection (only used for IN_MEMORY_ALLOCATOR)
	shared_ptr<StringHeap> heap;

public:
	//! Reset this segment for reuse, clearing all metadata while keeping allocated buffers
	void Reset();
	void AllocateNewChunk();
	//! Allocate space for a vector of a specific type in the segment
	VectorDataIndex AllocateVector(const LogicalType &type, ChunkMetaData &chunk_data,
	                               ChunkManagementState *chunk_state = nullptr,
	                               VectorDataIndex prev_index = VectorDataIndex(), bool defer_allocation = false);
	//! Allocate space for a vector during append
	VectorDataIndex AllocateVector(const LogicalType &type, ChunkMetaData &chunk_data,
	                               ColumnDataAppendState &append_state, VectorDataIndex prev_index = VectorDataIndex());
	//! Allocate space for string data during append (BUFFER_MANAGER_ALLOCATOR only)
	VectorDataIndex AllocateStringHeap(idx_t size, ChunkMetaData &chunk_meta, ColumnDataAppendState &append_state,
	                                   VectorDataIndex prev_index = VectorDataIndex());
	//! Allocate block storage for an entry whose allocation was deferred or whose storage is replaced
	void AllocateVectorStorage(VectorDataIndex index, idx_t allocation_size, ChunkMetaData &chunk_meta,
	                           ChunkManagementState *chunk_state);
	//! Fix string pointers of a single entry that point into relocated heap blocks
	void UnswizzleEntryPointers(ChunkManagementState &state, VectorMetaData &vdata, Vector &result);
	//! Read the materialized dictionary values of the given slot as a zero-copy vector
	Vector ReadSegmentDictionary(ChunkManagementState &state, idx_t slot);
	//! Load the value of a CONSTANT entry, resolving heap-resident string bytes
	string_t GetConstantString(ChunkManagementState &state, const VectorMetaData &vdata);

	void InitializeChunkState(idx_t chunk_index, ChunkManagementState &state);
	void ReadChunk(idx_t chunk_index, ChunkManagementState &state, DataChunk &chunk,
	               const vector<column_t> &column_ids);

	idx_t ReadVector(ChunkManagementState &state, VectorDataIndex vector_index, Vector &result);

	VectorDataIndex GetChildIndex(VectorChildIndex index, idx_t child_entry = 0);
	VectorChildIndex AddChildIndex(VectorDataIndex index);
	VectorChildIndex ReserveChildren(idx_t child_count);
	void SetChildIndex(VectorChildIndex base_idx, idx_t child_number, VectorDataIndex index);

	VectorMetaData &GetVectorData(VectorDataIndex index) {
		D_ASSERT(index.index < vector_data.size());
		return vector_data[index.index];
	}

	idx_t ChunkCount() const;
	//! Get the total *used* size (not cached)
	idx_t SizeInBytes() const;
	//! Get the currently allocated size in bytes (cached)
	idx_t AllocationSize() const;
	//! Get the allocation size of the chunk
	idx_t GetChunkAllocationSize(idx_t chunk_index) const;
	//! Get the allocation size of selected columns of the chunk
	idx_t GetChunkAllocationSize(idx_t chunk_index, const vector<column_t> &column_ids) const;

	void FetchChunk(idx_t chunk_idx, DataChunk &result);
	void FetchChunk(idx_t chunk_idx, DataChunk &result, const vector<column_t> &column_ids);

	void Verify();

	static idx_t GetDataSize(idx_t type_size);
	static validity_t *GetValidityPointerForWriting(data_ptr_t base_ptr, idx_t type_size);
	static validity_t *GetValidityPointer(data_ptr_t base_ptr, idx_t type_size, idx_t count);

private:
	idx_t ReadVectorInternal(ChunkManagementState &state, VectorDataIndex vector_index, Vector &result);
	idx_t ReadCompactVector(ChunkManagementState &state, VectorDataIndex vector_index, Vector &result);
	VectorDataIndex AllocateVectorInternal(const LogicalType &type, ChunkMetaData &chunk_meta,
	                                       ChunkManagementState *chunk_state, bool defer_allocation);
};

} // namespace duckdb
