#include "duckdb/common/vector/array_vector.hpp"
#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/vector/dictionary_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/types/column/column_data_collection_segment.hpp"

#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/storage/buffer_manager.hpp"

namespace duckdb {

ColumnDataSharedListChild::ColumnDataSharedListChild(BufferManager &buffer_manager_p, shared_ptr<BlockHandle> block_p,
                                                     idx_t capacity_p, idx_t type_size_p, idx_t count)
    : buffer_manager(buffer_manager_p), block(std::move(block_p)), capacity(capacity_p), type_size(type_size_p),
      claimed_count(count) {
	static atomic<idx_t> next_id {0};
	id = next_id++;
	D_ASSERT(count <= capacity);
	// all-valid until a creator initializes a mask, the capacity covers the whole storage
	validity.Reset(capacity);
}

BufferHandle ColumnDataSharedListChild::Pin() {
	return buffer_manager.Pin(block);
}

idx_t ColumnDataSharedListChild::AllocationSize() const {
	return capacity * type_size + validity.GetAllocationSize();
}

ColumnDataListChildBuffer::ColumnDataListChildBuffer(shared_ptr<ColumnDataSharedListChild> owner_p, BufferHandle pin_p,
                                                     count_t count)
    : StandardVectorBuffer(pin_p.GetDataMutable(), count, owner_p->TypeSize()), owner(std::move(owner_p)),
      pin(std::move(pin_p)), view_data(data_ptr) {
	D_ASSERT(owner->GetClaimedCount() >= count);
	buffer_type = VectorBufferType::SHARED_DATA_BUFFER;
	if (owner->GetValidity().CanHaveNull()) {
		validity.Initialize(owner->GetValidity());
	}
}

void ColumnDataListChildBuffer::Detach() {
	if (!owner) {
		return;
	}
	// copy the viewed entries into private storage before anything writes to them
	auto private_data = Allocator::DefaultAllocator().Allocate(capacity * type_size);
	memcpy(private_data.get(), data_ptr, capacity * type_size);
	ValidityMask private_validity;
	private_validity.Copy(validity, capacity);
	allocated_data = std::move(private_data);
	data_ptr = allocated_data.get();
	validity = std::move(private_validity);
	// the pin stays: slices taken before the detach still point into the pinned storage
	owner.reset();
	buffer_type = VectorBufferType::STANDARD_BUFFER;
}

bool ColumnDataListChildBuffer::IsUnchangedView() const {
	if (!owner || vector_type != VectorType::FLAT_VECTOR || data_ptr != view_data) {
		return false;
	}
	// a mask allocated on the view alone (e.g. a NULL assigned to an all-valid view) diverges from the storage
	return validity.GetData() == owner->GetValidity().GetData();
}

data_ptr_t ColumnDataListChildBuffer::GetDataMutable() {
	Detach();
	return StandardVectorBuffer::GetDataMutable();
}

ValidityMask &ColumnDataListChildBuffer::GetValidityMask() {
	Detach();
	return StandardVectorBuffer::GetValidityMask();
}

void ColumnDataListChildBuffer::ResetCapacity(idx_t capacity_p) {
	Detach();
	StandardVectorBuffer::ResetCapacity(capacity_p);
}

void ColumnDataListChildBuffer::ReserveInternal(idx_t new_size) {
	Detach();
	StandardVectorBuffer::ReserveInternal(new_size);
}

ColumnDataCollectionSegment::ColumnDataCollectionSegment(shared_ptr<ColumnDataAllocator> allocator_p,
                                                         vector<LogicalType> types_p)
    : allocator(std::move(allocator_p)), total_allocated(0), last_chunk_total_allocated(0), constant_metadata_size(0),
      types(std::move(types_p)), count(0), heap(make_shared_ptr<StringHeap>(allocator->GetAllocator())) {
}

void ColumnDataCollectionSegment::Reset() {
	count = 0;
	total_allocated = 0;
	last_chunk_total_allocated = 0;
	constant_metadata_size = 0;
	chunk_data.clear();
	vector_data.clear();
	child_indices.clear();
	dictionaries.clear();
	dictionary_map.clear();
	shared_child_buffers.clear();
	shared_child_owners.clear();
	shared_child_allocation_size = 0;
	heap->GetAllocator().Reset();
}

idx_t ColumnDataCollectionSegment::GetVectorCount(VectorDataIndex index) {
	auto &vdata = GetVectorData(index);
	if (vdata.representation == ColumnDataRepresentation::SHARED_LIST_CHILD) {
		return GetSharedListChild(vdata).count;
	}
	return vdata.count;
}

VectorDataIndex ColumnDataCollectionSegment::AddSharedListChild(shared_ptr<ColumnDataSharedListChild> owner,
                                                                idx_t count_p, VectorDataIndex prev_index,
                                                                ChunkMetaData &chunk_meta) {
	D_ASSERT(owner && count_p <= owner->GetClaimedCount());
	VectorMetaData meta_data;
	meta_data.block_id = 0;
	meta_data.offset = NumericCast<uint32_t>(shared_child_buffers.size());
	// mark the entry full so later appends continue on a fresh entry instead of writing into shared data
	meta_data.count = STANDARD_VECTOR_SIZE;
	meta_data.representation = ColumnDataRepresentation::SHARED_LIST_CHILD;
	// the segment holds each distinct storage once, chunk estimates charge the bytes the reference covers
	if (shared_child_owners.insert(owner->Id()).second) {
		shared_child_allocation_size += owner->AllocationSize();
	}
	total_allocated += count_p * owner->TypeSize();
	chunk_meta.shared_child_slots.push_back(shared_child_buffers.size());
	shared_child_buffers.push_back(SharedListChildReference {std::move(owner), count_p});

	VectorDataIndex index(vector_data.size());
	vector_data.push_back(std::move(meta_data));
	if (prev_index.IsValid()) {
		GetVectorData(prev_index).next_data = index;
	}
	return index;
}

void ColumnDataCollectionSegment::ReleaseSharedListChildren(idx_t chunk_index) {
	D_ASSERT(chunk_index < chunk_data.size());
	for (auto slot : chunk_data[chunk_index].shared_child_slots) {
		shared_child_buffers[slot].owner.reset();
	}
	chunk_data[chunk_index].shared_child_slots.clear();
}

idx_t ColumnDataCollectionSegment::GetDataSize(idx_t type_size) {
	return AlignValue(type_size * STANDARD_VECTOR_SIZE);
}

validity_t *ColumnDataCollectionSegment::GetValidityPointerForWriting(data_ptr_t base_ptr, idx_t type_size) {
	return reinterpret_cast<validity_t *>(base_ptr + GetDataSize(type_size));
}

validity_t *ColumnDataCollectionSegment::GetValidityPointer(data_ptr_t base_ptr, idx_t type_size, idx_t count) {
	auto validity_mask = reinterpret_cast<validity_t *>(base_ptr + GetDataSize(type_size));

	// Optimized check to see if all entries are valid
	for (idx_t i = 0; i < (count / ValidityMask::BITS_PER_VALUE); i++) {
		if (!ValidityMask::AllValid(validity_mask[i])) {
			return validity_mask;
		}
	}

	if ((count % ValidityMask::BITS_PER_VALUE) != 0) {
		// Create a mask with the lower `bits_to_check` bits set to 1
		validity_t mask = (1ULL << (count % ValidityMask::BITS_PER_VALUE)) - 1;
		if ((validity_mask[(count / ValidityMask::BITS_PER_VALUE)] & mask) != mask) {
			return validity_mask;
		}
	}
	// All entries are valid, no need to initialize the validity mask
	return nullptr;
}

void ColumnDataCollectionSegment::AllocateVectorStorage(VectorDataIndex index, idx_t allocation_size,
                                                        ChunkMetaData &chunk_meta, ChunkManagementState *chunk_state) {
	uint32_t block_id;
	uint32_t offset;
	allocator->AllocateData(allocation_size, block_id, offset, chunk_state);
	total_allocated += allocation_size;

	auto &meta_data = GetVectorData(index);
	meta_data.block_id = block_id;
	meta_data.offset = offset;
	meta_data.is_allocated = true;
	if (allocator->GetType() == ColumnDataAllocatorType::BUFFER_MANAGER_ALLOCATOR ||
	    allocator->GetType() == ColumnDataAllocatorType::HYBRID) {
		chunk_meta.block_ids.insert(block_id);
	}
}

VectorDataIndex ColumnDataCollectionSegment::AllocateVectorInternal(const LogicalType &type, ChunkMetaData &chunk_meta,
                                                                    ChunkManagementState *chunk_state,
                                                                    bool defer_allocation) {
	VectorMetaData meta_data;
	meta_data.count = 0;

	auto index = vector_data.size();
	vector_data.push_back(meta_data);
	VectorDataIndex result(index);
	if (!defer_allocation) {
		auto internal_type = type.InternalType();
		auto struct_or_array = internal_type == PhysicalType::STRUCT || internal_type == PhysicalType::ARRAY;
		auto type_size = struct_or_array ? 0 : GetTypeIdSize(internal_type);
		AllocateVectorStorage(result, GetDataSize(type_size) + ValidityMask::STANDARD_MASK_SIZE, chunk_meta,
		                      chunk_state);
	}
	return result;
}

VectorDataIndex ColumnDataCollectionSegment::AllocateVector(const LogicalType &type, ChunkMetaData &chunk_meta,
                                                            ChunkManagementState *chunk_state,
                                                            VectorDataIndex prev_index, bool defer_allocation) {
	auto index = AllocateVectorInternal(type, chunk_meta, chunk_state, defer_allocation);
	if (prev_index.IsValid()) {
		GetVectorData(prev_index).next_data = index;
	}
	if (type.InternalType() == PhysicalType::STRUCT) {
		// initialize the struct children
		auto &child_types = StructType::GetChildTypes(type);
		auto base_child_index = ReserveChildren(child_types.size());
		for (idx_t child_idx = 0; child_idx < child_types.size(); child_idx++) {
			VectorDataIndex prev_child_index;
			if (prev_index.IsValid()) {
				prev_child_index = GetChildIndex(GetVectorData(prev_index).child_index, child_idx);
			}
			auto child_index = AllocateVector(child_types[child_idx].second, chunk_meta, chunk_state, prev_child_index,
			                                  defer_allocation);
			SetChildIndex(base_child_index, child_idx, child_index);
		}
		GetVectorData(index).child_index = base_child_index;
	}
	return index;
}

VectorDataIndex ColumnDataCollectionSegment::AllocateVector(const LogicalType &type, ChunkMetaData &chunk_meta,
                                                            ColumnDataAppendState &append_state,
                                                            VectorDataIndex prev_index) {
	return AllocateVector(type, chunk_meta, &append_state.current_chunk_state, prev_index);
}

VectorDataIndex ColumnDataCollectionSegment::AllocateStringHeap(idx_t size, ChunkMetaData &chunk_meta,
                                                                ColumnDataAppendState &append_state,
                                                                VectorDataIndex prev_index) {
	D_ASSERT(allocator->GetType() == ColumnDataAllocatorType::BUFFER_MANAGER_ALLOCATOR);
	D_ASSERT(size != 0);

	VectorMetaData meta_data;
	meta_data.count = 0;

	const auto allocation_size = AlignValue(size);
	allocator->AllocateData(allocation_size, meta_data.block_id, meta_data.offset, &append_state.current_chunk_state);
	total_allocated += allocation_size;
	meta_data.is_allocated = true;

	chunk_meta.block_ids.insert(meta_data.block_id);

	VectorDataIndex index(vector_data.size());
	vector_data.push_back(meta_data);

	if (prev_index.IsValid()) {
		GetVectorData(prev_index).next_data = index;
	}

	return index;
}

void ColumnDataCollectionSegment::AllocateNewChunk() {
	if (!chunk_data.empty()) {
		chunk_data.back().allocation_size = total_allocated - last_chunk_total_allocated;
	}
	last_chunk_total_allocated = total_allocated;

	ChunkMetaData meta_data;
	meta_data.count = 0;
	meta_data.vector_data.reserve(types.size());
	meta_data.allocation_sizes.reserve(types.size());
	for (idx_t i = 0; i < types.size(); i++) {
		const auto allocated_before = total_allocated;
		// block storage is allocated lazily so that entries appended in a compact form never claim row-wise space
		auto vector_idx = AllocateVector(types[i], meta_data, nullptr, VectorDataIndex(), true);
		meta_data.vector_data.push_back(vector_idx);
		meta_data.allocation_sizes.push_back(total_allocated - allocated_before);
	}
	chunk_data.push_back(std::move(meta_data));
}

void ColumnDataCollectionSegment::InitializeChunkState(idx_t chunk_index, ChunkManagementState &state) {
	auto &chunk = chunk_data[chunk_index];
	allocator->InitializeChunkState(state, chunk);
}

VectorDataIndex ColumnDataCollectionSegment::GetChildIndex(VectorChildIndex index, idx_t child_entry) {
	D_ASSERT(index.IsValid());
	D_ASSERT(index.index + child_entry < child_indices.size());
	return VectorDataIndex(child_indices[index.index + child_entry]);
}

VectorChildIndex ColumnDataCollectionSegment::AddChildIndex(VectorDataIndex index) {
	auto result = child_indices.size();
	child_indices.push_back(index);
	return VectorChildIndex(result);
}

VectorChildIndex ColumnDataCollectionSegment::ReserveChildren(idx_t child_count) {
	auto result = child_indices.size();
	for (idx_t i = 0; i < child_count; i++) {
		child_indices.emplace_back();
	}
	return VectorChildIndex(result);
}

void ColumnDataCollectionSegment::SetChildIndex(VectorChildIndex base_idx, idx_t child_number, VectorDataIndex index) {
	D_ASSERT(base_idx.IsValid());
	D_ASSERT(index.IsValid());
	D_ASSERT(base_idx.index + child_number < child_indices.size());
	child_indices[base_idx.index + child_number] = index;
}

bool TypeHasData(const LogicalType &type) {
	return type.InternalType() != PhysicalType::STRUCT && type.InternalType() != PhysicalType::ARRAY;
}

void ColumnDataCollectionSegment::UnswizzleEntryPointers(ChunkManagementState &state, VectorMetaData &vdata,
                                                         Vector &result) {
	for (auto &swizzle_segment : vdata.swizzle_data) {
		auto &string_heap_segment = GetVectorData(swizzle_segment.child_index);
		allocator->UnswizzlePointers(state, result, swizzle_segment, string_heap_segment, 0, false);
	}
}

string_t ColumnDataCollectionSegment::GetConstantString(ChunkManagementState &state, const VectorMetaData &vdata) {
	D_ASSERT(vdata.representation == ColumnDataRepresentation::CONSTANT && vdata.constant.is_valid);
	auto stored = Load<string_t>(vdata.constant.data);
	if (!vdata.constant.heap_index.IsValid()) {
		// inlined string, or string bytes kept in the segment string heap
		return stored;
	}
	// non-inlined string with the bytes in a heap entry: resolve the current block pointer
	auto heap_index = vdata.constant.heap_index;
	auto &heap_vdata = GetVectorData(heap_index);
	auto heap_ptr = allocator->GetDataPointer(state, heap_vdata.block_id, heap_vdata.offset);
	return string_t(const_char_ptr_cast(heap_ptr), UnsafeNumericCast<uint32_t>(stored.GetSize()));
}

Vector ColumnDataCollectionSegment::ReadSegmentDictionary(ChunkManagementState &state, idx_t slot) {
	auto &dict_data = dictionaries[slot];
	auto &dict_vdata = GetVectorData(dict_data.data_index);
	D_ASSERT(!dict_vdata.next_data.IsValid());
	auto type_size = GetTypeIdSize(dict_data.type.InternalType());
	auto base_ptr = allocator->GetDataPointer(state, dict_vdata.block_id, dict_vdata.offset);
	auto validity_data = GetValidityPointer(base_ptr, type_size, dict_vdata.count);
	Vector dict(dict_data.type, base_ptr, dict_vdata.count);
	FlatVector::ValidityMutable(dict).Initialize(validity_data, STANDARD_VECTOR_SIZE);
	UnswizzleEntryPointers(state, dict_vdata, dict);
	return dict;
}

idx_t ColumnDataCollectionSegment::ReadCompactVector(ChunkManagementState &state, VectorDataIndex vector_index,
                                                     Vector &result) {
	auto &vdata = GetVectorData(vector_index);
	D_ASSERT(!vdata.next_data.IsValid());
	const idx_t count = vdata.count;
	const auto zero_copy = state.properties != ColumnDataScanProperties::DISALLOW_ZERO_COPY;
	if (vdata.representation == ColumnDataRepresentation::CONSTANT) {
		auto &constant = vdata.constant;
		auto internal_type = result.GetType().InternalType();
		if (!constant.is_valid) {
			if (zero_copy) {
				FlatVector::SetSize(result, count_t(count));
				result.SetVectorType(VectorType::CONSTANT_VECTOR);
				ConstantVector::SetNull(result, true);
			} else {
				auto &validity = FlatVector::ValidityMutable(result);
				for (idx_t i = 0; i < count; i++) {
					validity.SetInvalid(i);
				}
				FlatVector::SetSize(result, count_t(count));
			}
			return count;
		}
		if (internal_type == PhysicalType::VARCHAR) {
			auto value = GetConstantString(state, vdata);
			auto result_strings = FlatVector::GetDataMutable<string_t>(result);
			if (zero_copy) {
				result_strings[0] = value;
				FlatVector::SetSize(result, count_t(count));
				result.SetVectorType(VectorType::CONSTANT_VECTOR);
			} else {
				auto owned_value = StringVector::AddStringOrBlob(result, value);
				for (idx_t i = 0; i < count; i++) {
					result_strings[i] = owned_value;
				}
				FlatVector::SetSize(result, count_t(count));
			}
			return count;
		}
		auto type_size = GetTypeIdSize(internal_type);
		auto result_data = FlatVector::GetDataMutable(result);
		if (zero_copy) {
			memcpy(result_data, constant.data, type_size);
			FlatVector::SetSize(result, count_t(count));
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		} else {
			for (idx_t i = 0; i < count; i++) {
				memcpy(result_data + i * type_size, constant.data, type_size);
			}
			FlatVector::SetSize(result, count_t(count));
		}
		return count;
	}
	D_ASSERT(vdata.representation == ColumnDataRepresentation::DICTIONARY);
	auto codes = reinterpret_cast<sel_t *>(allocator->GetDataPointer(state, vdata.block_id, vdata.offset));
	SelectionVector sel(codes, count);
	auto dict_entry = make_buffer<DictionaryEntry>(ReadSegmentDictionary(state, vdata.dictionary_slot));
	dict_entry->id = dictionaries[vdata.dictionary_slot].id;
	if (zero_copy) {
		result.Dictionary(std::move(dict_entry), sel, count);
		return count;
	}
	Vector dictionary_vector(result.GetType());
	dictionary_vector.Dictionary(std::move(dict_entry), sel, count);
	VectorOperations::Copy(dictionary_vector, result, count, 0, 0);
	FlatVector::SetSize(result, count_t(count));
	return count;
}

idx_t ColumnDataCollectionSegment::ReadVectorInternal(ChunkManagementState &state, VectorDataIndex vector_index,
                                                      Vector &result) {
	auto &vector_type = result.GetType();
	auto internal_type = vector_type.InternalType();
	auto type_size = GetTypeIdSize(internal_type);
	auto &vdata = GetVectorData(vector_index);
	const auto shared_head = vdata.representation == ColumnDataRepresentation::SHARED_LIST_CHILD;
	D_ASSERT(vdata.is_allocated || shared_head);

	const auto zero_copy = state.properties != ColumnDataScanProperties::DISALLOW_ZERO_COPY;
	if (!vdata.next_data.IsValid() && zero_copy && shared_head) {
		// a complete shared list child: reference the shared buffer through a scan-local view
		auto &shared_child = GetSharedListChild(vdata);
		result.SetBuffer(make_buffer<ColumnDataListChildBuffer>(shared_child.owner, shared_child.owner->Pin(),
		                                                        count_t(shared_child.count)));
		return shared_child.count;
	}
	if (!vdata.next_data.IsValid() && zero_copy) {
		// no next data, we can do a zero-copy read of this vector
		auto base_ptr = allocator->GetDataPointer(state, vdata.block_id, vdata.offset);
		auto validity_data = GetValidityPointer(base_ptr, type_size, vdata.count);
		if (TypeHasData(result.GetType())) {
			FlatVector::SetData(result, base_ptr, count_t(vdata.count));
		}
		FlatVector::ValidityMutable(result).Initialize(validity_data, STANDARD_VECTOR_SIZE);
		FlatVector::SetSize(result, count_t(vdata.count));
		return vdata.count;
	}

	// the data for this vector is spread over multiple vector data entries
	// we need to copy over the data for each of the vectors
	// first figure out how many rows we need to copy by looping over all of the child vector indexes
	idx_t vector_count = 0;
	auto next_index = vector_index;
	while (next_index.IsValid()) {
		vector_count += GetVectorCount(next_index);
		next_index = GetVectorData(next_index).next_data;
	}
	// never materialize into a shared buffer - it may still be referenced by other owners
	auto &result_buffer = result.GetBufferRef();
	if (result_buffer && result_buffer->GetBufferType() == VectorBufferType::SHARED_DATA_BUFFER) {
		result.SetBuffer(make_buffer<StandardVectorBuffer>(capacity_t(vector_count), type_size));
	}
	// resize the result vector
	result.Reserve(vector_count);
	next_index = vector_index;
	// now perform the copy of each of the vectors
	auto target_data = FlatVector::GetDataMutable(result);
	auto &target_validity = FlatVector::ValidityMutable(result);
	idx_t current_offset = 0;
	while (next_index.IsValid()) {
		auto &current_vdata = GetVectorData(next_index);
		if (current_vdata.representation == ColumnDataRepresentation::SHARED_LIST_CHILD) {
			auto &shared_child = GetSharedListChild(current_vdata);
			auto shared_pin = shared_child.owner->Pin();
			if (type_size > 0) {
				memcpy(target_data + current_offset * type_size, shared_pin.Ptr(), shared_child.count * type_size);
			}
			target_validity.SliceInPlace(shared_child.owner->GetValidity(), current_offset, 0, shared_child.count);
			current_offset += shared_child.count;
			next_index = current_vdata.next_data;
			continue;
		}
		auto base_ptr = allocator->GetDataPointer(state, current_vdata.block_id, current_vdata.offset);
		auto validity_data = GetValidityPointer(base_ptr, type_size, current_vdata.count);
		if (type_size > 0) {
			memcpy(target_data + current_offset * type_size, base_ptr, current_vdata.count * type_size);
		}
		ValidityMask current_validity(validity_data, STANDARD_VECTOR_SIZE);
		target_validity.SliceInPlace(current_validity, current_offset, 0, current_vdata.count);
		current_offset += current_vdata.count;
		next_index = current_vdata.next_data;
	}
	FlatVector::SetSize(result, count_t(vector_count));
	return vector_count;
}

idx_t ColumnDataCollectionSegment::ReadVector(ChunkManagementState &state, VectorDataIndex vector_index,
                                              Vector &result) {
	auto &vector_type = result.GetType();
	auto internal_type = vector_type.InternalType();
	auto &vdata = GetVectorData(vector_index);
	if (vdata.count == 0) {
		FlatVector::SetSize(result, 0);
		return 0;
	}
	if (vdata.representation == ColumnDataRepresentation::CONSTANT ||
	    vdata.representation == ColumnDataRepresentation::DICTIONARY) {
		return ReadCompactVector(state, vector_index, result);
	}
	auto vcount = ReadVectorInternal(state, vector_index, result);
	if (internal_type == PhysicalType::LIST) {
		// list: copy child
		auto &child_vector = ListVector::GetChildMutable(result);
		auto child_count = ReadVector(state, GetChildIndex(vdata.child_index), child_vector);
		ListVector::SetListSize(result, child_count);
	} else if (internal_type == PhysicalType::ARRAY) {
		auto &child_vector = ArrayVector::GetChildMutable(result);
		auto child_count = ReadVector(state, GetChildIndex(vdata.child_index), child_vector);
		(void)child_count;
	} else if (internal_type == PhysicalType::STRUCT) {
		auto &child_vectors = StructVector::GetEntries(result);
		for (idx_t child_idx = 0; child_idx < child_vectors.size(); child_idx++) {
			auto child_count = ReadVector(state, GetChildIndex(vdata.child_index, child_idx), child_vectors[child_idx]);
			if (child_count != vcount) {
				throw InternalException("Column Data Collection: mismatch in struct child sizes");
			}
		}
	} else if (internal_type == PhysicalType::VARCHAR) {
		if (allocator->GetType() == ColumnDataAllocatorType::BUFFER_MANAGER_ALLOCATOR) {
			auto next_index = vector_index;
			const auto copied =
			    vdata.next_data.IsValid() || state.properties == ColumnDataScanProperties::DISALLOW_ZERO_COPY;
			idx_t offset = 0;
			while (next_index.IsValid()) {
				auto &current_vdata = GetVectorData(next_index);
				for (auto &swizzle_segment : current_vdata.swizzle_data) {
					auto &string_heap_segment = GetVectorData(swizzle_segment.child_index);
					allocator->UnswizzlePointers(state, result, swizzle_segment, string_heap_segment, offset, copied);
				}
				offset += current_vdata.count;
				next_index = current_vdata.next_data;
			}
		}
		if (state.properties == ColumnDataScanProperties::DISALLOW_ZERO_COPY) {
			VectorOperations::Copy(result, result, vcount, 0, 0);
		}
	}
	return vcount;
}

void ColumnDataCollectionSegment::ReadChunk(idx_t chunk_index, ChunkManagementState &state, DataChunk &chunk,
                                            const vector<column_t> &column_ids) {
	D_ASSERT(chunk.ColumnCount() == column_ids.size());
	D_ASSERT(state.properties != ColumnDataScanProperties::INVALID);
	chunk.Reset();
	InitializeChunkState(chunk_index, state);
	auto &chunk_meta = chunk_data[chunk_index];
	for (idx_t i = 0; i < column_ids.size(); i++) {
		auto vector_idx = column_ids[i];
		D_ASSERT(vector_idx < chunk_meta.vector_data.size());
		ReadVector(state, chunk_meta.vector_data[vector_idx], chunk.data[i]);
	}
	chunk.SetChildCardinality(chunk_meta.count);
}

idx_t ColumnDataCollectionSegment::ChunkCount() const {
	return chunk_data.size();
}

idx_t ColumnDataCollectionSegment::SizeInBytes() const {
	D_ASSERT(!allocator->IsShared());
	return allocator->SizeInBytes() + heap->SizeInBytes() + constant_metadata_size + shared_child_allocation_size;
}

idx_t ColumnDataCollectionSegment::AllocationSize() const {
	D_ASSERT(!allocator->IsShared());
	return allocator->AllocationSize() + heap->AllocationSize() + constant_metadata_size + shared_child_allocation_size;
}

idx_t ColumnDataCollectionSegment::GetChunkAllocationSize(const idx_t chunk_idx) const {
	auto &chunk_meta = chunk_data[chunk_idx];
	if (chunk_meta.allocation_size.IsValid()) {
		return chunk_meta.allocation_size.GetIndex();
	}
	// Last chunk - derive it
	return total_allocated - last_chunk_total_allocated;
}

idx_t ColumnDataCollectionSegment::GetChunkAllocationSize(const idx_t chunk_idx,
                                                          const vector<column_t> &column_ids) const {
	const auto &allocation_sizes = chunk_data[chunk_idx].allocation_sizes;
	idx_t result = 0;
	for (const auto &column_id : column_ids) {
		result += allocation_sizes[column_id];
	}
	return result;
}

void ColumnDataCollectionSegment::FetchChunk(idx_t chunk_idx, DataChunk &result) {
	vector<column_t> column_ids;
	column_ids.reserve(types.size());
	for (idx_t i = 0; i < types.size(); i++) {
		column_ids.push_back(i);
	}
	FetchChunk(chunk_idx, result, column_ids);
}

void ColumnDataCollectionSegment::FetchChunk(idx_t chunk_idx, DataChunk &result, const vector<column_t> &column_ids) {
	D_ASSERT(chunk_idx < chunk_data.size());
	ChunkManagementState state;
	state.properties = ColumnDataScanProperties::DISALLOW_ZERO_COPY;
	ReadChunk(chunk_idx, state, result, column_ids);
}

void ColumnDataCollectionSegment::Verify() {
#ifdef DEBUG
	idx_t total_count = 0;
	for (idx_t i = 0; i < chunk_data.size(); i++) {
		total_count += chunk_data[i].count;
		idx_t allocation_size = 0;
		for (const auto &column_size : chunk_data[i].allocation_sizes) {
			allocation_size += column_size;
		}
		D_ASSERT(allocation_size == GetChunkAllocationSize(i));
	}
	D_ASSERT(total_count == this->count);
#endif
}

} // namespace duckdb
