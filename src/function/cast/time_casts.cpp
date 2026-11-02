#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/function/cast/vector_cast_helpers.hpp"
#include "duckdb/common/operator/string_cast.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

namespace {

template <class SRC, class DST>
string TimestampCastErrorMessage(SRC input) {
	if ((std::is_same<DST, dtime_t>() || std::is_same<DST, dtime_tz_t>()) && !input.IsFinite()) {
		return "Can't get TIME of infinite TIMESTAMP";
	}
	if (std::is_same<DST, dtime_ns_t>() && !input.IsFinite()) {
		return "Can't get TIME_NS of infinite TIMESTAMP";
	}
	if (std::is_same<DST, date_t>()) {
		return StringUtil::Format("Could not convert Timestamp to %s.", TypeIdToString(GetTypeId<DST>()));
	}
	return "Could not convert Timestamp to higher precision.";
}

struct TryCastTimestampErrorMessage {
	template <class SRC, class DST>
	static bool Operation(SRC input, DST &result, CastParameters &parameters) {
		if (TryCast::Operation(input, result, parameters.strict)) {
			return true;
		}
		HandleCastError::AssignError(TimestampCastErrorMessage<SRC, DST>(input), parameters);
		return false;
	}
};

} // namespace

BoundCastInfo DefaultCasts::DateCastSwitch(BindCastInput &input, const LogicalType &source, const LogicalType &target) {
	// now switch on the result type
	switch (target.id()) {
	case LogicalTypeId::VARCHAR:
		// date to varchar
		return BoundCastInfo(&VectorCastHelpers::StringCast<date_t, duckdb::StringCast>);
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
		// date to timestamp
		return BoundCastInfo(&VectorCastHelpers::TryCastLoop<date_t, timestamp_t, duckdb::TryCast>);
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		return BoundCastInfo(&VectorCastHelpers::TryCastLoop<date_t, timestamp_ns_t, duckdb::TryCast>);
	case LogicalTypeId::TIMESTAMP_SEC:
		return BoundCastInfo(&VectorCastHelpers::TryCastLoop<date_t, timestamp_sec_t, duckdb::TryCast>);
	case LogicalTypeId::TIMESTAMP_MS:
		return BoundCastInfo(&VectorCastHelpers::TryCastLoop<date_t, timestamp_ms_t, duckdb::TryCast>);
	default:
		return TryVectorNullCast;
	}
}

BoundCastInfo DefaultCasts::TimeCastSwitch(BindCastInput &input, const LogicalType &source, const LogicalType &target) {
	// now switch on the result type
	switch (target.id()) {
	case LogicalTypeId::VARCHAR:
		// time to varchar
		return BoundCastInfo(&VectorCastHelpers::StringCast<dtime_t, duckdb::StringCast>);
	case LogicalTypeId::TIME_NS:
		// time (us) to time (ns)
		return BoundCastInfo(&VectorCastHelpers::TryCastLoop<dtime_t, dtime_ns_t, duckdb::TryCast>);
	case LogicalTypeId::TIME_TZ:
		// time to time with time zone
		return BoundCastInfo(&VectorCastHelpers::TryCastLoop<dtime_t, dtime_tz_t, duckdb::TryCast>);
	default:
		return TryVectorNullCast;
	}
}

BoundCastInfo DefaultCasts::TimeNsCastSwitch(BindCastInput &input, const LogicalType &src, const LogicalType &target) {
	// now switch on the result type
	switch (target.id()) {
	case LogicalTypeId::VARCHAR:
		// time (ns) to varchar
		return BoundCastInfo(&VectorCastHelpers::StringCast<dtime_ns_t, duckdb::StringCast>);
	case LogicalTypeId::TIME:
		// time (ns) to time (µs)
		return BoundCastInfo(&VectorCastHelpers::TryCastLoop<dtime_ns_t, dtime_t, duckdb::TryCast>);
	default:
		return TryVectorNullCast;
	}
}

