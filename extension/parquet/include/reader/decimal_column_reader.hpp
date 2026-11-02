//===----------------------------------------------------------------------===//
//                         DuckDB
//
// reader/decimal_column_reader.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/bignum_decimal.hpp"
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
			byte_len = plain_data.read<uint32_t>();
		}
		plain_data.available(byte_len);
		auto res = ParquetDecimalUtils::ReadDecimalValue<DUCKDB_PHYSICAL_TYPE>(const_data_ptr_cast(plain_data.ptr),
		                                                                       byte_len, reader.Schema());

		plain_data.inc(byte_len);
		return res;
	}

	template <bool CHECKED>
	static void PlainSkip(ByteBuffer &plain_data, ColumnReader &reader) {
		uint32_t decimal_len = FIXED_LENGTH ? reader.Schema().type_length : plain_data.read<uint32_t>();
		plain_data.inc(decimal_len);
	}

	static bool PlainAvailable(const ByteBuffer &plain_data, const idx_t count) {
		return true;
	}

	static idx_t PlainConstantSize() {
		return 0;
	}
};

//! Wide decimals are BIGNUM blobs, the two's complement bytes are converted into the vector being filled
class WideDecimalColumnReader;

template <bool FIXED_LENGTH>
struct WideDecimalParquetValueConversion {
	template <bool CHECKED>
	static string_t PlainRead(ByteBuffer &plain_data, ColumnReader &reader);

	template <bool CHECKED>
	static void PlainSkip(ByteBuffer &plain_data, ColumnReader &reader) {
		uint32_t decimal_len = FIXED_LENGTH ? reader.Schema().type_length : plain_data.read<uint32_t>();
		plain_data.inc(decimal_len);
	}

	static bool PlainAvailable(const ByteBuffer &plain_data, const idx_t count) {
		return false;
	}

	static idx_t PlainConstantSize() {
		return 0;
	}
};

//! INT32/INT64-backed Parquet decimals read into a wide decimal: a file that declares a width above 38 for a column
//! stored as an integer
template <class PARQUET_TYPE>
struct WideDecimalFromIntegerConversion {
	template <bool CHECKED>
	static string_t PlainRead(ByteBuffer &plain_data, ColumnReader &reader);

	template <bool CHECKED>
	static void PlainSkip(ByteBuffer &plain_data, ColumnReader &reader) {
		if (CHECKED) {
			plain_data.inc(sizeof(PARQUET_TYPE));
		} else {
			plain_data.unsafe_inc(sizeof(PARQUET_TYPE));
		}
	}

	static bool PlainAvailable(const ByteBuffer &plain_data, const idx_t count) {
		return plain_data.check_available(count * sizeof(PARQUET_TYPE));
	}

	static idx_t PlainConstantSize() {
		return sizeof(PARQUET_TYPE);
	}
};

class WideDecimalColumnReader : public ColumnReader {
public:
	static constexpr const PhysicalType TYPE = PhysicalType::VARCHAR;

public:
	WideDecimalColumnReader(const ParquetReader &reader, const ParquetColumnSchema &schema)
	    : ColumnReader(reader, schema) {
	}

	//! The vector the plain values are currently read into, so the conversion can allocate the blobs in it
	Vector *current_result = nullptr;

public:
	void Plain(ByteBuffer &plain_data, uint8_t *defines, idx_t num_values, idx_t result_offset,
	           Vector &result) override {
		current_result = &result;
		switch (Schema().parquet_type) {
		case duckdb_parquet::Type::INT32:
			PlainTemplated<string_t, WideDecimalFromIntegerConversion<int32_t>>(plain_data, defines, num_values,
			                                                                    result_offset, result);
			break;
		case duckdb_parquet::Type::INT64:
			PlainTemplated<string_t, WideDecimalFromIntegerConversion<int64_t>>(plain_data, defines, num_values,
			                                                                    result_offset, result);
			break;
		case duckdb_parquet::Type::FIXED_LEN_BYTE_ARRAY:
			PlainTemplated<string_t, WideDecimalParquetValueConversion<true>>(plain_data, defines, num_values,
			                                                                  result_offset, result);
			break;
		default:
			PlainTemplated<string_t, WideDecimalParquetValueConversion<false>>(plain_data, defines, num_values,
			                                                                   result_offset, result);
			break;
		}
		current_result = nullptr;
	}

	void PlainSkip(ByteBuffer &plain_data, uint8_t *defines, idx_t num_values) override {
		switch (Schema().parquet_type) {
		case duckdb_parquet::Type::INT32:
			PlainSkipTemplated<WideDecimalFromIntegerConversion<int32_t>>(plain_data, defines, num_values);
			break;
		case duckdb_parquet::Type::INT64:
			PlainSkipTemplated<WideDecimalFromIntegerConversion<int64_t>>(plain_data, defines, num_values);
			break;
		case duckdb_parquet::Type::FIXED_LEN_BYTE_ARRAY:
			PlainSkipTemplated<WideDecimalParquetValueConversion<true>>(plain_data, defines, num_values);
			break;
		default:
			PlainSkipTemplated<WideDecimalParquetValueConversion<false>>(plain_data, defines, num_values);
			break;
		}
	}
};

template <class PARQUET_TYPE>
template <bool CHECKED>
string_t WideDecimalFromIntegerConversion<PARQUET_TYPE>::PlainRead(ByteBuffer &plain_data, ColumnReader &reader) {
	auto value = CHECKED ? plain_data.read<PARQUET_TYPE>() : plain_data.unsafe_read<PARQUET_TYPE>();
	auto blob = BignumDecimal::FromInteger(hugeint_t(value));
	auto &wide_reader = reader.Cast<WideDecimalColumnReader>();
	D_ASSERT(wide_reader.current_result);
	return StringVector::AddStringOrBlob(*wide_reader.current_result, blob.data(), blob.size());
}

template <bool FIXED_LENGTH>
template <bool CHECKED>
string_t WideDecimalParquetValueConversion<FIXED_LENGTH>::PlainRead(ByteBuffer &plain_data, ColumnReader &reader) {
	idx_t byte_len;
	if (FIXED_LENGTH) {
		byte_len = reader.Schema().type_length;
	} else {
		byte_len = plain_data.read<uint32_t>();
	}
	plain_data.available(byte_len);
	auto blob = ParquetDecimalUtils::ReadWideDecimalValue(const_data_ptr_cast(plain_data.ptr), byte_len);
	plain_data.inc(byte_len);
	auto &wide_reader = reader.Cast<WideDecimalColumnReader>();
	D_ASSERT(wide_reader.current_result);
	return StringVector::AddStringOrBlob(*wide_reader.current_result, blob.data(), blob.size());
}

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
