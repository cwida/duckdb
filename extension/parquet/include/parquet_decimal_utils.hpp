//===----------------------------------------------------------------------===//
//                         DuckDB
//
// parquet_decimal_utils.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "column_reader.hpp"

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
		bool invalid = false;
		// Verify that there are only 0s here
		for (idx_t i = sizeof(PHYSICAL_TYPE); i < size; i++) {
			auto byte = *(pointer + (size - i - 1));
			if (!positive) {
				byte ^= 0xFF;
			}
			invalid |= byte != 0;
		}
		invalid |= res < 0;
		if (invalid) {
			ThrowInvalidDecimalEncoding();
		}
		if (!positive) {
			// Negate before subtracting one to keep the minimum signed value representable.
			return -res - 1;
		}
		return res;
	}

	static unique_ptr<ColumnReader> CreateReader(const ParquetReader &reader, const ParquetColumnSchema &schema);

private:
	[[noreturn]] static void ThrowInvalidDecimalEncoding();
};

template <>
double ParquetDecimalUtils::ReadDecimalValue(const_data_ptr_t pointer, idx_t size, const ParquetColumnSchema &schema);

} // namespace duckdb
