//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/compression/alprd/alprd_compress.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/helper.hpp"
#include "duckdb/function/compression_function.hpp"
#include "duckdb/storage/compression/standard_compression_state.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/compression/alp/alp_constants.hpp"
#include "duckdb/storage/compression/alprd/algorithm/alprd.hpp"
#include "duckdb/storage/compression/alprd/alprd_analyze.hpp"
#include "duckdb/storage/compression/alprd/alprd_constants.hpp"
#include "duckdb/storage/compression/patas/patas.hpp"
#include "duckdb/storage/table/column_data_checkpointer.hpp"
#include "duckdb/storage/table/column_segment.hpp"

namespace duckdb {

template <class T>
struct AlpRDCompressionState : public StandardCompressionState {
public:
	using EXACT_TYPE = typename FloatingToExact<T>::TYPE;

	AlpRDCompressionState(ColumnDataCheckpointData &checkpoint_data, AlpRDAnalyzeState<T> *analyze_state)
	    : StandardCompressionState(checkpoint_data, CompressionType::COMPRESSION_ALPRD) {
		//! State variables from the analyze step that are needed for compression
		compression_data.left_parts_dict_map = std::move(analyze_state->compression_data.left_parts_dict_map);
		compression_data.left_bit_width = analyze_state->compression_data.left_bit_width;
		compression_data.right_bit_width = analyze_state->compression_data.right_bit_width;
		compression_data.actual_dictionary_size = analyze_state->compression_data.actual_dictionary_size;
		actual_dictionary_size_bytes =
		    compression_data.actual_dictionary_size * AlpRDConstants::DICTIONARY_ELEMENT_SIZE;
		next_vector_byte_index_start = AlpRDConstants::HEADER_SIZE + actual_dictionary_size_bytes;
		memcpy((void *)compression_data.left_parts_dict, (void *)analyze_state->compression_data.left_parts_dict,
		       actual_dictionary_size_bytes);
		CreateEmptySegment();
	}

	StatsWriter<T> stats_writer;
	idx_t vector_idx = 0;
	idx_t nulls_idx = 0;
	idx_t vectors_flushed = 0;
	idx_t data_bytes_used = 0;

	data_ptr_t data_ptr;     // Pointer to next free spot in segment;
	data_ptr_t metadata_ptr; // Reverse pointer to the next free spot for the metadata; used in decoding to SKIP vectors
	uint32_t actual_dictionary_size_bytes;
	AlpRDConstants::METADATA_POINTER_TYPE next_vector_byte_index_start;

	EXACT_TYPE input_vector[AlpRDConstants::ALP_VECTOR_SIZE];
	uint16_t vector_null_positions[AlpRDConstants::ALP_VECTOR_SIZE];

	alp::AlpRDCompressionData<T, false> compression_data;

public:
	// Returns the space currently used in the segment (in bytes)
	idx_t UsedSpace() const {
		//! [Pointer to metadata + right bitwidth] + Dictionary Size + Bytes already used in the segment
		return AlpRDConstants::HEADER_SIZE + actual_dictionary_size_bytes + data_bytes_used;
	}

