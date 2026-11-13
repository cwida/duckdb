//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/time.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types.hpp"
#include "duckdb/common/types/datetime.hpp"
#include "duckdb/common/winapi.hpp"

namespace duckdb {

//! The Time class is a static class that holds helper functions for the Time
//! type.
class Time {
public:
	//! Convert a string in the format "hh:mm:ss" to a time object
	DUCKDB_API static dtime_t FromString(const string &str, bool strict = false, optional_ptr<int32_t> nanos = nullptr);
	DUCKDB_API static dtime_t FromCString(const char *buf, idx_t len, bool strict = false,
	                                      optional_ptr<int32_t> nanos = nullptr);
	DUCKDB_API static bool TryConvertTime(const char *buf, idx_t len, idx_t &pos, dtime_t &result, bool strict = false,
	                                      optional_ptr<int32_t> nanos = nullptr);
	DUCKDB_API static bool TryConvertTimeTZ(const char *buf, idx_t len, idx_t &pos, dtime_tz_t &result,
	                                        bool &has_offset, bool strict = false,
	                                        optional_ptr<int32_t> nanos = nullptr);
	// No hour limit
	DUCKDB_API static bool TryConvertInterval(const char *buf, idx_t len, idx_t &pos, dtime_t &result,
	                                          bool strict = false, optional_ptr<int32_t> nanos = nullptr,
	                                          bool negative = false);

	//! Convert a time object to a string in the format "hh:mm:ss"
	DUCKDB_API static string ToString(dtime_t time);
	//! Convert a UTC offset to ±HH[:MM]
	DUCKDB_API static string ToUTCOffset(int hour_offset, int minute_offset, int second_offset = 0);

	DUCKDB_API static dtime_t FromTime(int32_t hour, int32_t minute, int32_t second, int32_t microseconds = 0);
	DUCKDB_API static int64_t ToNanoTime(int32_t hour, int32_t minute, int32_t second, int32_t nanoseconds = 0);

	//! Normalize a TIME_TZ by adding the offset to the time part and returning the TIME
	DUCKDB_API static dtime_t NormalizeTimeTZ(dtime_tz_t timetz);

	//! Extract the time from a given timestamp object
	DUCKDB_API static void Convert(dtime_t time, int32_t &out_hour, int32_t &out_min, int32_t &out_sec,
	                               int32_t &out_micros);

	DUCKDB_API static string ConversionError(const string &str);
	DUCKDB_API static string ConversionError(string_t str);

	DUCKDB_API static dtime_t FromTimeMs(int64_t time_ms);
	DUCKDB_API static dtime_t FromTimeNs(int64_t time_ns);

	DUCKDB_API static bool IsValidTime(int32_t hour, int32_t minute, int32_t second, int32_t microseconds);

private:
	static bool TryConvertInternal(const char *buf, idx_t len, idx_t &pos, dtime_t &result, bool strict,
	                               optional_ptr<int32_t> nanos = nullptr, bool negative = false);
};

} // namespace duckdb
