#include "duckdb/common/types/row/tuple_data_iterator.hpp"

#include "duckdb/common/types/row/tuple_data_allocator.hpp"

namespace duckdb {

TupleDataChunkIterator::TupleDataChunkIterator(TupleDataCollection &collection_p, TupleDataPinProperties properties_p,
                                               bool init_heap)
    : TupleDataChunkIterator(collection_p, properties_p, 0, collection_p.ChunkCount(), init_heap) {
}

TupleDataChunkIterator::TupleDataChunkIterator(TupleDataCollection &collection_p, TupleDataPinProperties properties,
                                               idx_t chunk_idx_from, idx_t chunk_idx_to, bool init_heap_p)
    : collection(collection_p), init_heap(init_heap_p) {
	state.pin_state.properties = properties;
	D_ASSERT(chunk_idx_from < chunk_idx_to);
	D_ASSERT(chunk_idx_to <= collection.ChunkCount());
	idx_t overall_chunk_index = 0;
	for (idx_t segment_idx = 0; segment_idx < collection.segments.size(); segment_idx++) {
		const auto &segment = *collection.segments[segment_idx];
		if (chunk_idx_from >= overall_chunk_index && chunk_idx_from <= overall_chunk_index + segment.ChunkCount()) {
			// We start in this segment
			start_segment_idx = segment_idx;
			start_chunk_idx = chunk_idx_from - overall_chunk_index;
		}
		if (chunk_idx_to >= overall_chunk_index && chunk_idx_to <= overall_chunk_index + segment.ChunkCount()) {
			// We end in this segment
			end_segment_idx = segment_idx;
			end_chunk_idx = chunk_idx_to - overall_chunk_index;
		}
		overall_chunk_index += segment.ChunkCount();
	}

	Reset();
}

TupleDataChunkIterator::TupleDataChunkIterator(TupleDataCollection &collection_p, TupleDataPinProperties properties,
                                               const TupleDataChunkPosition &position, bool init_heap_p)
    : collection(collection_p), init_heap(init_heap_p) {
	state.pin_state.properties = properties;
	D_ASSERT(position.segment_index < collection.segments.size());
	const auto &segment = *collection.segments[position.segment_index];
	D_ASSERT(position.chunk_index < segment.ChunkCount());
	start_segment_idx = position.segment_index;
	start_chunk_idx = position.chunk_index;
	// the end is the next chunk: within this segment, at the start of the next one, or past the last chunk
	if (position.chunk_index + 1 < segment.ChunkCount()) {
		end_segment_idx = position.segment_index;
		end_chunk_idx = position.chunk_index + 1;
	} else if (position.segment_index + 1 < collection.segments.size()) {
		end_segment_idx = position.segment_index + 1;
		end_chunk_idx = 0;
	} else {
		end_segment_idx = position.segment_index;
		end_chunk_idx = segment.ChunkCount();
	}

	Reset();
}

void TupleDataChunkIterator::InitializeCurrentChunk() {
	auto &segment = *collection.segments[current_segment_idx];
	segment.allocator->InitializeChunkState(segment, state.pin_state, state.chunk_state, current_chunk_idx, init_heap);
}

bool TupleDataChunkIterator::Done() const {
	return current_segment_idx == end_segment_idx && current_chunk_idx == end_chunk_idx;
}

bool TupleDataChunkIterator::Next() {
	D_ASSERT(!Done()); // Check if called after already done

	// Set the next indices and checks if we're at the end of the collection
	// NextScanIndex can go past this iterators 'end', so we have to check the indices again
	const auto segment_idx_before = current_segment_idx;
	if (!collection.NextScanIndex(state, current_segment_idx, current_chunk_idx) || Done()) {
		// Drop pins / stores them if TupleDataPinProperties::KEEP_EVERYTHING_PINNED
		collection.FinalizePinState(state.pin_state, *collection.segments[segment_idx_before]);
		current_segment_idx = end_segment_idx;
		current_chunk_idx = end_chunk_idx;
		return false;
	}

	// Finalize pin state when moving from one segment to the next
	if (current_segment_idx != segment_idx_before) {
		collection.FinalizePinState(state.pin_state, *collection.segments[segment_idx_before]);
	}

	InitializeCurrentChunk();
	return true;
}

void TupleDataChunkIterator::Reset() {
	state.segment_index = start_segment_idx;
	state.chunk_index = start_chunk_idx;
	collection.NextScanIndex(state, current_segment_idx, current_chunk_idx);
	InitializeCurrentChunk();
}

idx_t TupleDataChunkIterator::GetCurrentChunkCount() const {
	return collection.segments[current_segment_idx]->chunks[current_chunk_idx]->count;
}

TupleDataChunkState &TupleDataChunkIterator::GetChunkState() {
	return state.chunk_state;
}

data_ptr_t *TupleDataChunkIterator::GetRowLocations() {
	return FlatVector::GetDataMutable<data_ptr_t>(state.chunk_state.row_locations);
}

data_ptr_t *TupleDataChunkIterator::GetHeapLocations() {
	return FlatVector::GetDataMutable<data_ptr_t>(state.chunk_state.heap_locations);
}

idx_t *TupleDataChunkIterator::GetHeapSizes() {
	return FlatVector::GetDataMutable<idx_t>(state.chunk_state.heap_sizes);
}

TupleDataChunkCursor::TupleDataChunkCursor(const TupleDataCollection &collection_p)
    : collection(collection_p), started(false) {
}

bool TupleDataChunkCursor::Next() {
	auto &segments = collection.segments;
	if (!started) {
		started = true;
		position.segment_index = 0;
		position.chunk_index = 0;
	} else if (position.segment_index < segments.size()) {
		position.chunk_index++;
	}
	while (position.segment_index < segments.size() &&
	       position.chunk_index >= segments[position.segment_index]->ChunkCount()) {
		position.segment_index++;
		position.chunk_index = 0;
	}
	return position.segment_index < segments.size();
}

TupleDataChunkPosition TupleDataChunkCursor::Position() const {
	return position;
}

idx_t TupleDataChunkCursor::Count() const {
	return collection.segments[position.segment_index]->chunks[position.chunk_index]->count;
}

idx_t TupleDataChunkCursor::HeapSize() const {
	return collection.segments[position.segment_index]->ChunkHeapSize(position.chunk_index);
}

} // namespace duckdb
