#include "duckdb.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/common/allocator.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/function/compression/compression.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/storage/table/column_data_checkpointer.hpp"
#include "duckdb/storage/block_manager.hpp"
#include "duckdb/storage/checkpoint/string_checkpoint_state.hpp"
#include "duckdb/storage/partial_block_manager.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/storage/compression/compression_segment_reader.hpp"
#include "duckdb/storage/string_uncompressed.hpp"
#include "duckdb/storage/table/data_table_info.hpp"

#include "duckdb/storage/compression/zstd/zstd.hpp"
#include "zstd.h"

/*
Data layout per segment:
+--------------------------------------------+
|            Vector Metadata                 |
|   +------------------------------------+   |
|   |   int64_t  page_id[]               |   |
|   |   uint32_t page_offset[]           |   |
|   |   uint64_t uncompressed_size[]     |   |
|   |   uint64_t compressed_size[]       |   |
|   +------------------------------------+   |
|                                            |
+--------------------------------------------+
|              [Vector Data]+                |
|   +------------------------------------+   |
|   |   uint32_t lengths[]               |   |
|   |   void    *compressed_data         |   |
|   +------------------------------------+   |
|                                            |
+--------------------------------------------+
*/

static int32_t GetCompressionLevel() {
	return duckdb_zstd::ZSTD_defaultCLevel();
}

static constexpr idx_t ZSTD_VECTOR_SIZE = STANDARD_VECTOR_SIZE > 2048 ? STANDARD_VECTOR_SIZE : 2048;

namespace duckdb {

static idx_t GetWritableSpace(const CompressionInfo &info) {
	return info.GetBlockSize() - sizeof(block_id_t);
}

static idx_t GetVectorCount(idx_t count) {
	idx_t vector_count = count / ZSTD_VECTOR_SIZE;
	vector_count += (count % ZSTD_VECTOR_SIZE) != 0;
	return vector_count;
}

static idx_t GetVectorMetadataSize(idx_t vector_count) {
	idx_t vector_metadata_size = 0;
	vector_metadata_size += sizeof(page_id_t) * vector_count;

	vector_metadata_size = AlignValue<idx_t, sizeof(page_offset_t)>(vector_metadata_size);
	vector_metadata_size += sizeof(page_offset_t) * vector_count;

	vector_metadata_size = AlignValue<idx_t, sizeof(uncompressed_size_t)>(vector_metadata_size);
	vector_metadata_size += sizeof(uncompressed_size_t) * vector_count;

	vector_metadata_size = AlignValue<idx_t, sizeof(compressed_size_t)>(vector_metadata_size);
	vector_metadata_size += sizeof(compressed_size_t) * vector_count;
	return vector_metadata_size;
}

struct ZSTDStorage {
	static unique_ptr<AnalyzeState> StringInitAnalyze(ColumnData &col_data, PhysicalType type);
	static bool StringAnalyze(AnalyzeState &state_p, const Vector &input);
	static idx_t StringFinalAnalyze(AnalyzeState &state_p);

	static unique_ptr<CompressionState> InitCompression(ColumnDataCheckpointData &checkpoint_data,
	                                                    unique_ptr<AnalyzeState> analyze_state_p);
	static void Compress(CompressionState &state_p, const Vector &scan_vector);
	static void FinalizeCompress(CompressionState &state_p);

	static unique_ptr<SegmentScanState> StringInitScan(const QueryContext &context, ColumnSegment &segment);
	static void StringScanPartial(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result,
	                              idx_t result_offset);
	static void StringScan(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result);
	static void StringSkip(ColumnSegment &segment, ColumnScanState &state, idx_t skip_count) {
		// NO OP
	}

	// Segment state metadata
	// Required because we are creating additional pages that have to be cleaned up

	static unique_ptr<CompressedSegmentState> StringInitSegment(ColumnSegment &segment, block_id_t block_id,
	                                                            optional_ptr<ColumnSegmentState> segment_state);
	static unique_ptr<ColumnSegmentState> SerializeState(ColumnSegment &segment);
	static unique_ptr<ColumnSegmentState> DeserializeState(Deserializer &deserializer);
	static void VisitBlockIds(const ColumnSegment &segment, BlockIdVisitor &visitor);
};

//===--------------------------------------------------------------------===//
// Analyze
//===--------------------------------------------------------------------===//

struct ZSTDAnalyzeState : public AnalyzeState {
public:
	ZSTDAnalyzeState(BlockManager &block_manager, DBConfig &config)
	    : AnalyzeState(block_manager), config(config), context(nullptr) {
		context = duckdb_zstd::ZSTD_createCCtx();
	}
	~ZSTDAnalyzeState() override {
		duckdb_zstd::ZSTD_freeCCtx(context);
	}

public:
	inline void AppendString(const string_t &str) {
		auto string_size = str.GetSize();
		total_size += string_size;
	}

public:
	DBConfig &config;

	duckdb_zstd::ZSTD_CCtx *context = nullptr;
	//! The combined string lengths for all values in the segment
	idx_t total_size = 0;
	//! The total amount of values in the segment
	idx_t count = 0;

