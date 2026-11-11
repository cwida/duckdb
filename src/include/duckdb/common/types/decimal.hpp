//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/decimal.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/bignum.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/storage/storage_info.hpp"

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

class ClientContext;

//! How the digits a decimal drops when it is rounded, rescaled or parsed are rounded
enum class DecimalRounding : uint8_t { TRUNCATE, FLOOR, CEIL, HALF_AWAY_FROM_ZERO, HALF_TO_EVEN };

//! The operations on the unscaled value of a DECIMAL stored as T: specialised for the fixed-size tiers int16_t,
//! int32_t, int64_t and hugeint_t in decimal_helper.hpp and for the bignum_t tier (above 38 digits) in
//! bignum_decimal.hpp, with the same interface, so that code is written once for every tier
template <class T>
struct DecimalHelper;

//! The Decimal class is a static class that holds helper functions for the Decimal type
class Decimal {
public:
	static constexpr uint32_t MAX_WIDTH_INT16 = DecimalWidth<int16_t>::max;
	static constexpr uint32_t MAX_WIDTH_INT32 = DecimalWidth<int32_t>::max;
	static constexpr uint32_t MAX_WIDTH_INT64 = DecimalWidth<int64_t>::max;
	//! MAX_WIDTH_INT128 is also the widest decimal that is stored in a fixed-size integer: wider decimals store their
	//! unscaled value as a BIGNUM blob and are bounded only by the uint32_t range of the width
	static constexpr uint32_t MAX_WIDTH_INT128 = DecimalWidth<hugeint_t>::max;
	//! The widest fixed-size decimal tier, kept for extensions; wider decimals are the BIGNUM-backed tier
	static constexpr uint32_t MAX_WIDTH_DECIMAL = MAX_WIDTH_INT128;
	//! The default of the max_decimal_width setting: the widest DECIMAL a session binds unless the setting is raised
	static constexpr uint32_t DEFAULT_MAX_WIDTH = 1000000;
	//! The storage version from which DECIMAL columns wider than MAX_WIDTH_INT128 digits can be stored: the engines of
	//! the versions before it read a DECIMAL width as a single byte and bind decimal arithmetic within 38 digits
	static constexpr StorageVersion WIDE_VERSION_ADDED = StorageVersion::V2_1_0;

public:
	//! Whether width/scale form a valid DECIMAL type: a width of at least 1 and a scale not exceeding the width
	static bool IsValidWidthScale(uint32_t width, uint32_t scale) {
		return width >= 1 && scale <= width;
	}
	//! Whether a decimal of this width is stored as a BIGNUM blob instead of a fixed-size integer
	static bool IsWide(uint32_t width) {
		return width > MAX_WIDTH_INT128;
	}
	//! The widest DECIMAL the session binds: the max_decimal_width setting (at least MAX_WIDTH_INT128, so the
	//! fixed-size tiers are never bounded by it), or the uint32_t range of the width without a context
	static uint32_t MaxWidth(optional_ptr<ClientContext> context);
	//! The widest result of combining decimals, given the widest DECIMAL among the inputs: MaxWidth, or the input
	//! width when that is wider
	static uint32_t MaxResultWidth(optional_ptr<ClientContext> context, uint32_t max_input_width);
	//! Narrows a computed result width, throws when the operation needs a DECIMAL beyond MaxResultWidth: the setting
	//! bounds the types an operation constructs, never the types of the inputs it already has
	static uint32_t CheckedWidth(optional_ptr<ClientContext> context, idx_t width, uint32_t max_input_width,
	                             const string &operation);
	static string ToString(int16_t value, uint32_t width, uint32_t scale);
	static string ToString(int32_t value, uint32_t width, uint32_t scale);
	static string ToString(int64_t value, uint32_t width, uint32_t scale);
	static string ToString(hugeint_t value, uint32_t width, uint32_t scale);
	//! Formats the unscaled value of a wide decimal (a BIGNUM blob), a long conversion checks for interrupts
	static string ToString(bignum_t value, uint32_t width, uint32_t scale,
	                       optional_ptr<ClientContext> context = nullptr);
};
} // namespace duckdb
