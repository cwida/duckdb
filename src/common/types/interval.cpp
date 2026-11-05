#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/enums/date_part_specifier.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/cast_helpers.hpp"
#include "duckdb/common/operator/add.hpp"
#include "duckdb/common/operator/multiply.hpp"
#include "duckdb/common/operator/subtract.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/limits.hpp"

namespace duckdb {

namespace {
void AssignInvalidInputErrorOrThrow(const string &message, string *error_message) {
	if (error_message) {
		HandleCastError::AssignError(message, error_message);
		return;
	}
	throw InvalidInputException(message);
}

void AssignOutOfRangeErrorOrThrow(const string &message, string *error_message) {
	if (error_message) {
		HandleCastError::AssignError(message, error_message);
		return;
	}
	throw OutOfRangeException(message);
}
} // namespace

bool Interval::FromString(const string &str, interval_t &result) {
	string error_message;
	return Interval::FromCString(str.c_str(), str.size(), result, &error_message, false);
}

template <class T>
bool IntervalTryAddition(T &target, int64_t input, int64_t multiplier, string *error_message, double fraction = 0) {
	int64_t addition;
	if (!TryMultiplyOperator::Operation<int64_t, int64_t, int64_t>(input, multiplier, addition)) {
		AssignOutOfRangeErrorOrThrow("interval value is out of range", error_message);
		return false;
	}
	T addition_base;
	if (!TryCast::Operation<int64_t, T>(addition, addition_base)) {
		AssignInvalidInputErrorOrThrow(CastExceptionText<int64_t, T>(addition), error_message);
		return false;
	}
	if (!TryAddOperator::Operation<T, T, T>(target, addition_base, target)) {
		AssignOutOfRangeErrorOrThrow("interval value is out of range", error_message);
		return false;
	}
	if (std::fabs(fraction) > 1e-10) {
		addition = static_cast<int64_t>(round(fraction * static_cast<double>(multiplier)));
		if (!TryCast::Operation<int64_t, T>(addition, addition_base)) {
			AssignInvalidInputErrorOrThrow(CastExceptionText<int64_t, T>(addition), error_message);
			return false;
		}
		if (!TryAddOperator::Operation<T, T, T>(target, addition_base, target)) {
			AssignOutOfRangeErrorOrThrow("interval fraction is out of range", error_message);
			return false;
		}
	}
	return true;
}

bool Interval::FromCString(const char *str, idx_t len, interval_t &result, string *error_message, bool strict) {
	idx_t pos = 0;
	idx_t start_pos;
	bool negative;
	bool found_any = false;
	int64_t number;
	double fraction;
	DatePartSpecifier specifier;
	string specifier_str;

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
			break;
		}
	}
	{
		if (pos == start_pos) {
			return false;
		}
		string_t nr_string(str + start_pos, UnsafeNumericCast<uint32_t>(pos - start_pos));
		if (!TryCast::Operation<string_t, int64_t>(nr_string, number)) {
			AssignInvalidInputErrorOrThrow(CastExceptionText<string_t, int64_t>(nr_string), error_message);
			return false;
		}
		fraction = 0;
		if (pos < len && str[pos] == '.') {
			idx_t frac_start = 0;
			for (++pos; pos < len && StringUtil::CharacterIsDigit(str[pos]); ++pos) {
				if (frac_start == 0) {
					frac_start = pos;
				}
			}
			if (frac_start != 0) {
				string_t frac_string(str + frac_start - 1, UnsafeNumericCast<uint32_t>(pos - frac_start + 1));
				fraction = Cast::Operation<string_t, double>(frac_string);
			}
		}
		if (negative) {
			number = -number;
			fraction = -fraction;
		}
		goto interval_parse_identifier;
	}
