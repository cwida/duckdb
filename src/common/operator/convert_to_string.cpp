#include "duckdb/common/operator/convert_to_string.hpp"
#include "duckdb/common/operator/string_cast.hpp"
#include "duckdb/common/types/string_heap.hpp"
#include "duckdb/common/types/bignum.hpp"

namespace duckdb {

template <class T>
string StandardStringCast(T input) {
	StringHeap heap;
	return StringCast::Operation(input, heap).GetString();
}

template <>
string ConvertToString::Operation(bool input) {
	return StandardStringCast(input);
}
template <>
string ConvertToString::Operation(int8_t input) {
	return StandardStringCast(input);
}
template <>
string ConvertToString::Operation(int16_t input) {
	return StandardStringCast(input);
}
template <>
string ConvertToString::Operation(int32_t input) {
	return StandardStringCast(input);
}
template <>
string ConvertToString::Operation(int64_t input) {
	return StandardStringCast(input);
}
template <>
string ConvertToString::Operation(uint8_t input) {
	return StandardStringCast(input);
}
template <>
string ConvertToString::Operation(uint16_t input) {
	return StandardStringCast(input);
}
template <>
string ConvertToString::Operation(uint32_t input) {
	return StandardStringCast(input);
}
template <>
string ConvertToString::Operation(uint64_t input) {
	return StandardStringCast(input);
}
template <>
string ConvertToString::Operation(hugeint_t input) {
	return StandardStringCast(input);
}
template <>
string ConvertToString::Operation(uhugeint_t input) {
	return StandardStringCast(input);
}
template <>
string ConvertToString::Operation(float input) {
	return StandardStringCast(input);
}
template <>
string ConvertToString::Operation(double input) {
	return StandardStringCast(input);
}
template <>
string ConvertToString::Operation(interval_t input) {
	return StandardStringCast(input);
}
template <>
string ConvertToString::Operation(date_t input) {
	return StandardStringCast(input);
}
template <>
string ConvertToString::Operation(dtime_t input) {
	return StandardStringCast(input);
}
template <>
string ConvertToString::Operation(timestamp_t input) {
	return StandardStringCast(input);
}
template <>
string ConvertToString::Operation(timestamp_ns_t input) {
	return StandardStringCast(input);
}
template <>
string ConvertToString::Operation(timestamp_tz_t input) {
	return StandardStringCast(input);
}
template <>
string ConvertToString::Operation(timestamp_tz_ns_t input) {
	return StandardStringCast(input);
}
template <>
string ConvertToString::Operation(string_t input) {
	return input.GetString();
}

template <>
string ConvertToString::Operation(bignum_t input) {
	return Bignum::BignumToVarchar(input);
}

} // namespace duckdb
