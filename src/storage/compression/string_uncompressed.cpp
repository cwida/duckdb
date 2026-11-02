#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/storage/string_uncompressed.hpp"

#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/storage/checkpoint/write_overflow_strings_to_disk.hpp"
#include "duckdb/storage/table/column_data.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Error Helpers
//===--------------------------------------------------------------------===//
[[noreturn]] static void ThrowInvalidStringDictionary() {
	throw DataCorruptionException("Corrupted uncompressed string segment: dictionary is outside the segment");
}

[[noreturn]] static void ThrowStringOffsetTableOutOfBounds() {
	throw DataCorruptionException("Corrupted uncompressed string segment: offset table overlaps the dictionary");
}

[[noreturn]] static void ThrowDictionaryOffsetMinimumValue() {
	throw DataCorruptionException("Corrupted uncompressed string segment: dictionary offset is INT32_MIN");
}

[[noreturn]] static void ThrowDictionaryOffsetOutOfBounds(uint32_t offset, uint32_t dictionary_size) {
	throw DataCorruptionException(
	    "Corrupted uncompressed string segment: dictionary offset %u exceeds dictionary size %u", offset,
	    dictionary_size);
}

[[noreturn]] static void ThrowDecreasingDictionaryOffset(uint32_t offset, uint32_t previous_offset) {
	throw DataCorruptionException(
	    "Corrupted uncompressed string segment: dictionary offset %u is smaller than preceding offset %u", offset,
	    previous_offset);
}

[[noreturn]] static void ThrowInvalidDictionaryEntryMarker() {
	throw DataCorruptionException(
	    "Corrupted uncompressed string segment: negative dictionary offset does not describe an overflow marker");
}

[[noreturn]] static void ThrowInvalidOverflowStringBlock() {
	throw DataCorruptionException("Corrupted uncompressed string segment: invalid overflow string block ID");
}

[[noreturn]] static void ThrowOverflowStringOffsetOutOfBounds() {
	throw DataCorruptionException("Corrupted uncompressed string segment: overflow string offset is outside its block");
}

//===--------------------------------------------------------------------===//
// Storage Class
//===--------------------------------------------------------------------===//
UncompressedStringSegmentState::~UncompressedStringSegmentState() {
	while (head) {
		// prevent deep recursion here
		head = std::move(head->next);
	}
}

//===--------------------------------------------------------------------===//
// Analyze
//===--------------------------------------------------------------------===//
struct StringAnalyzeState : public AnalyzeState {
	explicit StringAnalyzeState(BlockManager &block_manager)
	    : AnalyzeState(block_manager), count(0), total_string_size(0), overflow_strings(0) {
	}

	idx_t count;
	idx_t total_string_size;
	idx_t overflow_strings;
};

unique_ptr<AnalyzeState> UncompressedStringStorage::StringInitAnalyze(ColumnData &col_data, PhysicalType type) {
	return make_uniq<StringAnalyzeState>(col_data.GetBlockManager());
}

bool UncompressedStringStorage::StringAnalyze(AnalyzeState &state_p, const Vector &input) {
	auto &state = state_p.Cast<StringAnalyzeState>();
	UnifiedVectorFormat vdata;
	input.ToUnifiedFormat(vdata);

	const auto count = input.size();
	state.count += count;
	auto data = UnifiedVectorFormat::GetData<string_t>(vdata);
	for (idx_t i = 0; i < count; i++) {
		auto idx = vdata.sel->get_index(i);
		if (vdata.validity.RowIsValid(idx)) {
			auto string_size = data[idx].GetSize();
			state.total_string_size += string_size;
			if (string_size >= StringUncompressed::GetStringBlockLimit(state.info.GetBlockSize())) {
				state.overflow_strings++;
			}
		}
	}
	return true;
}

idx_t UncompressedStringStorage::StringFinalAnalyze(AnalyzeState &state_p) {
	auto &state = state_p.Cast<StringAnalyzeState>();
	return state.count * sizeof(int32_t) + state.total_string_size + state.overflow_strings * BIG_STRING_MARKER_SIZE;
}

//===--------------------------------------------------------------------===//
// Scan
//===--------------------------------------------------------------------===//
StringScanState::StringScanState(BufferHandle handle_p) : handle(std::move(handle_p)) {
}