BoundCastInfo DefaultCasts::TimeTzCastSwitch(BindCastInput &input, const LogicalType &source,
                                             const LogicalType &target) {
	// now switch on the result type
	switch (target.id()) {
	case LogicalTypeId::VARCHAR:
		// time with time zone to varchar
		return BoundCastInfo(&VectorCastHelpers::StringCast<dtime_tz_t, duckdb::StringCast>);
	case LogicalTypeId::TIME:
		// time with time zone to time
		return BoundCastInfo(&VectorCastHelpers::TryCastLoop<dtime_tz_t, dtime_t, duckdb::TryCast>);
	default:
		return TryVectorNullCast;
	}
}

BoundCastInfo DefaultCasts::TimestampCastSwitch(BindCastInput &input, const LogicalType &source,
                                                const LogicalType &target) {
	// now switch on the result type
	switch (target.id()) {
	case LogicalTypeId::VARCHAR:
		// timestamp to varchar
		return BoundCastInfo(&VectorCastHelpers::StringCast<timestamp_t, duckdb::StringCast>);
	case LogicalTypeId::DATE:
		// timestamp to date
		return BoundCastInfo(&VectorCastHelpers::TryCastLoop<timestamp_t, date_t, duckdb::TryCast>);
	case LogicalTypeId::TIME:
		// timestamp to time
		return BoundCastInfo(&VectorCastHelpers::TryCastErrorLoop<timestamp_t, dtime_t, TryCastTimestampErrorMessage>);
	case LogicalTypeId::TIME_TZ:
		// timestamp to time_tz
		return BoundCastInfo(
		    &VectorCastHelpers::TryCastErrorLoop<timestamp_t, dtime_tz_t, TryCastTimestampErrorMessage>);
	case LogicalTypeId::TIMESTAMP_TZ:
		// timestamp (us) to timestamp with time zone
		return ReinterpretCast;
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		// timestamp (us) to timestamp [with time zone] (ns)
		return BoundCastInfo(
		    &VectorCastHelpers::TryCastErrorLoop<timestamp_t, timestamp_ns_t, TryCastTimestampErrorMessage>);
	case LogicalTypeId::TIMESTAMP_MS:
		// timestamp (us) to timestamp (ms)
		return BoundCastInfo(&VectorCastHelpers::TryCastLoop<timestamp_t, timestamp_ms_t, duckdb::TryCast>);
	case LogicalTypeId::TIMESTAMP_SEC:
		// timestamp (us) to timestamp (s)
		return BoundCastInfo(&VectorCastHelpers::TryCastLoop<timestamp_t, timestamp_sec_t, duckdb::TryCast>);
	default:
		return TryVectorNullCast;
	}
}

BoundCastInfo DefaultCasts::TimestampTzCastSwitch(BindCastInput &input, const LogicalType &source,
                                                  const LogicalType &target) {
	// now switch on the result type
	switch (target.id()) {
	case LogicalTypeId::VARCHAR:
		// timestamp with time zone to varchar
		return BoundCastInfo(&VectorCastHelpers::StringCast<timestamp_tz_t, duckdb::StringCast>);
	case LogicalTypeId::TIME_TZ:
		// timestamp with time zone to time with time zone.
		return BoundCastInfo(
		    &VectorCastHelpers::TryCastErrorLoop<timestamp_tz_t, dtime_tz_t, TryCastTimestampErrorMessage>);
	case LogicalTypeId::TIMESTAMP:
		// timestamp with time zone to timestamp (us)
		return ReinterpretCast;
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		// timestamptz (us) to timestamp [with time zone] (ns)
		return BoundCastInfo(
		    &VectorCastHelpers::TryCastErrorLoop<timestamp_tz_t, timestamp_ns_t, TryCastTimestampErrorMessage>);
	case LogicalTypeId::TIMESTAMP_MS:
		// timestamptz (us) to timestamp (ms)
		return BoundCastInfo(&VectorCastHelpers::TryCastLoop<timestamp_tz_t, timestamp_ms_t, duckdb::TryCast>);
	case LogicalTypeId::TIMESTAMP_SEC:
		// timestamptz (us) to timestamp (s)
		return BoundCastInfo(&VectorCastHelpers::TryCastLoop<timestamp_tz_t, timestamp_sec_t, duckdb::TryCast>);
	default:
		return TryVectorNullCast;
	}
}