	//! The amount of vectors per filled segment
	idx_t vectors_per_segment = 0;
	//! The total amount of segments we will create
	idx_t segment_count = 0;
	//! Current vector in the segment
	idx_t vectors_in_segment = 0;
	//! Current amount of values in the vector
	idx_t values_in_vector = 0;
};

unique_ptr<AnalyzeState> ZSTDStorage::StringInitAnalyze(ColumnData &col_data, PhysicalType type) {
	// check if the storage version we are writing to supports sztd
	auto &storage = col_data.GetStorageManager();
	auto &block_manager = col_data.GetBlockManager();
	if (block_manager.InMemory()) {
		//! Can't use ZSTD in in-memory environment
		return nullptr;
	}
	if (StorageManager::IsPriorToVersion(StorageVersion::V1_2_0, storage.GetStorageVersion())) {
		// compatibility mode with old versions - disable zstd
		return nullptr;
	}
	auto &data_table_info = col_data.info;
	auto &attached_db = data_table_info.GetDB();
	auto &config = DBConfig::Get(attached_db);

	return make_uniq<ZSTDAnalyzeState>(col_data.GetBlockManager(), config);
}

bool ZSTDStorage::StringAnalyze(AnalyzeState &state_p, const Vector &input) {
	auto &state = state_p.Cast<ZSTDAnalyzeState>();
	UnifiedVectorFormat vdata;
	input.ToUnifiedFormat(vdata);

	const auto count = input.size();
	auto data = UnifiedVectorFormat::GetData<string_t>(vdata);
	for (idx_t i = 0; i < count; i++) {
		auto idx = vdata.sel->get_index(i);
		if (!vdata.validity.RowIsValid(idx)) {
			continue;
		}
		auto &str = data[idx];
		auto string_size = str.GetSize();
		state.total_size += string_size;
	}
	state.values_in_vector += count;
	while (state.values_in_vector >= ZSTD_VECTOR_SIZE) {
		if (GetVectorMetadataSize(state.vectors_in_segment + 1) > GetWritableSpace(state.info)) {
			state.vectors_per_segment = state.vectors_in_segment;
			state.segment_count++;
			state.vectors_in_segment = 0;
		} else {
			state.vectors_in_segment++;
		}
		state.values_in_vector -= ZSTD_VECTOR_SIZE;
	}
	state.count += count;
	return true;
}

// Compression score to determine which compression to use
idx_t ZSTDStorage::StringFinalAnalyze(AnalyzeState &state_p) {
	auto &state = state_p.Cast<ZSTDAnalyzeState>();

	if (!state.count) {
		return NumericLimits<idx_t>::Maximum();
	}

	if (state.values_in_vector) {
		D_ASSERT(state.values_in_vector < ZSTD_VECTOR_SIZE);
		state.vectors_in_segment++;
	}
	if (state.vectors_in_segment) {
		state.segment_count++;
	}

	double penalty;
	idx_t average_length = state.total_size / state.count;
	auto threshold = Settings::Get<ZstdMinStringLengthSetting>(state.config);
	if (average_length >= threshold) {
		penalty = 1.0;
	} else {
		// set a high penalty if we are below the threshold - this still allows ZSTD to be forced
		// but makes it very unlikely to be chosen automatically
		penalty = 1000.0;
	}

	auto expected_compressed_size = (double)state.total_size / 2.0;

	idx_t estimated_size = 0;
	estimated_size += LossyNumericCast<idx_t>(expected_compressed_size);

	estimated_size += state.count * sizeof(string_length_t);
	estimated_size += GetVectorMetadataSize(GetVectorCount(state.count));

	return LossyNumericCast<idx_t>((double)estimated_size * penalty);
}

//===--------------------------------------------------------------------===//
// Compress
//===--------------------------------------------------------------------===//

class ZSTDCompressionState : public CompressionState {
public:
	explicit ZSTDCompressionState(ColumnDataCheckpointData &checkpoint_data,
	                              unique_ptr<ZSTDAnalyzeState> &&analyze_state_p)
	    : CompressionState(checkpoint_data, CompressionType::COMPRESSION_ZSTD),
	      analyze_state(std::move(analyze_state_p)),
	      partial_block_manager(checkpoint_data.GetCheckpointState().GetPartialBlockManager()),
	      stats_writer(checkpoint_data.GetType()), total_tuple_count(analyze_state->count),
	      total_vector_count(GetVectorCount(total_tuple_count)), total_segment_count(analyze_state->segment_count),
	      vectors_per_segment(analyze_state->vectors_per_segment) {
		segment_count = 0;
		vector_count = 0;
		vector_state.tuple_count = 0;

		NewSegment();
		if (!(buffer_collection.GetCurrentOffset() <= GetWritableSpace(info))) {
			throw InternalException(
			    "(ZSTDCompressionState::ZSTDCompressionState) Offset (%d) exceeds writable space! (%d)",
			    buffer_collection.GetCurrentOffset(), GetWritableSpace(info));
		}
	}

public:
	void ResetOutBuffer() {
		if (!(buffer_collection.GetCurrentOffset() <= GetWritableSpace(info))) {
			throw InternalException("(ZSTDCompressionState::ResetOutBuffer) Offset (%d) exceeds writable space! (%d)",
			                        buffer_collection.GetCurrentOffset(), GetWritableSpace(info));
		}
		out_buffer.dst = buffer_collection.GetCurrentBufferPtr();
		out_buffer.pos = 0;

		auto remaining_space = info.GetBlockSize() - buffer_collection.GetCurrentOffset() - sizeof(block_id_t);
		out_buffer.size = remaining_space;
	}

	void WriteBlockIdPointer(page_id_t block_id) {
		auto ptr = buffer_collection.GetCurrentBufferPtr();
		Store<block_id_t>(block_id, ptr);
		buffer_collection.GetCurrentOffset() += sizeof(block_id_t);
	}

	void GetExtraPageBuffer(block_id_t current_block_id) {
		auto &block_manager = partial_block_manager.GetBlockManager();
		auto &buffer_manager = block_manager.buffer_manager;

		auto &current_buffer_state = buffer_collection.GetCurrentBufferState();
		current_buffer_state.full = true;

		if (buffer_collection.CanFlush()) {
			auto &buffer_state = buffer_collection.GetCurrentBufferState();
			FlushPage(buffer_collection.BufferHandleMutable(), current_block_id);
			buffer_state.flags.Clear();
			buffer_state.full = false;
			buffer_state.offset = 0;
			return;
		}

		//! Cycle through the extra pages, to figure out which one we can use
		//! In the worst case, the segment handle is entirely filled with vector metadata
		//! The last part of the first extra page is entirely filled with string metadata
		//! So we can only use the second extra page for data
		auto buffer_data = buffer_collection.GetBufferData(/*include_segment = */ false);
		for (auto &buffer : buffer_data) {
			auto &buffer_state = buffer.state;
			auto &flags = buffer_state.flags;
			if (flags.HasStringMetadata() || buffer_state.full) {
				continue;
			}
			buffer_collection.SetCurrentBuffer(buffer.slot);
			auto &buffer_handle = buffer_collection.BufferHandleMutable();
			if (!buffer_handle.IsValid()) {
				buffer_handle = buffer_manager.Allocate(partial_block_manager.GetClientContext(),
				                                        MemoryTag::OVERFLOW_STRINGS, &block_manager);
			}
			return;
		}
		throw InternalException(
		    "(ZSTDCompressionState::GetExtraPageBuffer) Wasn't able to find a buffer to write overflow data to!");
	}

