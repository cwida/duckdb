#include "duckdb/common/types/decimal.hpp"

#include "duckdb/common/operator/decimal_cast_operators.hpp"
#include "duckdb/common/types/cast_helpers.hpp"
#include "duckdb/common/types/decimal_helper.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/settings.hpp"

namespace duckdb {
constexpr uint32_t Decimal::MAX_WIDTH_DECIMAL;
constexpr uint32_t Decimal::DEFAULT_MAX_WIDTH;

DecimalWidthLimit::DecimalWidthLimit(uint32_t width_p, string hint_p) : width(width_p), hint(std::move(hint_p)) {
	D_ASSERT(width >= Decimal::MAX_WIDTH_INT128);
}

DecimalWidthLimit DecimalWidthLimit::FromSetting(optional_ptr<ClientContext> context) {
	const string hint = "SET max_decimal_width to allow wider decimals";
	if (!context) {
		return DecimalWidthLimit(NumericLimits<uint32_t>::Maximum(), hint);
	}
	const idx_t max_width = Settings::Get<MaxDecimalWidthSetting>(*context);
	const auto width = NumericCast<uint32_t>(
	    MinValue<idx_t>(MaxValue<idx_t>(max_width, Decimal::MAX_WIDTH_INT128), NumericLimits<uint32_t>::Maximum()));
	return DecimalWidthLimit(width, hint);
}

uint32_t DecimalWidthLimit::ResultWidth(uint32_t max_input_width) const {
	return MaxValue<uint32_t>(width, max_input_width);
}

uint32_t DecimalWidthLimit::CheckedWidth(idx_t result_width, uint32_t max_input_width, const string &operation) const {
	if (result_width > NumericLimits<uint32_t>::Maximum()) {
		throw OutOfRangeException("%s needs a DECIMAL of %llu digits, the width of a DECIMAL is at most %u", operation,
		                          result_width, NumericLimits<uint32_t>::Maximum());
	}
	if (result_width > ResultWidth(max_input_width)) {
		throw OutOfRangeException("%s needs a DECIMAL of %llu digits, the maximum DECIMAL width is %u (%s)", operation,
		                          result_width, width, hint);
	}
	return NumericCast<uint32_t>(result_width);
}

template <class SIGNED>
string TemplatedDecimalToString(SIGNED value, uint32_t width, uint32_t scale) {
	auto len = DecimalToString::DecimalLength<SIGNED>(value, width, scale);
	auto data = make_unsafe_uniq_array_uninitialized<char>(UnsafeNumericCast<size_t>(len + 1));
	DecimalToString::FormatDecimal<SIGNED>(value, width, scale, data.get(), UnsafeNumericCast<idx_t>(len));
	return string(data.get(), UnsafeNumericCast<uint32_t>(len));
}

string Decimal::ToString(int16_t value, uint32_t width, uint32_t scale) {
	return TemplatedDecimalToString<int16_t>(value, width, scale);
}

string Decimal::ToString(int32_t value, uint32_t width, uint32_t scale) {
	return TemplatedDecimalToString<int32_t>(value, width, scale);
}

string Decimal::ToString(int64_t value, uint32_t width, uint32_t scale) {
	return TemplatedDecimalToString<int64_t>(value, width, scale);
}

string Decimal::ToString(hugeint_t value, uint32_t width, uint32_t scale) {
	auto len = DecimalToString::DecimalLength(value, width, scale);
	auto data = make_unsafe_uniq_array_uninitialized<char>(UnsafeNumericCast<size_t>(len + 1));
	DecimalToString::FormatDecimal(value, width, scale, data.get(), UnsafeNumericCast<idx_t>(len));
	return string(data.get(), UnsafeNumericCast<uint32_t>(len));
}

string Decimal::ToString(bignum_t value, uint32_t width, uint32_t scale, optional_ptr<ClientContext> context) {
	return DecimalHelper<bignum_t>::ToString(value, width, scale, context);
}

//===--------------------------------------------------------------------===//
// DecimalHelper of the fixed-size tiers: the conversions that run through the cast operators
//===--------------------------------------------------------------------===//
template <class T, class POWERS_OF_TEN_CLASS>
bool FixedSizeDecimalHelper<T, POWERS_OF_TEN_CLASS>::TryParse(const char *data, idx_t size, uint32_t width,
                                                              uint32_t scale, T &result, char decimal_separator,
                                                              optional_ptr<ClientContext> context) {
	if (decimal_separator == ',') {
		return TryDecimalStringCast<T, ','>(data, size, result, width, scale);
	}
	return TryDecimalStringCast<T, '.'>(data, size, result, width, scale);
}

template <class T, class POWERS_OF_TEN_CLASS>
bool FixedSizeDecimalHelper<T, POWERS_OF_TEN_CLASS>::TryToFloating(T value, uint32_t scale, float &result,
                                                                   optional_ptr<ClientContext> context) {
	CastParameters parameters;
	return TryCastFromDecimal::Operation<T, float>(value, result, parameters, MAX_WIDTH, scale);
}

template <class T, class POWERS_OF_TEN_CLASS>
bool FixedSizeDecimalHelper<T, POWERS_OF_TEN_CLASS>::TryToFloating(T value, uint32_t scale, double &result,
                                                                   optional_ptr<ClientContext> context) {
	CastParameters parameters;
	return TryCastFromDecimal::Operation<T, double>(value, result, parameters, MAX_WIDTH, scale);
}

template <class T, class POWERS_OF_TEN_CLASS>
bool FixedSizeDecimalHelper<T, POWERS_OF_TEN_CLASS>::TryFromDouble(double value, uint32_t width, uint32_t scale,
                                                                   T &result, optional_ptr<ClientContext> context) {
	CastParameters parameters;
	return TryCastToDecimal::Operation<double, T>(value, result, parameters, width, scale);
}

template struct FixedSizeDecimalHelper<int16_t, NumericHelper>;
template struct FixedSizeDecimalHelper<int32_t, NumericHelper>;
template struct FixedSizeDecimalHelper<int64_t, NumericHelper>;
template struct FixedSizeDecimalHelper<hugeint_t, Hugeint>;

} // namespace duckdb
