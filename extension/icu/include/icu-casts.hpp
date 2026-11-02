//===----------------------------------------------------------------------===//
//                         DuckDB
//
// icu-datefunc.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "icu-datefunc.hpp"

namespace duckdb {

class ExtensionLoader;

struct ICUMakeDate : public ICUDateFunc {
	static date_t Operation(Calendar *calendar, timestamp_tz_t instant);

	static bool CastToDate(Vector &source, Vector &result, idx_t count, CastParameters &parameters);
	static bool CastNsToDate(Vector &source, Vector &result, idx_t count, CastParameters &parameters);

	static BoundCastInfo BindCastToDate(BindCastInput &input, const LogicalType &source, const LogicalType &target);
	static BoundCastInfo BindCastNsToDate(BindCastInput &input, const LogicalType &source, const LogicalType &target);

	static void AddCasts(ExtensionLoader &loader);

	static date_t ToDate(ClientContext &context, timestamp_tz_t instant);
};

struct ICUToTimeTZ : public ICUDateFunc {
	static dtime_tz_t Operation(Calendar *calendar, dtime_tz_t timetz);

	//! Returns false if the instant cannot be converted. If no error_message is provided, an out-of-range time zone
	//! offset throws, otherwise the error is written to error_message.
	static bool ToTimeTZ(Calendar *calendar, timestamp_tz_t instant, dtime_tz_t &result,
	                     optional_ptr<string> error_message = nullptr);

	static bool CastToTimeTZ(Vector &source, Vector &result, idx_t count, CastParameters &parameters);
	static bool CastFromTime(Vector &source, Vector &result, idx_t count, CastParameters &parameters);

	static BoundCastInfo BindCastToTimeTZ(BindCastInput &input, const LogicalType &source, const LogicalType &target);
	static BoundCastInfo BindCastFromTime(BindCastInput &input, const LogicalType &source, const LogicalType &target);

	static void AddCasts(ExtensionLoader &loader);
};

} // namespace duckdb
