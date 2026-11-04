#include "duckdb/storage/compression/dictionary/decompression.hpp"
#include "duckdb/common/vector/dictionary_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Dictionary Validation
//===--------------------------------------------------------------------===//
[[noreturn]] static void ThrowDictionaryIndexOutOfRange() {
	throw DataCorruptionException("Failed to scan dictionary string - dictionary index was out of range. Database file "
	                              "appears to be corrupted.");
}

[[noreturn]] static void ThrowDictionaryOffsetOutOfRange() {
	throw DataCorruptionException("Failed to scan dictionary string - dictionary offset was out of range. Database "
	                              "file appears to be corrupted.");
}

void CompressedStringScanState::ValidateDictionaryIndices(const SelectionVector &sel, const idx_t start_offset,
                                                          const idx_t scan_count) const {
	D_ASSERT(sel.IsSet());
	D_ASSERT(start_offset <= sel.Capacity());
	D_ASSERT(scan_count <= sel.Capacity() - start_offset);
	bool has_error = false;
	for (idx_t i = 0; i < scan_count; i++) {
		const idx_t sel_idx = sel.get_index_unsafe(i + start_offset);
		has_error |= sel_idx >= layout.index_buffer.size();
	}

	if (has_error) {
		ThrowDictionaryIndexOutOfRange();
	}
}

void CompressedStringScanState::ValidateIndexBuffer() const {
	// Only the checks required to avoid out-of-bounds reads when trusting the buffer: offsets must be
	// monotonically increasing (else a length underflows) and the largest offset must lie within the dictionary.
	bool has_error = false;
	for (idx_t i = 1; i < layout.index_buffer.size(); i++) {
		has_error |= layout.index_buffer[i] < layout.index_buffer[i - 1];
	}
	has_error |= layout.index_buffer[layout.index_buffer.size() - 1] > layout.dictionary_reader.Size();

	if (has_error) {
		ThrowDictionaryOffsetOutOfRange();
	}
}

//===--------------------------------------------------------------------===//
// String Reading
//===--------------------------------------------------------------------===//
uint32_t CompressedStringScanState::GetStringLength(idx_t index) const {
	D_ASSERT(index < layout.index_buffer.size());
	if (index == 0) {
		return 0;
	}
	D_ASSERT(layout.index_buffer[index] >= layout.index_buffer[index - 1]);
	// Offsets are validated up front by ValidateIndexBuffer, so the length can be read directly.
	const auto string_length = layout.index_buffer[index] - layout.index_buffer[index - 1];
	return string_length;
}

string_t CompressedStringScanState::FetchStringFromDict(uint32_t dict_offset, uint32_t string_len) const {
	D_ASSERT(dict_offset <= layout.dictionary_reader.Size());
	D_ASSERT(string_len <= dict_offset);
	if (dict_offset == 0) {
		return string_t(nullptr, 0);
	}

	// normal string: read string from this block
	auto string_data = layout.dictionary_reader.GetBytes(layout.dictionary_reader.Size() - dict_offset, string_len);
	return string_t(const_char_ptr_cast(string_data.data()), string_len);
}

