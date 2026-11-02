//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/compression/chimp/chimp_scan.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/storage/compression/chimp/chimp.hpp"
#include "duckdb/storage/compression/chimp/algorithm/chimp_utils.hpp"

#include "duckdb/common/limits.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/function/compression_function.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/compression/compression_segment_reader.hpp"

#include "duckdb/storage/table/column_segment.hpp"

#include "duckdb/storage/compression/chimp/algorithm/flag_buffer.hpp"
#include "duckdb/storage/compression/chimp/algorithm/leading_zero_buffer.hpp"
#include "duckdb/storage/table/scan_state.hpp"

namespace duckdb {

[[noreturn]] void ThrowChimpMetadataBeforeHeader();
[[noreturn]] void ThrowChimpLeadingZeroBlockCountOutOfBounds(uint8_t block_count);
[[noreturn]] void ThrowChimpLeadingZeroCountMismatch(idx_t stored_count, idx_t required_count);
[[noreturn]] void ThrowChimpPackedDataExceedsType(uint8_t leading_zero, uint8_t significant_bits, idx_t bit_width);
[[noreturn]] void ThrowChimpLeadingZeroStateMissing();

template <class CHIMP_TYPE>
struct ChimpGroupState {
public:
	void Reset() {
		chimp_state.Reset();
		index = 0;
	}

	bool Started() const {
		return !!index;
	}

	// Assuming the group is completely full
	idx_t RemainingInGroup() const {
		return ChimpPrimitives::CHIMP_SEQUENCE_SIZE - index;
	}

	void Scan(CHIMP_TYPE *dest, idx_t count) {
		memcpy(dest, (void *)(values + index), count * sizeof(CHIMP_TYPE));
		index += count;
	}

	void LoadFlags(unsafe_array_ptr<const uint8_t> packed_data, idx_t group_size) {
		FlagBuffer<false> flag_buffer(packed_data);
		flags[0] = ChimpConstants::Flags::VALUE_IDENTICAL; // First value doesn't require a flag
		for (idx_t i = 0; i < group_size; i++) {
			flags[1 + i] = static_cast<ChimpConstants::Flags>(flag_buffer.Extract());
		}
		max_flags_to_read = group_size;
		index = 0;
	}

	void LoadLeadingZeros(unsafe_array_ptr<const uint8_t> packed_data, idx_t leading_zero_count) {
		LeadingZeroBuffer<false> leading_zero_buffer(packed_data);
		for (idx_t i = 0; i < leading_zero_count; i++) {
			leading_zeros[i] = ChimpConstants::Decompression::LEADING_REPRESENTATION[leading_zero_buffer.Extract()];
		}
		max_leading_zeros_to_read = leading_zero_count;
		leading_zero_index = 0;
	}

	idx_t CalculatePackedDataCount() const {
		idx_t count = 0;
		for (idx_t i = 0; i < max_flags_to_read; i++) {
			count += flags[1 + i] == ChimpConstants::Flags::TRAILING_EXCEEDS_THRESHOLD;
		}
		return count;
	}

	idx_t CalculateLeadingZeroCount() const {
		idx_t count = 0;
		for (idx_t i = 0; i < max_flags_to_read; i++) {
			count += flags[1 + i] == ChimpConstants::Flags::LEADING_ZERO_LOAD;
		}
		return AlignValue<idx_t, 8>(count);
	}

	void LoadPackedData(unsafe_array_ptr<const uint16_t> packed_data) {
		auto packed_data_block_count = packed_data.size();
		for (idx_t i = 0; i < packed_data_block_count; i++) {
			auto unpacked = PackedDataUtils<CHIMP_TYPE>::Unpack(packed_data[i]);
			if (unpacked.significant_bits == 0) {
				unpacked.significant_bits = 64;
			}
			unpacked.leading_zero = ChimpConstants::Decompression::LEADING_REPRESENTATION[unpacked.leading_zero];
			unpacked_data_blocks[i] = unpacked;
		}
		unpacked_index = 0;
		max_packed_data_to_read = packed_data_block_count;
	}

