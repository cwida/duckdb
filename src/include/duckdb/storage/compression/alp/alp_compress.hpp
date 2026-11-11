//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/compression/alp/alp_compress.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/helper.hpp"
#include "duckdb/common/operator/subtract.hpp"
#include "duckdb/common/types/null_value.hpp"
#include "duckdb/function/compression_function.hpp"
#include "duckdb/storage/compression/standard_compression_state.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/compression/alp/algorithm/alp.hpp"
#include "duckdb/storage/compression/alp/alp_analyze.hpp"
#include "duckdb/storage/compression/patas/patas.hpp"
#include "duckdb/storage/table/column_data_checkpointer.hpp"
#include "duckdb/storage/table/column_segment.hpp"
#include "duckdb/storage/statistics/stats_writer.hpp"

namespace duckdb {

template <class T>
struct AlpCompressionState : public StandardCompressionState {
public:
	using EXACT_TYPE = typename FloatingToExact<T>::TYPE;

	AlpCompressionState(ColumnDataCheckpointData &checkpoint_data, AlpAnalyzeState<T> *analyze_state)
	    : StandardCompressionState(checkpoint_data, CompressionType::COMPRESSION_ALP) {
		CreateEmptySegment();

		//! Combinations found on the analyze step are needed for compression
		compression_data.best_k_combinations = analyze_state->compression_data.best_k_combinations;
	}

	StatsWriter<T> stats_writer;
	idx_t vector_idx = 0;
	idx_t nulls_idx = 0;
	idx_t vectors_flushed = 0;
	idx_t data_bytes_used = 0;

	data_ptr_t data_ptr;     // Pointer to next free spot in segment;
	data_ptr_t metadata_ptr; // Reverse pointer to the next free spot for the metadata; used in decoding to SKIP vectors
	AlpConstants::METADATA_POINTER_TYPE next_vector_byte_index_start = AlpConstants::HEADER_SIZE;

	T input_vector[AlpConstants::ALP_VECTOR_SIZE]; // Uncompressed data
	uint16_t vector_null_positions[AlpConstants::ALP_VECTOR_SIZE];

	alp::AlpCompressionData<T, false> compression_data;

public:
	// Returns the space currently used in the segment (in bytes)
	idx_t UsedSpace() const {
		return AlpConstants::METADATA_POINTER_SIZE + data_bytes_used;
	}

	bool HasEnoughSpace(idx_t vector_size) {
		//! If [start of block + used space + required space] is more than whats left (current position
		//! of metadata pointer - the size of a new metadata pointer)
		if ((handle.GetDataMutable() + AlignValue(UsedSpace() + vector_size)) >=
		    (metadata_ptr - AlpConstants::METADATA_POINTER_SIZE)) {
			return false;
		}
		return true;
	}

	void ResetVector() {
		compression_data.Reset();
	}

	void CreateEmptySegment() {
		CreateAndPinNewSegment();

		// The pointer to the start of the compressed data.
		data_ptr = handle.GetDataMutable() + current_segment->GetBlockOffset() + AlpConstants::HEADER_SIZE;
		// The pointer to the start of the metadata.
		metadata_ptr = handle.GetDataMutable() + current_segment->GetBlockOffset() + info.GetBlockSize();
		next_vector_byte_index_start = AlpConstants::HEADER_SIZE;
	}

	void CompressVector() {
		if (nulls_idx) {
			alp::AlpUtils::FindAndReplaceNullsInVector<T>(input_vector, vector_null_positions, vector_idx, nulls_idx);
		}
		alp::AlpCompression<T, false>::Compress(input_vector, vector_idx, vector_null_positions, nulls_idx,
		                                        compression_data);
		const idx_t uncompressed_size = AlpConstants::EXPONENT_SIZE + sizeof(T) * vector_idx;
		const idx_t compressed_size = compression_data.RequiredSpace();

		const auto storage_version = checkpoint_data.GetStorageManager().GetStorageVersion();
		const bool should_compress = compressed_size < uncompressed_size ||
		                             StorageManager::IsPriorToVersion(StorageVersion::V1_5_0, storage_version);

		const idx_t vector_size = should_compress ? compressed_size : uncompressed_size;

		//! Check if the compressed vector fits on current segment
		if (!HasEnoughSpace(vector_size)) {
			FlushSegment();
			CreateEmptySegment();
		}

		if (nulls_idx) {
			stats_writer.SetHasNull();
		}
		if (vector_idx != nulls_idx) { //! At least there is one valid value in the vector
			stats_writer.SetHasValid();
			for (idx_t i = 0; i < vector_idx; i++) {
				stats_writer.UpdateMinMax(input_vector[i]);
			}
		}
		current_segment->count += vector_idx;

		if (should_compress) {
			FlushCompressedVector();
		} else {
			FlushUncompressedVector();
		}
	}

