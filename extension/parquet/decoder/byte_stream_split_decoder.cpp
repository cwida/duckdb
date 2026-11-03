#include "decoder/byte_stream_split_decoder.hpp"

#include <stdexcept>

#include "column_reader.hpp"
#include "parquet_reader.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/shared_ptr_ipp.hpp"
#include "parquet_column_schema.hpp"
#include "parquet_types.h"
#include "resizable_buffer.hpp"

namespace duckdb {
class Vector;

ByteStreamSplitDecoder::ByteStreamSplitDecoder(ColumnReader &reader)
    : reader(reader), decoded_data_buffer(reader.encoding_buffers[0]) {
}

void ByteStreamSplitDecoder::InitializePage() {
	auto &block = reader.block;
	bss_decoder = make_uniq<BssDecoder>(block->ptr, block->len);
	block->inc(block->len);
}

void ByteStreamSplitDecoder::Read(uint8_t *defines, idx_t read_count, Vector &result, idx_t result_offset) {
	idx_t valid_count = reader.GetValidCount(defines, read_count, result_offset);

	auto &allocator = reader.reader.allocator;
	decoded_data_buffer.reset();
	switch (reader.Schema().parquet_type) {
	case duckdb_parquet::Type::FLOAT:
		decoded_data_buffer.resize(allocator, sizeof(float) * valid_count);
		bss_decoder->GetBatch<float>(decoded_data_buffer.ptr, valid_count);
		break;
	case duckdb_parquet::Type::DOUBLE:
		decoded_data_buffer.resize(allocator, sizeof(double) * valid_count);
		bss_decoder->GetBatch<double>(decoded_data_buffer.ptr, valid_count);
		break;
	case duckdb_parquet::Type::INT32:
		decoded_data_buffer.resize(allocator, sizeof(int32_t) * valid_count);
		bss_decoder->GetBatch<int32_t>(decoded_data_buffer.ptr, valid_count);
		break;
	case duckdb_parquet::Type::INT64:
		decoded_data_buffer.resize(allocator, sizeof(int64_t) * valid_count);
		bss_decoder->GetBatch<int64_t>(decoded_data_buffer.ptr, valid_count);
		break;
	default:
		throw std::runtime_error("BYTE_STREAM_SPLIT encoding is only supported for FLOAT, DOUBLE, INT32 or INT64 data");
	}

	reader.Plain(decoded_data_buffer, defines, read_count, result_offset, result);
}

void ByteStreamSplitDecoder::Skip(uint8_t *defines, idx_t skip_count) {
	idx_t valid_count = reader.GetValidCount(defines, skip_count);
	switch (reader.Schema().parquet_type) {
	case duckdb_parquet::Type::FLOAT:
		bss_decoder->Skip<float>(valid_count);
		break;
	case duckdb_parquet::Type::DOUBLE:
		bss_decoder->Skip<double>(valid_count);
		break;
	case duckdb_parquet::Type::INT32:
		bss_decoder->Skip<int32_t>(valid_count);
		break;
	case duckdb_parquet::Type::INT64:
		bss_decoder->Skip<int64_t>(valid_count);
		break;
	default:
		throw std::runtime_error("BYTE_STREAM_SPLIT encoding is only supported for FLOAT, DOUBLE, INT32 or INT64 data");
	}
}

} // namespace duckdb
