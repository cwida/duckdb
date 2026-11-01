#include "duckdb/common/operator/add.hpp"

#include "duckdb/common/limits.hpp"
#include "duckdb/common/types/value.hpp"

#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/uhugeint.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// + [add]
//===--------------------------------------------------------------------===//
template <>
float AddOperator::Operation(float left, float right) {
	auto result = left + right;
	return result;
}

template <>
double AddOperator::Operation(double left, double right) {
	auto result = left + right;
	return result;
}

template <>
interval_t AddOperator::Operation(interval_t left, interval_t right) {
	left.months = AddOperatorOverflowCheck::Operation<int32_t, int32_t, int32_t>(left.months, right.months);
	left.days = AddOperatorOverflowCheck::Operation<int32_t, int32_t, int32_t>(left.days, right.days);
	left.micros = AddOperatorOverflowCheck::Operation<int64_t, int64_t, int64_t>(left.micros, right.micros);
	return left;
}

template <>
date_t AddOperator::Operation(date_t left, int32_t right) {
	date_t result;
	if (!TryAddOperator::Operation(left, right, result)) {
		throw OutOfRangeException("Date out of range");
	}
	return result;
}

template <>
date_t AddOperator::Operation(int32_t left, date_t right) {
	return AddOperator::Operation<date_t, int32_t, date_t>(right, left);
}

template <>
timestamp_t AddOperator::Operation(date_t left, dtime_t right) {
	if (left == date_t::infinity()) {
		return timestamp_t::infinity();
	} else if (left == date_t::ninfinity()) {
		return timestamp_t::ninfinity();
	}
	timestamp_t result;
	if (!Timestamp::TryFromDatetime(left, right, result)) {
		throw OutOfRangeException("Timestamp out of range");
	}
	return result;
}

template <>
timestamp_t AddOperator::Operation(date_t left, dtime_tz_t right) {
	if (left == date_t::infinity()) {
		return timestamp_t::infinity();
	} else if (left == date_t::ninfinity()) {
		return timestamp_t::ninfinity();
	}
	timestamp_t result;
	if (!Timestamp::TryFromDatetime(left, right, result)) {
		throw OutOfRangeException("Timestamp with time zone out of range");
	}
	return result;
}

template <>
timestamp_t AddOperator::Operation(dtime_t left, date_t right) {
	return AddOperator::Operation<date_t, dtime_t, timestamp_t>(right, left);
}

template <>
timestamp_t AddOperator::Operation(dtime_tz_t left, date_t right) {
	return AddOperator::Operation<date_t, dtime_tz_t, timestamp_t>(right, left);
}

template <>
timestamp_t AddOperator::Operation(date_t left, interval_t right) {
	if (left == date_t::infinity()) {
		return timestamp_t::infinity();
	}
	if (left == date_t::ninfinity()) {
		return timestamp_t::ninfinity();
	}
	return Interval::Add(Timestamp::FromDatetime(left, dtime_t(0)), right);
}

template <>
timestamp_t AddOperator::Operation(interval_t left, date_t right) {
	return AddOperator::Operation<date_t, interval_t, timestamp_t>(right, left);
}

template <>
timestamp_t AddOperator::Operation(timestamp_t left, interval_t right) {
	return Interval::Add(left, right);
}

template <>
timestamp_t AddOperator::Operation(interval_t left, timestamp_t right) {
	return AddOperator::Operation<timestamp_t, interval_t, timestamp_t>(right, left);
}

template <>
timestamp_t AddOperator::Operation(timestamp_t left, double right) {
	timestamp_t result;
	if (!TryAddOperator::Operation(left.value, int64_t(right), result.value)) {
		throw OutOfRangeException("Overflow in timestamp addition");
	}
	return result;
}

//===--------------------------------------------------------------------===//
// + [add] with overflow check
//===--------------------------------------------------------------------===//

namespace {

struct OverflowCheckedAddition {
	template <class SRCTYPE, class UTYPE>
	static inline bool Operation(SRCTYPE left, SRCTYPE right, SRCTYPE &result) {
		UTYPE uresult = AddOperator::Operation<UTYPE, UTYPE, UTYPE>(UTYPE(left), UTYPE(right));
		if (uresult < NumericLimits<SRCTYPE>::Minimum() || uresult > NumericLimits<SRCTYPE>::Maximum()) {
			return false;
		}
		result = SRCTYPE(uresult);
		return true;
	}
};

} // namespace

template <>
bool TryAddOperator::Operation(uint8_t left, uint8_t right, uint8_t &result) {
	return OverflowCheckedAddition::Operation<uint8_t, uint16_t>(left, right, result);
}
template <>
bool TryAddOperator::Operation(uint16_t left, uint16_t right, uint16_t &result) {
	return OverflowCheckedAddition::Operation<uint16_t, uint32_t>(left, right, result);
}
template <>
bool TryAddOperator::Operation(uint32_t left, uint32_t right, uint32_t &result) {
	return OverflowCheckedAddition::Operation<uint32_t, uint64_t>(left, right, result);
}

