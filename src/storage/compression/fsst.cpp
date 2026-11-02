#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/fsst_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/common/fsst.hpp"

#include "duckdb/common/bitpacking.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/common/random_engine.hpp"
#include "duckdb/function/compression/compression.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/storage/string_uncompressed.hpp"
#include "duckdb/storage/table/column_data_checkpointer.hpp"
#include "duckdb/storage/compression/standard_compression_state.hpp"
#include "duckdb/storage/compression/compression_segment_reader.hpp"
#include "duckdb/main/settings.hpp"

#include "fsst.h"

#include "miniz_wrapper.hpp"

namespace duckdb {
struct FSSTScanState;

// Counts and offsets used during scanning/fetching
//                                         |               ColumnSegment to be scanned / fetched from				 |
//                                         | untouched | bp align | unused d-values | to scan | bp align | untouched |
typedef struct BPDeltaDecodeOffsets {
	idx_t delta_decode_start_row;      //                         X
	idx_t bitunpack_alignment_offset;  //			   <--------->
	idx_t bitunpack_start_row;         //	           X
	idx_t unused_delta_decoded_values; //						  <----------------->
	idx_t scan_offset;                 //			   <---------------------------->
	idx_t total_delta_decode_count;    //					      <-------------------------->
	idx_t total_bitunpack_count;       //              <------------------------------------------------>
} bp_delta_offsets_t;

struct FSSTStorage {
	static constexpr double MINIMUM_COMPRESSION_RATIO = 1.2;
	static constexpr double ANALYSIS_SAMPLE_SIZE = 0.25;

	static unique_ptr<AnalyzeState> StringInitAnalyze(ColumnData &col_data, PhysicalType type);
	static bool StringAnalyze(AnalyzeState &state_p, const Vector &input);
	static idx_t StringFinalAnalyze(AnalyzeState &state_p);

	static unique_ptr<CompressionState> InitCompression(ColumnDataCheckpointData &checkpoint_data,
	                                                    unique_ptr<AnalyzeState> analyze_state_p);
	static void Compress(CompressionState &state_p, const Vector &scan_vector);
	static void FinalizeCompress(CompressionState &state_p);

	static unique_ptr<SegmentScanState> StringInitScan(const QueryContext &context, ColumnSegment &segment);
	template <bool ALLOW_FSST_VECTORS = false>
	static void StringScanPartial(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result,
	                              idx_t result_offset);
	static void StringScan(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result);
	static void Select(ColumnSegment &segment, ColumnScanState &state, idx_t vector_count, Vector &result,
	                   const SelectionVector &sel, idx_t sel_count, idx_t result_offset, ScanVectorType scan_type);

	static void SetDictionary(ColumnSegment &segment, BufferHandle &handle, StringDictionaryContainer container);
	static StringDictionaryContainer GetDictionary(ColumnSegment &segment, BufferHandle &handle);

	static bp_delta_offsets_t CalculateBpDeltaOffsets(int64_t last_known_row, idx_t start, idx_t scan_count);
};

//===--------------------------------------------------------------------===//
// Analyze
//===--------------------------------------------------------------------===//
struct FSSTAnalyzeState : public AnalyzeState {
	explicit FSSTAnalyzeState(BlockManager &block_manager)
	    : AnalyzeState(block_manager), count(0), fsst_string_heap(block_manager.buffer_manager.GetBufferAllocator()),
	      fsst_string_total_size(0), empty_strings(0) {
	}

	~FSSTAnalyzeState() override {
		if (fsst_encoder) {
			duckdb_fsst_destroy(fsst_encoder);
		}
	}

	duckdb_fsst_encoder_t *fsst_encoder = nullptr;
	idx_t count;

	StringHeap fsst_string_heap;
	vector<string_t> fsst_strings;
	size_t fsst_string_total_size;

	RandomEngine random_engine;
	bool have_valid_row = false;