	// Count how many bits the decoder should read based on the metadata.
	// LoadGroup checks the resulting range through CompressionSegmentReader before the BitReader reads any values.
	idx_t ValidateAndCalculateDataBitCount(idx_t group_size) const {
		using Decompression = Chimp128Decompression<CHIMP_TYPE>;
		D_ASSERT(group_size == max_flags_to_read + 1);

		// The first value is stored at full width and has no flag.
		idx_t data_bit_count = Decompression::BIT_SIZE;
		idx_t leading_zero_position = 0;
		idx_t packed_data_position = 0;
		uint8_t leading_zero = NumericLimits<uint8_t>::Maximum();
		for (idx_t i = 1; i < group_size; i++) {
			switch (flags[i]) {
			case ChimpConstants::Flags::VALUE_IDENTICAL:
				// Identical values still read a ring-buffer index from the bitstream.
				data_bit_count += Decompression::INDEX_BITS_SIZE;
				break;
			case ChimpConstants::Flags::TRAILING_EXCEEDS_THRESHOLD: {
				D_ASSERT(packed_data_position < max_packed_data_to_read);
				auto &unpacked = unpacked_data_blocks[packed_data_position++];
				// The decoder derives its left shift as BIT_SIZE - significant_bits - leading_zero.
				if (unpacked.leading_zero > Decompression::BIT_SIZE ||
				    unpacked.significant_bits > Decompression::BIT_SIZE - unpacked.leading_zero) {
					ThrowChimpPackedDataExceedsType(unpacked.leading_zero, unpacked.significant_bits,
					                                Decompression::BIT_SIZE);
				}
				leading_zero = unpacked.leading_zero;
				data_bit_count += unpacked.significant_bits;
				break;
			}
			case ChimpConstants::Flags::LEADING_ZERO_EQUALITY:
				// Equality reuses the decoder's leading-zero count, the initial value of 255 is invalid.
				if (leading_zero > Decompression::BIT_SIZE) {
					ThrowChimpLeadingZeroStateMissing();
				}
				data_bit_count += Decompression::BIT_SIZE - leading_zero;
				break;
			case ChimpConstants::Flags::LEADING_ZERO_LOAD:
				D_ASSERT(leading_zero_position < max_leading_zeros_to_read);
				leading_zero = leading_zeros[leading_zero_position++];
				data_bit_count += Decompression::BIT_SIZE - leading_zero;
				break;
			}
		}
		D_ASSERT(packed_data_position == max_packed_data_to_read);
		return data_bit_count;
	}

	void LoadValues(BitReader &input, CHIMP_TYPE *result, idx_t count) {
		for (idx_t i = 0; i < count; i++) {
			result[i] = Chimp128Decompression<CHIMP_TYPE>::Load(
			    flags[i], leading_zeros, leading_zero_index, unpacked_data_blocks, unpacked_index, chimp_state, input);
		}
	}

public:
	uint32_t leading_zero_index;
	uint32_t unpacked_index;

	ChimpConstants::Flags flags[ChimpPrimitives::CHIMP_SEQUENCE_SIZE + 1];
	uint8_t leading_zeros[ChimpPrimitives::CHIMP_SEQUENCE_SIZE + 1];
	UnpackedData unpacked_data_blocks[ChimpPrimitives::CHIMP_SEQUENCE_SIZE];

	CHIMP_TYPE values[ChimpPrimitives::CHIMP_SEQUENCE_SIZE];

private:
	idx_t index = 0;
	idx_t max_leading_zeros_to_read;
	idx_t max_flags_to_read;
	idx_t max_packed_data_to_read;
	Chimp128DecompressionState<CHIMP_TYPE> chimp_state;
};

template <class T>
struct ChimpScanState : public SegmentScanState {
public:
	using CHIMP_TYPE = typename ChimpType<T>::TYPE;

