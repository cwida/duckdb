//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/time.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/types.hpp"

namespace duckdb {

//! The Time class is a static class that holds helper functions for the Time
//! type.
class Time {
public:
	//! Convert a string in the format "hh:mm:ss" to a time object
	static dtime_t FromString(const string &str, bool strict = false);
	static dtime_t FromCString(const char *buf, idx_t len, bool strict = false);
	static bool TryConvertTime(const char *buf, idx_t len, idx_t &pos, dtime_t &result, bool strict = false);

	//! Convert a time object to a string in the format "hh:mm:ss"
	static string ToString(dtime_t time);

	static dtime_t FromTime(int32_t hour, int32_t minute, int32_t second, int32_t microseconds = 0);

	//! Extract the time from a given timestamp object
	static void Convert(dtime_t time, int32_t &out_hour, int32_t &out_min, int32_t &out_sec, int32_t &out_micros);
};

} // namespace duckdb
