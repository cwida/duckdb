//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/timestamp.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/datetime.hpp"
#include "duckdb/common/types/timestamp_base.hpp"
#include "duckdb/common/winapi.hpp"

namespace duckdb {

struct date_t; // NOLINT

enum class TimestampCastResult : uint8_t {
	SUCCESS,
	ERROR_INCORRECT_FORMAT,
	ERROR_NON_UTC_TIMEZONE,
	ERROR_RANGE,
	STRICT_UTC
};

struct TimestampComponents {
	int32_t year;
	int32_t month;
	int32_t day;

	int32_t hour;
	int32_t minute;
	int32_t second;
	int32_t microsecond;
	int16_t nanosecond;
};

//! The static Timestamp class holds helper functions for the timestamp types.
class Timestamp {
public:
	// the first midnight of the timestamp range, 290309-12-22 (BC)
	constexpr static const int32_t MIN_YEAR = -290308;
	constexpr static const int32_t MIN_MONTH = 12;
	constexpr static const int32_t MIN_DAY = 22;

public:
	//! Convert a string in the format "YYYY-MM-DD hh:mm:ss[.f][-+TH[:tm]]" to a timestamp object
	DUCKDB_API static timestamp_t FromString(const string &str, bool use_offset);
	//! Convert a string where the offset can also be a time zone string: / [A_Za-z0-9/_]+/
	//! If has_offset is true, then the result is an instant that was offset from UTC
	//! If the tz is not empty, the result is still an instant, but the parts can be extracted and applied to the TZ
	DUCKDB_API static TimestampCastResult TryConvertTimestampTZ(const char *str, idx_t len, timestamp_t &result,
	                                                            const bool use_offset, bool &has_offset, string_t &tz,
	                                                            optional_ptr<int32_t> nanos = nullptr);
	//! Strict Timestamp does not accept offsets.
	DUCKDB_API static TimestampCastResult TryConvertTimestamp(const char *str, idx_t len, timestamp_t &result,
	                                                          const bool use_offset,
	                                                          optional_ptr<int32_t> nanos = nullptr,
	                                                          bool strict = false);
	DUCKDB_API static TimestampCastResult TryConvertTimestamp(const char *str, idx_t len, timestamp_ns_t &result,
	                                                          bool use_offset, bool strict = false);
	DUCKDB_API static timestamp_t FromCString(const char *str, idx_t len, bool use_offset = false,
	                                          optional_ptr<int32_t> nanos = nullptr);
	//! Convert a date object to a string in the format "YYYY-MM-DD hh:mm:ss"
	DUCKDB_API static string ToString(timestamp_t timestamp);

	DUCKDB_API static date_t GetDate(timestamp_t timestamp);
	DUCKDB_API static date_t GetDateNS(timestamp_ns_t timestamp);

	//	Split a time at some precision, producing positive values.
	static inline void SplitTime(const int64_t value, const int64_t precision, int64_t &whole, int64_t &frac) {
		whole = value / precision;
		frac = value % precision;
		if (frac < 0) {
			frac += precision;
			--whole;
		}
	}

	DUCKDB_API static dtime_t GetTime(timestamp_t timestamp);
	DUCKDB_API static dtime_ns_t GetTimeNs(timestamp_ns_t timestamp);
	//! whole * unit + part for a part in [0, unit], also when only the sum fits in an int64_t, as on the first
	//! partial day or millisecond of the range
	DUCKDB_API static bool TryCombineUnits(int64_t whole, int64_t part, int64_t unit, int64_t &result);
	//! Create a Timestamp object from a specified (date, time) combination
	DUCKDB_API static timestamp_t FromDatetime(date_t date, dtime_t time);
	DUCKDB_API static bool TryFromDatetime(date_t date, dtime_t time, timestamp_t &result);
	DUCKDB_API static bool TryFromDatetime(date_t date, dtime_tz_t timetz, timestamp_t &result);
	//! Scale up to ns
	DUCKDB_API static bool TryFromTimestampNanos(timestamp_t ts, int32_t nanos, timestamp_ns_t &result);