	explicit ChimpScanState(BufferHandle handle_p, ColumnSegment &segment)
	    : handle(std::move(handle_p)),
	      metadata(CompressionSegmentReader::FromSegment(handle, segment, "Chimp segment")), segment(segment),
	      segment_count(segment.count) {
		auto metadata_end = metadata.template Read<ChimpPrimitives::METADATA_POINTER_TYPE>();
		if (metadata_end < ChimpPrimitives::HEADER_SIZE) {
			ThrowChimpMetadataBeforeHeader();
		}
		metadata = metadata.GetSubReader(0, metadata_end, "Chimp segment");

		metadata.SetPosition(metadata_end);
	}

	BufferHandle handle;
	CompressionSegmentReader metadata;
	idx_t total_value_count = 0;
	idx_t data_bit_position = 0;
	ChimpGroupState<CHIMP_TYPE> group_state;

	ColumnSegment &segment;
	idx_t segment_count;

	idx_t LeftInGroup() const {
		return ChimpPrimitives::CHIMP_SEQUENCE_SIZE - (total_value_count % ChimpPrimitives::CHIMP_SEQUENCE_SIZE);
	}

	bool GroupFinished() const {
		return (total_value_count % ChimpPrimitives::CHIMP_SEQUENCE_SIZE) == 0;
	}

	template <class CHIMP_TYPE>
	void ScanGroup(CHIMP_TYPE *values, idx_t group_size) {
		D_ASSERT(group_size <= ChimpPrimitives::CHIMP_SEQUENCE_SIZE);
		D_ASSERT(group_size <= LeftInGroup());

		if (GroupFinished() && total_value_count < segment_count) {
			if (group_size == ChimpPrimitives::CHIMP_SEQUENCE_SIZE) {
				LoadGroup(values);
				total_value_count += group_size;
				return;
			} else {
				LoadGroup(group_state.values);
			}
		}
		group_state.Scan(values, group_size);
		total_value_count += group_size;
	}