	void NewSegment() {
		if (buffer_collection.IsOnSegmentBuffer()) {
			// This should never happen, the string lengths + vector metadata size should always exceed a page size,
			// even if the strings are all empty
			throw InternalException("(ZSTDCompressionState::NewSegment) We are asking for a new segment, but somehow "
			                        "we're still writing vector data onto "
			                        "the initial (segment) page");
		}
		FlushSegment();
		CreateEmptySegment();

		// Figure out how many vectors we are storing in this segment
		idx_t vectors_in_segment;
		if (segment_count + 1 >= total_segment_count) {
			vectors_in_segment = total_vector_count - (segment_count * vectors_per_segment);
		} else {
			vectors_in_segment = vectors_per_segment;
		}

		buffer_collection.SetCurrentBuffer(ZSTDCompressionBufferCollection::Slot::SEGMENT);
		buffer_collection.buffer_states[0].flags.SetVectorMetadata();
		segment_state.InitializeSegment(buffer_collection, vectors_in_segment);
		if (!(buffer_collection.GetCurrentOffset() <= GetWritableSpace(info))) {
			throw InternalException("(ZSTDCompressionState::NewSegment) Offset (%d) exceeds writable space! (%d)",
			                        buffer_collection.GetCurrentOffset(), GetWritableSpace(info));
		}
	}

	void InitializeVector() {
		D_ASSERT(!vector_state.in_vector);
		idx_t expected_tuple_count;
		if (vector_count + 1 >= total_vector_count) {
			//! Last vector
			expected_tuple_count = analyze_state->count - (ZSTD_VECTOR_SIZE * vector_count);
		} else {
			expected_tuple_count = ZSTD_VECTOR_SIZE;
		}
		buffer_collection.AlignCurrentOffset();
		if (!(buffer_collection.GetCurrentOffset() <= GetWritableSpace(info))) {
			throw InternalException("(ZSTDCompressionState::InitializeVector) Offset (%d) exceeds writable space! (%d)",
			                        buffer_collection.GetCurrentOffset(), GetWritableSpace(info));
		}
		vector_state.compressed_size = 0;
		vector_state.uncompressed_size = 0;
		vector_state.string_lengths = nullptr;
		vector_state.tuple_count = 0;
		vector_state.vector_size = 0;
		vector_state.starting_page = 0XDEADBEEF;
		vector_state.starting_offset = 0XDEADBEEF;

		if (segment_state.vector_in_segment_count + 1 > segment_state.total_vectors_in_segment) {
			//! Last vector in the segment
			NewSegment();
		}

		if (buffer_collection.GetCurrentOffset() + (expected_tuple_count * sizeof(string_length_t)) >=
		    GetWritableSpace(info)) {
			// Check if there is room on the current page for the vector data
			NewPage();
		}

		buffer_collection.AlignCurrentOffset();
		vector_state.Initialize(expected_tuple_count, buffer_collection, info);

		// 'out_buffer' should be set to point directly after the string_lengths
		ResetOutBuffer();

		// Initialize the context for streaming compression
		duckdb_zstd::ZSTD_CCtx_reset(analyze_state->context, duckdb_zstd::ZSTD_reset_session_only);
		duckdb_zstd::ZSTD_CCtx_refCDict(analyze_state->context, nullptr);
		duckdb_zstd::ZSTD_CCtx_setParameter(analyze_state->context, duckdb_zstd::ZSTD_c_compressionLevel,
		                                    GetCompressionLevel());
		vector_state.in_vector = true;
	}

	void CompressString(const string_t &string, bool end_of_vector) {
		duckdb_zstd::ZSTD_inBuffer in_buffer = {/*data = */ string.GetData(),
		                                        /*length = */ size_t(string.GetSize()),
		                                        /*pos = */ 0};

		if (!end_of_vector && string.GetSize() == 0) {
			return;
		}
		vector_state.uncompressed_size += string.GetSize();
		const auto end_mode = end_of_vector ? duckdb_zstd::ZSTD_e_end : duckdb_zstd::ZSTD_e_continue;

		size_t compress_result;
		while (true) {
			idx_t old_pos = out_buffer.pos;

			compress_result =
			    duckdb_zstd::ZSTD_compressStream2(analyze_state->context, &out_buffer, &in_buffer, end_mode);
			D_ASSERT(out_buffer.pos >= old_pos);
			auto diff = out_buffer.pos - old_pos;
			vector_state.compressed_size += diff;
			buffer_collection.GetCurrentOffset() += diff;

			if (duckdb_zstd::ZSTD_isError(compress_result)) {
				throw InvalidInputException("ZSTD Compression failed: %s",
				                            duckdb_zstd::ZSTD_getErrorName(compress_result));
			}
			if (!(buffer_collection.GetCurrentOffset() <= GetWritableSpace(info))) {
				throw InternalException(
				    "(ZSTDCompressionState::CompressString) Offset (%d) exceeds writable space! (%d)",
				    buffer_collection.GetCurrentOffset(), GetWritableSpace(info));
			}
			if (compress_result == 0) {
				// Finished
				break;
			}
			NewPage();
		}
	}

