//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/string_uncompressed.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/likely.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/function/compression_function.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/compression/compression_segment_reader.hpp"
#include "duckdb/storage/segment/uncompressed.hpp"
#include "duckdb/storage/table/column_segment.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/storage/statistics/stats_writer.hpp"

namespace duckdb {
struct StringDictionaryContainer {
	//! The size of the dictionary
	uint32_t size;
	//! The end of the dictionary, which defaults to the block size.
	uint32_t end;

	void Verify(const idx_t block_size) {
		D_ASSERT(size <= block_size);
		D_ASSERT(end <= block_size);
		D_ASSERT(size <= end);
	}
};

struct StringDictionaryEntry {
	unsafe_array_ptr<const uint8_t> data;
	bool is_overflow;
};

struct ValidatedStringRange {
public:
	StringDictionaryEntry GetEntry(idx_t index) const {
		D_ASSERT(index < offsets.size());
		const auto current_offset = offsets[index];
		const auto dictionary_offset = UnsafeNumericCast<uint32_t>(AbsValue<int32_t>(current_offset));
		const auto previous_offset = index > 0 ? UnsafeNumericCast<uint32_t>(AbsValue<int32_t>(offsets[index - 1]))
		                                       : preceding_dictionary_offset;
		const auto string_length = dictionary_offset - previous_offset;
		return {dictionary_data.SubArray(dictionary_data.size() - dictionary_offset, string_length),
		        current_offset < 0 && string_length > 0};
	}

private:
	friend struct StringSegmentLayout;
	ValidatedStringRange(unsafe_array_ptr<const uint8_t> dictionary_data_p, unsafe_array_ptr<const int32_t> offsets_p,
	                     uint32_t preceding_dictionary_offset_p)
	    : dictionary_data(dictionary_data_p), offsets(offsets_p),
	      preceding_dictionary_offset(preceding_dictionary_offset_p) {
	}

	unsafe_array_ptr<const uint8_t> dictionary_data;
	unsafe_array_ptr<const int32_t> offsets;
	uint32_t preceding_dictionary_offset;
};

struct StringSegmentLayout {
public:
	static StringSegmentLayout Read(const BufferHandle &handle, const ColumnSegment &segment);
	StringDictionaryEntry ValidateAndGetEntry(idx_t row_index) const;
	ValidatedStringRange ValidateRange(idx_t start, idx_t count) const;

private:
	StringSegmentLayout(const unsafe_array_ptr<const uint8_t> dictionary_data_p,
	                    const unsafe_array_ptr<const int32_t> offsets_p)
	    : dictionary_data(dictionary_data_p), offsets(offsets_p) {
	}

	//! Dictionary bytes [DICTIONARY_HEADER_SIZE + segment.count * sizeof(int32_t), dictionary end)
	unsafe_array_ptr<const uint8_t> dictionary_data;
	//! Dictionary offsets read from the segment after validating the array's byte range.
	unsafe_array_ptr<const int32_t> offsets;
};

struct StringScanState : public SegmentScanState {
public:
	explicit StringScanState(BufferHandle handle);