void UncompressedStringInitPrefetch(ColumnSegment &segment, PrefetchState &prefetch_state) {
	prefetch_state.AddBlock(segment.GetBlockHandle());
	auto segment_state = segment.GetSegmentState();
	if (segment_state) {
		auto &state = segment_state->Cast<UncompressedStringSegmentState>();
		auto &block_manager = segment.GetBlockHandle()->GetBlockManager();
		for (auto &block_id : state.GetOnDiskBlocks()) {
			auto block_handle = state.GetHandle(block_manager, block_id);
			prefetch_state.AddBlock(block_handle);
		}
	}
}

unique_ptr<SegmentScanState> UncompressedStringStorage::StringInitScan(const QueryContext &context,
                                                                       ColumnSegment &segment) {
	auto &buffer_manager = BufferManager::GetBufferManager(segment.GetDatabase());
	auto handle = buffer_manager.Pin(context, segment.GetBlockHandle());
	return make_uniq<StringScanState>(std::move(handle));
}

//===--------------------------------------------------------------------===//
// Scan base data
//===--------------------------------------------------------------------===//
void UncompressedStringStorage::StringScanPartial(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count,
                                                  Vector &result, idx_t result_offset) {
	// clear any previously locked buffers and get the primary buffer handle
	auto &scan_state = state.scan_state->Cast<StringScanState>();
	auto layout = StringSegmentLayout::Read(scan_state.handle, segment);
	auto start = state.GetPositionInSegment();
	D_ASSERT(start <= segment.count.load());
	D_ASSERT(scan_count <= segment.count.load() - start);

	auto result_data = FlatVector::GetDataMutable<string_t>(result);
	auto strings = layout.ValidateRange(start, scan_count);

	const bool copy_strings = state.string_scan_type == StringScanType::COPY_STRINGS;
	for (idx_t i = 0; i < scan_count; i++) {
		auto entry = strings.GetEntry(i);
		auto str = FetchStringFromEntry(state.context, segment, result, entry);
		// overflow strings are already owned by the result
		result_data[result_offset + i] =
		    copy_strings && !entry.is_overflow ? StringVector::AddStringOrBlob(result, str) : str;
	}
}

void UncompressedStringStorage::StringScan(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count,
                                           Vector &result) {
	StringScanPartial(segment, state, scan_count, result, 0);
}

//===--------------------------------------------------------------------===//
// Select
//===--------------------------------------------------------------------===//
void UncompressedStringStorage::Select(ColumnSegment &segment, ColumnScanState &state, idx_t vector_count,
                                       Vector &result, const SelectionVector &sel, idx_t sel_count, idx_t result_offset,
                                       ScanVectorType) {
	// clear any previously locked buffers and get the primary buffer handle
	auto &scan_state = state.scan_state->Cast<StringScanState>();
	auto layout = StringSegmentLayout::Read(scan_state.handle, segment);
	auto start = state.GetPositionInSegment();
	D_ASSERT(start <= segment.count.load());
	D_ASSERT(vector_count <= segment.count.load() - start);
	D_ASSERT(sel_count <= vector_count);

	auto result_data = FlatVector::GetDataMutable<string_t>(result);
	const bool copy_strings = state.string_scan_type == StringScanType::COPY_STRINGS;
	auto write_entry = [&](idx_t i, const StringDictionaryEntry &entry) {
		auto str = FetchStringFromEntry(state.context, segment, result, entry);
		result_data[result_offset + i] =
		    copy_strings && !entry.is_overflow ? StringVector::AddStringOrBlob(result, str) : str;
	};
	// a sparse selection validates its own entries rather than every entry in between
	if (sel_count * 4 < vector_count) {
		for (idx_t i = 0; i < sel_count; i++) {
			D_ASSERT(sel.get_index(i) < vector_count);
			write_entry(i, layout.ValidateAndGetEntry(start + sel.get_index(i)));
		}
		return;
	}
	auto strings = layout.ValidateRange(start, vector_count);
	for (idx_t i = 0; i < sel_count; i++) {
		auto selection_index = sel.get_index(i);
		D_ASSERT(selection_index < vector_count);
		write_entry(i, strings.GetEntry(selection_index));
	}
}

