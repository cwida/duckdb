#include "duckdb/main/capi/capi_internal.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"

using duckdb::Date;
using duckdb::Time;
using duckdb::Timestamp;

using duckdb::date_t;
using duckdb::dtime_t;
using duckdb::timestamp_ms_t;
using duckdb::timestamp_ns_t;
using duckdb::timestamp_sec_t;
using duckdb::timestamp_t;

duckdb_date_struct duckdb_from_date(duckdb_date date) {
	int32_t year, month, day;
	Date::Convert(date_t(date.days), year, month, day);

	duckdb_date_struct result;
	result.year = year;
	result.month = duckdb::UnsafeNumericCast<int8_t>(month);
	result.day = duckdb::UnsafeNumericCast<int8_t>(day);
	return result;
}

duckdb_date duckdb_to_date(duckdb_date_struct date) {
	date_t internal_result;
	if (!Date::TryFromDate(date.year, date.month, date.day, internal_result)) {
		internal_result = date_t(0);
	}

	duckdb_date result;
	result.days = internal_result.days;
	return result;
}

bool duckdb_is_finite_date(duckdb_date date) {
	return date_t(date.days).IsFinite();
}

duckdb_time_struct duckdb_from_time(duckdb_time time) {
	int32_t hour, minute, second, micros;
	Time::Convert(dtime_t(time.micros), hour, minute, second, micros);

	duckdb_time_struct result;
	result.hour = duckdb::UnsafeNumericCast<int8_t>(hour);
	result.min = duckdb::UnsafeNumericCast<int8_t>(minute);
	result.sec = duckdb::UnsafeNumericCast<int8_t>(second);
	result.micros = micros;
	return result;
}

duckdb_time_tz_struct duckdb_from_time_tz(duckdb_time_tz input) {
	duckdb_time_tz_struct result;
	duckdb_time time;

	duckdb::dtime_tz_t time_tz(input.bits);

	time.micros = time_tz.time().value;

	result.time = duckdb_from_time(time);
	result.offset = time_tz.offset();
	return result;
}

duckdb_time_tz duckdb_create_time_tz(int64_t micros, int32_t offset) {
	duckdb_time_tz time;
	time.bits = duckdb::dtime_tz_t(duckdb::dtime_t(micros), offset).bits;
	return time;
}

duckdb_time duckdb_to_time(duckdb_time_struct time) {
	duckdb_time result;
	result.micros = Time::FromTime(time.hour, time.min, time.sec, time.micros).value;
	return result;
}

duckdb_timestamp_struct duckdb_from_timestamp(duckdb_timestamp ts) {
	const timestamp_t timestamp(ts.micros);
	date_t date = Timestamp::GetDate(timestamp);
	dtime_t time(0);
	// an infinite timestamp has no time of day, it gets the parts of the infinite date as this function cannot throw
	if (timestamp.IsFinite()) {
		Timestamp::Convert(timestamp, date, time);
	}

	duckdb_date ddate;
	ddate.days = date.days;

	duckdb_time dtime;
	dtime.micros = time.value;

	duckdb_timestamp_struct result;
	result.date = duckdb_from_date(ddate);
	result.time = duckdb_from_time(dtime);
	return result;
}

duckdb_timestamp duckdb_to_timestamp(duckdb_timestamp_struct ts) {
	date_t date;
	dtime_t time = dtime_t(duckdb_to_time(ts.time).micros);
	timestamp_t internal_result;
	if (!Date::TryFromDate(ts.date.year, ts.date.month, ts.date.day, date) ||
	    !Timestamp::TryFromDatetime(date, time, internal_result)) {
		internal_result = timestamp_t(0);
	}

	duckdb_timestamp result;
	result.micros = internal_result.value;
	return result;
}

bool duckdb_is_finite_timestamp(duckdb_timestamp ts) {
	return timestamp_t(ts.micros).IsFinite();
}

bool duckdb_is_finite_timestamp_s(duckdb_timestamp_s ts) {
	return timestamp_sec_t(ts.seconds).IsFinite();
}

bool duckdb_is_finite_timestamp_ms(duckdb_timestamp_ms ts) {
	return timestamp_ms_t(ts.millis).IsFinite();
}

bool duckdb_is_finite_timestamp_ns(duckdb_timestamp_ns ts) {
	return timestamp_ns_t(ts.nanos).IsFinite();
}
