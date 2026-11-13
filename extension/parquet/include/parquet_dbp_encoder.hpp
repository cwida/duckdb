//===----------------------------------------------------------------------===//
//                         DuckDB
//
// parquet_dbp_encoder.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "decode_utils.hpp"

namespace duckdb {

class DbpEncoder {
private:
	static constexpr uint64_t BLOCK_SIZE_IN_VALUES = 2048;
	static constexpr uint64_t NUMBER_OF_MINIBLOCKS_IN_A_BLOCK = 8;
	static constexpr uint64_t NUMBER_OF_VALUES_IN_A_MINIBLOCK = BLOCK_SIZE_IN_VALUES / NUMBER_OF_MINIBLOCKS_IN_A_BLOCK;

public:
	explicit DbpEncoder(const idx_t total_value_count_p) : total_value_count(total_value_count_p), count(0) {
	}

public:
	template <class T>
	void BeginWrite(WriteStream &writer, const T &first_value) {
		throw InternalException("DbpEncoder should only be used with integers");
	}

	template <class T>
	void WriteValue(WriteStream &writer, const T &value) {
		throw InternalException("DbpEncoder should only be used with integers");
	}

	void FinishWrite(WriteStream &writer) {
		if (count + block_count != total_value_count) {
			throw InternalException("value count mismatch when writing DELTA_BINARY_PACKED");
		}
		if (block_count != 0) {
			WriteBlock(writer);
		}
	}

private:
	//! PHYSICAL_TYPE is the signed Parquet physical type (int32_t or int64_t)
	template <class PHYSICAL_TYPE>
	void BeginWriteInternal(WriteStream &writer, const int64_t &value) {
		const int64_t first_value = static_cast<PHYSICAL_TYPE>(value);
		// <block size in values> <number of miniblocks in a block> <total value count> <first value>

		// the block size is a multiple of 128; it is stored as a ULEB128 int
		ParquetDecodeUtils::VarintEncode(BLOCK_SIZE_IN_VALUES, writer);
		// the miniblock count per block is a divisor of the block size such that their quotient,
		// the number of values in a miniblock, is a multiple of 32
		static_assert(BLOCK_SIZE_IN_VALUES % NUMBER_OF_MINIBLOCKS_IN_A_BLOCK == 0 &&
		                  NUMBER_OF_VALUES_IN_A_MINIBLOCK % BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE == 0,
		              "invalid block sizes for DELTA_BINARY_PACKED");
		// it is stored as a ULEB128 int
		ParquetDecodeUtils::VarintEncode(NUMBER_OF_MINIBLOCKS_IN_A_BLOCK, writer);
		// the total value count is stored as a ULEB128 int
		ParquetDecodeUtils::VarintEncode(total_value_count, writer);
		// the first value is stored as a zigzag ULEB128 int
		ParquetDecodeUtils::VarintEncode(ParquetDecodeUtils::IntToZigzag(first_value), writer);

		// initialize
		if (total_value_count != 0) {
			count++;
		}
		previous_value = first_value;

		min_delta = NumericLimits<int64_t>::Maximum();
		block_count = 0;
	}

	template <class PHYSICAL_TYPE>
	void WriteValueInternal(WriteStream &writer, const int64_t &value) {
		// 1. Compute the differences between consecutive elements. For the first element in the block,
		// use the last element in the previous block or, in the case of the first block,
		// use the first value of the whole sequence, stored in the header.

		// Subtractions in steps 1) and 2) may incur signed arithmetic overflow,
		// and so will the corresponding additions when decoding.
		// Overflow should be allowed and handled as wrapping around in 2’s complement notation
		// so that the original values are correctly restituted.
		// This may require explicit care in some programming languages
		// (for example by doing all arithmetic in the unsigned domain).
		// Wrap around in the width of the physical type
		using UNSIGNED_TYPE = typename std::make_unsigned<PHYSICAL_TYPE>::type;
		const int64_t delta =
		    static_cast<PHYSICAL_TYPE>(static_cast<UNSIGNED_TYPE>(value) - static_cast<UNSIGNED_TYPE>(previous_value));
		previous_value = value;
		// Compute the frame of reference (the minimum of the deltas in the block).
		min_delta = MinValue(min_delta, delta);
		// append. if block is full, write it out
		data[block_count++] = delta;
		if (block_count == BLOCK_SIZE_IN_VALUES) {
			WriteBlock(writer);
		}
	}