//===--------------------------------------------------------------------===//
// Append
//===--------------------------------------------------------------------===//
SerializedStringSegmentState::SerializedStringSegmentState() {
}

SerializedStringSegmentState::SerializedStringSegmentState(vector<block_id_t> blocks_p) {
	blocks = std::move(blocks_p);
}

void SerializedStringSegmentState::Serialize(Serializer &serializer) const {
	serializer.WriteProperty(1, "overflow_blocks", blocks);
}

unique_ptr<CompressedSegmentState>
UncompressedStringStorage::StringInitSegment(ColumnSegment &segment, block_id_t block_id,
                                             optional_ptr<ColumnSegmentState> segment_state) {
	auto &buffer_manager = BufferManager::GetBufferManager(segment.GetDatabase());
	if (block_id == INVALID_BLOCK) {
		auto handle = buffer_manager.Pin(segment.GetBlockHandle());
		StringDictionaryContainer dictionary;
		dictionary.size = 0;
		dictionary.end = UnsafeNumericCast<uint32_t>(segment.SegmentSize());
		SetDictionary(segment, handle, dictionary);
	}
	auto result = make_uniq<UncompressedStringSegmentState>();
	if (segment_state) {
		auto &serialized_state = segment_state->Cast<SerializedStringSegmentState>();
		result->InitializeOnDiskBlocks(std::move(serialized_state.blocks));
	}
	return std::move(result);
}

idx_t UncompressedStringStorage::FinalizeAppend(ColumnSegment &segment, BaseStatistics &) {
	auto &buffer_manager = BufferManager::GetBufferManager(segment.GetDatabase());
	auto handle = buffer_manager.Pin(segment.GetBlockHandle());
	auto dict = GetDictionary(segment, handle);
	D_ASSERT(dict.end == segment.SegmentSize());
	// compute the total size required to store this segment
	auto offset_size = DICTIONARY_HEADER_SIZE + segment.count * sizeof(int32_t);
	auto total_size = offset_size + dict.size;

	CompressionInfo info(segment.GetBlockHandle()->GetBlockManager());
	if (total_size >= info.GetCompactionFlushLimit()) {
		// the block is full enough, don't bother moving around the dictionary
		return segment.SegmentSize();
	}

	// the block has space left: figure out how much space we can save
	auto move_amount = segment.SegmentSize() - total_size;
	// move the dictionary so it lines up exactly with the offsets
	auto dataptr = handle.GetDataMutable() + segment.GetBlockOffset();
	memmove(dataptr + offset_size, dataptr + dict.end - dict.size, dict.size);
	dict.end -= move_amount;
	D_ASSERT(dict.end == total_size);
	// write the new dictionary (with the updated "end")
	SetDictionary(segment, handle, dict);
	return total_size;
}

//===--------------------------------------------------------------------===//
// Serialization & Cleanup
//===--------------------------------------------------------------------===//
unique_ptr<ColumnSegmentState> UncompressedStringStorage::SerializeState(ColumnSegment &segment) {
	auto &state = segment.GetSegmentState()->Cast<UncompressedStringSegmentState>();
	if (state.GetOnDiskBlocks().empty()) {
		// no on-disk blocks - nothing to write
		return nullptr;
	}
	return make_uniq<SerializedStringSegmentState>(state.GetOnDiskBlocks());
}

unique_ptr<ColumnSegmentState> UncompressedStringStorage::DeserializeState(Deserializer &deserializer) {
	auto result = make_uniq<SerializedStringSegmentState>();
	deserializer.ReadProperty(1, "overflow_blocks", result->blocks);
	return std::move(result);
}

void UncompressedStringStorage::VisitBlockIds(const ColumnSegment &segment, BlockIdVisitor &visitor) {
	auto &state = segment.GetSegmentState()->Cast<UncompressedStringSegmentState>();
	for (auto &block_id : state.GetOnDiskBlocks()) {
		visitor.Visit(block_id);
	}
}