	bool HasEnoughSpace(idx_t vector_size) {
		//! If [start of block + used space + required space] is more than whats left (current position
		//! of metadata pointer - the size of a new metadata pointer)
		if ((handle.GetDataMutable() + AlignValue(UsedSpace() + vector_size)) >=
		    (metadata_ptr - AlpRDConstants::METADATA_POINTER_SIZE)) {
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
		data_ptr = handle.GetDataMutable() + current_segment->GetBlockOffset() + AlpRDConstants::HEADER_SIZE +
		           actual_dictionary_size_bytes;
		// The pointer to the start of the metadata.
		metadata_ptr = handle.GetDataMutable() + current_segment->GetBlockOffset() + info.GetBlockSize();
		next_vector_byte_index_start = AlpRDConstants::HEADER_SIZE + actual_dictionary_size_bytes;
	}

	void CompressVector() {
		if (nulls_idx) {
			alp::AlpUtils::FindAndReplaceNullsInVector<EXACT_TYPE>(input_vector, vector_null_positions, vector_idx,
			                                                       nulls_idx);
		}
		alp::AlpRDCompression<T, false>::Compress(input_vector, vector_idx, compression_data);

		const idx_t uncompressed_size = AlpConstants::EXCEPTIONS_COUNT_SIZE + sizeof(EXACT_TYPE) * vector_idx;
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
				T floating_point_value = Load<T>(const_data_ptr_cast(&input_vector[i]));
				stats_writer.UpdateMinMax(floating_point_value);
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
		Store<AlpRDConstants::EXCEPTIONS_COUNT_TYPE>(compression_data.exceptions_count, data_ptr);
		data_ptr += AlpRDConstants::EXCEPTIONS_COUNT_SIZE;

		memcpy((void *)data_ptr, (void *)compression_data.left_parts_encoded, compression_data.left_bit_packed_size);
		data_ptr += compression_data.left_bit_packed_size;

		memcpy((void *)data_ptr, (void *)compression_data.right_parts_encoded, compression_data.right_bit_packed_size);
		data_ptr += compression_data.right_bit_packed_size;

		if (compression_data.exceptions_count > 0) {
			memcpy((void *)data_ptr, (void *)compression_data.exceptions,
			       AlpRDConstants::EXCEPTION_SIZE * compression_data.exceptions_count);
			data_ptr += AlpRDConstants::EXCEPTION_SIZE * compression_data.exceptions_count;
			memcpy((void *)data_ptr, (void *)compression_data.exceptions_positions,
			       AlpRDConstants::EXCEPTION_POSITION_SIZE * compression_data.exceptions_count);
			data_ptr += AlpRDConstants::EXCEPTION_POSITION_SIZE * compression_data.exceptions_count;
		}

		data_bytes_used += compression_data.left_bit_packed_size + compression_data.right_bit_packed_size +
		                   (compression_data.exceptions_count *
		                    (AlpRDConstants::EXCEPTION_SIZE + AlpRDConstants::EXCEPTION_POSITION_SIZE)) +
		                   AlpRDConstants::EXCEPTIONS_COUNT_SIZE;

		// Write pointer to the vector data (metadata)
		metadata_ptr -= AlpRDConstants::METADATA_POINTER_SIZE;
		Store<AlpRDConstants::METADATA_POINTER_TYPE>(next_vector_byte_index_start, metadata_ptr);
		next_vector_byte_index_start = NumericCast<AlpRDConstants::METADATA_POINTER_TYPE>(UsedSpace());

		vectors_flushed++;
		vector_idx = 0;
		nulls_idx = 0;
		ResetVector();
	}

	//! Uncompressed mode
	void FlushUncompressedVector() {
		// Store a sentinel value, signaling the coming data is stored uncompressed.
		constexpr AlpRDConstants::EXCEPTIONS_COUNT_TYPE sentinel = AlpRDConstants::UNCOMPRESSED_MODE_SENTINEL;
		Store<AlpRDConstants::EXCEPTIONS_COUNT_TYPE>(sentinel, data_ptr);
		data_ptr += AlpRDConstants::EXCEPTIONS_COUNT_SIZE;

		// Store uncompressed data
		memcpy(data_ptr, input_vector, sizeof(EXACT_TYPE) * vector_idx);
		data_ptr += sizeof(EXACT_TYPE) * vector_idx;

		data_bytes_used += AlpConstants::EXCEPTIONS_COUNT_SIZE + (sizeof(EXACT_TYPE) * vector_idx);

		// Write pointer to the vector data (metadata)
		metadata_ptr -= AlpRDConstants::METADATA_POINTER_SIZE;
		Store<AlpRDConstants::METADATA_POINTER_TYPE>(next_vector_byte_index_start, metadata_ptr);
		next_vector_byte_index_start = NumericCast<AlpRDConstants::METADATA_POINTER_TYPE>(UsedSpace());

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
			AlpRDConstants::METADATA_POINTER_TYPE verify_bytes;
			memcpy((void *)&verify_bytes, metadata_ptr, AlpRDConstants::METADATA_POINTER_SIZE);
#endif
			memmove(dataptr + metadata_offset, metadata_ptr, bytes_used_by_metadata);
#ifdef DEBUG
			//! Now assert that the memmove was correct
			D_ASSERT(verify_bytes == *(AlpRDConstants::METADATA_POINTER_TYPE *)(dataptr + metadata_offset));
#endif
			total_segment_size = metadata_offset + bytes_used_by_metadata;
		} else {
			memset(dataptr + data_end, 0, info.GetBlockSize() - bytes_used_by_metadata - data_end);
		}

		// Store the offset to the end of metadata (to be used as a backwards pointer in decoding)
		Store<AlpRDConstants::METADATA_POINTER_TYPE>(
		    NumericCast<AlpRDConstants::METADATA_POINTER_TYPE>(total_segment_size), dataptr);
		dataptr += AlpRDConstants::METADATA_POINTER_SIZE;

		// Store the right bw for the segment
		Store<AlpRDConstants::BIT_WIDTH_TYPE>(compression_data.right_bit_width, dataptr);
		dataptr += AlpRDConstants::RIGHT_BIT_WIDTH_SIZE;

		// Store the left bw for the segment
		Store<AlpRDConstants::BIT_WIDTH_TYPE>(compression_data.left_bit_width, dataptr);
		dataptr += AlpRDConstants::LEFT_BIT_WIDTH_SIZE;

		// Store the actual number of elements on the dictionary of the segment
		Store<AlpRDConstants::DICTIONARY_COUNT_TYPE>(compression_data.actual_dictionary_size, dataptr);
		dataptr += AlpRDConstants::N_DICTIONARY_ELEMENTS_SIZE;

		// Store the Dictionary
		memcpy((void *)dataptr, (void *)compression_data.left_parts_dict, actual_dictionary_size_bytes);

		FlushCurrentSegment(stats_writer, total_segment_size);
		data_bytes_used = 0;
		vectors_flushed = 0;
	}

	void Finalize() {
		if (vector_idx != 0) {
			CompressVector();
		}
		FlushSegment();
		current_segment.reset();
	}

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
					EXACT_TYPE value = Load<EXACT_TYPE>(const_data_ptr_cast(&data[idx]));
					input_vector[vector_idx + i] = value;
				}
			} else {
				for (idx_t i = 0; i < values_to_fill_alp_input; i++) {
					auto idx = vdata.sel->get_index(offset_in_data + i);
					EXACT_TYPE value = Load<EXACT_TYPE>(const_data_ptr_cast(&data[idx]));
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
unique_ptr<CompressionState> AlpRDInitCompression(ColumnDataCheckpointData &checkpoint_data,
                                                  unique_ptr<AnalyzeState> state) {
	return make_uniq<AlpRDCompressionState<T>>(checkpoint_data, (AlpRDAnalyzeState<T> *)state.get());
}

template <class T>
void AlpRDCompress(CompressionState &state_p, const Vector &scan_vector) {
	auto &state = (AlpRDCompressionState<T> &)state_p;
	UnifiedVectorFormat vdata;
	scan_vector.ToUnifiedFormat(vdata);
	state.Append(vdata, scan_vector.size());
}

template <class T>
void AlpRDFinalizeCompress(CompressionState &state_p) {
	auto &state = (AlpRDCompressionState<T> &)state_p;
	state.Finalize();
}

} // namespace duckdb
