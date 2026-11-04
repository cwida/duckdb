#include "include/icu-datefunc.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/common/operator/add.hpp"
#include "duckdb/common/operator/multiply.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/exception/conversion_exception.hpp"
#include "icu-helpers.hpp"

namespace duckdb {

ICUDateFunc::BindData::BindData(const BindData &other)
    : tz_setting(other.tz_setting), cal_setting(other.cal_setting), calendar(other.calendar->Copy()) {
}

ICUDateFunc::BindData::BindData(const string &tz_setting_p, const string &cal_setting_p)
    : tz_setting(tz_setting_p), cal_setting(cal_setting_p) {
	InitCalendar();
}

ICUDateFunc::BindData::BindData(ClientContext &context) : cal_setting("gregorian") {
	context.TryGetCurrentSetting("TimeZone", tz_setting);
	context.TryGetCurrentSetting("Calendar", cal_setting);

	InitCalendar();
}

void ICUDateFunc::BindData::InitCalendar() {
	//	Postgres always assumes times are given in the proleptic Gregorian calendar,
	//	which is what our Gregorian calendar implements.
	auto tz = TimeZone::TryCreate(tz_setting);
	calendar = Calendar::TryCreate(cal_setting, tz ? std::move(tz) : TimeZone::TryCreate("UTC"));
	if (!calendar) {
		throw InternalException("Unable to create calendar.");
	}
}

bool ICUDateFunc::BindData::Equals(const FunctionData &other_p) const {
	auto &other = other_p.Cast<const BindData>();
	return calendar->Equals(*other.calendar);
}

unique_ptr<FunctionData> ICUDateFunc::BindData::Copy() const {
	return make_uniq<BindData>(*this);
}

unique_ptr<FunctionData> ICUDateFunc::Bind(BindScalarFunctionInput &input) {
	return make_uniq<BindData>(input.GetClientContext());
}

unique_ptr<ICUDateFunc::CastData> ICUDateFunc::BindCastData(BindCastInput &input) {
	input.stability = FunctionStability::CONSISTENT_WITHIN_QUERY;
	return make_uniq<CastData>(make_uniq<BindData>(*input.context));
}

const ICUDateFunc::CalendarCacheState::CacheEntry &ICUDateFunc::CalendarCacheState::GetEntry(const string_t &tz_id) {
	if (last && last->first.size() == tz_id.GetSize() &&
	    memcmp(last->first.data(), tz_id.GetData(), tz_id.GetSize()) == 0) {
		return last->second;
	}
	auto tz_str = tz_id.GetString();
	auto entry = cache.find(tz_str);
	if (entry == cache.end()) {
		CacheEntry new_entry;
		//	GetTimeZone may normalize the name - keep the original as the cache key
		auto resolved = tz_str;
		new_entry.tz = ICUHelpers::GetTimeZone(resolved, &new_entry.error);
		if (new_entry.tz) {
			new_entry.calendar = base_calendar->Copy();
			new_entry.calendar->SetTimeZone(new_entry.tz->Copy());
		}
		entry = cache.emplace(std::move(tz_str), std::move(new_entry)).first;
	}
	last = &*entry;
	return entry->second;
}

Calendar *ICUDateFunc::CalendarCacheState::GetCalendar(const string_t &tz_id) {
	auto &entry = GetEntry(tz_id);
	if (!entry.calendar) {
		throw NotImplementedException(entry.error);
	}
	return entry.calendar.get();
}

Calendar *ICUDateFunc::CalendarCacheState::TryGetCalendar(const string_t &tz_id, string *error_message) {
	auto &entry = GetEntry(tz_id);
	if (!entry.calendar) {
		if (error_message) {
			*error_message = entry.error;
		}
		return nullptr;
	}
	return entry.calendar.get();
}

void ICUDateFunc::CalendarCacheState::SetTimeZone(Calendar *calendar, const string_t &tz_id, string *error_message) {
	auto &entry = GetEntry(tz_id);
	if (!entry.tz) {
		if (error_message) {
			*error_message = entry.error;
			return;
		}
		throw NotImplementedException(entry.error);
	}
	calendar->SetTimeZone(entry.tz->Copy());
}

bool ICUDateFunc::CalendarCacheState::TrySetTimeZone(Calendar *calendar, const string_t &tz_id) {
	auto &entry = GetEntry(tz_id);
	if (!entry.tz) {
		return false;
	}
	calendar->SetTimeZone(entry.tz->Copy());
	return true;
}

unique_ptr<FunctionLocalState>
ICUDateFunc::InitCalendarCache(ExpressionState &state, const BoundFunctionExpression &expr, FunctionData *bind_data) {
	auto &info = bind_data->Cast<BindData>();
	return make_uniq<CalendarCacheState>(*info.calendar);
}

unique_ptr<FunctionLocalState> ICUDateFunc::InitCastCalendarCache(CastLocalStateParameters &parameters) {
	auto &cast_data = parameters.cast_data->Cast<CastData>();
	auto &info = cast_data.info->Cast<BindData>();
	return make_uniq<CalendarCacheState>(*info.calendar);
}

timestamp_tz_t ICUDateFunc::GetTimeUnsafe(Calendar *calendar, uint64_t micros) {
	// Extract the new time
	const auto millis = calendar->GetTime();
	if (calendar->HasFailed()) {
		throw InternalException("Unable to get calendar time.");
	}
	// The time is a whole number of milliseconds, so converting it back to microseconds can overflow.
	int64_t result;
	if (!Timestamp::TryCombineUnits(millis, int64_t(micros), Interval::MICROS_PER_MSEC, result)) {
		throw OutOfRangeException("Unable to get calendar time: timestamp out of range");
	}
	return timestamp_tz_t(result);
}

bool ICUDateFunc::TryGetTime(Calendar *calendar, uint64_t micros, timestamp_tz_t &result) {
	// Extract the new time
	auto millis = calendar->GetTime();
	if (calendar->HasFailed()) {
		return false;
	}

	// The time is a whole number of milliseconds, so converting it back to microseconds can overflow.
	if (!Timestamp::TryCombineUnits(millis, int64_t(micros), Interval::MICROS_PER_MSEC, result.value)) {
		return false;
	}
	return result.IsFinite();
}

timestamp_tz_t ICUDateFunc::GetTime(Calendar *calendar, uint64_t micros) {
	timestamp_tz_t result;
	if (!TryGetTime(calendar, micros, result)) {
		throw ConversionException("ICU date overflows timestamp range");
	}
	return result;
}

bool ICUDateFunc::TryGetTimeNS(Calendar *calendar, uint64_t nanos, timestamp_tz_ns_t &result) {
	timestamp_tz_t tstz_micros;
	if (!TryGetTime(calendar, nanos / Interval::NANOS_PER_MICRO, tstz_micros)) {
		return false;
	}

	nanos %= Interval::NANOS_PER_MICRO;
	timestamp_t us(tstz_micros);
	timestamp_ns_t ns;
	if (!Timestamp::TryFromTimestampNanos(us, int32_t(nanos), ns)) {
		return false;
	}
	result.value = ns.value;
	return true;
}

timestamp_tz_ns_t ICUDateFunc::GetTimeNS(Calendar *calendar, uint64_t nanos) {
	timestamp_tz_ns_t result;
	if (!TryGetTimeNS(calendar, nanos, result)) {
		throw ConversionException("ICU date overflows timestamp_ns range");
	}
	return result;
}

uint64_t ICUDateFunc::SetTime(Calendar *calendar, timestamp_tz_t date) {
	int64_t millis = date.value / Interval::MICROS_PER_MSEC;
	int64_t micros = date.value % Interval::MICROS_PER_MSEC;
	if (micros < 0) {
		--millis;
		micros += Interval::MICROS_PER_MSEC;
	}

	calendar->SetTime(millis);
	return uint64_t(micros);
}

uint64_t ICUDateFunc::SetTimeNS(Calendar *calendar, timestamp_tz_ns_t date) {
	int64_t millis = date.value / Interval::NANOS_PER_MSEC;
	int64_t nanos = date.value % Interval::NANOS_PER_MSEC;
	if (nanos < 0) {
		--millis;
		nanos += Interval::NANOS_PER_MSEC;
	}

	calendar->SetTime(millis);
	return uint64_t(nanos);
}

int32_t ICUDateFunc::ExtractField(Calendar *calendar, CalendarField field) {
	const auto result = calendar->Get(field);
	// a date the calendar cannot represent is something the query asked for, not a defect
	if (calendar->HasFailed()) {
		throw ConversionException("Unable to extract calendar part");
	}
	return result;
}

bool ICUDateFunc::TryGetTimeTZOffset(Calendar *calendar, int32_t &offset) {
	offset = ExtractField(calendar, CAL_ZONE_OFFSET);
	offset += ExtractField(calendar, CAL_DST_OFFSET);
	offset /= Interval::MSECS_PER_SEC;
	return offset >= dtime_tz_t::MIN_OFFSET && offset <= dtime_tz_t::MAX_OFFSET;
}

int32_t ICUDateFunc::GetTimeTZOffset(Calendar *calendar) {
	int32_t offset;
	if (!TryGetTimeTZOffset(calendar, offset)) {
		throw OutOfRangeException(TimeTZOffsetError(offset));
	}
	return offset;
}

string ICUDateFunc::TimeTZOffsetError(int32_t offset) {
	interval_t interval {0, 0, offset * Interval::MICROS_PER_SEC};
	return StringUtil::Format("Time zone offset %s is out of range, expected a value between -15:59:59 and +15:59:59",
	                          Interval::ToString(interval));
}

int32_t ICUDateFunc::SubtractField(Calendar *calendar, CalendarField field, timestamp_tz_t end_date) {
	const int64_t millis = end_date.value / Interval::MICROS_PER_MSEC;
	const auto sub = calendar->FieldDifference(millis, field);
	if (calendar->HasFailed()) {
		throw ConversionException("Unable to subtract calendar part");
	}
	return sub;
}

} // namespace duckdb
