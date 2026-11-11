#include "decoder/delta_length_byte_array_decoder.hpp"

#include <memory>
#include <stdexcept>

#include "decoder/delta_byte_array_decoder.hpp"
#include "column_reader.hpp"
#include "parquet_reader.hpp"
#include "reader/string_column_reader.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "resizable_buffer.hpp"

namespace duckdb {
class Vector;

DeltaLengthByteArrayDecoder::DeltaLengthByteArrayDecoder(ColumnReader &reader)
    : reader(reader), length_buffer(reader.encoding_buffers[0]), length_idx(0) {
}

void DeltaLengthByteArrayDecoder::InitializePage() {
	if (reader.Type().InternalType() != PhysicalType::VARCHAR) {
		throw std::runtime_error("Delta Length Byte Array encoding is only supported for string/blob data");
	}
	// read the binary packed lengths
	auto &block = *reader.block;
	auto &allocator = reader.reader.allocator;
	DeltaByteArrayDecoder::ReadDbpData(allocator, block, length_buffer, byte_array_count);

	// Verify that the sum of DBP string lengths match up with the available string data
	idx_t total_string_length = 0;
	const auto length_data = reinterpret_cast<uint32_t *>(length_buffer.ptr);
	for (idx_t i = 0; i < byte_array_count; i++) {
		total_string_length += length_data[i];
	}
	block.available(total_string_length);

	length_idx = 0;
}

bool IsContinuationByte(uint8_t b) {
	return (b & 0xC0) == 0x80;
}

void DeltaLengthByteArrayDecoder::Read(shared_ptr<ResizeableBuffer> &block_ref, uint8_t *defines, idx_t read_count,
                                       Vector &result, idx_t result_offset) {
	if (defines) {
		ReadInternal<true>(block_ref, defines, read_count, result, result_offset);
	} else {
		ReadInternal<false>(block_ref, defines, read_count, result, result_offset);
	}
}

template <bool HAS_DEFINES>
void DeltaLengthByteArrayDecoder::ReadInternal(shared_ptr<ResizeableBuffer> &block_ref, uint8_t *const defines,
                                               const idx_t read_count, Vector &result, const idx_t result_offset) {
	auto &block = *block_ref;
	const auto length_data = reinterpret_cast<uint32_t *>(length_buffer.ptr);

	if (!HAS_DEFINES) {
		// Fast path: take this out of the loop below
		if (length_idx + read_count > byte_array_count) {
			throw IOException(
			    "DELTA_LENGTH_BYTE_ARRAY - length mismatch between values and byte array lengths (attempted "
			    "read of %d from %d entries) - corrupt file?",
			    length_idx + read_count, byte_array_count);
		}
	}

	const auto &string_column_reader = reader.Cast<StringColumnReader>();
	string_column_reader.SetCurrentResult(result);

	// JSON and non-strict modes require individual validation: bytes are rewritten and the fast path discards its
	// return value so it can't sanitize
	const bool needs_full_individual_validation =
	    reader.Type().IsJSONType() || reader.Cast<StringColumnReader>().reader.parquet_options.utf8_validation_option !=
	                                      StringColumnReader::Utf8ValidationOption::STRICT_UTF8;

	const auto start_ptr = block.ptr;
	auto current_ptr = start_ptr;
	auto result_data = FlatVector::Writer<string_t>(result, read_count, result_offset);

	for (idx_t row_idx = 0; row_idx < read_count; row_idx++) {
		const auto result_idx = result_offset + row_idx;
		if (HAS_DEFINES) {
			if (defines[result_idx] != reader.MaxDefine()) {
				result_data.WriteNull();
				continue;
			}
			if (length_idx >= byte_array_count) {
				throw IOException(
				    "DELTA_LENGTH_BYTE_ARRAY - length mismatch between values and byte array lengths (attempted "
				    "read of %d from %d entries) - corrupt file?",
				    length_idx, byte_array_count);
			}
		}
		const auto str_len = length_data[length_idx++];

		if (needs_full_individual_validation) {
			auto verified = string_column_reader.VerifyString(char_ptr_cast(current_ptr), str_len);
			result_data.WriteValue(verified);
		} else {
			// Batch validation below catches incomplete trailing characters. Within a valid batch, a character
			// can only cross a string boundary if the next nonempty string starts with a continuation byte.
			if (str_len > 0 && IsContinuationByte(current_ptr[0])) {
				string_column_reader.VerifyString(char_ptr_cast(current_ptr), str_len);
			}
			result_data.WriteStringRef(string_t(char_ptr_cast(current_ptr), str_len));
		}
		current_ptr += str_len;
	}

	block.unsafe_inc(current_ptr - start_ptr);
	if (!needs_full_individual_validation) {
		string_column_reader.VerifyString(char_ptr_cast(start_ptr), NumericCast<uint32_t>(block.ptr - start_ptr));
	}
	StringColumnReader::ReferenceBlock(result, block_ref);
}

void DeltaLengthByteArrayDecoder::Skip(uint8_t *defines, idx_t skip_count) {
	if (defines) {
		SkipInternal<true>(defines, skip_count);
	} else {
		SkipInternal<false>(defines, skip_count);
	}
}

template <bool HAS_DEFINES>
void DeltaLengthByteArrayDecoder::SkipInternal(uint8_t *defines, idx_t skip_count) {
	auto &block = *reader.block;
	const auto length_data = reinterpret_cast<uint32_t *>(length_buffer.ptr);

	if (!HAS_DEFINES) {
		// Fast path: take this out of the loop below
		if (length_idx + skip_count > byte_array_count) {
			throw IOException(
			    "DELTA_LENGTH_BYTE_ARRAY - length mismatch between values and byte array lengths (attempted "
			    "read of %d from %d entries) - corrupt file?",
			    length_idx + skip_count, byte_array_count);
		}
	}

	idx_t skip_bytes = 0;
	for (idx_t row_idx = 0; row_idx < skip_count; row_idx++) {
		if (HAS_DEFINES) {
			if (defines[row_idx] != reader.MaxDefine()) {
				continue;
			}
			if (length_idx >= byte_array_count) {
				throw IOException(
				    "DELTA_LENGTH_BYTE_ARRAY - length mismatch between values and byte array lengths (attempted "
				    "read of %d from %d entries) - corrupt file?",
				    length_idx, byte_array_count);
			}
		}
		skip_bytes += length_data[length_idx++];
	}
	block.inc(skip_bytes);
}

} // namespace duckdb
