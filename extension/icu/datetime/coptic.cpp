#include "coptic.hpp"

#include "grego.hpp"

namespace duckdb {
namespace datetime {

//! The limits of the Coptic and Ethiopic calendar fields. The thirteenth month is only five days
//! long, or six in a leap year.
static const int32_t COPTIC_ETHIOPIC_LIMITS[CAL_FIELD_COUNT][4] = {
    //  minimum, greatest minimum, least maximum, maximum
    {0, 0, 1, 1},                           // ERA
    {1, 1, 5000000, 5000000},               // YEAR
    {0, 0, 12, 12},                         // MONTH
    {1, 1, 52, 53},                         // WEEK_OF_YEAR
    {-1, -1, -1, -1},                       // WEEK_OF_MONTH
    {1, 1, 5, 30},                          // DATE
    {1, 1, 365, 366},                       // DAY_OF_YEAR
    {-1, -1, -1, -1},                       // DAY_OF_WEEK
    {-1, -1, 1, 5},                         // DAY_OF_WEEK_IN_MONTH
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
    {-1, -1, -1, -1},                       // IS_LEAP_MONTH
    {0, 0, 12, 12},                         // ORDINAL_MONTH
};

int32_t CopticEthiopicCalendar::HandleGetLimit(CalendarField field, LimitType type) const {
	return COPTIC_ETHIOPIC_LIMITS[field][int32_t(type)];
}

int64_t CopticEthiopicCalendar::HandleComputeMonthStart(int32_t eyear, int32_t month, bool) const {
	int64_t year = eyear;
	// move a month outside of the year into range, adjusting the year with it
	if (month >= 0) {
		year += month / 13;
		month %= 13;
	} else {
		++month;
		year += month / 13 - 1;
		month = month % 13 + 12;
	}

	return int64_t(GetEpochOffset())            // the day the first year starts on
	       + 365 * year                         // the whole years
	       + FloorDiv::Divide(year, int64_t(4)) // their leap days
	       + 30 * int64_t(month)                // the whole months, which are all thirty days long
	       - 1;                                 // the day before the first of the month
}

void CopticEthiopicCalendar::HandleComputeFields(int32_t julian_day) {
	const auto days = int64_t(julian_day) - GetEpochOffset();
	if (days > NumericLimits<int32_t>::Maximum() || days < NumericLimits<int32_t>::Minimum()) {
		Fail();
		return;
	}

	// the calendar repeats every four years, which are 1461 days
	int32_t remainder;
	const auto cycles = FloorDiv::Divide(int32_t(days), 1461, remainder);
	const auto eyear = 4 * cycles + (remainder / 365 - remainder / 1460);
	// the last day of a leap year is the sixth day of the thirteenth month
	auto doy = (remainder == 1460) ? 365 : (remainder % 365);
	const auto month = doy / 30;
	const auto dom = (doy % 30) + 1;
	++doy;

	InternalSet(CAL_EXTENDED_YEAR, eyear);
	InternalSet(CAL_ERA, ExtendedYearToEra(eyear));
	InternalSet(CAL_YEAR, ExtendedYearToYear(eyear));
	InternalSet(CAL_MONTH, month);
	InternalSet(CAL_ORDINAL_MONTH, month);
	InternalSet(CAL_DATE, dom);
	InternalSet(CAL_DAY_OF_YEAR, doy);
}

int32_t CopticCalendar::HandleGetExtendedYear() {
	if (NewerField(CAL_EXTENDED_YEAR, CAL_YEAR) == CAL_EXTENDED_YEAR) {
		return InternalGet(CAL_EXTENDED_YEAR, 1);
	}
	// the year defaults to the start of the era, and the era to the one after the epoch
	if (InternalGet(CAL_ERA, CE) == BCE) {
		return 1 - InternalGet(CAL_YEAR, 1);
	}
	return InternalGet(CAL_YEAR, 1);
}

int32_t EthiopicCalendar::HandleGetExtendedYear() {
	// the extended year is always an Amete Mihret year
	if (NewerField(CAL_EXTENDED_YEAR, CAL_YEAR) == CAL_EXTENDED_YEAR) {
		return InternalGet(CAL_EXTENDED_YEAR, 1);
	}
	if (InternalGet(CAL_ERA, AMETE_MIHRET) == AMETE_MIHRET) {
		return InternalGet(CAL_YEAR, 1);
	}
	return InternalGet(CAL_YEAR, 1) - AMETE_MIHRET_DELTA;
}

int32_t EthiopicAmeteAlemCalendar::HandleGetExtendedYear() {
	if (NewerField(CAL_EXTENDED_YEAR, CAL_YEAR) == CAL_EXTENDED_YEAR) {
		return InternalGet(CAL_EXTENDED_YEAR, 1);
	}
	return InternalGet(CAL_YEAR, 1);
}

int32_t EthiopicAmeteAlemCalendar::HandleGetLimit(CalendarField field, LimitType type) const {
	if (field == CAL_ERA) {
		// there is only one era
		return 0;
	}
	return EthiopicCalendar::HandleGetLimit(field, type);
}

} // namespace datetime
} // namespace duckdb