template <>
bool TryAddOperator::Operation(uint64_t left, uint64_t right, uint64_t &result) {
	if (NumericLimits<uint64_t>::Maximum() - left < right) {
		return false;
	}
	return OverflowCheckedAddition::Operation<uint64_t, uint64_t>(left, right, result);
}

template <>
bool TryAddOperator::Operation(timestamp_t left, interval_t right, timestamp_t &result) {
	return Interval::TryAdd(left, right, result);
}

template <>
bool TryAddOperator::Operation(date_t left, int32_t right, date_t &result) {
	if (left == date_t::infinity() || left == date_t::ninfinity()) {
		result = date_t(left);
		return true;
	}
	// dates below -infinity are out of range, as input and as result
	int32_t days;
	if (left < date_t::ninfinity() || !TryAddOperator::Operation(left.days, right, days)) {
		return false;
	}
	result.days = days;
	return result > date_t::ninfinity() && result < date_t::infinity();
}

template <>
bool TryAddOperator::Operation(int8_t left, int8_t right, int8_t &result) {
	return OverflowCheckedAddition::Operation<int8_t, int16_t>(left, right, result);
}

template <>
bool TryAddOperator::Operation(int16_t left, int16_t right, int16_t &result) {
	return OverflowCheckedAddition::Operation<int16_t, int32_t>(left, right, result);
}

template <>
bool TryAddOperator::Operation(int32_t left, int32_t right, int32_t &result) {
	return OverflowCheckedAddition::Operation<int32_t, int64_t>(left, right, result);
}

template <>
bool TryAddOperator::Operation(int64_t left, int64_t right, int64_t &result) {
#if (__GNUC__ >= 5) || defined(__clang__)
	if (__builtin_add_overflow(left, right, &result)) {
		return false;
	}
#else
	// https://blog.regehr.org/archives/1139
	result = int64_t((uint64_t)left + (uint64_t)right);
	if ((left < 0 && right < 0 && result >= 0) || (left >= 0 && right >= 0 && result < 0)) {
		return false;
	}
#endif
	return true;
}

template <>
bool TryAddOperator::Operation(uhugeint_t left, uhugeint_t right, uhugeint_t &result) {
	if (!Uhugeint::TryAddInPlace(left, right)) {
		return false;
	}
	result = left;
	return true;
}

template <>
bool TryAddOperator::Operation(hugeint_t left, hugeint_t right, hugeint_t &result) {
	if (!Hugeint::TryAddInPlace(left, right)) {
		return false;
	}
	result = left;
	return true;
}

//===--------------------------------------------------------------------===//
// add decimal with overflow check
//===--------------------------------------------------------------------===//
template <class T, T min, T max>
static bool TryDecimalAddTemplated(T left, T right, T &result) {
	if (right < 0) {
		if (min - right > left) {
			return false;
		}
	} else {
		if (max - right < left) {
			return false;
		}
	}
	result = left + right;
	return true;
}

template <>
bool TryDecimalAdd::Operation(int16_t left, int16_t right, int16_t &result) {
	return TryDecimalAddTemplated<int16_t, -9999, 9999>(left, right, result);
}

template <>
bool TryDecimalAdd::Operation(int32_t left, int32_t right, int32_t &result) {
	return TryDecimalAddTemplated<int32_t, -999999999, 999999999>(left, right, result);
}

template <>
bool TryDecimalAdd::Operation(int64_t left, int64_t right, int64_t &result) {
	return TryDecimalAddTemplated<int64_t, -999999999999999999, 999999999999999999>(left, right, result);
}

template <>
bool TryDecimalAdd::Operation(hugeint_t left, hugeint_t right, hugeint_t &result) {
	if (!TryAddOperator::Operation(left, right, result)) {
		return false;
	}
	if (result <= -Hugeint::POWERS_OF_TEN[38] || result >= Hugeint::POWERS_OF_TEN[38]) {
		return false;
	}
	return true;
}

template <>
hugeint_t DecimalAddOverflowCheck::Operation(hugeint_t left, hugeint_t right) {
	hugeint_t result;
	if (!TryDecimalAdd::Operation(left, right, result)) {
		throw OutOfRangeException("Overflow in addition of DECIMAL(38) (%s + %s);", left.ToString(), right.ToString());
	}
	return result;
}

//===--------------------------------------------------------------------===//
// add time operator
//===--------------------------------------------------------------------===//
template <>
dtime_t AddTimeOperator::Operation(dtime_t left, interval_t right) {
	date_t date(0);
	return Interval::Add(left, right, date);
}

template <>
dtime_t AddTimeOperator::Operation(interval_t left, dtime_t right) {
	return AddTimeOperator::Operation<dtime_t, interval_t, dtime_t>(right, left);
}

template <>
dtime_tz_t AddTimeOperator::Operation(dtime_tz_t left, interval_t right) {
	date_t date(0);
	return Interval::Add(left, right, date);
}

template <>
dtime_tz_t AddTimeOperator::Operation(interval_t left, dtime_tz_t right) {
	return AddTimeOperator::Operation<dtime_tz_t, interval_t, dtime_tz_t>(right, left);
}

} // namespace duckdb
