//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/operator/subtract.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/type_util.hpp"
#include "duckdb/common/types/cast_helpers.hpp"
#include "duckdb/common/types/datetime.hpp"
#include "duckdb/common/types/timestamp_base.hpp"

namespace duckdb {

struct interval_t;
struct date_t;

struct SubtractOperator {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA left, TB right) {
		return left - right;
	}
};

template <>
float SubtractOperator::Operation(float left, float right);
template <>
double SubtractOperator::Operation(double left, double right);
template <>
interval_t SubtractOperator::Operation(interval_t left, interval_t right);
template <>
int64_t SubtractOperator::Operation(date_t left, date_t right);
template <>
date_t SubtractOperator::Operation(date_t left, int32_t right);
template <>
timestamp_t SubtractOperator::Operation(date_t left, interval_t right);
template <>
timestamp_t SubtractOperator::Operation(timestamp_t left, interval_t right);
template <>
interval_t SubtractOperator::Operation(timestamp_t left, timestamp_t right);
template <>
int64_t SubtractOperator::Operation(timestamp_t left, timestamp_t right);

struct TrySubtractOperator {
	template <class TA, class TB, class TR>
	static inline bool Operation(TA left, TB right, TR &result) {
		InternalException::Throw("Unimplemented type for TrySubtractOperator");
	}
};

template <>
bool TrySubtractOperator::Operation(uint8_t left, uint8_t right, uint8_t &result);
template <>
bool TrySubtractOperator::Operation(uint16_t left, uint16_t right, uint16_t &result);
template <>
bool TrySubtractOperator::Operation(uint32_t left, uint32_t right, uint32_t &result);
template <>
bool TrySubtractOperator::Operation(uint64_t left, uint64_t right, uint64_t &result);

template <>
bool TrySubtractOperator::Operation(int8_t left, int8_t right, int8_t &result);
template <>
bool TrySubtractOperator::Operation(int16_t left, int16_t right, int16_t &result);
template <>
bool TrySubtractOperator::Operation(int32_t left, int32_t right, int32_t &result);
template <>
bool TrySubtractOperator::Operation(int64_t left, int64_t right, int64_t &result);
template <>
bool TrySubtractOperator::Operation(hugeint_t left, hugeint_t right, hugeint_t &result);
template <>
bool TrySubtractOperator::Operation(uhugeint_t left, uhugeint_t right, uhugeint_t &result);

struct SubtractOperatorOverflowCheck {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA left, TB right) {
		TR result;
		if (!TrySubtractOperator::Operation(left, right, result)) {
			throw OutOfRangeException("Overflow in subtraction of %s (%s - %s)!", TypeIdToString(GetTypeId<TA>()),
			                          NumericHelper::ToString(left), NumericHelper::ToString(right));
		}
		return result;
	}
};

struct TryDecimalSubtract {
	template <class TA, class TB, class TR>
	static inline bool Operation(TA left, TB right, TR &result) {
		InternalException::Throw("Unimplemented type for TryDecimalSubtract");
	}
};

template <>
bool TryDecimalSubtract::Operation(int16_t left, int16_t right, int16_t &result);
template <>
bool TryDecimalSubtract::Operation(int32_t left, int32_t right, int32_t &result);
template <>
bool TryDecimalSubtract::Operation(int64_t left, int64_t right, int64_t &result);
template <>
bool TryDecimalSubtract::Operation(hugeint_t left, hugeint_t right, hugeint_t &result);

struct DecimalSubtractOverflowCheck {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA left, TB right) {
		TR result;
		if (!TryDecimalSubtract::Operation<TA, TB, TR>(left, right, result)) {
			throw OutOfRangeException("Overflow in subtract of DECIMAL(18) (%d - %d). You might want to add an "
			                          "explicit cast to a bigger decimal.",
			                          left, right);
		}
		return result;
	}
};

template <>
hugeint_t DecimalSubtractOverflowCheck::Operation(hugeint_t left, hugeint_t right);

template <>
inline bool TrySubtractOperator::Operation(timestamp_t left, timestamp_t right, int64_t &result) {
	return TrySubtractOperator::Operation(left.value, right.value, result);
}

struct SubtractTimeOperator {
	template <class TA, class TB, class TR>
	static TR Operation(TA left, TB right);
};

template <>
dtime_t SubtractTimeOperator::Operation(dtime_t left, interval_t right);

template <>
dtime_tz_t SubtractTimeOperator::Operation(dtime_tz_t left, interval_t right);

} // namespace duckdb