	void AddStringInternal(const string_t &string) {
		if (!vector_state.tuple_count) {
			InitializeVector();
		}

		auto is_final_string = vector_state.AddStringLength(string);
		CompressString(string, is_final_string);
		if (is_final_string) {
			// Reached the end of this vector
			FlushVector();
		}
	}

	void AddString(const string_t &string) {
		AddStringInternal(string);
		stats_writer.Update(string);
	}

	void NewPage(bool additional_data_page = false) {
		block_id_t new_id = FinalizePage();
		block_id_t current_block_id = buffer_collection.GetCurrentId();
		GetExtraPageBuffer(current_block_id);
		buffer_collection.block_id = new_id;
		ResetOutBuffer();
	}

	block_id_t FinalizePage() {
		auto &block_manager = partial_block_manager.GetBlockManager();
		auto new_id = partial_block_manager.GetFreeBlockId();

		auto &state = buffer_collection.segment->GetSegmentState()->Cast<UncompressedStringSegmentState>();
		state.RegisterBlock(block_manager, new_id);

		auto &buffer_state = buffer_collection.GetCurrentBufferState();
		buffer_state.full = true;

		// Write the new id at the end of the last page
		WriteBlockIdPointer(new_id);
		D_ASSERT(buffer_state.offset <= info.GetBlockSize());
		return new_id;
	}

	void FlushPage(BufferHandle &buffer, block_id_t block_id) {
		if (block_id == INVALID_BLOCK) {
			return;
		}

		// Write the current page to disk
		auto &block_manager = partial_block_manager.GetBlockManager();
		block_manager.Write(partial_block_manager.GetClientContext(), buffer.GetFileBuffer(), block_id);
	}

	void FlushVector() {
		// Write the metadata for this Vector
		segment_state.page_ids[segment_state.vector_in_segment_count] = vector_state.starting_page;
		segment_state.page_offsets[segment_state.vector_in_segment_count] = vector_state.starting_offset;
		segment_state.compressed_sizes[segment_state.vector_in_segment_count] = vector_state.compressed_size;
		segment_state.uncompressed_sizes[segment_state.vector_in_segment_count] = vector_state.uncompressed_size;
		if (segment_state.vector_in_segment_count >= segment_state.total_vectors_in_segment) {
			throw InternalException(
			    "(ZSTDCompressionState::FlushVector) Written too many vectors (%d) to this segment! (expected: %d)",
			    segment_state.vector_in_segment_count, segment_state.total_vectors_in_segment);
		}
		vector_count++;
		segment_state.vector_in_segment_count++;
		vector_state.in_vector = false;
		buffer_collection.segment->count += vector_state.tuple_count;

		vector_state.tuple_count = 0;

		//! If the string lengths live on an overflow page, and that buffer is full
		//! then we want to flush it

		auto buffer_data = buffer_collection.GetBufferData(/*include_segment=*/true);
		ZSTDCompressionBufferCollection::Slot slot = ZSTDCompressionBufferCollection::Slot::SEGMENT;
		optional_ptr<BufferHandle> buffer_handle_ptr;
		optional_ptr<ZSTDCompressionBufferState> buffer_state_ptr;
		for (auto &buffer : buffer_data) {
			auto &buffer_state = buffer.state;
			if (buffer_state.flags.HasStringMetadata()) {
				if (buffer_handle_ptr) {
					throw InternalException("(ZSTDCompressionState::FlushVector) Multiple buffers (%d and %d) have "
					                        "string metadata on them, this is impossible and indicates a bug!",
					                        static_cast<uint8_t>(slot), static_cast<uint8_t>(buffer.slot));
				}
				slot = buffer.slot;
				buffer_state_ptr = buffer.state;
				buffer_handle_ptr = buffer.handle;
			}
			buffer_state.flags.UnsetStringMetadata();
			buffer_state.flags.UnsetData();
		}

		if (!buffer_handle_ptr) {
			throw InternalException("(ZSTDCompressionState::FlushVector) None of the buffers have string metadata on "
			                        "them, this is impossible and indicates a bug!");
		}
		if (slot == ZSTDCompressionBufferCollection::Slot::SEGMENT) {
			//! This is the segment handle, we don't need to flush that here, it'll get flushed when the segment is done
			return;
		}
		auto &buffer_state = *buffer_state_ptr;
		if (!buffer_state.full) {
			//! It contains the string metadata of the current vector, but the buffer isn't full
			//! so we don't need to flush it yet
			return;
		}
		auto &buffer_handle = *buffer_handle_ptr;
		FlushPage(buffer_handle, vector_state.starting_page);
		buffer_state.offset = 0;
		buffer_state.full = false;
	}

	void CreateEmptySegment() {
		auto compressed_segment = CreateNewSegment();
		buffer_collection.segment = std::move(compressed_segment);
		stats_writer.Clear();

		auto &buffer_manager = BufferManager::GetBufferManager(checkpoint_data.GetDatabase());
		buffer_collection.segment_handle =
		    buffer_manager.Pin(partial_block_manager.GetClientContext(), buffer_collection.segment->GetBlockHandle());
	}

