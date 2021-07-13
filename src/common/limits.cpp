#include "duckdb/common/limits.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/windows_undefs.hpp"
#include <limits>

namespace duckdb {

using std::numeric_limits;

int8_t NumericLimits<int8_t>::Minimum() {
	return numeric_limits<int8_t>::lowest();
}

int8_t NumericLimits<int8_t>::Maximum() {
	return numeric_limits<int8_t>::max();
}

int16_t NumericLimits<int16_t>::Minimum() {
	return numeric_limits<int16_t>::lowest();
}

int16_t NumericLimits<int16_t>::Maximum() {
	return numeric_limits<int16_t>::max();
}

int32_t NumericLimits<int32_t>::Minimum() {
	return numeric_limits<int32_t>::lowest();
}

int32_t NumericLimits<int32_t>::Maximum() {
	return numeric_limits<int32_t>::max();
}

int64_t NumericLimits<int64_t>::Minimum() {
	return numeric_limits<int64_t>::lowest();
}

int64_t NumericLimits<int64_t>::Maximum() {
	return numeric_limits<int64_t>::max();
}

float NumericLimits<float>::Minimum() {
	return numeric_limits<float>::lowest();
}

float NumericLimits<float>::Maximum() {
	return numeric_limits<float>::max();
}

double NumericLimits<double>::Minimum() {
	return numeric_limits<double>::lowest();
}

double NumericLimits<double>::Maximum() {
	return numeric_limits<double>::max();
}

uint8_t NumericLimits<uint8_t>::Minimum() {
	return numeric_limits<uint8_t>::lowest();
}

uint8_t NumericLimits<uint8_t>::Maximum() {
	return numeric_limits<uint8_t>::max();
}

uint16_t NumericLimits<uint16_t>::Minimum() {
	return numeric_limits<uint16_t>::lowest();
}

uint16_t NumericLimits<uint16_t>::Maximum() {
	return numeric_limits<uint16_t>::max();
}

uint32_t NumericLimits<uint32_t>::Minimum() {
	return numeric_limits<uint32_t>::lowest();
}

uint32_t NumericLimits<uint32_t>::Maximum() {
	return numeric_limits<uint32_t>::max();
}

uint64_t NumericLimits<uint64_t>::Minimum() {
	return numeric_limits<uint64_t>::lowest();
}

uint64_t NumericLimits<uint64_t>::Maximum() {
	return numeric_limits<uint64_t>::max();
}

hugeint_t NumericLimits<hugeint_t>::Minimum() {
	hugeint_t result;
	result.lower = 1;
	result.upper = numeric_limits<int64_t>::lowest();
	return result;
}

hugeint_t NumericLimits<hugeint_t>::Maximum() {
	hugeint_t result;
	result.lower = numeric_limits<uint64_t>::max();
	result.upper = numeric_limits<int64_t>::max();
	return result;
}

} // namespace duckdb
