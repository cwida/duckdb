#include "duckdb/main/client_context.hpp"
#include "duckdb/common/vector/array_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/types/row/tuple_data_collection.hpp"

#include "duckdb/common/limits.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/common/row_operations/row_operations.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/common/type_visitor.hpp"
#include "duckdb/common/types/row/tuple_data_allocator.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parallel/parallel_destroy_task.hpp"

#include <algorithm>
#include "duckdb/storage/buffer_manager.hpp"

namespace duckdb {

using ValidityBytes = TupleDataLayout::ValidityBytes;

TupleDataCollection::TupleDataCollection(BufferManager &buffer_manager, shared_ptr<TupleDataLayout> layout_ptr_p,
                                         MemoryTag tag_p, shared_ptr<ArenaAllocator> stl_allocator_p,
                                         QueryContext context)
    : scheduler(TaskScheduler::GetScheduler(buffer_manager.GetDatabase())),
      stl_allocator(stl_allocator_p ? std::move(stl_allocator_p)
                                    : make_shared_ptr<ArenaAllocator>(buffer_manager.GetBufferAllocator())),
      layout_ptr(std::move(layout_ptr_p)), layout(*layout_ptr), tag(tag_p),
      allocator(make_shared_ptr<TupleDataAllocator>(buffer_manager, layout_ptr, tag, stl_allocator, context)),
      segments(*stl_allocator), scatter_functions(*stl_allocator), gather_functions(*stl_allocator) {
	Initialize();
}

TupleDataCollection::TupleDataCollection(ClientContext &context, shared_ptr<TupleDataLayout> layout_ptr, MemoryTag tag,
                                         shared_ptr<ArenaAllocator> stl_allocator)
    : TupleDataCollection(BufferManager::GetBufferManager(context), std::move(layout_ptr), tag,
                          std::move(stl_allocator), context) {
}

TupleDataCollection::~TupleDataCollection() {
	static constexpr idx_t PARALLEL_DESTROY_THRESHOLD = 1048576;
	if (count > PARALLEL_DESTROY_THRESHOLD) {
		ParallelDestroyTask<decltype(segments)>::Schedule(scheduler, segments);
	}
}

void TupleDataCollection::Initialize() {
	D_ASSERT(!layout.GetTypes().empty());
	if (TuplesPerBlock() == 0) {
		throw NotImplementedException("Too many columns: tuple width exceeds block size of %llu",
		                              allocator->GetBufferManager().GetBlockSize());
	}
	this->count = 0;
	this->data_size = 0;
	if (layout.IsSortKeyLayout()) {
		scatter_functions.emplace_back(GetSortKeyScatterFunction(layout.GetTypes()[0], layout.GetSortKeyType()));
		gather_functions.emplace_back(GetSortKeyGatherFunction(layout.GetTypes()[0], layout.GetSortKeyType()));
	} else {
		scatter_functions.reserve(layout.ColumnCount());
		gather_functions.reserve(layout.ColumnCount());
		for (auto &type : layout.GetTypes()) {
			scatter_functions.emplace_back(GetScatterFunction(type));
			gather_functions.emplace_back(GetGatherFunction(type));
		}
	}
}

unique_ptr<TupleDataCollection> TupleDataCollection::CreateUnique() const {
	return make_uniq<TupleDataCollection>(allocator->GetBufferManager(), layout_ptr, tag, nullptr,
	                                      allocator->GetContext());
}

void GetAllColumnIDsInternal(vector<column_t> &column_ids, const idx_t column_count) {
	column_ids.reserve(column_count);
	for (idx_t col_idx = 0; col_idx < column_count; col_idx++) {
		column_ids.emplace_back(col_idx);
	}
}

void TupleDataCollection::GetAllColumnIDs(vector<column_t> &column_ids) {
	GetAllColumnIDsInternal(column_ids, layout.ColumnCount());
}

shared_ptr<TupleDataLayout> TupleDataCollection::GetLayoutPtr() const {
	return layout_ptr;
}

const TupleDataLayout &TupleDataCollection::GetLayout() const {
	return layout;
}

idx_t TupleDataCollection::TuplesPerBlock() const {
	return allocator->GetBufferManager().GetBlockSize() / layout.GetRowWidth();
}

const idx_t &TupleDataCollection::Count() const {
	return count;
}

idx_t TupleDataCollection::ChunkCount() const {
	idx_t total_chunk_count = 0;
	for (const auto &segment : segments) {
		total_chunk_count += segment->ChunkCount();
	}
	return total_chunk_count;
}

idx_t TupleDataCollection::SizeInBytes() const {
	return data_size + stl_allocator->AllocationSize();
}

idx_t TupleDataCollection::GetMaxChunkPartRowSize() const {
	idx_t max_heap_size = 0;
	if (!layout.AllConstant()) {
		for (const auto &segment : segments) {
			for (const auto &part : segment->chunk_parts) {
				if (part->count == 0) {
					continue;
				}
				const auto heap_size = part->total_heap_size / part->count + (part->total_heap_size % part->count != 0);
				max_heap_size = MaxValue(max_heap_size, heap_size);
			}
		}
	}
	const auto row_width = layout.GetRowWidth();
	if (max_heap_size > NumericLimits<idx_t>::Maximum() - row_width) {
		return NumericLimits<idx_t>::Maximum();
	}
	return row_width + max_heap_size;
}

idx_t TupleDataCollection::GetChunkDataSize(idx_t chunk_idx) const {
	for (const auto &segment : segments) {
		if (chunk_idx >= segment->ChunkCount()) {
			chunk_idx -= segment->ChunkCount();
			continue;
		}
		const auto &chunk = *segment->chunks[chunk_idx];
		auto result = chunk.count * layout.GetRowWidth();
		if (!layout.AllConstant()) {
			for (auto part_idx = chunk.part_ids.Start(); part_idx < chunk.part_ids.End(); part_idx++) {
				result += segment->chunk_parts[part_idx]->total_heap_size;
			}
		}
		return result;
	}
	throw InternalException("Chunk index out of range in TupleDataCollection::GetChunkDataSize");
}

idx_t TupleDataCollection::GetBlockAllocationSize() const {
	if (segments.empty() || (segments.size() == 1 && segments[0] && segments[0]->allocator == allocator)) {
		return allocator->GetBlockAllocationSize();
	}
	reference_set_t<TupleDataAllocator> allocators;
	allocators.insert(*allocator);
	for (const auto &segment : segments) {
		if (segment) {
			allocators.insert(*segment->allocator);
		}
	}
	idx_t result = 0;
	for (auto &entry : allocators) {
		result += entry.get().GetBlockAllocationSize();
	}
	return result;
}

void TupleDataCollection::Unpin() {
	for (auto &segment : segments) {
		segment->Unpin();
	}
}

void TupleDataCollection::SetPartitionIndex(const idx_t index) {
	D_ASSERT(!partition_index.IsValid());
	D_ASSERT(Count() == 0);
	partition_index = index;
	allocator->SetPartitionIndex(index);
}

vector<pair<idx_t, idx_t>> TupleDataCollection::GetChunkRangesForPartition(const idx_t partition_idx) const {
	idx_t chunk_idx_start = 0;
	vector<pair<idx_t, idx_t>> chunk_ranges;
	for (const auto &segment : segments) {
		const idx_t segment_partition_idx = segment->allocator->GetPartitionIndex();
		if (partition_idx == segment_partition_idx) {
			chunk_ranges.emplace_back(chunk_idx_start, chunk_idx_start + segment->ChunkCount());
		}
		chunk_idx_start += segment->ChunkCount();
	}
	return chunk_ranges;
}

vector<data_ptr_t> TupleDataCollection::GetRowBlockPointers() {
	D_ASSERT(segments.size() == 1);
	auto &segment = *segments[0];
	vector<data_ptr_t> result;
	result.reserve(segment.pinned_row_handles.size());
	for (auto &pinned_row_handle : segment.pinned_row_handles) {
		result.emplace_back(pinned_row_handle.GetDataMutable());
	}
	return result;
}

static idx_t FindChunkPart(const TupleDataSegment &segment, const idx_t chunk_idx, idx_t offset) {
	if (chunk_idx == segment.ChunkCount()) {
		D_ASSERT(offset == 0);
		return segment.chunk_parts.size();
	}
	const auto &chunk = *segment.chunks[chunk_idx];
	D_ASSERT(offset < chunk.count);
	for (auto part_idx = chunk.part_ids.Start(); part_idx < chunk.part_ids.End(); part_idx++) {
		const auto count = segment.chunk_parts[part_idx]->count;
		if (offset < count) {
			return part_idx;
		}
		offset -= count;
	}
	throw InternalException("Row offset out of range in TupleDataCollection::DestroyChunks");
}

static idx_t FindNextHeapBlock(const TupleDataSegment &segment, idx_t chunk_idx, idx_t part_idx, const idx_t part_end) {
	for (; chunk_idx < segment.ChunkCount() && part_idx < part_end; chunk_idx++) {
		const auto &chunk = *segment.chunks[chunk_idx];
		part_idx = MaxValue<idx_t>(part_idx, chunk.part_ids.Start());
		for (; part_idx < MinValue<idx_t>(chunk.part_ids.End(), part_end); part_idx++) {
			const auto &part = *segment.chunk_parts[part_idx];
			if (part.total_heap_size != 0) {
				return part.heap_block_index;
			}
		}
	}
	return segment.allocator->HeapBlockCount();
}

void TupleDataCollection::DestroyChunks(const idx_t chunk_idx_begin, const idx_t chunk_idx_end,
                                        const idx_t begin_offset, const idx_t end_offset) {
	D_ASSERT(segments.size() == 1); // Assume 1 segment for now (multi-segment destroys can be implemented if needed)
	D_ASSERT(chunk_idx_begin <= chunk_idx_end && chunk_idx_end <= ChunkCount());
	D_ASSERT(chunk_idx_begin != chunk_idx_end || begin_offset <= end_offset);
	if (chunk_idx_begin == chunk_idx_end && begin_offset == end_offset) {
		return;
	}
	auto &segment = *segments[0];
	const auto part_begin = FindChunkPart(segment, chunk_idx_begin, begin_offset);
	const auto part_end = FindChunkPart(segment, chunk_idx_end, end_offset);

	const auto row_block_begin = segment.chunk_parts[part_begin]->row_block_index;
	const auto row_block_end = chunk_idx_end == segment.ChunkCount() ? segment.allocator->RowBlockCount()
	                                                                 : segment.chunk_parts[part_end]->row_block_index;
	segment.allocator->DestroyRowBlocks(row_block_begin, row_block_end);

	if (!layout.AllConstant() && segment.allocator->HeapBlockCount() != 0) {
		// Preserve heaps shared with later parts, including across parts without heap data.
		const auto heap_block_begin = FindNextHeapBlock(segment, chunk_idx_begin, part_begin, part_end);
		if (heap_block_begin == segment.allocator->HeapBlockCount()) {
			return;
		}
		const auto heap_block_end = FindNextHeapBlock(segment, chunk_idx_end, part_end, segment.chunk_parts.size());
		segment.allocator->DestroyHeapBlocks(heap_block_begin, heap_block_end);
	}
}

// LCOV_EXCL_START
void VerifyAppendColumns(const TupleDataLayout &layout, const vector<column_t> &column_ids) {
#ifdef D_ASSERT_IS_ENABLED
	for (idx_t col_idx = 0; col_idx < layout.ColumnCount(); col_idx++) {
		if (std::find(column_ids.begin(), column_ids.end(), col_idx) != column_ids.end()) {
			continue;
		}
		// This column will not be appended in the first go - verify that it is fixed-size - we cannot resize heap after
		const auto physical_type = layout.GetTypes()[col_idx].InternalType();
		D_ASSERT(physical_type != PhysicalType::VARCHAR && physical_type != PhysicalType::LIST &&
		         physical_type != PhysicalType::ARRAY);
		if (physical_type == PhysicalType::STRUCT) {
			const auto &struct_layout = layout.GetStructLayout(col_idx);
			vector<column_t> struct_column_ids;
			struct_column_ids.reserve(struct_layout.ColumnCount());
			for (idx_t struct_col_idx = 0; struct_col_idx < struct_layout.ColumnCount(); struct_col_idx++) {
				struct_column_ids.emplace_back(struct_col_idx);
			}
			VerifyAppendColumns(struct_layout, struct_column_ids);
		}
	}
#endif
}
// LCOV_EXCL_STOP

void TupleDataCollection::InitializeAppend(TupleDataAppendState &append_state, TupleDataPinProperties properties) {
	vector<column_t> column_ids;
	GetAllColumnIDs(column_ids);
	InitializeAppend(append_state, std::move(column_ids), properties);
}

void TupleDataCollection::InitializeAppend(TupleDataAppendState &append_state, vector<column_t> column_ids,
                                           TupleDataPinProperties properties) {
	VerifyAppendColumns(layout, column_ids);
	InitializeAppend(append_state.pin_state, properties);
	InitializeChunkState(append_state.chunk_state, std::move(column_ids));
}

void TupleDataCollection::InitializeAppend(TupleDataPinState &pin_state, TupleDataPinProperties properties) {
	pin_state.properties = properties;
	if (segments.empty()) {
		segments.emplace_back(stl_allocator->MakeUnsafePtr<TupleDataSegment>(allocator));
	}
}

static void InitializeVectorFormat(vector<TupleDataVectorFormat> &vector_data, const vector<LogicalType> &types) {
	vector_data.resize(types.size());
	for (idx_t col_idx = 0; col_idx < types.size(); col_idx++) {
		const auto &type = types[col_idx];
		switch (type.InternalType()) {
		case PhysicalType::STRUCT: {
			const auto &child_list = StructType::GetChildTypes(type);
			vector<LogicalType> child_types;
			child_types.reserve(child_list.size());
			for (const auto &child_entry : child_list) {
				child_types.emplace_back(child_entry.second);
			}
			InitializeVectorFormat(vector_data[col_idx].children, child_types);
			break;
		}
		case PhysicalType::LIST:
			InitializeVectorFormat(vector_data[col_idx].children, {ListType::GetChildType(type)});
			break;
		case PhysicalType::ARRAY:
			InitializeVectorFormat(vector_data[col_idx].children, {ArrayType::GetChildType(type)});
			break;
		default:
			break;
		}
	}
}

void TupleDataCollection::InitializeChunkState(TupleDataChunkState &chunk_state, vector<column_t> column_ids) {
	TupleDataCollection::InitializeChunkState(chunk_state, layout.GetTypes(), std::move(column_ids));
}

void TupleDataCollection::InitializeChunkState(TupleDataChunkState &chunk_state, const vector<LogicalType> &types,
                                               vector<column_t> column_ids) {
	if (column_ids.empty()) {
		GetAllColumnIDsInternal(column_ids, types.size());
	}
	InitializeVectorFormat(chunk_state.vector_data, types);

	chunk_state.cached_cast_vectors.clear();
	chunk_state.cached_cast_vector_cache.clear();
	for (auto &col : column_ids) {
		auto &type = types[col];
		if (TypeVisitor::Contains(type, LogicalTypeId::ARRAY)) {
			auto cast_type = ArrayType::ConvertToList(type);
			chunk_state.cached_cast_vector_cache.push_back(
			    make_uniq<VectorCache>(Allocator::DefaultAllocator(), cast_type));
			chunk_state.cached_cast_vectors.push_back(make_uniq<Vector>(*chunk_state.cached_cast_vector_cache.back()));
		} else {
			chunk_state.cached_cast_vectors.emplace_back();
			chunk_state.cached_cast_vector_cache.emplace_back();
		}
	}

	chunk_state.column_ids = std::move(column_ids);
}

void TupleDataCollection::Append(DataChunk &new_chunk, const SelectionVector &append_sel, idx_t append_count) {
	TupleDataAppendState append_state;
	InitializeAppend(append_state);
	Append(append_state, new_chunk, append_sel, append_count);
}

void TupleDataCollection::Append(DataChunk &new_chunk, vector<column_t> column_ids, const SelectionVector &append_sel,
                                 const idx_t append_count) {
	TupleDataAppendState append_state;
	InitializeAppend(append_state, std::move(column_ids));
	Append(append_state, new_chunk, append_sel, append_count);
}

void TupleDataCollection::Append(TupleDataAppendState &append_state, DataChunk &new_chunk,
                                 const SelectionVector &append_sel, const idx_t append_count) {
	Append(append_state.pin_state, append_state.chunk_state, new_chunk, append_sel, append_count);
}

void TupleDataCollection::Append(TupleDataPinState &pin_state, TupleDataChunkState &chunk_state, DataChunk &new_chunk,
                                 const SelectionVector &append_sel, const idx_t append_count) {
	TupleDataCollection::ToUnifiedFormat(chunk_state, new_chunk);
	AppendUnified(pin_state, chunk_state, new_chunk, append_sel, append_count);
}

void TupleDataCollection::AppendUnified(TupleDataPinState &pin_state, TupleDataChunkState &chunk_state,
                                        DataChunk &new_chunk, const SelectionVector &append_sel,
                                        const idx_t append_count) {
	const idx_t actual_append_count = append_count == DConstants::INVALID_INDEX ? new_chunk.size() : append_count;
	if (actual_append_count == 0) {
		return;
	}

	if (!layout.AllConstant()) {
		if (layout.IsSortKeyLayout()) {
			TupleDataCollection::SortKeyComputeHeapSizes(chunk_state, new_chunk, append_sel, actual_append_count,
			                                             layout.GetSortKeyType());
		} else {
			TupleDataCollection::ComputeHeapSizes(chunk_state, new_chunk, append_sel, actual_append_count);
		}
	}

	Build(pin_state, chunk_state, 0, actual_append_count);
	FlatVector::SetSize(chunk_state.row_locations, actual_append_count);
	Scatter(chunk_state, new_chunk, append_sel, actual_append_count);
}

static inline void ToUnifiedFormatInternal(TupleDataVectorFormat &format, const Vector &vector) {
	vector.ToUnifiedFormat(format.unified);
	format.original_sel = format.unified.sel;
	format.original_owned_sel.Initialize(format.unified.owned_sel);
	switch (vector.GetType().InternalType()) {
	case PhysicalType::STRUCT: {
		auto &entries = StructVector::GetEntries(vector);
		D_ASSERT(format.children.size() == entries.size());
		for (idx_t struct_col_idx = 0; struct_col_idx < entries.size(); struct_col_idx++) {
			ToUnifiedFormatInternal(format.children[struct_col_idx], entries[struct_col_idx]);
		}
		break;
	}
	case PhysicalType::LIST:
		D_ASSERT(format.children.size() == 1);
		ToUnifiedFormatInternal(format.children[0], ListVector::GetChild(vector));
		break;
	case PhysicalType::ARRAY: {
		D_ASSERT(format.children.size() == 1);

		// For arrays, we cheat a bit and pretend that they are lists by creating and assigning list_entry_t's to the
		// vector This allows us to reuse all the list serialization functions for array types too.
		auto array_size = ArrayType::GetSize(vector.GetType());

		// How many list_entry_t's do we need to cover the whole child array?
		// Make sure we round up so its all covered
		auto child_array_total_size = ArrayVector::GetTotalSize(vector);
		auto list_entry_t_count =
		    MaxValue((child_array_total_size + array_size) / array_size, format.unified.validity.Capacity());

		// Create list entries!
		format.array_list_entries = make_unsafe_uniq_array<list_entry_t>(list_entry_t_count);
		for (idx_t i = 0; i < list_entry_t_count; i++) {
			format.array_list_entries[i].length = array_size;
			format.array_list_entries[i].offset = i * array_size;
		}
		format.unified.data = reinterpret_cast<data_ptr_t>(format.array_list_entries.get());

		ToUnifiedFormatInternal(format.children[0], ArrayVector::GetChild(vector));
		break;
	}
	default:
		break;
	}
}

void TupleDataCollection::ToUnifiedFormat(TupleDataChunkState &chunk_state, DataChunk &new_chunk) {
	D_ASSERT(chunk_state.vector_data.size() >= chunk_state.column_ids.size()); // Needs InitializeAppend
	for (const auto &col_idx : chunk_state.column_ids) {
		ToUnifiedFormatInternal(chunk_state.vector_data[col_idx], new_chunk.data[col_idx]);
	}
}

void TupleDataCollection::GetVectorData(const TupleDataChunkState &chunk_state, UnifiedVectorFormat result[]) {
	const auto &vector_data = chunk_state.vector_data;
	for (idx_t i = 0; i < vector_data.size(); i++) {
		const auto &source = vector_data[i].unified;
		auto &target = result[i];
		target.sel = source.sel;
		target.data = source.data;
		target.validity = source.validity;
	}
}

void TupleDataCollection::Build(TupleDataPinState &pin_state, TupleDataChunkState &chunk_state,
                                const idx_t append_offset, const idx_t append_count) {
	auto &segment = *segments.back();
	const auto size_before = segment.SizeInBytes();
	segment.allocator->Build(segment, pin_state, chunk_state, append_offset, append_count);
	data_size += segment.SizeInBytes() - size_before;
	count += append_count;
	Verify();
}

// LCOV_EXCL_START
void VerifyHeapSizes(const data_ptr_t source_locations[], const idx_t heap_sizes[], const SelectionVector &append_sel,
                     const idx_t append_count, const idx_t heap_size_offset) {
#ifdef D_ASSERT_IS_ENABLED
	for (idx_t i = 0; i < append_count; i++) {
		auto idx = append_sel.get_index(i);
		const auto stored_heap_size = Load<idx_t>(source_locations[idx] + heap_size_offset);
		D_ASSERT(stored_heap_size == heap_sizes[idx]);
	}
#endif
}
// LCOV_EXCL_STOP

void TupleDataCollection::CopyRows(TupleDataChunkState &chunk_state, TupleDataChunkState &input,
                                   const SelectionVector &append_sel, const idx_t append_count) const {
	const auto source_locations = FlatVector::GetData<data_ptr_t>(input.row_locations);
	const auto target_locations = FlatVector::GetData<data_ptr_t>(chunk_state.row_locations);

	// Copy rows
	const auto row_width = layout.GetRowWidth();
	if (append_sel.IsSet()) {
		for (idx_t i = 0; i < append_count; i++) {
			const auto idx = append_sel[i];
			memcpy(target_locations[i], source_locations[idx], row_width);
		}
	} else {
		for (idx_t i = 0; i < append_count; i++) {
			memcpy(target_locations[i], source_locations[i], row_width);
		}
	}

	// Copy heap if we need to
	if (!layout.AllConstant()) {
		const auto source_heap_locations = FlatVector::GetData<data_ptr_t>(input.heap_locations);
		const auto target_heap_locations = FlatVector::GetData<data_ptr_t>(chunk_state.heap_locations);
		const auto heap_sizes = FlatVector::GetData<idx_t>(input.heap_sizes);
		VerifyHeapSizes(source_locations, heap_sizes, append_sel, append_count, layout.GetHeapSizeOffset());

		// Check if we need to copy anything at all
		idx_t total_heap_size = 0;
		if (!append_sel.IsSet()) {
			// Fast path
			for (idx_t i = 0; i < append_count; i++) {
				total_heap_size += heap_sizes[i];
			}
		} else {
			for (idx_t i = 0; i < append_count; i++) {
				auto idx = append_sel.get_index(i);
				total_heap_size += heap_sizes[idx];
			}
		}
		if (total_heap_size == 0) {
			return;
		}

		// Copy heap
		if (!append_sel.IsSet()) {
			// Fast path
			for (idx_t i = 0; i < append_count; i++) {
				if (heap_sizes[i] > 0) {
					memcpy(target_heap_locations[i], source_heap_locations[i], heap_sizes[i]);
				}
			}
		} else {
			for (idx_t i = 0; i < append_count; i++) {
				auto idx = append_sel.get_index(i);
				if (heap_sizes[idx] > 0) {
					memcpy(target_heap_locations[i], source_heap_locations[idx], heap_sizes[idx]);
				}
			}
		}

		// Recompute pointers after copying the data
		TupleDataAllocator::RecomputeHeapPointers(input.heap_locations, append_sel, target_locations,
		                                          chunk_state.heap_locations, 0, append_count, layout, 0);
	}
}

void TupleDataCollection::FindHeapPointers(TupleDataChunkState &chunk_state, const idx_t chunk_count) const {
	D_ASSERT(!layout.AllConstant());
	const auto row_locations = FlatVector::GetData<data_ptr_t>(chunk_state.row_locations);
	const auto heap_sizes = FlatVector::GetDataMutable<idx_t>(chunk_state.heap_sizes);

	auto &not_found = chunk_state.utility;
	idx_t not_found_count = 0;

	const auto &heap_size_offset = layout.GetHeapSizeOffset();
	for (idx_t i = 0; i < chunk_count; i++) {
		const auto &row_location = row_locations[i];
		auto &heap_size = heap_sizes[i];

		heap_size = Load<idx_t>(row_location + heap_size_offset);
		if (heap_size != 0) {
			not_found.set_index(not_found_count++, i);
		}
	}

	TupleDataAllocator::FindHeapPointers(chunk_state, not_found, not_found_count, layout, 0);
}

void TupleDataCollection::Combine(TupleDataCollection &other) {
	if (other.count == 0) {
		return;
	}
	if (this->layout.GetTypes() != other.GetLayout().GetTypes()) {
		throw InternalException("Attempting to combine TupleDataCollection with mismatching types");
	}
	this->segments.reserve(this->segments.size() + other.segments.size());
	for (auto &other_seg : other.segments) {
		AddSegment(std::move(other_seg));
	}
	other.Reset();
}

void TupleDataCollection::AddSegment(unsafe_arena_ptr<TupleDataSegment> segment) {
	count += segment->count;
	data_size += segment->data_size;
	segments.emplace_back(std::move(segment));
	Verify();
}

void TupleDataCollection::Combine(unique_ptr<TupleDataCollection> other) {
	Combine(*other);
}

void TupleDataCollection::Reset() {
	count = 0;
	data_size = 0;
	// Find the first non-null segment.
	// Segments may be null if they were moved out by a prior Combine() call.
	idx_t live_idx = segments.size(); // sentinel: no live segment
	for (idx_t i = 0; i < segments.size(); i++) {
		if (segments[i]) {
			live_idx = i;
			break;
		}
	}

	if (live_idx < segments.size()) {
		// At least one live segment found. Keep exactly one live segment and make the collection-level allocator
		// match that segment allocator before resetting to preserve segment/allocator consistency.
		if (live_idx > 0) {
			segments[0] = std::move(segments[live_idx]);
		}
		segments.resize(1);
		allocator = segments[0]->allocator;
		segments[0]->Reset();
		allocator->Reset();
		D_ASSERT(segments[0]->allocator.get() == allocator.get());
		D_ASSERT(segments[0]->count == 0);
		D_ASSERT(segments[0]->chunks.empty());
		D_ASSERT(segments[0]->chunk_parts.empty());
	} else {
		// All segments were null (moved out by Combine).  The old allocator is still shared by
		// those moved-out segments and must keep its row_blocks intact.  Create a fresh allocator.
		segments.clear();

		// Refreshes the TupleDataAllocator to prevent holding on to allocated data unnecessarily
		allocator = make_shared_ptr<TupleDataAllocator>(*allocator);
	}
}

void TupleDataCollection::InitializeChunk(DataChunk &chunk) const {
	chunk.Initialize(allocator->GetAllocator(), layout.GetTypes());
}

void TupleDataCollection::InitializeChunk(DataChunk &chunk, const vector<column_t> &columns) const {
	vector<LogicalType> chunk_types(columns.size());
	// keep the order of the columns
	for (idx_t i = 0; i < columns.size(); i++) {
		auto column_idx = columns[i];
		D_ASSERT(column_idx < layout.ColumnCount());
		chunk_types[i] = layout.GetTypes()[column_idx];
	}
	chunk.Initialize(allocator->GetAllocator(), chunk_types);
}

void TupleDataCollection::InitializeScanChunk(const TupleDataScanState &state, DataChunk &chunk) const {
	auto &column_ids = state.chunk_state.column_ids;
	D_ASSERT(!column_ids.empty());
	vector<LogicalType> chunk_types;
	chunk_types.reserve(column_ids.size());
	for (idx_t i = 0; i < column_ids.size(); i++) {
		auto column_idx = column_ids[i];
		D_ASSERT(column_idx < layout.ColumnCount());
		chunk_types.push_back(layout.GetTypes()[column_idx]);
	}
	chunk.Initialize(allocator->GetAllocator(), chunk_types);
}

void TupleDataCollection::InitializeScan(TupleDataScanState &state, TupleDataPinProperties properties) const {
	vector<column_t> column_ids;
	column_ids.reserve(layout.ColumnCount());
	for (idx_t i = 0; i < layout.ColumnCount(); i++) {
		column_ids.push_back(i);
	}
	InitializeScan(state, std::move(column_ids), properties);
}

void TupleDataCollection::InitializeScan(TupleDataScanState &state, vector<column_t> column_ids,
                                         TupleDataPinProperties properties) const {
	state.Reset();
	state.pin_state.properties = properties;
	state.segment_index = 0;
	state.chunk_index = 0;

	auto &chunk_state = state.chunk_state;

	for (auto &col : column_ids) {
		auto &type = layout.GetTypes()[col];

		if (TypeVisitor::Contains(type, LogicalTypeId::ARRAY)) {
			auto cast_type = ArrayType::ConvertToList(type);
			chunk_state.cached_cast_vector_cache.push_back(
			    make_uniq<VectorCache>(Allocator::DefaultAllocator(), cast_type));
			chunk_state.cached_cast_vectors.push_back(make_uniq<Vector>(*chunk_state.cached_cast_vector_cache.back()));
		} else {
			chunk_state.cached_cast_vectors.emplace_back();
			chunk_state.cached_cast_vector_cache.emplace_back();
		}
	}

	state.chunk_state.column_ids = std::move(column_ids);
}

void TupleDataCollection::SeekScan(TupleDataScanState &state, const TupleDataChunkPosition &position) const {
	D_ASSERT(state.pin_state.row_handles.begin() == state.pin_state.row_handles.end());
	D_ASSERT(position.segment_index < segments.size());
	D_ASSERT(position.chunk_index < segments[position.segment_index]->ChunkCount());
	state.segment_index = position.segment_index;
	state.chunk_index = position.chunk_index;
}

void TupleDataCollection::InitializeScan(TupleDataParallelScanState &gstate, TupleDataPinProperties properties) const {
	InitializeScan(gstate.scan_state, properties);
}

void TupleDataCollection::InitializeScan(TupleDataParallelScanState &state, vector<column_t> column_ids,
                                         TupleDataPinProperties properties) const {
	InitializeScan(state.scan_state, std::move(column_ids), properties);
}

idx_t TupleDataCollection::FetchChunk(TupleDataScanState &state, idx_t chunk_idx, bool init_heap,
                                      optional_ptr<SortKeyPayloadState> sort_key_payload_state) {
	for (idx_t segment_idx = 0; segment_idx < segments.size(); segment_idx++) {
		auto &segment = *segments[segment_idx];
		if (chunk_idx < segment.ChunkCount()) {
			segment.allocator->InitializeChunkState(segment, state.pin_state, state.chunk_state, chunk_idx, init_heap,
			                                        sort_key_payload_state);
			return segment.chunks[chunk_idx]->count;
		}
		chunk_idx -= segment.ChunkCount();
	}
	throw InternalException("Chunk index out of in TupleDataCollection::FetchChunk");
}

bool TupleDataCollection::Scan(TupleDataScanState &state, DataChunk &result) {
	const auto segment_index_before = state.segment_index;
	idx_t segment_index;
	idx_t chunk_index;
	if (!NextScanIndex(state, segment_index, chunk_index)) {
		if (!segments.empty()) {
			FinalizePinState(state.pin_state, *segments[segment_index_before]);
		}
		result.SetChildCardinality(0);
		return false;
	}
	if (segment_index_before != DConstants::INVALID_INDEX && segment_index != segment_index_before) {
		FinalizePinState(state.pin_state, *segments[segment_index_before]);
	}
	ScanAtIndex(state.pin_state, state.chunk_state, state.chunk_state.column_ids, segment_index, chunk_index, result);
	return true;
}

bool TupleDataCollection::Scan(TupleDataParallelScanState &gstate, TupleDataLocalScanState &lstate, DataChunk &result) {
	lstate.pin_state.properties = gstate.scan_state.pin_state.properties;

	const auto segment_index_before = lstate.segment_index;
	{
		lock_guard<mutex> guard(gstate.lock);
		if (!NextScanIndex(gstate.scan_state, lstate.segment_index, lstate.chunk_index)) {
			if (!segments.empty()) {
				FinalizePinState(lstate.pin_state, *segments[segment_index_before]);
			}
			result.SetChildCardinality(0);
			return false;
		}
	}
	if (segment_index_before != DConstants::INVALID_INDEX && segment_index_before != lstate.segment_index) {
		FinalizePinState(lstate.pin_state, *segments[lstate.segment_index]);
	}
	ScanAtIndex(lstate.pin_state, lstate.chunk_state, gstate.scan_state.chunk_state.column_ids, lstate.segment_index,
	            lstate.chunk_index, result);
	return true;
}

idx_t TupleDataCollection::Seek(TupleDataScanState &state, const idx_t target_chunk) {
	D_ASSERT(state.pin_state.properties == TupleDataPinProperties::UNPIN_AFTER_DONE);
	state.pin_state.row_handles.clear();
	state.pin_state.heap_handles.clear();

	// early return for empty collection
	if (segments.empty()) {
		return 0;
	}

	idx_t current_chunk = 0;
	idx_t total_rows = 0;
	for (idx_t seg_idx = 0; seg_idx < segments.size(); seg_idx++) {
		auto &segment = segments[seg_idx];
		idx_t chunk_count = segment->ChunkCount();

		if (current_chunk + chunk_count <= target_chunk) {
			total_rows += segment->count;
			current_chunk += chunk_count;
		} else {
			idx_t chunk_idx_in_segment = target_chunk - current_chunk;
			for (idx_t chunk_idx = 0; chunk_idx < chunk_idx_in_segment; chunk_idx++) {
				total_rows += segment->chunks[chunk_idx]->count;
			}
			current_chunk += chunk_count;

			// reset scan state to target segment
			state.segment_index = seg_idx;
			state.chunk_index = chunk_idx_in_segment;
			break;
		}
	}

	D_ASSERT(target_chunk < current_chunk);
	return total_rows;
}

bool TupleDataCollection::ScanComplete(const TupleDataScanState &state) const {
	if (Count() == 0) {
		return true;
	}
	return state.segment_index == segments.size() - 1 && state.chunk_index == segments.back()->ChunkCount();
}

void TupleDataCollection::FinalizePinState(TupleDataPinState &pin_state, TupleDataSegment &segment) {
	segment.allocator->ReleaseOrStoreHandles(pin_state, segment);
}

void TupleDataCollection::FinalizePinState(TupleDataPinState &pin_state) {
	if (segments.empty()) {
		// The collection was Reset() while an append state still referred to it, which happens when an exception
		// (e.g. out-of-memory) unwinds a Repartition after it moved this partition's rows and before the owner
		// re-initialised its append states. There is no segment left to store the handles in, so release them.
		pin_state.row_handles.clear();
		pin_state.heap_handles.clear();
		return;
	}
	FinalizePinState(pin_state, *segments.back());
}

bool TupleDataCollection::NextScanIndex(TupleDataScanState &state, idx_t &segment_index, idx_t &chunk_index) {
	// Check if we still have segments to scan
	if (state.segment_index >= segments.size()) {
		// No more data left in the scan
		return false;
	}
	// Check within the current segment if we still have chunks to scan
	while (state.chunk_index >= segments[state.segment_index]->ChunkCount()) {
		// Exhausted all chunks for this segment: Move to the next one
		state.segment_index++;
		state.chunk_index = 0;
		if (state.segment_index >= segments.size()) {
			return false;
		}
	}
	segment_index = state.segment_index;
	chunk_index = state.chunk_index++;
	return true;
}
void TupleDataCollection::ScanAtIndex(TupleDataPinState &pin_state, TupleDataChunkState &chunk_state,
                                      const vector<column_t> &column_ids, idx_t segment_index, idx_t chunk_index,
                                      DataChunk &result) {
	auto &segment = *segments[segment_index];
	const auto &chunk = *segment.chunks[chunk_index];
	segment.allocator->InitializeChunkState(segment, pin_state, chunk_state, chunk_index, false);
	result.Reset();

	ResetCachedCastVectors(chunk_state, column_ids);
	Gather(chunk_state.row_locations, *FlatVector::IncrementalSelectionVector(), chunk.count, column_ids, result,
	       *FlatVector::IncrementalSelectionVector(), chunk_state.cached_cast_vectors);
	result.SetChildCardinality(chunk.count);
}

void TupleDataCollection::ResetCachedCastVectors(TupleDataChunkState &chunk_state, const vector<column_t> &column_ids) {
	for (idx_t i = 0; i < column_ids.size(); i++) {
		if (chunk_state.cached_cast_vectors[i]) {
			chunk_state.cached_cast_vectors[i]->ResetFromCache(*chunk_state.cached_cast_vector_cache[i]);
		}
	}
}

// LCOV_EXCL_START
string TupleDataCollection::ToString() {
	DataChunk chunk;
	InitializeChunk(chunk);

	TupleDataScanState scan_state;
	InitializeScan(scan_state);

	string result = StringUtil::Format("TupleDataCollection - [%llu Chunks, %llu Rows]\n", ChunkCount(), Count());
	idx_t chunk_idx = 0;
	idx_t row_count = 0;
	while (Scan(scan_state, chunk)) {
		result +=
		    StringUtil::Format("Chunk %llu - [Rows %llu - %llu]\n", chunk_idx, row_count, row_count + chunk.size()) +
		    chunk.ToString();
		chunk_idx++;
		row_count += chunk.size();
	}

	return result;
}

void TupleDataCollection::Print() {
	Printer::Print(ToString());
}

void TupleDataCollection::Verify() const {
#ifdef D_ASSERT_IS_ENABLED
	idx_t total_count = 0;
	idx_t total_size = 0;
	for (const auto &segment : segments) {
		segment->Verify();
		total_count += segment->count;
		total_size += segment->data_size;
	}
	D_ASSERT(total_count == this->count);
	D_ASSERT(total_size == this->data_size);
#endif
}

void TupleDataCollection::VerifyEverythingPinned() const {
#ifdef D_ASSERT_IS_ENABLED
	for (const auto &segment : segments) {
		segment->VerifyEverythingPinned();
	}
#endif
}
// LCOV_EXCL_STOP

} // namespace duckdb
