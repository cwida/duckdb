#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/enums/date_part_specifier.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/cast_helpers.hpp"
#include "duckdb/common/operator/add.hpp"
#include "duckdb/common/operator/multiply.hpp"

namespace duckdb {

bool Interval::FromString(const string &str, interval_t &result) {
	return Interval::FromCString(str.c_str(), str.size(), result);
}

template <class T>
void IntervalTryAddition(T &target, int64_t input, int64_t multiplier) {
	int64_t addition;
	if (!TryMultiplyOperator::Operation<int64_t, int64_t, int64_t>(input, multiplier, addition)) {
		throw OutOfRangeException("interval value is out of range");
	}
	T addition_base = Cast::Operation<int64_t, T>(addition);
	if (!TryAddOperator::Operation<T, T, T>(target, addition_base, target)) {
		throw OutOfRangeException("interval value is out of range");
	}
}

bool Interval::FromCString(const char *str, idx_t len, interval_t &result) {
	idx_t pos = 0;
	idx_t start_pos;
	bool negative;
	bool found_any = false;
	int64_t number;
	DatePartSpecifier specifier;

	result.days = 0;
	result.micros = 0;
	result.months = 0;

	if (len == 0) {
		return false;
	}

	switch (str[pos]) {
	case '@':
		pos++;
		goto standard_interval;
	case 'P':
	case 'p':
		pos++;
		goto posix_interval;
	default:
		goto standard_interval;
	}
standard_interval:
	// start parsing a standard interval (e.g. 2 years 3 months...)
	for (; pos < len; pos++) {
		char c = str[pos];
		if (c == ' ' || c == '\t' || c == '\n') {
			// skip spaces
			continue;
		} else if (c >= '0' && c <= '9') {
			// start parsing a positive number
			negative = false;
			goto interval_parse_number;
		} else if (c == '-') {
			// negative number
			negative = true;
			pos++;
			goto interval_parse_number;
		} else if (c == 'a' || c == 'A') {
			// parse the word "ago" as the final specifier
			goto interval_parse_ago;
		} else {
			// unrecognized character, expected a number or end of string
			return false;
		}
	}
	goto end_of_string;
interval_parse_number:
	start_pos = pos;
	for (; pos < len; pos++) {
		char c = str[pos];
		if (c >= '0' && c <= '9') {
			// the number continues
			continue;
		} else if (c == ':') {
			// colon: we are parsing a time
			goto interval_parse_time;
		} else {
			if (pos == start_pos) {
				return false;
			}
			// finished the number, parse it from the string
			string_t nr_string(str + start_pos, pos - start_pos);
			number = Cast::Operation<string_t, int64_t>(nr_string);
			if (negative) {
				number = -number;
			}
			goto interval_parse_identifier;
		}
	}
	goto end_of_string;
interval_parse_time : {
	// parse the remainder of the time as a Time type
	dtime_t time = Time::FromCString(str + start_pos, len);
	result.micros += time.micros;
	found_any = true;
	goto end_of_string;
}
interval_parse_identifier:
	for (; pos < len; pos++) {
		char c = str[pos];
		if (c == ' ' || c == '\t' || c == '\n') {
			// skip spaces at the start
			continue;
		} else {
			break;
		}
	}
	// now parse the identifier
	start_pos = pos;
	for (; pos < len; pos++) {
		char c = str[pos];
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
			// keep parsing the string
			continue;
		} else {
			break;
		}
	}
	specifier = GetDatePartSpecifier(string(str + start_pos, pos - start_pos));
	// add the specifier to the interval
	switch (specifier) {
	case DatePartSpecifier::MILLENNIUM:
		IntervalTryAddition<int32_t>(result.months, number, MONTHS_PER_MILLENIUM);
		break;
	case DatePartSpecifier::CENTURY:
		IntervalTryAddition<int32_t>(result.months, number, MONTHS_PER_CENTURY);
		break;
	case DatePartSpecifier::DECADE:
		IntervalTryAddition<int32_t>(result.months, number, MONTHS_PER_DECADE);
		break;
	case DatePartSpecifier::YEAR:
		IntervalTryAddition<int32_t>(result.months, number, MONTHS_PER_YEAR);
		break;
	case DatePartSpecifier::QUARTER:
		IntervalTryAddition<int32_t>(result.months, number, MONTHS_PER_QUARTER);
		break;
	case DatePartSpecifier::MONTH:
		IntervalTryAddition<int32_t>(result.months, number, 1);
		break;
	case DatePartSpecifier::DAY:
		IntervalTryAddition<int32_t>(result.days, number, 1);
		break;
	case DatePartSpecifier::WEEK:
		IntervalTryAddition<int32_t>(result.days, number, DAYS_PER_WEEK);
		break;
	case DatePartSpecifier::MICROSECONDS:
		IntervalTryAddition<int64_t>(result.micros, number, 1);
		break;
	case DatePartSpecifier::MILLISECONDS:
		IntervalTryAddition<int64_t>(result.micros, number, MICROS_PER_MSEC);
		break;
	case DatePartSpecifier::SECOND:
		IntervalTryAddition<int64_t>(result.micros, number, MICROS_PER_SEC);
		break;
	case DatePartSpecifier::MINUTE:
		IntervalTryAddition<int64_t>(result.micros, number, MICROS_PER_MINUTE);
		break;
	case DatePartSpecifier::HOUR:
		IntervalTryAddition<int64_t>(result.micros, number, MICROS_PER_HOUR);
		break;
	default:
		return false;
	}
	found_any = true;
	goto standard_interval;
interval_parse_ago:
	D_ASSERT(str[pos] == 'a' || str[pos] == 'A');
	// parse the "ago" string at the end of the interval
	if (len - pos < 3) {
		return false;
	}
	pos++;
	if (!(str[pos] == 'g' || str[pos] == 'G')) {
		return false;
	}
	pos++;
	if (!(str[pos] == 'o' || str[pos] == 'O')) {
		return false;
	}
	pos++;
	// parse any trailing whitespace
	for (; pos < len; pos++) {
		char c = str[pos];
		if (c == ' ' || c == '\t' || c == '\n') {
			continue;
		} else {
			return false;
		}
	}
	// invert all the values
	result.months = -result.months;
	result.days = -result.days;
	result.micros = -result.micros;
	goto end_of_string;
end_of_string:
	if (!found_any) {
		// end of string and no identifiers were found: cannot convert empty interval
		return false;
	}
	return true;
posix_interval:
	return false;
}

