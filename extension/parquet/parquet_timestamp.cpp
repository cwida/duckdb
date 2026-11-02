#include "parquet_timestamp.hpp"

#include "duckdb/common/operator/add.hpp"
#include "duckdb/common/operator/multiply.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/operator/add.hpp"
#include "duckdb/common/operator/multiply.hpp"

namespace duckdb {

// surely they are joking
static constexpr int64_t JULIAN_TO_UNIX_EPOCH_DAYS = 2440588LL;
static constexpr int64_t MILLISECONDS_PER_DAY = 86400000LL;
static constexpr int64_t MICROSECONDS_PER_DAY = MILLISECONDS_PER_DAY * 1000LL;
static constexpr int64_t NANOSECONDS_PER_MICRO = 1000LL;
static constexpr int64_t NANOSECONDS_PER_DAY = MICROSECONDS_PER_DAY * 1000LL;

static inline int64_t ImpalaTimestampToDays(const Int96 &impala_timestamp) {
	return impala_timestamp.value[2] - JULIAN_TO_UNIX_EPOCH_DAYS;
}

static int64_t ImpalaTimestampToMicroseconds(const Int96 &impala_timestamp) {
	int64_t days_since_epoch = ImpalaTimestampToDays(impala_timestamp);
	auto nanoseconds = Load<int64_t>(const_data_ptr_cast(impala_timestamp.value));
	auto microseconds = nanoseconds / NANOSECONDS_PER_MICRO;
	int64_t result;
	if (!TryMultiplyOperator::Operation<int64_t, int64_t, int64_t>(days_since_epoch, MICROSECONDS_PER_DAY, result) ||
	    !TryAddOperator::Operation<int64_t, int64_t, int64_t>(result, microseconds, result)) {
		throw InvalidInputException("Int96 timestamp is out of range");
	}
	return result;
}

static int64_t ImpalaTimestampToNanoseconds(const Int96 &impala_timestamp) {
	int64_t result;
	int64_t days_since_epoch = ImpalaTimestampToDays(impala_timestamp);
	auto nanoseconds = Load<int64_t>(const_data_ptr_cast(impala_timestamp.value));
	int64_t day_nanoseconds;
	if (days_since_epoch < 0) {
		//	Don't saturate the minimum value by going past it
		if (!TryMultiplyOperator::Operation(days_since_epoch + 1, NANOSECONDS_PER_DAY, day_nanoseconds) ||
		    !TryAddOperator::Operation(day_nanoseconds, nanoseconds - NANOSECONDS_PER_DAY, result)) {
			// out of range for TIMESTAMP_NS - saturate to -infinity
			return timestamp_ns_t::ninfinity().value;
		}
		return result;
	}
	if (!TryMultiplyOperator::Operation(days_since_epoch, NANOSECONDS_PER_DAY, day_nanoseconds) ||
	    !TryAddOperator::Operation(day_nanoseconds, nanoseconds, result)) {
		// out of range for TIMESTAMP_NS - saturate to +infinity
		return timestamp_ns_t::infinity().value;
	}
	return result;
}

timestamp_ns_t ImpalaTimestampToTimestampNS(const Int96 &raw_ts) {
	timestamp_ns_t result;
	result.value = ImpalaTimestampToNanoseconds(raw_ts);
	return result;
}

timestamp_t ImpalaTimestampToTimestamp(const Int96 &raw_ts) {
	auto impala_us = ImpalaTimestampToMicroseconds(raw_ts);
	return Timestamp::FromEpochMicroSeconds(impala_us);
}

date_t ImpalaTimestampToDate(const Int96 &raw_ts) {
	return date_t(NumericCast<int32_t>(ImpalaTimestampToDays(raw_ts)));
}

dtime_ns_t ImpalaTimestampToTimeNs(const Int96 &raw_ts) {
	return dtime_ns_t(Load<int64_t>(const_data_ptr_cast(raw_ts.value)));
}

Int96 TimestampToImpalaTimestamp(timestamp_t &ts) {
	int32_t hour, min, sec, micros;
	Time::Convert(Timestamp::GetTime(ts), hour, min, sec, micros);
	uint64_t micros_since_midnight =
	    hour * Interval::MICROS_PER_HOUR + min * Interval::MICROS_PER_MINUTE + sec * Interval::MICROS_PER_SEC + micros;
	auto days_since_epoch = Date::Epoch(Timestamp::GetDate(ts)) / int64_t(24 * 60 * 60);
	// first two uint32 in Int96 are nanoseconds since midnights
	// last uint32 is number of days since year 4713 BC ("Julian date")
	Int96 impala_ts;
	Store<uint64_t>(micros_since_midnight * NANOSECONDS_PER_MICRO, data_ptr_cast(impala_ts.value));
	impala_ts.value[2] = days_since_epoch + JULIAN_TO_UNIX_EPOCH_DAYS;
	return impala_ts;
}

timestamp_t ParquetTimestampMicrosToTimestamp(const int64_t &raw_ts) {
	return Timestamp::FromEpochMicroSeconds(raw_ts);
}

timestamp_t ParquetTimestampMsToTimestamp(const int64_t &raw_ts) {
	timestamp_t input(raw_ts);
	if (!input.IsFinite()) {
		return input;
	}
	return Timestamp::FromEpochMs(raw_ts);
}

timestamp_ns_t ParquetTimestampMsToTimestampNs(const int64_t &raw_ms) {
	timestamp_ns_t input;
	input.value = raw_ms;
	if (!input.IsFinite()) {
		return input;
	}
	return Timestamp::TimestampNsFromEpochMillis(raw_ms);
}

timestamp_ns_t ParquetTimestampUsToTimestampNs(const int64_t &raw_us) {
	timestamp_ns_t input;
	input.value = raw_us;
	if (!input.IsFinite()) {
		return input;
	}
	return Timestamp::TimestampNsFromEpochMicros(raw_us);
}

timestamp_ns_t ParquetTimestampNsToTimestampNs(const int64_t &raw_ns) {
	timestamp_ns_t result;
	result.value = raw_ns;
	return result;
}

date_t ParquetIntToDate(const int32_t &raw_date) {
	return date_t(raw_date);
}

template <typename T>
static T ParquetWrapTime(const T &raw, const T day) {
	// Special case 24:00:00
	if (raw == day) {
		return raw;
	}
	const auto modulus = raw % day;
	return modulus + (modulus < 0) * day;
}

int64_t ParquetTimeTZToUTCMicros(const dtime_tz_t &timetz) {
	// Keep the end of the UTC day, which NormalizeTimeTZ wraps to its start
	if (timetz.time().value - timetz.offset() * Interval::MICROS_PER_SEC == Interval::MICROS_PER_DAY) {
		return Interval::MICROS_PER_DAY;
	}
	return Time::NormalizeTimeTZ(timetz).value;
}

bool ParquetTimeIsValid(const int64_t &raw, const int64_t day) {
	return raw >= 0 && raw <= day;
}

template <typename T>
static void CheckParquetTime(const T &raw, const int64_t day) {
	if (!ParquetTimeIsValid(raw, day)) {
		throw InvalidInputException("Invalid TIME value %d in Parquet file - TIME values must be within a day", raw);
	}
}

dtime_t ParquetMsIntToTime(const int32_t &raw_millis) {
	CheckParquetTime(raw_millis, Interval::MSECS_PER_SEC * Interval::SECS_PER_DAY);
	return Time::FromTimeMs(raw_millis);
}

dtime_t ParquetIntToTime(const int64_t &raw_micros) {
	CheckParquetTime(raw_micros, Interval::MICROS_PER_DAY);
	return dtime_t(raw_micros);
}

dtime_ns_t ParquetIntToTimeNs(const int64_t &raw_nanos) {
	CheckParquetTime(raw_nanos, Interval::NANOS_PER_DAY);
	return dtime_ns_t(raw_nanos);
}

dtime_tz_t ParquetIntToTimeMsTZ(const int32_t &raw_millis) {
	const int32_t MSECS_PER_DAY = Interval::MSECS_PER_SEC * Interval::SECS_PER_DAY;
	const auto millis = ParquetWrapTime(raw_millis, MSECS_PER_DAY);
	return dtime_tz_t(Time::FromTimeMs(millis), 0);
}

dtime_tz_t ParquetIntToTimeTZ(const int64_t &raw_micros) {
	const auto micros = ParquetWrapTime(raw_micros, Interval::MICROS_PER_DAY);
	return dtime_tz_t(dtime_t(micros), 0);
}

dtime_tz_t ParquetIntToTimeNsTZ(const int64_t &raw_nanos) {
	const auto nanos = ParquetWrapTime(raw_nanos, Interval::NANOS_PER_DAY);
	return dtime_tz_t(Time::FromTimeNs(nanos), 0);
}

} // namespace duckdb
