#include "chinese.hpp"

#include "astronomy.hpp"
#include "grego.hpp"

#include <cmath>

namespace duckdb {
namespace datetime {

static const int32_t CHINESE_LIMITS[CAL_FIELD_COUNT][4] = {
    //  minimum, greatest minimum, least maximum, maximum
    {1, 1, 83333, 83333},                   // ERA
    {1, 1, 60, 60},                         // YEAR
    {0, 0, 11, 11},                         // MONTH
    {1, 1, 50, 55},                         // WEEK_OF_YEAR
    {-1, -1, -1, -1},                       // WEEK_OF_MONTH
    {1, 1, 29, 30},                         // DATE
    {1, 1, 353, 385},                       // DAY_OF_YEAR
    {-1, -1, -1, -1},                       // DAY_OF_WEEK
    {-1, -1, 5, 5},                         // DAY_OF_WEEK_IN_MONTH
    {-1, -1, -1, -1},                       // AM_PM
    {-1, -1, -1, -1},                       // HOUR
    {-1, -1, -1, -1},                       // HOUR_OF_DAY
    {-1, -1, -1, -1},                       // MINUTE
    {-1, -1, -1, -1},                       // SECOND
    {-1, -1, -1, -1},                       // MILLISECOND
    {-1, -1, -1, -1},                       // ZONE_OFFSET
    {-1, -1, -1, -1},                       // DST_OFFSET
    {-5000000, -5000000, 5000000, 5000000}, // YEAR_WOY
    {-1, -1, -1, -1},                       // DOW_LOCAL
    {-5000000, -5000000, 5000000, 5000000}, // EXTENDED_YEAR
    {-1, -1, -1, -1},                       // JULIAN_DAY
    {-1, -1, -1, -1},                       // MILLISECONDS_IN_DAY
    {0, 0, 1, 1},                           // IS_LEAP_MONTH
    {0, 0, 11, 12},                         // ORDINAL_MONTH
};

//! The Gregorian year that the extended years are counted from, and the one the sixty year
//! cycles are counted from
static constexpr int32_t CHINESE_EPOCH_YEAR = 1;
static constexpr int32_t CYCLE_EPOCH = -2636;
//! China standard time, which the Chinese calculations are done in
static constexpr int32_t CHINA_OFFSET = 8 * int32_t(MILLIS_PER_HOUR);
//! Enough days to be sure of landing in the next month but not the one after it
static constexpr int32_t SYNODIC_GAP = 25;

//===--------------------------------------------------------------------===//
// Date resolution
//===--------------------------------------------------------------------===//
// The months of this calendar are identified by a month and a leap month flag rather than by a
// number alone, so a set leap month flag has to select the day of the month.
static const int32_t CHINESE_DATE_1[] = {CAL_DATE, FieldCalendar::RESOLVE_STOP};
static const int32_t CHINESE_DATE_2[] = {CAL_WEEK_OF_YEAR, CAL_DAY_OF_WEEK, FieldCalendar::RESOLVE_STOP};
static const int32_t CHINESE_DATE_3[] = {CAL_WEEK_OF_MONTH, CAL_DAY_OF_WEEK, FieldCalendar::RESOLVE_STOP};
static const int32_t CHINESE_DATE_4[] = {CAL_DAY_OF_WEEK_IN_MONTH, CAL_DAY_OF_WEEK, FieldCalendar::RESOLVE_STOP};
static const int32_t CHINESE_DATE_5[] = {CAL_WEEK_OF_YEAR, CAL_DOW_LOCAL, FieldCalendar::RESOLVE_STOP};
static const int32_t CHINESE_DATE_6[] = {CAL_WEEK_OF_MONTH, CAL_DOW_LOCAL, FieldCalendar::RESOLVE_STOP};
static const int32_t CHINESE_DATE_7[] = {CAL_DAY_OF_WEEK_IN_MONTH, CAL_DOW_LOCAL, FieldCalendar::RESOLVE_STOP};
static const int32_t CHINESE_DATE_8[] = {CAL_DAY_OF_YEAR, FieldCalendar::RESOLVE_STOP};
static const int32_t CHINESE_DATE_9[] = {FieldCalendar::RESOLVE_REMAP | CAL_DATE, CAL_IS_LEAP_MONTH,
                                         FieldCalendar::RESOLVE_STOP};
static const int32_t *const CHINESE_DATE_GROUP_1[] = {CHINESE_DATE_1, CHINESE_DATE_2, CHINESE_DATE_3, CHINESE_DATE_4,
                                                      CHINESE_DATE_5, CHINESE_DATE_6, CHINESE_DATE_7, CHINESE_DATE_8,
                                                      CHINESE_DATE_9, nullptr};

static const int32_t CHINESE_DATE_10[] = {CAL_WEEK_OF_YEAR, FieldCalendar::RESOLVE_STOP};
static const int32_t CHINESE_DATE_11[] = {CAL_WEEK_OF_MONTH, FieldCalendar::RESOLVE_STOP};
static const int32_t CHINESE_DATE_12[] = {CAL_DAY_OF_WEEK_IN_MONTH, FieldCalendar::RESOLVE_STOP};
static const int32_t CHINESE_DATE_13[] = {FieldCalendar::RESOLVE_REMAP | CAL_DAY_OF_WEEK_IN_MONTH, CAL_DAY_OF_WEEK,
                                          FieldCalendar::RESOLVE_STOP};
static const int32_t CHINESE_DATE_14[] = {FieldCalendar::RESOLVE_REMAP | CAL_DAY_OF_WEEK_IN_MONTH, CAL_DOW_LOCAL,
                                          FieldCalendar::RESOLVE_STOP};
static const int32_t *const CHINESE_DATE_GROUP_2[] = {CHINESE_DATE_10, CHINESE_DATE_11, CHINESE_DATE_12,
                                                      CHINESE_DATE_13, CHINESE_DATE_14, nullptr};

static const FieldCalendar::ResolutionGroup CHINESE_DATE_PRECEDENCE[] = {CHINESE_DATE_GROUP_1, CHINESE_DATE_GROUP_2,
                                                                         nullptr};

FieldCalendar::ResolutionTable ChineseCalendar::GetDatePrecedence() const {
	return CHINESE_DATE_PRECEDENCE;
}

int32_t ChineseCalendar::HandleGetLimit(CalendarField field, LimitType type) const {
	return CHINESE_LIMITS[field][int32_t(type)];
}

//===--------------------------------------------------------------------===//
// The astronomer time zones
//===--------------------------------------------------------------------===//
int32_t ChineseCalendar::GetAstronomerOffset(double) const {
	return CHINA_OFFSET;
}

int32_t DangiCalendar::GetAstronomerOffset(double millis) const {
	// Korea used the Chinese offset, then briefly its own, and settled on +09:00 in 1912.
	// The instants are the approximations that ICU uses, which are a few days out but well
	// away from any date the calendar cares about.
	static constexpr double YEAR_1897 = double(1897 - 1970) * 365 * double(MILLIS_PER_DAY);
	static constexpr double YEAR_1898 = double(1898 - 1970) * 365 * double(MILLIS_PER_DAY);
	static constexpr double YEAR_1912 = double(1912 - 1970) * 365 * double(MILLIS_PER_DAY);
	static constexpr int32_t OFFSET_7 = 7 * int32_t(MILLIS_PER_HOUR);
	static constexpr int32_t OFFSET_8 = 8 * int32_t(MILLIS_PER_HOUR);
	static constexpr int32_t OFFSET_9 = 9 * int32_t(MILLIS_PER_HOUR);

	// the transitions are given in the standard time that was in effect before them
	if (millis < YEAR_1897 - OFFSET_8) {
		return OFFSET_8;
	}
	if (millis < YEAR_1898 - OFFSET_7) {
		return OFFSET_7;
	}
	if (millis < YEAR_1912 - OFFSET_8) {
		return OFFSET_8;
	}
	return OFFSET_9;
}

double ChineseCalendar::DaysToMillis(double days) const {
	const auto millis = days * double(MILLIS_PER_DAY);
	return millis - double(GetAstronomerOffset(millis));
}

double ChineseCalendar::MillisToDays(double millis) const {
	return FloorDiv::Divide(millis + double(GetAstronomerOffset(millis)), double(MILLIS_PER_DAY));
}

//===--------------------------------------------------------------------===//
// The astronomical events
//===--------------------------------------------------------------------===//
//! The winter solstice and the start of the year are asked for over and over while scanning a
//! column of dates, and finding either takes a search over the position of the sun, so they are
//! cached the way ICU caches them.
struct YearCache {
	static constexpr idx_t SIZE = 512;
	struct Entry {
		int32_t year;
		int32_t day;
	};
	Entry entries[SIZE];
};

//! Two calendar systems, each with a cache of solstices and one of new years
static YearCache &GetYearCache(idx_t setting, idx_t event) {
	static thread_local YearCache year_caches[2][2] = {};
	return year_caches[setting][event];
}

static bool TryGetCached(YearCache &cache, int32_t year, int32_t &day) {
	const auto &entry = cache.entries[idx_t(uint32_t(year)) % YearCache::SIZE];
	if (entry.day == 0 || entry.year != year) {
		return false;
	}
	day = entry.day;
	return true;
}

static void PutCached(YearCache &cache, int32_t year, int32_t day) {
	cache.entries[idx_t(uint32_t(year)) % YearCache::SIZE] = {year, day};
}

int32_t ChineseCalendar::NewMoonNear(double days, bool after) const {
	Astronomer astronomer(DaysToMillis(days));
	return int32_t(MillisToDays(astronomer.GetMoonTime(0, after)));
}

//! The number of whole lunar months between two days
static int32_t SynodicMonthsBetween(int32_t day1, int32_t day2) {
	const auto months = (day2 - day1) / Astronomer::SYNODIC_MONTH;
	return int32_t(months + (months >= 0 ? .5 : -.5));
}

int32_t ChineseCalendar::WinterSolstice(int32_t gyear) const {
	auto &cache = GetYearCache(GetSettingIndex(), 0);
	int32_t cached;
	if (TryGetCached(cache, gyear, cached)) {
		return cached;
	}
	// search forward from the first of December, which is always before the solstice
	const auto millis = DaysToMillis(double(Grego::FieldsToDay(gyear, 11, 1)));
	Astronomer astronomer(millis);
	const auto days = MillisToDays(astronomer.GetSunTime(Astronomer::WINTER_SOLSTICE, true));
	if (days < NumericLimits<int32_t>::Minimum() || days > NumericLimits<int32_t>::Maximum()) {
		Fail();
		return 0;
	}
	const auto result = int32_t(days);
	PutCached(cache, gyear, result);
	return result;
}

int32_t ChineseCalendar::GetMajorSolarTerm(int32_t days) const {
	Astronomer astronomer(DaysToMillis(days));
	auto term = (int32_t(6 * astronomer.GetSunLongitude() / 3.14159265358979323846) + 2) % 12;
	if (term < 1) {
		term += 12;
	}
	return term;
}

bool ChineseCalendar::HasNoMajorSolarTerm(int32_t new_moon) const {
	// a month that holds no major solar term starts and ends in the same one
	return GetMajorSolarTerm(new_moon) == GetMajorSolarTerm(NewMoonNear(new_moon + SYNODIC_GAP, true));
}

bool ChineseCalendar::IsLeapMonthBetween(int32_t new_moon1, int32_t new_moon2) const {
	while (new_moon2 >= new_moon1) {
		if (HasNoMajorSolarTerm(new_moon2)) {
			return true;
		}
		new_moon2 = NewMoonNear(new_moon2 - SYNODIC_GAP, false);
	}
	return false;
}

int32_t ChineseCalendar::NewYear(int32_t gyear) const {
	auto &cache = GetYearCache(GetSettingIndex(), 1);
	int32_t cached;
	if (TryGetCached(cache, gyear, cached)) {
		return cached;
	}

	const auto solstice_before = WinterSolstice(gyear - 1);
	const auto solstice_after = WinterSolstice(gyear);
	const auto new_moon1 = NewMoonNear(solstice_before + 1, true);
	const auto new_moon2 = NewMoonNear(new_moon1 + SYNODIC_GAP, true);
	const auto new_moon11 = NewMoonNear(solstice_after + 1, false);

	int32_t result;
	if (SynodicMonthsBetween(new_moon1, new_moon11) == 12 &&
	    (HasNoMajorSolarTerm(new_moon1) || HasNoMajorSolarTerm(new_moon2))) {
		// one of the two months is a leap month, so the year starts a month later
		result = NewMoonNear(new_moon2 + SYNODIC_GAP, true);
	} else {
		result = new_moon2;
	}
	PutCached(cache, gyear, result);
	return result;
}

//===--------------------------------------------------------------------===//
// The calendar framework
//===--------------------------------------------------------------------===//
ChineseCalendar::MonthInfo ChineseCalendar::ComputeMonthInfo(int32_t gyear, int32_t days) const {
	MonthInfo result = {0, 0, 0, false, false};

	// the solstices bound the year: month 11 is the one that holds the solstice
	int32_t solstice_before;
	auto solstice_after = WinterSolstice(gyear);
	if (days < solstice_after) {
		solstice_before = WinterSolstice(gyear - 1);
	} else {
		solstice_before = solstice_after;
		solstice_after = WinterSolstice(gyear + 1);
	}
	if (!(solstice_before <= days && days < solstice_after)) {
		Fail();
		return result;
	}

	const auto first_moon = NewMoonNear(solstice_before + 1, true);
	const auto last_moon = NewMoonNear(solstice_after + 1, false);
	result.this_moon = NewMoonNear(days + 1, false);
	// thirteen new moons between the solstices means one of the months is a leap month
	result.has_leap_month_between_solstices = SynodicMonthsBetween(first_moon, last_moon) == 12;
	result.month = SynodicMonthsBetween(first_moon, result.this_moon);

	auto new_year = NewYear(gyear);
	if (days < new_year) {
		new_year = NewYear(gyear - 1);
	}

	if (result.has_leap_month_between_solstices && IsLeapMonthBetween(first_moon, result.this_moon)) {
		result.month--;
	}
	if (result.month < 1) {
		result.month += 12;
	}
	result.ordinal_month = SynodicMonthsBetween(new_year, result.this_moon);
	if (result.ordinal_month < 0) {
		result.ordinal_month += 12;
	}
	result.is_leap_month = result.has_leap_month_between_solstices && HasNoMajorSolarTerm(result.this_moon) &&
	                       !IsLeapMonthBetween(first_moon, NewMoonNear(result.this_moon - SYNODIC_GAP, false));
	return result;
}

void ChineseCalendar::HandleComputeFields(int32_t julian_day) {
	int32_t days;
	if (!TryAdd(julian_day, -JULIAN_1970_CE, days)) {
		Fail();
		return;
	}
	const auto gyear = gregorian_year;
	const auto gmonth = gregorian_month;

	const auto month_info = ComputeMonthInfo(gyear, days);
	if (HasFailedInternal()) {
		return;
	}
	has_leap_month_between_solstices = month_info.has_leap_month_between_solstices;

	int32_t eyear;
	int32_t cycle_year;
	if (!TryAdd(gyear, -CHINESE_EPOCH_YEAR, eyear) || !TryAdd(gyear, -CYCLE_EPOCH, cycle_year)) {
		Fail();
		return;
	}
	// the last months of a Chinese year fall in the next Gregorian one
	if (month_info.month < 11 || gmonth >= 6) {
		if (!TryAdd(eyear, 1, eyear) || !TryAdd(cycle_year, 1, cycle_year)) {
			Fail();
			return;
		}
	}

	const auto dom = days - month_info.this_moon + 1;
	int32_t year_of_cycle;
	auto cycle = FloorDiv::Divide(cycle_year - 1, 60, year_of_cycle);

	auto new_year = NewYear(gyear);
	if (days < new_year) {
		new_year = NewYear(gyear - 1);
	}
	if (HasFailedInternal()) {
		return;
	}
	cycle++;
	year_of_cycle++;
	const auto doy = days - new_year + 1;

	eyear = MinValue(MaxValue(eyear, HandleGetLimit(CAL_EXTENDED_YEAR, LimitType::MINIMUM)),
	                 HandleGetLimit(CAL_EXTENDED_YEAR, LimitType::MAXIMUM));

	InternalSet(CAL_MONTH, month_info.month - 1);
	InternalSet(CAL_ORDINAL_MONTH, month_info.ordinal_month);
	InternalSet(CAL_IS_LEAP_MONTH, month_info.is_leap_month ? 1 : 0);
	InternalSet(CAL_EXTENDED_YEAR, eyear);
	InternalSet(CAL_ERA, cycle);
	InternalSet(CAL_YEAR, year_of_cycle);
	InternalSet(CAL_DATE, dom);
	InternalSet(CAL_DAY_OF_YEAR, doy);
}

int32_t ChineseCalendar::HandleGetExtendedYear() {
	// the extended year is a Gregorian year, unless the era and the year within its cycle were
	// set more recently
	if (NewerField(CAL_EXTENDED_YEAR, NewerField(CAL_ERA, CAL_YEAR)) == CAL_EXTENDED_YEAR) {
		return InternalGet(CAL_EXTENDED_YEAR, 1);
	}
	auto cycle = InternalGet(CAL_ERA, 1);
	auto year = InternalGet(CAL_YEAR, 1);
	if (!TryAdd(cycle, -1, cycle) || !TryMultiply(cycle, 60, cycle) || !TryAdd(year, cycle, year) ||
	    !TryAdd(year, CYCLE_EPOCH - CHINESE_EPOCH_YEAR, year)) {
		Fail();
		return 0;
	}
	return year;
}

int64_t ChineseCalendar::ComputeMonthStart(int32_t eyear, int32_t month, bool is_leap_month) const {
	// move a month outside of the year into range, adjusting the year with it
	if (month < 0 || month > 11) {
		const auto years = int64_t(eyear) + FloorDiv::Divide(month, 12, month);
		if (years < NumericLimits<int32_t>::Minimum() || years > NumericLimits<int32_t>::Maximum()) {
			Fail();
			return 0;
		}
		eyear = int32_t(years);
	}

	const auto new_year = NewYear(eyear);
	auto new_moon = NewMoonNear(new_year + month * 29, true);
	if (HasFailedInternal()) {
		return 0;
	}
	int16_t unused_doy;
	const auto month_year = Grego::DayToYear(new_moon, unused_doy);
	const auto month_info = ComputeMonthInfo(month_year, new_moon);
	if (HasFailedInternal()) {
		return 0;
	}
	if (month != month_info.month - 1 || is_leap_month != month_info.is_leap_month) {
		// the estimate landed in the month before the one that was asked for
		new_moon = NewMoonNear(new_moon + SYNODIC_GAP, true);
	}

	int32_t julian_day;
	if (!TryAdd(new_moon - 1, JULIAN_1970_CE, julian_day)) {
		Fail();
		return 0;
	}
	return julian_day;
}

int64_t ChineseCalendar::HandleComputeMonthStart(int32_t eyear, int32_t month, bool use_month) const {
	const auto is_leap_month = use_month && InternalGet(CAL_IS_LEAP_MONTH) != 0;
	return ComputeMonthStart(eyear, month, is_leap_month);
}

int32_t ChineseCalendar::GetMonthLength(int32_t eyear, int32_t month, bool is_leap_month) const {
	auto start = int32_t(ComputeMonthStart(eyear, month, is_leap_month));
	if (HasFailedInternal()) {
		return 0;
	}
	start = start - JULIAN_1970_CE + 1;
	return NewMoonNear(start + SYNODIC_GAP, true) - start;
}

int32_t ChineseCalendar::HandleGetMonthLength(int32_t eyear, int32_t month) const {
	return GetMonthLength(eyear, month, InternalGet(CAL_IS_LEAP_MONTH) == 1);
}

int32_t ChineseCalendar::GetActualMaximum(CalendarField field) {
	if (field != CAL_DATE) {
		return FieldCalendar::GetActualMaximum(field);
	}
	ClearFailure();
	auto work = CopyFields();
	auto &fields = *work;
	static_cast<ChineseCalendar &>(fields).PrepareGetActual(field, false);
	const auto year = fields.Get(CAL_EXTENDED_YEAR);
	const auto month = fields.Get(CAL_MONTH);
	const auto leap = fields.Get(CAL_IS_LEAP_MONTH) != 0;
	return GetMonthLength(year, month, leap);
}

//===--------------------------------------------------------------------===//
// Arithmetic
//===--------------------------------------------------------------------===//
void ChineseCalendar::OffsetMonth(int32_t new_moon, int32_t dom, int32_t delta) {
	// move to the middle of the month before the one that is wanted, then find its new moon
	const auto value = double(new_moon) + Astronomer::SYNODIC_MONTH * (double(delta) - 0.5);
	if (value < NumericLimits<int32_t>::Minimum() || value > NumericLimits<int32_t>::Maximum()) {
		Fail();
		return;
	}
	new_moon = NewMoonNear(int32_t(value), true);

	int32_t julian_day;
	if (!TryAdd(new_moon, JULIAN_1970_CE - 1, julian_day) || !TryAdd(julian_day, dom, julian_day)) {
		Fail();
		return;
	}

	// every month is 29 or 30 days long, so pinning the day only has to handle the thirtieth
	if (dom > 29) {
		Set(CAL_JULIAN_DAY, julian_day - 1);
		Complete();
		if (GetActualMaximum(CAL_DATE) >= dom) {
			Set(CAL_JULIAN_DAY, julian_day);
		}
	} else {
		Set(CAL_JULIAN_DAY, julian_day);
	}
}

void ChineseCalendar::AddChecked(CalendarField field, int32_t amount) {
	if (field != CAL_MONTH && field != CAL_ORDINAL_MONTH) {
		FieldCalendar::AddChecked(field, amount);
		return;
	}
	if (HasFailedInternal() || amount == 0) {
		return;
	}
	const auto dom = GetChecked(CAL_DATE);
	const auto day = GetChecked(CAL_JULIAN_DAY) - JULIAN_1970_CE;
	if (HasFailedInternal()) {
		return;
	}
	OffsetMonth(day - dom + 1, dom, amount);
}

} // namespace datetime
} // namespace duckdb
