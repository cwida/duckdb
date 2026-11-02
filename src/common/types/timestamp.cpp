#include "duckdb/common/types/timestamp.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/chrono.hpp"
#include "duckdb/common/operator/add.hpp"
#include "duckdb/common/operator/multiply.hpp"
#include "duckdb/common/operator/subtract.hpp"
#include "duckdb/common/exception/conversion_exception.hpp"
#include "duckdb/common/windows.hpp"
#include <ctime>

namespace duckdb {

static_assert(sizeof(timestamp_t) == sizeof(int64_t), "timestamp_t was padded");

static bool CanStartTimestampSuffix(char c) {
	return c == 'Z' || c == '+' || c == '-' || StringUtil::CharacterIsSpace(c);
}

static idx_t TimestampTimeLength(const char *str, idx_t len) {
	idx_t suffix_pos = 0;
	while (suffix_pos < len && StringUtil::CharacterIsSpace(str[suffix_pos])) {
		suffix_pos++;
	}
	while (suffix_pos < len && !CanStartTimestampSuffix(str[suffix_pos])) {
		suffix_pos++;
	}
	return suffix_pos;
}

// timestamp/datetime uses 64 bits, high 32 bits for date and low 32 bits for time
// string format is YYYY-MM-DDThh:mm:ssZ
// T may be a space
// Z is optional
// ISO 8601

TimestampCastResult Timestamp::TryConvertTimestampTZ(const char *str, idx_t len, timestamp_t &result, bool use_offset,
                                                     bool &has_offset, string_t &tz, optional_ptr<int32_t> nanos) {
	idx_t pos;
	date_t date;
	dtime_t time;
	has_offset = false;
	switch (Date::TryConvertDate(str, len, pos, date, has_offset)) {
	case DateCastResult::ERROR_INCORRECT_FORMAT:
		return TimestampCastResult::ERROR_INCORRECT_FORMAT;
	case DateCastResult::ERROR_RANGE:
		return TimestampCastResult::ERROR_RANGE;
	default:
		break;
	}
	if (pos == len) {
		// no time: only a date or special
		if (date == date_t::infinity()) {
			result = timestamp_t::infinity();
			return TimestampCastResult::SUCCESS;
		} else if (date == date_t::ninfinity()) {
			result = timestamp_t::ninfinity();
			return TimestampCastResult::SUCCESS;
		}
		return Timestamp::TryFromDatetime(date, dtime_t(0), result) ? TimestampCastResult::SUCCESS
		                                                            : TimestampCastResult::ERROR_RANGE;
	}
	// try to parse a time field
	if (str[pos] == ' ' || str[pos] == 'T') {
		pos++;
	}
	idx_t time_pos = 0;
	// TryConvertTime may recursively call us, so we use TryConvertInterval.
	// Note that we can't pass strict== true here because we want to process any suffix.
	auto time_len = TimestampTimeLength(str + pos, len - pos);
	if (!Time::TryConvertInterval(str + pos, time_len, time_pos, time, false, nanos) || time_pos != time_len) {
		return TimestampCastResult::ERROR_INCORRECT_FORMAT;
	}
	//	We parsed an interval, so make sure it is in range.
	if (time.value > Interval::MICROS_PER_DAY) {
		return TimestampCastResult::ERROR_RANGE;
	}
	pos += time_pos;
	if (!Timestamp::TryFromDatetime(date, time, result)) {
		return TimestampCastResult::ERROR_RANGE;
	}
	if (pos < len) {
		// skip a "Z" at the end (as per the ISO8601 specs)
		int hh, mm, ss;
		if (str[pos] == 'Z') {
			pos++;
			has_offset = true;
		} else if (Timestamp::TryParseUTCOffset(str, pos, len, hh, mm, ss)) {
			const int64_t delta =
			    hh * Interval::MICROS_PER_HOUR + mm * Interval::MICROS_PER_MINUTE + ss * Interval::MICROS_PER_SEC;
			if (use_offset &&
			    (!TrySubtractOperator::Operation(result.value, delta, result.value) || !result.IsFinite())) {
				return TimestampCastResult::ERROR_RANGE;
			}
			has_offset = true;
		} else {
			// Parse a time zone: / [A-Za-z0-9/_]+/
			if (str[pos++] != ' ') {
				return TimestampCastResult::ERROR_NON_UTC_TIMEZONE;
			}
			auto tz_name = str + pos;
			for (; pos < len && CharacterIsTimeZone(str[pos]); ++pos) {
				continue;
			}
			auto tz_len = str + pos - tz_name;
			if (tz_len) {
				tz = string_t(tz_name, UnsafeNumericCast<uint32_t>(tz_len));
			}
			// Note that the caller must reinterpret the instant we return to the given time zone
		}

		// skip any spaces at the end
		while (pos < len && StringUtil::CharacterIsSpace(str[pos])) {
			pos++;
		}
		if (pos < len) {
			return TimestampCastResult::ERROR_INCORRECT_FORMAT;
		}
	}
	return TimestampCastResult::SUCCESS;
}

TimestampCastResult Timestamp::TryConvertTimestamp(const char *str, idx_t len, timestamp_t &result, bool use_offset,
                                                   optional_ptr<int32_t> nanos, bool strict) {
	string_t tz(nullptr, 0);
	bool has_offset = false;
	auto success = TryConvertTimestampTZ(str, len, result, use_offset, has_offset, tz, nanos);
	if (success != TimestampCastResult::SUCCESS) {
		return success;
	}
	if (tz.GetSize() == 0) {
		// no timezone provided - success!
		if (strict && has_offset) {
			return TimestampCastResult::STRICT_UTC;
		}
		return TimestampCastResult::SUCCESS;
	}
	if (tz.GetSize() == 3) {
		// we can ONLY handle UTC without ICU being loaded
		auto tz_ptr = tz.GetData();
		if ((tz_ptr[0] == 'u' || tz_ptr[0] == 'U') && (tz_ptr[1] == 't' || tz_ptr[1] == 'T') &&
		    (tz_ptr[2] == 'c' || tz_ptr[2] == 'C')) {
			if (strict && has_offset) {
				return TimestampCastResult::STRICT_UTC;
			}
			return TimestampCastResult::SUCCESS;
		}
	}
	// We don't understand TZ without an extension, so fail if one was provided AND we were supposed to use it.
	if (use_offset && !tz.Empty()) {
		return TimestampCastResult::ERROR_NON_UTC_TIMEZONE;
	} else if (strict && !tz.Empty()) {
		return TimestampCastResult::STRICT_UTC;
	} else {
		return TimestampCastResult::SUCCESS;
	}
}

bool Timestamp::TryFromTimestampNanos(timestamp_t input, int32_t nanos, timestamp_ns_t &result) {
	if (!input.IsFinite()) {
		result.value = input.value;
		return true;
	}
	const auto value = hugeint_t(input.value) * hugeint_t(Interval::NANOS_PER_MICRO) + hugeint_t(nanos);
	return Hugeint::TryCast(value, result.value) && result.IsFinite();
}

TimestampCastResult Timestamp::TryConvertTimestamp(const char *str, idx_t len, timestamp_ns_t &result, bool use_offset,
                                                   bool strict) {
	int32_t nanos = 0;
	timestamp_t micros;
	auto success = TryConvertTimestamp(str, len, micros, use_offset, &nanos, strict);
	if (success != TimestampCastResult::SUCCESS) {
		return success;
	}
	if (!TryFromTimestampNanos(micros, nanos, result)) {
		return TimestampCastResult::ERROR_INCORRECT_FORMAT;
	}
	return TimestampCastResult::SUCCESS;
}

string Timestamp::FormatError(const string &str) {
	return StringUtil::Format("invalid timestamp field format: \"%s\", "
	                          "expected format is (YYYY-MM-DD HH:MM[:SS[.US]][±HH[:MM[:SS]]| ZONE])",
	                          str);
}

string Timestamp::UnsupportedTimezoneError(const string &str) {
	return StringUtil::Format("timestamp field value \"%s\" has a timestamp that is not UTC.\nUse the TIMESTAMPTZ type "
	                          "with the ICU extension loaded to handle non-UTC timestamps.",
	                          str);
}

string Timestamp::RangeError(const string &str) {
	return StringUtil::Format("timestamp field value out of range: \"%s\"", str);
}

string Timestamp::FormatError(string_t str) {
	return Timestamp::FormatError(str.GetString());
}

string Timestamp::UnsupportedTimezoneError(string_t str) {
	return Timestamp::UnsupportedTimezoneError(str.GetString());
}

string Timestamp::RangeError(string_t str) {
	return Timestamp::RangeError(str.GetString());
}

timestamp_t Timestamp::FromCString(const char *str, idx_t len, bool use_offset, optional_ptr<int32_t> nanos) {
	timestamp_t result;
	switch (Timestamp::TryConvertTimestamp(str, len, result, use_offset, nanos)) {
	case TimestampCastResult::SUCCESS:
	case TimestampCastResult::STRICT_UTC:
		break;
	case TimestampCastResult::ERROR_NON_UTC_TIMEZONE:
		throw ConversionException(UnsupportedTimezoneError(string(str, len)));
	case TimestampCastResult::ERROR_INCORRECT_FORMAT:
		throw ConversionException(FormatError(string(str, len)));
	case TimestampCastResult::ERROR_RANGE:
		throw ConversionException(RangeError(string(str, len)));
	}
	return result;
}

bool Timestamp::TryParseUTCOffset(const char *str, idx_t &pos, idx_t len, int &hh, int &mm, int &ss, bool strict) {
	hh = 0;
	mm = 0;
	ss = 0;
	idx_t curpos = pos;
	// parse the next 3 characters
	const idx_t minlen = strict ? 2 : 1;
	if (curpos + minlen + 1 > len) {
		// no characters left to parse
		return strict ? false : true;
	}
	char sign_char = str[curpos];
	if (sign_char != '+' && sign_char != '-') {
		// expected either + or -
		return false;
	}
	curpos++;
	if (!StringUtil::CharacterIsDigit(str[curpos])) {
		// expected H
		return false;
	}
	hh = (str[curpos++] - '0');
	if (StringUtil::CharacterIsDigit(str[curpos])) {
		hh = hh * 10 + +(str[curpos++] - '0');
	} else if (strict) {
		// expected HH
		return false;
	}
	if (sign_char == '-') {
		hh = -hh;
	}

	// optional minute specifier: expected either "MM" or ":MM"
	if (curpos >= len) {
		// done, nothing left
		pos = curpos;
		return true;
	}
	const bool colons_used = (str[curpos] == ':');
	if (colons_used) {
		curpos++;
	}
	if (curpos + minlen > len || !StringUtil::CharacterIsDigit(str[curpos])) {
		// no MM specifier
		pos = curpos;
		return colons_used ? false : true;
	}
	// we have an MM specifier: parse it
	mm = (str[curpos++] - '0');
	if (StringUtil::CharacterIsDigit(str[curpos])) {
		mm = mm * 10 + +(str[curpos++] - '0');
	} else if (strict) {
		// expected MM
		return false;
	}
	if (sign_char == '-') {
		mm = -mm;
	}

	// optional seconds specifier: must be ":SS"
	if (curpos >= len || !colons_used || (str[curpos] != ':')) {
		// done, nothing left
		pos = curpos;
		return true;
	}
	// Skip colon and read seconds
	curpos++;
	if (curpos + minlen > len || !StringUtil::CharacterIsDigit(str[curpos])) {
		// no SS specifier
		pos = curpos;
		return colons_used ? false : true;
	}
	// we have an SS specifier: parse it
	ss = (str[curpos++] - '0');
	if (StringUtil::CharacterIsDigit(str[curpos])) {
		ss = ss * 10 + +(str[curpos++] - '0');
	} else if (strict) {
		// expected SS
		return false;
	}
	if (sign_char == '-') {
		ss = -ss;
	}
	pos = curpos;

	return true;
}

timestamp_t Timestamp::FromString(const string &str, bool use_offset) {
	return Timestamp::FromCString(str.c_str(), str.size(), use_offset);
}

string Timestamp::ToString(timestamp_t timestamp) {
	if (!timestamp.IsFinite()) {
		return Date::ToInfinity(timestamp);
	}

	date_t date;
	dtime_t time;
	Timestamp::Convert(timestamp, date, time);
	return Date::ToString(date) + " " + Time::ToString(time);
}

date_t Timestamp::GetDate(timestamp_t timestamp) {
	if (DUCKDB_UNLIKELY(timestamp == timestamp_t::infinity())) {
		return date_t::infinity();
	}
	if (DUCKDB_UNLIKELY(timestamp == timestamp_t::ninfinity())) {
		return date_t::ninfinity();
	}
	return date_t(UnsafeNumericCast<int32_t>((timestamp.value + (timestamp.value < 0)) / Interval::MICROS_PER_DAY -
	                                         (timestamp.value < 0)));
}

date_t Timestamp::GetDateNS(timestamp_ns_t timestamp) {
	if (DUCKDB_UNLIKELY(timestamp == timestamp_ns_t::infinity())) {
		return date_t::infinity();
	}
	if (DUCKDB_UNLIKELY(timestamp == timestamp_ns_t::ninfinity())) {
		return date_t::ninfinity();
	}
	return date_t(UnsafeNumericCast<int32_t>((timestamp.value + (timestamp.value < 0)) / Interval::NANOS_PER_DAY -
	                                         (timestamp.value < 0)));
}

dtime_t Timestamp::GetTime(timestamp_t timestamp) {
	if (!timestamp.IsFinite()) {
		throw ConversionException("Can't get TIME of infinite TIMESTAMP");
	}
	int64_t micros = timestamp.value % Interval::MICROS_PER_DAY;
	if (micros < 0) {
		micros += Interval::MICROS_PER_DAY;
	}
	return dtime_t(micros);
}

dtime_ns_t Timestamp::GetTimeNs(timestamp_ns_t input) {
	if (!input.IsFinite()) {
		throw ConversionException("Can't get TIME_NS of infinite TIMESTAMP");
	}
	auto nanos = input.value % Interval::NANOS_PER_DAY;
	if (nanos < 0) {
		nanos += Interval::NANOS_PER_DAY;
	}
	return dtime_ns_t(nanos);
}

bool Timestamp::TryCombineUnits(int64_t whole, int64_t part, int64_t unit, int64_t &result) {
	int64_t whole_units;
	if (TryMultiplyOperator::Operation<int64_t, int64_t, int64_t>(whole, unit, whole_units)) {
		return TryAddOperator::Operation<int64_t, int64_t, int64_t>(whole_units, part, result);
	}
	if (whole >= 0) {
		return false;
	}
	// start from the next whole unit, which still fits when the sum does
	if (!TryMultiplyOperator::Operation<int64_t, int64_t, int64_t>(whole + 1, unit, whole_units)) {
		return false;
	}
	return TryAddOperator::Operation<int64_t, int64_t, int64_t>(whole_units, part - unit, result);
}

bool Timestamp::TryFromDatetime(date_t date, dtime_t time, timestamp_t &result) {
	if (!TryCombineUnits(date.days, time.value, Interval::MICROS_PER_DAY, result.value)) {
		return false;
	}
	return result.IsFinite();
}

bool Timestamp::TryFromDatetime(date_t date, dtime_tz_t timetz, timestamp_t &result) {
	if (!TryFromDatetime(date, timetz.time(), result)) {
		return false;
	}
	// Offset is in seconds
	const auto offset = int64_t(timetz.offset() * Interval::MICROS_PER_SEC);
	if (!TryAddOperator::Operation(result.value, -offset, result.value)) {
		return false;
	}
	return result.IsFinite();
}

timestamp_t Timestamp::FromDatetime(date_t date, dtime_t time) {
	timestamp_t result;
	if (!TryFromDatetime(date, time, result)) {
		throw ConversionException("Date and time not in timestamp range");
	}
	return result;
}

void Timestamp::Convert(timestamp_t timestamp, date_t &out_date, dtime_t &out_time) {
	out_date = GetDate(timestamp);
	out_time = GetTime(timestamp);
	D_ASSERT(timestamp == Timestamp::FromDatetime(out_date, out_time));
}

void Timestamp::Convert(timestamp_ns_t input, date_t &out_date, dtime_t &out_time, int32_t &out_nanos) {
	out_date = Timestamp::GetDateNS(input);
	auto time_nanos = Timestamp::GetTimeNs(input).value;
	out_time = dtime_t(time_nanos / Interval::NANOS_PER_MICRO);
	out_nanos = UnsafeNumericCast<int32_t>(time_nanos % Interval::NANOS_PER_MICRO);
}

timestamp_t Timestamp::GetCurrentTimestamp() {
	auto now = system_clock::now();
	auto epoch_micros = duration_cast<microseconds>(now.time_since_epoch()).count();
	return FromEpochMicroSeconds(epoch_micros);
}

timestamp_t Timestamp::FromEpochSecondsPossiblyInfinite(int64_t sec) {
	timestamp_t input(sec);
	if (!input.IsFinite()) {
		return input;
	}
	int64_t result;
	if (!TryMultiplyOperator::Operation(sec, Interval::MICROS_PER_SEC, result)) {
		throw ConversionException("Could not convert Timestamp(S) to Timestamp(US)");
	}
	return timestamp_t(result);
}

timestamp_t Timestamp::FromEpochSeconds(int64_t sec) {
	D_ASSERT(timestamp_t(sec).IsFinite());
	return FromEpochSecondsPossiblyInfinite(sec);
}

timestamp_t Timestamp::FromEpochMsPossiblyInfinite(int64_t ms) {
	int64_t result;
	if (!TryMultiplyOperator::Operation(ms, Interval::MICROS_PER_MSEC, result)) {
		throw ConversionException("Could not convert Timestamp(MS) to Timestamp(US)");
	}
	return timestamp_t(result);
}

timestamp_t Timestamp::FromEpochMs(int64_t ms) {
	D_ASSERT(timestamp_ms_t(ms).IsFinite());
	return FromEpochMsPossiblyInfinite(ms);
}

timestamp_t Timestamp::FromEpochMicroSeconds(int64_t micros) {
	return timestamp_t(micros);
}

timestamp_t Timestamp::FromEpochNanoSecondsPossiblyInfinite(int64_t ns) {
	return timestamp_t(ns / Interval::NANOS_PER_MICRO);
}

timestamp_t Timestamp::FromEpochNanoSeconds(int64_t ns) {
	D_ASSERT(timestamp_ns_t(ns).IsFinite());
	return FromEpochNanoSecondsPossiblyInfinite(ns);
}

timestamp_ns_t Timestamp::TimestampNsFromEpochMillis(int64_t millis) {
	D_ASSERT(timestamp_ms_t(millis).IsFinite());
	timestamp_ns_t result;
	if (!TryMultiplyOperator::Operation(millis, Interval::NANOS_PER_MICRO, result.value)) {
		throw ConversionException("Could not convert Timestamp(US) to Timestamp(NS)");
	}
	return result;
}

timestamp_ns_t Timestamp::TimestampNsFromEpochMicros(int64_t micros) {
	D_ASSERT(timestamp_t(micros).IsFinite());
	timestamp_ns_t result;
	if (!TryMultiplyOperator::Operation(micros, Interval::NANOS_PER_MSEC, result.value)) {
		throw ConversionException("Could not convert Timestamp(MS) to Timestamp(NS)");
	}
	return result;
}

int64_t Timestamp::GetEpochSeconds(timestamp_t timestamp) {
	D_ASSERT(timestamp.IsFinite());
	return timestamp.value / Interval::MICROS_PER_SEC;
}

int64_t Timestamp::GetEpochMs(timestamp_t timestamp) {
	D_ASSERT(timestamp.IsFinite());
	return timestamp.value / Interval::MICROS_PER_MSEC;
}

int64_t Timestamp::GetEpochMicroSeconds(timestamp_t timestamp) {
	return timestamp.value;
}

bool Timestamp::TryGetEpochNanoSeconds(timestamp_t timestamp, int64_t &result) {
	D_ASSERT(timestamp.IsFinite());
	if (!TryMultiplyOperator::Operation(timestamp.value, Interval::NANOS_PER_MICRO, result)) {
		return false;
	}
	return true;
}

int64_t Timestamp::GetEpochNanoSeconds(timestamp_t timestamp) {
	int64_t result;
	D_ASSERT(timestamp.IsFinite());
	if (!TryGetEpochNanoSeconds(timestamp, result)) {
		throw ConversionException("Could not convert Timestamp(US) to Timestamp(NS)");
	}
	return result;
}

int64_t Timestamp::GetEpochNanoSeconds(timestamp_ns_t timestamp) {
	D_ASSERT(timestamp.IsFinite());
	return timestamp.value;
}

int64_t Timestamp::GetEpochRounded(timestamp_t input, int64_t power_of_ten) {
	D_ASSERT(input.IsFinite());
	//	Round away from the epoch.
	//	Scale first so we don't overflow.
	const auto scaling = power_of_ten / 2;
	input.value /= scaling;
	if (input.value < 0) {
		--input.value;
	} else {
		++input.value;
	}
	input.value /= 2;
	return input.value;
}

double Timestamp::GetJulianDay(timestamp_t timestamp) {
	double result = double(Timestamp::GetTime(timestamp).value);
	result /= Interval::MICROS_PER_DAY;
	result += double(Date::ExtractJulianDay(Timestamp::GetDate(timestamp)));
	return result;
}

TimestampComponents Timestamp::GetComponents(timestamp_t timestamp) {
	date_t date;
	dtime_t time;

	Convert(timestamp, date, time);

	TimestampComponents result;
	Date::Convert(date, result.year, result.month, result.day);
	Time::Convert(time, result.hour, result.minute, result.second, result.microsecond);
	result.nanosecond = 0;
	return result;
}

time_t Timestamp::ToTimeT(timestamp_t timestamp) {
	auto components = Timestamp::GetComponents(timestamp);
	struct tm tm {};
	tm.tm_year = components.year - 1900;
	tm.tm_mon = components.month - 1;
	tm.tm_mday = components.day;
	tm.tm_hour = components.hour;
	tm.tm_min = components.minute;
	tm.tm_sec = components.second;
	tm.tm_isdst = 0;
	return mktime(&tm);
}

timestamp_t Timestamp::FromTimeT(time_t time) {
#ifdef DUCKDB_WINDOWS
	auto tm = localtime(&time);
#else
	struct tm tm_storage {};
	auto tm = localtime_r(&time, &tm_storage);
#endif
	if (!tm) {
		throw InternalException("FromTimeT failed: null pointer returned");
	}

	int32_t year = tm->tm_year + 1900;
	int32_t month = tm->tm_mon + 1;
	int32_t day = tm->tm_mday;
	int32_t hour = tm->tm_hour;
	int32_t min = tm->tm_min;
	int32_t sec = tm->tm_sec;

	auto dt = Date::FromDate(year, month, day);
	auto t = Time::FromTime(hour, min, sec, 0);
	return FromDatetime(dt, t);
}

} // namespace duckdb