	void FlushSegment() {
		if (!buffer_collection.segment) {
			return;
		}
		if (segment_state.vector_in_segment_count != segment_state.total_vectors_in_segment) {
			throw InternalException("(ZSTDCompressionState::FlushSegment) We haven't written all vectors that we were "
			                        "expecting to write (%d instead of %d)!",
			                        segment_state.vector_in_segment_count, segment_state.total_vectors_in_segment);
		}

		auto &segment_buffer_state = buffer_collection.buffer_states[0];
		auto segment_block_size = segment_buffer_state.offset;
		if (segment_block_size < GetVectorMetadataSize(segment_state.total_vectors_in_segment)) {
			throw InternalException(
			    "(ZSTDCompressionState::FlushSegment) Expected offset to be at least %d, but found %d instead",
			    GetVectorMetadataSize(segment_state.total_vectors_in_segment), segment_block_size);
		}

		bool seen_dirty_buffer = false;
		auto buffer_data = buffer_collection.GetBufferData(/*include_segment=*/false);
		for (auto &buffer : buffer_data) {
			auto &buffer_state = buffer.state;
			auto &buffer_handle = buffer.handle;
			if (buffer_state.offset != 0) {
				if (seen_dirty_buffer) {
					throw InternalException("(ZSTDCompressionState::FlushSegment) Both extra pages were dirty (needed "
					                        "to be flushed), this should be impossible");
				}
				FlushPage(buffer_handle, buffer_collection.block_id);
				buffer_state.full = false;
				buffer_state.offset = 0;
				buffer_state.flags.Clear();
				seen_dirty_buffer = true;
			}
		}

		auto &state = checkpoint_data.GetCheckpointState();
		stats_writer.Merge(buffer_collection.segment->GetStatsMutable());
		state.FlushSegment(std::move(buffer_collection.segment), std::move(buffer_collection.segment_handle),
		                   segment_block_size);
		segment_buffer_state.flags.Clear();
		segment_buffer_state.full = true;
		segment_buffer_state.offset = 0;
		segment_count++;
	}

	void Finalize() {
		D_ASSERT(!vector_state.tuple_count);
		FlushSegment();
		buffer_collection.segment.reset();
	}

	void AddNull() {
		stats_writer.SetHasNull();
		string_t empty(static_cast<uint32_t>(0));
		AddStringInternal(empty);
	}

public:
	unique_ptr<ZSTDAnalyzeState> analyze_state;
	PartialBlockManager &partial_block_manager;
	StatsWriter<string_t> stats_writer;

	//! --- Analyzed Data ---
	//! The amount of tuples we're writing
	const idx_t total_tuple_count;
	//! The amount of vectors we're writing
	const idx_t total_vector_count;
	//! The total amount of segments we're writing
	const idx_t total_segment_count;
	//! The vectors to store in a segment (not the last one)
	const idx_t vectors_per_segment;

	//! The amount of vectors we've seen so far
	idx_t vector_count = 0;
	//! Current segment index we're at
	idx_t segment_count = 0;

	ZSTDCompressionBufferCollection buffer_collection;

	//! The compression context indicating where we are in the output buffer
	duckdb_zstd::ZSTD_outBuffer out_buffer;

	//! State of the current Vector we are writing
	ZSTDCompressionVectorState vector_state;
	//! State of the current Segment we are writing
	ZSTDCompressionSegmentState segment_state;
};

unique_ptr<CompressionState> ZSTDStorage::InitCompression(ColumnDataCheckpointData &checkpoint_data,
                                                          unique_ptr<AnalyzeState> analyze_state_p) {
	return make_uniq<ZSTDCompressionState>(checkpoint_data,
	                                       unique_ptr_cast<AnalyzeState, ZSTDAnalyzeState>(std::move(analyze_state_p)));
}

void ZSTDStorage::Compress(CompressionState &state_p, const Vector &input) {
	auto &state = state_p.Cast<ZSTDCompressionState>();

	// Get vector data
	UnifiedVectorFormat vdata;
	input.ToUnifiedFormat(vdata);
	auto data = UnifiedVectorFormat::GetData<string_t>(vdata);

	for (idx_t i = 0; i < input.size(); i++) {
		auto idx = vdata.sel->get_index(i);
		// Note: we treat nulls and empty strings the same
		if (!vdata.validity.RowIsValid(idx)) {
			state.AddNull();
			continue;
		}
		state.AddString(data[idx]);
	}
}

void ZSTDStorage::FinalizeCompress(CompressionState &state_p) {
	auto &state = state_p.Cast<ZSTDCompressionState>();
	state.Finalize();
}

[[noreturn]] static void ThrowZSTDPageTooSmall() {
	throw DataCorruptionException("Corrupted ZSTD vector: no space for compressed data and the next page ID");
}

[[noreturn]] static void ThrowZSTDCompressedDataExhausted() {
	throw DataCorruptionException("Corrupted ZSTD vector: compressed data ended before decompression finished");
}

[[noreturn]] static void ThrowZSTDPageCycle() {
	throw DataCorruptionException("Corrupted ZSTD vector: cycle in overflow page chain");
}

[[noreturn]] static void ThrowZSTDFrameEndedEarly() {
	throw DataCorruptionException("Corrupted ZSTD vector: frame ended before producing the requested bytes");
}

[[noreturn]] static void ThrowZSTDNoProgress() {
	throw DataCorruptionException("Corrupted ZSTD vector: decompression made no progress");
}

[[noreturn]] static void ThrowZSTDDecompressionFailed(size_t error) {
	throw DataCorruptionException("ZSTD Decompression failed: %s", duckdb_zstd::ZSTD_getErrorName(error));
}

struct ZSTDVectorScanMetadata {
	//! The index of the (internal) vector being read
	idx_t vector_idx;
	block_id_t block_id;
	page_offset_t block_offset;

	uncompressed_size_t uncompressed_size;
	compressed_size_t compressed_size;

