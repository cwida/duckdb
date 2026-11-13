//===----------------------------------------------------------------------===//
//                         DuckDB
//
// reader/decimal_column_reader.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include <stdint.h>

#include "column_reader.hpp"
#include "reader/templated_column_reader.hpp"
#include "parquet_reader.hpp"
#include "parquet_decimal_utils.hpp"
#include "duckdb/common/typedefs.hpp"
#include "parquet_column_schema.hpp"
#include "resizable_buffer.hpp"

namespace duckdb {
class ParquetReader;

template <class DUCKDB_PHYSICAL_TYPE, bool FIXED_LENGTH>
struct DecimalParquetValueConversion {
	template <bool CHECKED>
	static DUCKDB_PHYSICAL_TYPE PlainRead(ByteBuffer &plain_data, ColumnReader &reader) {
		idx_t byte_len;
		if (FIXED_LENGTH) {
			byte_len = reader.Schema().type_length;
		} else {
			byte_len = plain_data.Read<uint32_t>();
		}
		plain_data.Available(byte_len);
		auto res = ParquetDecimalUtils::ReadDecimalValue<DUCKDB_PHYSICAL_TYPE>(
		    const_data_ptr_cast(plain_data.GetCurrentLoc()), byte_len, reader.Schema());

		plain_data.Inc(byte_len);
		return res;
	}

	template <bool CHECKED>
	static void PlainSkip(ByteBuffer &plain_data, ColumnReader &reader) {
		uint32_t decimal_len = FIXED_LENGTH ? reader.Schema().type_length : plain_data.Read<uint32_t>();
		plain_data.Inc(decimal_len);
	}

	static bool PlainAvailable(const ByteBuffer &plain_data, const idx_t count) {
		return true;
	}

	static idx_t PlainConstantSize() {
		return 0;
	}
};

template <class DUCKDB_PHYSICAL_TYPE, bool FIXED_LENGTH>
class DecimalColumnReader
    : public TemplatedColumnReader<DUCKDB_PHYSICAL_TYPE,
                                   DecimalParquetValueConversion<DUCKDB_PHYSICAL_TYPE, FIXED_LENGTH>> {
	using BaseType =
	    TemplatedColumnReader<DUCKDB_PHYSICAL_TYPE, DecimalParquetValueConversion<DUCKDB_PHYSICAL_TYPE, FIXED_LENGTH>>;

public:
	DecimalColumnReader(const ParquetReader &reader, const ParquetColumnSchema &schema)
	    : TemplatedColumnReader<DUCKDB_PHYSICAL_TYPE,
	                            DecimalParquetValueConversion<DUCKDB_PHYSICAL_TYPE, FIXED_LENGTH>>(reader, schema) {
	}
};

} // namespace duckdb
