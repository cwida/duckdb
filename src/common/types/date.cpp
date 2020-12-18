#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/cast_helpers.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/assert.hpp"
#include "duckdb/common/limits.hpp"

#include <cstring>
#include <cctype>
#include <algorithm>

namespace duckdb {

string_t Date::MonthNamesAbbreviated[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
string_t Date::MonthNames[] = {"January", "February", "March",     "April",   "May",      "June",
                               "July",    "August",   "September", "October", "November", "December"};
string_t Date::DayNames[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
string_t Date::DayNamesAbbreviated[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

int32_t Date::NormalDays[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
int32_t Date::CumulativeDays[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365};
int32_t Date::LeapDays[] = {0, 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
int32_t Date::CumulativeLeapDays[] = {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366};
int8_t Date::MonthPerDayOfYear[] = {
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,
    2,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
    3,  3,  3,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,
    4,  4,  4,  4,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    5,  5,  5,  5,  5,  5,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
    6,  6,  6,  6,  6,  6,  6,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,
    7,  7,  7,  7,  7,  7,  7,  7,  7,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,
    8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,
    9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11,
    11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12};
int8_t Date::LeapMonthPerDayOfYear[] = {
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,
    2,  2,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
    3,  3,  3,  3,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,
    4,  4,  4,  4,  4,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    5,  5,  5,  5,  5,  5,  5,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
    6,  6,  6,  6,  6,  6,  6,  6,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,
    7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,
    8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,
    9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11,
    11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12};
int32_t Date::CumulativeYearDays[] = {
    0,      365,    730,    1096,   1461,   1826,   2191,   2557,   2922,   3287,   3652,   4018,   4383,   4748,
    5113,   5479,   5844,   6209,   6574,   6940,   7305,   7670,   8035,   8401,   8766,   9131,   9496,   9862,
    10227,  10592,  10957,  11323,  11688,  12053,  12418,  12784,  13149,  13514,  13879,  14245,  14610,  14975,
    15340,  15706,  16071,  16436,  16801,  17167,  17532,  17897,  18262,  18628,  18993,  19358,  19723,  20089,
    20454,  20819,  21184,  21550,  21915,  22280,  22645,  23011,  23376,  23741,  24106,  24472,  24837,  25202,
    25567,  25933,  26298,  26663,  27028,  27394,  27759,  28124,  28489,  28855,  29220,  29585,  29950,  30316,
    30681,  31046,  31411,  31777,  32142,  32507,  32872,  33238,  33603,  33968,  34333,  34699,  35064,  35429,
    35794,  36160,  36525,  36890,  37255,  37621,  37986,  38351,  38716,  39082,  39447,  39812,  40177,  40543,
    40908,  41273,  41638,  42004,  42369,  42734,  43099,  43465,  43830,  44195,  44560,  44926,  45291,  45656,
    46021,  46387,  46752,  47117,  47482,  47847,  48212,  48577,  48942,  49308,  49673,  50038,  50403,  50769,
    51134,  51499,  51864,  52230,  52595,  52960,  53325,  53691,  54056,  54421,  54786,  55152,  55517,  55882,
    56247,  56613,  56978,  57343,  57708,  58074,  58439,  58804,  59169,  59535,  59900,  60265,  60630,  60996,
    61361,  61726,  62091,  62457,  62822,  63187,  63552,  63918,  64283,  64648,  65013,  65379,  65744,  66109,
    66474,  66840,  67205,  67570,  67935,  68301,  68666,  69031,  69396,  69762,  70127,  70492,  70857,  71223,
    71588,  71953,  72318,  72684,  73049,  73414,  73779,  74145,  74510,  74875,  75240,  75606,  75971,  76336,
    76701,  77067,  77432,  77797,  78162,  78528,  78893,  79258,  79623,  79989,  80354,  80719,  81084,  81450,
    81815,  82180,  82545,  82911,  83276,  83641,  84006,  84371,  84736,  85101,  85466,  85832,  86197,  86562,
    86927,  87293,  87658,  88023,  88388,  88754,  89119,  89484,  89849,  90215,  90580,  90945,  91310,  91676,
    92041,  92406,  92771,  93137,  93502,  93867,  94232,  94598,  94963,  95328,  95693,  96059,  96424,  96789,
    97154,  97520,  97885,  98250,  98615,  98981,  99346,  99711,  100076, 100442, 100807, 101172, 101537, 101903,
    102268, 102633, 102998, 103364, 103729, 104094, 104459, 104825, 105190, 105555, 105920, 106286, 106651, 107016,
    107381, 107747, 108112, 108477, 108842, 109208, 109573, 109938, 110303, 110669, 111034, 111399, 111764, 112130,
    112495, 112860, 113225, 113591, 113956, 114321, 114686, 115052, 115417, 115782, 116147, 116513, 116878, 117243,
    117608, 117974, 118339, 118704, 119069, 119435, 119800, 120165, 120530, 120895, 121260, 121625, 121990, 122356,
    122721, 123086, 123451, 123817, 124182, 124547, 124912, 125278, 125643, 126008, 126373, 126739, 127104, 127469,
    127834, 128200, 128565, 128930, 129295, 129661, 130026, 130391, 130756, 131122, 131487, 131852, 132217, 132583,
    132948, 133313, 133678, 134044, 134409, 134774, 135139, 135505, 135870, 136235, 136600, 136966, 137331, 137696,
    138061, 138427, 138792, 139157, 139522, 139888, 140253, 140618, 140983, 141349, 141714, 142079, 142444, 142810,
    143175, 143540, 143905, 144271, 144636, 145001, 145366, 145732, 146097};

void Date::ExtractYearOffset(int32_t &n, int32_t &year, int32_t &year_offset) {
	year = Date::EpochYear;
	// first we normalize n to be in the year range [1970, 2370]
	// since leap years repeat every 400 years, we can safely normalize just by "shifting" the CumulativeYearDays array
	while (n < 0) {
		n += Date::DaysPerYearInterval;
		year -= Date::YearInterval;
	}
	while (n >= Date::DaysPerYearInterval) {
		n -= Date::DaysPerYearInterval;
		year += Date::YearInterval;
	}
	// interpolation search
	// we can find an upper bound of the year by assuming each year has 365 days
	year_offset = n / 365;
	// because of leap years we might be off by a little bit: compensate by decrementing the year offset until we find
	// our year
	while (n < Date::CumulativeYearDays[year_offset]) {
		year_offset--;
		D_ASSERT(year_offset >= 0);
	}
	year += year_offset;
	D_ASSERT(n >= Date::CumulativeYearDays[year_offset]);
}

void Date::Convert(int32_t n, int32_t &year, int32_t &month, int32_t &day) {
	int32_t year_offset;
	Date::ExtractYearOffset(n, year, year_offset);

	day = n - Date::CumulativeYearDays[year_offset];
	D_ASSERT(day >= 0 && day <= 365);

	bool is_leap_year = (Date::CumulativeYearDays[year_offset + 1] - Date::CumulativeYearDays[year_offset]) == 366;
	if (is_leap_year) {
		month = Date::LeapMonthPerDayOfYear[day];
		day -= Date::CumulativeLeapDays[month - 1];
	} else {
		month = Date::MonthPerDayOfYear[day];
		day -= Date::CumulativeDays[month - 1];
	}
	day++;
	D_ASSERT(day > 0 && day <= (is_leap_year ? Date::LeapDays[month] : Date::NormalDays[month]));
	D_ASSERT(month > 0 && month <= 12);
}

int32_t Date::FromDate(int32_t year, int32_t month, int32_t day) {
	int32_t n = 0;
	if (!Date::IsValid(year, month, day)) {
		throw ConversionException("Date out of range: %d-%d-%d", year, month, day);
	}
	while (year < 1970) {
		year += Date::YearInterval;
		n -= Date::DaysPerYearInterval;
	}
	while (year >= 2370) {
		year -= Date::YearInterval;
		n += Date::DaysPerYearInterval;
	}
	n += Date::CumulativeYearDays[year - 1970];
	n += Date::IsLeapYear(year) ? Date::CumulativeLeapDays[month - 1] : Date::CumulativeDays[month - 1];
	n += day - 1;
	return n;
}

bool Date::ParseDoubleDigit(const char *buf, idx_t len, idx_t &pos, int32_t &result) {
	if (pos < len && StringUtil::CharacterIsDigit(buf[pos])) {
		result = buf[pos++] - '0';
		if (pos < len && StringUtil::CharacterIsDigit(buf[pos])) {
			result = (buf[pos++] - '0') + result * 10;
		}
		return true;
	}
	return false;
}

bool Date::TryConvertDate(const char *buf, idx_t len, idx_t &pos, date_t &result, bool strict) {
	pos = 0;
	if (len == 0) {
		return false;
	}

	int32_t day = 0;
	int32_t month = -1;
	int32_t year = 0;
	bool yearneg = false;
	int sep;

	// skip leading spaces
	while (pos < len && StringUtil::CharacterIsSpace(buf[pos])) {
		pos++;
	}

	if (pos >= len) {
		return false;
	}
	if (buf[pos] == '-') {
		yearneg = true;
		pos++;
		if (pos >= len) {
			return false;
		}
	}
	if (!StringUtil::CharacterIsDigit(buf[pos])) {
		return false;
	}
	// first parse the year
	for (; pos < len && StringUtil::CharacterIsDigit(buf[pos]); pos++) {
		year = (buf[pos] - '0') + year * 10;
		if (year > Date::MaxYear) {
			break;
		}
	}
	if (yearneg) {
		year = -year;
		if (year < Date::MinYear) {
			return false;
		}
	}

	if (pos >= len) {
		return false;
	}

	// fetch the separator
	sep = buf[pos++];
	if (sep != ' ' && sep != '-' && sep != '/' && sep != '\\') {
		// invalid separator
		return false;
	}

	// parse the month
	if (!Date::ParseDoubleDigit(buf, len, pos, month)) {
		return false;
	}

	if (pos >= len) {
		return false;
	}

	if (buf[pos++] != sep) {
		return false;
	}

	if (pos >= len) {
		return false;
	}

	// now parse the day
	if (!Date::ParseDoubleDigit(buf, len, pos, day)) {
		return false;
	}

	// check for an optional trailing " (BC)""
	if (len - pos >= 5 && StringUtil::CharacterIsSpace(buf[pos]) && buf[pos + 1] == '(' && buf[pos + 2] == 'B' &&
	    buf[pos + 3] == 'C' && buf[pos + 4] == ')') {
		if (yearneg || year == 0) {
			return false;
		}
		year = -year + 1;
		pos += 5;

		if (year < Date::MinYear) {
			return false;
		}
	}

	// in strict mode, check remaining string for non-space characters
	if (strict) {
		// skip trailing spaces
		while (pos < len && StringUtil::CharacterIsSpace((unsigned char)buf[pos])) {
			pos++;
		}
		// check position. if end was not reached, non-space chars remaining
		if (pos < len) {
			return false;
		}
	} else {
		// in non-strict mode, check for any direct trailing digits
		if (pos < len && StringUtil::CharacterIsDigit((unsigned char)buf[pos])) {
			return false;
		}
	}

	result = Date::FromDate(year, month, day);
	return true;
}

date_t Date::FromCString(const char *buf, idx_t len, bool strict) {
	date_t result;
	idx_t pos;
	if (!TryConvertDate(buf, len, pos, result, strict)) {
		throw ConversionException("date/time field value out of range: \"%s\", "
		                          "expected format is (YYYY-MM-DD)",
		                          string(buf, len));
	}
	return result;
}

date_t Date::FromString(string str, bool strict) {
	return Date::FromCString(str.c_str(), str.size(), strict);
}

string Date::ToString(int32_t date) {
	int32_t date_units[3];
	idx_t year_length;
	bool add_bc;
	Date::Convert(date, date_units[0], date_units[1], date_units[2]);

	auto length = DateToStringCast::Length(date_units, year_length, add_bc);
	auto buffer = unique_ptr<char[]>(new char[length]);
	DateToStringCast::Format(buffer.get(), date_units, year_length, add_bc);
	return string(buffer.get(), length);
}

string Date::Format(int32_t year, int32_t month, int32_t day) {
	return ToString(Date::FromDate(year, month, day));
}

bool Date::IsLeapYear(int32_t year) {
	return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

bool Date::IsValid(int32_t year, int32_t month, int32_t day) {
	if (month < 1 || month > 12) {
		return false;
	}
	if (year < Date::MinYear || year > Date::MaxYear) {
		return false;
	}
	if (day < 1) {
		return false;
	}
	return Date::IsLeapYear(year) ? day <= Date::LeapDays[month] : day <= Date::NormalDays[month];
}

date_t Date::EpochDaysToDate(int32_t epoch) {
	return (date_t)epoch;
}

int32_t Date::EpochDays(date_t date) {
	return (int32_t)date;
}

date_t Date::EpochToDate(int64_t epoch) {
	return (date_t)(epoch / Interval::SECS_PER_DAY);
}

int64_t Date::Epoch(date_t date) {
	return ((int64_t)date) * Interval::SECS_PER_DAY;
}

int32_t Date::ExtractYear(date_t n, int32_t *last_year) {
	// cached look up: check if year of this date is the same as the last one we looked up
	// note that this only works for years in the range [1970, 2370]
	if (n >= Date::CumulativeYearDays[*last_year] && n < Date::CumulativeYearDays[*last_year + 1]) {
		return Date::EpochYear + *last_year;
	}
	int32_t year;
	Date::ExtractYearOffset(n, year, *last_year);
	return year;
}

int32_t Date::ExtractYear(timestamp_t ts, int32_t *last_year) {
	return Date::ExtractYear(Timestamp::GetDate(ts), last_year);
}

int32_t Date::ExtractYear(date_t n) {
	int32_t year, year_offset;
	Date::ExtractYearOffset(n, year, year_offset);
	return year;
}

int32_t Date::ExtractMonth(date_t date) {
	int32_t out_year, out_month, out_day;
	Date::Convert(date, out_year, out_month, out_day);
	return out_month;
}

int32_t Date::ExtractDay(date_t date) {
	int32_t out_year, out_month, out_day;
	Date::Convert(date, out_year, out_month, out_day);
	return out_day;
}

int32_t Date::ExtractDayOfTheYear(date_t date) {
	int32_t year, year_offset;
	Date::ExtractYearOffset(date, year, year_offset);
	return date - Date::CumulativeYearDays[year_offset] + 1;
}

int32_t Date::ExtractISODayOfTheWeek(date_t date) {
	// date of 0 is 1970-01-01, which was a Thursday (4)
	// -7 = 4
	// -6 = 5
	// -5 = 6
	// -4 = 7
	// -3 = 1
	// -2 = 2
	// -1 = 3
	// 0  = 4
	// 1  = 5
	// 2  = 6
	// 3  = 7
	// 4  = 1
	// 5  = 2
	// 6  = 3
	// 7  = 4
	if (date < 0) {
		// negative date: start off at 4 and cycle downwards
		return (7 - ((-date + 3) % 7));
	} else {
		// positive date: start off at 4 and cycle upwards
		return ((date + 3) % 7) + 1;
	}
}

static int32_t GetISOWeek(int32_t year, int32_t month, int32_t day) {
	auto day_of_the_year =
	    (Date::IsLeapYear(year) ? Date::CumulativeLeapDays[month] : Date::CumulativeDays[month]) + day;
	// get the first day of the first week of the year
	// the first week is the week that has the 4th of January in it
	auto day_of_the_fourth = Date::ExtractISODayOfTheWeek(Date::FromDate(year, 1, 4));
	// if fourth is monday, then fourth is the first day
	// if fourth is tuesday, third is the first day
	// if fourth is wednesday, second is the first day
	// if fourth is thursday - sunday, first is the first day
	auto first_day_of_the_first_week = day_of_the_fourth >= 4 ? 0 : 5 - day_of_the_fourth;
	if (day_of_the_year < first_day_of_the_first_week) {
		// day is part of last year
		return GetISOWeek(year - 1, 12, day);
	} else {
		return ((day_of_the_year - first_day_of_the_first_week) / 7) + 1;
	}
}

int32_t Date::ExtractISOWeekNumber(date_t date) {
	int32_t year, month, day;
	Date::Convert(date, year, month, day);
	return GetISOWeek(year, month - 1, day - 1);
}

int32_t Date::ExtractWeekNumberRegular(date_t date, bool monday_first) {
	int32_t year, month, day;
	Date::Convert(date, year, month, day);
	month -= 1;
	day -= 1;
	// get the day of the year
	auto day_of_the_year =
	    (Date::IsLeapYear(year) ? Date::CumulativeLeapDays[month] : Date::CumulativeDays[month]) + day;
	// now figure out the first monday or sunday of the year
	// what day is January 1st?
	auto day_of_jan_first = Date::ExtractISODayOfTheWeek(Date::FromDate(year, 1, 1));
	// monday = 1, sunday = 7
	int32_t first_week_start;
	if (monday_first) {
		// have to find next "1"
		if (day_of_jan_first == 1) {
			// jan 1 is monday: starts immediately
			first_week_start = 0;
		} else {
			// jan 1 is not monday: count days until next monday
			first_week_start = 8 - day_of_jan_first;
		}
	} else {
		first_week_start = 7 - day_of_jan_first;
	}
	if (day_of_the_year < first_week_start) {
		// day occurs before first week starts: week 0
		return 0;
	}
	return ((day_of_the_year - first_week_start) / 7) + 1;
}

// Returns the date of the monday of the current week.
date_t Date::GetMondayOfCurrentWeek(date_t date) {
	int32_t dotw = Date::ExtractISODayOfTheWeek(date);
	return date - (dotw - 1);
}

} // namespace duckdb