	idx_t empty_strings;
};

unique_ptr<AnalyzeState> FSSTStorage::StringInitAnalyze(ColumnData &col_data, PhysicalType type) {
	auto &storage_manager = col_data.GetStorageManager();
	if (StorageManager::TargetAtLeastVersion(StorageVersion::V1_3_0, storage_manager.GetStorageVersion())) {
		// dict_fsst introduced - disable fsst
		return nullptr;
	}

	return make_uniq<FSSTAnalyzeState>(col_data.GetBlockManager());
}

bool FSSTStorage::StringAnalyze(AnalyzeState &state_p, const Vector &input) {
	auto &state = state_p.Cast<FSSTAnalyzeState>();
	UnifiedVectorFormat vdata;
	input.ToUnifiedFormat(vdata);

	const auto count = input.size();
	state.count += count;
	auto data = UnifiedVectorFormat::GetData<string_t>(vdata);

	// Note that we ignore the sampling in case we have not found any valid strings yet, this solves the issue of
	// not having seen any valid strings here leading to an empty fsst symbol table.
	bool sample_selected = !state.have_valid_row || state.random_engine.NextRandom() < ANALYSIS_SAMPLE_SIZE;

	for (idx_t i = 0; i < count; i++) {
		auto idx = vdata.sel->get_index(i);

		if (!vdata.validity.RowIsValid(idx)) {
			continue;
		}

		// We need to check all strings for this, otherwise we run in to trouble during compression if we miss ones
		auto string_size = data[idx].GetSize();
		if (string_size >= StringUncompressed::GetStringBlockLimit(state.info.GetBlockSize())) {
			return false;
		}

		if (!sample_selected) {
			continue;
		}

		if (string_size > 0) {
			state.have_valid_row = true;
			state.fsst_strings.emplace_back(state.fsst_string_heap.AddBlob(data[idx]));
			state.fsst_string_total_size += string_size;
		} else {
			state.empty_strings++;
		}
	}
	return true;
}

idx_t FSSTStorage::StringFinalAnalyze(AnalyzeState &state_p) {
	auto &state = state_p.Cast<FSSTAnalyzeState>();

	size_t compressed_dict_size = 0;
	size_t max_compressed_string_length = 0;

	auto string_count = state.fsst_strings.size();

	if (!string_count) {
		return DConstants::INVALID_INDEX;
	}

	size_t output_buffer_size = 7 + 2 * state.fsst_string_total_size; // size as specified in fsst.h

	// the sample can be large, so all scratch buffers are allocated through the buffer allocator
	auto &allocator = state.info.GetBlockManager().buffer_manager.GetBufferAllocator();
	auto fsst_string_sizes_data = allocator.Allocate(string_count * sizeof(size_t));
	auto fsst_string_ptrs_data = allocator.Allocate(string_count * sizeof(unsigned char *));
	auto fsst_string_sizes = reinterpret_cast<size_t *>(fsst_string_sizes_data.get());
	auto fsst_string_ptrs = reinterpret_cast<unsigned char **>(fsst_string_ptrs_data.get());
	for (idx_t i = 0; i < string_count; i++) {
		auto &str = state.fsst_strings[i];
		fsst_string_sizes[i] = str.GetSize();
		fsst_string_ptrs[i] = (unsigned char *)str.GetData(); // NOLINT
	}

	state.fsst_encoder = duckdb_fsst_create(string_count, fsst_string_sizes, fsst_string_ptrs, 0);

	// TODO: do we really need to encode to get a size estimate?
	auto compressed_ptrs_data = allocator.Allocate(string_count * sizeof(unsigned char *));
	auto compressed_sizes_data = allocator.Allocate(string_count * sizeof(size_t));
	auto compressed_ptrs = reinterpret_cast<unsigned char **>(compressed_ptrs_data.get());
	auto compressed_sizes = reinterpret_cast<size_t *>(compressed_sizes_data.get());
	auto compressed_buffer = allocator.Allocate(output_buffer_size);

	auto res = duckdb_fsst_compress(state.fsst_encoder, string_count, fsst_string_sizes, fsst_string_ptrs,
	                                output_buffer_size, compressed_buffer.get(), compressed_sizes, compressed_ptrs);

	if (string_count != res) {
		throw std::runtime_error("FSST output buffer is too small unexpectedly");
	}

	// Sum and and Max compressed lengths
	for (idx_t i = 0; i < string_count; i++) {
		const auto size = compressed_sizes[i];
		compressed_dict_size += size;
		max_compressed_string_length = MaxValue(max_compressed_string_length, size);
	}
	D_ASSERT(compressed_dict_size ==
	         (uint64_t)(compressed_ptrs[res - 1] - compressed_ptrs[0]) + compressed_sizes[res - 1]);

	auto minimum_width = BitpackingPrimitives::MinimumBitWidth(max_compressed_string_length);
	auto bitpacked_offsets_size =
	    BitpackingPrimitives::GetRequiredSize(string_count + state.empty_strings, minimum_width);

	auto estimated_base_size = double(bitpacked_offsets_size + compressed_dict_size) * (1 / ANALYSIS_SAMPLE_SIZE);
	auto num_blocks = estimated_base_size / double(state.info.GetBlockSize() - sizeof(duckdb_fsst_decoder_t));
	auto symtable_size = num_blocks * sizeof(duckdb_fsst_decoder_t);
	auto estimated_size = estimated_base_size + symtable_size;

	return LossyNumericCast<idx_t>(estimated_size * MINIMUM_COMPRESSION_RATIO);
}

//===--------------------------------------------------------------------===//
// Compress
//===--------------------------------------------------------------------===//

class FSSTCompressionState : public StandardCompressionState {
public:
	explicit FSSTCompressionState(ColumnDataCheckpointData &checkpoint_data)
	    : StandardCompressionState(checkpoint_data, CompressionType::COMPRESSION_FSST),
	      stats_writer(checkpoint_data.GetType()) {
		CreateEmptySegment();
	}

	~FSSTCompressionState() override {
		if (fsst_encoder) {
			duckdb_fsst_destroy(fsst_encoder);
		}
	}

	void Reset() {
		index_buffer.clear();
		current_width = 0;
		max_compressed_string_length = 0;
		last_fitting_size = 0;

		// Reset the pointers into the current segment
		current_dictionary = FSSTStorage::GetDictionary(*current_segment, handle);
		current_end_ptr = handle.GetDataMutable() + current_dictionary.end;
	}

	void CreateEmptySegment() {
		CreateAndPinNewSegment();
		Reset();
	}

