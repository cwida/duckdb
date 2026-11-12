//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/decimal.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types.hpp"

namespace duckdb {

template <class PHYSICAL_TYPE>
struct DecimalWidth {};

template <>
struct DecimalWidth<int16_t> {
	static constexpr uint32_t max = 4;
};

template <>
struct DecimalWidth<int32_t> {
	static constexpr uint32_t max = 9;
};

template <>
struct DecimalWidth<int64_t> {
	static constexpr uint32_t max = 18;
};

template <>
struct DecimalWidth<hugeint_t> {
	static constexpr uint32_t max = 38;
};

//! The Decimal class is a static class that holds helper functions for the Decimal type
class Decimal {
public:
	static constexpr uint32_t MAX_WIDTH_INT16 = DecimalWidth<int16_t>::max;
	static constexpr uint32_t MAX_WIDTH_INT32 = DecimalWidth<int32_t>::max;
	static constexpr uint32_t MAX_WIDTH_INT64 = DecimalWidth<int64_t>::max;
	static constexpr uint32_t MAX_WIDTH_INT128 = DecimalWidth<hugeint_t>::max;
	static constexpr uint32_t MAX_WIDTH_DECIMAL = MAX_WIDTH_INT128;

public:
	//! Whether width/scale form a valid DECIMAL type: width in [1, MAX_WIDTH_DECIMAL] and scale not exceeding width.
	static bool IsValidWidthScale(uint32_t width, uint32_t scale) {
		return width >= 1 && width <= MAX_WIDTH_DECIMAL && scale <= width;
	}
	static string ToString(int16_t value, uint32_t width, uint32_t scale);
	static string ToString(int32_t value, uint32_t width, uint32_t scale);
	static string ToString(int64_t value, uint32_t width, uint32_t scale);
	static string ToString(hugeint_t value, uint32_t width, uint32_t scale);
};
} // namespace duckdb