	//! The amount of tuples that are in this Vector
	idx_t count;
};

struct ZSTDVectorScanState {
public:
	ZSTDVectorScanState(const ZSTDVectorScanMetadata &metadata_p,
	                    unsafe_array_ptr<const string_length_t> string_lengths_p,
	                    const CompressionSegmentReader &reader_p)
	    : metadata(metadata_p), string_lengths(string_lengths_p), reader(reader_p) {
	}
	ZSTDVectorScanState(ZSTDVectorScanState &&other) = default;
	ZSTDVectorScanState(const ZSTDVectorScanState &other) = delete;

public:
	//! The metadata of the vector
	ZSTDVectorScanMetadata metadata;
	//! The (pinned) buffer handle(s) for this vectors data
	vector<BufferHandle> buffer_handles;
	//! Blocks visited by this vector
	unordered_set<block_id_t> visited_blocks;
	//! The (uncompressed) string lengths for this vector
	unsafe_array_ptr<const string_length_t> string_lengths;
	//! Reader for the current page.
	CompressionSegmentReader reader;
	//! The amount of values already consumed from the state
	idx_t scanned_count = 0;
	//! The amount of compressed data read
	idx_t compressed_scan_count = 0;
	//! The inBuffer that ZSTD_decompressStream reads the compressed data from
	duckdb_zstd::ZSTD_inBuffer in_buffer;
};

//===--------------------------------------------------------------------===//
// Scan
//===--------------------------------------------------------------------===//
struct ZSTDScanState : public SegmentScanState {
public:
	//! ZSTD segment layout on disk.
	struct SegmentLayout {
		//! Vector metadata
		unsafe_array_ptr<const page_id_t> page_ids;
		unsafe_array_ptr<const page_offset_t> page_offsets;
		unsafe_array_ptr<const uncompressed_size_t> uncompressed_sizes;
		unsafe_array_ptr<const compressed_size_t> compressed_sizes;
		//! String lengths and compressed strings.
		CompressionSegmentReader data;
		//! Offset of data from the start of the segment.
		idx_t data_offset;
	};

	static SegmentLayout ReadLayout(const BufferHandle &handle, const ColumnSegment &segment);

	explicit ZSTDScanState(const QueryContext &context, ColumnSegment &segment)
	    : state(segment.GetSegmentState()->Cast<UncompressedStringSegmentState>()),
	      block_manager(segment.GetBlockHandle()->GetBlockManager()),
	      buffer_manager(BufferManager::GetBufferManager(segment.GetDatabase())), context(context),
	      segment_handle(buffer_manager.Pin(context, segment.GetBlockHandle())),
	      layout(ReadLayout(segment_handle, segment)), segment_count(segment.count.load()), segment(segment) {
		decompression_context = duckdb_zstd::ZSTD_createDCtx();
	}
	~ZSTDScanState() override {
		duckdb_zstd::ZSTD_freeDCtx(decompression_context);
	}

public:
	idx_t GetVectorIndex(idx_t start_index, idx_t &offset) {
		idx_t vector_idx = start_index / ZSTD_VECTOR_SIZE;
		offset = start_index % ZSTD_VECTOR_SIZE;
		return vector_idx;
	}

	ZSTDVectorScanMetadata GetVectorMetadata(idx_t vector_idx) {
		D_ASSERT(vector_idx < layout.page_ids.size());
		idx_t previous_value_count = vector_idx * ZSTD_VECTOR_SIZE;
		idx_t value_count = MinValue<idx_t>(segment_count - previous_value_count, ZSTD_VECTOR_SIZE);

		return ZSTDVectorScanMetadata {/* vector_idx = */ vector_idx,
		                               /* block_id = */ layout.page_ids[vector_idx],
		                               /* block_offset = */ layout.page_offsets[vector_idx],
		                               /* uncompressed_size = */ layout.uncompressed_sizes[vector_idx],
		                               /* compressed_size = */ layout.compressed_sizes[vector_idx],
		                               /* count = */ value_count};
	}

	shared_ptr<BlockHandle> LoadPage(block_id_t block_id) {
		return state.GetHandle(block_manager, block_id);
	}

	bool UseVectorStateCache(idx_t vector_idx, idx_t internal_offset) {
		if (!current_vector) {
			// No vector loaded yet
			return false;
		}
		if (current_vector->metadata.vector_idx != vector_idx) {
			// Not the same vector
			return false;
		}
		if (current_vector->scanned_count > internal_offset) {
			// Already past the scan offset
			return false;
		}
		return true;
	}

	ZSTDVectorScanState &LoadVector(idx_t vector_idx, idx_t internal_offset) {
		if (UseVectorStateCache(vector_idx, internal_offset)) {
			if (internal_offset > current_vector->scanned_count) {
				Skip(*current_vector, internal_offset - current_vector->scanned_count);
			}
			return *current_vector;
		}
		auto metadata = GetVectorMetadata(vector_idx);
		BufferHandle data_handle;
		auto reader = layout.data;
		// The next page ID sits at the physical page end, which can exceed the recorded segment size.
		idx_t page_end = segment.GetBlockSize() - segment.GetBlockOffset() - layout.data_offset;
		if (metadata.block_id == INVALID_BLOCK) {
			// Data lives on the segment's page
			reader.SetPosition(metadata.block_offset - layout.data_offset);
		} else {
			// Data lives on an extra page, have to load the block first
			auto block = LoadPage(metadata.block_id);
			data_handle = buffer_manager.Pin(context, block);
			reader = CompressionSegmentReader(data_handle.Ptr(), block->GetBlockSize(), "ZSTD overflow page");
			reader.SetPosition(metadata.block_offset);
			page_end = block->GetBlockSize();
		}

		reader.Align(sizeof(string_length_t));
		auto string_lengths = reader.ReadArray<string_length_t>(metadata.count);

		current_vector = make_uniq<ZSTDVectorScanState>(metadata, string_lengths, reader);
		auto &scan_state = *current_vector;
		const auto first_id = metadata.block_id == INVALID_BLOCK ? segment.GetBlockId() : metadata.block_id;
		scan_state.visited_blocks.insert(first_id);
		if (data_handle.IsValid()) {
			scan_state.buffer_handles.push_back(std::move(data_handle));
		}

		// Update the in_buffer to point to the start of the compressed data frame
		if (metadata.block_id != INVALID_BLOCK || segment.GetBlockOffset() == 0) {
			const auto page_remaining = page_end - scan_state.reader.Position();
			// Exclude the next page ID so it isn't passed to the decompressor.
			const auto data_remaining = page_remaining > sizeof(block_id_t) ? page_remaining - sizeof(block_id_t) : 0;
			if (metadata.compressed_size > data_remaining && data_remaining == 0) {
				ThrowZSTDPageTooSmall();
			}
			scan_state.in_buffer.size = MinValue(metadata.compressed_size, data_remaining);
		} else {
			// A packed primary segment cannot contain a vector that continues onto another block.
			scan_state.in_buffer.size = MinValue(metadata.compressed_size, scan_state.reader.Remaining());
		}
		auto compressed_data = scan_state.reader.ReadBytes(scan_state.in_buffer.size);
		scan_state.in_buffer.src = compressed_data.data();
		scan_state.in_buffer.pos = 0;

		// Initialize the context for streaming decompression
		duckdb_zstd::ZSTD_DCtx_reset(decompression_context, duckdb_zstd::ZSTD_reset_session_only);
		duckdb_zstd::ZSTD_DCtx_refDDict(decompression_context, nullptr);

		if (internal_offset) {
			Skip(scan_state, internal_offset);
		}
		return scan_state;
	}

