//===----------------------------------------------------------------------===//
//                         DuckDB
//
// reader/templated_column_reader.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "column_reader.hpp"
#include "duckdb/common/helper.hpp"

namespace duckdb {

template <class VALUE_TYPE>
struct TemplatedParquetValueConversion {
	template <bool CHECKED>
	static VALUE_TYPE PlainRead(ByteBuffer &plain_data, ColumnReader &reader) {
		if (CHECKED) {
			return plain_data.Read<VALUE_TYPE>();
		} else {
			return plain_data.UnsafeRead<VALUE_TYPE>();
		}
	}

	template <bool CHECKED>
	static void PlainSkip(ByteBuffer &plain_data, ColumnReader &reader) {
		if (CHECKED) {
			plain_data.Inc(sizeof(VALUE_TYPE));
		} else {
			plain_data.UnsafeInc(sizeof(VALUE_TYPE));
		}
	}

	static bool PlainAvailable(const ByteBuffer &plain_data, const idx_t count) {
		return plain_data.CheckAvailable(count * sizeof(VALUE_TYPE));
	}

	static idx_t PlainConstantSize() {
		return sizeof(VALUE_TYPE);
	}
};

template <class VALUE_TYPE, class VALUE_CONVERSION>
class TemplatedColumnReader : public ColumnReader {
public:
	static constexpr const PhysicalType TYPE = PhysicalType::INVALID;

public:
	TemplatedColumnReader(const ParquetReader &reader, const ParquetColumnSchema &schema)
	    : ColumnReader(reader, schema) {
	}

	shared_ptr<ResizeableBuffer> dict;

public:
	void AllocateDict(idx_t size) {
		if (!dict) {
			dict = make_shared_ptr<ResizeableBuffer>(GetAllocator(), size);
		} else {
			dict->Resize(GetAllocator(), size);
		}
	}

	void Plain(ByteBuffer &plain_data, uint8_t *defines, uint64_t num_values, idx_t result_offset,
	           Vector &result) override {
		PlainTemplated<VALUE_TYPE, VALUE_CONVERSION>(plain_data, defines, num_values, result_offset, result);
	}

	void PlainSkip(ByteBuffer &plain_data, uint8_t *defines, idx_t num_values) override {
		PlainSkipTemplated<VALUE_CONVERSION>(plain_data, defines, num_values);
	}

	bool SupportsDirectFilter() const override {
		return true;
	}
};

template <class PARQUET_PHYSICAL_TYPE, class DUCKDB_PHYSICAL_TYPE,
          DUCKDB_PHYSICAL_TYPE (*FUNC)(const PARQUET_PHYSICAL_TYPE &input)>
struct CallbackParquetValueConversion {
	template <bool CHECKED>
	static DUCKDB_PHYSICAL_TYPE PlainRead(ByteBuffer &plain_data, ColumnReader &reader) {
		if (CHECKED) {
			return FUNC(plain_data.Read<PARQUET_PHYSICAL_TYPE>());
		} else {
			return FUNC(plain_data.UnsafeRead<PARQUET_PHYSICAL_TYPE>());
		}
	}

	template <bool CHECKED>
	static void PlainSkip(ByteBuffer &plain_data, ColumnReader &reader) {
		if (CHECKED) {
			plain_data.Inc(sizeof(PARQUET_PHYSICAL_TYPE));
		} else {
			plain_data.UnsafeInc(sizeof(PARQUET_PHYSICAL_TYPE));
		}
	}

	static bool PlainAvailable(const ByteBuffer &plain_data, const idx_t count) {
		return plain_data.CheckAvailable(count * sizeof(PARQUET_PHYSICAL_TYPE));
	}

	static idx_t PlainConstantSize() {
		return 0;
	}
};

} // namespace duckdb
