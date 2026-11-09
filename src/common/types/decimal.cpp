#include "duckdb/common/types/decimal.hpp"

#include "duckdb/common/types/bignum_decimal.hpp"
#include "duckdb/common/types/cast_helpers.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/settings.hpp"

namespace duckdb {
constexpr uint32_t Decimal::MAX_WIDTH_DECIMAL;
constexpr uint32_t Decimal::DEFAULT_MAX_WIDTH;

uint32_t Decimal::MaxWidth(optional_ptr<ClientContext> context) {
	if (!context) {
		return NumericLimits<uint32_t>::Maximum();
	}
	const idx_t max_width = Settings::Get<MaxDecimalWidthSetting>(*context);
	return NumericCast<uint32_t>(
	    MinValue<idx_t>(MaxValue<idx_t>(max_width, MAX_WIDTH_INT128), NumericLimits<uint32_t>::Maximum()));
}

uint32_t Decimal::MaxResultWidth(optional_ptr<ClientContext> context, uint32_t max_input_width) {
	if (max_input_width > MAX_WIDTH_INT128) {
		// a wide input never existed before wide decimals, its result is not bound by the legacy rule
		return MaxValue<uint32_t>(MaxWidth(context), max_input_width);
	}
	if (context && context->legacy_decimal_result_types) {
		return MAX_WIDTH_INT128;
	}
	return MaxWidth(context);
}

uint32_t Decimal::MaxLiteralWidth(optional_ptr<ClientContext> context) {
	if (context && context->legacy_decimal_result_types) {
		return MAX_WIDTH_INT128;
	}
	return MaxWidth(context);
}

LegacyDecimalResultTypeScope::LegacyDecimalResultTypeScope(ClientContext &context, bool legacy)
    : context(context), previous(context.legacy_decimal_result_types) {
	context.legacy_decimal_result_types = legacy;
}

LegacyDecimalResultTypeScope::~LegacyDecimalResultTypeScope() {
	context.legacy_decimal_result_types = previous;
}

uint32_t Decimal::CheckedWidth(optional_ptr<ClientContext> context, idx_t width, const string &operation) {
	if (width > NumericLimits<uint32_t>::Maximum()) {
		throw OutOfRangeException("%s needs a DECIMAL of %llu digits, the width of a DECIMAL is at most %u", operation,
		                          width, NumericLimits<uint32_t>::Maximum());
	}
	const auto max_width = MaxWidth(context);
	if (width > max_width) {
		throw OutOfRangeException("%s needs a DECIMAL of %llu digits, the maximum DECIMAL width is %u (SET "
		                          "max_decimal_width to allow wider decimals)",
		                          operation, width, max_width);
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
	auto result = BignumDecimal::ToString(value, scale, context);
	if (width == scale && scale > 0) {
		// like the fixed-size decimals, a decimal without integer digits prints no leading zero: .100, -.100
		auto point = result.find('.');
		D_ASSERT(point != string::npos && point > 0 && result[point - 1] == '0');
		result.erase(point - 1, 1);
	}
	return result;
}

} // namespace duckdb
