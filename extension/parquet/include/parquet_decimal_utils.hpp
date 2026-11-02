//===----------------------------------------------------------------------===//
//                         DuckDB
//
// parquet_decimal_utils.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "column_reader.hpp"
#include "reader/templated_column_reader.hpp"

namespace duckdb {

class ParquetDecimalUtils {
public:
	template <class PHYSICAL_TYPE>
	static PHYSICAL_TYPE ReadDecimalValue(const_data_ptr_t pointer, idx_t size, const ParquetColumnSchema &) {
		PHYSICAL_TYPE res = 0;
		if (size == 0) {
			// empty byte array - value is zero, and there is no sign byte to read
			return res;
		}

		auto res_ptr = (uint8_t *)&res;
		bool positive = (*pointer & 0x80) == 0;

		// numbers are stored as two's complement so some muckery is required
		for (idx_t i = 0; i < MinValue<idx_t>(size, sizeof(PHYSICAL_TYPE)); i++) {
			auto byte = *(pointer + (size - i - 1));
			res_ptr[i] = positive ? byte : byte ^ 0xFF;
		}
		// Verify that there are only 0s here
		if (size > sizeof(PHYSICAL_TYPE)) {
			for (idx_t i = sizeof(PHYSICAL_TYPE); i < size; i++) {
				auto byte = *(pointer + (size - i - 1));
				if (!positive) {
					byte ^= 0xFF;
				}
				if (byte != 0) {
					throw InvalidInputException("Invalid decimal encoding in Parquet file");
				}
			}
		}
		if (!positive) {
			res += 1;
			return -res;
		}
		return res;
	}

	static unique_ptr<ColumnReader> CreateReader(const ParquetReader &reader, const ParquetColumnSchema &schema);

	//! Converts a big-endian two's complement Parquet decimal into the BIGNUM blob of a wide decimal
	static string ReadWideDecimalValue(const_data_ptr_t pointer, idx_t size);
	//! The number of bytes a FIXED_LEN_BYTE_ARRAY needs to hold decimals of the given width
	static idx_t WideDecimalByteLength(uint32_t width);
	//! Writes the BIGNUM blob of a wide decimal as a big-endian two's complement value of byte_length bytes
	static void WriteWideDecimal(const string_t &blob, idx_t byte_length, data_ptr_t result);
};

template <>
double ParquetDecimalUtils::ReadDecimalValue(const_data_ptr_t pointer, idx_t size, const ParquetColumnSchema &schema);

} // namespace duckdb