	BufferHandle handle;
};

//===--------------------------------------------------------------------===//
// Append
//===--------------------------------------------------------------------===//
struct SerializedStringSegmentState : public ColumnSegmentState {
public:
	SerializedStringSegmentState();
	explicit SerializedStringSegmentState(vector<block_id_t> blocks_p);

public:
	void Serialize(Serializer &serializer) const override;
};

struct UncompressedStringStorage {
public:
	//! Dictionary header size at the beginning of the string segment (offset + length)
	static constexpr uint16_t DICTIONARY_HEADER_SIZE = sizeof(uint32_t) + sizeof(uint32_t);
	//! Marker used in length field to indicate the presence of a big string
	static constexpr uint16_t BIG_STRING_MARKER = (uint16_t)-1;
	//! Base size of big string marker (block id + offset)
	static constexpr idx_t BIG_STRING_MARKER_BASE_SIZE = sizeof(block_id_t) + sizeof(int32_t);
	//! The marker size of the big string
	static constexpr idx_t BIG_STRING_MARKER_SIZE = BIG_STRING_MARKER_BASE_SIZE;

public:
	static unique_ptr<AnalyzeState> StringInitAnalyze(ColumnData &col_data, PhysicalType type);
	static bool StringAnalyze(AnalyzeState &state_p, const Vector &input);
	static idx_t StringFinalAnalyze(AnalyzeState &state_p);
	static unique_ptr<SegmentScanState> StringInitScan(const QueryContext &context, ColumnSegment &segment);
	static void StringScanPartial(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result,
	                              idx_t result_offset);
	static void StringScan(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result);
	static void Select(ColumnSegment &segment, ColumnScanState &state, idx_t vector_count, Vector &result,
	                   const SelectionVector &sel, idx_t sel_count, idx_t result_offset, ScanVectorType scan_type);

	static unique_ptr<CompressedSegmentState> StringInitSegment(ColumnSegment &segment, block_id_t block_id,
	                                                            optional_ptr<ColumnSegmentState> segment_state);

	static unique_ptr<CompressionAppendState> StringInitAppend(ColumnSegment &segment) {
		auto &buffer_manager = BufferManager::GetBufferManager(segment.GetDatabase());
		// This block was initialized in StringInitSegment
		auto handle = buffer_manager.Pin(segment.GetBlockHandle());
		return make_uniq<CompressionAppendState>(std::move(handle));
	}

	static idx_t StringAppend(CompressionAppendState &append_state, ColumnSegment &segment, BaseStatistics &stats,
	                          UnifiedVectorFormat &data, idx_t offset, idx_t count) {
		return StringAppendBase(append_state.handle, segment, stats, data, offset, count);
	}

	static idx_t StringAppendBase(ColumnSegment &segment, BaseStatistics &stats, UnifiedVectorFormat &data,
	                              idx_t offset, idx_t count) {
		auto &buffer_manager = BufferManager::GetBufferManager(segment.GetDatabase());
		auto handle = buffer_manager.Pin(segment.GetBlockHandle());
		return StringAppendBase(handle, segment, stats, data, offset, count);
	}

