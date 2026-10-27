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
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/storage/buffer/buffer_handle.hpp"

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
	DICTIONARY,
	//! A list child chain entry referencing a shared child buffer instead of block data
	SHARED_LIST_CHILD
};

//! Storage for the complete child data of large lists, shared between collections. The data lives in a
//! buffer-managed block, so it spills like any other column data block while no vector pins it.
//! Written data is immutable, but the storage may grow at the end: appending beyond every published list entry
//! leaves the prefixes other references cover unchanged, so grown lists share their prefix storage.
class ColumnDataSharedListChild {
public:
	ColumnDataSharedListChild(BufferManager &buffer_manager, shared_ptr<BlockHandle> block, idx_t capacity,
	                          idx_t type_size, idx_t count);

	//! Pin the storage for reading or writing
	BufferHandle Pin();
	//! The entries the storage can hold
	idx_t Capacity() const {
		return capacity;
	}
	idx_t TypeSize() const {
		return type_size;
	}
	//! The end of the claimed entries. A claim advances it before the claimant writes the claimed region, and
	//! only views created after those writes cover it, so every view reads written data
	idx_t GetClaimedCount() const {
		return claimed_count.load();
	}
	//! Claim 'extra' entries at the current end for an in-place append. At most one claimant succeeds for a
	//! given end position, so claimed regions never overlap
	bool TryClaimExtension(idx_t current_end, idx_t extra) {
		if (current_end + extra > capacity) {
			return false;
		}
		auto expected = current_end;
		return claimed_count.compare_exchange_strong(expected, current_end + extra);
	}
	//! The validity of the entries. Set once at creation, covering the whole capacity, and never written after
	//! the storage is shared (in-place appends only add valid entries)
	ValidityMask &GetValidity() {
		return validity;
	}
	//! Whether 'buffer_manager' backs this storage, so a collection using it may reference the storage
	bool IsBackedBy(const BufferManager &other) const {
		return &buffer_manager == &other;
	}
	//! The memory the storage occupies
	idx_t AllocationSize() const;
	//! A process-unique identity, so accounting never confuses storage that reuses an address
	idx_t Id() const {
		return id;
	}

private:
	BufferManager &buffer_manager;
	shared_ptr<BlockHandle> block;
	idx_t capacity;
	idx_t type_size;
	ValidityMask validity;
	atomic<idx_t> claimed_count;
	idx_t id;
};

//! A vector buffer viewing the first 'count' entries of shared list child storage, handed out by zero-copy
//! scans. Like every zero-copy scan result it aliases collection storage and must be treated as read-only.
//! The view pins the storage for its whole lifetime, so slices of it stay valid. The common mutable accessors
//! detach it into a private copy first, and sharing decisions trust a view only while it still verifiably
//! matches its storage (IsUnchangedView), so a modified vector is never mistaken for the shared prefix.
class ColumnDataListChildBuffer : public StandardVectorBuffer {
public:
	ColumnDataListChildBuffer(shared_ptr<ColumnDataSharedListChild> owner, BufferHandle pin, count_t count);

	//! The viewed storage, only meaningful while IsUnchangedView() holds
	const shared_ptr<ColumnDataSharedListChild> &GetOwner() const {
		D_ASSERT(owner);
		return owner;
	}
	//! Whether this buffer still presents exactly the viewed storage: not detached, flat, pointing at the
	//! pinned storage, and sharing the storage's validity
	bool IsUnchangedView() const;

	data_ptr_t GetDataMutable() override;
	ValidityMask &GetValidityMask() override;
	const ValidityMask &GetValidityMask() const override {
		return validity;
	}
	void ResetCapacity(idx_t capacity) override;

protected:
	void ReserveInternal(idx_t new_size) override;

private:
	void Detach();

	shared_ptr<ColumnDataSharedListChild> owner;
	BufferHandle pin;
	//! The storage address the view was created with
	data_ptr_t view_data;
};

//! A reference to shared list child storage, covering the claimed count at reference time
struct SharedListChildReference {
	shared_ptr<ColumnDataSharedListChild> owner;
	//! The number of entries this reference covers (the storage may have grown since)
	idx_t count;
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
	//! Slots in the segment's shared list child references that belong to this chunk
	vector<idx_t> shared_child_slots;
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
	//! Shared list child storage referenced by SHARED_LIST_CHILD entries (the entry's 'offset' is the slot)
	vector<SharedListChildReference> shared_child_buffers;
	//! The distinct shared storage referenced by this segment, so each is accounted once
	unordered_set<idx_t> shared_child_owners;
	//! Allocation bytes of the distinct shared list child storage, counted like block storage
	idx_t shared_child_allocation_size = 0;
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
	//! The number of entries in a vector data entry, resolving shared list children
	idx_t GetVectorCount(VectorDataIndex index);
	//! The shared buffer reference of a SHARED_LIST_CHILD entry
	const SharedListChildReference &GetSharedListChild(const VectorMetaData &vdata) const {
		D_ASSERT(vdata.representation == ColumnDataRepresentation::SHARED_LIST_CHILD);
		D_ASSERT(vdata.offset < shared_child_buffers.size());
		// released references belong to consumed chunks, which are never read again
		D_ASSERT(shared_child_buffers[vdata.offset].owner);
		return shared_child_buffers[vdata.offset];
	}
	//! Add a vector data entry of the chunk referencing 'count' entries of shared list child storage
	VectorDataIndex AddSharedListChild(shared_ptr<ColumnDataSharedListChild> owner, idx_t count,
	                                   VectorDataIndex prev_index, ChunkMetaData &chunk_meta);
	//! Drop the shared list child references of a consumed chunk, so storage nothing else references is freed
	void ReleaseSharedListChildren(idx_t chunk_index);
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
