//===----------------------------------------------------------------------===//
//                         DuckDB
//
// reader/boolean_column_reader.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "column_reader.hpp"
#include "parquet_arena.h"
#include "reader/templated_column_reader.hpp"

namespace duckdb {

struct BooleanParquetValueConversion;

class BooleanColumnReader : public TemplatedColumnReader<bool, BooleanParquetValueConversion> {
public:
	static constexpr const PhysicalType TYPE = PhysicalType::BOOL;

public:
	BooleanColumnReader(const ParquetReader &reader, const ParquetColumnSchema &schema)
	    : TemplatedColumnReader<bool, BooleanParquetValueConversion>(reader, schema), byte_pos(0) {
	}

	uint8_t byte_pos;

	void InitializeRead(idx_t row_group_idx_p, idx_t row_group_num_rows, const duckdb_parquet::ColumnChunkList &columns,
	                    TProtocol &protocol_p) override {
		byte_pos = 0;
		TemplatedColumnReader<bool, BooleanParquetValueConversion>::InitializeRead(row_group_idx_p, row_group_num_rows,
		                                                                           columns, protocol_p);
	}

	void ResetPage() override {
		byte_pos = 0;
	}
};

struct BooleanParquetValueConversion {
	template <bool CHECKED>
	static bool PlainRead(ByteBuffer &plain_data, ColumnReader &reader) {
		auto &byte_pos = reader.Cast<BooleanColumnReader>().byte_pos;
		bool ret = (*plain_data.ptr >> byte_pos) & 1;
		if (++byte_pos == 8) {
			byte_pos = 0;
			if (CHECKED) {
				plain_data.inc(1);
			} else {
				plain_data.unsafe_inc(1);
			}
		}
		return ret;
	}

	template <bool CHECKED>
	static void PlainSkip(ByteBuffer &plain_data, ColumnReader &reader) {
		PlainRead<CHECKED>(plain_data, reader);
	}

	static bool PlainAvailable(const ByteBuffer &plain_data, const idx_t count) {
		return plain_data.check_available((count + 7) / 8);
	}

	static idx_t PlainConstantSize() {
		return 0;
	}
};

} // namespace duckdb