	void UpdateState(string_t uncompressed_string, unsigned char *compressed_string, size_t compressed_string_len) {
		if (!HasEnoughSpace(compressed_string_len)) {
			Flush();
			if (!HasEnoughSpace(compressed_string_len)) {
				throw InternalException("FSST string compression failed due to insufficient space in empty block");
			};
		}

		stats_writer.Update(uncompressed_string);

		// Write string into dictionary
		current_dictionary.size += compressed_string_len;
		auto dict_pos = current_end_ptr - current_dictionary.size;
		memcpy(dict_pos, compressed_string, compressed_string_len);
		current_dictionary.Verify(info.GetBlockSize());

		// We just push the string length to effectively delta encode the strings
		index_buffer.push_back(NumericCast<uint32_t>(compressed_string_len));

		max_compressed_string_length = MaxValue(max_compressed_string_length, compressed_string_len);

		current_width = BitpackingPrimitives::MinimumBitWidth(max_compressed_string_length);
		current_segment->count++;
	}

	void AddEmptyStringInternal() {
		if (!HasEnoughSpace(0)) {
			Flush();
			if (!HasEnoughSpace(0)) {
				throw InternalException("FSST string compression failed due to insufficient space in empty block");
			};
		}
		index_buffer.push_back(0);
		current_segment->count++;
	}

	void AddNull() {
		AddEmptyStringInternal();
		stats_writer.SetHasNull();
	}

	void AddEmptyString() {
		AddEmptyStringInternal();
		stats_writer.Update(string_t(""));
	}

	size_t GetRequiredSize(size_t string_len) {
		bitpacking_width_t required_minimum_width;
		if (string_len > max_compressed_string_length) {
			required_minimum_width = BitpackingPrimitives::MinimumBitWidth(string_len);
		} else {
			required_minimum_width = current_width;
		}

		size_t current_dict_size = current_dictionary.size;
		idx_t current_string_count = index_buffer.size();

		size_t dict_offsets_size =
		    BitpackingPrimitives::GetRequiredSize(current_string_count + 1, required_minimum_width);

		// TODO switch to a symbol table per RowGroup, saves a bit of space
		return sizeof(fsst_compression_header_t) + current_dict_size + dict_offsets_size + string_len +
		       fsst_serialized_symbol_table_size;
	}

	// Checks if there is enough space, if there is, sets last_fitting_size
	bool HasEnoughSpace(size_t string_len) {
		auto required_size = GetRequiredSize(string_len);

		if (required_size <= info.GetBlockSize()) {
			last_fitting_size = required_size;
			return true;
		}
		return false;
	}

	void Flush(bool final = false) {
		auto segment_size = Finalize();
		FlushCurrentSegment(stats_writer, segment_size);

		if (!final) {
			CreateEmptySegment();
		}
	}

	idx_t Finalize() {
		auto &buffer_manager = BufferManager::GetBufferManager(current_segment->GetDatabase());
		auto handle = buffer_manager.Pin(current_segment->GetBlockHandle());
		if (current_dictionary.end != info.GetBlockSize()) {
			throw InternalException("dictionary end does not match the block size in FSSTCompressionState::Finalize");
		}

		// calculate sizes
		auto compressed_index_buffer_size =
		    BitpackingPrimitives::GetRequiredSize(current_segment->count, current_width);
		auto total_size = sizeof(fsst_compression_header_t) + compressed_index_buffer_size + current_dictionary.size +
		                  fsst_serialized_symbol_table_size;
		if (total_size != last_fitting_size) {
			throw InternalException("FSST string compression failed due to incorrect size calculation");
		}

		// calculate ptr and offsets
		auto base_ptr = handle.GetDataMutable();
		auto header_ptr = reinterpret_cast<fsst_compression_header_t *>(base_ptr);
		auto compressed_index_buffer_offset = sizeof(fsst_compression_header_t);
		auto symbol_table_offset = compressed_index_buffer_offset + compressed_index_buffer_size;

		D_ASSERT(current_segment->count == index_buffer.size());
		BitpackingPrimitives::PackBuffer<sel_t, false>(base_ptr + compressed_index_buffer_offset,
		                                               reinterpret_cast<uint32_t *>(index_buffer.data()),
		                                               current_segment->count, current_width);

		// Write the fsst symbol table or nothing
		if (fsst_encoder != nullptr) {
			memcpy(base_ptr + symbol_table_offset, &fsst_serialized_symbol_table[0], fsst_serialized_symbol_table_size);
		} else {
			memset(base_ptr + symbol_table_offset, 0, fsst_serialized_symbol_table_size);
		}

		auto cast_symbol_table_offset = NumericCast<uint32_t>(symbol_table_offset);
		if (cast_symbol_table_offset > info.GetBlockSize()) {
			throw InternalException("invalid fsst_symbol_table_offset in FSSTCompressionState::Finalize");
		}

		Store<uint32_t>(cast_symbol_table_offset, data_ptr_cast(&header_ptr->fsst_symbol_table_offset));
		Store<uint32_t>((uint32_t)current_width, data_ptr_cast(&header_ptr->bitpacking_width));

		if (total_size >= info.GetCompactionFlushLimit()) {
			// the block is full enough, don't bother moving around the dictionary
			// NOTE: We forgot to call SetDictionary here, so dict_size on disk is stale.
			// not worth fixing as it's a legacy encoder, the reader can be bounded by dict_end
			return info.GetBlockSize();
		}

		// the block has space left: figure out how much space we can save
		auto move_amount = info.GetBlockSize() - total_size;
		// move the dictionary so it lines up exactly with the offsets
		auto new_dictionary_offset = symbol_table_offset + fsst_serialized_symbol_table_size;
		memmove(base_ptr + new_dictionary_offset, base_ptr + current_dictionary.end - current_dictionary.size,
		        current_dictionary.size);
		current_dictionary.end -= move_amount;
		D_ASSERT(current_dictionary.end == total_size);
		// write the new dictionary (with the updated "end")
		FSSTStorage::SetDictionary(*current_segment, handle, current_dictionary);

		return total_size;
	}

