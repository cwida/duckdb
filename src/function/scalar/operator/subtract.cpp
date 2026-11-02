#include "duckdb/common/operator/subtract.hpp"

#include "duckdb/common/limits.hpp"
#include "duckdb/common/operator/add.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/uhugeint.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/interval.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// - [subtract]
//===--------------------------------------------------------------------===//
template <>
float SubtractOperator::Operation(float left, float right) {
	auto result = left - right;
	return result;
}

template <>
double SubtractOperator::Operation(double left, double right) {
	auto result = left - right;
	return result;
}

template <>
int64_t SubtractOperator::Operation(date_t left, date_t right) {
	return int64_t(left.days) - int64_t(right.days);
}

template <>
date_t SubtractOperator::Operation(date_t left, int32_t right) {
	if (!left.IsFinite()) {
		return left;
	}
	// dates below -infinity are out of range, as input and as result
	int32_t days;
	if (left < date_t::ninfinity() || !TrySubtractOperator::Operation(left.days, right, days)) {
		throw OutOfRangeException("Date out of range");
	}

	date_t result(days);
	if (result <= date_t::ninfinity() || result >= date_t::infinity()) {
		throw OutOfRangeException("Date out of range");
	}
	return result;
}

template <>
interval_t SubtractOperator::Operation(interval_t left, interval_t right) {
	interval_t result;
	if (!TrySubtractOperator::Operation(left.months, right.months, result.months)) {
		throw OutOfRangeException("Interval months subtraction out of range");
	}
	if (!TrySubtractOperator::Operation(left.days, right.days, result.days)) {
		throw OutOfRangeException("Interval days subtraction out of range");
	}
	if (!TrySubtractOperator::Operation(left.micros, right.micros, result.micros)) {
		throw OutOfRangeException("Interval micros subtraction out of range");
	}
	return result;
}

template <>
timestamp_t SubtractOperator::Operation(date_t left, interval_t right) {
	return AddOperator::Operation<date_t, interval_t, timestamp_t>(left, Interval::Invert(right));
}

template <>
timestamp_t SubtractOperator::Operation(timestamp_t left, interval_t right) {
	return AddOperator::Operation<timestamp_t, interval_t, timestamp_t>(left, Interval::Invert(right));
}

template <>
interval_t SubtractOperator::Operation(timestamp_t left, timestamp_t right) {
	return Interval::GetDifference(left, right);
}

template <>
int64_t SubtractOperator::Operation(timestamp_t left, timestamp_t right) {
	int64_t result;
	if (!TrySubtractOperator::Operation(left, right, result)) {
		throw OutOfRangeException("Overflow in timestamp subtraction");
	}
	return result;
}

//===--------------------------------------------------------------------===//
// - [subtract] with overflow check
//===--------------------------------------------------------------------===//
namespace {
struct OverflowCheckedSubtract {
	template <class SRCTYPE, class UTYPE>
	static inline bool Operation(SRCTYPE left, SRCTYPE right, SRCTYPE &result) {
		UTYPE uresult = SubtractOperator::Operation<UTYPE, UTYPE, UTYPE>(UTYPE(left), UTYPE(right));
		if (uresult < NumericLimits<SRCTYPE>::Minimum() || uresult > NumericLimits<SRCTYPE>::Maximum()) {
			return false;
		}
		result = SRCTYPE(uresult);
		return true;
	}
};
} // namespace

template <>
bool TrySubtractOperator::Operation(uint8_t left, uint8_t right, uint8_t &result) {
	if (right > left) {
		return false;
	}
	return OverflowCheckedSubtract::Operation<uint8_t, uint16_t>(left, right, result);
}

template <>
bool TrySubtractOperator::Operation(uint16_t left, uint16_t right, uint16_t &result) {
	if (right > left) {
		return false;
	}
	return OverflowCheckedSubtract::Operation<uint16_t, uint32_t>(left, right, result);
}

template <>
bool TrySubtractOperator::Operation(uint32_t left, uint32_t right, uint32_t &result) {
	if (right > left) {
		return false;
	}
	return OverflowCheckedSubtract::Operation<uint32_t, uint64_t>(left, right, result);
}

template <>
bool TrySubtractOperator::Operation(uint64_t left, uint64_t right, uint64_t &result) {
	if (right > left) {
		return false;
	}
	return OverflowCheckedSubtract::Operation<uint64_t, uint64_t>(left, right, result);
}

