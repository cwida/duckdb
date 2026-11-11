#include "duckdb/common/vector/array_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/types/column/column_data_collection_segment.hpp"

#include "duckdb/common/vector_operations/vector_operations.hpp"

namespace duckdb {

ColumnDataCollectionSegment::ColumnDataCollectionSegment(shared_ptr<ColumnDataAllocator> allocator_p,
                                                         vector<LogicalType> types_p)
    : allocator(std::move(allocator_p)), total_allocated(0), last_chunk_total_allocated(0), types(std::move(types_p)),
      count(0), heap(make_shared_ptr<StringHeap>(allocator->GetAllocator())) {
}

void ColumnDataCollectionSegment::Reset() {
	count = 0;
	total_allocated = 0;
	last_chunk_total_allocated = 0;
	chunk_data.clear();
	vector_data.clear();
	child_indices.clear();
	heap->GetAllocator().Reset();
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

VectorDataIndex ColumnDataCollectionSegment::AllocateVectorInternal(const LogicalType &type, ChunkMetaData &chunk_meta,
                                                                    ChunkManagementState *chunk_state) {
	VectorMetaData meta_data;
	meta_data.count = 0;

	auto internal_type = type.InternalType();
	auto struct_or_array = internal_type == PhysicalType::STRUCT || internal_type == PhysicalType::ARRAY;
	auto type_size = struct_or_array ? 0 : GetTypeIdSize(internal_type);

	const auto allocation_size = GetDataSize(type_size) + ValidityMask::STANDARD_MASK_SIZE;
	allocator->AllocateData(allocation_size, meta_data.block_id, meta_data.offset, chunk_state);
	total_allocated += allocation_size;
	if (allocator->GetType() == ColumnDataAllocatorType::BUFFER_MANAGER_ALLOCATOR ||
	    allocator->GetType() == ColumnDataAllocatorType::HYBRID) {
		chunk_meta.block_ids.insert(meta_data.block_id);
	}

	auto index = vector_data.size();
	vector_data.push_back(meta_data);
	return VectorDataIndex(index);
}

VectorDataIndex ColumnDataCollectionSegment::AllocateVector(const LogicalType &type, ChunkMetaData &chunk_meta,
                                                            ChunkManagementState *chunk_state,
                                                            VectorDataIndex prev_index) {
	auto index = AllocateVectorInternal(type, chunk_meta, chunk_state);
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
			auto child_index = AllocateVector(child_types[child_idx].second, chunk_meta, chunk_state, prev_child_index);
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
		auto vector_idx = AllocateVector(types[i], meta_data);
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

idx_t ColumnDataCollectionSegment::ReadVectorInternal(ChunkManagementState &state, VectorDataIndex vector_index,
                                                      Vector &result) {
	auto &vector_type = result.GetType();
	auto internal_type = vector_type.InternalType();
	auto type_size = GetTypeIdSize(internal_type);
	auto &vdata = GetVectorData(vector_index);

	auto base_ptr = allocator->GetDataPointer(state, vdata.block_id, vdata.offset);
	auto validity_data = GetValidityPointer(base_ptr, type_size, vdata.count);
	if (!vdata.next_data.IsValid() && state.properties != ColumnDataScanProperties::DISALLOW_ZERO_COPY) {
		// no next data, we can do a zero-copy read of this vector
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
		auto &current_vdata = GetVectorData(next_index);
		vector_count += current_vdata.count;
		next_index = current_vdata.next_data;
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
		base_ptr = allocator->GetDataPointer(state, current_vdata.block_id, current_vdata.offset);
		validity_data = GetValidityPointer(base_ptr, type_size, current_vdata.count);
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
	return allocator->SizeInBytes() + heap->SizeInBytes();
}

idx_t ColumnDataCollectionSegment::AllocationSize() const {
	D_ASSERT(!allocator->IsShared());
	return allocator->AllocationSize() + heap->AllocationSize();
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