	// State regarding current segment
	StringDictionaryContainer current_dictionary;
	data_ptr_t current_end_ptr;
	StatsWriter<string_t> stats_writer;

	// Buffers and map for current segment
	vector<uint32_t> index_buffer;

	size_t max_compressed_string_length;
	bitpacking_width_t current_width;
	idx_t last_fitting_size;

	duckdb_fsst_encoder_t *fsst_encoder = nullptr;
	unsigned char fsst_serialized_symbol_table[sizeof(duckdb_fsst_decoder_t)];
	size_t fsst_serialized_symbol_table_size = sizeof(duckdb_fsst_decoder_t);
};

unique_ptr<CompressionState> FSSTStorage::InitCompression(ColumnDataCheckpointData &checkpoint_data,
                                                          unique_ptr<AnalyzeState> analyze_state_p) {
	auto &analyze_state = analyze_state_p->Cast<FSSTAnalyzeState>();
	auto compression_state = make_uniq<FSSTCompressionState>(checkpoint_data);

	if (analyze_state.fsst_encoder == nullptr) {
		throw InternalException("No encoder found during FSST compression");
	}

	compression_state->fsst_encoder = analyze_state.fsst_encoder;
	compression_state->fsst_serialized_symbol_table_size =
	    duckdb_fsst_export(compression_state->fsst_encoder, &compression_state->fsst_serialized_symbol_table[0]);
	analyze_state.fsst_encoder = nullptr;

	return std::move(compression_state);
}

void FSSTStorage::Compress(CompressionState &state_p, const Vector &scan_vector) {
	auto &state = state_p.Cast<FSSTCompressionState>();

	// Get vector data
	UnifiedVectorFormat vdata;
	scan_vector.ToUnifiedFormat(vdata);
	auto data = UnifiedVectorFormat::GetData<string_t>(vdata);

	// Collect pointers to strings to compress
	vector<size_t> sizes_in;
	vector<unsigned char *> strings_in;
	size_t total_size = 0;
	idx_t total_count = 0;
	const auto count = scan_vector.size();
	for (idx_t i = 0; i < count; i++) {
		auto idx = vdata.sel->get_index(i);

		// Note: we treat nulls and empty strings the same
		const bool is_null = !vdata.validity.RowIsValid(idx);
		if (is_null || data[idx].GetSize() == 0) {
			continue;
		}

		total_count++;
		total_size += data[idx].GetSize();
		sizes_in.push_back(data[idx].GetSize());
		strings_in.push_back((unsigned char *)data[idx].GetData()); // NOLINT
	}

	// Only Nulls or empty strings in this vector, nothing to compress
	if (total_count == 0) {
		for (idx_t i = 0; i < count; i++) {
			auto idx = vdata.sel->get_index(i);
			if (!vdata.validity.RowIsValid(idx)) {
				state.AddNull();
			} else if (data[idx].GetSize() == 0) {
				state.AddEmptyString();
			} else {
				throw FatalException("FSST: no encoder found even though there are values to encode");
			}
		}
		return;
	}

	// Compress buffers
	size_t compress_buffer_size = MaxValue<size_t>(total_size * 2 + 7, 1);
	vector<unsigned char *> strings_out(total_count, nullptr);
	vector<size_t> sizes_out(total_count, 0);
	vector<unsigned char> compress_buffer(compress_buffer_size, 0);

	auto res = duckdb_fsst_compress(
	    state.fsst_encoder,   /* IN: encoder obtained from duckdb_fsst_create(). */
	    total_count,          /* IN: number of strings in batch to compress. */
	    &sizes_in[0],         /* IN: byte-lengths of the inputs */
	    &strings_in[0],       /* IN: input string start pointers. */
	    compress_buffer_size, /* IN: byte-length of output buffer. */
	    &compress_buffer[0],  /* OUT: memory buffer to put the compressed strings in (one after the other). */
	    &sizes_out[0],        /* OUT: byte-lengths of the compressed strings. */
	    &strings_out[0]       /* OUT: output string start pointers. Will all point into [output,output+size). */
	);

	if (res != total_count) {
		throw FatalException("FSST compression failed to compress all strings");
	}

	// Push the compressed strings to the compression state one by one
	idx_t compressed_idx = 0;
	for (idx_t i = 0; i < count; i++) {
		auto idx = vdata.sel->get_index(i);
		if (!vdata.validity.RowIsValid(idx)) {
			state.AddNull();
		} else if (data[idx].GetSize() == 0) {
			state.AddEmptyString();
		} else {
			state.UpdateState(data[idx], strings_out[compressed_idx], sizes_out[compressed_idx]);
			compressed_idx++;
		}
	}
}

void FSSTStorage::FinalizeCompress(CompressionState &state_p) {
	auto &state = state_p.Cast<FSSTCompressionState>();
	state.Flush(true);
}

//===--------------------------------------------------------------------===//
// Scan
//===--------------------------------------------------------------------===//
[[noreturn]] static void ThrowInvalidFSSTSegment(const char *reason) {
	throw DataCorruptionException("Failed to read FSST string segment - %s. Database file appears to be corrupted.",
	                              reason);
}

struct FSSTScanState : public SegmentScanState {
	struct SegmentLayout {
		//! Bitpacking width (read from the header and checked to be at most 32 bits)
		bitpacking_width_t width;
		//! Bitpacked string lengths (bounded by the row count and width, before decoding individual lengths)
		CompressionSegmentReader lengths;
		//! Serialized FSST symbol table (bounded by dict_end, narrowed to the imported size on success)
		CompressionSegmentReader symbol_table;
		//! Compressed string bytes (starts after the symbol table once imported, ends at dict_end)
		CompressionSegmentReader dictionary;
	};

