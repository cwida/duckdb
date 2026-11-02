#include "islamic.hpp"

#include "astronomy.hpp"
#include "grego.hpp"

#include <cmath>

namespace duckdb {
namespace datetime {

static const int32_t ISLAMIC_LIMITS[CAL_FIELD_COUNT][4] = {
    //  minimum, greatest minimum, least maximum, maximum
    {0, 0, 0, 0},             // ERA
    {1, 1, 5000000, 5000000}, // YEAR
    {0, 0, 11, 11},           // MONTH
    {1, 1, 50, 51},           // WEEK_OF_YEAR
    {-1, -1, -1, -1},         // WEEK_OF_MONTH
    {1, 1, 29, 31},           // DATE - 31 rather than 30 to work around a calendar implementation bug
    {1, 1, 354, 355},         // DAY_OF_YEAR
    {-1, -1, -1, -1},         // DAY_OF_WEEK
    {-1, -1, 5, 5},           // DAY_OF_WEEK_IN_MONTH
    {-1, -1, -1, -1},         // AM_PM
    {-1, -1, -1, -1},         // HOUR
    {-1, -1, -1, -1},         // HOUR_OF_DAY
    {-1, -1, -1, -1},         // MINUTE
    {-1, -1, -1, -1},         // SECOND
    {-1, -1, -1, -1},         // MILLISECOND
    {-1, -1, -1, -1},         // ZONE_OFFSET
    {-1, -1, -1, -1},         // DST_OFFSET
    {1, 1, 5000000, 5000000}, // YEAR_WOY
    {-1, -1, -1, -1},         // DOW_LOCAL
    {1, 1, 5000000, 5000000}, // EXTENDED_YEAR
    {-1, -1, -1, -1},         // JULIAN_DAY
    {-1, -1, -1, -1},         // MILLISECONDS_IN_DAY
    {-1, -1, -1, -1},         // IS_LEAP_MONTH
    {0, 0, 11, 11},           // ORDINAL_MONTH
};

//! The last month of the year, which is the one that gains a day in a leap year
static constexpr int32_t DHU_AL_HIJJAH = 11;

//! Eleven years of every thirty year cycle are a day longer
static bool IsCivilLeapYear(int32_t year) {
	return (14 + 11 * year) % 30 < 11;
}

int32_t IslamicCivilCalendar::HandleGetLimit(CalendarField field, LimitType type) const {
	return ISLAMIC_LIMITS[field][int32_t(type)];
}

int64_t IslamicCivilCalendar::YearStart(int32_t year) const {
	return 354 * (int64_t(year) - 1) + FloorDiv::Divide(3 + 11 * int64_t(year), int64_t(30));
}

int64_t IslamicCivilCalendar::MonthStart(int32_t year, int32_t month) const {
	// the months alternate between 30 and 29 days, which averages to 29.5
	return int64_t(std::ceil(29.5 * month)) + 354 * (int64_t(year) - 1) +
	       FloorDiv::Divide(11 * int64_t(year) + 3, int64_t(30));
}

int64_t IslamicCivilCalendar::HandleComputeMonthStart(int32_t eyear, int32_t month, bool) const {
	// the field resolution can ask for months outside of a year, which the month start does
	// not handle, so they are moved into range first
	if (month > 11) {
		const auto years = int64_t(eyear) + month / 12;
		if (years > NumericLimits<int32_t>::Maximum() || years < NumericLimits<int32_t>::Minimum()) {
			Fail();
			return 0;
		}
		eyear = int32_t(years);
		month %= 12;
	} else if (month < 0) {
		month++;
		const auto years = int64_t(eyear) + month / 12 - 1;
		if (years > NumericLimits<int32_t>::Maximum() || years < NumericLimits<int32_t>::Minimum()) {
			Fail();
			return 0;
		}
		eyear = int32_t(years);
		month = (month % 12) + 11;
	}
	return MonthStart(eyear, month) + GetEpoch() - 1;
}

int32_t IslamicCivilCalendar::HandleGetMonthLength(int32_t eyear, int32_t month) const {
	auto length = 29 + (month + 1) % 2;
	if (month == DHU_AL_HIJJAH && IsCivilLeapYear(eyear)) {
		length++;
	}
	return length;
}

int32_t IslamicCivilCalendar::HandleGetYearLength(int32_t eyear) const {
	return 354 + (IsCivilLeapYear(eyear) ? 1 : 0);
}

int32_t IslamicCivilCalendar::HandleGetExtendedYear() {
	if (NewerField(CAL_EXTENDED_YEAR, CAL_YEAR) == CAL_EXTENDED_YEAR) {
		return InternalGet(CAL_EXTENDED_YEAR, 1);
	}
	return InternalGet(CAL_YEAR, 1);
}

void IslamicCivilCalendar::HandleComputeFields(int32_t julian_day) {
	const auto days = int64_t(julian_day) - GetEpoch();
	const auto year = FloorDiv::Divide(30 * days + 10646, int64_t(10631));
	auto month = int32_t(std::ceil((double(days) - 29 - double(YearStart(int32_t(year)))) / 29.5));
	month = MinValue(month, 11);

	const auto dom = (days - MonthStart(int32_t(year), month)) + 1;
	const auto doy = (days - MonthStart(int32_t(year), 0)) + 1;
	if (dom > NumericLimits<int32_t>::Maximum() || dom < NumericLimits<int32_t>::Minimum() ||
	    doy > NumericLimits<int32_t>::Maximum() || doy < NumericLimits<int32_t>::Minimum()) {
		Fail();
		return;
	}

	InternalSet(CAL_ERA, 0);
	InternalSet(CAL_YEAR, int32_t(year));
	InternalSet(CAL_EXTENDED_YEAR, int32_t(year));
	InternalSet(CAL_MONTH, month);
	InternalSet(CAL_ORDINAL_MONTH, month);
	InternalSet(CAL_DATE, int32_t(dom));
	InternalSet(CAL_DAY_OF_YEAR, int32_t(doy));
}

//===--------------------------------------------------------------------===//
// Observed
//===--------------------------------------------------------------------===//
//! The instant of the Hijra, which the months are counted from
static constexpr double HIJRA_MILLIS = -42521587200000.0;

static int64_t ComputeTrueMonthStart(int32_t month);

//! The angle between the moon and the sun in degrees, in the range -180..180, which is negative
//! before the new moon and positive after it
static double MoonAge(double millis) {
	Astronomer astronomer(millis);
	auto age = astronomer.GetMoonAge() * 180 / 3.14159265358979323846;
	if (age > 180) {
		age -= 360;
	}
	return age;
}

//! The day the given month starts on, counted from the Hijra. This is the day after the new
//! moon was first visible at sunset.
//!
//! Finding it takes a good number of moon positions, and the same months are asked for over and
//! over while scanning a column of dates, so the results are cached. ICU caches them as well,
//! without which this is much slower than the calendars that are pure arithmetic.
static int64_t TrueMonthStart(int32_t month) {
	struct Entry {
		int32_t month;
		int64_t start;
	};
	// a direct mapped cache, which keeps the memory bounded and needs no locking
	static constexpr idx_t CACHE_SIZE = 512;
	static thread_local Entry cache[CACHE_SIZE] = {};
	auto &entry = cache[idx_t(uint32_t(month)) % CACHE_SIZE];
	if (entry.start != 0 && entry.month == month) {
		return entry.start;
	}
	const auto start = ComputeTrueMonthStart(month);
	entry = {month, start};
	return start;
}

static int64_t ComputeTrueMonthStart(int32_t month) {
	// guess when the month started from the average length of a lunar month, then walk to the
	// day the moon was actually new
	auto origin = HIJRA_MILLIS + std::floor(month * Astronomer::SYNODIC_MONTH) * double(MILLIS_PER_DAY);
	auto age = MoonAge(origin);
	if (age >= 0) {
		// the month had already started
		do {
			origin -= double(MILLIS_PER_DAY);
			age = MoonAge(origin);
		} while (age >= 0);
	} else {
		// the previous month had not ended yet
		do {
			origin += double(MILLIS_PER_DAY);
			age = MoonAge(origin);
		} while (age < 0);
	}
	return FloorDiv::Divide(int64_t(origin) - int64_t(HIJRA_MILLIS), int64_t(MILLIS_PER_DAY)) + 1;
}

int64_t IslamicCalendar::YearStart(int32_t year) const {
	return TrueMonthStart(12 * (year - 1));
}

int64_t IslamicCalendar::MonthStart(int32_t year, int32_t month) const {
	int32_t months;
	if (!TryAdd(year, -1, months) || !TryMultiply(months, 12, months) || !TryAdd(months, month, months)) {
		Fail();
		return 0;
	}
	return TrueMonthStart(months);
}

int32_t IslamicCalendar::HandleGetMonthLength(int32_t eyear, int32_t month) const {
	month = 12 * (eyear - 1) + month;
	return int32_t(TrueMonthStart(month + 1) - TrueMonthStart(month));
}

int32_t IslamicCalendar::HandleGetYearLength(int32_t eyear) const {
	const auto month = 12 * (eyear - 1);
	return int32_t(TrueMonthStart(month + 12) - TrueMonthStart(month));
}

void IslamicCalendar::HandleComputeFields(int32_t julian_day) {
	const auto days = int32_t(int64_t(julian_day) - GetEpoch());
	// guess how many whole months have passed, then correct the guess
	auto month = int32_t(std::floor(double(days) / Astronomer::SYNODIC_MONTH));
	const auto start_date = int32_t(std::floor(month * Astronomer::SYNODIC_MONTH));
	if (days - start_date >= 25 && MoonAge(double(GetTimeInternal())) > 0) {
		// the day is near the end of the month, so assume the next one and search backwards
		month++;
	}
	while (TrueMonthStart(month) > days) {
		month--;
	}

	const auto year = month >= 0 ? ((month / 12) + 1) : ((month + 1) / 12);
	month = ((month % 12) + 12) % 12;
	const auto dom = (days - MonthStart(year, month)) + 1;
	const auto doy = (days - MonthStart(year, 0)) + 1;
	if (dom > NumericLimits<int32_t>::Maximum() || dom < NumericLimits<int32_t>::Minimum() ||
	    doy > NumericLimits<int32_t>::Maximum() || doy < NumericLimits<int32_t>::Minimum()) {
		Fail();
		return;
	}

	InternalSet(CAL_ERA, 0);
	InternalSet(CAL_YEAR, year);
	InternalSet(CAL_EXTENDED_YEAR, year);
	InternalSet(CAL_MONTH, month);
	InternalSet(CAL_ORDINAL_MONTH, month);
	InternalSet(CAL_DATE, int32_t(dom));
	InternalSet(CAL_DAY_OF_YEAR, int32_t(doy));
}

//===--------------------------------------------------------------------===//
// Umm al-Qura
//===--------------------------------------------------------------------===//
//! The observed length of every month of the years 1300 to 1600, one bit per month with the
//! first month of the year in the most significant of the twelve bits
static const uint16_t UMALQURA_MONTH_LENGTHS[] = {
    0x0AAA, 0x0D54, 0x0EC9, 0x06D4, 0x06EA, 0x036C, 0x0AAD, 0x0555, 0x06A9, 0x0792, 0x0BA9, 0x05D4, 0x0ADA, 0x055C,
    0x0D2D, 0x0695, 0x074A, 0x0B54, 0x0B6A, 0x05AD, 0x04AE, 0x0A4F, 0x0517, 0x068B, 0x06A5, 0x0AD5, 0x02D6, 0x095B,
    0x049D, 0x0A4D, 0x0D26, 0x0D95, 0x05AC, 0x09B6, 0x02BA, 0x0A5B, 0x052B, 0x0A95, 0x06CA, 0x0AE9, 0x02F4, 0x0976,
    0x02B6, 0x0956, 0x0ACA, 0x0BA4, 0x0BD2, 0x05D9, 0x02DC, 0x096D, 0x054D, 0x0AA5, 0x0B52, 0x0BA5, 0x05B4, 0x09B6,
    0x0557, 0x0297, 0x054B, 0x06A3, 0x0752, 0x0B65, 0x056A, 0x0AAB, 0x052B, 0x0C95, 0x0D4A, 0x0DA5, 0x05CA, 0x0AD6,
    0x0957, 0x04AB, 0x094B, 0x0AA5, 0x0B52, 0x0B6A, 0x0575, 0x0276, 0x08B7, 0x045B, 0x0555, 0x05A9, 0x05B4, 0x09DA,
    0x04DD, 0x026E, 0x0936, 0x0AAA, 0x0D54, 0x0DB2, 0x05D5, 0x02DA, 0x095B, 0x04AB, 0x0A55, 0x0B49, 0x0B64, 0x0B71,
    0x05B4, 0x0AB5, 0x0A55, 0x0D25, 0x0E92, 0x0EC9, 0x06D4, 0x0AE9, 0x096B, 0x04AB, 0x0A93, 0x0D49, 0x0DA4, 0x0DB2,
    0x0AB9, 0x04BA, 0x0A5B, 0x052B, 0x0A95, 0x0B2A, 0x0B55, 0x055C, 0x04BD, 0x023D, 0x091D, 0x0A95, 0x0B4A, 0x0B5A,
    0x056D, 0x02B6, 0x093B, 0x049B, 0x0655, 0x06A9, 0x0754, 0x0B6A, 0x056C, 0x0AAD, 0x0555, 0x0B29, 0x0B92, 0x0BA9,
    0x05D4, 0x0ADA, 0x055A, 0x0AAB, 0x0595, 0x0749, 0x0764, 0x0BAA, 0x05B5, 0x02B6, 0x0A56, 0x0E4D, 0x0B25, 0x0B52,
    0x0B6A, 0x05AD, 0x02AE, 0x092F, 0x0497, 0x064B, 0x06A5, 0x06AC, 0x0AD6, 0x055D, 0x049D, 0x0A4D, 0x0D16, 0x0D95,
    0x05AA, 0x05B5, 0x02DA, 0x095B, 0x04AD, 0x0595, 0x06CA, 0x06E4, 0x0AEA, 0x04F5, 0x02B6, 0x0956, 0x0AAA, 0x0B54,
    0x0BD2, 0x05D9, 0x02EA, 0x096D, 0x04AD, 0x0A95, 0x0B4A, 0x0BA5, 0x05B2, 0x09B5, 0x04D6, 0x0A97, 0x0547, 0x0693,
    0x0749, 0x0B55, 0x056A, 0x0A6B, 0x052B, 0x0A8B, 0x0D46, 0x0DA3, 0x05CA, 0x0AD6, 0x04DB, 0x026B, 0x094B, 0x0AA5,
    0x0B52, 0x0B69, 0x0575, 0x0176, 0x08B7, 0x025B, 0x052B, 0x0565, 0x05B4, 0x09DA, 0x04ED, 0x016D, 0x08B6, 0x0AA6,
    0x0D52, 0x0DA9, 0x05D4, 0x0ADA, 0x095B, 0x04AB, 0x0653, 0x0729, 0x0762, 0x0BA9, 0x05B2, 0x0AB5, 0x0555, 0x0B25,
    0x0D92, 0x0EC9, 0x06D2, 0x0AE9, 0x056B, 0x04AB, 0x0A55, 0x0D29, 0x0D54, 0x0DAA, 0x09B5, 0x04BA, 0x0A3B, 0x049B,
    0x0A4D, 0x0AAA, 0x0AD5, 0x02DA, 0x095D, 0x045E, 0x0A2E, 0x0C9A, 0x0D55, 0x06B2, 0x06B9, 0x04BA, 0x0A5D, 0x052D,
    0x0A95, 0x0B52, 0x0BA8, 0x0BB4, 0x05B9, 0x02DA, 0x095A, 0x0B4A, 0x0DA4, 0x0ED1, 0x06E8, 0x0B6A, 0x056D, 0x0535,
    0x0695, 0x0D4A, 0x0DA8, 0x0DD4, 0x06DA, 0x055B, 0x029D, 0x062B, 0x0B15, 0x0B4A, 0x0B95, 0x05AA, 0x0AAE, 0x092E,
    0x0C8F, 0x0527, 0x0695, 0x06AA, 0x0AD6, 0x055D, 0x029D};

static const int8_t UMALQURA_YEAR_START_FIX[] = {
    0, 0,  -1, 0,  -1, 0,  0,  0,  0,  0,  -1, 0,  0,  0,  0,  0,  0, 0,  -1, 0,  1,  0,  1,  1,  0, 0,  0,  0,
    1, 0,  0,  0,  0,  0,  0,  0,  1,  0,  0,  0,  0,  0,  1,  0,  0, -1, -1, 0,  0,  0,  1,  0,  0, -1, 0,  0,
    0, 1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  -1, 0,  0,  0,  1,  1, 0,  0,  -1, 0,  1,  0,  1,  1, 0,  0,  -1,
    0, 1,  0,  0,  0,  -1, 0,  1,  0,  1,  0,  0,  0,  -1, 0,  0,  0, 0,  -1, -1, 0,  -1, 0,  1,  0, 0,  0,  -1,
    0, 0,  0,  1,  0,  0,  0,  0,  0,  1,  0,  0,  -1, -1, 0,  0,  0, 1,  0,  0,  -1, -1, 0,  -1, 0, 0,  -1, -1,
    0, -1, 0,  -1, 0,  0,  -1, -1, 0,  0,  0,  0,  0,  0,  -1, 0,  1, 0,  1,  1,  0,  0,  -1, 0,  1, 0,  0,  0,
    0, 0,  1,  0,  1,  0,  0,  0,  -1, 0,  1,  0,  0,  -1, -1, 0,  0, 0,  1,  0,  0,  0,  0,  0,  0, 0,  1,  0,
    0, 0,  0,  0,  1,  0,  0,  -1, 0,  0,  0,  1,  1,  0,  0,  -1, 0, 1,  0,  1,  1,  0,  0,  0,  0, 1,  0,  0,
    0, -1, 0,  0,  0,  1,  0,  0,  0,  -1, 0,  0,  0,  0,  0,  -1, 0, -1, 0,  1,  0,  0,  0,  -1, 0, 1,  0,  1,
    0, 0,  0,  0,  0,  1,  0,  0,  -1, 0,  0,  0,  0,  1,  0,  0,  0, -1, 0,  0,  0,  0,  -1, -1, 0, -1, 0,  1,
    0, 0,  -1, -1, 0,  0,  1,  1,  0,  0,  -1, 0,  0,  0,  0,  1,  0, 0,  0,  0,  1};

int64_t IslamicUmalquraCalendar::YearStart(int32_t year) const {
	if (year < TABLE_START || year > TABLE_END) {
		return IslamicCivilCalendar::YearStart(year);
	}
	year -= TABLE_START;
	// a rounded least squares fit of the dates that follow from the observed month lengths,
	// with a correction for the years it does not land on
	const auto estimate = int64_t(std::llround((354.36720 * double(year)) + 460322.05));
	return estimate + UMALQURA_YEAR_START_FIX[year];
}

int64_t IslamicUmalquraCalendar::MonthStart(int32_t year, int32_t month) const {
	auto start = YearStart(year);
	for (int32_t i = 0; i < month; i++) {
		start += HandleGetMonthLength(year, i);
	}
	return start;
}

int32_t IslamicUmalquraCalendar::HandleGetMonthLength(int32_t eyear, int32_t month) const {
	if (eyear < TABLE_START || eyear > TABLE_END) {
		return IslamicCivilCalendar::HandleGetMonthLength(eyear, month);
	}
	const auto mask = uint16_t(0x01 << (11 - month));
	return (UMALQURA_MONTH_LENGTHS[eyear - TABLE_START] & mask) != 0 ? 30 : 29;
}

int32_t IslamicUmalquraCalendar::HandleGetYearLength(int32_t eyear) const {
	if (eyear < TABLE_START || eyear > TABLE_END) {
		return IslamicCivilCalendar::HandleGetYearLength(eyear);
	}
	int32_t length = 0;
	for (int32_t i = 0; i < 12; i++) {
		length += HandleGetMonthLength(eyear, i);
	}
	return length;
}

void IslamicUmalquraCalendar::HandleComputeFields(int32_t julian_day) {
	const auto days = int32_t(int64_t(julian_day) - GetEpoch());
	if (days < YearStart(TABLE_START)) {
		IslamicCivilCalendar::HandleComputeFields(julian_day);
		return;
	}

	// estimate a year that is close to but not greater than the real one, by inverting the
	// least squares fit that the year start uses, and then walk forward
	auto year = int32_t((double(days) - (460322.05 + 0.5)) / 354.36720) + TABLE_START - 1;
	int32_t month = 0;
	int32_t remaining = 1;
	while (remaining > 0) {
		remaining = days - int32_t(YearStart(++year)) + 1;
		const auto length = HandleGetYearLength(year);
		if (remaining == length) {
			month = 11;
			break;
		}
		if (remaining < length) {
			auto month_length = HandleGetMonthLength(year, month);
			for (month = 0; remaining > month_length; month_length = HandleGetMonthLength(year, ++month)) {
				remaining -= month_length;
			}
			break;
		}
	}

	const auto dom = days - int32_t(MonthStart(year, month)) + 1;
	const auto doy = days - int32_t(MonthStart(year, 0)) + 1;

	InternalSet(CAL_ERA, 0);
	InternalSet(CAL_YEAR, year);
	InternalSet(CAL_EXTENDED_YEAR, year);
	InternalSet(CAL_MONTH, month);
	InternalSet(CAL_ORDINAL_MONTH, month);
	InternalSet(CAL_DATE, dom);
	InternalSet(CAL_DAY_OF_YEAR, doy);
}

} // namespace datetime
} // namespace duckdb