	// Stores the vector and its metadata
	void FlushCompressedVector() {
		Store<AlpConstants::EXPONENT_TYPE>(compression_data.vector_encoding_indices.exponent, data_ptr);
		data_ptr += AlpConstants::EXPONENT_SIZE;

		Store<AlpConstants::FACTOR_TYPE>(compression_data.vector_encoding_indices.factor, data_ptr);
		data_ptr += AlpConstants::FACTOR_SIZE;

		Store<AlpConstants::EXCEPTIONS_COUNT_TYPE>(compression_data.exceptions_count, data_ptr);
		data_ptr += AlpConstants::EXCEPTIONS_COUNT_SIZE;

		Store<AlpConstants::FRAME_OF_REFERENCE_TYPE>(compression_data.frame_of_reference, data_ptr);
		data_ptr += AlpConstants::FOR_SIZE;

		Store<AlpConstants::BIT_WIDTH_TYPE>(UnsafeNumericCast<AlpConstants::BIT_WIDTH_TYPE>(compression_data.bit_width),
		                                    data_ptr);
		data_ptr += AlpConstants::BIT_WIDTH_SIZE;

		memcpy((void *)data_ptr, (void *)compression_data.values_encoded, compression_data.bp_size);
		// We should never go out of bounds in the values_encoded array
		D_ASSERT((AlpConstants::ALP_VECTOR_SIZE * 8) >= compression_data.bp_size);

		data_ptr += compression_data.bp_size;

		if (compression_data.exceptions_count > 0) {
			memcpy((void *)data_ptr, (void *)compression_data.exceptions,
			       sizeof(EXACT_TYPE) * compression_data.exceptions_count);
			data_ptr += sizeof(EXACT_TYPE) * compression_data.exceptions_count;
			memcpy((void *)data_ptr, (void *)compression_data.exceptions_positions,
			       AlpConstants::EXCEPTION_POSITION_SIZE * compression_data.exceptions_count);
			data_ptr += AlpConstants::EXCEPTION_POSITION_SIZE * compression_data.exceptions_count;
		}

		data_bytes_used +=
		    compression_data.bp_size +
		    (compression_data.exceptions_count * (sizeof(EXACT_TYPE) + AlpConstants::EXCEPTION_POSITION_SIZE)) +
		    AlpConstants::EXPONENT_SIZE + AlpConstants::FACTOR_SIZE + AlpConstants::EXCEPTIONS_COUNT_SIZE +
		    AlpConstants::FOR_SIZE + AlpConstants::BIT_WIDTH_SIZE;

		// Write pointer to the vector data (metadata)
		metadata_ptr -= AlpConstants::METADATA_POINTER_SIZE;
		Store<AlpConstants::METADATA_POINTER_TYPE>(next_vector_byte_index_start, metadata_ptr);
		next_vector_byte_index_start = NumericCast<AlpConstants::METADATA_POINTER_TYPE>(UsedSpace());

		vectors_flushed++;
		vector_idx = 0;
		nulls_idx = 0;
		ResetVector();
	}

	// Uncompressed mode
	void FlushUncompressedVector() {
		// Store a sentinel value instead of the exponent, signaling the coming data is stored uncompressed.
		constexpr AlpConstants::EXPONENT_TYPE sentinel = AlpConstants::UNCOMPRESSED_MODE_SENTINEL;
		Store<AlpConstants::EXPONENT_TYPE>(sentinel, data_ptr);
		data_ptr += AlpConstants::EXPONENT_SIZE;

		// Store uncompressed data
		memcpy(data_ptr, input_vector, sizeof(T) * vector_idx);
		data_ptr += sizeof(T) * vector_idx;

		data_bytes_used += AlpConstants::EXPONENT_SIZE + (sizeof(T) * vector_idx);

		// Write pointer to the vector data (metadata)
		metadata_ptr -= AlpConstants::METADATA_POINTER_SIZE;
		Store<AlpConstants::METADATA_POINTER_TYPE>(next_vector_byte_index_start, metadata_ptr);
		next_vector_byte_index_start = NumericCast<AlpConstants::METADATA_POINTER_TYPE>(UsedSpace());

		vectors_flushed++;
		vector_idx = 0;
		nulls_idx = 0;
		ResetVector();
	}