	static idx_t StringAppendBase(BufferHandle &handle, ColumnSegment &segment, BaseStatistics &stats,
	                              UnifiedVectorFormat &data, idx_t offset, idx_t count) {
		auto handle_ptr = handle.GetDataMutable() + segment.GetBlockOffset();
		auto source_data = UnifiedVectorFormat::GetData<string_t>(data);
		auto result_data = reinterpret_cast<int32_t *>(handle_ptr + DICTIONARY_HEADER_SIZE);
		auto dictionary_size = reinterpret_cast<uint32_t *>(handle_ptr);
		auto dictionary_end = reinterpret_cast<uint32_t *>(handle_ptr + sizeof(uint32_t));

		idx_t remaining_space = RemainingSpace(segment, handle);
		auto base_count = segment.count.load();
		StatsWriter<string_t> stats_writer(stats.GetType());
		for (idx_t i = 0; i < count; i++) {
			auto source_idx = data.sel->get_index(offset + i);
			auto target_idx = base_count + i;
			if (remaining_space < sizeof(int32_t)) {
				// string index does not fit in the block at all
				count = i;
				break;
			}
			remaining_space -= sizeof(int32_t);
			const bool is_null = !data.validity.RowIsValid(source_idx);
			if (is_null) {
				stats_writer.SetHasNull();
				// null value is stored as a copy of the last value, this is done to be able to efficiently do the
				// string_length calculation
				if (target_idx > 0) {
					result_data[target_idx] = result_data[target_idx - 1];
				} else {
					result_data[target_idx] = 0;
				}
				continue;
			}
			auto end = handle_ptr + *dictionary_end;

#ifdef DEBUG
			GetDictionary(segment, handle).Verify(segment.GetBlockSize());
#endif
			// Unknown string, continue
			// non-null value, check if we can fit it within the block
			idx_t string_length = source_data[source_idx].GetSize();

			// determine whether or not we have space in the block for this string
			bool use_overflow_block = false;
			idx_t required_space = string_length;
			if (DUCKDB_UNLIKELY(required_space >= StringUncompressed::GetStringBlockLimit(segment.GetBlockSize()))) {
				// string exceeds block limit, store in overflow block and only write a marker here
				required_space = BIG_STRING_MARKER_SIZE;
				use_overflow_block = true;
			}
			if (DUCKDB_UNLIKELY(required_space > remaining_space)) {
				// no space remaining: return how many tuples we ended up writing
				count = i;
				break;
			}

			// we have space: write the string
			stats_writer.Update(source_data[source_idx]);

			if (DUCKDB_UNLIKELY(use_overflow_block)) {
				// write to overflow blocks
				block_id_t block;
				int32_t current_offset;
				// write the string into the current string block
				WriteString(segment, source_data[source_idx], block, current_offset);
				*dictionary_size += BIG_STRING_MARKER_SIZE;
				remaining_space -= BIG_STRING_MARKER_SIZE;
				auto dict_pos = end - *dictionary_size;

				// write a big string marker into the dictionary
				WriteStringMarker(dict_pos, block, current_offset);

				// place the dictionary offset into the set of vectors
				// note: for overflow strings we write negative value

				// dictionary_size is an uint32_t value, so we can cast up.
				D_ASSERT(NumericCast<idx_t>(*dictionary_size) <= segment.GetBlockSize());
				result_data[target_idx] = -NumericCast<int32_t>((*dictionary_size));
			} else {
				// string fits in block, append to dictionary and increment dictionary position
				D_ASSERT(string_length < NumericLimits<uint16_t>::Maximum());
				*dictionary_size += required_space;
				remaining_space -= required_space;
				auto dict_pos = end - *dictionary_size;
				// now write the actual string data into the dictionary
				memcpy(dict_pos, source_data[source_idx].GetData(), string_length);

				// dictionary_size is an uint32_t value, so we can cast up.
				D_ASSERT(NumericCast<idx_t>(*dictionary_size) <= segment.GetBlockSize());
				// Place the dictionary offset into the set of vectors.
				result_data[target_idx] = NumericCast<int32_t>(*dictionary_size);
			}
			D_ASSERT(RemainingSpace(segment, handle) <= segment.GetBlockSize());
#ifdef DEBUG
			GetDictionary(segment, handle).Verify(segment.GetBlockSize());
#endif
		}
		stats_writer.Merge(stats);
		segment.count += count;
		return count;
	}

	static void StringRevertAppend(ColumnSegment &segment, idx_t new_count) {
		if (new_count >= segment.count) {
			return;
		}
		// we need to decrement the dictionary size by all of the strings we are erasing
		auto &buffer_manager = BufferManager::GetBufferManager(segment.GetDatabase());
		auto handle = buffer_manager.Pin(segment.GetBlockHandle());
		auto handle_ptr = handle.GetDataMutable() + segment.GetBlockOffset();
		auto result_data = reinterpret_cast<int32_t *>(handle_ptr + DICTIONARY_HEADER_SIZE);
		auto dictionary_size = reinterpret_cast<uint32_t *>(handle_ptr);
		uint32_t new_dictionary_size;
		if (new_count == 0) {
			new_dictionary_size = 0;
		} else {
			auto entry_offset = result_data[new_count - 1];
			if (entry_offset < 0) {
				// overflow strings store the dict offset negatively - invert size
				new_dictionary_size = NumericCast<uint32_t>(-entry_offset);
			} else {
				new_dictionary_size = NumericCast<uint32_t>(entry_offset);
			}
		}
		*dictionary_size = new_dictionary_size;
	}