BoundCastInfo DefaultCasts::TimestampTzNsCastSwitch(BindCastInput &input, const LogicalType &source,
                                                    const LogicalType &target) {
	// now switch on the result type
	switch (target.id()) {
	case LogicalTypeId::VARCHAR:
		// timestamp(ns) with time zone to varchar
		return BoundCastInfo(&VectorCastHelpers::StringCast<timestamp_tz_ns_t, duckdb::StringCast>);
	case LogicalTypeId::TIME_TZ:
		// timestamp with time zone to time with time zone.
		return BoundCastInfo(
		    &VectorCastHelpers::TryCastErrorLoop<timestamp_tz_ns_t, dtime_tz_t, TryCastTimestampErrorMessage>);
	case LogicalTypeId::TIMESTAMP_NS:
		// timestamp with time zone to timestamp (us)
		return ReinterpretCast;
	case LogicalTypeId::TIMESTAMP_TZ:
		// timestamp with time zone (ns) to timestamp with time zone (us)
		return BoundCastInfo(&VectorCastHelpers::TryCastLoop<timestamp_tz_ns_t, timestamp_t, duckdb::TryCast>);
	case LogicalTypeId::TIMESTAMP_MS:
		// timestamp with time zone (ns) to timestamp (ms)
		return BoundCastInfo(&VectorCastHelpers::TryCastLoop<timestamp_tz_ns_t, timestamp_ms_t, duckdb::TryCast>);
	case LogicalTypeId::TIMESTAMP_SEC:
		// timestamp with time zone (ns) to timestamp (s)
		return BoundCastInfo(&VectorCastHelpers::TryCastLoop<timestamp_tz_ns_t, timestamp_sec_t, duckdb::TryCast>);
	default:
		return TryVectorNullCast;
	}
}

BoundCastInfo DefaultCasts::TimestampNsCastSwitch(BindCastInput &input, const LogicalType &source,
                                                  const LogicalType &target) {
	// now switch on the result type
	switch (target.id()) {
	case LogicalTypeId::VARCHAR:
		// timestamp (ns) to varchar
		return BoundCastInfo(&VectorCastHelpers::StringCast<timestamp_ns_t, duckdb::StringCast>);
	case LogicalTypeId::DATE:
		// timestamp (ns) to date
		return BoundCastInfo(&VectorCastHelpers::TryCastLoop<timestamp_ns_t, date_t, duckdb::TryCast>);
	case LogicalTypeId::TIME:
		// timestamp (ns) to time
		return BoundCastInfo(
		    &VectorCastHelpers::TryCastErrorLoop<timestamp_ns_t, dtime_t, TryCastTimestampErrorMessage>);
	case LogicalTypeId::TIME_NS:
		// timestamp (ns) to time (ns)
		return BoundCastInfo(
		    &VectorCastHelpers::TryCastErrorLoop<timestamp_ns_t, dtime_ns_t, TryCastTimestampErrorMessage>);
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
		// timestamp (ns) to timestamp [with time zone] (us)
		return BoundCastInfo(&VectorCastHelpers::TryCastLoop<timestamp_ns_t, timestamp_t, duckdb::TryCast>);
	case LogicalTypeId::TIMESTAMP_MS:
		return BoundCastInfo(&VectorCastHelpers::TryCastLoop<timestamp_ns_t, timestamp_ms_t, duckdb::TryCast>);
	default:
		return TryVectorNullCast;
	}
}