//===--------------------------------------------------------------------===//
// Segment Layout
//===--------------------------------------------------------------------===//
CompressedStringScanState::SegmentLayout CompressedStringScanState::ReadLayout(const BufferHandle &handle,
                                                                               const ColumnSegment &segment) {
	auto reader = CompressionSegmentReader::FromSegment(handle, segment, "dictionary segment");
	auto header = reader.Read<dictionary_compression_header_t>();
	// Index zero represents NULL, so even an all-NULL segment needs one dictionary offset.
	if (header.index_buffer_count == 0) {
		throw DataCorruptionException(
		    "Failed to scan dictionary string - dictionary was out of range. Database file appears to be corrupted.");
	}
	// Selections refer to entries in the offset table, so the last index determines the bit width.
	auto expected_width = BitpackingPrimitives::MinimumBitWidth(header.index_buffer_count - 1);
	if (header.bitpacking_width != expected_width) {
		throw DataCorruptionException(
		    "Failed to scan dictionary string - bitpacking width was invalid. Database file appears to be "
		    "corrupted.");
	}
	// The offset table must start at (or after) the header's end.
	if (header.index_buffer_offset < reader.Position()) {
		throw DataCorruptionException(
		    "Failed to scan dictionary string - selection buffer was out of range. Database file appears "
		    "to be corrupted.");
	}
	auto selection_reader =
	    reader.ReadSubReader(header.index_buffer_offset - reader.Position(), "dictionary selections");

	constexpr auto group_size = BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE;
	const auto row_count = segment.count.load();
	// Count the final partial group without rounding up the row count, which could overflow.
	const auto group_count = row_count / group_size + (row_count % group_size != 0);
	const auto group_bytes = group_size * expected_width / 8;
	if (group_bytes == 0) {
		// With only the NULL entry to select, no bits are encoded.
		// The selection reader must therefore be empty, otherwise its size conflicts with the zero bit width.
		if (selection_reader.Size() != 0) {
			throw DataCorruptionException(
			    "Failed to scan dictionary string - selection buffer was out of range. Database file appears "
			    "to be corrupted.");
		}
	} else {
		// Require exactly enough whole groups for the rows. Compare by division to avoid overflowing.
		if (selection_reader.Size() % group_bytes != 0 || selection_reader.Size() / group_bytes != group_count) {
			throw DataCorruptionException(
			    "Failed to scan dictionary string - selection buffer was out of range. Database file appears "
			    "to be corrupted.");
		}
	}

	// Check that the offset table is aligned and fits within the segment.
	auto index_buffer = reader.GetArray<uint32_t>(header.index_buffer_offset, header.index_buffer_count);
	auto index_buffer_end = header.index_buffer_offset + sizeof(uint32_t) * index_buffer.size();
	// Dictionary bytes grow backwards from dict_end and must not overlap the offset table.
	if (header.dict_size > header.dict_end || header.dict_end - header.dict_size < index_buffer_end) {
		throw DataCorruptionException(
		    "Failed to scan dictionary string - dictionary was out of range. Database file appears to be corrupted.");
	}
	// Check that the dictionary fits within the reader, then restrict string reads to its bytes.
	auto dictionary_reader =
	    reader.GetSubReader(header.dict_end - header.dict_size, header.dict_size, "dictionary strings");
	return {expected_width, selection_reader, dictionary_reader, index_buffer};
}

//===--------------------------------------------------------------------===//
// Scan
//===--------------------------------------------------------------------===//
void CompressedStringScanState::InitializeDictionary(const ColumnSegment &segment) {
	// Validate the whole index buffer once so the dictionary build below can trust it.
	ValidateIndexBuffer();

	dictionary = DictionaryVector::CreateReusableDictionary(segment.GetType(), layout.index_buffer.size());
	auto dict_child_data = FlatVector::Writer<string_t>(dictionary->data, layout.index_buffer.size());
	// A separate validity scan can mark a row selecting index zero valid, so initialize its string slot.
	dict_child_data.WriteStringRef(string_t(nullptr, 0));
	FlatVector::SetNull(dictionary->data, 0, true);
	for (idx_t i = 1; i < layout.index_buffer.size(); i++) {
		const auto str_len = GetStringLength(i);
		dict_child_data.WriteStringRef(FetchStringFromDict(layout.index_buffer[i], str_len));
	}
}

