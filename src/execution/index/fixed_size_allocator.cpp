#include "duckdb/execution/index/fixed_size_allocator.hpp"

#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/metadata/metadata_reader.hpp"

namespace duckdb {

void FixedSizeAllocator::ThrowInvalidPointer(const IndexPointer ptr) const {
	throw DataCorruptionException("Index pointer to buffer %llu, segment %llu does not point to an allocated segment",
	                              ptr.GetBufferId(), ptr.GetOffset());
}

FixedSizeAllocator::FixedSizeAllocator(const idx_t segment_size, BlockManager &block_manager, MemoryTag memory_tag)
    : block_manager(block_manager), buffer_manager(block_manager.buffer_manager), memory_tag(memory_tag),
      segment_size(segment_size), total_segment_count(0) {
	if (segment_size > block_manager.GetBlockSize() - sizeof(validity_t)) {
		throw InternalException("The maximum segment size of fixed-size allocators is " +
		                        to_string(block_manager.GetBlockSize() - sizeof(validity_t)));
	}

	// calculate how many segments fit into one buffer (available_segments_per_buffer)

	const idx_t block_size = block_manager.GetBlockSize();
	const idx_t bits_per_value = sizeof(validity_t) * 8;

	// each full validity_t value is followed by bits_per_value segments: skip all of them at once
	const idx_t full_value_bytes = sizeof(validity_t) + bits_per_value * segment_size;
	const idx_t full_value_count = block_size / full_value_bytes;
	idx_t byte_count = full_value_count * full_value_bytes;

	bitmask_count = full_value_count;
	available_segments_per_buffer = full_value_count * bits_per_value;

	while (byte_count < block_size) {
		if (!bitmask_count || (bitmask_count * bits_per_value) % available_segments_per_buffer == 0) {
			// we need to add another validity_t value to the bitmask, to allow storing another
			// bits_per_value segments on a buffer
			bitmask_count++;
			byte_count += sizeof(validity_t);
		}

		auto remaining_bytes = block_size - byte_count;
		auto remaining_segments = MinValue(remaining_bytes / segment_size, bits_per_value);

		if (remaining_segments == 0) {
			break;
		}

		available_segments_per_buffer += remaining_segments;
		byte_count += remaining_segments * segment_size;
	}

	bitmask_offset = bitmask_count * sizeof(validity_t);
}

IndexPointer FixedSizeAllocator::New() {
	// No more free segments available.
	if (!buffer_with_free_space.IsValid()) {
		// Add a new buffer.
		auto buffer_id = GetAvailableBufferId();
		buffers[buffer_id] = make_shared_ptr<FixedSizeBuffer>(block_manager, memory_tag);
		buffers_with_free_space.insert(buffer_id);
		buffer_with_free_space = buffer_id;

		// Set and initialize the bitmask of the new buffer.
		D_ASSERT(buffers.find(buffer_id) != buffers.end());
		auto &buffer = buffers.find(buffer_id)->second;

		// Get a handle to the buffer's validity mask (offset 0).
		SegmentHandle handle(*buffer, 0);
		const auto bitmask_ptr = handle.GetPtr<validity_t>();
		ValidityMask mask(bitmask_ptr, available_segments_per_buffer);
		mask.SetAllValid(available_segments_per_buffer);
	}

	// Extract an index pointer to a free segment.

	D_ASSERT(buffer_with_free_space.IsValid());
	auto buffer_id = buffer_with_free_space.GetIndex();

	D_ASSERT(buffers.find(buffer_id) != buffers.end());
	auto &buffer = buffers.find(buffer_id)->second;
	auto offset = buffer->AllocateSegment(bitmask_count, available_segments_per_buffer);

	total_segment_count++;

	// If the buffer is full, we cache the next buffer that we're going to fill.
	if (buffer->segment_count == available_segments_per_buffer) {
		buffers_with_free_space.erase(buffer_id);
		NextBufferWithFreeSpace();
	}

	return IndexPointer(uint32_t(buffer_id), offset);
}

void FixedSizeAllocator::Free(const IndexPointer ptr) {
	auto buffer_id = ptr.GetBufferId();
	auto offset = ptr.GetOffset();

	auto buffer_it = buffers.find(buffer_id);
	if (offset >= available_segments_per_buffer || buffer_it == buffers.end()) {
		ThrowInvalidPointer(ptr);
	}
	auto &buffer = buffer_it->second;

	buffer->FreeSegment(offset, available_segments_per_buffer);

	// Adjust the allocator fields.
	D_ASSERT(total_segment_count > 0);
	total_segment_count--;

	// Early-out, if the buffer is not empty.
	if (buffer->segment_count != 0) {
		buffers_with_free_space.insert(buffer_id);
		if (!buffer_with_free_space.IsValid()) {
			buffer_with_free_space = buffer_id;
		}
		return;
	}

	// The segment count went to 0, i.e, the buffer is now empty.
	// We keep the buffer alive, if it is the only buffer with free space.
	// We do so to prevent too much buffer creation fluctuation.
	if (buffers_with_free_space.size() == 1) {
		D_ASSERT(*buffers_with_free_space.begin() == buffer_id);
		D_ASSERT(buffer_with_free_space.GetIndex() == buffer_id);
		return;
	}

	D_ASSERT(buffer_with_free_space.IsValid());
	buffers_with_free_space.erase(buffer_id);
	buffers.erase(buffer_it);

	// Cache the next buffer that we're going to fill.
	if (buffer_with_free_space.GetIndex() == buffer_id) {
		NextBufferWithFreeSpace();
	}
}

void FixedSizeAllocator::Reset() {
	buffers.clear();
	buffers_with_free_space.clear();
	buffer_with_free_space.SetInvalid();
	total_segment_count = 0;
}

void FixedSizeAllocator::VisitBlockIds(BlockIdVisitor &visitor) const {
	for (auto &buffer : buffers) {
		if (buffer.second->OnDisk()) {
			visitor.Visit(buffer.second->block_pointer.block_id);
		}
	}
}

void FixedSizeAllocator::DisownBlocks() {
	for (auto &buffer : buffers) {
		buffer.second->block_pointer = BlockPointer();
	}
}

idx_t FixedSizeAllocator::GetInMemorySize() const {
	idx_t memory_usage = 0;
	for (auto &buffer : buffers) {
		if (buffer.second->InMemory()) {
			memory_usage += block_manager.GetBlockSize();
		}
	}
	return memory_usage;
}

idx_t FixedSizeAllocator::GetUpperBoundBufferId() const {
	idx_t upper_bound_id = 0;
	for (auto &buffer : buffers) {
		if (buffer.first >= upper_bound_id) {
			upper_bound_id = buffer.first + 1;
		}
	}
	return upper_bound_id;
}

void FixedSizeAllocator::Merge(FixedSizeAllocator &other) {
	D_ASSERT(segment_size == other.segment_size);

	// remember the buffer count and merge the buffers
	idx_t upper_bound_id = GetUpperBoundBufferId();
	for (auto &buffer : other.buffers) {
		buffers.insert(make_pair(buffer.first + upper_bound_id, std::move(buffer.second)));
	}
	other.buffers.clear();

	// merge the buffers with free spaces
	for (auto &buffer_id : other.buffers_with_free_space) {
		buffers_with_free_space.insert(buffer_id + upper_bound_id);
	}
	other.buffers_with_free_space.clear();
	NextBufferWithFreeSpace();

	// add the total allocations
	total_segment_count += other.total_segment_count;
}

bool FixedSizeAllocator::InitializeVacuum() {
	// NOTE: we do not vacuum buffers that are not in memory. We might consider changing this
	// in the future, although buffers on disk should almost never be eligible for a vacuum

	if (total_segment_count == 0) {
		Reset();
		return false;
	}
	RemoveEmptyBuffers();

	// determine if a vacuum is necessary
	multimap<idx_t, idx_t> temporary_vacuum_buffers;
	D_ASSERT(vacuum_buffers.empty());
	idx_t available_segments_in_memory = 0;

	for (auto &buffer : buffers) {
		buffer.second->vacuum = false;
		if (buffer.second->InMemory()) {
			auto available_segments_in_buffer = available_segments_per_buffer - buffer.second->segment_count;
			available_segments_in_memory += available_segments_in_buffer;
			temporary_vacuum_buffers.emplace(available_segments_in_buffer, buffer.first);
		}
	}

	// no buffers in memory
	if (temporary_vacuum_buffers.empty()) {
		return false;
	}

	auto excess_buffer_count = available_segments_in_memory / available_segments_per_buffer;

	// calculate the vacuum threshold adaptively
	D_ASSERT(excess_buffer_count < temporary_vacuum_buffers.size());
	idx_t memory_usage = GetInMemorySize();
	idx_t excess_memory_usage = excess_buffer_count * block_manager.GetBlockSize();
	auto excess_percentage = double(excess_memory_usage) / double(memory_usage);
	auto threshold = double(VACUUM_THRESHOLD) / 100.0;
	if (excess_percentage < threshold) {
		return false;
	}

	D_ASSERT(excess_buffer_count <= temporary_vacuum_buffers.size());
	D_ASSERT(temporary_vacuum_buffers.size() <= buffers.size());

	// erasing from a multimap, we vacuum the buffers with the most free spaces (least full)
	while (temporary_vacuum_buffers.size() != excess_buffer_count) {
		temporary_vacuum_buffers.erase(temporary_vacuum_buffers.begin());
	}

	// adjust the buffers, and erase all to-be-vacuumed buffers from the available buffer list
	for (auto &vacuum_buffer : temporary_vacuum_buffers) {
		auto buffer_id = vacuum_buffer.second;
		D_ASSERT(buffers.find(buffer_id) != buffers.end());
		buffers.find(buffer_id)->second->vacuum = true;
		buffers_with_free_space.erase(buffer_id);
	}
	D_ASSERT(!buffers_with_free_space.empty());
	NextBufferWithFreeSpace();

	for (auto &vacuum_buffer : temporary_vacuum_buffers) {
		vacuum_buffers.insert(vacuum_buffer.second);
	}
	return true;
}

void FixedSizeAllocator::FinalizeVacuum() {
	for (auto &buffer_id : vacuum_buffers) {
		D_ASSERT(buffers.find(buffer_id) != buffers.end());
		D_ASSERT(buffers.find(buffer_id)->second->InMemory());
		buffers.erase(buffer_id);
	}
	vacuum_buffers.clear();
}

IndexPointer FixedSizeAllocator::VacuumPointer(const IndexPointer old_ptr) {
	// we do not need to adjust the bitmask of the old buffer, because we will free the entire
	// buffer after the vacuum operation

	auto new_ptr = New();
	// new increases the allocation count, we need to counter that here
	total_segment_count--;

	auto old_handle = GetHandle(old_ptr);
	auto new_handle = GetHandle(new_ptr);
	new_handle.MarkModified();

	memcpy(new_handle.GetPtr(), old_handle.GetPtr(), segment_size);
	return new_ptr;
}

FixedSizeAllocatorInfo FixedSizeAllocator::GetInfo() const {
	FixedSizeAllocatorInfo info;
	info.segment_size = segment_size;

	for (const auto &[buffer_id, buffer] : buffers) {
		if (buffer->IsEmpty()) {
			continue;
		}

		info.buffer_ids.push_back(buffer_id);

		// Memory safety check.
		if (buffer_id > idx_t(MAX_ROW_ID)) {
			throw InternalException("Initializing invalid buffer ID in FixedSizeAllocator::GetInfo");
		}

		info.block_pointers.push_back(buffer->block_pointer);
		info.segment_counts.push_back(buffer->segment_count);
		info.allocation_sizes.push_back(buffer->allocation_size);

		if (buffers_with_free_space.find(buffer_id) != buffers_with_free_space.end()) {
			info.buffers_with_free_space.push_back(buffer_id);
		}
	}

	return info;
}

unsafe_unique_ptr<FixedSizeAllocator> FixedSizeAllocator::Persist(PartialBlockManager &partial_block_manager) {
	auto result = make_unsafe_uniq<FixedSizeAllocator>(segment_size, block_manager, memory_tag);

	D_ASSERT(result->available_segments_per_buffer == available_segments_per_buffer);
	D_ASSERT(result->bitmask_count == bitmask_count);
	D_ASSERT(result->bitmask_offset == bitmask_offset);

	result->total_segment_count = total_segment_count;

	for (auto &[buffer_id, buffer] : buffers) {
		if (buffer->IsEmpty()) {
			continue;
		}

		D_ASSERT(buffer->readers == 0);
		if (buffer->RequiresPersist()) {
			result->buffers[buffer_id] =
			    buffer->Persist(partial_block_manager, available_segments_per_buffer, segment_size, bitmask_offset);
		} else {
			result->buffers[buffer_id] = buffer;
		}

		if (buffers_with_free_space.find(buffer_id) != buffers_with_free_space.end()) {
			result->buffers_with_free_space.insert(buffer_id);
		}
	}

	result->NextBufferWithFreeSpace();

	return result;
}

vector<IndexBufferInfo> FixedSizeAllocator::InitSerializationToWAL() {
	vector<IndexBufferInfo> buffer_infos;
	for (auto &[buffer_id, buffer] : buffers) {
		if (buffer->IsEmpty()) {
			continue;
		}

		buffer->SetAllocationSize(available_segments_per_buffer, segment_size, bitmask_offset);

		// Get a handle to the buffer's memory (offset 0).
		SegmentHandle handle(*buffer, 0);
		buffer_infos.emplace_back(handle.GetPtr(), buffer->allocation_size);
	}
	return buffer_infos;
}

void FixedSizeAllocator::Init(const FixedSizeAllocatorInfo &info) {
	segment_size = info.segment_size;
	total_segment_count = 0;

	for (idx_t i = 0; i < info.buffer_ids.size(); i++) {
		// read all FixedSizeBuffer data
		auto buffer_id = info.buffer_ids[i];

		// Memory safety check.
		if (buffer_id > idx_t(MAX_ROW_ID)) {
			throw InternalException("Initializing invalid buffer ID in FixedSizeAllocator::Init");
		}

		auto buffer_block_pointer = info.block_pointers[i];
		if (buffer_block_pointer.block_id >= MAXIMUM_BLOCK) {
			throw SerializationException("invalid block ID in index storage information");
		}

		auto segment_count = info.segment_counts[i];
		auto allocation_size = info.allocation_sizes[i];

		// create the FixedSizeBuffer
		if (info.transient_block_handles) {
			D_ASSERT(info.transient_block_handles->size() == info.buffer_ids.size());
			buffers[buffer_id] = make_shared_ptr<FixedSizeBuffer>(block_manager, segment_count, allocation_size,
			                                                      std::move((*info.transient_block_handles)[i]));
		} else {
			buffers[buffer_id] =
			    make_shared_ptr<FixedSizeBuffer>(block_manager, segment_count, allocation_size, buffer_block_pointer);
		}
		total_segment_count += segment_count;
	}

	for (const auto &buffer_id : info.buffers_with_free_space) {
		buffers_with_free_space.insert(buffer_id);
	}
	NextBufferWithFreeSpace();
}

void FixedSizeAllocator::Deserialize(MetadataManager &metadata_manager, const BlockPointer &block_pointer) {
	MetadataReader reader(metadata_manager, block_pointer);
	segment_size = reader.Read<idx_t>();
	auto buffer_count = reader.Read<idx_t>();
	auto buffers_with_free_space_count = reader.Read<idx_t>();

	total_segment_count = 0;

	for (idx_t i = 0; i < buffer_count; i++) {
		auto buffer_id = reader.Read<idx_t>();
		auto buffer_block_pointer = reader.Read<BlockPointer>();
		auto segment_count = reader.Read<idx_t>();
		auto allocation_size = reader.Read<idx_t>();
		buffers[buffer_id] =
		    make_shared_ptr<FixedSizeBuffer>(block_manager, segment_count, allocation_size, buffer_block_pointer);
		total_segment_count += segment_count;
	}
	for (idx_t i = 0; i < buffers_with_free_space_count; i++) {
		buffers_with_free_space.insert(reader.Read<idx_t>());
	}
}

idx_t FixedSizeAllocator::GetAvailableBufferId() const {
	idx_t buffer_id = buffers.size();
	while (buffers.find(buffer_id) != buffers.end()) {
		D_ASSERT(buffer_id > 0);
		buffer_id--;
	}
	return buffer_id;
}

void FixedSizeAllocator::RemoveEmptyBuffers() {
	auto buffer_it = buffers.begin();
	while (buffer_it != buffers.end()) {
		if (buffer_it->second->segment_count != 0) {
			++buffer_it;
			continue;
		}
		buffers_with_free_space.erase(buffer_it->first);
		buffer_it = buffers.erase(buffer_it);
	}
	NextBufferWithFreeSpace();
}

void FixedSizeAllocator::VerifyBuffers() {
	idx_t count = 0;
	auto buffer_it = buffers.begin();
	while (buffer_it != buffers.end()) {
		if (buffer_it->second->segment_count == 0) {
			count++;
		}
		buffer_it++;
	}

	if (count > 1) {
		throw InternalException("expected one, but got %d empty buffers in allocator", count);
	}
}

void FixedSizeAllocator::NextBufferWithFreeSpace() {
	if (!buffers_with_free_space.empty()) {
		buffer_with_free_space = *buffers_with_free_space.begin();
		return;
	}
	buffer_with_free_space.SetInvalid();
}

} // namespace duckdb