	void LoadGroup(CHIMP_TYPE *value_buffer) {
		D_ASSERT(GroupFinished());
		D_ASSERT(total_value_count < segment_count);

		//! FIXME: If we change the order of this to flag -> leading_zero_blocks -> packed_data
		//! We can leave out the leading zero block count as well, because it can be derived from
		//! Extracting all the flags and counting the 3's

		// The stored group data offset is unused by scan and fetch
		(void)metadata.ReadBackward<uint32_t>();

		// Load how many blocks of leading zero bits we have
		auto leading_zero_block_count = metadata.ReadBackward<uint8_t>();
		if (leading_zero_block_count > ChimpPrimitives::CHIMP_SEQUENCE_SIZE / 8) {
			ThrowChimpLeadingZeroBlockCountOutOfBounds(leading_zero_block_count);
		}

		// Load the leading zero blocks
		auto leading_zero_blocks = metadata.ReadBytesBackward(3ULL * leading_zero_block_count);

		// Figure out how many flags there are
		D_ASSERT(segment_count >= total_value_count);
		auto group_size = MinValue<idx_t>(segment_count - total_value_count, ChimpPrimitives::CHIMP_SEQUENCE_SIZE);
		// Reduce by one, because the first value of a group does not have a flag
		auto flag_count = group_size - 1;
		uint16_t flag_byte_count = AlignValue<uint16_t, 4>(UnsafeNumericCast<uint16_t>(flag_count)) / 4;

		// Load the flags
		auto flags = metadata.ReadBytesBackward(flag_byte_count);
		group_state.LoadFlags(flags, flag_count);

		// Load the leading zero blocks
		auto leading_zero_count = static_cast<idx_t>(leading_zero_block_count) * 8;
		auto required_leading_zero_count = group_state.CalculateLeadingZeroCount();
		if (leading_zero_count != required_leading_zero_count) {
			ThrowChimpLeadingZeroCountMismatch(leading_zero_count, required_leading_zero_count);
		}
		group_state.LoadLeadingZeros(leading_zero_blocks, leading_zero_count);

		// Load packed data blocks
		auto packed_data_block_count = group_state.CalculatePackedDataCount();
		// Align (backwards) on a two-byte boundary
		metadata.AlignBackward(sizeof(uint16_t));
		auto packed_data = metadata.ReadArrayBackward<uint16_t>(packed_data_block_count);
		group_state.LoadPackedData(packed_data);

		// Validate the full group data range before using the unchecked bit reader
		auto data_bit_count = group_state.ValidateAndCalculateDataBitCount(group_size);
		auto bit_offset = UnsafeNumericCast<uint8_t>(data_bit_position & 7);
		auto data_byte_position = data_bit_position >> 3;
		auto data_byte_count = (static_cast<idx_t>(bit_offset) + data_bit_count + 7) / 8;
		auto data = metadata.GetSubReader(ChimpPrimitives::HEADER_SIZE, metadata.Size() - ChimpPrimitives::HEADER_SIZE,
		                                  "Chimp data");
		BitReader input(data.GetBytes(data_byte_position, data_byte_count), bit_offset);
		group_state.Reset();

		// Load all values for the group
		group_state.LoadValues(input, value_buffer, group_size);
		data_bit_position += data_bit_count;
	}

public:
	//! Skip the next 'skip_count' values, we don't store the values
	// TODO: use the metadata to determine if we can skip a group
	void Skip(ColumnSegment &segment, idx_t skip_count) {
		using INTERNAL_TYPE = typename ChimpType<T>::TYPE;
		INTERNAL_TYPE buffer[ChimpPrimitives::CHIMP_SEQUENCE_SIZE];
		D_ASSERT(total_value_count <= segment_count);
		D_ASSERT(skip_count <= segment_count - total_value_count);

		while (skip_count) {
			auto skip_size = MinValue(skip_count, LeftInGroup());
			ScanGroup<CHIMP_TYPE>(buffer, skip_size);
			skip_count -= skip_size;
		}
	}
};

template <class T>
unique_ptr<SegmentScanState> ChimpInitScan(const QueryContext &context, ColumnSegment &segment) {
	auto &buffer_manager = BufferManager::GetBufferManager(segment.GetDatabase());
	auto handle = buffer_manager.Pin(context, segment.GetBlockHandle());
	auto result = make_uniq_base<SegmentScanState, ChimpScanState<T>>(std::move(handle), segment);
	return result;
}

//===--------------------------------------------------------------------===//
// Scan base data
//===--------------------------------------------------------------------===//
template <class T>
void ChimpScanPartial(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result,
                      idx_t result_offset) {
	using INTERNAL_TYPE = typename ChimpType<T>::TYPE;
	auto &scan_state = state.scan_state->Cast<ChimpScanState<T>>();
	D_ASSERT(scan_state.total_value_count <= scan_state.segment_count);
	D_ASSERT(scan_count <= scan_state.segment_count - scan_state.total_value_count);

	T *result_data = FlatVector::GetDataMutable<T>(result);
	result.SetVectorType(VectorType::FLAT_VECTOR);

	auto current_result_ptr = (INTERNAL_TYPE *)(result_data + result_offset);

	idx_t scanned = 0;
	while (scanned < scan_count) {
		idx_t to_scan = MinValue(scan_count - scanned, scan_state.LeftInGroup());
		scan_state.template ScanGroup<INTERNAL_TYPE>(current_result_ptr + scanned, to_scan);
		scanned += to_scan;
	}
}

template <class T>
void ChimpSkip(ColumnSegment &segment, ColumnScanState &state, idx_t skip_count) {
	auto &scan_state = state.scan_state->Cast<ChimpScanState<T>>();
	scan_state.Skip(segment, skip_count);
}

template <class T>
void ChimpScan(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result) {
	ChimpScanPartial<T>(segment, state, scan_count, result, 0);
}

} // namespace duckdb
