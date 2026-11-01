//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/interval.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types.hpp"
#include "duckdb/common/types/datetime.hpp"
#include "duckdb/common/types/timestamp_base.hpp"
#include "duckdb/common/numeric_utils.hpp"

namespace duckdb {

struct date_t; // NOLINT: literal casing
struct TimestampComponents;

class Serializer;
class Deserializer;

struct interval_t { // NOLINT
	int32_t months;
	int32_t days;
	int64_t micros;

	inline void Normalize(int64_t &months, int64_t &days, int64_t &micros) const;

	// Normalize to interval bounds.
	inline static void Borrow(const int64_t msf, int64_t &lsf, int32_t &f, const int64_t scale);
	inline interval_t Normalize() const;

	inline bool operator==(const interval_t &right) const {
		//	Quick equality check
		const auto &left = *this;
		if (left.months == right.months && left.days == right.days && left.micros == right.micros) {
			return true;
		}

		int64_t lmonths, ldays, lmicros;
		int64_t rmonths, rdays, rmicros;
		left.Normalize(lmonths, ldays, lmicros);
		right.Normalize(rmonths, rdays, rmicros);

		return lmonths == rmonths && ldays == rdays && lmicros == rmicros;
	}
	inline bool operator!=(const interval_t &right) const {
		return !(*this == right);
	}

	inline bool operator>(const interval_t &right) const {
		const auto &left = *this;
		int64_t lmonths, ldays, lmicros;
		int64_t rmonths, rdays, rmicros;
		left.Normalize(lmonths, ldays, lmicros);
		right.Normalize(rmonths, rdays, rmicros);

		if (lmonths > rmonths) {
			return true;
		} else if (lmonths < rmonths) {
			return false;
		}
		if (ldays > rdays) {
			return true;
		} else if (ldays < rdays) {
			return false;
		}
		return lmicros > rmicros;
	}

	inline bool operator<(const interval_t &right) const {
		return right > *this;
	}

	inline bool operator<=(const interval_t &right) const {
		return !(*this > right);
	}

	inline bool operator>=(const interval_t &right) const {
		return !(*this < right);
	}

	inline bool IsFinite() const {
		return true;
	}

	// Serialization
	void Serialize(Serializer &serializer) const;
	static interval_t Deserialize(Deserializer &source);
};

//! The Interval class is a static class that holds helper functions for the Interval
//! type.
class Interval {
public:
	static constexpr const int32_t MONTHS_PER_MILLENIUM = 12000;
	static constexpr const int32_t MONTHS_PER_CENTURY = 1200;
	static constexpr const int32_t MONTHS_PER_DECADE = 120;
	static constexpr const int32_t MONTHS_PER_YEAR = 12;
	static constexpr const int32_t MONTHS_PER_QUARTER = 3;
	static constexpr const int32_t DAYS_PER_WEEK = 7;
	//! only used for interval comparison/ordering purposes, in which case a month counts as 30 days
	static constexpr const int64_t DAYS_PER_MONTH = 30;
	static constexpr const int64_t DAYS_PER_YEAR = 365;
	static constexpr const int64_t MSECS_PER_SEC = 1000;
	static constexpr const int32_t SECS_PER_MINUTE = 60;
	static constexpr const int32_t MINS_PER_HOUR = 60;
	static constexpr const int32_t HOURS_PER_DAY = 24;
	static constexpr const int32_t SECS_PER_HOUR = SECS_PER_MINUTE * MINS_PER_HOUR;
	static constexpr const int32_t SECS_PER_DAY = SECS_PER_HOUR * HOURS_PER_DAY;
	static constexpr const int32_t SECS_PER_WEEK = SECS_PER_DAY * DAYS_PER_WEEK;

	static constexpr const int64_t MICROS_PER_MSEC = 1000;
	static constexpr const int64_t MICROS_PER_SEC = MICROS_PER_MSEC * MSECS_PER_SEC;
	static constexpr const int64_t MICROS_PER_MINUTE = MICROS_PER_SEC * SECS_PER_MINUTE;
	static constexpr const int64_t MICROS_PER_HOUR = MICROS_PER_MINUTE * MINS_PER_HOUR;
	static constexpr const int64_t MICROS_PER_DAY = MICROS_PER_HOUR * HOURS_PER_DAY;
	static constexpr const int64_t MICROS_PER_WEEK = MICROS_PER_DAY * DAYS_PER_WEEK;
	static constexpr const int64_t MICROS_PER_MONTH = MICROS_PER_DAY * DAYS_PER_MONTH;

	static constexpr const int64_t NANOS_PER_MICRO = 1000;
	static constexpr const int64_t NANOS_PER_MSEC = NANOS_PER_MICRO * MICROS_PER_MSEC;
	static constexpr const int64_t NANOS_PER_SEC = NANOS_PER_MSEC * MSECS_PER_SEC;
	static constexpr const int64_t NANOS_PER_MINUTE = NANOS_PER_SEC * SECS_PER_MINUTE;
	static constexpr const int64_t NANOS_PER_HOUR = NANOS_PER_MINUTE * MINS_PER_HOUR;
	static constexpr const int64_t NANOS_PER_DAY = NANOS_PER_HOUR * HOURS_PER_DAY;
	static constexpr const int64_t NANOS_PER_WEEK = NANOS_PER_DAY * DAYS_PER_WEEK;

public:
	//! Convert a string to an interval object
	static bool FromString(const string &str, interval_t &result);
	//! Convert a string to an interval object
	static bool FromCString(const char *str, idx_t len, interval_t &result, string *error_message, bool strict);
	//! Convert an interval object to a string
	static string ToString(const interval_t &val);

