#include "duckdb/storage/compression/dictionary/compression.hpp"

namespace duckdb {

DictionaryCompressionCompressState::DictionaryCompressionCompressState(ColumnDataCheckpointData &checkpoint_data_p,
                                                                       const idx_t max_unique_count_across_all_segments)
    : StandardCompressionState(checkpoint_data_p, CompressionType::COMPRESSION_DICTIONARY),
      stats_writer(checkpoint_data.GetType()),
      current_string_map(
          info.GetBlockManager().buffer_manager.GetBufferAllocator(),
          max_unique_count_across_all_segments * 2, // * 2 results in less linear probing, improving performance
          1 // maximum_target_capacity_p, 1 because we don't care about target for our use-case, as we
            // only use PrimitiveDictionary for duplicate checks, and not for writing to any target
      ) {
	CreateEmptySegment();
}

void DictionaryCompressionCompressState::CreateEmptySegment() {
	CreateAndPinNewSegment();

	// Reset the buffers and the string map.
	stats_writer.Clear();
	current_string_map.Clear();
	index_buffer.clear();

	// Reserve index 0 for null strings.
	index_buffer.push_back(0);
	selection_buffer.clear();

	current_width = 0;
	next_width = 0;

	// Reset the pointers into the current segment.
	current_dictionary = DictionaryCompression::GetDictionary(*current_segment, handle);
	current_end_ptr = handle.GetDataMutable() + current_dictionary.end;
}

void DictionaryCompressionCompressState::Verify() {
	current_dictionary.Verify(info.GetBlockSize());
	D_ASSERT(current_segment->count == selection_buffer.size());
	D_ASSERT(DictionaryCompression::HasEnoughSpace(current_segment->count.load(), index_buffer.size(),
	                                               current_dictionary.size, current_width, info.GetBlockSize()));
	D_ASSERT(current_dictionary.end == info.GetBlockSize());
	D_ASSERT(index_buffer.size() == current_string_map.GetSize() + 1); // +1 is for null value
}

bool DictionaryCompressionCompressState::LookupString(string_t str) {
	const auto &entry = current_string_map.Lookup(str);
	const auto has_result = !entry.IsEmpty();
	if (has_result) {
		latest_lookup_result = entry.index + 1;
	}
	return has_result;
}

void DictionaryCompressionCompressState::AddNewString(string_t str) {
	stats_writer.Update(str);

	// Copy string to dict
	current_dictionary.size += str.GetSize();
	auto dict_pos = current_end_ptr - current_dictionary.size;
	memcpy(dict_pos, str.GetData(), str.GetSize());
	current_dictionary.Verify(info.GetBlockSize());
	D_ASSERT(current_dictionary.end == info.GetBlockSize());

	// Update buffers and map
	index_buffer.push_back(current_dictionary.size);
	selection_buffer.push_back(UnsafeNumericCast<uint32_t>(index_buffer.size() - 1));
	if (str.IsInlined()) {
		current_string_map.Insert(str);
	} else {
		string_t dictionary_string((const char *)dict_pos, UnsafeNumericCast<uint32_t>(str.GetSize())); // NOLINT
		D_ASSERT(!dictionary_string.IsInlined());
		current_string_map.Insert(dictionary_string);
	}
	DictionaryCompression::SetDictionary(*current_segment, handle, current_dictionary);

	current_width = next_width;
	current_segment->count++;
}

void DictionaryCompressionCompressState::AddNull() {
	stats_writer.SetHasNull();
	selection_buffer.push_back(0);
	current_segment->count++;
}

void DictionaryCompressionCompressState::AddLastLookup(string_t str) {
	// the string is already in the dictionary - only additive stats (total_string_length) need updating
	stats_writer.UpdateRepeated(str);
	selection_buffer.push_back(latest_lookup_result);
	current_segment->count++;
}

bool DictionaryCompressionCompressState::CalculateSpaceRequirements(bool new_string, idx_t string_size) {
	if (!new_string) {
		return DictionaryCompression::HasEnoughSpace(current_segment->count.load() + 1, index_buffer.size(),
		                                             current_dictionary.size, current_width, info.GetBlockSize());
	}
	next_width = BitpackingPrimitives::MinimumBitWidth(index_buffer.size() - 1 + new_string);
	return DictionaryCompression::HasEnoughSpace(current_segment->count.load() + 1, index_buffer.size() + 1,
	                                             current_dictionary.size + string_size, next_width,
	                                             info.GetBlockSize());
}

void DictionaryCompressionCompressState::Flush(bool final) {
	auto segment_size = Finalize();
	FlushCurrentSegment(stats_writer, segment_size);

	if (!final) {
		CreateEmptySegment();
	}
}

idx_t DictionaryCompressionCompressState::Finalize() {
	auto &buffer_manager = BufferManager::GetBufferManager(checkpoint_data.GetDatabase());
	auto handle = buffer_manager.Pin(current_segment->GetBlockHandle());
	D_ASSERT(current_dictionary.end == info.GetBlockSize());

	// calculate sizes
	auto compressed_selection_buffer_size =
	    BitpackingPrimitives::GetRequiredSize(current_segment->count, current_width);
	auto index_buffer_size = index_buffer.size() * sizeof(uint32_t);
	auto total_size = DictionaryCompression::DICTIONARY_HEADER_SIZE + compressed_selection_buffer_size +
	                  index_buffer_size + current_dictionary.size;

	// calculate ptr and offsets
	auto base_ptr = handle.GetDataMutable();
	auto header_ptr = reinterpret_cast<dictionary_compression_header_t *>(base_ptr);
	auto compressed_selection_buffer_offset = DictionaryCompression::DICTIONARY_HEADER_SIZE;
	auto index_buffer_offset = compressed_selection_buffer_offset + compressed_selection_buffer_size;

	// Write compressed selection buffer
	BitpackingPrimitives::PackBuffer<sel_t, false>(base_ptr + compressed_selection_buffer_offset,
	                                               (sel_t *)(selection_buffer.data()), current_segment->count,
	                                               current_width);

	// Write the index buffer
	memcpy(base_ptr + index_buffer_offset, index_buffer.data(), index_buffer_size);

	// Store sizes and offsets in segment header
	Store<uint32_t>(NumericCast<uint32_t>(index_buffer_offset), data_ptr_cast(&header_ptr->index_buffer_offset));
	Store<uint32_t>(NumericCast<uint32_t>(index_buffer.size()), data_ptr_cast(&header_ptr->index_buffer_count));
	Store<uint32_t>((uint32_t)current_width, data_ptr_cast(&header_ptr->bitpacking_width));

	D_ASSERT(current_width == BitpackingPrimitives::MinimumBitWidth(index_buffer.size() - 1));
	D_ASSERT(DictionaryCompression::HasEnoughSpace(current_segment->count, index_buffer.size(), current_dictionary.size,
	                                               current_width, info.GetBlockSize()));
	// an empty segment (e.g. the child of a column of empty lists) has no selection entries
	D_ASSERT(selection_buffer.empty() || (uint64_t)*max_element(std::begin(selection_buffer),
	                                                            std::end(selection_buffer)) == index_buffer.size() - 1);

	// Early-out, if the block is sufficiently full.
	if (total_size >= info.GetCompactionFlushLimit()) {
		return info.GetBlockSize();
	}

	// Sufficient space: calculate how much space we can save.
	auto move_amount = info.GetBlockSize() - total_size;

	// Move the dictionary to align it with the offsets.
	auto new_dictionary_offset = index_buffer_offset + index_buffer_size;
	memmove(base_ptr + new_dictionary_offset, base_ptr + current_dictionary.end - current_dictionary.size,
	        current_dictionary.size);
	current_dictionary.end -= move_amount;
	D_ASSERT(current_dictionary.end == total_size);

	// Write the new dictionary with the updated "end".
	DictionaryCompression::SetDictionary(*current_segment, handle, current_dictionary);
	return total_size;
}

} // namespace duckdb
