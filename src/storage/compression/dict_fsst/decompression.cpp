#include "duckdb/storage/compression/dict_fsst/decompression.hpp"

#include "duckdb/common/fsst.hpp"
#include "duckdb/common/vector/dictionary_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/storage/table/column_segment.hpp"
#include "fsst.h"

namespace duckdb {
namespace dict_fsst {

[[noreturn]] static void ThrowPackedValuesOutOfBounds(const char *context) {
	throw DataCorruptionException("Corrupted %s: packed values exceed the available space", context);
}

[[noreturn]] static void ThrowInvalidMode(DictFSSTMode mode) {
	throw DataCorruptionException("Corrupted DICT_FSST segment: unrecognized compression mode %d",
	                              static_cast<uint8_t>(mode));
}

[[noreturn]] static void ThrowInvalidDictionaryCount() {
	throw DataCorruptionException("Corrupted DICT_FSST segment: dictionary has no NULL entry");
}

[[noreturn]] static void ThrowZeroWidthDictionaryTooLarge() {
	throw DataCorruptionException(
	    "Corrupted DICT_FSST segment: zero-width lengths allow at most two dictionary entries");
}

[[noreturn]] static void ThrowDictionaryIndexOutOfBounds() {
	throw DataCorruptionException("Corrupted DICT_FSST segment: dictionary index is out of range");
}

[[noreturn]] static void ThrowInvalidNullLength() {
	throw DataCorruptionException("Corrupted DICT_FSST segment: NULL dictionary entry has a nonzero length");
}

[[noreturn]] static void ThrowStringLengthsOutOfBounds() {
	throw DataCorruptionException("Corrupted DICT_FSST segment: string lengths exceed the dictionary size");
}

[[noreturn]] static void ThrowInvalidBitpackingWidth() {
	throw DataCorruptionException("Failed to scan dictionary string - bitpacking width was invalid. Database file "
	                              "appears to be corrupted.");
}

[[noreturn]] static void ThrowInvalidSymbolTable() {
	throw DataCorruptionException("Failed to scan DICT_FSST string segment: invalid FSST symbol table. Database file "
	                              "appears to be corrupted.");
}

static unsafe_array_ptr<const uint8_t> ReadBitpackedValues(CompressionSegmentReader &reader, idx_t count,
                                                           bitpacking_width_t width, const char *context) {
	constexpr auto group_size = BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE;
	const auto group_count = count / group_size + (count % group_size != 0);
	const auto group_bytes = group_size * idx_t(width) / 8;
	// Check the number of groups before multiplying, so a stored count cannot overflow the byte size.
	if (group_bytes && group_count > reader.Remaining() / group_bytes) {
		ThrowPackedValuesOutOfBounds(context);
	}
	// The unpacker reads uint32_t words, even when the segment starts at a nonzero block offset.
	return reader.ReadBytesAligned<uint32_t>(group_count * group_bytes);
}

CompressedStringScanState::~CompressedStringScanState() {
	delete reinterpret_cast<duckdb_fsst_decoder_t *>(decoder);
}

string_t CompressedStringScanState::FetchStringFromDict(Vector &result, uint32_t dict_offset, idx_t dict_idx,
                                                        StringScanType string_scan_type) {
	D_ASSERT(dict_idx < layout.dict_count);
	if (dict_idx == 0) {
		return string_t(nullptr, 0);
	}
	uint32_t string_len = GetStringLength(dict_idx);

	// normal string: read string from this block
	auto string_data = layout.dictionary.GetBytes(dict_offset, string_len);

	auto str_ptr = const_char_ptr_cast(string_data.data());
	switch (layout.mode) {
	case DictFSSTMode::FSST_ONLY:
	case DictFSSTMode::DICT_FSST: {
		if (string_len == 0) {
			return string_t(nullptr, 0);
		}
		D_ASSERT(decoder);
		D_ASSERT(string_len <= string_t::MAX_STRING_SIZE / 8);
		if (all_values_inlined) {
			return FSSTPrimitives::DecompressInlinedValue(decoder, str_ptr, string_len);
		} else {
			return FSSTPrimitives::DecompressValue(decoder, StringVector::GetStringAllocator(result), str_ptr,
			                                       string_len);
		}
	}
	default:
		if (string_scan_type == StringScanType::COPY_STRINGS) {
			return StringVector::AddStringOrBlob(result, string_t(str_ptr, string_len));
		}
		// FIXME: the Vector doesn't seem to take ownership of the non-inlined string data???
		return string_t(str_ptr, string_len);
	}
}

CompressedStringScanState::SegmentLayout CompressedStringScanState::ReadLayout(const BufferHandle &handle,
                                                                               const ColumnSegment &segment) {
	auto reader = CompressionSegmentReader::FromSegment(handle, segment, "DICT_FSST segment");

	// Load header values
	const auto header = reader.Read<dict_fsst_compression_header_t>();
	if (header.mode >= DictFSSTMode::COUNT) {
		ThrowInvalidMode(header.mode);
	}
	if (header.dict_count == 0) {
		ThrowInvalidDictionaryCount();
	}
	// FSST_ONLY maps row i to dictionary entry i + 1, skipping the NULL entry.
	if (header.mode == DictFSSTMode::FSST_ONLY && segment.count.load() > header.dict_count - 1) {
		ThrowDictionaryIndexOutOfBounds();
	}
	if (header.dictionary_indices_width > sizeof(uint32_t) * 8 || header.string_lengths_width > sizeof(uint32_t) * 8) {
		ThrowInvalidBitpackingWidth();
	}
	if (header.string_lengths_width == 0) {
		// FSST segments retain nonempty training strings, so their lengths cannot all be zero.
		if (header.mode != DictFSSTMode::DICTIONARY) {
			ThrowInvalidBitpackingWidth();
		}
		// A zero-width dictionary has only NULL and possibly the empty string.
		if (header.dict_count > 2) {
			ThrowZeroWidthDictionaryTooLarge();
		}
	}

	reader.Align(sizeof(idx_t));
	auto dictionary = reader.ReadSubReader(header.dict_size, "DICT_FSST dictionary");
	reader.Align(sizeof(idx_t));
	auto symbol_table = reader.ReadSubReader(header.symbol_table_size, "DICT_FSST symbol table");
	reader.Align(sizeof(idx_t));
	auto lengths = ReadBitpackedValues(reader, header.dict_count, header.string_lengths_width, "DICT_FSST lengths");
	reader.Align(sizeof(idx_t));
	auto indices = ReadBitpackedValues(reader, segment.count.load(), header.dictionary_indices_width,
	                                   "DICT_FSST dictionary indices");
	return {header.mode,
	        header.dict_count,
	        header.dictionary_indices_width,
	        header.string_lengths_width,
	        dictionary,
	        symbol_table,
	        lengths,
	        indices};
}

void CompressedStringScanState::Initialize() {
	switch (layout.mode) {
	case DictFSSTMode::FSST_ONLY:
	case DictFSSTMode::DICT_FSST: {
		decoder = new duckdb_fsst_decoder_t;
		// in FSST_ONLY / DICT_FSST modes a symbol table is always present, so any failure (out-of-bounds or a
		// version mismatch) means the segment is corrupted
		auto symbol_table = layout.symbol_table.GetBytes(0, layout.symbol_table.Size());
		auto ret = duckdb_fsst_import(reinterpret_cast<duckdb_fsst_decoder_t *>(decoder), symbol_table.data(),
		                              symbol_table.size());
		if (ret == DUCKDB_FSST_IMPORT_VERSION_MISMATCH || ret == DUCKDB_FSST_IMPORT_OUT_OF_BOUNDS) {
			ThrowInvalidSymbolTable();
		}
		break;
	}
	default:
		break;
	}
}

void CompressedStringScanState::LoadStringLengths() {
	if (!string_lengths.empty()) {
		return;
	}
	constexpr auto group_size = BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE;
	string_lengths.resize(AlignValue<idx_t, group_size>(layout.dict_count));

	// Both the entry count and each length fit in uint32_t, so their sum fits in uint64_t.
	uint64_t total_bytes = 0;
	for (idx_t i = 0; i < layout.dict_count; i += group_size) {
		BitpackingPrimitives::UnPackBlock<uint32_t>(
		    data_ptr_cast(string_lengths.data() + i),
		    layout.string_lengths.data() + (i * layout.string_lengths_width) / 8, layout.string_lengths_width);

		// Validate only real entries, not the final group's padding.
		const auto end = MinValue<idx_t>(i + group_size, layout.dict_count);
		uint64_t group_bytes = 0;
		for (idx_t j = i; j < end; j++) {
			group_bytes += string_lengths[j];
		}

		// Each string offset sums earlier lengths, so bounding the total keeps all offsets in range.
		total_bytes += group_bytes;
		if (total_bytes > layout.dictionary.Size()) {
			ThrowStringLengthsOutOfBounds();
		}
	}

	// Index 0 represents NULL, so its length must be 0
	if (string_lengths[0] != 0) {
		ThrowInvalidNullLength();
	}
}

const uint32_t *CompressedStringScanState::GetStringLengthGroup(idx_t group_start) {
	D_ASSERT(group_start % BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE == 0 &&
	         group_start < layout.dict_count);
	if (string_length_group_start != group_start) {
		BitpackingPrimitives::UnPackBlock<uint32_t>(data_ptr_cast(string_length_group),
		                                            layout.string_lengths.data() +
		                                                (group_start * layout.string_lengths_width) / 8,
		                                            layout.string_lengths_width);
		string_length_group_start = group_start;
	}
	return string_length_group;
}

uint32_t CompressedStringScanState::GetStringLength(idx_t dict_idx) {
	if (!string_lengths.empty()) {
		return string_lengths[dict_idx];
	}
	const auto group_offset = dict_idx % BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE;
	return GetStringLengthGroup(dict_idx - group_offset)[group_offset];
}

idx_t CompressedStringScanState::SumStringLengths(idx_t start, idx_t end) {
	idx_t sum = 0;
	if (!string_lengths.empty()) {
		const auto lengths = string_lengths.data();
		for (auto i = start; i < end; i++) {
			sum += lengths[i];
		}
		return sum;
	}
	constexpr idx_t GROUP_SIZE = BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE;
	for (auto group_start = start - start % GROUP_SIZE; group_start < end; group_start += GROUP_SIZE) {
		const auto lengths = GetStringLengthGroup(group_start);
		const auto to = MinValue<idx_t>(end - group_start, GROUP_SIZE);
		for (auto i = MaxValue<idx_t>(start, group_start) - group_start; i < to; i++) {
			sum += lengths[i];
		}
	}
	return sum;
}

bool CompressedStringScanState::EnsureDictionary() {
	if (dictionary || layout.mode == DictFSSTMode::FSST_ONLY) {
		return dictionary != nullptr;
	}
	LoadStringLengths();
	dictionary = DictionaryVector::CreateReusableDictionary(segment.GetType(), layout.dict_count);
	auto &dict_data = dictionary->data;
	auto dict_child_data = FlatVector::GetDataMutable<string_t>(dict_data);
	auto &validity = FlatVector::ValidityMutable(dict_data);
	D_ASSERT(layout.dict_count >= 1);
	validity.SetInvalid(0);

	uint32_t offset = 0;
	for (uint32_t i = 0; i < layout.dict_count; i++) {
		//! We can uncompress during fetching, we need the length of the string inside the dictionary
		auto string_len = string_lengths[i];
		dict_child_data[i] = FetchStringFromDict(dict_data, offset, i);
		offset += string_len;
	}
	return true;
}

uint32_t CompressedStringScanState::GetStringOffset(idx_t string_number, bool allow_restart) {
	D_ASSERT(string_number < layout.dict_count);
	if (!string_offsets.empty()) {
		return string_offsets[string_number];
	}
	if (string_number < decompress_position) {
		if (!allow_restart || summed_lengths >= layout.dict_count) {
			LoadStringLengths();
			string_offsets.resize(layout.dict_count);
			uint32_t offset = 0;
			for (uint32_t i = 0; i < layout.dict_count; i++) {
				string_offsets[i] = offset;
				offset += string_lengths[i];
			}
			return string_offsets[string_number];
		}
		decompress_position = 0;
		decompress_offset = 0;
	}
	// lengths read group by group are not validated up front, so bound each offset as LoadStringLengths does
	const auto offset = decompress_offset + SumStringLengths(decompress_position, string_number);
	if (offset > layout.dictionary.Size()) {
		ThrowStringLengthsOutOfBounds();
	}
	decompress_offset = UnsafeNumericCast<uint32_t>(offset);
	summed_lengths += string_number - decompress_position;
	decompress_position = string_number;
	return decompress_offset;
}

const SelectionVector &CompressedStringScanState::GetSelVec(idx_t start, idx_t scan_count) {
	D_ASSERT(start <= segment.count.load());
	D_ASSERT(scan_count <= segment.count.load() - start);
	switch (layout.mode) {
	case DictFSSTMode::FSST_ONLY: {
		return *FlatVector::IncrementalSelectionVector();
	}
	default: {
		// Handling non-bitpacking-group-aligned start values;
		idx_t start_offset = start % BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE;

		// We will scan in blocks of BITPACKING_ALGORITHM_GROUP_SIZE, so we may scan some extra values.
		idx_t decompress_count = BitpackingPrimitives::RoundUpToAlgorithmGroupSize(scan_count + start_offset);

		// Start at the group containing start. Its leading values are removed after unpacking.
		const auto group_size = BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE;
		const auto group_bytes = group_size * idx_t(layout.dictionary_indices_width) / 8;
		const auto source_offset = (start / group_size) * group_bytes;
		const auto source_size =
		    BitpackingPrimitives::GetRequiredSize(decompress_count, layout.dictionary_indices_width);
		auto indices = layout.dictionary_indices.SubArray(source_offset, source_size);

		if (!sel_vec || sel_vec_size < decompress_count) {
			sel_vec_size = decompress_count;
			sel_vec = make_buffer<SelectionVector>(decompress_count);
		}

		sel_t *sel_vec_ptr = sel_vec->data();
		BitpackingPrimitives::UnPackBuffer<sel_t>(data_ptr_cast(sel_vec_ptr), indices.data(), decompress_count,
		                                          layout.dictionary_indices_width);

		sel_vec->ShiftLeft(start_offset, scan_count);

		// Validate the requested rows, not the leading values or final unpacking padding.
		bool has_invalid_index = false;
		for (idx_t i = 0; i < scan_count; i++) {
			has_invalid_index |= sel_vec->get_index_unsafe(i) >= layout.dict_count;
		}
		if (has_invalid_index) {
			ThrowDictionaryIndexOutOfBounds();
		}

		return *sel_vec;
	}
	}
}

void CompressedStringScanState::ScanToFlatVector(Vector &result, idx_t result_offset, idx_t start, idx_t scan_count,
                                                 StringScanType string_scan_type) {
	Select(result, result_offset, start, *FlatVector::IncrementalSelectionVector(), scan_count, string_scan_type);
	result.Verify();
}

void CompressedStringScanState::Select(Vector &result, idx_t result_offset, idx_t start, const SelectionVector &sel,
                                       idx_t sel_count, StringScanType string_scan_type) {
	// The last selected row bounds the unpacked range, so selections must be ordered.
#ifdef DEBUG
	for (idx_t i = 1; i < sel_count; i++) {
		D_ASSERT(sel.get_index(i - 1) <= sel.get_index(i));
	}
#endif
	const auto scan_count = sel_count == 0 ? 0 : sel.get_index(sel_count - 1) + 1;
	auto &selvec = GetSelVec(start, scan_count);

	//! (index 0 is reserved for NULL, which we don't have in this mode)
	const idx_t start_offset = layout.mode == DictFSSTMode::FSST_ONLY ? start + 1 : 0;

	auto result_data = FlatVector::Writer<string_t>(result, sel_count, result_offset);
	if (string_scan_type == StringScanType::COPY_STRINGS) {
		// a select of many rows unpacks every length up front and computes all offsets on its first step back
		const bool few_rows = sel_count < BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE;
		if (!few_rows) {
			LoadStringLengths();
		}
		for (idx_t i = 0; i < sel_count; i++) {
			auto string_number = selvec.get_index(start_offset + sel.get_index(i));
			if (string_number == 0) {
				result_data.WriteNull();
				continue;
			}
			result_data.WriteStringRef(FetchStringFromDict(result, GetStringOffset(string_number, few_rows),
			                                               string_number, StringScanType::COPY_STRINGS));
		}
	} else if (EnsureDictionary()) {
		// We have prepared the full dictionary, we can reference these strings directly
		auto dictionary_values = FlatVector::GetData<string_t>(dictionary->data);
		for (idx_t i = 0; i < sel_count; i++) {
			// Lookup dict offset in index buffer
			auto string_number = selvec.get_index(start_offset + sel.get_index(i));
			if (string_number == 0) {
				result_data.WriteNull();
				continue;
			}
			result_data.WriteStringRef(dictionary_values[string_number]);
		}
	} else {
		LoadStringLengths();
		for (idx_t i = 0; i < sel_count; i++) {
			// Lookup dict offset in index buffer
			auto string_number = selvec.get_index(start_offset + sel.get_index(i));
			if (string_number == 0) {
				result_data.WriteNull();
				continue;
			}
			if (decompress_position > string_number) {
				throw InternalException("DICT_FSST: not performing a sequential scan?");
			}
			for (; decompress_position < string_number; decompress_position++) {
				decompress_offset += string_lengths[decompress_position];
			}
			result_data.WriteStringRef(FetchStringFromDict(result, decompress_offset, string_number));
		}
	}
}

bool CompressedStringScanState::AllowDictionaryScan(idx_t scan_count) {
	if (layout.mode == DictFSSTMode::FSST_ONLY) {
		return false;
	}
	if (scan_count != STANDARD_VECTOR_SIZE) {
		return false;
	}
	return EnsureDictionary();
}

void CompressedStringScanState::ScanToDictionaryVector(ColumnSegment &segment, Vector &result, idx_t result_offset,
                                                       idx_t start, idx_t scan_count) {
	D_ASSERT(scan_count == STANDARD_VECTOR_SIZE);
	D_ASSERT(result_offset == 0);

	auto &selvec = GetSelVec(start, scan_count);
	result.Dictionary(dictionary, selvec, scan_count);
	result.Verify();
}

} // namespace dict_fsst
} // namespace duckdb