	struct ValidatedStringRange {
	public:
		unsafe_array_ptr<const uint8_t> GetCompressedString(idx_t index) const {
			D_ASSERT(index < dictionary_offsets.size());
			auto current_offset = dictionary_offsets[index];
			auto previous_offset = index > 0 ? dictionary_offsets[index - 1] : preceding_offset;
			// Offsets count backwards from the dictionary end, lengths are the differences between offsets.
			return dictionary.SubArray(dictionary.size() - current_offset, current_offset - previous_offset);
		}

		ValidatedStringRange Slice(idx_t start, idx_t count) const {
			D_ASSERT(start <= dictionary_offsets.size());
			D_ASSERT(count <= dictionary_offsets.size() - start);
			auto previous_offset = start > 0 ? dictionary_offsets[start - 1] : preceding_offset;
			return ValidatedStringRange(dictionary, dictionary_offsets.SubArray(start, count), previous_offset);
		}

	private:
		friend struct FSSTScanState;
		ValidatedStringRange(unsafe_array_ptr<const uint8_t> dictionary_p,
		                     unsafe_array_ptr<const uint32_t> dictionary_offsets_p, uint32_t preceding_offset_p)
		    : dictionary(dictionary_p), dictionary_offsets(dictionary_offsets_p), preceding_offset(preceding_offset_p) {
		}

		unsafe_array_ptr<const uint8_t> dictionary;
		unsafe_array_ptr<const uint32_t> dictionary_offsets;
		uint32_t preceding_offset;
	};

	static SegmentLayout ParseFSSTSegmentHeader(CompressionSegmentReader reader, const idx_t segment_count);
	static bool TryImportSymbolTable(SegmentLayout &layout, duckdb_fsst_decoder_t *decoder_out);
	static ValidatedStringRange DecodeAndValidateOffsets(const SegmentLayout &layout, const uint32_t *lengths,
	                                                     uint32_t *dictionary_offsets, idx_t decode_count,
	                                                     uint32_t preceding_offset, bool have_symbol_table);
	//! The returned range borrows the decoding buffer and is valid until the next ValidateRange call.
	ValidatedStringRange ValidateRange(idx_t start, idx_t scan_count);

	explicit FSSTScanState(BufferHandle handle_p, const SegmentLayout &layout, buffer_ptr<void> decoder_p)
	    : duckdb_fsst_decoder(std::move(decoder_p)), handle(std::move(handle_p)), layout(layout) {
		ResetStoredDelta();
	}

	buffer_ptr<void> duckdb_fsst_decoder;
	void *duckdb_fsst_decoder_ptr = nullptr;
	BufferHandle handle;
	SegmentLayout layout;

	// To speed up delta decoding we store the last index
	uint32_t last_known_index;
	int64_t last_known_row;

	unsafe_unique_array<uint32_t> bitunpack_buffer;
	idx_t bitunpack_buffer_capacity = 0;
	unsafe_unique_array<uint32_t> delta_decode_buffer;
	idx_t delta_decode_capacity = 0;

	bool all_values_inlined = false;