//===--------------------------------------------------------------------===//
// Get Function
//===--------------------------------------------------------------------===//
CompressionFunction StringUncompressed::GetFunction(PhysicalType data_type) {
	D_ASSERT(data_type == PhysicalType::VARCHAR);
	return CompressionFunction(
	    CompressionType::COMPRESSION_UNCOMPRESSED, data_type, UncompressedStringStorage::StringInitAnalyze,
	    UncompressedStringStorage::StringAnalyze, UncompressedStringStorage::StringFinalAnalyze,
	    UncompressedFunctions::InitCompression, UncompressedFunctions::Compress,
	    UncompressedFunctions::FinalizeCompress, UncompressedStringStorage::StringInitScan,
	    UncompressedStringStorage::StringScan, UncompressedStringStorage::StringScanPartial,
	    UncompressedFunctions::EmptySkip, UncompressedStringStorage::StringInitSegment,
	    UncompressedStringStorage::StringInitAppend, UncompressedStringStorage::StringAppend,
	    UncompressedStringStorage::FinalizeAppend, UncompressedStringStorage::StringRevertAppend,
	    UncompressedStringStorage::SerializeState, UncompressedStringStorage::DeserializeState,
	    UncompressedStringStorage::VisitBlockIds, UncompressedStringInitPrefetch, UncompressedStringStorage::Select);
}

//===--------------------------------------------------------------------===//
// Dictionary Validation
//===--------------------------------------------------------------------===//
//! Returns the absolute offset without validating it.
static inline uint32_t GetDictionaryOffset(const int32_t encoded_offset) {
	// Widen first so taking the absolute value of INT32_MIN does not overflow.
	return UnsafeNumericCast<uint32_t>(AbsValue<int64_t>(encoded_offset));
}

static uint32_t ValidateAndGetDictionaryOffset(const int32_t encoded_offset, const uint32_t dictionary_size) {
	if (encoded_offset == NumericLimits<int32_t>::Minimum()) {
		ThrowDictionaryOffsetMinimumValue();
	}

	const auto dictionary_offset = GetDictionaryOffset(encoded_offset);
	if (dictionary_offset > dictionary_size) {
		ThrowDictionaryOffsetOutOfBounds(dictionary_offset, dictionary_size);
	}
	return dictionary_offset;
}

//! SIMD friendly version of ValidateDictionaryEntry that avoids early exits.
//! Returns 1 if ValidateDictionaryEntry would accept the offsets, otherwise 0.
static uint32_t ValidateDictionaryEntryFast(const int32_t current_offset, const int32_t previous_offset,
                                            const uint32_t dictionary_size) {
	const auto current_dictionary_offset = GetDictionaryOffset(current_offset);
	const auto previous_dictionary_offset = GetDictionaryOffset(previous_offset);
	// Offsets store the cumulative number of dictionary bytes used.
	const auto string_length = current_dictionary_offset - previous_dictionary_offset;
	uint32_t invalid = current_offset == NumericLimits<int32_t>::Minimum();
	invalid |= current_dictionary_offset > dictionary_size;
	// Dictionary offsets must not decrease.
	invalid |= current_dictionary_offset < previous_dictionary_offset;

	// If the offset is negative, the entry is either NULL or an overflow string.
	const uint32_t is_negative = current_offset < 0;
	const uint32_t has_zero_length = string_length == 0;
	const uint32_t has_bytes = string_length != 0;
	const uint32_t offset_changed = current_offset != previous_offset;
	const uint32_t wrong_marker_size = string_length != UncompressedStringStorage::BIG_STRING_MARKER_SIZE;

	// If it is NULL, the current offset must be inherited unchanged from the previous entry.
	const auto invalid_null = has_zero_length & offset_changed;
	// If it is an overflow string, the entry's length must match the marker's length.
	const auto invalid_overflow = has_bytes & wrong_marker_size;
	invalid |= is_negative & (invalid_null | invalid_overflow);

	return invalid == 0;
}

