#pragma once

#include "duckdb/storage/compression/compression_segment_reader.hpp"
#include "duckdb/storage/compression/dictionary/common.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Scan
//===--------------------------------------------------------------------===//
struct CompressedStringScanState : public SegmentScanState {
private:
	//! Dictionary segment data from disk, with byte ranges checked by ReadLayout.
	struct SegmentLayout {
		//! Bits per dictionary index (derived from the entry count and checked against the stored width)
		bitpacking_width_t current_width;
		//! Packed indices, mapping each row to a dictionary entry.
		CompressionSegmentReader selection_reader;
		//! String contents stored backwards from dict_end.
		CompressionSegmentReader dictionary_reader;
		//! Maps dictionary indices to byte offsets measured backwards from dict_end.
		//! Consecutive offsets determine each (non NULL) string's length.
		unsafe_array_ptr<const uint32_t> index_buffer;
	};

public:
	explicit CompressedStringScanState(BufferHandle &&handle_p, const ColumnSegment &segment)
	    : owned_handle(std::move(handle_p)), layout(ReadLayout(owned_handle, segment)) {
	}

public:
	void InitializeDictionary(const ColumnSegment &segment);
	//! Scans require InitializeDictionary, COPY_STRINGS scans validate their selected entries instead.
	template <bool NEEDS_STRING_OFFSET_CHECK = false>
	void ScanToFlatVector(Vector &result, idx_t result_offset, idx_t start, idx_t scan_count,
	                      StringScanType string_scan_type = StringScanType::STANDARD);
	void ScanToDictionaryVector(ColumnSegment &segment, Vector &result, idx_t result_offset, idx_t start,
	                            idx_t scan_count);

private:
	//! The offset must be within the dictionary and the length must not extend past its end.
	string_t FetchStringFromDict(uint32_t dict_offset, uint32_t string_len) const;
	//! The index must be within the table and the offsets must be nondecreasing to avoid underflow.
	uint32_t GetStringLength(idx_t index) const;
	//! Checks each selected dictionary index against the offset table.
	//! The selection must be set and the requested range must fit within its capacity.
	void ValidateDictionaryIndices(const SelectionVector &sel, idx_t start_offset, idx_t scan_count) const;
	//! Validate the index buffer (offsets must be monotonic and within the dictionary) so scans can trust it.
	void ValidateIndexBuffer() const;
	static SegmentLayout ReadLayout(const BufferHandle &handle, const ColumnSegment &segment);
	//! Returns packed bytes starting at the group containing start.
	//! decompress_count must cover whole bitpacking groups that fit within the selection buffer.
	unsafe_array_ptr<const uint8_t> GetSelectionBytes(idx_t start, idx_t decompress_count) const;

public:
	BufferHandle owned_handle;

	SegmentLayout layout;
	buffer_ptr<SelectionVector> sel_vec;
	idx_t sel_vec_size = 0;

	buffer_ptr<DictionaryEntry> dictionary;
};

} // namespace duckdb
