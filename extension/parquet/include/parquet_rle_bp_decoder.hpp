//===----------------------------------------------------------------------===//
//                         DuckDB
//
// parquet_rle_bp_decoder.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once
#include "decode_utils.hpp"
#include "parquet_types.h"
#include "resizable_buffer.hpp"
#include "thrift_tools.hpp"

namespace duckdb {

class RleBpDecoder {
public:
	/// Create a decoder object. buffer/buffer_len is the decoded data.
	/// bit_width is the width of each value (before encoding).
	RleBpDecoder(data_ptr_t buffer, uint32_t buffer_len, uint32_t bit_width)
	    : buffer_(buffer, buffer_len), bit_width_(bit_width), current_value_(0), repeat_count_(0), literal_count_(0) {
		if (bit_width >= 64) {
			throw std::runtime_error("Decode bit width too large");
		}
		byte_encoded_len = ((bit_width_ + 7) / 8);
		max_val = (uint64_t(1) << bit_width_) - 1;
	}

	template <class T>
	bool HasRepeatedBatch(const uint32_t batch_size, const T value) {
		if (repeat_count_ == 0 && literal_count_ == 0) {
			NextCounts();
		}
		return repeat_count_ >= batch_size && current_value_ == static_cast<uint64_t>(value);
	}

	template <typename T>
	void GetRepeatedBatch(const uint32_t batch_size, const T value) {
		D_ASSERT(repeat_count_ >= batch_size && current_value_ == static_cast<uint64_t>(value));
		repeat_count_ -= batch_size;
	}

	template <typename T>
	void GetBatch(data_ptr_t values_target_ptr, const uint32_t batch_size) {
		auto values = reinterpret_cast<T *>(values_target_ptr);
		uint32_t values_read = 0;

		while (values_read < batch_size) {
			if (repeat_count_ > 0) {
				auto repeat_batch = MinValue<uint32_t>(batch_size - values_read, repeat_count_);
				std::fill_n(values + values_read, repeat_batch, static_cast<T>(current_value_));
				repeat_count_ -= repeat_batch;
				values_read += repeat_batch;
			} else if (literal_count_ > 0) {
				auto literal_batch = MinValue<uint32_t>(batch_size - values_read, literal_count_);
				ParquetDecodeUtils::BitUnpack<T>(buffer_, bitpack_pos, values + values_read, literal_batch, bit_width_);
				literal_count_ -= literal_batch;
				values_read += literal_batch;
			} else {
				NextCounts();
			}
		}
		D_ASSERT(values_read == batch_size);
	}

	void Skip(uint32_t batch_size) {
		uint32_t values_skipped = 0;

		while (values_skipped < batch_size) {
			if (repeat_count_ > 0) {
				auto repeat_batch = MinValue<uint32_t>(batch_size - values_skipped, repeat_count_);
				repeat_count_ -= repeat_batch;
				values_skipped += repeat_batch;
			} else if (literal_count_ > 0) {
				auto literal_batch = MinValue<uint32_t>(batch_size - values_skipped, literal_count_);
				ParquetDecodeUtils::Skip(buffer_, bitpack_pos, literal_batch, bit_width_);
				literal_count_ -= literal_batch;
				values_skipped += literal_batch;
			} else {
				NextCounts();
			}
		}
		D_ASSERT(values_skipped == batch_size);
	}

	static uint8_t ComputeBitWidthFromMaxValue(idx_t max_value) {
		if (max_value == 0) {
			return 0;
		}
		uint8_t ret = 1;
		while ((((idx_t)1u << (idx_t)ret) - 1) < max_value) {
			ret++;
		}
		return ret;
	}

	static uint8_t ComputeBitWidthFromValueCount(idx_t value_count) {
		if (value_count == 0) {
			return 0;
		}
		if (value_count == 1) {
			// A singleton dictionary only emits index 0. In theory that needs 0 bits, but the
			// current writer still assumes dictionary bit widths are non-zero when it flushes pages.
			return 1;
		}
		return ComputeBitWidthFromMaxValue(value_count - 1);
	}

private:
	ByteBuffer buffer_;

	/// Number of bits needed to encode the value. Must be between 0 and 64.
	uint32_t bit_width_;
	uint64_t current_value_;
	uint32_t repeat_count_;
	uint32_t literal_count_;
	uint8_t byte_encoded_len;
	uint64_t max_val;

	uint8_t bitpack_pos = 0;

	/// Fills literal_count_ and repeat_count_ with next values. Returns false if there
	/// are no more.
	template <bool CHECKED>
	void NextCountsTemplated() {
		// Read the next run's indicator int, it could be a literal or repeated run.
		// The int is encoded as a vlq-encoded value.
		if (bitpack_pos != 0) {
			if (CHECKED) {
				buffer_.inc(1);
			} else {
				buffer_.unsafe_inc(1);
			}
			bitpack_pos = 0;
		}
		auto indicator_value = ParquetDecodeUtils::VarintDecode<uint32_t, CHECKED>(buffer_);

		// lsb indicates if it is a literal run or repeated run
		bool is_literal = indicator_value & 1;
		if (is_literal) {
			literal_count_ = (indicator_value >> 1) * 8;
		} else {
			repeat_count_ = indicator_value >> 1;
			// (ARROW-4018) this is not big-endian compatible, lol
			current_value_ = 0;
			if (CHECKED) {
				buffer_.available(byte_encoded_len);
			}
			for (auto i = 0; i < byte_encoded_len; i++) {
				auto next_byte = Load<uint8_t>(buffer_.ptr + i);
				current_value_ |= (static_cast<uint64_t>(next_byte) << (i * 8));
			}
			buffer_.unsafe_inc(byte_encoded_len);
			// sanity check
			if (repeat_count_ > 0 && current_value_ > max_val) {
				throw std::runtime_error("Payload value bigger than allowed. Corrupted file?");
			}
		}
	}

	void NextCounts() {
		if (buffer_.check_available(byte_encoded_len + sizeof(uint32_t) + 2)) {
			NextCountsTemplated<false>();
		} else {
			NextCountsTemplated<true>();
		}
	}
};
} // namespace duckdb