//! Validates both offsets and their entry, throwing on invalid data.
static void ValidateDictionaryEntry(const int32_t current_offset, const int32_t previous_offset,
                                    const uint32_t dictionary_size) {
	const auto previous_dictionary_offset = ValidateAndGetDictionaryOffset(previous_offset, dictionary_size);
	const auto current_dictionary_offset = ValidateAndGetDictionaryOffset(current_offset, dictionary_size);

	// Dictionary offsets must not decrease.
	if (current_dictionary_offset < previous_dictionary_offset) {
		D_ASSERT(!ValidateDictionaryEntryFast(current_offset, previous_offset, dictionary_size));
		ThrowDecreasingDictionaryOffset(current_dictionary_offset, previous_dictionary_offset);
	}

	// Offsets store the cumulative number of dictionary bytes used.
	const auto string_length = current_dictionary_offset - previous_dictionary_offset;

	// If the offset is negative, the entry is either NULL or an overflow string.
	if (current_offset < 0) {
		// If it is NULL, the current offset must be inherited unchanged from the previous entry.
		if (string_length == 0 && current_offset != previous_offset) {
			D_ASSERT(!ValidateDictionaryEntryFast(current_offset, previous_offset, dictionary_size));
			ThrowInvalidDictionaryEntryMarker();
		}

		// If it is an overflow string, the entry's length must match the marker's length.
		if (string_length > 0 && string_length != UncompressedStringStorage::BIG_STRING_MARKER_SIZE) {
			D_ASSERT(!ValidateDictionaryEntryFast(current_offset, previous_offset, dictionary_size));
			ThrowInvalidDictionaryEntryMarker();
		}
	}

	D_ASSERT(ValidateDictionaryEntryFast(current_offset, previous_offset, dictionary_size));
}

StringSegmentLayout StringSegmentLayout::Read(const BufferHandle &handle, const ColumnSegment &segment) {
	auto reader = CompressionSegmentReader::FromSegment(handle, segment, "uncompressed string segment");
	// Appends change the dictionary size, so reading it during a scan would race.
	// The dictionary end does not change during appends, so use that instead.
	auto dictionary_end = NumericCast<idx_t>(reader.Get<uint32_t>(sizeof(uint32_t)));
	if (dictionary_end > reader.Size()) {
		ThrowInvalidStringDictionary();
	}
	auto offset_count = segment.count.load();
	if (dictionary_end < UncompressedStringStorage::DICTIONARY_HEADER_SIZE ||
	    offset_count > (dictionary_end - UncompressedStringStorage::DICTIONARY_HEADER_SIZE) / sizeof(int32_t)) {
		ThrowStringOffsetTableOutOfBounds();
	}
	// Dictionary bytes do not overlap the offsets table, so use the end of the table
	// as the lower bound instead of reading the dictionary size.
	auto dictionary_start = UncompressedStringStorage::DICTIONARY_HEADER_SIZE + offset_count * sizeof(int32_t);
	auto dictionary_data = reader.GetBytes(dictionary_start, dictionary_end - dictionary_start);
	auto offsets = reader.GetArray<int32_t>(UncompressedStringStorage::DICTIONARY_HEADER_SIZE, offset_count);
	return {dictionary_data, offsets};
}

ValidatedStringRange StringSegmentLayout::ValidateRange(const idx_t start, const idx_t count) const {
	D_ASSERT(start <= offsets.size());
	D_ASSERT(count <= offsets.size() - start);
	auto range_offsets = offsets.SubArray(start, count);
	const auto previous_offset = start > 0 ? offsets[start - 1] : 0;
	const auto dictionary_size = NumericCast<uint32_t>(dictionary_data.size());
	const auto preceding_dictionary_offset = ValidateAndGetDictionaryOffset(previous_offset, dictionary_size);
	if (DUCKDB_UNLIKELY(count == 0)) {
		return ValidatedStringRange(dictionary_data, range_offsets, preceding_dictionary_offset);
	}

	uint32_t valid = ValidateDictionaryEntryFast(range_offsets[0], previous_offset, dictionary_size);
	// Hot path assumes no errors, so accumulate and later deal with them.
	for (idx_t i = 1; i < count; i++) {
		valid &= ValidateDictionaryEntryFast(range_offsets[i], range_offsets[i - 1], dictionary_size);
	}

	// If we have an error, find it and report it.
	if (DUCKDB_UNLIKELY(!valid)) {
		for (idx_t i = 0; i < count; i++) {
			ValidateDictionaryEntry(range_offsets[i], i > 0 ? range_offsets[i - 1] : previous_offset, dictionary_size);
		}
		throw InternalException("ValidateDictionaryEntryFast and ValidateDictionaryEntry disagree");
	}

	return ValidatedStringRange(dictionary_data, range_offsets, preceding_dictionary_offset);
}

