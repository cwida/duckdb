#include "duckdb/common/types/decimal.hpp"

#include "duckdb/common/types/bignum_decimal.hpp"
#include "duckdb/common/types/cast_helpers.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {
constexpr uint32_t Decimal::MAX_WIDTH_DECIMAL;
uint32_t Decimal::MaxResultWidth(optional_ptr<ClientContext> context, uint32_t max_input_width) {
	if (max_input_width > MAX_WIDTH_INT128) {
		// a wide input never existed before wide decimals, its result is not bound by the legacy rule
		return MAX_WIDTH_DECIMAL;
	}
	if (context && context->legacy_decimal_result_types) {
		return MAX_WIDTH_INT128;
	}
	return MAX_WIDTH_DECIMAL;
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

string Decimal::ToString(string_t value, uint32_t width, uint32_t scale) {
	auto result = BignumDecimal::ToString(value, scale);
	if (width == scale && scale > 0) {
		// like the fixed-size decimals, a decimal without integer digits prints no leading zero: .100, -.100
		auto point = result.find('.');
		D_ASSERT(point != string::npos && point > 0 && result[point - 1] == '0');
		result.erase(point - 1, 1);
	}
	return result;
}

} // namespace duckdb