	void FlushSegment() {
		auto dataptr = handle.GetDataMutable();

		idx_t metadata_offset = AlignValue(UsedSpace());

		// Verify that the metadata_ptr is not smaller than the space used by the data
		D_ASSERT(dataptr + metadata_offset <= metadata_ptr);

		auto bytes_used_by_metadata = UnsafeNumericCast<idx_t>(dataptr + info.GetBlockSize() - metadata_ptr);

		// Initially the total segment size is the size of the block
		auto total_segment_size = info.GetBlockSize();

		//! We compact the block if the space used is less than a threshold
		const auto used_space_percentage =
		    static_cast<float>(metadata_offset + bytes_used_by_metadata) / static_cast<float>(total_segment_size);
		const auto data_end = UsedSpace();
		if (used_space_percentage < AlpConstants::COMPACT_BLOCK_THRESHOLD) {
			memset(dataptr + data_end, 0, metadata_offset - data_end);
#ifdef DEBUG
			//! Copy the first 4 bytes of the metadata
			AlpConstants::METADATA_POINTER_TYPE verify_bytes;
			memcpy((void *)&verify_bytes, metadata_ptr, AlpConstants::METADATA_POINTER_SIZE);
#endif
			memmove(dataptr + metadata_offset, metadata_ptr, bytes_used_by_metadata);
#ifdef DEBUG
			//! Now assert that the memmove was correct
			D_ASSERT(verify_bytes == *(AlpConstants::METADATA_POINTER_TYPE *)(dataptr + metadata_offset));
#endif
			total_segment_size = metadata_offset + bytes_used_by_metadata;
		} else {
			memset(dataptr + data_end, 0, info.GetBlockSize() - bytes_used_by_metadata - data_end);
		}

		// Store the offset to the end of metadata (to be used as a backwards pointer in decoding)
		Store<AlpConstants::METADATA_POINTER_TYPE>(NumericCast<AlpConstants::METADATA_POINTER_TYPE>(total_segment_size),
		                                           dataptr);

		FlushCurrentSegment(stats_writer, total_segment_size);
		data_bytes_used = 0;
		vectors_flushed = 0;
	}

	void Finalize() {
		if (vector_idx != 0) {
			CompressVector();
			D_ASSERT(vector_idx == 0);
		}
		FlushSegment();
		current_segment.reset();
	}
	//! Stages uncompressed input values into fixed-size batches (ALP_VECTOR_SIZE), calling CompressVector() to
	//! compress and flush each full batch to the segment. Handles nulls and processes arbitrarily large inputs.
	void Append(UnifiedVectorFormat &vdata, idx_t count) {
		auto data = UnifiedVectorFormat::GetData<T>(vdata);
		idx_t values_left_in_data = count;
		idx_t offset_in_data = 0;
		while (values_left_in_data > 0) {
			// We calculate until which value in data we must go to fill the input_vector
			// to avoid checking if input_vector is filled in each iteration
			auto values_to_fill_alp_input =
			    MinValue<idx_t>(AlpConstants::ALP_VECTOR_SIZE - vector_idx, values_left_in_data);
			if (vdata.validity.CannotHaveNull()) { //! We optimize a loop when there are no null
				for (idx_t i = 0; i < values_to_fill_alp_input; i++) {
					auto idx = vdata.sel->get_index(offset_in_data + i);
					T value = data[idx];
					input_vector[vector_idx + i] = value;
				}
			} else {
				for (idx_t i = 0; i < values_to_fill_alp_input; i++) {
					auto idx = vdata.sel->get_index(offset_in_data + i);
					T value = data[idx];
					bool is_null = !vdata.validity.RowIsValid(idx);
					//! We resolve null values with a predicated comparison
					vector_null_positions[nulls_idx] = UnsafeNumericCast<uint16_t>(vector_idx + i);
					nulls_idx += is_null;
					input_vector[vector_idx + i] = value;
				}
			}
			offset_in_data += values_to_fill_alp_input;
			values_left_in_data -= values_to_fill_alp_input;
			vector_idx += values_to_fill_alp_input;
			// We still need this check since we could have an incomplete input_vector at the end of data
			if (vector_idx == AlpConstants::ALP_VECTOR_SIZE) {
				CompressVector();
				D_ASSERT(vector_idx == 0);
			}
		}
	}
};

template <class T>
unique_ptr<CompressionState> AlpInitCompression(ColumnDataCheckpointData &checkpoint_data,
                                                unique_ptr<AnalyzeState> state) {
	return make_uniq<AlpCompressionState<T>>(checkpoint_data, (AlpAnalyzeState<T> *)state.get());
}

template <class T>
void AlpCompress(CompressionState &state_p, const Vector &scan_vector) {
	auto &state = (AlpCompressionState<T> &)state_p;
	UnifiedVectorFormat vdata;
	scan_vector.ToUnifiedFormat(vdata);
	state.Append(vdata, scan_vector.size());
}

template <class T>
void AlpFinalizeCompress(CompressionState &state_p) {
	auto &state = (AlpCompressionState<T> &)state_p;
	state.Finalize();
}

} // namespace duckdb
