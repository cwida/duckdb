#include "timezone.hpp"

#include "duckdb/common/string_util.hpp"
#include "grego.hpp"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace duckdb {
namespace datetime {

//===--------------------------------------------------------------------===//
// SimpleTimeZone
//===--------------------------------------------------------------------===//
SimpleTimeZone::SimpleTimeZone(string id_p, int32_t raw_offset_p)
    : TimeZone(std::move(id_p)), raw_offset(raw_offset_p), use_daylight(false), dst_savings(0), start(), end() {
}

SimpleTimeZone::SimpleTimeZone(string id_p, int32_t raw_offset_p, const TZRule &rule)
    : TimeZone(std::move(id_p)), raw_offset(raw_offset_p), use_daylight(true),
      dst_savings(rule.dst_savings * int32_t(MILLIS_PER_SECOND)),
      start(DecodeRule(rule.start_month, rule.start_day, rule.start_day_of_week,
                       rule.start_time * int32_t(MILLIS_PER_SECOND), rule.start_time_mode)),
      end(DecodeRule(rule.end_month, rule.end_day, rule.end_day_of_week, rule.end_time * int32_t(MILLIS_PER_SECOND),
                     rule.end_time_mode)) {
	if (dst_savings == 0) {
		dst_savings = int32_t(MILLIS_PER_HOUR);
	}
}

SimpleTimeZone::Boundary SimpleTimeZone::DecodeRule(int8_t month, int8_t day, int8_t day_of_week, int32_t time,
                                                    int8_t time_mode) {
	Boundary result;
	result.month = month;
	result.day = day;
	result.day_of_week = day_of_week;
	result.time = time;
	result.time_mode = TimeMode(time_mode);
	if (day_of_week == 0) {
		result.day_mode = DayMode::DAY_OF_MONTH;
	} else if (day_of_week > 0) {
		result.day_mode = DayMode::DAY_OF_WEEK_IN_MONTH;
	} else {
		result.day_of_week = static_cast<int8_t>(-day_of_week);
		if (day > 0) {
			result.day_mode = DayMode::DAY_OF_WEEK_GE_DOM;
		} else {
			result.day = static_cast<int8_t>(-day);
			result.day_mode = DayMode::DAY_OF_WEEK_LE_DOM;
		}
	}
	return result;
}

int32_t SimpleTimeZone::CompareToRule(int8_t month, int8_t month_len, int8_t prev_month_len, int8_t dom, int8_t dow,
                                      int32_t millis, int32_t millis_delta, const Boundary &rule) {
	// shift the date so that the time can be compared against the rule directly
	millis += millis_delta;
	while (millis >= MILLIS_PER_DAY) {
		millis -= int32_t(MILLIS_PER_DAY);
		++dom;
		dow = static_cast<int8_t>(1 + (dow % 7));
		if (dom > month_len) {
			dom = 1;
			// the month is deliberately allowed to overflow past December, since the result is
			// only compared against a real month
			++month;
		}
	}
	while (millis < 0) {
		millis += int32_t(MILLIS_PER_DAY);
		--dom;
		dow = static_cast<int8_t>(1 + ((dow + 5) % 7));
		if (dom < 1) {
			dom = prev_month_len;
			--month;
		}
	}

	if (month < rule.month) {
		return -1;
	} else if (month > rule.month) {
		return 1;
	}

	// adjust the rule day for February 29 rules in non-leap years
	auto rule_day = rule.day;
	if (rule_day > month_len) {
		rule_day = month_len;
	}

	int32_t rule_dom = 0;
	switch (rule.day_mode) {
	case DayMode::DAY_OF_MONTH:
		rule_dom = rule_day;
		break;
	case DayMode::DAY_OF_WEEK_IN_MONTH:
		// the day of week of the first of the month follows from the given day and day of week
		if (rule_day > 0) {
			rule_dom = 1 + (rule_day - 1) * 7 + (7 + rule.day_of_week - (dow - dom + 1)) % 7;
		} else {
			rule_dom = month_len + (rule_day + 1) * 7 - (7 + (dow + month_len - dom) - rule.day_of_week) % 7;
		}
		break;
	case DayMode::DAY_OF_WEEK_GE_DOM:
		rule_dom = rule_day + (49 + rule.day_of_week - rule_day - dow + dom) % 7;
		break;
	case DayMode::DAY_OF_WEEK_LE_DOM:
		// note that this can be less than 1 for rules that are not well-formed
		rule_dom = rule_day - (49 - rule.day_of_week + rule_day + dow - dom) % 7;
		break;
	}

	if (dom < rule_dom) {
		return -1;
	} else if (dom > rule_dom) {
		return 1;
	}
	if (millis < rule.time) {
		return -1;
	} else if (millis > rule.time) {
		return 1;
	}
	return 0;
}

int32_t SimpleTimeZone::GetOffsetForFields(int32_t year, int8_t month, int8_t dom, int8_t dow, int32_t millis) const {
	if (!use_daylight || year < 0) {
		return raw_offset;
	}

	const auto month_len = Grego::MonthLength(year, month);
	const auto prev_month_len = Grego::PreviousMonthLength(year, month);

	// in the southern hemisphere the daylight savings period wraps around the end of the year
	const auto southern = start.month > end.month;

	const auto start_compare = CompareToRule(month, month_len, prev_month_len, dom, dow, millis,
	                                         start.time_mode == TimeMode::UTC ? -raw_offset : 0, start);
	int32_t end_compare = 0;
	// in the northern hemisphere a date before the start rule can never be in the daylight savings
	// period, and in the southern hemisphere a date after the start rule always is
	if (southern != (start_compare >= 0)) {
		const auto delta = end.time_mode == TimeMode::WALL  ? dst_savings
		                   : end.time_mode == TimeMode::UTC ? -raw_offset
		                                                    : 0;
		end_compare = CompareToRule(month, month_len, prev_month_len, dom, dow, millis, delta, end);
	}

	if ((!southern && (start_compare >= 0 && end_compare < 0)) ||
	    (southern && (start_compare >= 0 || end_compare < 0))) {
		return raw_offset + dst_savings;
	}
	return raw_offset;
}

void SimpleTimeZone::GetOffset(int64_t millis, int32_t &raw_offset_out, int32_t &dst_offset_out) const {
	raw_offset_out = raw_offset;
	dst_offset_out = 0;

	// convert to local standard time
	millis += raw_offset;

	int32_t year;
	int8_t month, dom, dow;
	int16_t doy;
	int32_t mid;
	if (!Grego::TimeToFields(millis, year, month, dom, dow, doy, mid)) {
		return;
	}
	dst_offset_out = GetOffsetForFields(year, month, dom, dow, mid) - raw_offset;
}

void SimpleTimeZone::GetOffsetFromLocal(int64_t millis, LocalOption non_existing, LocalOption duplicated,
                                        int32_t &raw_offset_out, int32_t &dst_offset_out) const {
	raw_offset_out = raw_offset;
	dst_offset_out = 0;

	int32_t year;
	int8_t month, dom, dow;
	int16_t doy;
	int32_t mid;
	if (!Grego::TimeToFields(millis, year, month, dom, dow, doy, mid)) {
		return;
	}
	dst_offset_out = GetOffsetForFields(year, month, dom, dow, mid) - raw_offset;

	// the local time was interpreted as daylight savings time, so shift it if the caller asked for
	// the offsets before the transition instead - and the other way around for duplicated times
	const auto recalculate =
	    dst_offset_out > 0 ? non_existing == LocalOption::FORMER : duplicated == LocalOption::FORMER;
	if (!recalculate) {
		return;
	}
	millis -= dst_savings;
	if (!Grego::TimeToFields(millis, year, month, dom, dow, doy, mid)) {
		return;
	}
	dst_offset_out = GetOffsetForFields(year, month, dom, dow, mid) - raw_offset;
}

int64_t SimpleTimeZone::GetRuleTime(int32_t year, const Boundary &rule, bool is_start) const {
	const int32_t month_len = Grego::MonthLength(year, rule.month);
	// adjust the rule day for February 29 rules in non-leap years
	const int32_t rule_day = MinValue<int32_t>(rule.day, month_len);
	const auto first_day = Grego::FieldsToDay(year, rule.month, 1);

	int32_t dom = 0;
	switch (rule.day_mode) {
	case DayMode::DAY_OF_MONTH:
		dom = rule_day;
		break;
	case DayMode::DAY_OF_WEEK_IN_MONTH:
		if (rule_day > 0) {
			const auto first_dow = Grego::DayOfWeek(int32_t(first_day));
			dom = 1 + (rule_day - 1) * 7 + (7 + rule.day_of_week - first_dow) % 7;
		} else {
			const auto last_dow = Grego::DayOfWeek(int32_t(first_day + month_len - 1));
			dom = month_len + (rule_day + 1) * 7 - (7 + last_dow - rule.day_of_week) % 7;
		}
		break;
	case DayMode::DAY_OF_WEEK_GE_DOM: {
		const auto day_dow = Grego::DayOfWeek(int32_t(first_day + rule_day - 1));
		dom = rule_day + (7 + rule.day_of_week - day_dow) % 7;
		break;
	}
	case DayMode::DAY_OF_WEEK_LE_DOM: {
		const auto day_dow = Grego::DayOfWeek(int32_t(first_day + rule_day - 1));
		dom = rule_day - (7 + day_dow - rule.day_of_week) % 7;
		break;
	}
	}

	const auto local = (first_day + dom - 1) * MILLIS_PER_DAY + rule.time;
	switch (rule.time_mode) {
	case TimeMode::UTC:
		return local;
	case TimeMode::STANDARD:
		return local - raw_offset;
	default:
		// the wall clock runs on standard time before the start and on daylight time before the end
		return local - raw_offset - (is_start ? 0 : dst_savings);
	}
}

bool SimpleTimeZone::TryGetNextTransition(int64_t millis, int64_t &transition) const {
	if (!use_daylight) {
		return false;
	}
	int32_t year;
	int8_t month, dom, dow;
	int16_t doy;
	int32_t mid;
	if (!Grego::TimeToFields(millis, year, month, dom, dow, doy, mid)) {
		return false;
	}
	// every year has both transitions, so the next one is at most a year away. The rule does not apply before
	// year 0, so the first transition is in year 0.
	bool found = false;
	const auto first_year = MaxValue<int32_t>(year - 1, 0);
	for (auto candidate_year = first_year; candidate_year <= first_year + 2; candidate_year++) {
		for (auto is_start : {true, false}) {
			const auto candidate = GetRuleTime(candidate_year, is_start ? start : end, is_start);
			if (candidate > millis && (!found || candidate < transition)) {
				transition = candidate;
				found = true;
			}
		}
	}
	return found;
}

unique_ptr<TimeZone> SimpleTimeZone::Copy() const {
	return unique_ptr<TimeZone>(new SimpleTimeZone(*this));
}

//===--------------------------------------------------------------------===//
// OlsonTimeZone
//===--------------------------------------------------------------------===//
OlsonTimeZone::OlsonTimeZone(string id_p, const TZZoneData &data_p)
    : TimeZone(std::move(id_p)), data(data_p), final_start_millis(0) {
	if (data.final_rule < 0) {
		return;
	}
	final_zone = make_uniq<SimpleTimeZone>(string(), data.final_raw * int32_t(MILLIS_PER_SECOND),
	                                       GetTZData().rules[data.final_rule]);
	// the final rule applies from the start of its first year onwards
	final_start_millis = Grego::FieldsToDay(data.final_year, 0, 1) * MILLIS_PER_DAY;
}

const TZTypeOffset &OlsonTimeZone::OffsetsAt(int32_t transition_index) const {
	const auto type = transition_index >= 0 ? data.type_map[transition_index] : 0;
	return data.type_offsets[type];
}

void OlsonTimeZone::GetHistoricalOffset(int64_t millis, bool local, LocalOption non_existing, LocalOption duplicated,
                                        int32_t &raw_offset, int32_t &dst_offset) const {
	// the maximum absolute offset of a zone, used as a safety margin when scanning the transitions
	static constexpr int64_t MAX_OFFSET_SECONDS = 86400;

	const auto transition_count = int32_t(data.transition_count);
	int32_t transition_index = -1;
	if (transition_count > 0) {
		const auto sec = FloorDiv::Divide(millis, MILLIS_PER_SECOND);
		if (local || sec >= data.transitions[0]) {
			// searching backwards is the fastest approach, since most lookups are at or near the end
			for (transition_index = transition_count - 1; transition_index >= 0; transition_index--) {
				auto transition = data.transitions[transition_index];

				if (local && sec >= transition - MAX_OFFSET_SECONDS) {
					const auto &before = OffsetsAt(transition_index - 1);
					const auto &after = OffsetsAt(transition_index);
					const auto offset_before = before.raw_offset + before.dst_offset;
					const auto offset_after = after.raw_offset + after.dst_offset;

					if (offset_after - offset_before >= 0) {
						// a positive transition creates a local time range that does not exist
						transition += non_existing == LocalOption::LATTER ? offset_before : offset_after;
					} else {
						// a negative transition creates a local time range that occurs twice
						transition += duplicated == LocalOption::FORMER ? offset_before : offset_after;
					}
				}
				if (sec >= transition) {
					break;
				}
			}
		}
	}
	const auto &offsets = OffsetsAt(transition_index);
	raw_offset = offsets.raw_offset * int32_t(MILLIS_PER_SECOND);
	dst_offset = offsets.dst_offset * int32_t(MILLIS_PER_SECOND);
}

void OlsonTimeZone::GetOffset(int64_t millis, int32_t &raw_offset, int32_t &dst_offset) const {
	if (final_zone && millis >= final_start_millis) {
		final_zone->GetOffset(millis, raw_offset, dst_offset);
	} else {
		GetHistoricalOffset(millis, false, LocalOption::FORMER, LocalOption::LATTER, raw_offset, dst_offset);
	}
}

void OlsonTimeZone::GetOffsetFromLocal(int64_t millis, LocalOption non_existing, LocalOption duplicated,
                                       int32_t &raw_offset, int32_t &dst_offset) const {
	if (final_zone && millis >= final_start_millis) {
		final_zone->GetOffsetFromLocal(millis, non_existing, duplicated, raw_offset, dst_offset);
	} else {
		GetHistoricalOffset(millis, true, non_existing, duplicated, raw_offset, dst_offset);
	}
}

bool OlsonTimeZone::TryGetNextTransition(int64_t millis, int64_t &transition) const {
	if (final_zone && millis >= final_start_millis) {
		return final_zone->TryGetNextTransition(millis, transition);
	}
	const auto sec = FloorDiv::Divide(millis, MILLIS_PER_SECOND);
	const auto end = data.transitions + data.transition_count;
	// a transition at second 'sec' starts at or before 'millis'
	const auto next = std::upper_bound(data.transitions, end, sec);
	if (next != end && (!final_zone || *next * MILLIS_PER_SECOND < final_start_millis)) {
		transition = *next * MILLIS_PER_SECOND;
		return true;
	}
	if (final_zone) {
		// the switch to the recurring rule can change the offsets as well
		transition = final_start_millis;
		return true;
	}
	return false;
}

unique_ptr<TimeZone> OlsonTimeZone::Copy() const {
	return make_uniq<OlsonTimeZone>(id, data);
}

//===--------------------------------------------------------------------===//
// Lookup
//===--------------------------------------------------------------------===//
//! Finds the entry of a zone in the (lexicographically sorted) zone table, or nullptr
static const TZZone *FindZone(const string &id) {
	const auto &tz = GetTZData();
	idx_t lower = 0;
	idx_t upper = tz.zone_count;
	while (lower < upper) {
		const auto middle = (lower + upper) / 2;
		const auto comparison = id.compare(tz.zones[middle].name);
		if (comparison == 0) {
			return tz.zones + middle;
		} else if (comparison < 0) {
			upper = middle;
		} else {
			lower = middle + 1;
		}
	}
	return nullptr;
}

//! Parses the offset of a GMT[+-]hh[:mm[:ss]] identifier, returning false if it is not one
static bool TryParseCustomOffset(const string &id, int32_t &offset) {
	static constexpr idx_t GMT_LENGTH = 3;
	// the maximum offset that can be expressed is 23:59:59
	static constexpr int32_t MAX_HOUR = 23;
	static constexpr int32_t MAX_MINUTE = 59;
	static constexpr int32_t MAX_SECOND = 59;

	if (id.size() < GMT_LENGTH + 2 || !StringUtil::CIEquals(id.substr(0, GMT_LENGTH), "GMT")) {
		return false;
	}
	int32_t sign;
	if (id[GMT_LENGTH] == '-') {
		sign = -1;
	} else if (id[GMT_LENGTH] == '+') {
		sign = 1;
	} else {
		return false;
	}

	//! Reads a run of digits, returning the value and the number of digits read
	auto read_number = [&](idx_t &pos, int32_t &result) {
		const auto start = pos;
		result = 0;
		while (pos < id.size() && StringUtil::CharacterIsDigit(id[pos])) {
			result = result * 10 + (id[pos] - '0');
			++pos;
			if (pos - start > 6) {
				return idx_t(0);
			}
		}
		return pos - start;
	};

	idx_t pos = GMT_LENGTH + 1;
	int32_t hour = 0;
	int32_t minute = 0;
	int32_t second = 0;
	auto digits = read_number(pos, hour);
	if (pos == id.size()) {
		// the digits encode H, HH, Hmm, HHmm, Hmmss or HHmmss
		switch (digits) {
		case 1:
		case 2:
			break;
		case 3:
		case 4:
			minute = hour % 100;
			hour /= 100;
			break;
		case 5:
		case 6:
			second = hour % 100;
			minute = (hour / 100) % 100;
			hour /= 10000;
			break;
		default:
			return false;
		}
	} else {
		// the remainder is [H]H:mm[:ss]
		if (digits < 1 || digits > 2 || id[pos] != ':') {
			return false;
		}
		++pos;
		if (read_number(pos, minute) != 2) {
			return false;
		}
		if (pos < id.size()) {
			if (id[pos] != ':') {
				return false;
			}
			++pos;
			if (read_number(pos, second) != 2 || pos < id.size()) {
				return false;
			}
		}
	}
	if (hour > MAX_HOUR || minute > MAX_MINUTE || second > MAX_SECOND) {
		return false;
	}
	offset = sign * ((hour * 60 + minute) * 60 + second) * int32_t(MILLIS_PER_SECOND);
	return true;
}

unique_ptr<TimeZone> TimeZone::TryCreate(const string &id) {
	const auto zone = FindZone(id);
	if (zone) {
		return make_uniq<OlsonTimeZone>(id, GetTZData().zone_data[zone->data_index]);
	}
	int32_t offset;
	if (TryParseCustomOffset(id, offset)) {
		return make_uniq<SimpleTimeZone>(id, offset);
	}
	return nullptr;
}

const vector<string> &TimeZone::GetAvailableIds() {
	static const auto IDS = []() {
		vector<string> result;
		const auto &tz = GetTZData();
		for (idx_t i = 0; i < tz.zone_count; i++) {
			// Etc/Unknown is the placeholder for an unidentified zone, so it is not listed
			// even though it can be looked up
			if (StringUtil::Equals(tz.zones[i].name, "Etc/Unknown")) {
				continue;
			}
			result.emplace_back(tz.zones[i].name);
		}
		return result;
	}();
	return IDS;
}

vector<string> TimeZone::GetEquivalentIds(const string &id) {
	vector<string> result;
	const auto zone = FindZone(id);
	if (!zone) {
		return result;
	}
	const auto &tz = GetTZData();
	const auto &data = tz.zone_data[zone->data_index];
	for (idx_t i = 0; i < data.link_count; i++) {
		result.emplace_back(tz.zones[data.links[i]].name);
	}
	return result;
}

#ifdef _WIN32
//! The zone that a Windows time zone name maps to, preferring the region of the host
static unique_ptr<TimeZone> TryCreateFromWindowsName(const string &name, const string &region) {
	const TZWindowsZone *fallback = nullptr;
	const auto &tz = GetTZData();
	for (idx_t i = 0; i < tz.windows_zone_count; i++) {
		const auto &entry = tz.windows_zones[i];
		if (name != entry.windows_name) {
			continue;
		}
		if (region == entry.region) {
			return TimeZone::TryCreate(entry.zone);
		}
		if (StringUtil::Equals(entry.region, "001")) {
			fallback = &entry;
		}
	}
	return fallback ? TimeZone::TryCreate(fallback->zone) : nullptr;
}

//! The time zone of the host on Windows, which names its zones differently
static unique_ptr<TimeZone> TryCreateWindowsDefault() {
	DYNAMIC_TIME_ZONE_INFORMATION info;
	memset(&info, 0, sizeof(info));
	SYSTEMTIME zeroed;
	memset(&zeroed, 0, sizeof(zeroed));
	if (GetDynamicTimeZoneInformation(&info) == TIME_ZONE_ID_INVALID) {
		return nullptr;
	}

	// daylight savings time has been switched off, which leaves a fixed offset. This is how the
	// control panel itself decides that the setting is off.
	if (info.DynamicDaylightTimeDisabled != 0 &&
	    memcmp(&info.StandardDate, &info.DaylightDate, sizeof(info.StandardDate)) == 0 &&
	    ((info.TimeZoneKeyName[0] != L'\0' && memcmp(&info.StandardDate, &zeroed, sizeof(zeroed)) == 0) ||
	     (info.TimeZoneKeyName[0] == L'\0' && memcmp(&info.StandardDate, &zeroed, sizeof(zeroed)) != 0))) {
		const auto offset_minutes = info.Bias;
		if (offset_minutes == 0) {
			return TimeZone::TryCreate("Etc/UTC");
		}
		// the bias already follows the sign convention of the Etc zones, so it is not negated
		if (offset_minutes % 60 == 0) {
			char zone[16];
			snprintf(zone, sizeof(zone), "Etc/GMT%+ld", long(offset_minutes / 60));
			auto result = TimeZone::TryCreate(zone);
			if (result) {
				return result;
			}
		}
	}

	if (info.TimeZoneKeyName[0] == L'\0') {
		return nullptr;
	}
	// the key name holds invariant characters only, so it converts directly
	string name;
	for (idx_t i = 0; i < sizeof(info.TimeZoneKeyName) / sizeof(WCHAR) && info.TimeZoneKeyName[i]; i++) {
		name += char(info.TimeZoneKeyName[i]);
	}

	// a Windows zone can cover several regions, which each have their own zone
	string region;
	wchar_t region_code[3] = {};
	const auto length =
	    GetGeoInfoW(GetUserGeoID(GEOCLASS_NATION), GEO_ISO2, region_code, sizeof(region_code) / sizeof(wchar_t), 0);
	if (length != 0) {
		for (idx_t i = 0; i < 2 && region_code[i]; i++) {
			region += char(region_code[i]);
		}
	}
	return TryCreateFromWindowsName(name, region);
}
#endif

unique_ptr<TimeZone> TimeZone::TryCreateDefault() {
	// an explicitly configured zone takes priority over the host configuration
	const auto tz_env = std::getenv("TZ");
	if (tz_env) {
		string tz_id(tz_env);
		// the zone can be prefixed with a colon to force it to be interpreted as a name
		if (!tz_id.empty() && tz_id[0] == ':') {
			tz_id = tz_id.substr(1);
		}
		auto result = TryCreate(tz_id);
		if (result) {
			return result;
		}
	}
#ifdef _WIN32
	auto windows_zone = TryCreateWindowsDefault();
	if (windows_zone) {
		return windows_zone;
	}
#else
	// the host zone is the zone info file that /etc/localtime points at
	char buffer[PATH_MAX];
	const auto length = readlink("/etc/localtime", buffer, sizeof(buffer) - 1);
	if (length > 0) {
		string path(buffer, idx_t(length));
		const auto pos = path.find("zoneinfo/");
		if (pos != string::npos) {
			auto result = TryCreate(path.substr(pos + 9));
			if (result) {
				return result;
			}
		}
	}
#endif
	return nullptr;
}

} // namespace datetime
} // namespace duckdb