BoundCastInfo DefaultCasts::TimestampMsCastSwitch(BindCastInput &input, const LogicalType &source,
                                                  const LogicalType &target) {
	// now switch on the result type
	switch (target.id()) {
	case LogicalTypeId::VARCHAR:
		// timestamp (ms) to varchar
		return BoundCastInfo(&VectorCastHelpers::StringCast<timestamp_ms_t, duckdb::CastFromTimestampMS>);
	case LogicalTypeId::DATE:
		// timestamp (ms) to date
		return BoundCastInfo(
		    &VectorCastHelpers::TryCastErrorLoop<timestamp_ms_t, date_t, TryCastTimestampErrorMessage>);
	case LogicalTypeId::TIME:
		// timestamp (ms) to time
		return BoundCastInfo(
		    &VectorCastHelpers::TryCastErrorLoop<timestamp_ms_t, dtime_t, TryCastTimestampErrorMessage>);
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
		// timestamp (ms) to timestamp [with time zone] (us)
		return BoundCastInfo(
		    &VectorCastHelpers::TryCastErrorLoop<timestamp_ms_t, timestamp_t, TryCastTimestampErrorMessage>);
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		// timestamp (ms) to timestamp [with time zone] (ns)
		return BoundCastInfo(
		    &VectorCastHelpers::TryCastErrorLoop<timestamp_ms_t, timestamp_ns_t, TryCastTimestampErrorMessage>);
	case LogicalTypeId::TIMESTAMP_SEC:
		// timestamp (ms) to timestamp (s)
		return BoundCastInfo(&VectorCastHelpers::TryCastLoop<timestamp_ms_t, timestamp_sec_t, duckdb::TryCast>);
	default:
		return TryVectorNullCast;
	}
}

BoundCastInfo DefaultCasts::TimestampSecCastSwitch(BindCastInput &input, const LogicalType &source,
                                                   const LogicalType &target) {
	// now switch on the result type
	switch (target.id()) {
	case LogicalTypeId::VARCHAR:
		// timestamp (s) to varchar
		return BoundCastInfo(&VectorCastHelpers::StringCast<timestamp_sec_t, duckdb::CastFromTimestampSec>);
	case LogicalTypeId::DATE:
		// timestamp (s) to date
		return BoundCastInfo(
		    &VectorCastHelpers::TryCastErrorLoop<timestamp_sec_t, date_t, TryCastTimestampErrorMessage>);
	case LogicalTypeId::TIME:
		// timestamp (s) to time
		return BoundCastInfo(
		    &VectorCastHelpers::TryCastErrorLoop<timestamp_sec_t, dtime_t, TryCastTimestampErrorMessage>);
	case LogicalTypeId::TIMESTAMP_MS:
		// timestamp (s) to timestamp (ms)
		return BoundCastInfo(
		    &VectorCastHelpers::TryCastErrorLoop<timestamp_sec_t, timestamp_ms_t, TryCastTimestampErrorMessage>);
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
		// timestamp (s) to timestamp [with time zone] (us)
		return BoundCastInfo(
		    &VectorCastHelpers::TryCastErrorLoop<timestamp_sec_t, timestamp_t, TryCastTimestampErrorMessage>);
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		// timestamp (s) to timestamp [with time zone] (ns)
		return BoundCastInfo(
		    &VectorCastHelpers::TryCastErrorLoop<timestamp_sec_t, timestamp_ns_t, TryCastTimestampErrorMessage>);
	default:
		return TryVectorNullCast;
	}
}
BoundCastInfo DefaultCasts::IntervalCastSwitch(BindCastInput &input, const LogicalType &source,
                                               const LogicalType &target) {
	// now switch on the result type
	switch (target.id()) {
	case LogicalTypeId::VARCHAR:
		// time to varchar
		return BoundCastInfo(&VectorCastHelpers::StringCast<interval_t, duckdb::StringCast>);
	default:
		return TryVectorNullCast;
	}
}

} // namespace duckdb