template <bool NEEDS_STRING_OFFSET_CHECK>
void CompressedStringScanState::ScanToFlatVector(Vector &result, idx_t result_offset, idx_t start, idx_t scan_count,
                                                 StringScanType string_scan_type) {
	D_ASSERT(NEEDS_STRING_OFFSET_CHECK || dictionary);
	if (NEEDS_STRING_OFFSET_CHECK) {
		D_ASSERT(result.GetVectorType() == VectorType::FLAT_VECTOR);
		D_ASSERT(result_offset < FlatVector::GetCapacity(result));
	}
	// Handling non-bitpacking-group-aligned start values;
	idx_t start_offset = start % BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE;

	// We will scan in blocks of BITPACKING_ALGORITHM_GROUP_SIZE, so we may scan some extra values.
	idx_t decompress_count = BitpackingPrimitives::RoundUpToAlgorithmGroupSize(scan_count + start_offset);

	// Create a decompression buffer of sufficient size if we don't already have one.
	if (!sel_vec || sel_vec_size < decompress_count) {
		sel_vec_size = decompress_count;
		sel_vec = make_buffer<SelectionVector>(decompress_count);
	}

	D_ASSERT(decompress_count <= sel_vec->Capacity());
	auto src = GetSelectionBytes(start, decompress_count);
	sel_t *sel_vec_ptr = sel_vec->data();

	BitpackingPrimitives::UnPackBuffer<sel_t>(data_ptr_cast(sel_vec_ptr), src.data(), decompress_count,
	                                          layout.current_width);

	auto result_data = FlatVector::Writer<string_t>(result, scan_count, result_offset);

	bool has_index_error = false;
	bool has_offset_error = false;
	for (idx_t i = 0; i < scan_count; i++) {
		// Lookup dict offset in index buffer
		auto string_dict_index = sel_vec->get_index(i + start_offset);

		const bool index_error = string_dict_index >= layout.index_buffer.size();
		has_index_error |= index_error;
		string_dict_index = index_error ? 0 : string_dict_index;
		auto str_dict_offset = layout.index_buffer[string_dict_index];

		if (NEEDS_STRING_OFFSET_CHECK) {
			bool offset_error = str_dict_offset > layout.dictionary_reader.Size();
			if (string_dict_index > 0) {
				offset_error |= str_dict_offset < layout.index_buffer[string_dict_index - 1];
			}
			has_offset_error |= offset_error;
			// On error, fall back to index/offset 0 so the fetch below stays in bounds.
			string_dict_index = offset_error ? 0 : string_dict_index;
			str_dict_offset = offset_error ? 0 : str_dict_offset;
		}

		const auto str_len = GetStringLength(string_dict_index);
		auto str = FetchStringFromDict(str_dict_offset, str_len);
		if (string_scan_type == StringScanType::COPY_STRINGS) {
			result_data.WriteValue(str);
		} else {
			result_data.WriteStringRef(str);
		}
	}

	// offsets first, as scans validate the whole index buffer before the indices
	if (has_offset_error) {
		ThrowDictionaryOffsetOutOfRange();
	}
	if (has_index_error) {
		ThrowDictionaryIndexOutOfRange();
	}
}

template void CompressedStringScanState::ScanToFlatVector<false>(Vector &result, idx_t result_offset, idx_t start,
                                                                 idx_t scan_count, StringScanType string_scan_type);
template void CompressedStringScanState::ScanToFlatVector<true>(Vector &result, idx_t result_offset, idx_t start,
                                                                idx_t scan_count, StringScanType string_scan_type);

void CompressedStringScanState::ScanToDictionaryVector(ColumnSegment &segment, Vector &result, idx_t result_offset,
                                                       idx_t start, idx_t scan_count) {
	D_ASSERT(scan_count == STANDARD_VECTOR_SIZE);
	D_ASSERT(result_offset == 0);

	idx_t start_offset = start % BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE;
	idx_t decompress_count = BitpackingPrimitives::RoundUpToAlgorithmGroupSize(scan_count + start_offset);

	// Create a selection vector of sufficient size if we don't already have one.
	if (!sel_vec || sel_vec_size < decompress_count) {
		sel_vec_size = decompress_count;
		sel_vec = make_buffer<SelectionVector>(decompress_count);
	}

	// Scanning 2048 values, emitting a dict vector
	data_ptr_t dst = data_ptr_cast(sel_vec->data());
	auto src = GetSelectionBytes(start, decompress_count);

	BitpackingPrimitives::UnPackBuffer<sel_t>(dst, src.data(), decompress_count, layout.current_width);

	sel_vec->ShiftLeft(start_offset, scan_count);
	ValidateDictionaryIndices(*sel_vec, 0, scan_count);

	result.Dictionary(dictionary, *sel_vec, scan_count);
}

unsafe_array_ptr<const uint8_t> CompressedStringScanState::GetSelectionBytes(idx_t start,
                                                                             idx_t decompress_count) const {
	const auto group_size = BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE;
	const auto group_bytes = group_size * layout.current_width / 8;
	D_ASSERT(decompress_count % group_size == 0);
	D_ASSERT(group_bytes == 0 || start / group_size <= layout.selection_reader.Size() / group_bytes);
	// Start at the group containing the requested row.
	// Divide before multiplying to avoid overflowing start * current_width.
	const auto source_offset = (start / group_size) * group_bytes;
	D_ASSERT(group_bytes == 0 ||
	         decompress_count / group_size <= (layout.selection_reader.Size() - source_offset) / group_bytes);
	const auto source_size = BitpackingPrimitives::GetRequiredSize(decompress_count, layout.current_width);
	return layout.selection_reader.GetBytes(source_offset, source_size);
}

} // namespace duckdb