	//! Is the character a valid part of a time zone name?
	static inline bool CharacterIsTimeZone(char c) {
		return StringUtil::CharacterIsAlpha(c) || StringUtil::CharacterIsDigit(c) || c == '_' || c == '/' || c == '+' ||
		       c == '-' || c == ':';
	}

	//! Extract the date and time from a given timestamp object
	DUCKDB_API static void Convert(timestamp_t date, date_t &out_date, dtime_t &out_time);
	//! Extract the date and time from a given timestamp object
	DUCKDB_API static void Convert(timestamp_ns_t date, date_t &out_date, dtime_t &out_time, int32_t &out_nanos);
	//! Returns current timestamp
	DUCKDB_API static timestamp_t GetCurrentTimestamp();

	//! Convert the epoch (in sec) to a timestamp
	DUCKDB_API static timestamp_t FromEpochSecondsPossiblyInfinite(int64_t s);
	DUCKDB_API static timestamp_t FromEpochSeconds(int64_t s);
	//! Convert the epoch (in ms) to a timestamp
	DUCKDB_API static timestamp_t FromEpochMsPossiblyInfinite(int64_t ms);
	DUCKDB_API static timestamp_t FromEpochMs(int64_t ms);
	//! Convert the epoch (in microseconds) to a timestamp
	DUCKDB_API static timestamp_t FromEpochMicroSeconds(int64_t micros);
	//! Convert the epoch (in nanoseconds) to a timestamp
	DUCKDB_API static timestamp_t FromEpochNanoSecondsPossiblyInfinite(int64_t nanos);
	DUCKDB_API static timestamp_t FromEpochNanoSeconds(int64_t nanos);

	//! Construct ns timestamps from various epoch units
	DUCKDB_API static timestamp_ns_t TimestampNsFromEpochMicros(int64_t micros);
	DUCKDB_API static timestamp_ns_t TimestampNsFromEpochMillis(int64_t millis);

	//! Try convert a timestamp to epoch (in nanoseconds)
	DUCKDB_API static bool TryGetEpochNanoSeconds(timestamp_t timestamp, int64_t &result);
	//! Convert the epoch (in seconds) to a timestamp
	DUCKDB_API static int64_t GetEpochSeconds(timestamp_t timestamp);
	//! Convert the epoch (in ms) to a timestamp
	DUCKDB_API static int64_t GetEpochMs(timestamp_t timestamp);
	//! Convert a timestamp to epoch (in microseconds)
	DUCKDB_API static int64_t GetEpochMicroSeconds(timestamp_t timestamp);
	//! Convert a timestamp to epoch (in nanoseconds)
	DUCKDB_API static int64_t GetEpochNanoSeconds(timestamp_t timestamp);
	DUCKDB_API static int64_t GetEpochNanoSeconds(timestamp_ns_t timestamp);
	//! Convert a timestamp to a rounded epoch at a given resolution.
	DUCKDB_API static int64_t GetEpochRounded(timestamp_t timestamp, const int64_t power_of_ten);
	//! Convert a timestamp to a Julian Day
	DUCKDB_API static double GetJulianDay(timestamp_t timestamp);

	//! Decompose a timestamp into its components
	DUCKDB_API static TimestampComponents GetComponents(timestamp_t timestamp);
	DUCKDB_API static time_t ToTimeT(timestamp_t);
	DUCKDB_API static timestamp_t FromTimeT(time_t);

	DUCKDB_API static bool TryParseUTCOffset(const char *str, idx_t &pos, idx_t len, int &hh, int &mm, int &ss,
	                                         bool strict = true);

	DUCKDB_API static string FormatError(const string &str);
	DUCKDB_API static string FormatError(string_t str);
	DUCKDB_API static string UnsupportedTimezoneError(const string &str);
	DUCKDB_API static string UnsupportedTimezoneError(string_t str);
	DUCKDB_API static string RangeError(const string &str);
	DUCKDB_API static string RangeError(string_t str);
};

} // namespace duckdb