	void StoreLastDelta(uint32_t value, int64_t row) {
		last_known_index = value;
		last_known_row = row;
	}
	void ResetStoredDelta() {
		last_known_index = 0;
		last_known_row = -1;
	}
	inline string_t DecompressString(unsafe_array_ptr<const uint8_t> compressed_string,
	                                 ArenaAllocator &str_allocator) const {
		auto str_len = compressed_string.size();
		if (str_len == 0) {
			return string_t(nullptr, 0);
		}
		auto str_ptr = const_char_ptr_cast(compressed_string.data());
		if (all_values_inlined) {
			return FSSTPrimitives::DecompressInlinedValue(duckdb_fsst_decoder_ptr, str_ptr, str_len);
		} else {
			return FSSTPrimitives::DecompressValue(duckdb_fsst_decoder_ptr, str_allocator, str_ptr, str_len);
		}
	}
};

unique_ptr<SegmentScanState> FSSTStorage::StringInitScan(const QueryContext &context, ColumnSegment &segment) {
	auto &buffer_manager = BufferManager::GetBufferManager(segment.GetDatabase());
	auto handle = buffer_manager.Pin(context, segment.GetBlockHandle());
	auto layout = FSSTScanState::ParseFSSTSegmentHeader(CompressionSegmentReader::FromSegment(handle, segment, "FSST"),
	                                                    segment.count.load());
	auto decoder = make_buffer<duckdb_fsst_decoder_t>();
	if (!FSSTScanState::TryImportSymbolTable(layout, decoder.get())) {
		decoder = nullptr;
	}
	auto state = make_uniq<FSSTScanState>(std::move(handle), layout, std::move(decoder));
	state->duckdb_fsst_decoder_ptr = state->duckdb_fsst_decoder.get();

	const auto &stats = segment.GetStats();
	if (stats.GetStatsType() == StatisticsType::STRING_STATS && StringStats::HasMaxStringLength(stats)) {
		state->all_values_inlined = StringStats::MaxStringLength(stats) <= string_t::INLINE_LENGTH;
	}

	return std::move(state);
}

FSSTScanState::ValidatedStringRange
FSSTScanState::DecodeAndValidateOffsets(const SegmentLayout &layout, const uint32_t *lengths,
                                        uint32_t *dictionary_offsets, idx_t decode_count, uint32_t preceding_offset,
                                        bool have_symbol_table) {
	auto dictionary_size = layout.dictionary.Size();
	if (preceding_offset > dictionary_size) {
		ThrowInvalidFSSTSegment("dictionary offset was out of range");
	}
	auto dictionary_offset = preceding_offset;
	for (idx_t i = 0; i < decode_count; i++) {
		auto string_length = lengths[i];
		// Check the remaining dictionary bytes before adding the length so the offset cannot wrap.
		if (string_length > dictionary_size - dictionary_offset) {
			ThrowInvalidFSSTSegment("string length exceeded the remaining dictionary bytes");
		}
		dictionary_offset += string_length;
		dictionary_offsets[i] = dictionary_offset;
	}
	if (!have_symbol_table && dictionary_offset != preceding_offset) {
		ThrowInvalidFSSTSegment("nonempty string had no symbol table");
	}
	return ValidatedStringRange(layout.dictionary.GetBytes(0, dictionary_size),
	                            unsafe_array_ptr<const uint32_t>(dictionary_offsets, decode_count), preceding_offset);
}

void BitUnpackRange(const CompressionSegmentReader &lengths, data_ptr_t dst_ptr, idx_t count, idx_t row,
                    bitpacking_width_t width) {
	constexpr auto group_size = BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE;
	D_ASSERT(row % group_size == 0);
	D_ASSERT(width <= sizeof(uint32_t) * 8);
	auto start_group = row / group_size;
	// Include the final partial group because UnPackBuffer reads a full group even for fewer values.
	auto group_count = count / group_size + (count % group_size != 0);
	auto bytes_per_group = (group_size * static_cast<idx_t>(width)) / 8;
	// Unpacking reads complete groups. Check their range before multiplying by the group byte size.
	if (DUCKDB_LIKELY(bytes_per_group > 0)) {
		auto available_groups = lengths.Size() / bytes_per_group;
		if (start_group > available_groups || group_count > available_groups - start_group) {
			ThrowInvalidFSSTSegment("bitpacking buffer was out of range");
		}
	}
	auto source = lengths.GetBytes(start_group * bytes_per_group, group_count * bytes_per_group);
	BitpackingPrimitives::UnPackBuffer<uint32_t>(dst_ptr, source.data(), count, width);
}

//===--------------------------------------------------------------------===//
// Scan base data
//===--------------------------------------------------------------------===//
FSSTScanState::ValidatedStringRange FSSTScanState::ValidateRange(idx_t start, idx_t scan_count) {
	if (scan_count == 0) {
		// No decoding buffer is needed for an empty range, leave the cached position unchanged.
		return ValidatedStringRange(layout.dictionary.GetBytes(0, layout.dictionary.Size()),
		                            unsafe_array_ptr<const uint32_t>(&last_known_index, 0), last_known_index);
	}
	// Offsets are cumulative, so a cached offset at or past the requested start cannot be reused.
	if (start == 0 || last_known_row >= (int64_t)start) {
		ResetStoredDelta();
	}

	// Decode from the cached position through the requested range, including any skipped rows.
	// Unpacking also includes padding to complete the first and last bitpacking groups.
	const auto offsets = FSSTStorage::CalculateBpDeltaOffsets(last_known_row, start, scan_count);

	if (bitunpack_buffer_capacity < offsets.total_bitunpack_count) {
		bitunpack_buffer = make_unsafe_uniq_array_uninitialized<uint32_t>(offsets.total_bitunpack_count);
		bitunpack_buffer_capacity = offsets.total_bitunpack_count;
	}
	BitUnpackRange(layout.lengths, data_ptr_cast(bitunpack_buffer.get()), offsets.total_bitunpack_count,
	               offsets.bitunpack_start_row, layout.width);
	if (delta_decode_capacity < offsets.total_delta_decode_count) {
		delta_decode_buffer = make_unsafe_uniq_array_uninitialized<uint32_t>(offsets.total_delta_decode_count);
		delta_decode_capacity = offsets.total_delta_decode_count;
	}
	const auto strings = DecodeAndValidateOffsets(layout, bitunpack_buffer.get() + offsets.bitunpack_alignment_offset,
	                                              delta_decode_buffer.get(), offsets.total_delta_decode_count,
	                                              last_known_index, duckdb_fsst_decoder != nullptr);
	// Cache the final offset only after all decoded lengths have passed validation.
	StoreLastDelta(delta_decode_buffer[offsets.total_delta_decode_count - 1],
	               UnsafeNumericCast<int64_t>(start + scan_count - 1));
	// Preceding rows were needed to calculate offsets, but are not part of the requested range.
	return strings.Slice(offsets.unused_delta_decoded_values, scan_count);
}

template <bool ALLOW_FSST_VECTORS>
void FSSTStorage::StringScanPartial(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result,
                                    idx_t result_offset) {
	auto &scan_state = state.scan_state->Cast<FSSTScanState>();
	auto start = state.GetPositionInSegment();
	D_ASSERT(start <= segment.count.load());
	D_ASSERT(scan_count <= segment.count.load() - start);

	bool enable_fsst_vectors = false;
	// With a decoder and when enabled, use the FSST vector path.
	if (ALLOW_FSST_VECTORS && scan_state.duckdb_fsst_decoder) {
		enable_fsst_vectors = Settings::Get<EnableFSSTVectorsSetting>(segment.GetDatabase());
	}

	string_t *result_data;

	if (scan_count == 0) {
		return;
	}

	if (enable_fsst_vectors) {
		D_ASSERT(result_offset == 0);
		auto string_block_limit = StringUncompressed::GetStringBlockLimit(segment.GetBlockSize());
		FSSTVector::Create(result, scan_state.duckdb_fsst_decoder, string_block_limit, scan_count);
		result_data = FSSTVector::GetCompressedData(result);
	} else {
		D_ASSERT(result.GetVectorType() == VectorType::FLAT_VECTOR);
		result_data = FlatVector::GetDataMutable<string_t>(result);
	}

	auto strings = scan_state.ValidateRange(start, scan_count);
	if (enable_fsst_vectors) {
		// The result could outlive the ScanState, so keep the segment block pinned with the result vector
		auto &buffer_manager = BufferManager::GetBufferManager(segment.GetDatabase());
		StringVector::AddHandle(result, buffer_manager.Pin(state.context, segment.GetBlockHandle()));
		for (idx_t i = 0; i < scan_count; i++) {
			auto compressed_string = strings.GetCompressedString(i);
			result_data[i] = string_t(const_char_ptr_cast(compressed_string.data()),
			                          NumericCast<uint32_t>(compressed_string.size()));
		}
		FSSTVector::SetCount(result, scan_count);
	} else {
		// Just decompress
		auto &str_allocator = StringVector::GetStringAllocator(result);
		for (idx_t i = 0; i < scan_count; i++) {
			result_data[i + result_offset] = scan_state.DecompressString(strings.GetCompressedString(i), str_allocator);
		}
	}
}

void FSSTStorage::StringScan(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result) {
	StringScanPartial<true>(segment, state, scan_count, result, 0);
}

//===--------------------------------------------------------------------===//
// Select
//===--------------------------------------------------------------------===//
void FSSTStorage::Select(ColumnSegment &segment, ColumnScanState &state, idx_t vector_count, Vector &result,
                         const SelectionVector &sel, idx_t sel_count, idx_t result_offset, ScanVectorType) {
	auto &scan_state = state.scan_state->Cast<FSSTScanState>();
	auto start = state.GetPositionInSegment();
	D_ASSERT(start <= segment.count.load());
	D_ASSERT(vector_count <= segment.count.load() - start);
	D_ASSERT(sel_count <= vector_count);
	D_ASSERT(result.GetVectorType() == VectorType::FLAT_VECTOR);

	auto &str_allocator = StringVector::GetStringAllocator(result);
	auto strings = scan_state.ValidateRange(start, vector_count);
	auto result_data = FlatVector::GetDataMutable<string_t>(result);

	for (idx_t i = 0; i < sel_count; i++) {
		idx_t index = sel.get_index(i);
		D_ASSERT(index < vector_count);
		result_data[result_offset + i] = scan_state.DecompressString(strings.GetCompressedString(index), str_allocator);
	}
}

//===--------------------------------------------------------------------===//
// Get Function
//===--------------------------------------------------------------------===//
CompressionFunction FSSTFun::GetFunction(PhysicalType data_type) {
	D_ASSERT(data_type == PhysicalType::VARCHAR);
	return CompressionFunction(CompressionType::COMPRESSION_FSST, data_type, FSSTStorage::StringInitAnalyze,
	                           FSSTStorage::StringAnalyze, FSSTStorage::StringFinalAnalyze,
	                           FSSTStorage::InitCompression, FSSTStorage::Compress, FSSTStorage::FinalizeCompress,
	                           FSSTStorage::StringInitScan, FSSTStorage::StringScan,
	                           FSSTStorage::StringScanPartial<false>, UncompressedFunctions::EmptySkip,
	                           UncompressedStringStorage::StringInitSegment, nullptr, nullptr, nullptr, nullptr,
	                           nullptr, nullptr, nullptr, nullptr, FSSTStorage::Select);
}

bool FSSTFun::TypeIsSupported(const PhysicalType physical_type) {
	return physical_type == PhysicalType::VARCHAR;
}

//===--------------------------------------------------------------------===//
// Helper Functions
//===--------------------------------------------------------------------===//
void FSSTStorage::SetDictionary(ColumnSegment &segment, BufferHandle &handle, StringDictionaryContainer container) {
	auto header_ptr = reinterpret_cast<fsst_compression_header_t *>(handle.GetDataMutable() + segment.GetBlockOffset());
	Store<uint32_t>(container.size, data_ptr_cast(&header_ptr->dict_size));
	Store<uint32_t>(container.end, data_ptr_cast(&header_ptr->dict_end));
}

StringDictionaryContainer FSSTStorage::GetDictionary(ColumnSegment &segment, BufferHandle &handle) {
	auto header_ptr = reinterpret_cast<fsst_compression_header_t *>(handle.GetDataMutable() + segment.GetBlockOffset());
	StringDictionaryContainer container;
	container.size = Load<uint32_t>(data_ptr_cast(&header_ptr->dict_size));
	container.end = Load<uint32_t>(data_ptr_cast(&header_ptr->dict_end));
	return container;
}

FSSTScanState::SegmentLayout FSSTScanState::ParseFSSTSegmentHeader(CompressionSegmentReader reader,
                                                                   const idx_t segment_count) {
	const auto header = reader.Read<fsst_compression_header_t>();
	if (header.bitpacking_width > sizeof(uint32_t) * 8) {
		ThrowInvalidFSSTSegment("bitpacking width was invalid");
	}
	auto width = UnsafeNumericCast<bitpacking_width_t>(header.bitpacking_width);
	// Count groups before multiplying so a corrupt row count cannot overflow the buffer size.
	constexpr auto group_size = BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE;
	auto group_count = segment_count / group_size + (segment_count % group_size != 0);
	auto bytes_per_group = (group_size * idx_t(width)) / 8;
	if (bytes_per_group && group_count > reader.Remaining() / bytes_per_group) {
		ThrowInvalidFSSTSegment("bitpacking buffer was out of range");
	}
	auto lengths = reader.ReadSubReader(group_count * bytes_per_group, "FSST lengths");
	if (header.fsst_symbol_table_offset != reader.Position()) {
		ThrowInvalidFSSTSegment("bitpacking width did not match the stored layout");
	}
	if (header.dict_end < reader.Position()) {
		ThrowInvalidFSSTSegment("dictionary was out of range");
	}

	// Non-compacted blocks leave dict_size at zero, even when they contain strings.
	// Bound the symbol table input by dict_end. Importing it establishes the dictionary's lower bound.
	auto symbol_table = reader.ReadSubReader(header.dict_end - reader.Position(), "FSST symbol table");
	auto dictionary = symbol_table.GetSubReader(0, symbol_table.Size(), "FSST dictionary");
	return {width, lengths, symbol_table, dictionary};
}

bool FSSTScanState::TryImportSymbolTable(SegmentLayout &layout, duckdb_fsst_decoder_t *decoder_out) {
	auto symbol_table_data = layout.symbol_table.GetBytes(0, layout.symbol_table.Size());
	const auto consumed = duckdb_fsst_import(decoder_out, symbol_table_data.data(), symbol_table_data.size());
	// an inconsistent header is corruption; a version mismatch just means there is no symbol table (all strings are
	// empty/null), in which case we report "no table" and the scan continues without a decoder
	if (consumed == DUCKDB_FSST_IMPORT_OUT_OF_BOUNDS) {
		ThrowInvalidFSSTSegment("symbol table was out of range");
	}

	// Currently, we allow an empty symbol table for a row group all strings are of length 0.
	// This case is detected by reading a zeroed symbol table, which will fail on VERSION_MISMATCH
	// as the data we are reading is not really a fsst symbol table.
	if (consumed == DUCKDB_FSST_IMPORT_VERSION_MISMATCH) {
		return false;
	}
	// Dictionary offsets count backwards from dict_end and must not reach into the symbol table.
	layout.dictionary =
	    layout.dictionary.GetSubReader(consumed, layout.dictionary.Size() - consumed, "FSST dictionary");
	layout.symbol_table = layout.symbol_table.GetSubReader(0, consumed, "FSST symbol table");
	return true;
}

// The calculation of offsets and counts while scanning or fetching is a bit tricky, for two reasons:
// - bitunpacking needs to be aligned to BITPACKING_ALGORITHM_GROUP_SIZE
// - delta decoding needs to decode from the last known value.
bp_delta_offsets_t FSSTStorage::CalculateBpDeltaOffsets(int64_t last_known_row, idx_t start, idx_t scan_count) {
	D_ASSERT(last_known_row >= -1);
	// The next scan resumes at last_known_row + 1, which is start + scan_count.
	// Keep that value within int64_t.
	const auto max_row = static_cast<idx_t>(NumericLimits<int64_t>::Maximum());
	if (start > max_row || scan_count > max_row - start) {
		ThrowInvalidFSSTSegment("row range exceeded the supported limit");
	}
	bp_delta_offsets_t result;

	result.delta_decode_start_row = last_known_row < 0 ? 0 : static_cast<idx_t>(last_known_row) + 1;
	D_ASSERT(result.delta_decode_start_row <= start);
	result.bitunpack_alignment_offset =
	    result.delta_decode_start_row % BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE;
	result.bitunpack_start_row = result.delta_decode_start_row - result.bitunpack_alignment_offset;
	result.unused_delta_decoded_values = start - result.delta_decode_start_row;
	result.scan_offset = result.bitunpack_alignment_offset + result.unused_delta_decoded_values;
	result.total_delta_decode_count = scan_count + result.unused_delta_decoded_values;
	result.total_bitunpack_count =
	    BitpackingPrimitives::RoundUpToAlgorithmGroupSize<idx_t>(scan_count + result.scan_offset);
	// Both decoding buffers contain uint32_t values, including complete groups in the unpack buffer.
	if (result.total_bitunpack_count > NumericLimits<size_t>::Maximum() / sizeof(uint32_t)) {
		ThrowInvalidFSSTSegment("decoding buffer size was out of range");
	}

	D_ASSERT(result.total_delta_decode_count + result.bitunpack_alignment_offset <= result.total_bitunpack_count);
	return result;
}

} // namespace duckdb
