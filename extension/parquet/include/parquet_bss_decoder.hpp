//===----------------------------------------------------------------------===//
//                         DuckDB
//
// parquet_bss_decoder.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once
#include "parquet_types.h"
#include "resizable_buffer.hpp"

namespace duckdb {

/// Decoder for the Byte Stream Split encoding
class BssDecoder {
public:
	/// Create a decoder object. buffer/buffer_len is the encoded data, type_size the width of a single value.
	BssDecoder(data_ptr_t buffer, uint32_t buffer_len, idx_t type_size)
	    : buffer_(buffer, buffer_len), type_size_(type_size), value_offset_(0) {
		if (type_size == 0 || buffer_len % type_size != 0) {
			duckdb::stringstream error;
			error << "Data buffer size for the BYTE_STREAM_SPLIT encoding (" << buffer_len
			      << ") should be a multiple of the type size (" << type_size << ")";
			throw std::runtime_error(error.str());
		}
		num_buffer_values_ = buffer_len / type_size;
	}

public:
	/// Writes batch_size values in the PLAIN layout (consecutive type_size-byte values) to values_target_ptr
	void GetBatch(data_ptr_t values_target_ptr, uint32_t batch_size) {
		buffer_.available((value_offset_ + batch_size) * type_size_);

		for (idx_t byte_offset = 0; byte_offset < type_size_; ++byte_offset) {
			data_ptr_t input_bytes = buffer_.ptr + byte_offset * num_buffer_values_ + value_offset_;
			for (uint32_t i = 0; i < batch_size; ++i) {
				values_target_ptr[byte_offset + i * type_size_] = *(input_bytes + i);
			}
		}
		value_offset_ += batch_size;
	}

	void Skip(uint32_t batch_size) {
		buffer_.available((value_offset_ + batch_size) * type_size_);
		value_offset_ += batch_size;
	}

private:
	ByteBuffer buffer_;
	idx_t type_size_;
	uint32_t num_buffer_values_;
	uint32_t value_offset_;
};

} // namespace duckdb