interval_parse_time : {
	// parse the remainder of the time as a Time type
	dtime_t time;
	idx_t pos;
	if (!Time::TryConvertInterval(str + start_pos, len - start_pos, pos, time, false, nullptr, negative)) {
		return false;
	}
	if (!IntervalTryAddition<int64_t>(result.micros, time.value, 1, error_message)) {
		return false;
	}
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
	specifier_str = string(str + start_pos, pos - start_pos);

	// Special case SS[.FFFFFF] - implied SECONDS/MICROSECONDS
	if (specifier_str.empty() && !found_any) {
		if (!IntervalTryAddition<int64_t>(result.micros, number, MICROS_PER_SEC, error_message) ||
		    !IntervalTryAddition<int64_t>(result.micros, 0, MICROS_PER_SEC, error_message, fraction)) {
			return false;
		}
		found_any = true;
		// parse any trailing whitespace
		for (; pos < len; pos++) {
			char c = str[pos];
			if (c == ' ' || c == '\t' || c == '\n') {
				continue;
			} else {
				return false;
			}
		}
		goto end_of_string;
	}

	if (!TryGetDatePartSpecifier(specifier_str, specifier)) {
		HandleCastError::AssignError(StringUtil::Format("extract specifier \"%s\" not recognized", specifier_str),
		                             error_message);
		return false;
	}
	// add the specifier to the interval
	switch (specifier) {
	case DatePartSpecifier::MILLENNIUM:
		if (!IntervalTryAddition<int32_t>(result.months, number, MONTHS_PER_MILLENIUM, error_message, fraction)) {
			return false;
		}
		break;
	case DatePartSpecifier::CENTURY:
		if (!IntervalTryAddition<int32_t>(result.months, number, MONTHS_PER_CENTURY, error_message, fraction)) {
			return false;
		}
		break;
	case DatePartSpecifier::DECADE:
		if (!IntervalTryAddition<int32_t>(result.months, number, MONTHS_PER_DECADE, error_message, fraction)) {
			return false;
		}
		break;
	case DatePartSpecifier::YEAR:
		if (!IntervalTryAddition<int32_t>(result.months, number, MONTHS_PER_YEAR, error_message, fraction)) {
			return false;
		}
		break;
	case DatePartSpecifier::QUARTER: {
		if (!IntervalTryAddition<int32_t>(result.months, number, MONTHS_PER_QUARTER, error_message)) {
			return false;
		}
		// Reduce to fraction of a month
		int32_t month = static_cast<int32_t>(fraction * MONTHS_PER_QUARTER);
		if (!IntervalTryAddition<int32_t>(result.months, month, 1, error_message)) {
			return false;
		}
		fraction = fraction * MONTHS_PER_QUARTER - month;
		if (!IntervalTryAddition<int32_t>(result.days, 0, DAYS_PER_MONTH, error_message, fraction)) {
			return false;
		}
		break;
	}
	case DatePartSpecifier::MONTH: {
		if (!IntervalTryAddition<int32_t>(result.months, number, 1, error_message)) {
			return false;
		}
		int32_t day = static_cast<int32_t>(fraction * DAYS_PER_MONTH);
		if (!IntervalTryAddition<int32_t>(result.days, day, 1, error_message)) {
			return false;
		}
		fraction = fraction * DAYS_PER_MONTH - day;
		if (!IntervalTryAddition<int64_t>(result.micros, 0, MICROS_PER_DAY, error_message, fraction)) {
			return false;
		}
		break;
	}
	case DatePartSpecifier::DAY:
		if (!IntervalTryAddition<int32_t>(result.days, number, 1, error_message) ||
		    !IntervalTryAddition<int64_t>(result.micros, 0, MICROS_PER_DAY, error_message, fraction)) {
			return false;
		}
		break;
	case DatePartSpecifier::WEEK: {
		if (!IntervalTryAddition<int32_t>(result.days, number, DAYS_PER_WEEK, error_message)) {
			return false;
		}
		// Reduce to fraction of a day
		int32_t day = static_cast<int32_t>(fraction * DAYS_PER_WEEK);
		if (!IntervalTryAddition<int32_t>(result.days, day, 1, error_message)) {
			return false;
		}
		fraction = fraction * DAYS_PER_WEEK - day;
		if (!IntervalTryAddition<int64_t>(result.micros, 0, MICROS_PER_DAY, error_message, fraction)) {
			return false;
		}
		break;
	}
	case DatePartSpecifier::MICROSECONDS:
		if (!IntervalTryAddition<int64_t>(result.micros, number, 1, error_message)) {
			return false;
		}
		break;
	case DatePartSpecifier::MILLISECONDS:
		if (!IntervalTryAddition<int64_t>(result.micros, number, MICROS_PER_MSEC, error_message, fraction)) {
			return false;
		}
		break;
	case DatePartSpecifier::SECOND:
		if (!IntervalTryAddition<int64_t>(result.micros, number, MICROS_PER_SEC, error_message, fraction)) {
			return false;
		}
		break;
	case DatePartSpecifier::MINUTE:
		if (!IntervalTryAddition<int64_t>(result.micros, number, MICROS_PER_MINUTE, error_message, fraction)) {
			return false;
		}
		break;
	case DatePartSpecifier::HOUR:
		if (!IntervalTryAddition<int64_t>(result.micros, number, MICROS_PER_HOUR, error_message, fraction)) {
			return false;
		}
		break;
	default:
		HandleCastError::AssignError(
		    StringUtil::Format("extract specifier \"%s\" not supported for interval", specifier_str), error_message);
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
	if (result.months == NumericLimits<int32_t>::Minimum() || result.days == NumericLimits<int32_t>::Minimum() ||
	    result.micros == NumericLimits<int64_t>::Minimum()) {
		AssignOutOfRangeErrorOrThrow("AGO interval value is out of range", error_message);
		return false;
	}

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

string Interval::ToString(const interval_t &interval) {
	char buffer[70];
	idx_t length = IntervalToStringCast::Format(interval, buffer);
	return string(buffer, length);
}

int64_t Interval::GetMilli(const interval_t &val) {
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

bool Interval::TryGetMicro(const interval_t &val, int64_t &micro_total) {
	int64_t micro_month, micro_day;
	micro_total = val.micros;
	if (!TryMultiplyOperator::Operation((int64_t)val.months, Interval::MICROS_PER_MONTH, micro_month)) {
		return false;
	}
	if (!TryMultiplyOperator::Operation((int64_t)val.days, Interval::MICROS_PER_DAY, micro_day)) {
		return false;
	}
	if (!TryAddOperator::Operation<int64_t, int64_t, int64_t>(micro_total, micro_month, micro_total)) {
		return false;
	}
	if (!TryAddOperator::Operation<int64_t, int64_t, int64_t>(micro_total, micro_day, micro_total)) {
		return false;
	}

	return true;
}

int64_t Interval::GetMicro(const interval_t &val) {
	int64_t micro_month, micro_day, micro_total;
	micro_total = val.micros;
	if (!TryMultiplyOperator::Operation((int64_t)val.months, MICROS_PER_MONTH, micro_month)) {
		throw ConversionException("Could not convert Month to Microseconds");
	}
	if (!TryMultiplyOperator::Operation((int64_t)val.days, MICROS_PER_DAY, micro_day)) {
		throw ConversionException("Could not convert Day to Microseconds");
	}
	if (!TryAddOperator::Operation<int64_t, int64_t, int64_t>(micro_total, micro_month, micro_total)) {
		throw ConversionException("Could not convert Interval to Microseconds");
	}
	if (!TryAddOperator::Operation<int64_t, int64_t, int64_t>(micro_total, micro_day, micro_total)) {
		throw ConversionException("Could not convert Interval to Microseconds");
	}

	return micro_total;
}

int64_t Interval::GetNanoseconds(const interval_t &val) {
	int64_t nano;
	const auto micro_total = GetMicro(val);
	if (!TryMultiplyOperator::Operation(micro_total, NANOS_PER_MICRO, nano)) {
		throw ConversionException("Could not convert Interval to Nanoseconds");
	}

	return nano;
}

interval_t Interval::GetAge(TimestampComponents ts1, TimestampComponents ts2, bool is_negative) {
	// perform the differences
	auto year_diff = ts1.year - ts2.year;
	auto month_diff = ts1.month - ts2.month;
	auto day_diff = ts1.day - ts2.day;

	auto hour_diff = ts1.hour - ts2.hour;
	auto min_diff = ts1.minute - ts2.minute;
	auto sec_diff = ts1.second - ts2.second;
	auto micros_diff = ts1.microsecond - ts2.microsecond;

	// flip sign if necessary
	bool sign_flipped = false;
	if (is_negative) {
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
		if (is_negative) {
			day_diff += Date::IsLeapYear(ts1.year) ? Date::LEAP_DAYS[ts1.month] : Date::NORMAL_DAYS[ts1.month];
			month_diff--;
		} else {
			day_diff += Date::IsLeapYear(ts2.year) ? Date::LEAP_DAYS[ts2.month] : Date::NORMAL_DAYS[ts2.month];
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
	interval.micros = Time::FromTime(hour_diff, min_diff, sec_diff, micros_diff).value;

	return interval;
}

interval_t Interval::GetAge(timestamp_t timestamp_1, timestamp_t timestamp_2) {
	D_ASSERT(timestamp_1.IsFinite() && timestamp_2.IsFinite());

	auto ts_component1 = Timestamp::GetComponents(timestamp_1);
	auto ts_component2 = Timestamp::GetComponents(timestamp_2);

	return Interval::GetAge(ts_component1, ts_component2, timestamp_1 < timestamp_2);
}

interval_t Interval::GetDifference(timestamp_t timestamp_1, timestamp_t timestamp_2) {
	if (!timestamp_1.IsFinite() || !timestamp_2.IsFinite()) {
		throw InvalidInputException("Cannot subtract infinite timestamps");
	}
	const auto us_1 = Timestamp::GetEpochMicroSeconds(timestamp_1);
	const auto us_2 = Timestamp::GetEpochMicroSeconds(timestamp_2);
	int64_t delta_us;
	if (TrySubtractOperator::Operation(us_1, us_2, delta_us)) {
		return FromMicro(delta_us);
	}
	// the days between two finite timestamps always fit, even when their microseconds do not
	const auto delta = Hugeint::Convert(us_1) - Hugeint::Convert(us_2);
	const auto micros_per_day = Hugeint::Convert(Interval::MICROS_PER_DAY);
	interval_t result;
	result.months = 0;
	if (!Hugeint::TryCast(delta / micros_per_day, result.days) ||
	    !Hugeint::TryCast(delta % micros_per_day, result.micros)) {
		throw ConversionException("Timestamp difference is out of bounds");
	}
	return result;
}

int64_t Interval::GetDifferenceInUnits(int64_t start_us, int64_t end_us, int64_t micros_per_unit) {
	int64_t delta_us;
	if (TrySubtractOperator::Operation(end_us, start_us, delta_us)) {
		return delta_us / micros_per_unit;
	}
	const auto delta = Hugeint::Convert(end_us) - Hugeint::Convert(start_us);
	int64_t units;
	if (!Hugeint::TryCast(delta / Hugeint::Convert(micros_per_unit), units)) {
		throw OutOfRangeException("Overflow in the difference of two timestamps");
	}
	return units;
}

interval_t Interval::FromMicro(int64_t delta_us) {
	interval_t result;
	result.months = 0;
	result.days = UnsafeNumericCast<int32_t>(delta_us / Interval::MICROS_PER_DAY);
	result.micros = delta_us % Interval::MICROS_PER_DAY;

	return result;
}

interval_t Interval::Invert(interval_t interval) {
	if (interval.days == NumericLimits<int32_t>::Minimum()) {
		throw OutOfRangeException("Interval days value out of range");
	}
	if (interval.micros == NumericLimits<int64_t>::Minimum()) {
		throw OutOfRangeException("Interval micros value out of range");
	}
	if (interval.months == NumericLimits<int32_t>::Minimum()) {
		throw OutOfRangeException("Interval months value out of range");
	}
	interval.days = -interval.days;
	interval.micros = -interval.micros;
	interval.months = -interval.months;
	return interval;
}

date_t Interval::Add(date_t left, interval_t right) {
	date_t result;
	if (!TryAdd(left, right, result)) {
		throw OutOfRangeException("Date out of range");
	}
	return result;
}

bool Interval::TryAdd(date_t left, interval_t right, date_t &result) {
	if (!left.IsFinite()) {
		result = left;
		return true;
	}
	result = left;
	if (right.months != 0) {
		int32_t year, month, day;
		Date::Convert(left, year, month, day);
		int32_t year_diff = right.months / Interval::MONTHS_PER_YEAR;
		year += year_diff;
		month += right.months - year_diff * Interval::MONTHS_PER_YEAR;
		if (month > Interval::MONTHS_PER_YEAR) {
			year++;
			month -= Interval::MONTHS_PER_YEAR;
		} else if (month <= 0) {
			year--;
			month += Interval::MONTHS_PER_YEAR;
		}
		if (month < 1 || month > Interval::MONTHS_PER_YEAR) {
			return false;
		}
		day = MinValue<int32_t>(day, Date::MonthDays(year, month));
		if (!Date::TryFromDate(year, month, day, result)) {
			return false;
		}
	}
	if (right.days != 0 && !TryAddOperator::Operation(result.days, right.days, result.days)) {
		return false;
	}
	if (right.micros != 0 &&
	    !TryAddOperator::Operation(result.days, int32_t(right.micros / Interval::MICROS_PER_DAY), result.days)) {
		return false;
	}
	return result.IsFinite();
}

bool Interval::TryAdd(timestamp_t left, interval_t right, timestamp_t &result) {
	if (!left.IsFinite()) {
		result = left;
		return true;
	}
	date_t date;
	dtime_t time;
	Timestamp::Convert(left, date, time);
	date_t new_date;
	if (!TryAdd(date, right, new_date)) {
		return false;
	}
	auto new_time = Interval::Add(time, right, new_date);
	return Timestamp::TryFromDatetime(new_date, new_time, result);
}

dtime_t Interval::Add(dtime_t left, interval_t right, date_t &date) {
	int64_t diff = right.micros - ((right.micros / Interval::MICROS_PER_DAY) * Interval::MICROS_PER_DAY);
	left += diff;
	if (left.value >= Interval::MICROS_PER_DAY) {
		left.value -= Interval::MICROS_PER_DAY;
		date.days++;
	} else if (left.value < 0) {
		left.value += Interval::MICROS_PER_DAY;
		date.days--;
	}
	return left;
}

dtime_tz_t Interval::Add(dtime_tz_t left, interval_t right, date_t &date) {
	return dtime_tz_t(Interval::Add(left.time(), right, date), left.offset());
}

timestamp_t Interval::Add(timestamp_t left, interval_t right) {
	if (!left.IsFinite()) {
		return left;
	}
	date_t date;
	dtime_t time;
	Timestamp::Convert(left, date, time);
	auto new_date = Interval::Add(date, right);
	auto new_time = Interval::Add(time, right, new_date);
	return Timestamp::FromDatetime(new_date, new_time);
}

} // namespace duckdb