template <>
bool TrySubtractOperator::Operation(int8_t left, int8_t right, int8_t &result) {
	return OverflowCheckedSubtract::Operation<int8_t, int16_t>(left, right, result);
}

template <>
bool TrySubtractOperator::Operation(int16_t left, int16_t right, int16_t &result) {
	return OverflowCheckedSubtract::Operation<int16_t, int32_t>(left, right, result);
}

template <>
bool TrySubtractOperator::Operation(int32_t left, int32_t right, int32_t &result) {
	return OverflowCheckedSubtract::Operation<int32_t, int64_t>(left, right, result);
}

template <>
bool TrySubtractOperator::Operation(int64_t left, int64_t right, int64_t &result) {
#if (__GNUC__ >= 5) || defined(__clang__)
	if (__builtin_sub_overflow(left, right, &result)) {
		return false;
	}
#else
	if (right < 0) {
		if (NumericLimits<int64_t>::Maximum() + right < left) {
			return false;
		}
	} else {
		if (NumericLimits<int64_t>::Minimum() + right > left) {
			return false;
		}
	}
	result = left - right;
#endif
	return true;
}

template <>
bool TrySubtractOperator::Operation(hugeint_t left, hugeint_t right, hugeint_t &result) {
	result = left;
	return Hugeint::TrySubtractInPlace(result, right);
}

template <>
bool TrySubtractOperator::Operation(uhugeint_t left, uhugeint_t right, uhugeint_t &result) {
	result = left;
	return Uhugeint::TrySubtractInPlace(result, right);
}

//===--------------------------------------------------------------------===//
// subtract decimal with overflow check
//===--------------------------------------------------------------------===//
template <class T, T min, T max>
static bool TryDecimalSubtractTemplated(T left, T right, T &result) {
	if (right < 0) {
		if (max + right < left) {
			return false;
		}
	} else {
		if (min + right > left) {
			return false;
		}
	}
	result = left - right;
	return true;
}

template <>
bool TryDecimalSubtract::Operation(int16_t left, int16_t right, int16_t &result) {
	return TryDecimalSubtractTemplated<int16_t, -9999, 9999>(left, right, result);
}

template <>
bool TryDecimalSubtract::Operation(int32_t left, int32_t right, int32_t &result) {
	return TryDecimalSubtractTemplated<int32_t, -999999999, 999999999>(left, right, result);
}

template <>
bool TryDecimalSubtract::Operation(int64_t left, int64_t right, int64_t &result) {
	return TryDecimalSubtractTemplated<int64_t, -999999999999999999, 999999999999999999>(left, right, result);
}

template <>
bool TryDecimalSubtract::Operation(hugeint_t left, hugeint_t right, hugeint_t &result) {
	if (!TrySubtractOperator::Operation(left, right, result)) {
		return false;
	}
	if (result <= -Hugeint::POWERS_OF_TEN[38] || result >= Hugeint::POWERS_OF_TEN[38]) {
		return false;
	}
	return true;
}

template <>
hugeint_t DecimalSubtractOverflowCheck::Operation(hugeint_t left, hugeint_t right) {
	hugeint_t result;
	if (!TryDecimalSubtract::Operation(left, right, result)) {
		throw OutOfRangeException("Overflow in subtract of DECIMAL(38) (%s - %s);", left.ToString(), right.ToString());
	}
	return result;
}

//===--------------------------------------------------------------------===//
// subtract time operator
//===--------------------------------------------------------------------===//
template <>
dtime_t SubtractTimeOperator::Operation(dtime_t left, interval_t right) {
	if (right.micros == NumericLimits<int64_t>::Minimum()) {
		throw OutOfRangeException("Interval micros value out of range");
	}
	right.micros = -right.micros;
	return AddTimeOperator::Operation<dtime_t, interval_t, dtime_t>(left, right);
}

template <>
dtime_tz_t SubtractTimeOperator::Operation(dtime_tz_t left, interval_t right) {
	if (right.micros == NumericLimits<int64_t>::Minimum()) {
		throw OutOfRangeException("Interval micros value out of range");
	}
	right.micros = -right.micros;
	return AddTimeOperator::Operation<dtime_tz_t, interval_t, dtime_tz_t>(left, right);
}

} // namespace duckdb
