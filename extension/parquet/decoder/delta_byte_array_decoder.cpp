#include "decoder/delta_byte_array_decoder.hpp"

#include <string.h>
#include <stdexcept>
#include <utility>

#include "column_reader.hpp"
#include "parquet_reader.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/operator/add.hpp"
#include "duckdb/common/operator/multiply.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "parquet_column_schema.hpp"
#include "parquet_dbp_decoder.hpp"
#include "parquet_types.h"
#include "resizable_buffer.hpp"

namespace duckdb {
class Allocator;
class Vector;

DeltaByteArrayDecoder::DeltaByteArrayDecoder(ColumnReader &reader) : reader(reader) {
}

void DeltaByteArrayDecoder::ReadDbpData(Allocator &allocator, ResizeableBuffer &buffer, ResizeableBuffer &result_buffer,
                                        idx_t &value_count) {
	auto decoder = make_uniq<DbpDecoder>(buffer.ptr, buffer.len);
	value_count = decoder->TotalValues();
	result_buffer.reset();
	// value_count is read from the file, so the buffer size can overflow on a corrupt input
	idx_t result_size;
	if (!TryMultiplyOperator::Operation<idx_t, idx_t, idx_t>(value_count, sizeof(uint32_t), result_size)) {
		throw InvalidInputException("DELTA_BYTE_ARRAY value count is too large - corrupt file?");
	}
	result_buffer.resize(allocator, result_size);
	decoder->GetBatch<uint32_t>(result_buffer.ptr, value_count);
	decoder->Finalize();
	buffer.inc(buffer.len - decoder->BufferPtr().len);
}

void DeltaByteArrayDecoder::InitializePage() {
	auto &block = *reader.block;
	auto &allocator = reader.reader.allocator;
	idx_t prefix_count, suffix_count;
	auto &prefix_buffer = reader.encoding_buffers[0];
	auto &suffix_buffer = reader.encoding_buffers[1];
	ReadDbpData(allocator, block, prefix_buffer, prefix_count);
	ReadDbpData(allocator, block, suffix_buffer, suffix_count);
	if (prefix_count != suffix_count) {
		throw std::runtime_error("DELTA_BYTE_ARRAY - prefix and suffix counts are different - corrupt file?");
	}

	auto prefix_data = reinterpret_cast<uint32_t *>(prefix_buffer.ptr);
	auto suffix_data = reinterpret_cast<uint32_t *>(suffix_buffer.ptr);

	// Allocate the plain data buffer per page
	plain_data = make_shared_ptr<ResizeableBuffer>();

	if (prefix_count == 0) {
		plain_data->resize(allocator, 0);
		return;
	}

	// Decode DELTA_BYTE_ARRAY into plain Parquet page format
	// Plain format for BYTE_ARRAY: [4-byte length][data] repeated
	// Plain format for FIXED_LEN_BYTE_ARRAY: [data] repeated (no length prefix)
	auto &schema = reader.Schema();
	bool is_fixed_len = (schema.parquet_type == duckdb_parquet::Type::FIXED_LEN_BYTE_ARRAY);
	idx_t fixed_len = is_fixed_len ? schema.type_length : 0;

	// Calculate total buffer size and max value length in one pass
	idx_t total_size = 0;
	idx_t max_len = 0;
	for (idx_t i = 0; i < prefix_count; i++) {
		// the lengths are read from the file - add them as idx_t so the sum cannot wrap around
		idx_t len = idx_t(prefix_data[i]) + idx_t(suffix_data[i]);
		if (len >= NumericLimits<uint32_t>::Maximum()) {
			throw InvalidInputException("DELTA_BYTE_ARRAY - value length is out of range - corrupt file?");
		}
		if (is_fixed_len && len != fixed_len) {
			throw std::runtime_error(
			    "DELTA_BYTE_ARRAY on FIXED_LEN_BYTE_ARRAY: decoded length does not match type length");
		}
		if (!TryAddOperator::Operation<idx_t, idx_t, idx_t>(total_size, len, total_size)) {
			throw InvalidInputException("DELTA_BYTE_ARRAY - total length is too large - corrupt file?");
		}
		max_len = MaxValue(max_len, len);
	}

	if (!is_fixed_len) {
		if (!TryAddOperator::Operation<idx_t, idx_t, idx_t>(total_size, prefix_count * sizeof(uint32_t), total_size)) {
			throw InvalidInputException("DELTA_BYTE_ARRAY - total length is too large - corrupt file?");
		}
	}

	plain_data->resize(allocator, total_size);
	unsafe_vector<uint8_t> prev_value(max_len);
	idx_t prev_len = 0;

	auto output = plain_data->ptr;
	for (idx_t i = 0; i < prefix_count; i++) {
		idx_t prefix_len = prefix_data[i];
		idx_t suffix_len = suffix_data[i];
		idx_t value_len = prefix_len + suffix_len;

		if (prefix_len > prev_len) {
			throw std::runtime_error("DELTA_BYTE_ARRAY - prefix is out of range - corrupt file?");
		}

		if (!is_fixed_len) {
			Store<uint32_t>(static_cast<uint32_t>(value_len), output);
			output += sizeof(uint32_t);
		}

		memcpy(output, prev_value.data(), prefix_len);
		block.available(suffix_len);
		memcpy(output + prefix_len, block.ptr, suffix_len);
		block.inc(suffix_len);

		memcpy(prev_value.data(), output, value_len);
		prev_len = value_len;
		output += value_len;
	}
}

void DeltaByteArrayDecoder::Read(uint8_t *defines, idx_t read_count, Vector &result, idx_t result_offset) {
	reader.Plain(plain_data, defines, read_count, result_offset, result);
}

void DeltaByteArrayDecoder::Skip(uint8_t *defines, idx_t skip_count) {
	reader.PlainSkip(*plain_data, defines, skip_count);
}

} // namespace duckdb