	static idx_t FinalizeAppend(ColumnSegment &segment, BaseStatistics &stats);

public:
	static void SetDictionary(ColumnSegment &segment, BufferHandle &handle, StringDictionaryContainer dict);
	static StringDictionaryContainer GetDictionary(ColumnSegment &segment, BufferHandle &handle);
	static idx_t RemainingSpace(ColumnSegment &segment, BufferHandle &handle);
	static void WriteString(ColumnSegment &segment, string_t string, block_id_t &result_block, int32_t &result_offset);
	static void WriteStringMemory(ColumnSegment &segment, string_t string, block_id_t &result_block,
	                              int32_t &result_offset);
	static string_t ReadOverflowString(const QueryContext &context, ColumnSegment &segment, Vector &result,
	                                   block_id_t block, int32_t offset);
	static string_t ReadStringWithLength(CompressionSegmentReader reader, int32_t offset);
	static void WriteStringMarker(data_ptr_t target, block_id_t block_id, int32_t offset);

	// FIXME: Remove this raw pointer helper once FSST no longer needs it.
	inline static string_t FetchStringFromDict(const QueryContext &context, ColumnSegment &segment,
	                                           uint32_t dict_end_offset, Vector &result, data_ptr_t base_ptr,
	                                           int32_t dict_offset, uint32_t string_length) {
		D_ASSERT(dict_offset <= NumericCast<int32_t>(segment.GetBlockSize()));
		if (DUCKDB_LIKELY(dict_offset >= 0)) {
			// regular string - fetch from dictionary
			auto dict_end = base_ptr + dict_end_offset;
			auto dict_pos = dict_end - dict_offset;
			auto str_ptr = char_ptr_cast(dict_pos);
			return string_t(str_ptr, string_length);
		} else if (string_length == 0) {
			// NULL values are stored as a copy of the previous entry's dictionary offset (see
			// StringAppendBase). When the previous entry is a big (overflow) string, its offset is
			// negative, so the NULL inherits that negative offset even though it references no
			// overflow data (its computed length is 0). Return an empty string here instead of
			// following the marker into ReadOverflowString - otherwise every NULL that trails a big
			// string re-reads and re-allocates the entire overflow string (O(num_nulls * big_size)
			// memory, tagged OVERFLOW_STRINGS). The value is masked NULL by the caller regardless.
			return string_t(char_ptr_cast(base_ptr), 0);
		} else {
			// read overflow string
			auto marker = base_ptr + dict_end_offset - AbsValue<int32_t>(dict_offset);
			block_id_t block_id = Load<block_id_t>(marker);
			int32_t offset = Load<int32_t>(marker + sizeof(block_id_t));

			return ReadOverflowString(context, segment, result, block_id, offset);
		}
	}

	inline static string_t FetchStringFromEntry(const QueryContext &context, ColumnSegment &segment, Vector &result,
	                                            const StringDictionaryEntry &entry) {
		if (DUCKDB_LIKELY(!entry.is_overflow)) {
			// regular string - fetch from dictionary
			auto str_ptr = const_char_ptr_cast(entry.data.data());
			return string_t(str_ptr, NumericCast<uint32_t>(entry.data.size()));
		} else {
			// read overflow string
			D_ASSERT(entry.data.size() == BIG_STRING_MARKER_SIZE);
			auto block_id = Load<block_id_t>(entry.data.data());
			auto offset = Load<int32_t>(entry.data.data() + sizeof(block_id_t));

			return ReadOverflowString(context, segment, result, block_id, offset);
		}
	}

	static unique_ptr<ColumnSegmentState> SerializeState(ColumnSegment &segment);
	static unique_ptr<ColumnSegmentState> DeserializeState(Deserializer &deserializer);
	static void VisitBlockIds(const ColumnSegment &segment, BlockIdVisitor &visitor);
};
} // namespace duckdb
