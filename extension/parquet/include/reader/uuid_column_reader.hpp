//===----------------------------------------------------------------------===//
//                         DuckDB
//
// reader/uuid_column_reader.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "column_reader.hpp"
#include "templated_column_reader.hpp"
#include "parquet_reader.hpp"
#include "duckdb/common/types/uuid.hpp"

namespace duckdb {

struct UUIDValueConversion {
	static hugeint_t ReadParquetUUID(const_data_ptr_t input) {
		// Use the utility function from BaseUUID
		return BaseUUID::FromBlob(input);
	}

	template <bool CHECKED>
	static hugeint_t PlainRead(ByteBuffer &plain_data, ColumnReader &reader) {
		if (CHECKED) {
			plain_data.Available(sizeof(hugeint_t));
		}
		auto res = ReadParquetUUID(const_data_ptr_cast(plain_data.GetCurrentLoc()));
		plain_data.UnsafeInc(sizeof(hugeint_t));
		return res;
	}

	template <bool CHECKED>
	static void PlainSkip(ByteBuffer &plain_data, ColumnReader &reader) {
		if (CHECKED) {
			plain_data.Inc(sizeof(hugeint_t));
		} else {
			plain_data.UnsafeInc(sizeof(hugeint_t));
		}
	}

	static bool PlainAvailable(const ByteBuffer &plain_data, const idx_t count) {
		return plain_data.CheckAvailable(count * sizeof(hugeint_t));
	}

	static idx_t PlainConstantSize() {
		return 0;
	}
};

class UUIDColumnReader : public TemplatedColumnReader<hugeint_t, UUIDValueConversion> {
public:
	UUIDColumnReader(const ParquetReader &reader, const ParquetColumnSchema &schema)
	    : TemplatedColumnReader<hugeint_t, UUIDValueConversion>(reader, schema) {
	}
};

} // namespace duckdb