StringDictionaryEntry StringSegmentLayout::ValidateAndGetEntry(const idx_t row_index) const {
	D_ASSERT(row_index < offsets.size());
	const auto current_offset = offsets[row_index];
	const auto previous_offset = row_index > 0 ? offsets[row_index - 1] : 0;
	ValidateDictionaryEntry(current_offset, previous_offset, NumericCast<uint32_t>(dictionary_data.size()));
	const auto previous_dictionary_offset = GetDictionaryOffset(previous_offset);
	const auto current_dictionary_offset = GetDictionaryOffset(current_offset);
	const auto string_length = current_dictionary_offset - previous_dictionary_offset;
	const auto is_overflow = current_offset < 0 && string_length > 0;
	return {dictionary_data.SubArray(dictionary_data.size() - current_dictionary_offset, string_length), is_overflow};
}

//===--------------------------------------------------------------------===//
// Helper Functions
//===--------------------------------------------------------------------===//
void UncompressedStringStorage::SetDictionary(ColumnSegment &segment, BufferHandle &handle,
                                              StringDictionaryContainer container) {
	auto startptr = handle.GetDataMutable() + segment.GetBlockOffset();
	Store<uint32_t>(container.size, startptr);
	Store<uint32_t>(container.end, startptr + sizeof(uint32_t));
}

StringDictionaryContainer UncompressedStringStorage::GetDictionary(ColumnSegment &segment, BufferHandle &handle) {
	auto startptr = handle.Ptr() + segment.GetBlockOffset();
	StringDictionaryContainer container;
	container.size = Load<uint32_t>(startptr);
	container.end = Load<uint32_t>(startptr + sizeof(uint32_t));
	return container;
}

idx_t UncompressedStringStorage::RemainingSpace(ColumnSegment &segment, BufferHandle &handle) {
	auto dictionary = GetDictionary(segment, handle);
	D_ASSERT(dictionary.end == segment.SegmentSize());
	idx_t used_space = dictionary.size + segment.count * sizeof(int32_t) + DICTIONARY_HEADER_SIZE;
	D_ASSERT(segment.SegmentSize() >= used_space);
	return segment.SegmentSize() - used_space;
}

void UncompressedStringStorage::WriteString(ColumnSegment &segment, string_t string, block_id_t &result_block,
                                            int32_t &result_offset) {
	auto &state = segment.GetSegmentState()->Cast<UncompressedStringSegmentState>();
	if (state.overflow_writer) {
		// overflow writer is set: write string there
		state.overflow_writer->WriteString(state, string, result_block, result_offset);
	} else {
		// default overflow behavior: use in-memory buffer to store the overflow string
		WriteStringMemory(segment, string, result_block, result_offset);
	}
}

void UncompressedStringStorage::WriteStringMemory(ColumnSegment &segment, string_t string, block_id_t &result_block,
                                                  int32_t &result_offset) {
	auto total_length = UnsafeNumericCast<uint32_t>(string.GetSize() + sizeof(uint32_t));
	shared_ptr<BlockHandle> block;
	BufferHandle handle;

	auto &buffer_manager = BufferManager::GetBufferManager(segment.GetDatabase());
	auto &state = segment.GetSegmentState()->Cast<UncompressedStringSegmentState>();
	// check if the string fits in the current block
	if (!state.head || state.head->offset + total_length >= state.head->size) {
		// string does not fit, allocate space for it
		// create a new string block
		auto alloc_size = MaxValue<idx_t>(total_length, segment.GetBlockSize());
		auto new_block = make_uniq<StringBlock>();
		new_block->offset = 0;
		new_block->size = alloc_size;
		// allocate an in-memory buffer for it
		handle = buffer_manager.Allocate(MemoryTag::OVERFLOW_STRINGS, alloc_size, false);
		block = handle.GetBlockHandle();
		state.InsertOverflowBlock(block->BlockId(), reference<StringBlock>(*new_block));
		new_block->block = std::move(block);
		new_block->next = std::move(state.head);
		state.head = std::move(new_block);
	} else {
		// string fits, copy it into the current block
		handle = buffer_manager.Pin(state.head->block);
	}

	result_block = state.head->block->BlockId();
	result_offset = UnsafeNumericCast<int32_t>(state.head->offset);

	// copy the string and the length there
	auto ptr = handle.GetDataMutable() + state.head->offset;
	Store<uint32_t>(UnsafeNumericCast<uint32_t>(string.GetSize()), ptr);
	ptr += sizeof(uint32_t);
	memcpy(ptr, string.GetData(), string.GetSize());
	state.head->offset += total_length;
}

