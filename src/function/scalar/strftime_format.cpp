#include "duckdb/function/scalar/strftime_format.hpp"
#include "duckdb/common/exception/conversion_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/common/types/cast_helpers.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/operator/add.hpp"
#include "duckdb/common/operator/multiply.hpp"

#include <cctype>

namespace duckdb {

static idx_t StrfTimepecifierSize(StrTimeSpecifier specifier) {
	switch (specifier) {
	case StrTimeSpecifier::ABBREVIATED_WEEKDAY_NAME:
	case StrTimeSpecifier::ABBREVIATED_MONTH_NAME:
		return 3;
	case StrTimeSpecifier::WEEKDAY_DECIMAL:
	case StrTimeSpecifier::WEEKDAY_ISO:
		return 1;
	case StrTimeSpecifier::DAY_OF_MONTH_PADDED:
	case StrTimeSpecifier::MONTH_DECIMAL_PADDED:
	case StrTimeSpecifier::YEAR_WITHOUT_CENTURY_PADDED:
	case StrTimeSpecifier::HOUR_24_PADDED:
	case StrTimeSpecifier::HOUR_12_PADDED:
	case StrTimeSpecifier::MINUTE_PADDED:
	case StrTimeSpecifier::SECOND_PADDED:
	case StrTimeSpecifier::AM_PM:
	case StrTimeSpecifier::WEEK_NUMBER_PADDED_SUN_FIRST:
	case StrTimeSpecifier::WEEK_NUMBER_PADDED_MON_FIRST:
	case StrTimeSpecifier::WEEK_NUMBER_ISO:
		return 2;
	case StrTimeSpecifier::NANOSECOND_PADDED:
		return 9;
	case StrTimeSpecifier::MICROSECOND_PADDED:
		return 6;
	case StrTimeSpecifier::MILLISECOND_PADDED:
		return 3;
	case StrTimeSpecifier::DAY_OF_YEAR_PADDED:
		return 3;
	default:
		return 0;
	}
}

//! Years within [0, 9999] are padded to 4 digits, other years are written in full (with their sign)
static idx_t StrfTimeYearLength(int32_t year) {
	if (0 <= year && year <= 9999) {
		return 4;
	}
	return UnsafeNumericCast<idx_t>(NumericHelper::SignedLength<int32_t, uint32_t>(year));
}

static void StrfTimeSplitOffset(int offset, int &hh, int &mm, int &ss) {
	hh = offset / Interval::SECS_PER_HOUR;
	offset = offset % Interval::SECS_PER_HOUR;

	mm = offset / Interval::SECS_PER_MINUTE;
	ss = offset % Interval::SECS_PER_MINUTE;
}

void StrTimeFormat::AddLiteral(string literal) {
	constant_size += literal.size();
	literals.push_back(std::move(literal));
}

void StrTimeFormat::AddFormatSpecifier(string preceding_literal, StrTimeSpecifier specifier) {
	AddLiteral(std::move(preceding_literal));
	specifiers.push_back(specifier);
}

void StrfTimeFormat::AddFormatSpecifier(string preceding_literal, StrTimeSpecifier specifier) {
	is_date_specifier.push_back(IsDateSpecifier(specifier));
	idx_t specifier_size = StrfTimepecifierSize(specifier);
	if (specifier_size == 0) {
		// variable length specifier
		var_length_specifiers.push_back(specifier);
	} else {
		// constant size specifier
		constant_size += specifier_size;
	}
	StrTimeFormat::AddFormatSpecifier(std::move(preceding_literal), specifier);
}

idx_t StrfTimeFormat::GetSpecifierLength(StrTimeSpecifier specifier, date_t date, int32_t data[8],
                                         const char *tz_name) {
	switch (specifier) {
	case StrTimeSpecifier::FULL_WEEKDAY_NAME:
		return Date::DAY_NAMES[Date::ExtractISODayOfTheWeek(date) % 7].GetSize();
	case StrTimeSpecifier::FULL_MONTH_NAME:
		return Date::MONTH_NAMES[data[1] - 1].GetSize();
	case StrTimeSpecifier::YEAR_DECIMAL:
		return StrfTimeYearLength(data[0]);
	case StrTimeSpecifier::YEAR_ISO:
		return StrfTimeYearLength(Date::ExtractISOYearNumber(date));
	case StrTimeSpecifier::MONTH_DECIMAL: {
		idx_t len = 1;
		auto month = data[1];
		len += month >= 10;
		return len;
	}
	case StrTimeSpecifier::UTC_OFFSET: {
		// ±HH or ±HH:MM or ±HH:MM:SS
		int hh, mm, ss;
		StrfTimeSplitOffset(data[7], hh, mm, ss);
		return ss ? 9 : (mm ? 6 : 3);
	}
	case StrTimeSpecifier::TZ_NAME:
		if (tz_name) {
			return strlen(tz_name);
		}
		// empty for now
		return 0;
	case StrTimeSpecifier::HOUR_24_DECIMAL:
	case StrTimeSpecifier::HOUR_12_DECIMAL:
	case StrTimeSpecifier::MINUTE_DECIMAL:
	case StrTimeSpecifier::SECOND_DECIMAL: {
		// time specifiers
		idx_t len = 1;
		int32_t hour = data[3], min = data[4], sec = data[5];
		switch (specifier) {
		case StrTimeSpecifier::HOUR_24_DECIMAL:
			len += hour >= 10;
			break;
		case StrTimeSpecifier::HOUR_12_DECIMAL:
			hour = hour % 12;
			if (hour == 0) {
				hour = 12;
			}
			len += hour >= 10;
			break;
		case StrTimeSpecifier::MINUTE_DECIMAL:
			len += min >= 10;
			break;
		case StrTimeSpecifier::SECOND_DECIMAL:
			len += sec >= 10;
			break;
		default:
			throw InternalException("Time specifier mismatch");
		}
		return len;
	}
	case StrTimeSpecifier::DAY_OF_MONTH:
		return UnsafeNumericCast<idx_t>(NumericHelper::UnsignedLength<uint32_t>(UnsafeNumericCast<uint32_t>(data[2])));
	case StrTimeSpecifier::DAY_OF_YEAR_DECIMAL:
		return UnsafeNumericCast<idx_t>(
		    NumericHelper::UnsignedLength<uint32_t>(UnsafeNumericCast<uint32_t>(Date::ExtractDayOfTheYear(date))));
	case StrTimeSpecifier::YEAR_WITHOUT_CENTURY:
		return UnsafeNumericCast<idx_t>(
		    NumericHelper::UnsignedLength<uint32_t>(UnsafeNumericCast<uint32_t>(AbsValue(data[0]) % 100)));
	default:
		throw InternalException("Unimplemented specifier for GetSpecifierLength");
	}
}

//! Returns the total length of the date formatted by this format specifier
idx_t StrfTimeFormat::GetLength(date_t date, int32_t data[8], const char *tz_name) const {
	idx_t size = constant_size;
	if (!var_length_specifiers.empty()) {
		for (auto &specifier : var_length_specifiers) {
			size += GetSpecifierLength(specifier, date, data, tz_name);
		}
	}
	return size;
}

idx_t StrfTimeFormat::GetLength(date_t date, dtime_t time, int32_t utc_offset, const char *tz_name) {
	if (!var_length_specifiers.empty()) {
		int32_t data[8];
		Date::Convert(date, data[0], data[1], data[2]);
		Time::Convert(time, data[3], data[4], data[5], data[6]);
		data[6] *= Interval::NANOS_PER_MICRO;
		data[7] = utc_offset;
		return GetLength(date, data, tz_name);
	}
	return constant_size;
}

char *StrfTimeFormat::WriteString(char *target, const string_t &str) const {
	idx_t size = str.GetSize();
	memcpy(target, str.GetData(), size);
	return target + size;
}

// write a value in the range of 0..99 unpadded (e.g. "1", "2", ... "98", "99")
char *StrfTimeFormat::Write2(char *target, uint8_t value) const {
	D_ASSERT(value < 100);
	if (value >= 10) {
		return WritePadded2(target, value);
	} else {
		*target = char(uint8_t('0') + value);
		return target + 1;
	}
}

// write a value in the range of 0..99 padded to 2 digits
char *StrfTimeFormat::WritePadded2(char *target, uint32_t value) const {
	D_ASSERT(value < 100);
	auto index = static_cast<unsigned>(value * 2);
	*target++ = duckdb_fmt::internal::data::digits[index];
	*target++ = duckdb_fmt::internal::data::digits[index + 1];
	return target;
}

// write a value in the range of 0..999 padded
char *StrfTimeFormat::WritePadded3(char *target, uint32_t value) const {
	D_ASSERT(value < 1000);
	if (value >= 100) {
		WritePadded2(target + 1, value % 100);
		*target = char(uint8_t('0') + value / 100);
		return target + 3;
	} else {
		*target = '0';
		target++;
		return WritePadded2(target, value);
	}
}

// write a year padded to 4 digits if it is within [0, 9999], or in full (with its sign) otherwise
char *StrfTimeFormat::WriteYear(char *target, int32_t year) const {
	if (year >= 0 && year <= 9999) {
		return WritePadded(target, UnsafeNumericCast<uint32_t>(year), 4);
	}
	auto abs_year = year < 0 ? -int64_t(year) : int64_t(year);
	if (year < 0) {
		*target = '-';
		target++;
	}
	auto len = NumericHelper::UnsignedLength<uint32_t>(UnsafeNumericCast<uint32_t>(abs_year));
	NumericHelper::FormatUnsigned(UnsafeNumericCast<uint32_t>(abs_year), target + len);
	return target + len;
}

// write a value in the range of 0..999999... padded to the given number of digits
char *StrfTimeFormat::WritePadded(char *target, uint32_t value, size_t padding) const {
	D_ASSERT(padding > 1);
	auto end = target + padding;
	if (padding % 2) {
		uint32_t decimals = value % 1000u;
		WritePadded3(target + padding - 3, decimals);
		value /= 1000;
		padding -= 3;
	}
	for (size_t i = 0; i < padding / 2; i++) {
		uint32_t decimals = value % 100u;
		WritePadded2(target + padding - 2 * (i + 1), decimals);
		value /= 100;
	}
	return end;
}

bool StrfTimeFormat::IsDateSpecifier(StrTimeSpecifier specifier) {
	switch (specifier) {
	case StrTimeSpecifier::ABBREVIATED_WEEKDAY_NAME:
	case StrTimeSpecifier::FULL_WEEKDAY_NAME:
	case StrTimeSpecifier::DAY_OF_YEAR_PADDED:
	case StrTimeSpecifier::DAY_OF_YEAR_DECIMAL:
	case StrTimeSpecifier::WEEK_NUMBER_PADDED_MON_FIRST:
	case StrTimeSpecifier::WEEK_NUMBER_PADDED_SUN_FIRST:
	case StrTimeSpecifier::WEEK_NUMBER_ISO:
	case StrTimeSpecifier::WEEKDAY_DECIMAL:
	case StrTimeSpecifier::WEEKDAY_ISO:
	case StrTimeSpecifier::YEAR_ISO:
		return true;
	default:
		return false;
	}
}

char *StrfTimeFormat::WriteDateSpecifier(StrTimeSpecifier specifier, date_t date, char *target) const {
	switch (specifier) {
	case StrTimeSpecifier::ABBREVIATED_WEEKDAY_NAME: {
		auto dow = Date::ExtractISODayOfTheWeek(date);
		target = WriteString(target, Date::DAY_NAMES_ABBREVIATED[dow % 7]);
		break;
	}
	case StrTimeSpecifier::FULL_WEEKDAY_NAME: {
		auto dow = Date::ExtractISODayOfTheWeek(date);
		target = WriteString(target, Date::DAY_NAMES[dow % 7]);
		break;
	}
	case StrTimeSpecifier::WEEKDAY_DECIMAL: {
		auto dow = Date::ExtractISODayOfTheWeek(date);
		*target = char('0' + uint8_t(dow % 7));
		target++;
		break;
	}
	case StrTimeSpecifier::DAY_OF_YEAR_PADDED: {
		int32_t doy = Date::ExtractDayOfTheYear(date);
		target = WritePadded3(target, UnsafeNumericCast<uint32_t>(doy));
		break;
	}
	case StrTimeSpecifier::WEEK_NUMBER_PADDED_MON_FIRST:
		target = WritePadded2(target, UnsafeNumericCast<uint32_t>(Date::ExtractWeekNumberRegular(date, true)));
		break;
	case StrTimeSpecifier::WEEK_NUMBER_PADDED_SUN_FIRST:
		target = WritePadded2(target, UnsafeNumericCast<uint32_t>(Date::ExtractWeekNumberRegular(date, false)));
		break;
	case StrTimeSpecifier::WEEK_NUMBER_ISO:
		target = WritePadded2(target, UnsafeNumericCast<uint32_t>(Date::ExtractISOWeekNumber(date)));
		break;
	case StrTimeSpecifier::DAY_OF_YEAR_DECIMAL: {
		auto doy = UnsafeNumericCast<uint32_t>(Date::ExtractDayOfTheYear(date));
		target += NumericHelper::UnsignedLength<uint32_t>(doy);
		NumericHelper::FormatUnsigned(doy, target);
		break;
	}
	case StrTimeSpecifier::YEAR_ISO:
		target = WriteYear(target, Date::ExtractISOYearNumber(date));
		break;
	case StrTimeSpecifier::WEEKDAY_ISO:
		*target = char('0' + uint8_t(Date::ExtractISODayOfTheWeek(date)));
		target++;
		break;
	default:
		throw InternalException("Unimplemented date specifier for strftime");
	}
	return target;
}

char *StrfTimeFormat::WriteStandardSpecifier(StrTimeSpecifier specifier, int32_t data[], const char *tz_name,
                                             size_t tz_len, char *target) const {
	// data contains [0] year, [1] month, [2] day, [3] hour, [4] minute, [5] second, [6] ns, [7] utc (secs)
	switch (specifier) {
	case StrTimeSpecifier::DAY_OF_MONTH_PADDED:
		target = WritePadded2(target, UnsafeNumericCast<uint32_t>(data[2]));
		break;
	case StrTimeSpecifier::ABBREVIATED_MONTH_NAME: {
		auto &month_name = Date::MONTH_NAMES_ABBREVIATED[data[1] - 1];
		return WriteString(target, month_name);
	}
	case StrTimeSpecifier::FULL_MONTH_NAME: {
		auto &month_name = Date::MONTH_NAMES[data[1] - 1];
		return WriteString(target, month_name);
	}
	case StrTimeSpecifier::MONTH_DECIMAL_PADDED:
		target = WritePadded2(target, UnsafeNumericCast<uint32_t>(data[1]));
		break;
	case StrTimeSpecifier::YEAR_WITHOUT_CENTURY_PADDED:
		target = WritePadded2(target, UnsafeNumericCast<uint32_t>(AbsValue(data[0]) % 100));
		break;
	case StrTimeSpecifier::YEAR_DECIMAL:
		target = WriteYear(target, data[0]);
		break;
	case StrTimeSpecifier::HOUR_24_PADDED: {
		target = WritePadded2(target, UnsafeNumericCast<uint32_t>(data[3]));
		break;
	}
	case StrTimeSpecifier::HOUR_12_PADDED: {
		int hour = data[3] % 12;
		if (hour == 0) {
			hour = 12;
		}
		target = WritePadded2(target, UnsafeNumericCast<uint32_t>(hour));
		break;
	}
	case StrTimeSpecifier::AM_PM:
		*target++ = data[3] >= 12 ? 'P' : 'A';
		*target++ = 'M';
		break;
	case StrTimeSpecifier::MINUTE_PADDED: {
		target = WritePadded2(target, UnsafeNumericCast<uint32_t>(data[4]));
		break;
	}
	case StrTimeSpecifier::SECOND_PADDED:
		target = WritePadded2(target, UnsafeNumericCast<uint32_t>(data[5]));
		break;
	case StrTimeSpecifier::NANOSECOND_PADDED:
		target = WritePadded(target, UnsafeNumericCast<uint32_t>(data[6]), 9);
		break;
	case StrTimeSpecifier::MICROSECOND_PADDED:
		target = WritePadded(target, UnsafeNumericCast<uint32_t>(data[6] / Interval::NANOS_PER_MICRO), 6);
		break;
	case StrTimeSpecifier::MILLISECOND_PADDED:
		target = WritePadded3(target, UnsafeNumericCast<uint32_t>(data[6] / Interval::NANOS_PER_MSEC));
		break;
	case StrTimeSpecifier::UTC_OFFSET: {
		*target++ = (data[7] < 0) ? '-' : '+';

		auto offset = abs(data[7]);
		int hh, mm, ss;
		StrfTimeSplitOffset(offset, hh, mm, ss);
		target = WritePadded2(target, UnsafeNumericCast<uint32_t>(hh));
		if (mm || ss) {
			*target++ = ':';
			target = WritePadded2(target, UnsafeNumericCast<uint32_t>(mm));
		}
		if (ss) {
			*target++ = ':';
			target = WritePadded2(target, UnsafeNumericCast<uint32_t>(ss));
		}
		break;
	}
	case StrTimeSpecifier::TZ_NAME:
		if (tz_name) {
			memcpy(target, tz_name, tz_len);
			target += strlen(tz_name);
		}
		break;
	case StrTimeSpecifier::DAY_OF_MONTH: {
		target = Write2(target, UnsafeNumericCast<uint8_t>(data[2] % 100));
		break;
	}
	case StrTimeSpecifier::MONTH_DECIMAL: {
		target = Write2(target, UnsafeNumericCast<uint8_t>(data[1]));
		break;
	}
	case StrTimeSpecifier::YEAR_WITHOUT_CENTURY: {
		target = Write2(target, UnsafeNumericCast<uint8_t>(AbsValue(data[0]) % 100));
		break;
	}
	case StrTimeSpecifier::HOUR_24_DECIMAL: {
		target = Write2(target, UnsafeNumericCast<uint8_t>(data[3]));
		break;
	}
	case StrTimeSpecifier::HOUR_12_DECIMAL: {
		int hour = data[3] % 12;
		if (hour == 0) {
			hour = 12;
		}
		target = Write2(target, UnsafeNumericCast<uint8_t>(hour));
		break;
	}
	case StrTimeSpecifier::MINUTE_DECIMAL: {
		target = Write2(target, UnsafeNumericCast<uint8_t>(data[4]));
		break;
	}
	case StrTimeSpecifier::SECOND_DECIMAL: {
		target = Write2(target, UnsafeNumericCast<uint8_t>(data[5]));
		break;
	}
	default:
		throw InternalException("Unimplemented specifier for WriteStandardSpecifier in strftime");
	}
	return target;
}

void StrfTimeFormat::FormatStringNS(date_t date, int32_t data[8], const char *tz_name, char *target) const {
	D_ASSERT(specifiers.size() + 1 == literals.size());
	idx_t i;
	for (i = 0; i < specifiers.size(); i++) {
		// first copy the current literal
		memcpy(target, literals[i].c_str(), literals[i].size());
		target += literals[i].size();
		// now copy the specifier
		if (is_date_specifier[i]) {
			target = WriteDateSpecifier(specifiers[i], date, target);
		} else {
			auto tz_len = tz_name ? strlen(tz_name) : 0;
			target = WriteStandardSpecifier(specifiers[i], data, tz_name, tz_len, target);
		}
	}
	// copy the final literal into the target
	memcpy(target, literals[i].c_str(), literals[i].size());
}

void StrfTimeFormat::FormatString(date_t date, int32_t data[8], const char *tz_name, char *target) {
	data[6] *= Interval::NANOS_PER_MICRO;
	FormatStringNS(date, data, tz_name, target);
	data[6] /= Interval::NANOS_PER_MICRO;
}

void StrfTimeFormat::FormatString(date_t date, dtime_t time, char *target) {
	int32_t data[8]; // year, month, day, hour, min, sec, µs, offset
	Date::Convert(date, data[0], data[1], data[2]);
	Time::Convert(time, data[3], data[4], data[5], data[6]);
	data[7] = 0;

	FormatString(date, data, nullptr, target);
}

string StrfTimeFormat::Format(timestamp_t timestamp, const string &format_str) {
	StrfTimeFormat format;
	format.ParseFormatSpecifier(format_str, format);

	auto date = Timestamp::GetDate(timestamp);
	auto time = Timestamp::GetTime(timestamp);

	auto len = format.GetLength(date, time, 0, nullptr);
	auto result = make_unsafe_uniq_array_uninitialized<char>(len);
	format.FormatString(date, time, result.get());
	return string(result.get(), len);
}

string StrTimeFormat::ParseFormatSpecifier(const string &format_string, StrTimeFormat &format) {
	if (format_string.empty()) {
		return "Empty format string";
	}
	format.format_specifier = format_string;
	format.specifiers.clear();
	format.literals.clear();
	format.numeric_width.clear();
	format.constant_size = 0;
	idx_t pos = 0;
	string current_literal;
	for (idx_t i = 0; i < format_string.size(); i++) {
		if (format_string[i] == '%') {
			if (i + 1 == format_string.size()) {
				return "Trailing format character %";
			}
			if (i > pos) {
				// push the previous string to the current literal
				current_literal += format_string.substr(pos, i - pos);
			}
			char format_char = format_string[++i];
			if (format_char == '%') {
				// special case: %%
				// set the pos for the next literal and continue
				pos = i;
				continue;
			}
			StrTimeSpecifier specifier;
			if (format_char == '-' && i + 1 < format_string.size()) {
				format_char = format_string[++i];
				switch (format_char) {
				case 'd':
					specifier = StrTimeSpecifier::DAY_OF_MONTH;
					break;
				case 'm':
					specifier = StrTimeSpecifier::MONTH_DECIMAL;
					break;
				case 'y':
					specifier = StrTimeSpecifier::YEAR_WITHOUT_CENTURY;
					break;
				case 'H':
					specifier = StrTimeSpecifier::HOUR_24_DECIMAL;
					break;
				case 'I':
					specifier = StrTimeSpecifier::HOUR_12_DECIMAL;
					break;
				case 'M':
					specifier = StrTimeSpecifier::MINUTE_DECIMAL;
					break;
				case 'S':
					specifier = StrTimeSpecifier::SECOND_DECIMAL;
					break;
				case 'j':
					specifier = StrTimeSpecifier::DAY_OF_YEAR_DECIMAL;
					break;
				default:
					return "Unrecognized format for strftime/strptime: %-" + string(1, format_char);
				}
			} else {
				switch (format_char) {
				case 'a':
					specifier = StrTimeSpecifier::ABBREVIATED_WEEKDAY_NAME;
					break;
				case 'A':
					specifier = StrTimeSpecifier::FULL_WEEKDAY_NAME;
					break;
				case 'w':
					specifier = StrTimeSpecifier::WEEKDAY_DECIMAL;
					break;
				case 'u':
					specifier = StrTimeSpecifier::WEEKDAY_ISO;
					break;
				case 'd':
					specifier = StrTimeSpecifier::DAY_OF_MONTH_PADDED;
					break;
				case 'h':
				case 'b':
					specifier = StrTimeSpecifier::ABBREVIATED_MONTH_NAME;
					break;
				case 'B':
					specifier = StrTimeSpecifier::FULL_MONTH_NAME;
					break;
				case 'm':
					specifier = StrTimeSpecifier::MONTH_DECIMAL_PADDED;
					break;
				case 'y':
					specifier = StrTimeSpecifier::YEAR_WITHOUT_CENTURY_PADDED;
					break;
				case 'Y':
					specifier = StrTimeSpecifier::YEAR_DECIMAL;
					break;
				case 'G':
					specifier = StrTimeSpecifier::YEAR_ISO;
					break;
				case 'H':
					specifier = StrTimeSpecifier::HOUR_24_PADDED;
					break;
				case 'I':
					specifier = StrTimeSpecifier::HOUR_12_PADDED;
					break;
				case 'p':
					specifier = StrTimeSpecifier::AM_PM;
					break;
				case 'M':
					specifier = StrTimeSpecifier::MINUTE_PADDED;
					break;
				case 'S':
					specifier = StrTimeSpecifier::SECOND_PADDED;
					break;
				case 'n':
					specifier = StrTimeSpecifier::NANOSECOND_PADDED;
					break;
				case 'f':
					specifier = StrTimeSpecifier::MICROSECOND_PADDED;
					break;
				case 'g':
					specifier = StrTimeSpecifier::MILLISECOND_PADDED;
					break;
				case 'z':
					specifier = StrTimeSpecifier::UTC_OFFSET;
					break;
				case 'Z':
					specifier = StrTimeSpecifier::TZ_NAME;
					break;
				case 'j':
					specifier = StrTimeSpecifier::DAY_OF_YEAR_PADDED;
					break;
				case 'U':
					specifier = StrTimeSpecifier::WEEK_NUMBER_PADDED_SUN_FIRST;
					break;
				case 'W':
					specifier = StrTimeSpecifier::WEEK_NUMBER_PADDED_MON_FIRST;
					break;
				case 'V':
					specifier = StrTimeSpecifier::WEEK_NUMBER_ISO;
					break;
				case 'c':
				case 'x':
				case 'X':
				case 'T': {
					string subformat;
					if (format_char == 'c') {
						// %c: Locale’s appropriate date and time representation.
						// we push the ISO timestamp representation here
						subformat = "%Y-%m-%d %H:%M:%S";
					} else if (format_char == 'x') {
						// %x - Locale’s appropriate date representation.
						// we push the ISO date format here
						subformat = "%Y-%m-%d";
					} else if (format_char == 'X' || format_char == 'T') {
						// %X - Locale’s appropriate time representation.
						// we push the ISO time format here
						subformat = "%H:%M:%S";
					}
					// parse the subformat in a separate format specifier
					StrfTimeFormat locale_format;
					string error = StrTimeFormat::ParseFormatSpecifier(subformat, locale_format);
					if (!error.empty()) {
						throw InternalException("Failed to bind sub-format specifier \"%s\": %s", subformat, error);
					}
					// add the previous literal to the first literal of the subformat
					locale_format.literals[0] = std::move(current_literal) + locale_format.literals[0];
					current_literal = "";
					// now push the subformat into the current format specifier
					for (idx_t i = 0; i < locale_format.specifiers.size(); i++) {
						format.AddFormatSpecifier(std::move(locale_format.literals[i]), locale_format.specifiers[i]);
					}
					pos = i + 1;
					continue;
				}
				default:
					return "Unrecognized format for strftime/strptime: %" + string(1, format_char);
				}
			}
			format.AddFormatSpecifier(std::move(current_literal), specifier);
			current_literal = "";
			pos = i + 1;
		}
	}
	// add the final literal
	if (pos < format_string.size()) {
		current_literal += format_string.substr(pos, format_string.size() - pos);
	}
	format.AddLiteral(std::move(current_literal));
	return string();
}

void StrfTimeFormat::ConvertDateVector(const Vector &input, Vector &result) {
	D_ASSERT(input.GetType().id() == LogicalTypeId::DATE);
	D_ASSERT(result.GetType().id() == LogicalTypeId::VARCHAR);
	auto &heap = StringVector::GetStringHeap(result);
	UnaryExecutor::Execute<date_t, string_t>(input, result, [&](date_t input) {
		if (input.IsFinite()) {
			dtime_t time(0);
			idx_t len = GetLength(input, time, 0, nullptr);
			string_t target = heap.EmptyString(len);
			FormatString(input, time, target.GetDataWriteable());
			target.Finalize();
			return target;
		} else {
			return heap.AddString(Date::ToString(input));
		}
	});
}

string_t StrfTimeFormat::ConvertTimestampValue(const timestamp_t &input, StringHeap &heap) const {
	if (input.IsFinite()) {
		date_t date;
		dtime_t time;
		Timestamp::Convert(input, date, time);

		int32_t data[8]; // year, month, day, hour, min, sec, ns, offset
		Date::Convert(date, data[0], data[1], data[2]);
		Time::Convert(time, data[3], data[4], data[5], data[6]);
		data[6] *= Interval::NANOS_PER_MICRO;
		data[7] = 0;
		const char *tz_name = nullptr;

		idx_t len = GetLength(date, data, tz_name);
		string_t target = heap.EmptyString(len);
		FormatStringNS(date, data, tz_name, target.GetDataWriteable());
		target.Finalize();
		return target;
	} else {
		return heap.AddString(Timestamp::ToString(input));
	}
}

string_t StrfTimeFormat::ConvertTimestampValue(const timestamp_ns_t &input, StringHeap &heap) const {
	if (input.IsFinite()) {
		date_t date;
		dtime_t time;
		int32_t nanos;
		Timestamp::Convert(input, date, time, nanos);

		int32_t data[8]; // year, month, day, hour, min, sec, ns, offset
		Date::Convert(date, data[0], data[1], data[2]);
		Time::Convert(time, data[3], data[4], data[5], data[6]);
		data[6] *= Interval::NANOS_PER_MICRO;
		data[6] += nanos;
		data[7] = 0;
		const char *tz_name = nullptr;

		idx_t len = GetLength(date, data, tz_name);
		string_t target = heap.EmptyString(len);
		FormatStringNS(date, data, tz_name, target.GetDataWriteable());
		target.Finalize();
		return target;
	} else {
		return heap.AddString(Timestamp::ToString(timestamp_t(input.value)));
	}
}

void StrfTimeFormat::ConvertTimestampVector(const Vector &input, Vector &result) {
	D_ASSERT(input.GetType().id() == LogicalTypeId::TIMESTAMP || input.GetType().id() == LogicalTypeId::TIMESTAMP_TZ);
	D_ASSERT(result.GetType().id() == LogicalTypeId::VARCHAR);
	auto &heap = StringVector::GetStringHeap(result);
	UnaryExecutor::Execute<timestamp_t, string_t>(input, result,
	                                              [&](timestamp_t ts) { return ConvertTimestampValue(ts, heap); });
}

void StrfTimeFormat::ConvertTimestampNSVector(const Vector &input, Vector &result) {
	D_ASSERT(input.GetType().id() == LogicalTypeId::TIMESTAMP_NS ||
	         input.GetType().id() == LogicalTypeId::TIMESTAMP_TZ_NS);
	D_ASSERT(result.GetType().id() == LogicalTypeId::VARCHAR);
	auto &heap = StringVector::GetStringHeap(result);
	UnaryExecutor::Execute<timestamp_ns_t, string_t>(
	    input, result, [&](timestamp_ns_t ts) { return ConvertTimestampValue(ts, heap); });
}

void StrpTimeFormat::AddFormatSpecifier(string preceding_literal, StrTimeSpecifier specifier) {
	numeric_width.push_back(NumericSpecifierWidth(specifier));
	StrTimeFormat::AddFormatSpecifier(std::move(preceding_literal), specifier);
}

int StrpTimeFormat::NumericSpecifierWidth(StrTimeSpecifier specifier) {
	switch (specifier) {
	case StrTimeSpecifier::WEEKDAY_DECIMAL:
	case StrTimeSpecifier::WEEKDAY_ISO:
		return 1;
	case StrTimeSpecifier::DAY_OF_MONTH_PADDED:
	case StrTimeSpecifier::DAY_OF_MONTH:
	case StrTimeSpecifier::MONTH_DECIMAL_PADDED:
	case StrTimeSpecifier::MONTH_DECIMAL:
	case StrTimeSpecifier::YEAR_WITHOUT_CENTURY_PADDED:
	case StrTimeSpecifier::YEAR_WITHOUT_CENTURY:
	case StrTimeSpecifier::HOUR_24_PADDED:
	case StrTimeSpecifier::HOUR_24_DECIMAL:
	case StrTimeSpecifier::HOUR_12_PADDED:
	case StrTimeSpecifier::HOUR_12_DECIMAL:
	case StrTimeSpecifier::MINUTE_PADDED:
	case StrTimeSpecifier::MINUTE_DECIMAL:
	case StrTimeSpecifier::SECOND_PADDED:
	case StrTimeSpecifier::SECOND_DECIMAL:
	case StrTimeSpecifier::WEEK_NUMBER_PADDED_SUN_FIRST:
	case StrTimeSpecifier::WEEK_NUMBER_PADDED_MON_FIRST:
	case StrTimeSpecifier::WEEK_NUMBER_ISO:
		return 2;
	case StrTimeSpecifier::MILLISECOND_PADDED:
	case StrTimeSpecifier::DAY_OF_YEAR_PADDED:
	case StrTimeSpecifier::DAY_OF_YEAR_DECIMAL:
		return 3;
	case StrTimeSpecifier::YEAR_DECIMAL:
	case StrTimeSpecifier::YEAR_ISO:
		return 4;
	case StrTimeSpecifier::MICROSECOND_PADDED:
		return 6;
	case StrTimeSpecifier::NANOSECOND_PADDED:
		return 9;
	default:
		return -1;
	}
}

enum class TimeSpecifierAMOrPM : uint8_t { TIME_SPECIFIER_NONE = 0, TIME_SPECIFIER_AM = 1, TIME_SPECIFIER_PM = 2 };

int32_t StrpTimeFormat::TryParseCollection(const char *data, idx_t &pos, idx_t size, const string_t collection[],
                                           idx_t collection_count) const {
	for (idx_t c = 0; c < collection_count; c++) {
		auto &entry = collection[c];
		auto entry_data = entry.GetData();
		auto entry_size = entry.GetSize();
		// check if this entry matches
		if (pos + entry_size > size) {
			// too big: can't match
			continue;
		}
		// compare the characters
		idx_t i;
		for (i = 0; i < entry_size; i++) {
			if (StringUtil::CharacterToLower(entry_data[i]) != StringUtil::CharacterToLower(data[pos + i])) {
				break;
			}
		}
		if (i == entry_size) {
			// full match
			pos += entry_size;
			return UnsafeNumericCast<int32_t>(c);
		}
	}
	return -1;
}

bool StrpTimeFormat::Parse(const char *data, size_t size, ParseResult &result, bool strict) const {
	auto &result_data = result.data;
	auto &error_message = result.error_message;
	auto &error_position = result.error_position;

	// initialize the result
	result_data[0] = 1900;
	result_data[1] = 1;
	result_data[2] = 1;
	result_data[3] = 0;
	result_data[4] = 0;
	result_data[5] = 0;
	result_data[6] = 0;
	result_data[7] = 0;
	// skip leading spaces
	while (size > 0 && StringUtil::CharacterIsSpace(*data)) {
		data++;
		size--;
	}
	//	Check for specials
	//	Precheck for alphas for performance.
	idx_t pos = 0;
	result.is_special = false;
	if (size > 4) {
		if (StringUtil::CharacterIsAlpha(*data)) {
			if (Date::TryConvertDateSpecial(data, size, pos, Date::PINF)) {
				result.is_special = true;
				result.special = date_t::infinity();
			} else if (Date::TryConvertDateSpecial(data, size, pos, Date::EPOCH)) {
				result.is_special = true;
				result.special = date_t::epoch();
			}
		} else if (*data == '-' && Date::TryConvertDateSpecial(data, size, pos, Date::NINF)) {
			result.is_special = true;
			result.special = date_t::ninfinity();
		}
	}
	if (result.is_special) {
		// skip trailing spaces
		while (pos < size && StringUtil::CharacterIsSpace(data[pos])) {
			pos++;
		}
		if (pos != size) {
			error_message = "Special timestamp did not match: trailing characters";
			error_position = pos;
			return false;
		}
		return true;
	}

	TimeSpecifierAMOrPM ampm = TimeSpecifierAMOrPM::TIME_SPECIFIER_NONE;

	// Year offset state (Year+W/j)
	auto offset_specifier = StrTimeSpecifier::WEEKDAY_DECIMAL;
	uint64_t weekno = 0;
	uint64_t weekday = 0;
	uint64_t yearday = 0;
	bool has_weekday = false;

	// ISO state (%G/%V/%u)
	// Out of range values to detect multiple specifications
	uint64_t iso_year = 10000;
	uint64_t iso_week = 54;
	uint64_t iso_weekday = 8;

	for (idx_t i = 0;; i++) {
		D_ASSERT(i < literals.size());
		// first compare the literal
		const auto &literal = literals[i];
		for (size_t l = 0; l < literal.size();) {
			// Match runs of spaces to runs of spaces.
			if (StringUtil::CharacterIsSpace(literal[l])) {
				if (pos >= size || !StringUtil::CharacterIsSpace(data[pos])) {
					error_message = "Space does not match, expected " + literals[i];
					error_position = pos;
					return false;
				}
				for (++pos; pos < size && StringUtil::CharacterIsSpace(data[pos]); ++pos) {
					continue;
				}
				for (++l; l < literal.size() && StringUtil::CharacterIsSpace(literal[l]); ++l) {
					continue;
				}
				continue;
			}
			// literal does not match
			if (pos >= size || data[pos] != literal[l]) {
				error_message = "Literal does not match, expected " + literal;
				error_position = pos;
				return false;
			}
			pos++;
			l++;
		}
		if (i == specifiers.size()) {
			break;
		}
		// now parse the specifier
		if (numeric_width[i] > 0) {
			// numeric specifier: parse a number
			uint64_t number = 0;
			int digits = 0;
			size_t start_pos = pos;
			size_t end_pos = start_pos + UnsafeNumericCast<size_t>(numeric_width[i]);
			while (pos < size && pos < end_pos && StringUtil::CharacterIsDigit(data[pos])) {
				number = number * 10 + UnsafeNumericCast<uint64_t>(data[pos]) - '0';
				pos++;
				++digits;
			}
			if (pos == start_pos) {
				// expected a number here
				error_message = "Expected a number";
				error_position = start_pos;
				return false;
			}
			switch (specifiers[i]) {
			case StrTimeSpecifier::DAY_OF_MONTH_PADDED:
			case StrTimeSpecifier::DAY_OF_MONTH:
				if (number < 1 || number > 31) {
					error_message = "Day out of range, expected a value between 1 and 31";
					error_position = start_pos;
					return false;
				}
				// day of the month
				result_data[2] = UnsafeNumericCast<int32_t>(number);
				offset_specifier = specifiers[i];
				break;
			case StrTimeSpecifier::MONTH_DECIMAL_PADDED:
			case StrTimeSpecifier::MONTH_DECIMAL:
				if (number < 1 || number > 12) {
					error_message = "Month out of range, expected a value between 1 and 12";
					error_position = start_pos;
					return false;
				}
				// month number
				result_data[1] = UnsafeNumericCast<int32_t>(number);
				offset_specifier = specifiers[i];
				break;
			case StrTimeSpecifier::YEAR_WITHOUT_CENTURY_PADDED:
			case StrTimeSpecifier::YEAR_WITHOUT_CENTURY:
				switch (offset_specifier) {
				case StrTimeSpecifier::YEAR_ISO:
				case StrTimeSpecifier::WEEK_NUMBER_ISO:
					// Override
				case StrTimeSpecifier::WEEKDAY_DECIMAL:
					// First offset specifier
					offset_specifier = specifiers[i];
					break;
				default:
					break;
				}
				// year without century..
				// Python uses 69 as a crossover point (i.e. >= 69 is 19.., < 69 is 20..)
				if (pos - start_pos < 2 && strict) {
					return false;
				}
				if (number >= 100) {
					// %y only supports numbers between [0..99]
					error_message = "Year without century out of range, expected a value between 0 and 99";
					error_position = start_pos;
					return false;
				}
				if (number >= 69) {
					result_data[0] = int32_t(1900 + number);
				} else {
					result_data[0] = int32_t(2000 + number);
				}
				break;
			case StrTimeSpecifier::YEAR_DECIMAL:
				switch (offset_specifier) {
				case StrTimeSpecifier::YEAR_ISO:
				case StrTimeSpecifier::WEEK_NUMBER_ISO:
					// Override
				case StrTimeSpecifier::WEEKDAY_DECIMAL:
					// First offset specifier
					offset_specifier = specifiers[i];
					break;
				default:
					break;
				}
				if (pos - start_pos < 2 && strict) {
					return false;
				}
				// year as full number
				result_data[0] = UnsafeNumericCast<int32_t>(number);
				break;
			case StrTimeSpecifier::YEAR_ISO:
				switch (offset_specifier) {
				// y/m/d overrides G/V/u but does not conflict
				case StrTimeSpecifier::DAY_OF_MONTH_PADDED:
				case StrTimeSpecifier::DAY_OF_MONTH:
				case StrTimeSpecifier::MONTH_DECIMAL_PADDED:
				case StrTimeSpecifier::MONTH_DECIMAL:
				case StrTimeSpecifier::YEAR_WITHOUT_CENTURY_PADDED:
				case StrTimeSpecifier::YEAR_WITHOUT_CENTURY:
				case StrTimeSpecifier::YEAR_DECIMAL:
					// Just validate, don't use
					break;
				case StrTimeSpecifier::WEEKDAY_DECIMAL:
					// First offset specifier
					offset_specifier = specifiers[i];
					break;
				case StrTimeSpecifier::YEAR_ISO:
				case StrTimeSpecifier::WEEK_NUMBER_ISO:
					// Already parsing ISO
					if (iso_year <= 9999) {
						error_message = "Multiple ISO year offsets specified";
						error_position = start_pos;
						return false;
					}
					break;
				default:
					error_message = "Incompatible ISO year offset specified";
					error_position = start_pos;
					return false;
				}
				if (number > 9999) {
					// %G only supports numbers between [0..9999]
					error_message = "ISO Year out of range, expected a value between 0000 and 9999";
					error_position = start_pos;
					return false;
				}
				iso_year = number;
				break;
			case StrTimeSpecifier::HOUR_24_PADDED:
			case StrTimeSpecifier::HOUR_24_DECIMAL:
				if (number >= 24) {
					error_message = "Hour out of range, expected a value between 0 and 23";
					error_position = start_pos;
					return false;
				}
				// hour as full number
				result_data[3] = UnsafeNumericCast<int32_t>(number);
				break;
			case StrTimeSpecifier::HOUR_12_PADDED:
			case StrTimeSpecifier::HOUR_12_DECIMAL:
				if (number < 1 || number > 12) {
					error_message = "Hour12 out of range, expected a value between 1 and 12";
					error_position = start_pos;
					return false;
				}
				// 12-hour number: start off by just storing the number
				result_data[3] = UnsafeNumericCast<int32_t>(number);
				break;
			case StrTimeSpecifier::MINUTE_PADDED:
			case StrTimeSpecifier::MINUTE_DECIMAL:
				if (number >= 60) {
					error_message = "Minutes out of range, expected a value between 0 and 59";
					error_position = start_pos;
					return false;
				}
				// minutes
				result_data[4] = UnsafeNumericCast<int32_t>(number);
				break;
			case StrTimeSpecifier::SECOND_PADDED:
			case StrTimeSpecifier::SECOND_DECIMAL:
				if (number >= 60) {
					error_message = "Seconds out of range, expected a value between 0 and 59";
					error_position = start_pos;
					return false;
				}
				// seconds
				result_data[5] = UnsafeNumericCast<int32_t>(number);
				break;
			case StrTimeSpecifier::NANOSECOND_PADDED:
				for (; digits < numeric_width[i]; ++digits) {
					number *= 10;
				}
				D_ASSERT(number < Interval::NANOS_PER_SEC); // enforced by the length of the number
				// nanoseconds
				result_data[6] = UnsafeNumericCast<int32_t>(number);
				break;
			case StrTimeSpecifier::MICROSECOND_PADDED:
				for (; digits < numeric_width[i]; ++digits) {
					number *= 10;
				}
				D_ASSERT(number < Interval::MICROS_PER_SEC); // enforced by the length of the number
				// nanoseconds
				result_data[6] = UnsafeNumericCast<int32_t>(number * Interval::NANOS_PER_MICRO);
				break;
			case StrTimeSpecifier::MILLISECOND_PADDED:
				for (; digits < numeric_width[i]; ++digits) {
					number *= 10;
				}
				D_ASSERT(number < Interval::MSECS_PER_SEC); // enforced by the length of the number
				// nanoseconds
				result_data[6] = UnsafeNumericCast<int32_t>(number * Interval::NANOS_PER_MSEC);
				break;
			case StrTimeSpecifier::WEEK_NUMBER_PADDED_SUN_FIRST:
			case StrTimeSpecifier::WEEK_NUMBER_PADDED_MON_FIRST:
				// m/d overrides WU/w but does not conflict
				switch (offset_specifier) {
				case StrTimeSpecifier::DAY_OF_MONTH_PADDED:
				case StrTimeSpecifier::DAY_OF_MONTH:
				case StrTimeSpecifier::MONTH_DECIMAL_PADDED:
				case StrTimeSpecifier::MONTH_DECIMAL:
					// Just validate, don't use
					break;
				case StrTimeSpecifier::YEAR_WITHOUT_CENTURY_PADDED:
				case StrTimeSpecifier::YEAR_WITHOUT_CENTURY:
				case StrTimeSpecifier::YEAR_DECIMAL:
					// Switch to offset parsing
				case StrTimeSpecifier::WEEKDAY_DECIMAL:
					// First offset specifier
					offset_specifier = specifiers[i];
					break;
				default:
					error_message = "Multiple week offsets specified";
					error_position = start_pos;
					return false;
				}
				if (number > 53) {
					error_message = "Week out of range, expected a value between 0 and 53";
					error_position = start_pos;
					return false;
				}
				weekno = number;
				break;
			case StrTimeSpecifier::WEEKDAY_DECIMAL:
				if (number > 6) {
					error_message = "Weekday out of range, expected a value between 0 and 6";
					error_position = start_pos;
					return false;
				}
				has_weekday = true;
				weekday = number;
				break;
			case StrTimeSpecifier::WEEK_NUMBER_ISO:
				switch (offset_specifier) {
				case StrTimeSpecifier::YEAR_WITHOUT_CENTURY_PADDED:
				case StrTimeSpecifier::YEAR_WITHOUT_CENTURY:
				case StrTimeSpecifier::YEAR_DECIMAL:
					error_message = "ISO week offsets are incompatible with non-ISO year specifiers. Use '%G' instead";
					error_position = start_pos;
					return false;
				case StrTimeSpecifier::DAY_OF_MONTH_PADDED:
				case StrTimeSpecifier::DAY_OF_MONTH:
				case StrTimeSpecifier::MONTH_DECIMAL_PADDED:
				case StrTimeSpecifier::MONTH_DECIMAL:
					// Just validate, don't use
					break;
				case StrTimeSpecifier::WEEKDAY_DECIMAL:
					// First offset specifier
					offset_specifier = specifiers[i];
					break;
				case StrTimeSpecifier::WEEK_NUMBER_ISO:
				case StrTimeSpecifier::YEAR_ISO:
					// Already parsing ISO
					if (iso_week <= 53) {
						error_message = "Multiple ISO week offsets specified";
						error_position = start_pos;
						return false;
					}
					break;
				default:
					error_message = "Incompatible ISO week offset specified";
					error_position = start_pos;
					return false;
				}
				if (number < 1 || number > 53) {
					error_message = "ISO week offset out of range, expected a value between 1 and 53";
					error_position = start_pos;
					return false;
				}
				iso_week = number;
				break;
			case StrTimeSpecifier::WEEKDAY_ISO:
				if (iso_weekday <= 7) {
					error_message = "Multiple ISO weekday offsets specified";
					error_position = start_pos;
					return false;
				}
				if (number < 1 || number > 7) {
					error_message = "ISO weekday offset out of range, expected a value between 1 and 7";
					error_position = start_pos;
					return false;
				}
				iso_weekday = number;
				break;
			case StrTimeSpecifier::DAY_OF_YEAR_PADDED:
			case StrTimeSpecifier::DAY_OF_YEAR_DECIMAL:
				// m/d overrides j but does not conflict
				switch (offset_specifier) {
				case StrTimeSpecifier::DAY_OF_MONTH_PADDED:
				case StrTimeSpecifier::DAY_OF_MONTH:
				case StrTimeSpecifier::MONTH_DECIMAL_PADDED:
				case StrTimeSpecifier::MONTH_DECIMAL:
					// Just validate, don't use
					break;
				case StrTimeSpecifier::YEAR_WITHOUT_CENTURY_PADDED:
				case StrTimeSpecifier::YEAR_WITHOUT_CENTURY:
				case StrTimeSpecifier::YEAR_DECIMAL:
					// Switch to offset parsing
				case StrTimeSpecifier::WEEKDAY_DECIMAL:
					// First offset specifier
					offset_specifier = specifiers[i];
					break;
				default:
					error_message = "Multiple year offsets specified";
					error_position = start_pos;
					return false;
				}
				if (number < 1 || number > 366) {
					error_message = "Year day out of range, expected a value between 1 and 366";
					error_position = start_pos;
					return false;
				}
				yearday = number;
				break;
			default:
				throw NotImplementedException("Unsupported specifier for strptime");
			}
		} else {
			switch (specifiers[i]) {
			case StrTimeSpecifier::AM_PM: {
				// parse the next 2 characters
				if (pos + 2 > size) {
					// no characters left to parse
					error_message = "Expected AM/PM";
					error_position = pos;
					return false;
				}
				char pa_char = StringUtil::CharacterToLower(data[pos]);
				char m_char = StringUtil::CharacterToLower(data[pos + 1]);
				if (m_char != 'm') {
					error_message = "Expected AM/PM";
					error_position = pos;
					return false;
				}
				if (pa_char == 'p') {
					ampm = TimeSpecifierAMOrPM::TIME_SPECIFIER_PM;
				} else if (pa_char == 'a') {
					ampm = TimeSpecifierAMOrPM::TIME_SPECIFIER_AM;
				} else {
					error_message = "Expected AM/PM";
					error_position = pos;
					return false;
				}
				pos += 2;
				break;
			}
			// we parse weekday names, but we don't use them as information
			case StrTimeSpecifier::ABBREVIATED_WEEKDAY_NAME:
				if (TryParseCollection(data, pos, size, Date::DAY_NAMES_ABBREVIATED, 7) < 0) {
					error_message = "Expected an abbreviated day name (Mon, Tue, Wed, Thu, Fri, Sat, Sun)";
					error_position = pos;
					return false;
				}
				break;
			case StrTimeSpecifier::FULL_WEEKDAY_NAME:
				if (TryParseCollection(data, pos, size, Date::DAY_NAMES, 7) < 0) {
					error_message = "Expected a full day name (Monday, Tuesday, etc...)";
					error_position = pos;
					return false;
				}
				break;
			case StrTimeSpecifier::ABBREVIATED_MONTH_NAME: {
				int32_t month = TryParseCollection(data, pos, size, Date::MONTH_NAMES_ABBREVIATED, 12);
				if (month < 0) {
					error_message = "Expected an abbreviated month name (Jan, Feb, Mar, etc..)";
					error_position = pos;
					return false;
				}
				result_data[1] = month + 1;
				break;
			}
			case StrTimeSpecifier::FULL_MONTH_NAME: {
				int32_t month = TryParseCollection(data, pos, size, Date::MONTH_NAMES, 12);
				if (month < 0) {
					error_message = "Expected a full month name (January, February, etc...)";
					error_position = pos;
					return false;
				}
				result_data[1] = month + 1;
				break;
			}
			case StrTimeSpecifier::UTC_OFFSET: {
				int hh, mm, ss;
				if (!Timestamp::TryParseUTCOffset(data, pos, size, hh, mm, ss)) {
					error_message = "Expected ±HH[MM] or -HH[:MM[:SS]]";
					error_position = pos;
					return false;
				}
				result_data[7] = (hh * Interval::MINS_PER_HOUR + mm) * Interval::SECS_PER_MINUTE + ss;
				break;
			}
			case StrTimeSpecifier::TZ_NAME: {
				// skip leading spaces
				while (pos < size && StringUtil::CharacterIsSpace(data[pos])) {
					pos++;
				}
				const auto tz_begin = data + pos;
				// stop when we encounter a non-tz character
				while (pos < size && Timestamp::CharacterIsTimeZone(data[pos])) {
					pos++;
				}
				const auto tz_end = data + pos;
				// Can't fully validate without a list - caller's responsibility.
				// But tz must not be empty.
				if (tz_end == tz_begin) {
					error_message = "Empty Time Zone name";
					error_position = UnsafeNumericCast<idx_t>(tz_begin - data);
					return false;
				}
				result.tz.assign(tz_begin, tz_end);
				break;
			}
			default:
				throw NotImplementedException("Unsupported specifier for strptime");
			}
		}
	}
	// skip trailing spaces
	while (pos < size && StringUtil::CharacterIsSpace(data[pos])) {
		pos++;
	}
	if (pos != size) {
		error_message = "Full specifier did not match: trailing characters";
		error_position = pos;
		return false;
	}
	if (ampm != TimeSpecifierAMOrPM::TIME_SPECIFIER_NONE) {
		if (result_data[3] > 12) {
			error_message =
			    "Invalid hour: " + to_string(result_data[3]) + " AM/PM, expected an hour within the range [0..12]";
			return false;
		}
		// adjust the hours based on the AM or PM specifier
		if (ampm == TimeSpecifierAMOrPM::TIME_SPECIFIER_AM) {
			// AM: 12AM=0, 1AM=1, 2AM=2, ..., 11AM=11
			if (result_data[3] == 12) {
				result_data[3] = 0;
			}
		} else {
			// PM: 12PM=12, 1PM=13, 2PM=14, ..., 11PM=23
			if (result_data[3] != 12) {
				result_data[3] += 12;
			}
		}
	}
	switch (offset_specifier) {
	case StrTimeSpecifier::YEAR_ISO:
	case StrTimeSpecifier::WEEK_NUMBER_ISO: {
		// Default to 1900-01-01
		iso_year = (iso_year > 9999) ? 1900 : iso_year;
		iso_week = (iso_week > 53) ? 1 : iso_week;
		iso_weekday = (iso_weekday > 7) ? 1 : iso_weekday;
		// Gregorian and ISO agree on the year of January 4
		auto jan4 = Date::FromDate(UnsafeNumericCast<int32_t>(iso_year), 1, 4);
		// ISO Week 1 starts on the previous Monday
		auto week1 = Date::GetMondayOfCurrentWeek(jan4);
		// ISO Week N starts N-1 weeks later
		auto iso_date = week1 + UnsafeNumericCast<int32_t>((iso_week - 1) * 7 + (iso_weekday - 1));
		Date::Convert(iso_date, result_data[0], result_data[1], result_data[2]);
		break;
	}
	case StrTimeSpecifier::WEEK_NUMBER_PADDED_SUN_FIRST:
	case StrTimeSpecifier::WEEK_NUMBER_PADDED_MON_FIRST: {
		// Adjust weekday to be 0-based for the week type
		if (has_weekday) {
			weekday = (weekday + 7 -
			           static_cast<uint64_t>(offset_specifier == StrTimeSpecifier::WEEK_NUMBER_PADDED_MON_FIRST)) %
			          7;
		}
		// Get the start of week 1, move back 7 days and then weekno * 7 + weekday gives the date
		const auto jan1 = Date::FromDate(result_data[0], 1, 1);
		auto yeardate = Date::GetMondayOfCurrentWeek(jan1);
		yeardate -= int(offset_specifier == StrTimeSpecifier::WEEK_NUMBER_PADDED_SUN_FIRST);
		// Is there a week 0?
		yeardate -= 7 * int(yeardate >= jan1);
		yeardate += UnsafeNumericCast<int32_t>(weekno * 7 + weekday);
		Date::Convert(yeardate, result_data[0], result_data[1], result_data[2]);
		break;
	}
	case StrTimeSpecifier::DAY_OF_YEAR_PADDED:
	case StrTimeSpecifier::DAY_OF_YEAR_DECIMAL: {
		auto year = result_data[0];
		auto yeardate = Date::FromDate(year, 1, 1);
		yeardate += UnsafeNumericCast<int32_t>(yearday - 1);
		Date::Convert(yeardate, result_data[0], result_data[1], result_data[2]);
		if (result_data[0] != year) {
			// day 366 of a year that is not a leap year
			error_message = "Year day out of range for the year";
			error_position = 0;
			return false;
		}
		break;
	}
	case StrTimeSpecifier::DAY_OF_MONTH_PADDED:
	case StrTimeSpecifier::DAY_OF_MONTH:
	case StrTimeSpecifier::MONTH_DECIMAL_PADDED:
	case StrTimeSpecifier::MONTH_DECIMAL:
	case StrTimeSpecifier::YEAR_WITHOUT_CENTURY_PADDED:
	case StrTimeSpecifier::YEAR_WITHOUT_CENTURY:
	case StrTimeSpecifier::YEAR_DECIMAL:
		// m/d overrides UWVwu/j
		break;
	default:
		D_ASSERT(offset_specifier == StrTimeSpecifier::WEEKDAY_DECIMAL);
		break;
	}

	return true;
}

//! Parses a timestamp using the given specifier
bool StrpTimeFormat::Parse(string_t str, ParseResult &result, bool strict) const {
	auto data = str.GetData();
	idx_t size = str.GetSize();
	return Parse(data, size, result, strict);
}

StrpTimeFormat::ParseResult StrpTimeFormat::Parse(const string &format_string, const string &text) {
	StrpTimeFormat format;
	format.format_specifier = format_string;
	string error = StrTimeFormat::ParseFormatSpecifier(format_string, format);
	if (!error.empty()) {
		throw InvalidInputException("Failed to parse format specifier %s: %s", format_string, error);
	}
	StrpTimeFormat::ParseResult result;
	if (!format.Parse(text, result)) {
		throw InvalidInputException("Failed to parse string \"%s\" with format specifier \"%s\"", text, format_string);
	}
	return result;
}

bool StrpTimeFormat::TryParse(const string &format_string, const string &text, ParseResult &result) {
	StrpTimeFormat format;
	format.format_specifier = format_string;
	string error = StrTimeFormat::ParseFormatSpecifier(format_string, format);
	if (!error.empty()) {
		throw InvalidInputException("Failed to parse format specifier %s: %s", format_string, error);
	}
	return format.Parse(text, result);
}

bool StrTimeFormat::Empty() const {
	return format_specifier.empty();
}

string StrpTimeFormat::FormatStrpTimeError(const string &input, optional_idx position) {
	if (!position.IsValid()) {
		return string();
	}
	return input + "\n" + string(position.GetIndex(), ' ') + "^";
}

date_t StrpTimeFormat::ParseResult::ToDate() {
	if (is_special) {
		return special;
	}
	return Date::FromDate(data[0], data[1], data[2]);
}

bool StrpTimeFormat::ParseResult::TryToDate(date_t &result) {
	return Date::TryFromDate(data[0], data[1], data[2], result);
}

int32_t StrpTimeFormat::ParseResult::GetMicros() const {
	return UnsafeNumericCast<int32_t>((data[6] + Interval::NANOS_PER_MICRO / 2) / Interval::NANOS_PER_MICRO);
}

dtime_t StrpTimeFormat::ParseResult::ToTime() {
	int hh, mm, ss;
	StrfTimeSplitOffset(data[7], hh, mm, ss);
	return Time::FromTime(data[3] - hh, data[4] - mm, data[5] - ss, GetMicros());
}

int64_t StrpTimeFormat::ParseResult::ToTimeNS() {
	int hh, mm, ss;
	StrfTimeSplitOffset(data[7], hh, mm, ss);
	return Time::ToNanoTime(data[3] - hh, data[4] - mm, data[5] - ss, data[6]);
}

bool StrpTimeFormat::ParseResult::TryToTime(dtime_t &result) {
	if (data[7]) {
		return false;
	}
	result = Time::FromTime(data[3], data[4], data[5], GetMicros());
	return true;
}

timestamp_t StrpTimeFormat::ParseResult::ToTimestamp() {
	if (is_special) {
		if (special == date_t::infinity()) {
			return timestamp_t::infinity();
		} else if (special == date_t::ninfinity()) {
			return timestamp_t::ninfinity();
		}
		return Timestamp::FromDatetime(special, dtime_t(0));
	}

	date_t date = ToDate();
	dtime_t time = ToTime();
	return Timestamp::FromDatetime(date, time);
}

bool StrpTimeFormat::ParseResult::TryToTimestamp(timestamp_t &result) {
	date_t date;
	if (!TryToDate(date)) {
		return false;
	}
	dtime_t time = ToTime();
	return Timestamp::TryFromDatetime(date, time, result);
}

timestamp_ns_t StrpTimeFormat::ParseResult::ToTimestampNS() {
	timestamp_ns_t result;
	if (is_special) {
		if (special == date_t::infinity()) {
			result.value = timestamp_t::infinity().value;
		} else if (special == date_t::ninfinity()) {
			result.value = timestamp_t::ninfinity().value;
		} else {
			result.value = special.days * Interval::NANOS_PER_DAY;
		}
	} else {
		// Don't use rounded µs
		const auto date = ToDate();
		const auto time = ToTimeNS();
		if (!Timestamp::TryCombineUnits(date.days, time, Interval::NANOS_PER_DAY, result.value)) {
			throw ConversionException("Date out of nanosecond range: %d-%d-%d", data[0], data[1], data[2]);
		}
		if (!result.IsFinite()) {
			throw ConversionException("Overflow exception in date/time -> timestamp_ns conversion");
		}
	}

	return result;
}

bool StrpTimeFormat::ParseResult::TryToTimestampNS(timestamp_ns_t &result) {
	date_t date;
	if (!TryToDate(date)) {
		return false;
	}

	// Don't use rounded µs
	const auto time = ToTimeNS();
	if (!Timestamp::TryCombineUnits(date.days, time, Interval::NANOS_PER_DAY, result.value)) {
		return false;
	}
	return result.IsFinite();
}

string StrpTimeFormat::ParseResult::FormatError(string_t input, const string &format_specifier) {
	return StringUtil::Format("Could not parse string \"%s\" according to format specifier \"%s\"\n%s\nError: %s",
	                          input.GetString(), format_specifier,
	                          FormatStrpTimeError(input.GetString(), error_position), error_message);
}

bool StrpTimeFormat::TryParseDate(string_t input, date_t &result, string &error_message) const {
	ParseResult parse_result;
	if (!Parse(input, parse_result)) {
		error_message = parse_result.FormatError(input, format_specifier);
		return false;
	}
	return parse_result.TryToDate(result);
}

bool StrpTimeFormat::TryParseDate(const char *data, size_t size, date_t &result) const {
	ParseResult parse_result;
	if (!Parse(data, size, parse_result)) {
		return false;
	}
	return parse_result.TryToDate(result);
}

bool StrpTimeFormat::TryParseTime(string_t input, dtime_t &result, string &error_message) const {
	ParseResult parse_result;
	if (!Parse(input, parse_result)) {
		error_message = parse_result.FormatError(input, format_specifier);
		return false;
	}
	return parse_result.TryToTime(result);
}

bool StrpTimeFormat::TryParseTimestamp(string_t input, timestamp_t &result, string &error_message) const {
	ParseResult parse_result;
	if (!Parse(input, parse_result)) {
		error_message = parse_result.FormatError(input, format_specifier);
		return false;
	}
	return parse_result.TryToTimestamp(result);
}

bool StrpTimeFormat::TryParseTimestamp(const char *data, size_t size, timestamp_t &result) const {
	ParseResult parse_result;
	if (!Parse(data, size, parse_result)) {
		return false;
	}
	return parse_result.TryToTimestamp(result);
}

bool StrpTimeFormat::TryParseTimestampNS(string_t input, timestamp_ns_t &result, string &error_message) const {
	ParseResult parse_result;
	if (!Parse(input, parse_result)) {
		error_message = parse_result.FormatError(input, format_specifier);
		return false;
	}
	return parse_result.TryToTimestampNS(result);
}

bool StrpTimeFormat::TryParseTimestampNS(const char *data, size_t size, timestamp_ns_t &result) const {
	ParseResult parse_result;
	if (!Parse(data, size, parse_result)) {
		return false;
	}
	return parse_result.TryToTimestampNS(result);
}

} // namespace duckdb