string Interval::ToString(interval_t interval) {
	char buffer[70];
	idx_t length = IntervalToStringCast::Format(interval, buffer);
	return string(buffer, length);
}

int64_t Interval::GetMilli(interval_t val) {
	int64_t milli_month, milli_day, milli;
	if (!TryMultiplyOperator::Operation((int64_t)val.months, Interval::MICROS_PER_MONTH / 1000, milli_month)) {
		throw ConversionException("Could not convert Interval to Milliseconds");
	}
	if (!TryMultiplyOperator::Operation((int64_t)val.days, Interval::MICROS_PER_DAY / 1000, milli_day)) {
		throw ConversionException("Could not convert Interval to Milliseconds");
	}
	milli = val.micros / 1000;
	if (!TryAddOperator::Operation<int64_t, int64_t, int64_t>(milli, milli_month, milli)) {
		throw ConversionException("Could not convert Interval to Milliseconds");
	}
	if (!TryAddOperator::Operation<int64_t, int64_t, int64_t>(milli, milli_day, milli)) {
		throw ConversionException("Could not convert Interval to Milliseconds");
	}
	return milli;
}

int64_t Interval::GetNanoseconds(interval_t val) {
	int64_t micro_month, micro_day, micro_total, nano;
	int64_t ns_in_us = 1000;
	micro_total = val.micros;
	if (!TryMultiplyOperator::Operation((int64_t)val.months, Interval::MICROS_PER_MONTH, micro_month)) {
		throw ConversionException("Could not convert Month to Nanoseconds");
	}
	if (!TryMultiplyOperator::Operation((int64_t)val.days, Interval::MICROS_PER_DAY, micro_day)) {
		throw ConversionException("Could not convert Day to Nanoseconds");
	}
	if (!TryAddOperator::Operation<int64_t, int64_t, int64_t>(micro_total, micro_month, micro_total)) {
		throw ConversionException("Could not convert Interval to Nanoseconds");
	}
	if (!TryAddOperator::Operation<int64_t, int64_t, int64_t>(micro_total, micro_day, micro_total)) {
		throw ConversionException("Could not convert Interval to Nanoseconds");
	}
	if (!TryMultiplyOperator::Operation(micro_total, ns_in_us, nano)) {
		throw ConversionException("Could not convert Interval to Nanoseconds");
	}

	return nano;
}
interval_t Interval::GetDifference(timestamp_t timestamp_1, timestamp_t timestamp_2) {
	date_t date1, date2;
	dtime_t time1, time2;

	Timestamp::Convert(timestamp_1, date1, time1);
	Timestamp::Convert(timestamp_2, date2, time2);

	// and from date extract the years, months and days
	int32_t year1, month1, day1;
	int32_t year2, month2, day2;
	Date::Convert(date1, year1, month1, day1);
	Date::Convert(date2, year2, month2, day2);
	// finally perform the differences
	auto year_diff = year1 - year2;
	auto month_diff = month1 - month2;
	auto day_diff = day1 - day2;

	// and from time extract hours, minutes, seconds and milliseconds
	int32_t hour1, min1, sec1, micros1;
	int32_t hour2, min2, sec2, micros2;
	Time::Convert(time1, hour1, min1, sec1, micros1);
	Time::Convert(time2, hour2, min2, sec2, micros2);
	// finally perform the differences
	auto hour_diff = hour1 - hour2;
	auto min_diff = min1 - min2;
	auto sec_diff = sec1 - sec2;
	auto micros_diff = micros1 - micros2;

	// flip sign if necessary
	bool sign_flipped = false;
	if (timestamp_1 < timestamp_2) {
		year_diff = -year_diff;
		month_diff = -month_diff;
		day_diff = -day_diff;
		hour_diff = -hour_diff;
		min_diff = -min_diff;
		sec_diff = -sec_diff;
		micros_diff = -micros_diff;
		sign_flipped = true;
	}
	// now propagate any negative field into the next higher field
	while (micros_diff < 0) {
		micros_diff += MICROS_PER_SEC;
		sec_diff--;
	}
	while (sec_diff < 0) {
		sec_diff += SECS_PER_MINUTE;
		min_diff--;
	}
	while (min_diff < 0) {
		min_diff += MINS_PER_HOUR;
		hour_diff--;
	}
	while (hour_diff < 0) {
		hour_diff += HOURS_PER_DAY;
		day_diff--;
	}
	while (day_diff < 0) {
		if (timestamp_1 < timestamp_2) {
			day_diff += Date::IsLeapYear(year1) ? Date::LEAP_DAYS[month1] : Date::NORMAL_DAYS[month1];
			month_diff--;
		} else {
			day_diff += Date::IsLeapYear(year2) ? Date::LEAP_DAYS[month2] : Date::NORMAL_DAYS[month2];
			month_diff--;
		}
	}
	while (month_diff < 0) {
		month_diff += MONTHS_PER_YEAR;
		year_diff--;
	}

	// recover sign if necessary
	if (sign_flipped) {
		year_diff = -year_diff;
		month_diff = -month_diff;
		day_diff = -day_diff;
		hour_diff = -hour_diff;
		min_diff = -min_diff;
		sec_diff = -sec_diff;
		micros_diff = -micros_diff;
	}
	interval_t interval;
	interval.months = year_diff * MONTHS_PER_YEAR + month_diff;
	interval.days = day_diff;
	interval.micros = Time::FromTime(hour_diff, min_diff, sec_diff, micros_diff).micros;

	return interval;
}

