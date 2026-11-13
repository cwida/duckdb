#include "decoder/byte_stream_split_decoder.hpp"

#include <stdexcept>

#include "column_reader.hpp"
#include "parquet_reader.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/shared_ptr_ipp.hpp"
#include "parquet_column_schema.hpp"
#include "parquet_types.h"
#include "resizable_buffer.hpp"

namespace duckdb {
class Vector;

ByteStreamSplitDecoder::ByteStreamSplitDecoder(ColumnReader &reader)
    : reader(reader), decoded_data_buffer(reader.encoding_buffers[0]) {
}

static idx_t GetValueWidth(const ParquetColumnSchema &schema) {
	switch (schema.parquet_type) {
	case duckdb_parquet::Type::FLOAT:
	case duckdb_parquet::Type::INT32:
		return sizeof(int32_t);
	case duckdb_parquet::Type::DOUBLE:
	case duckdb_parquet::Type::INT64:
		return sizeof(int64_t);
	case duckdb_parquet::Type::FIXED_LEN_BYTE_ARRAY:
		return schema.type_length;
	default:
		throw std::runtime_error("BYTE_STREAM_SPLIT encoding is only supported for FLOAT, DOUBLE, INT32, INT64 or "
		                         "FIXED_LEN_BYTE_ARRAY data");
	}
}

void ByteStreamSplitDecoder::InitializePage() {
	auto &block = reader.block;
	block_offset = block->GetOffset();
	idx_t bss_len;
	auto loc = block->ConsumeRemaining(bss_len);
	bss_decoder = make_uniq<BssDecoder>(loc, NumericCast<uint32_t>(bss_len), GetValueWidth(reader.Schema()));
}

void ByteStreamSplitDecoder::Rebase() {
	ColumnReader::RebaseDecoder(bss_decoder, *reader.block, block_offset);
}

void ByteStreamSplitDecoder::Read(uint8_t *defines, idx_t read_count, Vector &result, idx_t result_offset) {
	idx_t valid_count = reader.GetValidCount(defines, read_count, result_offset);
	auto &allocator = reader.GetAllocator();
	auto value_width = GetValueWidth(reader.Schema());

	if (reader.Schema().parquet_type == duckdb_parquet::Type::FIXED_LEN_BYTE_ARRAY) {
		// string readers can reference the decoded values, so the buffer must be able to outlive this call
		auto plain_data = make_shared_ptr<ResizeableBuffer>(allocator, value_width * valid_count);
		bss_decoder->GetBatch(plain_data->GetCurrentLoc(), NumericCast<uint32_t>(valid_count));
		reader.Plain(plain_data, defines, read_count, result_offset, result);
		return;
	}

	decoded_data_buffer.Resize(allocator, value_width * valid_count);
	bss_decoder->GetBatch(decoded_data_buffer.GetCurrentLoc(), NumericCast<uint32_t>(valid_count));
	reader.Plain(decoded_data_buffer, defines, read_count, result_offset, result);
}

void ByteStreamSplitDecoder::Skip(uint8_t *defines, idx_t skip_count) {
	idx_t valid_count = reader.GetValidCount(defines, skip_count);
	bss_decoder->Skip(NumericCast<uint32_t>(valid_count));
}

} // namespace duckdb
