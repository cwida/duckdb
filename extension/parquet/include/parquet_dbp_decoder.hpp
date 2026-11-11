//===----------------------------------------------------------------------===//
//                         DuckDB
//
// parquet_dbp_deccoder.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "decode_utils.hpp"

namespace duckdb {

class DbpDecoder {
public:
	DbpDecoder(const data_ptr_t buffer, const uint32_t buffer_len)
	    : buffer_(buffer, buffer_len),
	      //<block size in values> <number of miniblocks in a block> <total value count> <first value>
	      block_size_in_values(ParquetDecodeUtils::VarintDecode<uint64_t>(buffer_)),
	      number_of_miniblocks_per_block(DecodeNumberOfMiniblocksPerBlock(buffer_)),
	      number_of_values_in_a_miniblock(block_size_in_values / number_of_miniblocks_per_block),
	      total_value_count(ParquetDecodeUtils::VarintDecode<uint64_t>(buffer_)),
	      previous_value(ParquetDecodeUtils::ZigzagToInt(ParquetDecodeUtils::VarintDecode<uint64_t>(buffer_))),
	      // init state to something sane
	      is_first_value(true), read_values(0), min_delta(NumericLimits<int64_t>::Maximum()),
	      miniblock_index(number_of_miniblocks_per_block - 1), list_of_bitwidths_of_miniblocks(nullptr),
	      miniblock_offset(number_of_values_in_a_miniblock),
	      unpacked_data_offset(BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE) {
		if (!(block_size_in_values % number_of_miniblocks_per_block == 0 && number_of_values_in_a_miniblock > 0 &&
		      number_of_values_in_a_miniblock % BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE == 0)) {
			throw InvalidInputException("Parquet file has invalid block sizes for DELTA_BINARY_PACKED");
		}
	}

	ByteBuffer BufferPtr() const {
		return buffer_;
	}

	uint64_t TotalValues() const {
		return total_value_count;
	}

	template <typename T>
	void GetBatch(const data_ptr_t target_values_ptr, const idx_t batch_size) {
		if (read_values + batch_size > total_value_count) {
			throw std::runtime_error("DBP decode did not find enough values");
		}
		read_values += batch_size;
		GetBatchInternal<T>(target_values_ptr, batch_size);
	}

	template <class T>
	void Skip(idx_t skip_count) {
		if (read_values + skip_count > total_value_count) {
			throw std::runtime_error("DBP decode did not find enough values");
		}
		read_values += skip_count;
		GetBatchInternal<T, true>(nullptr, skip_count);
	}

	void Finalize() {
		if (miniblock_offset == number_of_values_in_a_miniblock) {
			return;
		}
		if (miniblock_offset > number_of_values_in_a_miniblock) {
			throw InvalidInputException("Parquet file has invalid miniblocks for DELTA_BINARY_PACKED");
		}
		auto data = make_unsafe_uniq_array<int64_t>(number_of_values_in_a_miniblock);
		GetBatchInternal<int64_t>(data_ptr_cast(data.get()), number_of_values_in_a_miniblock - miniblock_offset);
	}

private:
	static idx_t DecodeNumberOfMiniblocksPerBlock(ByteBuffer &buffer) {
		auto res = ParquetDecodeUtils::VarintDecode<uint64_t>(buffer);
		if (res == 0) {
			throw InvalidInputException(
			    "Parquet file has invalid number of miniblocks per block for DELTA_BINARY_PACKED");
		}
		return res;
	}

	template <typename T, bool SKIP_READ = false>
	void GetBatchInternal(const data_ptr_t target_values_ptr, const idx_t batch_size) {
		if (batch_size == 0) {
			return;
		}
		D_ASSERT(target_values_ptr || SKIP_READ);

		T *target_values = nullptr;
		if (!SKIP_READ) {
			target_values = reinterpret_cast<T *>(target_values_ptr);
		}
		idx_t target_values_offset = 0;
		if (is_first_value) {
			if (!SKIP_READ) {
				target_values[0] = static_cast<T>(previous_value);
			}
			target_values_offset++;
			is_first_value = false;
		}

		while (target_values_offset < batch_size) {
			// Copy over any remaining data
			const idx_t next = MinValue(batch_size - target_values_offset,
			                            BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE - unpacked_data_offset);
			if (next != 0) {
				for (idx_t i = 0; i < next; i++) {
					const auto &unpacked_value = unpacked_data[unpacked_data_offset + i];
					auto current_value = static_cast<T>(static_cast<uint64_t>(previous_value) +
					                                    static_cast<uint64_t>(min_delta) + unpacked_value);
					if (!SKIP_READ) {
						target_values[target_values_offset + i] = current_value;
					}
					previous_value = static_cast<int64_t>(current_value);
				}
				target_values_offset += next;
				unpacked_data_offset += next;
				continue;
			}

			// Move to next miniblock / block
			D_ASSERT(unpacked_data_offset == BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE);
			D_ASSERT(miniblock_index < number_of_miniblocks_per_block);
			D_ASSERT(miniblock_offset <= number_of_values_in_a_miniblock);
			if (miniblock_offset == number_of_values_in_a_miniblock) {
				miniblock_offset = 0;
				if (++miniblock_index == number_of_miniblocks_per_block) {
					// <min delta> <list of bitwidths of miniblocks> <miniblocks>
					min_delta = ParquetDecodeUtils::ZigzagToInt(ParquetDecodeUtils::VarintDecode<uint64_t>(buffer_));
					buffer_.available(number_of_miniblocks_per_block);
					list_of_bitwidths_of_miniblocks = buffer_.ptr;
					buffer_.unsafe_inc(number_of_miniblocks_per_block);
					miniblock_index = 0;
				}
			}

			// Unpack from current miniblock
			ParquetDecodeUtils::BitUnpackAligned(buffer_, unpacked_data,
			                                     BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE,
			                                     list_of_bitwidths_of_miniblocks[miniblock_index]);
			unpacked_data_offset = 0;
			miniblock_offset += BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE;
		}
	}

private:
	ByteBuffer buffer_;
	const idx_t block_size_in_values;
	const idx_t number_of_miniblocks_per_block;
	const idx_t number_of_values_in_a_miniblock;
	const idx_t total_value_count;
	int64_t previous_value;

	bool is_first_value;
	idx_t read_values;

	//! Block stuff
	int64_t min_delta;
	idx_t miniblock_index;
	bitpacking_width_t *list_of_bitwidths_of_miniblocks;
	idx_t miniblock_offset;
	uint64_t unpacked_data[BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE];
	idx_t unpacked_data_offset;
};
} // namespace duckdb