static void NormalizeIntervalEntries(interval_t input, int64_t &months, int64_t &days, int64_t &micros) {
	int64_t extra_months_d = input.days / Interval::DAYS_PER_MONTH;
	int64_t extra_months_micros = input.micros / Interval::MICROS_PER_MONTH;
	input.days -= extra_months_d * Interval::DAYS_PER_MONTH;
	input.micros -= extra_months_micros * Interval::MICROS_PER_MONTH;

	int64_t extra_days_micros = input.micros / Interval::MICROS_PER_DAY;
	input.micros -= extra_days_micros * Interval::MICROS_PER_DAY;

	months = input.months + extra_months_d + extra_months_micros;
	days = input.days + extra_days_micros;
	micros = input.micros;
}

bool Interval::Equals(interval_t left, interval_t right) {
	return left.months == right.months && left.days == right.days && left.micros == right.micros;
}

bool Interval::GreaterThan(interval_t left, interval_t right) {
	int64_t lmonths, ldays, lmicros;
	int64_t rmonths, rdays, rmicros;
	NormalizeIntervalEntries(left, lmonths, ldays, lmicros);
	NormalizeIntervalEntries(right, rmonths, rdays, rmicros);

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

bool Interval::GreaterThanEquals(interval_t left, interval_t right) {
	return GreaterThan(left, right) || Equals(left, right);
}

} // namespace duckdb
