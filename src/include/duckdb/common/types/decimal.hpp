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
	//! The widest result of combining decimals, given the widest DECIMAL among the inputs. The result grows as far as
	//! the uint32_t width reaches, except under the legacy rule of databases written before wide decimals (see
	//! ClientContext::legacy_decimal_result_types), where fixed-size inputs stay within MAX_WIDTH_INT128.
	static uint32_t MaxResultWidth(optional_ptr<ClientContext> context, uint32_t max_input_width);
	//! The widest DECIMAL a numeric literal binds to, wider literals bind to DOUBLE: every width the uint32_t holds, or
	//! MAX_WIDTH_INT128 under the legacy rule, as every literal above 38 digits was a DOUBLE before wide decimals.
	static uint32_t MaxLiteralWidth(optional_ptr<ClientContext> context);
	//! Narrows a computed width to the uint32_t range of a DECIMAL width, throws when the operation needs more
	static uint32_t CheckedWidth(idx_t width, const char *operation);
	static string ToString(int16_t value, uint32_t width, uint32_t scale);
	static string ToString(int32_t value, uint32_t width, uint32_t scale);
	static string ToString(int64_t value, uint32_t width, uint32_t scale);
	static string ToString(hugeint_t value, uint32_t width, uint32_t scale);
	//! Formats the unscaled value of a wide decimal (a BIGNUM blob)
	static string ToString(bignum_t value, uint32_t width, uint32_t scale);
};

//! Binds decimal results as the engines before wide decimals did while in scope, when legacy is set: arithmetic and
//! type combination stay within MAX_WIDTH_INT128 and a literal above 38 digits is a DOUBLE (see
//! ClientContext::legacy_decimal_result_types)
class LegacyDecimalResultTypeScope {
public:
	LegacyDecimalResultTypeScope(ClientContext &context, bool legacy);
	~LegacyDecimalResultTypeScope();

private:
	ClientContext &context;
	bool previous;
};
} // namespace duckdb
