#include "calendar.hpp"

#include "duckdb/common/string_util.hpp"
#include "grego.hpp"
#include "coptic.hpp"
#include "gregorian.hpp"
#include "chinese.hpp"
#include "hebrew.hpp"
#include "islamic.hpp"
#include "japanese.hpp"
#include "persian.hpp"

#include <chrono>
#include <cmath>

namespace duckdb {
namespace datetime {

//===--------------------------------------------------------------------===//
// Field resolution
//===--------------------------------------------------------------------===//
// The order in which the fields that describe a date are tried. The first row whose fields have
// all been set, and that was set most recently, wins.
static const int32_t DATE_1[] = {CAL_DATE, FieldCalendar::RESOLVE_STOP};
static const int32_t DATE_2[] = {CAL_WEEK_OF_YEAR, CAL_DAY_OF_WEEK, FieldCalendar::RESOLVE_STOP};
static const int32_t DATE_3[] = {CAL_WEEK_OF_MONTH, CAL_DAY_OF_WEEK, FieldCalendar::RESOLVE_STOP};
static const int32_t DATE_4[] = {CAL_DAY_OF_WEEK_IN_MONTH, CAL_DAY_OF_WEEK, FieldCalendar::RESOLVE_STOP};
static const int32_t DATE_5[] = {CAL_WEEK_OF_YEAR, CAL_DOW_LOCAL, FieldCalendar::RESOLVE_STOP};
static const int32_t DATE_6[] = {CAL_WEEK_OF_MONTH, CAL_DOW_LOCAL, FieldCalendar::RESOLVE_STOP};
static const int32_t DATE_7[] = {CAL_DAY_OF_WEEK_IN_MONTH, CAL_DOW_LOCAL, FieldCalendar::RESOLVE_STOP};
static const int32_t DATE_8[] = {CAL_DAY_OF_YEAR, FieldCalendar::RESOLVE_STOP};
//! if YEAR is set over YEAR_WOY, resolve to DATE
static const int32_t DATE_9[] = {FieldCalendar::RESOLVE_REMAP | CAL_DATE, CAL_YEAR, FieldCalendar::RESOLVE_STOP};
//! if YEAR_WOY is set, resolve based on WEEK_OF_YEAR
static const int32_t DATE_10[] = {FieldCalendar::RESOLVE_REMAP | CAL_WEEK_OF_YEAR, CAL_YEAR_WOY,
                                  FieldCalendar::RESOLVE_STOP};
static const int32_t *const DATE_GROUP_1[] = {DATE_1, DATE_2, DATE_3, DATE_4,  DATE_5, DATE_6,
                                              DATE_7, DATE_8, DATE_9, DATE_10, nullptr};

static const int32_t DATE_11[] = {CAL_WEEK_OF_YEAR, FieldCalendar::RESOLVE_STOP};
static const int32_t DATE_12[] = {CAL_WEEK_OF_MONTH, FieldCalendar::RESOLVE_STOP};
static const int32_t DATE_13[] = {CAL_DAY_OF_WEEK_IN_MONTH, FieldCalendar::RESOLVE_STOP};
static const int32_t DATE_14[] = {FieldCalendar::RESOLVE_REMAP | CAL_DAY_OF_WEEK_IN_MONTH, CAL_DAY_OF_WEEK,
                                  FieldCalendar::RESOLVE_STOP};
static const int32_t DATE_15[] = {FieldCalendar::RESOLVE_REMAP | CAL_DAY_OF_WEEK_IN_MONTH, CAL_DOW_LOCAL,
                                  FieldCalendar::RESOLVE_STOP};
static const int32_t *const DATE_GROUP_2[] = {DATE_11, DATE_12, DATE_13, DATE_14, DATE_15, nullptr};

const FieldCalendar::ResolutionGroup FieldCalendar::DATE_PRECEDENCE[] = {DATE_GROUP_1, DATE_GROUP_2, nullptr};

static const int32_t YEAR_1[] = {CAL_YEAR, FieldCalendar::RESOLVE_STOP};
static const int32_t YEAR_2[] = {CAL_EXTENDED_YEAR, FieldCalendar::RESOLVE_STOP};
//! YEAR_WOY is useless without WEEK_OF_YEAR
static const int32_t YEAR_3[] = {CAL_YEAR_WOY, CAL_WEEK_OF_YEAR, FieldCalendar::RESOLVE_STOP};
static const int32_t *const YEAR_GROUP[] = {YEAR_1, YEAR_2, YEAR_3, nullptr};

const FieldCalendar::ResolutionGroup FieldCalendar::YEAR_PRECEDENCE[] = {YEAR_GROUP, nullptr};

static const int32_t DOW_1[] = {CAL_DAY_OF_WEEK, FieldCalendar::RESOLVE_STOP};
static const int32_t DOW_2[] = {CAL_DOW_LOCAL, FieldCalendar::RESOLVE_STOP};
static const int32_t *const DOW_GROUP[] = {DOW_1, DOW_2, nullptr};

const FieldCalendar::ResolutionGroup FieldCalendar::DOW_PRECEDENCE[] = {DOW_GROUP, nullptr};

static const int32_t MONTH_1[] = {CAL_MONTH, FieldCalendar::RESOLVE_STOP};
static const int32_t MONTH_2[] = {CAL_ORDINAL_MONTH, FieldCalendar::RESOLVE_STOP};
static const int32_t *const MONTH_GROUP[] = {MONTH_1, MONTH_2, nullptr};

const FieldCalendar::ResolutionGroup FieldCalendar::MONTH_PRECEDENCE[] = {MONTH_GROUP, nullptr};

//! The limits of the fields that every calendar system shares
static const int32_t SHARED_LIMITS[CAL_FIELD_COUNT][4] = {
    //  minimum, greatest minimum, least maximum, maximum
    {-1, -1, -1, -1},                                                                       // ERA
    {-1, -1, -1, -1},                                                                       // YEAR
    {-1, -1, -1, -1},                                                                       // MONTH
    {-1, -1, -1, -1},                                                                       // WEEK_OF_YEAR
    {-1, -1, -1, -1},                                                                       // WEEK_OF_MONTH
    {-1, -1, -1, -1},                                                                       // DATE
    {-1, -1, -1, -1},                                                                       // DAY_OF_YEAR
    {1, 1, 7, 7},                                                                           // DAY_OF_WEEK
    {-1, -1, -1, -1},                                                                       // DAY_OF_WEEK_IN_MONTH
    {0, 0, 1, 1},                                                                           // AM_PM
    {0, 0, 11, 11},                                                                         // HOUR
    {0, 0, 23, 23},                                                                         // HOUR_OF_DAY
    {0, 0, 59, 59},                                                                         // MINUTE
    {0, 0, 59, 59},                                                                         // SECOND
    {0, 0, 999, 999},                                                                       // MILLISECOND
    {-24 * 60 * 60 * 1000, -16 * 60 * 60 * 1000, 12 * 60 * 60 * 1000, 30 * 60 * 60 * 1000}, // ZONE_OFFSET
    {-1 * 60 * 60 * 1000, -1 * 60 * 60 * 1000, 2 * 60 * 60 * 1000, 2 * 60 * 60 * 1000},     // DST_OFFSET
    {-1, -1, -1, -1},                                                                       // YEAR_WOY
    {1, 1, 7, 7},                                                                           // DOW_LOCAL
    {-1, -1, -1, -1},                                                                       // EXTENDED_YEAR
    {MIN_JULIAN, MIN_JULIAN, MAX_JULIAN, MAX_JULIAN},                                       // JULIAN_DAY
    {0, 0, 24 * 60 * 60 * 1000 - 1, 24 * 60 * 60 * 1000 - 1},                               // MILLISECONDS_IN_DAY
    {0, 0, 1, 1},                                                                           // IS_LEAP_MONTH
    {0, 0, 11, 11},                                                                         // ORDINAL_MONTH
};

//! The largest and smallest instants that can be represented
static constexpr int64_t MAX_MILLIS = int64_t(MAX_JULIAN - JULIAN_1970_CE) * MILLIS_PER_DAY;
static constexpr int64_t MIN_MILLIS = int64_t(MIN_JULIAN - JULIAN_1970_CE) * MILLIS_PER_DAY;

//===--------------------------------------------------------------------===//
// Construction
//===--------------------------------------------------------------------===//
FieldCalendar::FieldCalendar(unique_ptr<TimeZone> zone_p)
    : gregorian_year(0), gregorian_month(0), gregorian_dom(0), gregorian_doy(0), zone(std::move(zone_p)), time(0),
      fields(), stamp(), next_stamp(STAMP_MINIMUM_USER), time_set(false), fields_set(false), all_fields_set(false),
      fields_virtually_set(false), failed(false), first_day_of_week(CAL_SUNDAY), minimal_days_in_first_week(1) {
	SetTime(GetNow());
}

FieldCalendar::FieldCalendar(const FieldCalendar &other)
    : gregorian_year(other.gregorian_year), gregorian_month(other.gregorian_month), gregorian_dom(other.gregorian_dom),
      gregorian_doy(other.gregorian_doy), zone(other.zone->Copy()), time(other.time), next_stamp(other.next_stamp),
      time_set(other.time_set), fields_set(other.fields_set), all_fields_set(other.all_fields_set),
      fields_virtually_set(other.fields_virtually_set), failed(other.failed),
      first_day_of_week(other.first_day_of_week), minimal_days_in_first_week(other.minimal_days_in_first_week) {
	for (idx_t i = 0; i < CAL_FIELD_COUNT; i++) {
		fields[i] = other.fields[i];
		stamp[i] = other.stamp[i];
	}
}

int64_t Calendar::GetNow() {
	const auto now = std::chrono::system_clock::now().time_since_epoch();
	return int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

void FieldCalendar::SetTimeZone(unique_ptr<TimeZone> zone_p) {
	if (!zone_p) {
		return;
	}
	zone = std::move(zone_p);
	// the fields describe a local time, so they have to be recomputed
	fields_set = false;
}

bool FieldCalendar::Equals(const Calendar &other) const {
	if (!StringUtil::Equals(GetType(), other.GetType())) {
		return false;
	}
	auto &field_other = static_cast<const FieldCalendar &>(other);
	return zone->Equals(*field_other.zone) && first_day_of_week == field_other.first_day_of_week &&
	       minimal_days_in_first_week == field_other.minimal_days_in_first_week;
}

void FieldCalendar::SetFirstDayOfWeek(int32_t dow) {
	if (first_day_of_week != dow && dow >= CAL_SUNDAY && dow <= CAL_SATURDAY) {
		first_day_of_week = dow;
		fields_set = false;
	}
}

void FieldCalendar::SetMinimalDaysInFirstWeek(int32_t days) {
	days = MinValue<int32_t>(MaxValue<int32_t>(days, 1), 7);
	if (minimal_days_in_first_week != days) {
		minimal_days_in_first_week = days;
		fields_set = false;
	}
}

//===--------------------------------------------------------------------===//
// Getting and setting
//===--------------------------------------------------------------------===//
void FieldCalendar::SetTime(int64_t millis) {
	failed = false;
	SetTimeChecked(millis);
}

void FieldCalendar::SetTimeChecked(int64_t millis) {
	if (failed) {
		return;
	}
	time = MinValue<int64_t>(MaxValue<int64_t>(millis, MIN_MILLIS), MAX_MILLIS);
	fields_set = false;
	all_fields_set = false;
	time_set = true;
	fields_virtually_set = true;
	for (idx_t i = 0; i < CAL_FIELD_COUNT; i++) {
		fields[i] = 0;
		stamp[i] = STAMP_UNSET;
	}
}

int64_t FieldCalendar::GetTime() {
	// the time is what an operation is read back out of, so a failure along the way is still
	// the answer to this call rather than something that happened before it
	return GetTimeChecked();
}

int64_t FieldCalendar::GetTimeChecked() {
	if (failed) {
		return 0;
	}
	if (!time_set) {
		UpdateTime();
	}
	return failed ? 0 : time;
}

int32_t FieldCalendar::Get(CalendarField field) {
	failed = false;
	return GetChecked(field);
}

int32_t FieldCalendar::GetChecked(CalendarField field) {
	if (failed) {
		return 0;
	}
	Complete();
	return failed ? 0 : fields[field];
}

void FieldCalendar::Set(CalendarField field, int32_t value) {
	if (fields_virtually_set) {
		// a failure to materialize the fields is not reported, matching ICU
		const auto was_failed = failed;
		failed = false;
		ComputeFields();
		failed = was_failed;
	}
	fields[field] = value;
	if (next_stamp == NumericLimits<int32_t>::Maximum()) {
		RecalculateStamps();
	}
	stamp[field] = next_stamp++;
	fields_set = false;
	all_fields_set = false;
	time_set = false;
	fields_virtually_set = false;
}

void FieldCalendar::RecalculateStamps() {
	// hand out fresh stamps in the order in which the fields were set
	next_stamp = STAMP_INTERNALLY_SET;
	for (idx_t j = 0; j < CAL_FIELD_COUNT; j++) {
		auto current = NumericLimits<int32_t>::Maximum();
		idx_t index = CAL_FIELD_COUNT;
		for (idx_t i = 0; i < CAL_FIELD_COUNT; i++) {
			if (stamp[i] > next_stamp && stamp[i] < current) {
				current = stamp[i];
				index = i;
			}
		}
		if (index == CAL_FIELD_COUNT) {
			break;
		}
		stamp[index] = ++next_stamp;
	}
	next_stamp++;
}

void FieldCalendar::UpdateTime() {
	ComputeTime();
	if (failed) {
		return;
	}
	// the calendar is always lenient, so the fields are recomputed to normalize them
	fields_set = false;
	time_set = true;
	fields_virtually_set = false;
}

void FieldCalendar::Complete() {
	if (!time_set) {
		UpdateTime();
		if (failed) {
			return;
		}
	}
	if (!fields_set) {
		ComputeFields();
		if (failed) {
			return;
		}
		fields_set = true;
		all_fields_set = true;
	}
}

int32_t FieldCalendar::NewestStamp(CalendarField first, CalendarField last, int32_t best_so_far) const {
	auto best = best_so_far;
	for (auto i = int32_t(first); i <= int32_t(last); i++) {
		if (stamp[i] > best) {
			best = stamp[i];
		}
	}
	return best;
}

//===--------------------------------------------------------------------===//
// Limits
//===--------------------------------------------------------------------===//
int32_t FieldCalendar::GetLimit(CalendarField field, LimitType type) const {
	switch (field) {
	case CAL_DAY_OF_WEEK:
	case CAL_AM_PM:
	case CAL_HOUR:
	case CAL_HOUR_OF_DAY:
	case CAL_MINUTE:
	case CAL_SECOND:
	case CAL_MILLISECOND:
	case CAL_ZONE_OFFSET:
	case CAL_DST_OFFSET:
	case CAL_DOW_LOCAL:
	case CAL_JULIAN_DAY:
	case CAL_MILLISECONDS_IN_DAY:
	case CAL_IS_LEAP_MONTH:
		return SHARED_LIMITS[field][int32_t(type)];
	case CAL_WEEK_OF_MONTH: {
		if (type == LimitType::MINIMUM) {
			return minimal_days_in_first_week == 1 ? 1 : 0;
		}
		if (type == LimitType::GREATEST_MINIMUM) {
			return 1;
		}
		const auto days_in_month = HandleGetLimit(CAL_DATE, type);
		if (type == LimitType::LEAST_MAXIMUM) {
			return (days_in_month + (7 - minimal_days_in_first_week)) / 7;
		}
		return (days_in_month + 6 + (7 - minimal_days_in_first_week)) / 7;
	}
	default:
		return HandleGetLimit(field, type);
	}
}

//===--------------------------------------------------------------------===//
// Computing the time from the fields
//===--------------------------------------------------------------------===//
bool FieldCalendar::TryAdd(int32_t left, int32_t right, int32_t &result) {
	const auto sum = int64_t(left) + int64_t(right);
	result = int32_t(sum);
	return sum >= NumericLimits<int32_t>::Minimum() && sum <= NumericLimits<int32_t>::Maximum();
}

bool FieldCalendar::TryMultiply(int32_t left, int32_t right, int32_t &result) {
	const auto product = int64_t(left) * int64_t(right);
	result = int32_t(product);
	return product >= NumericLimits<int32_t>::Minimum() && product <= NumericLimits<int32_t>::Maximum();
}

CalendarField FieldCalendar::ResolveFields(ResolutionTable table) const {
	auto best_field = int32_t(CAL_FIELD_COUNT);
	for (idx_t g = 0; table[g] && best_field == int32_t(CAL_FIELD_COUNT); g++) {
		auto best_stamp = STAMP_UNSET;
		for (idx_t l = 0; table[g][l]; l++) {
			const auto row = table[g][l];
			auto line_stamp = STAMP_UNSET;
			// the first entry is skipped when it only records which field the row resolves to
			bool complete = true;
			for (idx_t i = (row[0] >= RESOLVE_REMAP) ? 1 : 0; row[i] != RESOLVE_STOP; i++) {
				const auto s = stamp[row[i]];
				if (s == STAMP_UNSET) {
					complete = false;
					break;
				}
				line_stamp = MaxValue(line_stamp, s);
			}
			if (!complete || line_stamp <= best_stamp) {
				continue;
			}
			auto candidate = row[0];
			if (candidate >= RESOLVE_REMAP) {
				candidate &= (RESOLVE_REMAP - 1);
				// a remapped DATE only wins if the week of the month was not set more recently
				if (candidate == CAL_DATE && stamp[CAL_WEEK_OF_MONTH] >= stamp[candidate]) {
					continue;
				}
			}
			best_field = candidate;
			best_stamp = line_stamp;
		}
	}
	return CalendarField(best_field);
}

int32_t FieldCalendar::InternalGetMonth() const {
	if (ResolveFields(MONTH_PRECEDENCE) == CAL_MONTH) {
		return InternalGet(CAL_MONTH);
	}
	return InternalGet(CAL_ORDINAL_MONTH);
}

int32_t FieldCalendar::InternalGetMonth(int32_t default_value) const {
	if (ResolveFields(MONTH_PRECEDENCE) == CAL_MONTH) {
		return InternalGet(CAL_MONTH, default_value);
	}
	return InternalGet(CAL_ORDINAL_MONTH, default_value);
}

int64_t FieldCalendar::ComputeMillisInDay() const {
	// there are two ways of specifying the time of day: the hour of the day, or the hour
	// together with the half of the day
	int64_t millis_in_day = 0;
	const auto hour_of_day_stamp = stamp[CAL_HOUR_OF_DAY];
	const auto hour_stamp = MaxValue(stamp[CAL_HOUR], stamp[CAL_AM_PM]);
	const auto best_stamp = MaxValue(hour_stamp, hour_of_day_stamp);
	if (best_stamp != STAMP_UNSET) {
		if (best_stamp == hour_of_day_stamp) {
			// values are deliberately not normalized, so that they carry into the next period
			millis_in_day += InternalGet(CAL_HOUR_OF_DAY);
		} else {
			millis_in_day += InternalGet(CAL_HOUR);
			millis_in_day += (InternalGet(CAL_AM_PM) % 2 == 0) ? 0 : 12;
		}
	}

	millis_in_day *= 60;
	millis_in_day += InternalGet(CAL_MINUTE);
	millis_in_day *= 60;
	millis_in_day += InternalGet(CAL_SECOND);
	millis_in_day *= 1000;
	millis_in_day += InternalGet(CAL_MILLISECOND);
	return millis_in_day;
}

int32_t FieldCalendar::ComputeZoneOffset(int64_t millis, int64_t millis_in_day) const {
	int32_t raw_offset;
	int32_t dst_offset;
	// a local time that occurs twice is the latter of the two, and a local time that does not
	// exist is interpreted with the offsets from before the transition
	zone->GetOffsetFromLocal(millis + millis_in_day, LocalOption::FORMER, LocalOption::LATTER, raw_offset, dst_offset);
	return raw_offset + dst_offset;
}

int32_t FieldCalendar::ComputeJulianDay() {
	// the Julian day is only used if the caller set it more recently than any of the date fields
	if (stamp[CAL_JULIAN_DAY] >= STAMP_MINIMUM_USER) {
		auto best_stamp = NewestStamp(CAL_ERA, CAL_DAY_OF_WEEK_IN_MONTH, STAMP_UNSET);
		best_stamp = NewestStamp(CAL_YEAR_WOY, CAL_EXTENDED_YEAR, best_stamp);
		best_stamp = NewestStamp(CAL_ORDINAL_MONTH, CAL_ORDINAL_MONTH, best_stamp);
		if (best_stamp <= stamp[CAL_JULIAN_DAY]) {
			return InternalGet(CAL_JULIAN_DAY);
		}
	}

	auto best_field = ResolveFields(GetDatePrecedence());
	if (best_field == CAL_FIELD_COUNT) {
		best_field = CAL_DATE;
	}
	return HandleComputeJulianDay(best_field);
}

void FieldCalendar::ComputeTime() {
	const auto julian_day = ComputeJulianDay();
	const auto millis = Grego::JulianDayToMillis(julian_day);

	// the milliseconds in the day are only used if the caller set them more recently than any
	// of the time fields
	int64_t millis_in_day;
	if (stamp[CAL_MILLISECONDS_IN_DAY] >= STAMP_MINIMUM_USER &&
	    NewestStamp(CAL_AM_PM, CAL_MILLISECOND, STAMP_UNSET) <= stamp[CAL_MILLISECONDS_IN_DAY]) {
		millis_in_day = InternalGet(CAL_MILLISECONDS_IN_DAY);
	} else {
		millis_in_day = ComputeMillisInDay();
	}

	if (stamp[CAL_ZONE_OFFSET] >= STAMP_MINIMUM_USER || stamp[CAL_DST_OFFSET] >= STAMP_MINIMUM_USER) {
		time = millis + millis_in_day - InternalGet(CAL_ZONE_OFFSET) - InternalGet(CAL_DST_OFFSET);
	} else {
		time = millis + millis_in_day - ComputeZoneOffset(millis, millis_in_day);
	}
}

int32_t FieldCalendar::HandleGetMonthLength(int32_t eyear, int32_t month) const {
	int32_t next_month;
	if (!TryAdd(month, 1, next_month)) {
		Fail();
		return 0;
	}
	return int32_t(HandleComputeMonthStart(eyear, next_month, true) - HandleComputeMonthStart(eyear, month, true));
}

int32_t FieldCalendar::HandleGetYearLength(int32_t eyear) const {
	return int32_t(HandleComputeMonthStart(eyear + 1, 0, false) - HandleComputeMonthStart(eyear, 0, false));
}

int32_t FieldCalendar::HandleComputeJulianDay(CalendarField best_field) {
	const auto use_month =
	    (best_field == CAL_DATE || best_field == CAL_WEEK_OF_MONTH || best_field == CAL_DAY_OF_WEEK_IN_MONTH);

	int32_t year;
	if (best_field == CAL_WEEK_OF_YEAR && NewerField(CAL_YEAR_WOY, CAL_YEAR) == CAL_YEAR_WOY) {
		year = InternalGet(CAL_YEAR_WOY);
	} else {
		year = HandleGetExtendedYear();
	}
	InternalSet(CAL_EXTENDED_YEAR, year);
	if (failed || year > NumericLimits<int32_t>::Maximum() / 400) {
		failed = true;
		return 0;
	}

	const auto month =
	    (IsSet(CAL_MONTH) || IsSet(CAL_ORDINAL_MONTH)) ? InternalGetMonth() : GetDefaultMonthInYear(year);
	auto julian_day = int32_t(HandleComputeMonthStart(year, use_month ? month : 0, use_month));
	if (failed) {
		return 0;
	}

	if (best_field == CAL_DATE) {
		const auto dom = IsSet(CAL_DATE) ? InternalGet(CAL_DATE, 1) : GetDefaultDayInMonth(year, month);
		int32_t result;
		if (!TryAdd(dom, julian_day, result)) {
			failed = true;
			return 0;
		}
		return result;
	}
	if (best_field == CAL_DAY_OF_YEAR) {
		int32_t result;
		if (!TryAdd(InternalGet(CAL_DAY_OF_YEAR), julian_day, result)) {
			failed = true;
			return 0;
		}
		return result;
	}

	// at this point julian_day is the day before the first day of the year or the month, so the
	// week fields are resolved relative to it
	const auto first_dow = GetFirstDayOfWeek();
	// the 0-based localized day of week of the first day of the period, 0..6
	auto first = Grego::JulianDayToDayOfWeek(julian_day + 1) - first_dow;
	if (first < 0) {
		first += 7;
	}
	const auto dow_local = GetLocalDOW();
	// the first day with the target day of week, which can fall just before the period
	auto date = 1 - first + dow_local;

	if (best_field == CAL_DAY_OF_WEEK_IN_MONTH) {
		if (date < 1) {
			date += 7;
		}
		const auto dim = InternalGet(CAL_DAY_OF_WEEK_IN_MONTH, 1);
		int32_t shift;
		if (dim >= 0) {
			if (!TryMultiply(7, dim - 1, shift) || !TryAdd(date, shift, date)) {
				failed = true;
				return 0;
			}
		} else {
			// count backwards from the last day with that day of week in the month
			const auto month_length = HandleGetMonthLength(year, InternalGetMonth(0));
			if (!TryAdd((month_length - date) / 7, dim + 1, shift) || !TryMultiply(shift, 7, shift) ||
			    !TryAdd(date, shift, date)) {
				failed = true;
				return 0;
			}
		}
	} else {
		if (best_field == CAL_WEEK_OF_YEAR &&
		    (!IsSet(CAL_YEAR_WOY) ||
		     (ResolveFields(YEAR_PRECEDENCE) != CAL_YEAR_WOY && stamp[CAL_YEAR_WOY] != STAMP_INTERNALLY_SET))) {
			// the week of the year has to stay within the year it was given for
			const auto woy = InternalGet(best_field);
			const auto next_julian_day = int32_t(HandleComputeMonthStart(year + 1, 0, false));
			auto next_first = Grego::JulianDayToDayOfWeek(next_julian_day + 1) - first_dow;
			if (next_first < 0) {
				next_first += 7;
			}

			if (woy == 1) {
				// the first week may belong to the next year
				if (next_first > 0 && (7 - next_first) >= GetMinimalDaysInFirstWeek()) {
					julian_day = next_julian_day;
					first = Grego::JulianDayToDayOfWeek(julian_day + 1) - first_dow;
					if (first < 0) {
						first += 7;
					}
					date = 1 - first + dow_local;
				}
			} else if (woy >= GetLimit(best_field, LimitType::LEAST_MAXIMUM)) {
				// the last week may belong to the previous year
				auto test_date = date;
				if ((7 - first) < GetMinimalDaysInFirstWeek()) {
					test_date += 7;
				}
				int32_t weeks;
				if (!TryMultiply(woy - 1, 7, weeks) || !TryAdd(weeks, test_date, test_date) ||
				    !TryAdd(julian_day, test_date, test_date)) {
					failed = true;
					return 0;
				}
				if (test_date > next_julian_day) {
					int32_t previous_year;
					if (!TryAdd(year, -1, previous_year)) {
						failed = true;
						return 0;
					}
					julian_day = int32_t(HandleComputeMonthStart(previous_year, 0, false));
					if (failed) {
						return 0;
					}
					first = Grego::JulianDayToDayOfWeek(julian_day + 1) - first_dow;
					if (first < 0) {
						first += 7;
					}
					date = 1 - first + dow_local;
				}
			}
		}

		if ((7 - first) < GetMinimalDaysInFirstWeek()) {
			date += 7;
		}
		int32_t weeks;
		if (!TryAdd(InternalGet(best_field), -1, weeks) || !TryMultiply(7, weeks, weeks) ||
		    !TryAdd(date, weeks, date)) {
			failed = true;
			return 0;
		}
	}

	if (!TryAdd(julian_day, date, julian_day)) {
		failed = true;
		return 0;
	}
	return julian_day;
}

int32_t FieldCalendar::GetLocalDOW() const {
	int32_t dow_local = 0;
	switch (ResolveFields(DOW_PRECEDENCE)) {
	case CAL_DAY_OF_WEEK:
		dow_local = InternalGet(CAL_DAY_OF_WEEK) - first_day_of_week;
		break;
	case CAL_DOW_LOCAL:
		dow_local = InternalGet(CAL_DOW_LOCAL) - 1;
		break;
	default:
		break;
	}
	dow_local %= 7;
	if (dow_local < 0) {
		dow_local += 7;
	}
	return dow_local;
}

FieldCalendar::ResolutionTable FieldCalendar::GetDatePrecedence() const {
	return DATE_PRECEDENCE;
}

int32_t FieldCalendar::HandleGetExtendedYearFromWeekFields(int32_t year_woy, int32_t woy) {
	const auto best_field = ResolveFields(GetDatePrecedence());
	const auto dow_local = GetLocalDOW();
	const auto first_dow = GetFirstDayOfWeek();
	const auto jan1_start = int32_t(HandleComputeMonthStart(year_woy, 0, false));
	const auto next_jan1_start = int32_t(HandleComputeMonthStart(year_woy + 1, 0, false));

	auto first = int32_t(Grego::JulianDayToDayOfWeek(jan1_start + 1)) - first_dow;
	if (first < 0) {
		first += 7;
	}
	// whether the first week of the year is too short to belong to it
	const auto jan1_in_prev_year = (7 - first) < GetMinimalDaysInFirstWeek();

	switch (best_field) {
	case CAL_WEEK_OF_YEAR:
		if (woy == 1) {
			if (jan1_in_prev_year) {
				return year_woy;
			}
			// the first week is split between the two years
			return dow_local < first ? year_woy - 1 : year_woy;
		}
		if (woy >= GetLimit(best_field, LimitType::LEAST_MAXIMUM)) {
			// the last week may already be the first week of the next year
			auto jd = jan1_start + (7 - first) + (woy - 1) * 7 + dow_local;
			if (!jan1_in_prev_year) {
				jd -= 7;
			}
			return (jd + 1) >= next_jan1_start ? year_woy + 1 : year_woy;
		}
		return year_woy;
	case CAL_DATE: {
		const auto month = InternalGetMonth();
		if (month == 0 && woy >= GetLimit(CAL_WEEK_OF_YEAR, LimitType::LEAST_MAXIMUM)) {
			return year_woy + 1;
		}
		if (woy == 1) {
			return month == 0 ? year_woy : year_woy - 1;
		}
		return year_woy;
	}
	default:
		return year_woy;
	}
}

//===--------------------------------------------------------------------===//
// Computing the fields from the time
//===--------------------------------------------------------------------===//
void FieldCalendar::ComputeFields() {
	auto local_millis = time;
	int32_t raw_offset;
	int32_t dst_offset;
	zone->GetOffset(local_millis, raw_offset, dst_offset);
	local_millis += (raw_offset + dst_offset);

	// the fields that the calendar system derives from the Julian day start out unset
	static constexpr uint32_t DERIVED = (1u << CAL_ERA) | (1u << CAL_YEAR) | (1u << CAL_MONTH) | (1u << CAL_DATE) |
	                                    (1u << CAL_DAY_OF_YEAR) | (1u << CAL_EXTENDED_YEAR) | (1u << CAL_ORDINAL_MONTH);
	for (idx_t i = 0; i < CAL_FIELD_COUNT; i++) {
		stamp[i] = (DERIVED & (1u << i)) ? STAMP_UNSET : STAMP_INTERNALLY_SET;
	}

	int32_t millis_in_day;
	const auto days = FloorDiv::Divide(local_millis, int32_t(MILLIS_PER_DAY), millis_in_day) + JULIAN_1970_CE;
	if (days > int64_t(NumericLimits<int32_t>::Maximum()) || days < int64_t(NumericLimits<int32_t>::Minimum())) {
		failed = true;
		return;
	}
	const auto julian_day = int32_t(days);
	InternalSet(CAL_JULIAN_DAY, julian_day);

	// every calendar system is computed from the proleptic Gregorian fields of the Julian day
	const auto epoch_day = int64_t(julian_day) - JULIAN_1970_CE;
	if (epoch_day > NumericLimits<int32_t>::Maximum() || epoch_day < NumericLimits<int32_t>::Minimum()) {
		failed = true;
		return;
	}
	int8_t dow;
	Grego::DayToFields(int32_t(epoch_day), gregorian_year, gregorian_month, gregorian_dom, dow, gregorian_doy);
	InternalSet(CAL_DAY_OF_WEEK, dow);

	HandleComputeFields(julian_day);
	ComputeWeekFields();

	fields[CAL_MILLISECONDS_IN_DAY] = millis_in_day;
	fields[CAL_MILLISECOND] = millis_in_day % 1000;
	millis_in_day /= 1000;
	fields[CAL_SECOND] = millis_in_day % 60;
	millis_in_day /= 60;
	fields[CAL_MINUTE] = millis_in_day % 60;
	millis_in_day /= 60;
	fields[CAL_HOUR_OF_DAY] = millis_in_day;
	fields[CAL_AM_PM] = millis_in_day / 12;
	fields[CAL_HOUR] = millis_in_day % 12;
	fields[CAL_ZONE_OFFSET] = raw_offset;
	fields[CAL_DST_OFFSET] = dst_offset;
}

int32_t FieldCalendar::WeekNumber(int32_t desired_day, int32_t day_of_period, int32_t day_of_week) const {
	// the 0-based localized day of week of the first day of the period
	auto period_start_dow = (day_of_week - first_day_of_week - day_of_period + 1) % 7;
	if (period_start_dow < 0) {
		period_start_dow += 7;
	}
	// fill out the first week, then only count it if it is long enough
	auto week_no = (desired_day + period_start_dow - 1) / 7;
	if ((7 - period_start_dow) >= minimal_days_in_first_week) {
		++week_no;
	}
	return week_no;
}

void FieldCalendar::ComputeWeekFields() {
	const auto day_of_week = fields[CAL_DAY_OF_WEEK];
	auto dow_local = day_of_week - first_day_of_week + 1;
	if (dow_local < 1) {
		dow_local += 7;
	}
	InternalSet(CAL_DOW_LOCAL, dow_local);

	const auto eyear = fields[CAL_EXTENDED_YEAR];
	const auto day_of_year = fields[CAL_DAY_OF_YEAR];

	// days at the start of a year can fall into the last week of the previous year, and days at
	// the end of a year into the first week of the next one
	auto year_of_woy = eyear;
	const auto rel_dow = (day_of_week + 7 - first_day_of_week) % 7;
	const auto rel_dow_jan1 = (day_of_week - day_of_year + 7001 - first_day_of_week) % 7;
	auto woy = (day_of_year - 1 + rel_dow_jan1) / 7;
	if ((7 - rel_dow_jan1) >= minimal_days_in_first_week) {
		++woy;
	}

	if (woy == 0) {
		// the day is in the last week of the previous year
		const auto prev_doy = day_of_year + HandleGetYearLength(eyear - 1);
		woy = WeekNumber(prev_doy, prev_doy, day_of_week);
		year_of_woy--;
	} else {
		const auto last_doy = HandleGetYearLength(eyear);
		// only the last few days of the year can be in the first week of the next one
		if (day_of_year >= (last_doy - 5)) {
			auto last_rel_dow = (rel_dow + last_doy - day_of_year) % 7;
			if (last_rel_dow < 0) {
				last_rel_dow += 7;
			}
			if ((6 - last_rel_dow) >= minimal_days_in_first_week && (day_of_year + 7 - rel_dow) > last_doy) {
				woy = 1;
				year_of_woy++;
			}
		}
	}
	fields[CAL_WEEK_OF_YEAR] = woy;
	fields[CAL_YEAR_WOY] = year_of_woy;

	const auto dom = fields[CAL_DATE];
	fields[CAL_WEEK_OF_MONTH] = WeekNumber(dom, dom, day_of_week);
	fields[CAL_DAY_OF_WEEK_IN_MONTH] = (dom - 1) / 7 + 1;
}

//===--------------------------------------------------------------------===//
// Arithmetic
//===--------------------------------------------------------------------===//
void FieldCalendar::Add(CalendarField field, int32_t amount) {
	AddChecked(field, amount);
}

void FieldCalendar::AddChecked(CalendarField field, int32_t amount) {
	if (failed) {
		return;
	}
	if (amount == 0) {
		return;
	}

	// The fields are added by converting the amount to milliseconds and adding that to the time.
	// For fields of a day or more the wall clock time has to stay the same across a change in the
	// zone offset, which is corrected for afterwards. For smaller fields no correction is made,
	// because it would undo the addition itself.
	int64_t delta = amount;
	bool keep_wall_time = true;

	switch (field) {
	case CAL_ERA: {
		const auto era = GetChecked(CAL_ERA);
		const auto year = GetChecked(CAL_YEAR);
		if (failed) {
			return;
		}
		int32_t sum;
		if (!TryAdd(era, amount, sum)) {
			Fail();
			return;
		}
		Set(CAL_ERA, sum);
		PinField(CAL_ERA);
		// keep the same year within the new era
		Set(CAL_YEAR, year);
		PinField(CAL_DATE);
		return;
	}
	case CAL_YEAR: {
		// extended years continue across era boundaries
		const auto year = GetChecked(CAL_EXTENDED_YEAR);
		int32_t sum;
		if (failed || !TryAdd(year, amount, sum)) {
			Fail();
			return;
		}
		Set(CAL_EXTENDED_YEAR, sum);
		PinField(CAL_DATE);
		return;
	}
	case CAL_YEAR_WOY: {
		// week-based year numbers continue across era boundaries
		const auto year = GetChecked(CAL_YEAR_WOY);
		int32_t sum;
		if (failed || !TryAdd(year, amount, sum)) {
			Fail();
			return;
		}
		Set(CAL_YEAR_WOY, sum);
		PinField(CAL_DATE);
		return;
	}
	case CAL_EXTENDED_YEAR:
	case CAL_MONTH:
	case CAL_ORDINAL_MONTH: {
		const auto value = GetChecked(field);
		if (failed) {
			return;
		}
		int32_t sum;
		if (!TryAdd(value, amount, sum)) {
			Fail();
			return;
		}
		Set(field, sum);
		// adding a month to the 31st of a month keeps it within the resulting month
		PinField(CAL_DATE);
		return;
	}
	case CAL_WEEK_OF_YEAR:
	case CAL_WEEK_OF_MONTH:
	case CAL_DAY_OF_WEEK_IN_MONTH:
		delta *= MILLIS_PER_WEEK;
		break;
	case CAL_AM_PM:
		delta *= 12 * MILLIS_PER_HOUR;
		break;
	case CAL_DATE:
	case CAL_DAY_OF_YEAR:
	case CAL_DAY_OF_WEEK:
	case CAL_DOW_LOCAL:
	case CAL_JULIAN_DAY:
		delta *= MILLIS_PER_DAY;
		break;
	case CAL_HOUR_OF_DAY:
	case CAL_HOUR:
		delta *= MILLIS_PER_HOUR;
		keep_wall_time = false;
		break;
	case CAL_MINUTE:
		delta *= MILLIS_PER_MINUTE;
		keep_wall_time = false;
		break;
	case CAL_SECOND:
		delta *= MILLIS_PER_SECOND;
		keep_wall_time = false;
		break;
	case CAL_MILLISECOND:
	case CAL_MILLISECONDS_IN_DAY:
		keep_wall_time = false;
		break;
	default:
		failed = true;
		return;
	}

	int32_t prev_offset = 0;
	int32_t prev_wall_time = 0;
	if (keep_wall_time) {
		prev_offset = GetChecked(CAL_DST_OFFSET) + GetChecked(CAL_ZONE_OFFSET);
		prev_wall_time = GetChecked(CAL_MILLISECONDS_IN_DAY);
	}

	SetTimeChecked(GetTimeChecked() + delta);

	if (!keep_wall_time) {
		return;
	}
	auto new_wall_time = GetChecked(CAL_MILLISECONDS_IN_DAY);
	if (new_wall_time == prev_wall_time) {
		return;
	}
	// a zone transition between the two instants shifted the wall clock time
	const auto t = time;
	const auto new_offset = GetChecked(CAL_DST_OFFSET) + GetChecked(CAL_ZONE_OFFSET);
	if (new_offset == prev_offset) {
		return;
	}
	// a shift of a whole day or more must not move the date, which happens for the zones that
	// crossed the date line
	auto adjustment = prev_offset - new_offset;
	adjustment = adjustment >= 0 ? adjustment % int32_t(MILLIS_PER_DAY) : -(-adjustment % int32_t(MILLIS_PER_DAY));
	if (adjustment != 0) {
		SetTimeChecked(t + adjustment);
		new_wall_time = GetChecked(CAL_MILLISECONDS_IN_DAY);
	}
	// the wall clock time does not exist on the resulting date, so it is interpreted with the
	// offsets from after the transition
	if (new_wall_time != prev_wall_time && adjustment < 0) {
		SetTimeChecked(t);
	}
}

//! Roughly how long a field is, in milliseconds, which is only used to guess how many of them
//! fit between two instants. A guess that is wrong costs nothing but the search it saves.
static double ApproximateFieldLength(CalendarField field) {
	switch (field) {
	case CAL_ERA:
		return 0;
	case CAL_YEAR:
	case CAL_YEAR_WOY:
	case CAL_EXTENDED_YEAR:
		return 365.2425 * double(MILLIS_PER_DAY);
	case CAL_MONTH:
	case CAL_ORDINAL_MONTH:
		return 30.436875 * double(MILLIS_PER_DAY);
	case CAL_WEEK_OF_YEAR:
	case CAL_WEEK_OF_MONTH:
	case CAL_DAY_OF_WEEK_IN_MONTH:
		return double(MILLIS_PER_WEEK);
	case CAL_DATE:
	case CAL_DAY_OF_YEAR:
	case CAL_DAY_OF_WEEK:
	case CAL_DOW_LOCAL:
	case CAL_JULIAN_DAY:
		return double(MILLIS_PER_DAY);
	case CAL_AM_PM:
		return 12 * double(MILLIS_PER_HOUR);
	case CAL_HOUR:
	case CAL_HOUR_OF_DAY:
		return double(MILLIS_PER_HOUR);
	case CAL_MINUTE:
		return double(MILLIS_PER_MINUTE);
	case CAL_SECOND:
		return double(MILLIS_PER_SECOND);
	default:
		return 1;
	}
}

int32_t FieldCalendar::TryGuessDifference(int64_t start, int64_t target, CalendarField field, int32_t &result) {
	const auto length = ApproximateFieldLength(field);
	if (length <= 0) {
		return false;
	}
	const auto estimate = (static_cast<double>(target) - static_cast<double>(start)) / length;
	// a small difference is found by the search below in about as many steps as the guess would
	// take to check, so it is only worth guessing once the two instants are further apart
	static constexpr double WORTH_GUESSING = 8;
	if (!(std::fabs(estimate) >= WORTH_GUESSING) ||
	    estimate < static_cast<double>(NumericLimits<int32_t>::Minimum()) / 2.0 ||
	    estimate > static_cast<double>(NumericLimits<int32_t>::Maximum()) / 2.0) {
		return false;
	}

	// The answer is the number of whole fields that fit between the two instants: the largest
	// count that lands on or before the target. The guess is only a starting point, and it is
	// walked to the answer rather than trusted, so a guess that is wrong only costs a step.
	const auto step = (target < start) ? -1 : 1;
	const auto reaches = [&](int32_t count, int64_t &reached) {
		SetTimeChecked(start);
		AddChecked(field, count);
		reached = GetTimeChecked();
		if (failed) {
			return false;
		}
		return step > 0 ? (reached <= target) : (reached >= target);
	};

	static constexpr int32_t MAX_STEPS = 4;
	auto count = int32_t(estimate);
	int64_t reached;
	if (!reaches(count, reached)) {
		if (failed) {
			return false;
		}
		// the guess passed the target, so walk back to the last count that does not
		for (int32_t i = 0; i < MAX_STEPS; i++) {
			count -= step;
			if (reaches(count, reached)) {
				result = count;
				return true;
			}
			if (failed) {
				return false;
			}
		}
		return false;
	}
	// the guess lands on or before the target, so walk forward while the next one still does
	for (int32_t i = 0; i < MAX_STEPS; i++) {
		int64_t next;
		if (!reaches(count + step, next)) {
			if (failed) {
				return false;
			}
			result = count;
			return true;
		}
		count += step;
	}
	return false;
}

int32_t FieldCalendar::FieldDifference(int64_t target, CalendarField field) {
	failed = false;
	int32_t min = 0;
	const auto start = GetTimeChecked();
	if (field == CAL_ERA) {
		// eras can start partway through a year, so only count complete eras between the two dates
		const auto start_era = GetChecked(CAL_ERA);
		SetTimeChecked(target);
		const auto target_era = GetChecked(CAL_ERA);
		const auto difference = int64_t(target_era) - start_era;
		if (failed) {
			return 0;
		}
		if (difference < NumericLimits<int32_t>::Minimum() || difference > NumericLimits<int32_t>::Maximum()) {
			Fail();
			return 0;
		}
		// start by applying the difference between the two era numbers
		auto result = int32_t(difference);
		SetTimeChecked(start);
		AddChecked(field, result);
		const auto reached = GetTimeChecked();
		if (failed) {
			return 0;
		}
		// check whether applying the era number difference reaches the target without overshooting it
		if (reached == target || (start < target ? reached < target : reached > target)) {
			return result;
		}
		result += start < target ? -1 : 1;
		// leave the calendar at the date reached by the returned difference
		SetTimeChecked(start);
		AddChecked(field, result);
		return failed ? 0 : result;
	}

	// most differences are close to what the average length of the field predicts, which saves
	// the search below from having to bracket the answer from scratch
	if (start != target && TryGuessDifference(start, target, field, min)) {
		SetTimeChecked(start);
		AddChecked(field, min);
		return min;
	}
	failed = false;
	// the amount is always added to the start, so that adding a year to February 29 four times
	// in a row does not get stuck on February 28
	if (start < target) {
		int32_t max = 1;
		// double the amount until it overshoots
		while (!failed) {
			SetTimeChecked(start);
			AddChecked(field, max);
			const auto ms = GetTimeChecked();
			if (ms == target) {
				return max;
			}
			if (ms > target) {
				break;
			}
			if (max == NumericLimits<int32_t>::Maximum()) {
				failed = true;
				return 0;
			}
			min = max;
			max <<= 1;
			if (max < 0) {
				max = NumericLimits<int32_t>::Maximum();
			}
		}
		while ((max - min) > 1 && !failed) {
			const auto t = min + (max - min) / 2;
			SetTimeChecked(start);
			AddChecked(field, t);
			const auto ms = GetTimeChecked();
			if (ms == target) {
				return t;
			} else if (ms > target) {
				max = t;
			} else {
				min = t;
			}
		}
	} else if (start > target) {
		int32_t max = -1;
		while (!failed) {
			SetTimeChecked(start);
			AddChecked(field, max);
			const auto ms = GetTimeChecked();
			if (ms == target) {
				return max;
			}
			if (ms < target) {
				break;
			}
			min = max;
			max = int32_t(uint32_t(max) << 1);
			if (max == 0) {
				failed = true;
				return 0;
			}
		}
		while ((min - max) > 1 && !failed) {
			const auto t = min + (max - min) / 2;
			SetTimeChecked(start);
			AddChecked(field, t);
			const auto ms = GetTimeChecked();
			if (ms == target) {
				return t;
			} else if (ms < target) {
				max = t;
			} else {
				min = t;
			}
		}
	}
	// leave the calendar at the end point
	SetTimeChecked(start);
	AddChecked(field, min);
	return min;
}

//===--------------------------------------------------------------------===//
// Actual limits
//===--------------------------------------------------------------------===//
void FieldCalendar::PrepareGetActual(CalendarField field, bool is_minimum) {
	Set(CAL_MILLISECONDS_IN_DAY, 0);

	switch (field) {
	case CAL_YEAR:
	case CAL_EXTENDED_YEAR:
		Set(CAL_DAY_OF_YEAR, GetLimit(CAL_DAY_OF_YEAR, LimitType::GREATEST_MINIMUM));
		break;
	case CAL_YEAR_WOY:
		Set(CAL_WEEK_OF_YEAR, GetLimit(CAL_WEEK_OF_YEAR, LimitType::GREATEST_MINIMUM));
		DUCKDB_EXPLICIT_FALLTHROUGH;
	case CAL_MONTH:
		Set(CAL_DATE, GetLimit(CAL_DATE, LimitType::GREATEST_MINIMUM));
		break;
	case CAL_DAY_OF_WEEK_IN_MONTH:
		// the maximum occurs for the day of week of the first of the month
		Set(CAL_DATE, 1);
		Set(CAL_DAY_OF_WEEK, Get(CAL_DAY_OF_WEEK));
		break;
	case CAL_WEEK_OF_MONTH:
	case CAL_WEEK_OF_YEAR: {
		// the last week of a month or year contains the first day of the week, and the first
		// week contains the last day of the week
		auto dow = first_day_of_week;
		if (is_minimum) {
			dow = (dow + 6) % 7;
			if (dow < CAL_SUNDAY) {
				dow += 7;
			}
		}
		Set(CAL_DAY_OF_WEEK, dow);
		break;
	}
	default:
		break;
	}

	// setting the field itself last gives it the newest stamp
	Set(field, GetLimit(field, LimitType::GREATEST_MINIMUM));
}

unique_ptr<FieldCalendar> FieldCalendar::CopyFields() const {
	auto result = Copy();
	return unique_ptr<FieldCalendar>(static_cast<FieldCalendar *>(result.release()));
}

int32_t FieldCalendar::GetActualHelper(CalendarField field, int32_t start_value, int32_t end_value) {
	if (start_value == end_value) {
		return start_value;
	}
	const auto delta = (end_value > start_value) ? 1 : -1;

	auto work = CopyFields();
	work->PrepareGetActual(field, delta < 0);
	work->Set(field, start_value);
	if (work->Get(field) != start_value) {
		return start_value;
	}
	auto result = start_value;
	do {
		start_value += delta;
		work->Add(field, delta);
		if (work->Get(field) != start_value) {
			break;
		}
		result = start_value;
	} while (start_value != end_value);
	failed = work->failed;
	return result;
}

int32_t FieldCalendar::GetActualMaximum(CalendarField field) {
	failed = false;
	return GetActualMaximumChecked(field);
}

int32_t FieldCalendar::GetActualMaximumChecked(CalendarField field) {
	if (failed) {
		return 0;
	}
	switch (field) {
	case CAL_DATE: {
		auto work = CopyFields();
		work->PrepareGetActual(field, false);
		const auto result = HandleGetMonthLength(work->Get(CAL_EXTENDED_YEAR), work->Get(CAL_MONTH));
		failed = work->failed;
		return result;
	}
	case CAL_DAY_OF_YEAR: {
		auto work = CopyFields();
		work->PrepareGetActual(field, false);
		const auto result = HandleGetYearLength(work->Get(CAL_EXTENDED_YEAR));
		failed = work->failed;
		return result;
	}
	case CAL_DAY_OF_WEEK:
	case CAL_AM_PM:
	case CAL_HOUR:
	case CAL_HOUR_OF_DAY:
	case CAL_MINUTE:
	case CAL_SECOND:
	case CAL_MILLISECOND:
	case CAL_ZONE_OFFSET:
	case CAL_DST_OFFSET:
	case CAL_DOW_LOCAL:
	case CAL_JULIAN_DAY:
	case CAL_MILLISECONDS_IN_DAY:
		return GetMaximum(field);
	default:
		return GetActualHelper(field, GetLimit(field, LimitType::LEAST_MAXIMUM), GetMaximum(field));
	}
}

int32_t FieldCalendar::GetActualMinimum(CalendarField field) {
	failed = false;
	return GetActualMinimumChecked(field);
}

int32_t FieldCalendar::GetActualMinimumChecked(CalendarField field) {
	if (failed) {
		return 0;
	}
	return GetActualHelper(field, GetLimit(field, LimitType::GREATEST_MINIMUM), GetLimit(field, LimitType::MINIMUM));
}

void FieldCalendar::PinField(CalendarField field) {
	const auto max = GetActualMaximumChecked(field);
	const auto min = GetActualMinimumChecked(field);
	if (failed) {
		return;
	}
	if (fields[field] > max) {
		Set(field, max);
	} else if (fields[field] < min) {
		Set(field, min);
	}
}

//===--------------------------------------------------------------------===//
// Calendar systems
//===--------------------------------------------------------------------===//
unique_ptr<Calendar> Calendar::TryCreate(const string &type, unique_ptr<TimeZone> zone) {
	if (!zone) {
		return nullptr;
	}
	if (type.empty() || StringUtil::CIEquals(type, "gregorian")) {
		// Postgres always assumes times are given in the proleptic Gregorian calendar
		return make_uniq<GregorianCalendar>(std::move(zone), true);
	}
	if (StringUtil::CIEquals(type, "buddhist")) {
		return make_uniq<BuddhistCalendar>(std::move(zone));
	}
	if (StringUtil::CIEquals(type, "roc")) {
		return make_uniq<ROCCalendar>(std::move(zone));
	}
	if (StringUtil::CIEquals(type, "iso8601")) {
		return make_uniq<ISO8601Calendar>(std::move(zone));
	}
	if (StringUtil::CIEquals(type, "coptic")) {
		return make_uniq<CopticCalendar>(std::move(zone));
	}
	if (StringUtil::CIEquals(type, "ethiopic")) {
		return make_uniq<EthiopicCalendar>(std::move(zone));
	}
	if (StringUtil::CIEquals(type, "ethiopic-amete-alem")) {
		return make_uniq<EthiopicAmeteAlemCalendar>(std::move(zone));
	}
	if (StringUtil::CIEquals(type, "persian")) {
		return make_uniq<PersianCalendar>(std::move(zone));
	}
	if (StringUtil::CIEquals(type, "indian")) {
		return make_uniq<IndianCalendar>(std::move(zone));
	}
	if (StringUtil::CIEquals(type, "islamic")) {
		return make_uniq<IslamicCalendar>(std::move(zone));
	}
	if (StringUtil::CIEquals(type, "islamic-rgsa")) {
		return make_uniq<IslamicRGSACalendar>(std::move(zone));
	}
	if (StringUtil::CIEquals(type, "islamic-civil")) {
		return make_uniq<IslamicCivilCalendar>(std::move(zone));
	}
	if (StringUtil::CIEquals(type, "islamic-tbla")) {
		return make_uniq<IslamicTBLACalendar>(std::move(zone));
	}
	if (StringUtil::CIEquals(type, "islamic-umalqura")) {
		return make_uniq<IslamicUmalquraCalendar>(std::move(zone));
	}
	if (StringUtil::CIEquals(type, "hebrew")) {
		return make_uniq<HebrewCalendar>(std::move(zone));
	}
	if (StringUtil::CIEquals(type, "japanese")) {
		return make_uniq<JapaneseCalendar>(std::move(zone));
	}
	if (StringUtil::CIEquals(type, "chinese")) {
		return make_uniq<ChineseCalendar>(std::move(zone));
	}
	if (StringUtil::CIEquals(type, "dangi")) {
		return make_uniq<DangiCalendar>(std::move(zone));
	}
	return nullptr;
}

const vector<string> &Calendar::GetAvailableTypes() {
	static const vector<string> TYPES = {
	    "buddhist",     "chinese",          "coptic",  "dangi",    "ethiopic",      "ethiopic-amete-alem",
	    "gregorian",    "hebrew",           "indian",  "islamic",  "islamic-civil", "islamic-rgsa",
	    "islamic-tbla", "islamic-umalqura", "iso8601", "japanese", "persian",       "roc"};
	return TYPES;
}

} // namespace datetime
} // namespace duckdb