	void LoadNextPageForVector(ZSTDVectorScanState &scan_state) {
		if (scan_state.in_buffer.pos != scan_state.in_buffer.size) {
			throw InternalException(
			    "(ZSTDScanState::LoadNextPageForVector) Trying to load the next page before consuming the current one");
		}
		if (scan_state.compressed_scan_count >= scan_state.metadata.compressed_size) {
			ThrowZSTDCompressedDataExhausted();
		}
		// Read the next block id from the end of the page
		const auto next_id = scan_state.reader.Read<block_id_t>();
		// Check if we have a cycle
		if (!scan_state.visited_blocks.insert(next_id).second) {
			ThrowZSTDPageCycle();
		}

		// Load the next page
		auto block = LoadPage(next_id);
		auto handle = buffer_manager.Pin(context, block);
		auto reader = CompressionSegmentReader(handle.Ptr(), block->GetBlockSize(), "ZSTD overflow page");
		idx_t page_size = reader.Remaining() - sizeof(block_id_t);
		idx_t remaining_compressed_data = scan_state.metadata.compressed_size - scan_state.compressed_scan_count;
		auto compressed_data = reader.ReadBytes(MinValue<idx_t>(page_size, remaining_compressed_data));
		scan_state.buffer_handles.push_back(std::move(handle));
		scan_state.reader = reader;

		// Update the in_buffer to point to the new page
		scan_state.in_buffer.src = compressed_data.data();
		scan_state.in_buffer.pos = 0;
		scan_state.in_buffer.size = compressed_data.size();
	}

	void DecompressString(ZSTDVectorScanState &scan_state, data_ptr_t destination, idx_t uncompressed_length) {
		if (uncompressed_length == 0) {
			return;
		}

		auto &in_buffer = scan_state.in_buffer;
		duckdb_zstd::ZSTD_outBuffer out_buffer;

		out_buffer.dst = destination;
		out_buffer.pos = 0;
		out_buffer.size = uncompressed_length;

		while (true) {
			idx_t old_pos = in_buffer.pos;
			idx_t old_output_pos = out_buffer.pos;
			size_t res = duckdb_zstd::ZSTD_decompressStream(
			    /* zds = */ decompression_context,
			    /* output =*/&out_buffer,
			    /* input =*/&in_buffer);
			scan_state.compressed_scan_count += in_buffer.pos - old_pos;
			if (duckdb_zstd::ZSTD_isError(res)) {
				ThrowZSTDDecompressionFailed(res);
			}
			if (out_buffer.pos == out_buffer.size) {
				//! Done decompressing the relevant portion
				break;
			}
			if (!res) {
				ThrowZSTDFrameEndedEarly();
			}
			if (in_buffer.pos == in_buffer.size) {
				// Did not fully decompress, it needs a new page to read from
				LoadNextPageForVector(scan_state);
			} else if (in_buffer.pos == old_pos && out_buffer.pos == old_output_pos) {
				ThrowZSTDNoProgress();
			}
		}
	}

	void Skip(ZSTDVectorScanState &scan_state, idx_t count) {
		if (!skip_buffer.IsSet()) {
			skip_buffer = Allocator::DefaultAllocator().Allocate(duckdb_zstd::ZSTD_DStreamOutSize());
		}

		D_ASSERT(scan_state.scanned_count + count <= scan_state.metadata.count);

		// Figure out how much we need to skip
		auto string_lengths = scan_state.string_lengths.SubArray(scan_state.scanned_count, count);
		idx_t uncompressed_length = 0;
		for (idx_t i = 0; i < count; i++) {
			uncompressed_length += string_lengths[i];
		}

		// Skip that many bytes by decompressing into the skip_buffer
		idx_t remaining = uncompressed_length;
		while (remaining) {
			idx_t to_scan = MinValue<idx_t>(skip_buffer.GetSize(), remaining);
			DecompressString(scan_state, skip_buffer.get(), to_scan);
			remaining -= to_scan;
		}
		scan_state.scanned_count += count;
		scanned_count += count;
	}

	void ScanInternal(ZSTDVectorScanState &scan_state, idx_t count, Vector &result, idx_t result_offset) {
		D_ASSERT(scan_state.scanned_count + count <= scan_state.metadata.count);
		D_ASSERT(result.GetType().InternalType() == PhysicalType::VARCHAR);

		auto string_lengths = scan_state.string_lengths.SubArray(scan_state.scanned_count, count);
		idx_t uncompressed_length = 0;
		for (idx_t i = 0; i < count; i++) {
			uncompressed_length += string_lengths[i];
		}
		auto &allocator = StringVector::GetStringAllocator(result);
		auto uncompressed_data = StringVector::AllocateShrinkableBuffer(allocator, uncompressed_length);
		auto string_data = FlatVector::GetDataMutable<string_t>(result);

		DecompressString(scan_state, uncompressed_data, uncompressed_length);

		idx_t offset = 0;
		for (idx_t i = 0; i < count; i++) {
			string_data[result_offset + i] = string_t(char_ptr_cast(uncompressed_data + offset), string_lengths[i]);
			offset += string_lengths[i];
		}
		scan_state.scanned_count += count;
		scanned_count += count;
	}

