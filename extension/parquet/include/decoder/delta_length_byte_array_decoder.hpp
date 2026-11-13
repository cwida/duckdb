//===----------------------------------------------------------------------===//
//                         DuckDB
//
// decoder/delta_length_byte_array_decoder.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include <stdint.h>

#include "duckdb.hpp"
#include "parquet_dbp_decoder.hpp"
#include "resizable_buffer.hpp"
#include "duckdb/common/shared_ptr_ipp.hpp"
#include "duckdb/common/typedefs.hpp"

namespace duckdb {
class ColumnReader;
class ResizeableBuffer;
class Vector;

class DeltaLengthByteArrayDecoder {
public:
	explicit DeltaLengthByteArrayDecoder(ColumnReader &reader);

public:
	void InitializePage();

	void Read(shared_ptr<ResizeableBuffer> &block, uint8_t *defines, idx_t read_count, Vector &result,
	          idx_t result_offset);
	void Skip(uint8_t *defines, idx_t skip_count);

private:
	template <bool HAS_DEFINES>
	void ReadInternal(shared_ptr<ResizeableBuffer> &block, uint8_t *defines, idx_t read_count, Vector &result,
	                  idx_t result_offset);
	template <bool HAS_DEFINES>
	void SkipInternal(uint8_t *defines, idx_t skip_count);
	void DecodeToPlain(ResizeableBuffer &block, idx_t total_string_length);

private:
	ColumnReader &reader;
	ResizeableBuffer &length_buffer;
	idx_t byte_array_count = 0;
	idx_t length_idx;
	//! Readers of other VARCHAR-backed types (wide DECIMAL) are not string readers: their pages are decoded into the
	//! plain BYTE_ARRAY layout and read through the reader's own Plain path
	bool decode_to_plain = false;
	shared_ptr<ResizeableBuffer> plain_data;
};

} // namespace duckdb