	void WriteBlock(WriteStream &writer) {
		D_ASSERT(count + block_count == total_value_count || block_count == BLOCK_SIZE_IN_VALUES);
		const auto number_of_miniblocks =
		    (block_count + NUMBER_OF_VALUES_IN_A_MINIBLOCK - 1) / NUMBER_OF_VALUES_IN_A_MINIBLOCK;
		for (idx_t miniblock_idx = 0; miniblock_idx < number_of_miniblocks; miniblock_idx++) {
			for (idx_t i = 0; i < NUMBER_OF_VALUES_IN_A_MINIBLOCK; i++) {
				const idx_t index = miniblock_idx * NUMBER_OF_VALUES_IN_A_MINIBLOCK + i;
				auto &value = data[index];
				if (index < block_count) {
					// 2. Compute the frame of reference (the minimum of the deltas in the block).
					// Subtract this min delta from all deltas in the block.
					// This guarantees that all values are non-negative.
					D_ASSERT(min_delta <= value);
					value = static_cast<int64_t>(static_cast<uint64_t>(value) - static_cast<uint64_t>(min_delta));
				} else {
					// If there are not enough values to fill the last miniblock, we pad the miniblock
					// so that its length is always the number of values in a full miniblock multiplied by the bit
					// width. The values of the padding bits should be zero, but readers must accept paddings consisting
					// of arbitrary bits as well.
					value = 0;
				}
			}
		}

		for (idx_t miniblock_idx = 0; miniblock_idx < NUMBER_OF_MINIBLOCKS_IN_A_BLOCK; miniblock_idx++) {
			auto &width = list_of_bitwidths_of_miniblocks[miniblock_idx];
			if (miniblock_idx < number_of_miniblocks) {
				const auto src = &data[miniblock_idx * NUMBER_OF_VALUES_IN_A_MINIBLOCK];
				width = BitpackingPrimitives::MinimumBitWidth(reinterpret_cast<uint64_t *>(src),
				                                              NUMBER_OF_VALUES_IN_A_MINIBLOCK);
				D_ASSERT(width <= sizeof(int64_t) * 8);
			} else {
				// If, in the last block, less than <number of miniblocks in a block> miniblocks are needed to store the
				// values, the bytes storing the bit widths of the unneeded miniblocks are still present, their value
				// should be zero, but readers must accept arbitrary values as well. There are no additional padding
				// bytes for the miniblock bodies though, as if their bit widths were 0 (regardless of the actual byte
				// values). The reader knows when to stop reading by keeping track of the number of values read.
				width = 0;
			}
		}

		// 3. Encode the frame of reference (min delta) as a zigzag ULEB128 int
		// followed by the bit widths of the miniblocks
		// and the delta values (minus the min delta) bit-packed per miniblock.
		// <min delta> <list of bitwidths of miniblocks> <miniblocks>

		// the min delta is a zigzag ULEB128 int (we compute a minimum as we need positive integers for bit packing)
		ParquetDecodeUtils::VarintEncode(ParquetDecodeUtils::IntToZigzag(min_delta), writer);
		// the bitwidth of each block is stored as a byte
		writer.WriteData(list_of_bitwidths_of_miniblocks, NUMBER_OF_MINIBLOCKS_IN_A_BLOCK);
		// each miniblock is a list of bit packed ints according to the bit width stored at the beginning of the block
		for (idx_t miniblock_idx = 0; miniblock_idx < number_of_miniblocks; miniblock_idx++) {
			const auto src = &data[miniblock_idx * NUMBER_OF_VALUES_IN_A_MINIBLOCK];
			const auto &width = list_of_bitwidths_of_miniblocks[miniblock_idx];
			memset(data_packed, 0, sizeof(data_packed));
			ParquetDecodeUtils::BitPackAligned(reinterpret_cast<uint64_t *>(src), data_packed,
			                                   NUMBER_OF_VALUES_IN_A_MINIBLOCK, width);
			const auto write_size = NUMBER_OF_VALUES_IN_A_MINIBLOCK * width / 8;
#ifdef DEBUG
			// immediately verify that unpacking yields the input data
			int64_t verification_data[NUMBER_OF_VALUES_IN_A_MINIBLOCK];
			ByteBuffer byte_buffer(data_ptr_cast(data_packed), write_size);
			bitpacking_width_t bitpack_pos = 0;
			ParquetDecodeUtils::BitUnpack(byte_buffer, bitpack_pos, reinterpret_cast<uint64_t *>(verification_data),
			                              NUMBER_OF_VALUES_IN_A_MINIBLOCK, width);
			for (idx_t i = 0; i < NUMBER_OF_VALUES_IN_A_MINIBLOCK; i++) {
				D_ASSERT(src[i] == verification_data[i]);
			}
#endif
			writer.WriteData(data_packed, write_size);
		}

		count += block_count;

		min_delta = NumericLimits<int64_t>::Maximum();
		block_count = 0;
	}

private:
	//! Overall fields
	const idx_t total_value_count;
	idx_t count;
	int64_t previous_value;

	//! Block-specific fields
	int64_t min_delta;
	int64_t data[BLOCK_SIZE_IN_VALUES];
	idx_t block_count;

	//! Bitpacking fields
	bitpacking_width_t list_of_bitwidths_of_miniblocks[NUMBER_OF_MINIBLOCKS_IN_A_BLOCK];
	data_t data_packed[NUMBER_OF_VALUES_IN_A_MINIBLOCK * sizeof(int64_t)];
};

template <>
inline void DbpEncoder::BeginWrite(WriteStream &writer, const int32_t &first_value) {
	BeginWriteInternal<int32_t>(writer, static_cast<int64_t>(first_value));
}

template <>
inline void DbpEncoder::BeginWrite(WriteStream &writer, const int64_t &first_value) {
	BeginWriteInternal<int64_t>(writer, static_cast<int64_t>(first_value));
}

template <>
inline void DbpEncoder::BeginWrite(WriteStream &writer, const uint32_t &first_value) {
	BeginWriteInternal<int32_t>(writer, static_cast<int64_t>(first_value));
}

template <>
inline void DbpEncoder::BeginWrite(WriteStream &writer, const uint64_t &first_value) {
	BeginWriteInternal<int64_t>(writer, static_cast<int64_t>(first_value));
}

template <>
inline void DbpEncoder::WriteValue(WriteStream &writer, const int32_t &first_value) {
	WriteValueInternal<int32_t>(writer, static_cast<int64_t>(first_value));
}

template <>
inline void DbpEncoder::WriteValue(WriteStream &writer, const int64_t &first_value) {
	WriteValueInternal<int64_t>(writer, static_cast<int64_t>(first_value));
}

template <>
inline void DbpEncoder::WriteValue(WriteStream &writer, const uint32_t &first_value) {
	WriteValueInternal<int32_t>(writer, static_cast<int64_t>(first_value));
}

template <>
inline void DbpEncoder::WriteValue(WriteStream &writer, const uint64_t &first_value) {
	WriteValueInternal<int64_t>(writer, static_cast<int64_t>(first_value));
}

} // namespace duckdb