string_t UncompressedStringStorage::ReadOverflowString(const QueryContext &context, ColumnSegment &segment,
                                                       Vector &result, block_id_t block, int32_t offset) {
	auto &buffer_manager = segment.GetBlockHandle()->GetMemory().GetBufferManager();
	auto &state = segment.GetSegmentState()->Cast<UncompressedStringSegmentState>();

	if (block < 0) {
		ThrowInvalidOverflowStringBlock();
	}
	if (offset < 0) {
		ThrowOverflowStringOffsetOutOfBounds();
	}

	if (block < MAXIMUM_BLOCK) {
		// read the overflow string from disk
		// pin the initial handle and read the length
		auto block_handle = state.GetHandle(segment.GetBlockHandle()->GetBlockManager(), block);
		auto handle = buffer_manager.Pin(context, block_handle);

		// read header
		auto block_size = segment.GetBlockSize();
		auto string_space = block_size - sizeof(block_id_t);
		CompressionSegmentReader block_reader(handle.Ptr(), block_size, "overflow string block");
		auto reader = block_reader.GetSubReader(0, string_space, "overflow string data");
		reader.SetPosition(NumericCast<idx_t>(offset));
		uint32_t length = reader.Read<uint32_t>();
		uint32_t remaining = length;

		BufferHandle target_handle;
		string_t overflow_string;
		data_ptr_t target_ptr;
		bool allocate_block = length >= segment.GetBlockSize();
		if (allocate_block) {
			// overflow string is bigger than a block - allocate a temporary buffer for it
			target_handle = buffer_manager.Allocate(MemoryTag::OVERFLOW_STRINGS, length);
			target_ptr = target_handle.GetDataMutable();
		} else {
			// overflow string is smaller than a block - add it to the vector directly
			overflow_string = StringVector::EmptyString(result, length);
			target_ptr = data_ptr_cast(overflow_string.GetDataWriteable());
		}

		// now append the string to the single buffer
		while (remaining > 0) {
			idx_t to_write = MinValue<idx_t>(remaining, reader.Remaining());
			reader.ReadBytesInto(target_ptr, to_write);
			remaining -= to_write;
			target_ptr += to_write;
			if (remaining > 0) {
				// read the next block
				block_id_t next_block = block_reader.Get<block_id_t>(string_space);
				block_handle = state.GetHandle(segment.GetBlockHandle()->GetBlockManager(), next_block);
				handle = buffer_manager.Pin(context, block_handle);
				block_reader = CompressionSegmentReader(handle.Ptr(), block_size, "overflow string block");
				reader = block_reader.GetSubReader(0, string_space, "overflow string data");
			}
		}
		if (allocate_block) {
			auto final_buffer = target_handle.Ptr();
			StringVector::AddHandle(result, std::move(target_handle));
			return string_t(const_char_ptr_cast(final_buffer), length);
		} else {
			overflow_string.Finalize();
			return overflow_string;
		}
	}

	// read the overflow string from memory
	// first pin the handle, if it is not pinned yet
	auto string_block = state.FindOverflowBlock(block);
	auto handle = buffer_manager.Pin(context, string_block.get().block);
	auto final_buffer = handle.Ptr();
	StringVector::AddHandle(result, std::move(handle));
	CompressionSegmentReader reader(final_buffer, string_block.get().size, "in-memory overflow string block");
	return ReadStringWithLength(reader, offset);
}

string_t UncompressedStringStorage::ReadStringWithLength(CompressionSegmentReader reader, int32_t offset) {
	reader.SetPosition(NumericCast<idx_t>(offset));
	auto string_length = reader.Read<uint32_t>();
	auto string_data = reader.ReadBytes(string_length);
	return string_t(const_char_ptr_cast(string_data.data()), string_length);
}

void UncompressedStringStorage::WriteStringMarker(data_ptr_t target, block_id_t block_id, int32_t offset) {
	memcpy(target, &block_id, sizeof(block_id_t));
	target += sizeof(block_id_t);
	memcpy(target, &offset, sizeof(int32_t));
}

} // namespace duckdb