	void ScanPartial(idx_t start_idx, Vector &result, idx_t offset, idx_t count) {
		idx_t remaining = count;
		idx_t scanned = 0;
		while (remaining) {
			idx_t internal_offset;
			idx_t vector_idx = GetVectorIndex(start_idx + scanned, internal_offset);
			auto &scan_state = LoadVector(vector_idx, internal_offset);
			idx_t remaining_in_vector = scan_state.metadata.count - scan_state.scanned_count;
			idx_t to_scan = MinValue<idx_t>(remaining, remaining_in_vector);
			ScanInternal(scan_state, to_scan, result, offset + scanned);
			remaining -= to_scan;
			scanned += to_scan;
		}
		D_ASSERT(scanned == count);
	}

public:
	UncompressedStringSegmentState &state;
	BlockManager &block_manager;
	BufferManager &buffer_manager;
	const QueryContext context;

	duckdb_zstd::ZSTD_DCtx *decompression_context = nullptr;

	BufferHandle segment_handle;

	SegmentLayout layout;

	//! Cache of (the scan state of) the current vector being read
	unique_ptr<ZSTDVectorScanState> current_vector;

	//! The amount of tuples stored in the segment
	idx_t segment_count;
	//! The amount of tuples consumed
	idx_t scanned_count = 0;
	ColumnSegment &segment;

	//! Buffer for skipping data
	AllocatedData skip_buffer;
};

ZSTDScanState::SegmentLayout ZSTDScanState::ReadLayout(const BufferHandle &handle, const ColumnSegment &segment) {
	auto reader = CompressionSegmentReader::FromSegment(handle, segment, "ZSTD segment");
	const auto vector_count = GetVectorCount(segment.count.load());

	reader.Align(sizeof(page_id_t));
	auto page_ids = reader.ReadArray<page_id_t>(vector_count);

	reader.Align(sizeof(page_offset_t));
	auto page_offsets = reader.ReadArray<page_offset_t>(vector_count);

	reader.Align(sizeof(uncompressed_size_t));
	auto uncompressed_sizes = reader.ReadArray<uncompressed_size_t>(vector_count);

	reader.Align(sizeof(compressed_size_t));
	auto compressed_sizes = reader.ReadArray<compressed_size_t>(vector_count);

	auto data_offset = reader.Position();
	auto data = reader.ReadSubReader(reader.Remaining(), "ZSTD segment data");
	return {page_ids, page_offsets, uncompressed_sizes, compressed_sizes, data, data_offset};
}

unique_ptr<SegmentScanState> ZSTDStorage::StringInitScan(const QueryContext &context, ColumnSegment &segment) {
	auto result = make_uniq<ZSTDScanState>(context, segment);
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// Scan base data
//===--------------------------------------------------------------------===//
void ZSTDStorage::StringScanPartial(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result,
                                    idx_t result_offset) {
	auto &scan_state = state.scan_state->template Cast<ZSTDScanState>();
	auto start = state.GetPositionInSegment();

	scan_state.ScanPartial(start, result, result_offset, scan_count);
}

void ZSTDStorage::StringScan(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result) {
	StringScanPartial(segment, state, scan_count, result, 0);
}

//===--------------------------------------------------------------------===//
// Serialization & Cleanup
//===--------------------------------------------------------------------===//

unique_ptr<CompressedSegmentState> ZSTDStorage::StringInitSegment(ColumnSegment &segment, block_id_t block_id,
                                                                  optional_ptr<ColumnSegmentState> segment_state) {
	auto result = make_uniq<UncompressedStringSegmentState>();
	if (segment_state) {
		auto &serialized_state = segment_state->Cast<SerializedStringSegmentState>();
		result->InitializeOnDiskBlocks(std::move(serialized_state.blocks));
	}
	return std::move(result);
}

unique_ptr<ColumnSegmentState> ZSTDStorage::SerializeState(ColumnSegment &segment) {
	auto &state = segment.GetSegmentState()->Cast<UncompressedStringSegmentState>();
	if (state.GetOnDiskBlocks().empty()) {
		// no on-disk blocks - nothing to write
		return nullptr;
	}
	return make_uniq<SerializedStringSegmentState>(state.GetOnDiskBlocks());
}

unique_ptr<ColumnSegmentState> ZSTDStorage::DeserializeState(Deserializer &deserializer) {
	auto result = make_uniq<SerializedStringSegmentState>();
	deserializer.ReadProperty(1, "overflow_blocks", result->blocks);
	return std::move(result);
}

void ZSTDStorage::VisitBlockIds(const ColumnSegment &segment, BlockIdVisitor &visitor) {
	auto &state = segment.GetSegmentState()->Cast<UncompressedStringSegmentState>();
	for (auto &block_id : state.GetOnDiskBlocks()) {
		visitor.Visit(block_id);
	}
}

//===--------------------------------------------------------------------===//
// Get Function
//===--------------------------------------------------------------------===//
CompressionFunction ZSTDFun::GetFunction(PhysicalType data_type) {
	D_ASSERT(data_type == PhysicalType::VARCHAR);
	auto zstd =
	    CompressionFunction(CompressionType::COMPRESSION_ZSTD, data_type, ZSTDStorage::StringInitAnalyze,
	                        ZSTDStorage::StringAnalyze, ZSTDStorage::StringFinalAnalyze, ZSTDStorage::InitCompression,
	                        ZSTDStorage::Compress, ZSTDStorage::FinalizeCompress, ZSTDStorage::StringInitScan,
	                        ZSTDStorage::StringScan, ZSTDStorage::StringScanPartial, ZSTDStorage::StringSkip);
	zstd.init_segment = ZSTDStorage::StringInitSegment;
	zstd.serialize_state = ZSTDStorage::SerializeState;
	zstd.deserialize_state = ZSTDStorage::DeserializeState;
	zstd.visit_block_ids = ZSTDStorage::VisitBlockIds;
	return zstd;
}

bool ZSTDFun::TypeIsSupported(PhysicalType type) {
	return type == PhysicalType::VARCHAR;
}

} // namespace duckdb
