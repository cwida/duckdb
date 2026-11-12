//===----------------------------------------------------------------------===//
//                         DuckDB
//
// decoder/rle_decoder.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include <stdint.h>

#include "duckdb.hpp"
#include "parquet_rle_bp_decoder.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/unique_ptr.hpp"

namespace duckdb {
class ColumnReader;
class ResizeableBuffer;
class Vector;

class RLEDecoder {
public:
	explicit RLEDecoder(ColumnReader &reader);

public:
	void InitializePage();
	void Rebase();
	void Read(uint8_t *defines, idx_t read_count, Vector &result, idx_t result_offset);
	void Skip(uint8_t *defines, idx_t skip_count);

private:
	ColumnReader &reader;
	ResizeableBuffer &decoded_data_buffer;
	unique_ptr<RleBpDecoder> rle_decoder;
	idx_t block_offset = 0;
};

} // namespace duckdb