	//! Convert milliseconds to a normalised interval
	DUCKDB_API static interval_t FromMicro(int64_t micros);

	//! Get Interval in milliseconds
	static int64_t GetMilli(const interval_t &val);

	//! Get Interval in microseconds
	static bool TryGetMicro(const interval_t &val, int64_t &micros);
	static int64_t GetMicro(const interval_t &val);

	//! Get Interval in Nanoseconds
	static int64_t GetNanoseconds(const interval_t &val);

	//! Returns the age between two timestamps (including months)
	static interval_t GetAge(timestamp_t timestamp_1, timestamp_t timestamp_2);
	//! Returns the age between two timestamp components
	static interval_t GetAge(TimestampComponents ts1, TimestampComponents ts2, bool is_negative);

	//! Returns the exact difference between two timestamps (days and seconds)
	static interval_t GetDifference(timestamp_t timestamp_1, timestamp_t timestamp_2);
	//! Returns the number of whole units of micros_per_unit from start_us to end_us, also when their difference in
	//! microseconds does not fit an int64
	DUCKDB_API static int64_t GetDifferenceInUnits(int64_t start_us, int64_t end_us, int64_t micros_per_unit);

	//! Returns the inverted interval
	static interval_t Invert(interval_t interval);

	//! Add an interval to a date
	static date_t Add(date_t left, interval_t right);
	//! Add an interval to a timestamp
	static timestamp_t Add(timestamp_t left, interval_t right);
	//! Add an interval to a date, returns false instead of throwing if the result is out of range
	static bool TryAdd(date_t left, interval_t right, date_t &result);
	//! Add an interval to a timestamp, returns false instead of throwing if the result is out of range
	static bool TryAdd(timestamp_t left, interval_t right, timestamp_t &result);
	//! Add an interval to a time. In case the time overflows or underflows, modify the date by the overflow.
	//! For example if we go from 23:00 to 02:00, we add a day to the date
	static dtime_t Add(dtime_t left, interval_t right, date_t &date);
	static dtime_tz_t Add(dtime_tz_t left, interval_t right, date_t &date);

	//! Comparison operators
	inline static bool Equals(const interval_t &left, const interval_t &right) {
		return left == right;
	}
	inline static bool GreaterThan(const interval_t &left, const interval_t &right) {
		return left > right;
	}
};

void interval_t::Normalize(int64_t &months, int64_t &days, int64_t &micros) const {
	auto &input = *this;

	//  Carry left, borrowing so that the remainders are never negative.
	//  Truncating division would leave e.g. 1 month -4 days unnormalized, which then compares above 26 days.
	micros = input.micros;
	int64_t carry_days = micros / Interval::MICROS_PER_DAY;
	micros -= carry_days * Interval::MICROS_PER_DAY;
	if (micros < 0) {
		--carry_days;
		micros += Interval::MICROS_PER_DAY;
	}

	days = input.days;
	days += carry_days;
	int64_t carry_months = days / Interval::DAYS_PER_MONTH;
	days -= carry_months * Interval::DAYS_PER_MONTH;
	if (days < 0) {
		--carry_months;
		days += Interval::DAYS_PER_MONTH;
	}

	months = input.months;
	months += carry_months;
}

void interval_t::Borrow(const int64_t msf, int64_t &lsf, int32_t &f, const int64_t scale) {
	if (msf > NumericLimits<int32_t>::Maximum()) {
		f = NumericLimits<int32_t>::Maximum();
	} else if (msf < NumericLimits<int32_t>::Minimum()) {
		f = NumericLimits<int32_t>::Minimum();
	} else {
		f = UnsafeNumericCast<int32_t>(msf);
		return;
	}

	//	The borrowed amount is not always representable, so saturate instead of overflowing.
	//	Normalize leaves lsf in [0, scale), so the headroom below keeps the addition in range as well.
	const int64_t remainder = msf - f;
	const int64_t max_units = NumericLimits<int64_t>::Maximum() / scale - 1;
	if (remainder > max_units) {
		lsf = NumericLimits<int64_t>::Maximum();
	} else if (remainder < -max_units) {
		lsf = NumericLimits<int64_t>::Minimum();
	} else {
		lsf += remainder * scale;
	}
}

interval_t interval_t::Normalize() const {
	interval_t result;

	int64_t mm;
	int64_t dd;
	Normalize(mm, dd, result.micros);

	//  Borrow right on overflow
	Borrow(mm, dd, result.months, Interval::DAYS_PER_MONTH);
	Borrow(dd, result.micros, result.days, Interval::MICROS_PER_DAY);

	return result;
}

} // namespace duckdb

namespace std {
template <>
struct hash<duckdb::interval_t> {
	size_t operator()(const duckdb::interval_t &val) const {
		int64_t months, days, micros;
		val.Normalize(months, days, micros);
		using std::hash;

		return hash<int32_t> {}(duckdb::UnsafeNumericCast<int32_t>(days)) ^
		       hash<int32_t> {}(duckdb::UnsafeNumericCast<int32_t>(months)) ^ hash<int64_t> {}(micros);
	}
};
} // namespace std
