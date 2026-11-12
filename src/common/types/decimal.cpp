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

//! The scope of the current thread: a trivially destructible thread_local, a thread registers no destructor for it
static optional_ptr<const FixedDecimalWidthScope> &CurrentFixedDecimalWidth() {
	static thread_local optional_ptr<const FixedDecimalWidthScope> current;
	return current;
}

FixedDecimalWidthScope::FixedDecimalWidthScope(uint32_t width_p, string reason_p)
    : width(width_p), reason(std::move(reason_p)), previous(CurrentFixedDecimalWidth()) {
	D_ASSERT(width >= Decimal::MAX_WIDTH_INT128);
	CurrentFixedDecimalWidth() = this;
}

FixedDecimalWidthScope::~FixedDecimalWidthScope() {
	D_ASSERT(CurrentFixedDecimalWidth().get() == this);
	CurrentFixedDecimalWidth() = previous;
}

optional_ptr<const FixedDecimalWidthScope> FixedDecimalWidthScope::Current() {
	return CurrentFixedDecimalWidth();
}

uint32_t Decimal::MaxWidth(optional_ptr<ClientContext> context) {
	auto fixed = FixedDecimalWidthScope::Current();
	if (fixed) {
		return fixed->width;
	}
	if (!context) {
		return NumericLimits<uint32_t>::Maximum();
	}
	const idx_t max_width = Settings::Get<MaxDecimalWidthSetting>(*context);
	return NumericCast<uint32_t>(
	    MinValue<idx_t>(MaxValue<idx_t>(max_width, MAX_WIDTH_INT128), NumericLimits<uint32_t>::Maximum()));
}

string Decimal::MaxWidthHint() {
	auto fixed = FixedDecimalWidthScope::Current();
	if (fixed) {
		return fixed->reason;
	}
	return "SET max_decimal_width to allow wider decimals";
}

uint32_t Decimal::MaxResultWidth(optional_ptr<ClientContext> context, uint32_t max_input_width) {
	return MaxValue<uint32_t>(MaxWidth(context), max_input_width);
}

uint32_t Decimal::CheckedWidth(optional_ptr<ClientContext> context, idx_t width, uint32_t max_input_width,
                               const string &operation) {
	if (width > NumericLimits<uint32_t>::Maximum()) {
		throw OutOfRangeException("%s needs a DECIMAL of %llu digits, the width of a DECIMAL is at most %u", operation,
		                          width, NumericLimits<uint32_t>::Maximum());
	}
	if (width > MaxResultWidth(context, max_input_width)) {
		throw OutOfRangeException("%s needs a DECIMAL of %llu digits, the maximum DECIMAL width is %u (%s)", operation,
		                          width, MaxWidth(context), MaxWidthHint());
	}
	return NumericCast<uint32_t>(width);
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
