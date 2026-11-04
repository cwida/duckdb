#include "duckdb/common/exception.hpp"
#include "duckdb/common/operator/add.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/operator/subtract.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"
#include "duckdb/common/vector_operations/ternary_executor.hpp"
#include "core_functions/scalar/date_functions.hpp"

namespace duckdb {

namespace {

template <typename T>
timestamp_t ToBucketTimestamp(const T &t) {
	return Cast::template Operation<T, timestamp_t>(t);
}

template <>
timestamp_t ToBucketTimestamp(const dtime_t &t) {
	return Timestamp::FromDatetime(date_t::epoch(), t);
}

template <typename T>
date_t ToBucketDate(const T &t) {
	return Cast::template Operation<T, date_t>(t);
}

template <>
date_t ToBucketDate(const dtime_t &t) {
	return date_t::epoch();
}

template <typename T>
T FromBucketTimestamp(const timestamp_t &t) {
	return Cast::template Operation<timestamp_t, T>(t);
}

template <>
dtime_t FromBucketTimestamp(const timestamp_t &t) {
	return Timestamp::GetTime(t);
}

template <typename T>
T FromBucketDate(const date_t &t) {
	return Cast::template Operation<date_t, T>(t);
}

template <>
dtime_t FromBucketDate(const date_t &t) {
	return dtime_t(0);
}

struct TimeBucket {
	// Use 2000-01-03 00:00:00 (Monday) as origin when bucket_width is days, hours, ... for TimescaleDB compatibility
	// There are 10959 days between 1970-01-01 and 2000-01-03
	constexpr static const int64_t DEFAULT_ORIGIN_MICROS = 10959 * Interval::MICROS_PER_DAY;
	// Use 2000-01-01 as origin when bucket_width is months, years, ... for TimescaleDB compatibility
	// There are 360 months between 1970-01-01 and 2000-01-01
	constexpr static const int32_t DEFAULT_ORIGIN_MONTHS = 360;

	enum struct BucketWidthType : uint8_t { CONVERTIBLE_TO_MICROS, CONVERTIBLE_TO_MONTHS, UNCLASSIFIED };

	static inline BucketWidthType ClassifyBucketWidth(const interval_t bucket_width) {
		if (bucket_width.months == 0 && Interval::GetMicro(bucket_width) > 0) {
			return BucketWidthType::CONVERTIBLE_TO_MICROS;
		} else if (bucket_width.months > 0 && bucket_width.days == 0 && bucket_width.micros == 0) {
			return BucketWidthType::CONVERTIBLE_TO_MONTHS;
		} else {
			return BucketWidthType::UNCLASSIFIED;
		}
	}

	static inline BucketWidthType ClassifyBucketWidthErrorThrow(const interval_t bucket_width) {
		if (bucket_width.months == 0) {
			int64_t bucket_width_micros = Interval::GetMicro(bucket_width);
			if (bucket_width_micros <= 0) {
				throw NotImplementedException("Period must be greater than 0");
			}
			return BucketWidthType::CONVERTIBLE_TO_MICROS;
		} else if (bucket_width.months != 0 && bucket_width.days == 0 && bucket_width.micros == 0) {
			if (bucket_width.months < 0) {
				throw NotImplementedException("Period must be greater than 0");
			}
			return BucketWidthType::CONVERTIBLE_TO_MONTHS;
		} else {
			throw NotImplementedException("Month intervals cannot have day or time component");
		}
	}

	template <typename T>
	static inline int32_t EpochMonths(T ts) {
		date_t ts_date = Cast::template Operation<T, date_t>(ts);
		return (Date::ExtractYear(ts_date) - 1970) * 12 + Date::ExtractMonth(ts_date) - 1;
	}

	static inline timestamp_t WidthConvertibleToMicrosCommon(int64_t bucket_width_micros, int64_t ts_micros,
	                                                         int64_t origin_micros) {
		origin_micros %= bucket_width_micros;
		ts_micros = SubtractOperatorOverflowCheck::Operation<int64_t, int64_t, int64_t>(ts_micros, origin_micros);

		int64_t result_micros = (ts_micros / bucket_width_micros) * bucket_width_micros;
		if (ts_micros < 0 && ts_micros % bucket_width_micros != 0) {
			result_micros =
			    SubtractOperatorOverflowCheck::Operation<int64_t, int64_t, int64_t>(result_micros, bucket_width_micros);
		}
		result_micros = AddOperatorOverflowCheck::Operation<int64_t, int64_t, int64_t>(result_micros, origin_micros);
		if (!timestamp_t(result_micros).IsFinite()) {
			throw OutOfRangeException("Overflow in TIME_BUCKET: the result is not a finite TIMESTAMP");
		}

		return Timestamp::FromEpochMicroSeconds(result_micros);
	}

	static inline date_t WidthConvertibleToMonthsCommon(int32_t bucket_width_months, int32_t ts_months,
	                                                    int32_t origin_months) {
		origin_months %= bucket_width_months;
		ts_months = SubtractOperatorOverflowCheck::Operation<int32_t, int32_t, int32_t>(ts_months, origin_months);

		int32_t result_months = (ts_months / bucket_width_months) * bucket_width_months;
		if (ts_months < 0 && ts_months % bucket_width_months != 0) {
			result_months =
			    SubtractOperatorOverflowCheck::Operation<int32_t, int32_t, int32_t>(result_months, bucket_width_months);
		}
		result_months = AddOperatorOverflowCheck::Operation<int32_t, int32_t, int32_t>(result_months, origin_months);

		int32_t year =
		    (result_months < 0 && result_months % 12 != 0) ? 1970 + result_months / 12 - 1 : 1970 + result_months / 12;
		int32_t month =
		    (result_months < 0 && result_months % 12 != 0) ? result_months % 12 + 13 : result_months % 12 + 1;

		return Date::FromDate(year, month, 1);
	}

	struct WidthConvertibleToMicrosBinaryOperator {
		template <class TA, class TB, class TR>
		static inline TR Operation(TA bucket_width, TB ts) {
			if (!Value::IsFinite(ts)) {
				return Cast::template Operation<TB, TR>(ts);
			}
			int64_t bucket_width_micros = Interval::GetMicro(bucket_width);
			int64_t ts_micros = Timestamp::GetEpochMicroSeconds(ToBucketTimestamp<TB>(ts));
			return FromBucketTimestamp<TR>(
			    WidthConvertibleToMicrosCommon(bucket_width_micros, ts_micros, DEFAULT_ORIGIN_MICROS));
		}
	};

	struct WidthConvertibleToMonthsBinaryOperator {
		template <class TA, class TB, class TR>
		static inline TR Operation(TA bucket_width, TB ts) {
			if (!Value::IsFinite(ts)) {
				return Cast::template Operation<TB, TR>(ts);
			}
			int32_t ts_months = EpochMonths(ToBucketDate(ts));
			return FromBucketDate<TR>(
			    WidthConvertibleToMonthsCommon(bucket_width.months, ts_months, DEFAULT_ORIGIN_MONTHS));
		}
	};

	struct BinaryOperator {
		template <class TA, class TB, class TR>
		static inline TR Operation(TA bucket_width, TB ts) {
			BucketWidthType bucket_width_type = ClassifyBucketWidthErrorThrow(bucket_width);
			switch (bucket_width_type) {
			case BucketWidthType::CONVERTIBLE_TO_MICROS:
				return WidthConvertibleToMicrosBinaryOperator::Operation<TA, TB, TR>(bucket_width, ts);
			case BucketWidthType::CONVERTIBLE_TO_MONTHS:
				return WidthConvertibleToMonthsBinaryOperator::Operation<TA, TB, TR>(bucket_width, ts);
			default:
				throw NotImplementedException("Bucket type not implemented for TIME_BUCKET");
			}
		}
	};

	struct OffsetWidthConvertibleToMicrosTernaryOperator {
		template <class TA, class TB, class TC, class TR>
		static inline TR Operation(TA bucket_width, TB ts, TC offset) {
			if (!Value::IsFinite(ts)) {
				return Cast::template Operation<TB, TR>(ts);
			}
			int64_t bucket_width_micros = Interval::GetMicro(bucket_width);
			int64_t ts_micros =
			    Timestamp::GetEpochMicroSeconds(Interval::Add(ToBucketTimestamp(ts), Interval::Invert(offset)));
			return FromBucketTimestamp<TR>(Interval::Add(
			    WidthConvertibleToMicrosCommon(bucket_width_micros, ts_micros, DEFAULT_ORIGIN_MICROS), offset));
		}
	};

	struct OffsetWidthConvertibleToMonthsTernaryOperator {
		template <class TA, class TB, class TC, class TR>
		static inline TR Operation(TA bucket_width, TB ts, TC offset) {
			if (!Value::IsFinite(ts)) {
				return Cast::template Operation<TB, TR>(ts);
			}
			int32_t ts_months = EpochMonths(Interval::Add(ToBucketTimestamp(ts), Interval::Invert(offset)));
			return FromBucketTimestamp<TR>(Interval::Add(ToBucketTimestamp(WidthConvertibleToMonthsCommon(
			                                                 bucket_width.months, ts_months, DEFAULT_ORIGIN_MONTHS)),
			                                             offset));
		}
	};

	struct OffsetTernaryOperator {
		template <class TA, class TB, class TC, class TR>
		static inline TR Operation(TA bucket_width, TB ts, TC offset) {
			BucketWidthType bucket_width_type = ClassifyBucketWidthErrorThrow(bucket_width);
			switch (bucket_width_type) {
			case BucketWidthType::CONVERTIBLE_TO_MICROS:
				return OffsetWidthConvertibleToMicrosTernaryOperator::Operation<TA, TB, TC, TR>(bucket_width, ts,
				                                                                                offset);
			case BucketWidthType::CONVERTIBLE_TO_MONTHS:
				return OffsetWidthConvertibleToMonthsTernaryOperator::Operation<TA, TB, TC, TR>(bucket_width, ts,
				                                                                                offset);
			default:
				throw NotImplementedException("Bucket type not implemented for TIME_BUCKET");
			}
		}
	};

	struct OriginWidthConvertibleToMicrosTernaryOperator {
		template <class TA, class TB, class TC, class TR>
		static inline TR Operation(TA bucket_width, TB ts, TC origin) {
			if (!Value::IsFinite(ts)) {
				return Cast::template Operation<TB, TR>(ts);
			}
			int64_t bucket_width_micros = Interval::GetMicro(bucket_width);
			int64_t ts_micros = Timestamp::GetEpochMicroSeconds(ToBucketTimestamp(ts));
			int64_t origin_micros = Timestamp::GetEpochMicroSeconds(ToBucketTimestamp(origin));
			return FromBucketTimestamp<TR>(
			    WidthConvertibleToMicrosCommon(bucket_width_micros, ts_micros, origin_micros));
		}
	};

	struct OriginWidthConvertibleToMonthsTernaryOperator {
		template <class TA, class TB, class TC, class TR>
		static inline TR Operation(TA bucket_width, TB ts, TC origin) {
			if (!Value::IsFinite(ts)) {
				return Cast::template Operation<TB, TR>(ts);
			}
			int32_t ts_months = EpochMonths(ToBucketDate(ts));
			int32_t origin_months = EpochMonths(ToBucketDate(origin));
			return FromBucketDate<TR>(WidthConvertibleToMonthsCommon(bucket_width.months, ts_months, origin_months));
		}
	};

	struct OriginTernaryOperator {
		template <class TA, class TB, class TC, class TR>
		static inline optional<TR> Operation(TA bucket_width, TB ts, TC origin) {
			if (!Value::IsFinite(origin)) {
				return nullopt;
			}
			BucketWidthType bucket_width_type = ClassifyBucketWidthErrorThrow(bucket_width);
			switch (bucket_width_type) {
			case BucketWidthType::CONVERTIBLE_TO_MICROS:
				return OriginWidthConvertibleToMicrosTernaryOperator::Operation<TA, TB, TC, TR>(bucket_width, ts,
				                                                                                origin);
			case BucketWidthType::CONVERTIBLE_TO_MONTHS:
				return OriginWidthConvertibleToMonthsTernaryOperator::Operation<TA, TB, TC, TR>(bucket_width, ts,
				                                                                                origin);
			default:
				throw NotImplementedException("Bucket type not implemented for TIME_BUCKET");
			}
		}
	};
};

template <typename T>
void TimeBucketFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	D_ASSERT(args.ColumnCount() == 2);

	const auto &bucket_width_arg = args.data[0];
	const auto &ts_arg = args.data[1];

	if (bucket_width_arg.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		if (ConstantVector::IsNull(bucket_width_arg)) {
			throw InternalException("DateSub called with constant NULL part");
		}
		interval_t bucket_width = *ConstantVector::GetData<interval_t>(bucket_width_arg);
		TimeBucket::BucketWidthType bucket_width_type = TimeBucket::ClassifyBucketWidth(bucket_width);
		switch (bucket_width_type) {
		case TimeBucket::BucketWidthType::CONVERTIBLE_TO_MICROS:
			BinaryExecutor::Execute<interval_t, T, T>(
			    bucket_width_arg, ts_arg, result,
			    TimeBucket::WidthConvertibleToMicrosBinaryOperator::Operation<interval_t, T, T>);
			break;
		case TimeBucket::BucketWidthType::CONVERTIBLE_TO_MONTHS:
			BinaryExecutor::Execute<interval_t, T, T>(
			    bucket_width_arg, ts_arg, result,
			    TimeBucket::WidthConvertibleToMonthsBinaryOperator::Operation<interval_t, T, T>);
			break;
		case TimeBucket::BucketWidthType::UNCLASSIFIED:
			BinaryExecutor::Execute<interval_t, T, T>(bucket_width_arg, ts_arg, result,
			                                          TimeBucket::BinaryOperator::Operation<interval_t, T, T>);
			break;
		default:
			throw NotImplementedException("Bucket type not implemented for TIME_BUCKET");
		}
	} else {
		BinaryExecutor::Execute<interval_t, T, T>(bucket_width_arg, ts_arg, result,
		                                          TimeBucket::BinaryOperator::Operation<interval_t, T, T>);
	}
}

template <typename T>
void TimeBucketOffsetFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	D_ASSERT(args.ColumnCount() == 3);

	const auto &bucket_width_arg = args.data[0];
	const auto &ts_arg = args.data[1];
	const auto &offset_arg = args.data[2];

	if (bucket_width_arg.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		if (ConstantVector::IsNull(bucket_width_arg)) {
			throw InternalException("TimeBucketOffset called with constant NULL bucket width");
		}
		interval_t bucket_width = *ConstantVector::GetData<interval_t>(bucket_width_arg);
		TimeBucket::BucketWidthType bucket_width_type = TimeBucket::ClassifyBucketWidth(bucket_width);
		switch (bucket_width_type) {
		case TimeBucket::BucketWidthType::CONVERTIBLE_TO_MICROS:
			TernaryExecutor::Execute<interval_t, T, interval_t, T>(
			    bucket_width_arg, ts_arg, offset_arg, result,
			    TimeBucket::OffsetWidthConvertibleToMicrosTernaryOperator::Operation<interval_t, T, interval_t, T>);
			break;
		case TimeBucket::BucketWidthType::CONVERTIBLE_TO_MONTHS:
			TernaryExecutor::Execute<interval_t, T, interval_t, T>(
			    bucket_width_arg, ts_arg, offset_arg, result,
			    TimeBucket::OffsetWidthConvertibleToMonthsTernaryOperator::Operation<interval_t, T, interval_t, T>);
			break;
		case TimeBucket::BucketWidthType::UNCLASSIFIED:
			TernaryExecutor::Execute<interval_t, T, interval_t, T>(
			    bucket_width_arg, ts_arg, offset_arg, result,
			    TimeBucket::OffsetTernaryOperator::Operation<interval_t, T, interval_t, T>);
			break;
		default:
			throw NotImplementedException("Bucket type not implemented for TIME_BUCKET");
		}
	} else {
		TernaryExecutor::Execute<interval_t, T, interval_t, T>(
		    bucket_width_arg, ts_arg, offset_arg, result,
		    TimeBucket::OffsetTernaryOperator::Operation<interval_t, T, interval_t, T>);
	}
}

template <typename T>
void TimeBucketOriginFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	D_ASSERT(args.ColumnCount() == 3);

	const auto &bucket_width_arg = args.data[0];
	const auto &ts_arg = args.data[1];
	const auto &origin_arg = args.data[2];

	if (bucket_width_arg.GetVectorType() == VectorType::CONSTANT_VECTOR &&
	    origin_arg.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		if (ConstantVector::IsNull(bucket_width_arg) || ConstantVector::IsNull(origin_arg) ||
		    !Value::IsFinite(*ConstantVector::GetData<T>(origin_arg))) {
			ConstantVector::SetNull(result, count_t(args.size()));
		} else {
			interval_t bucket_width = *ConstantVector::GetData<interval_t>(bucket_width_arg);
			TimeBucket::BucketWidthType bucket_width_type = TimeBucket::ClassifyBucketWidth(bucket_width);
			switch (bucket_width_type) {
			case TimeBucket::BucketWidthType::CONVERTIBLE_TO_MICROS:
				TernaryExecutor::Execute<interval_t, T, T, T>(
				    bucket_width_arg, ts_arg, origin_arg, result,
				    TimeBucket::OriginWidthConvertibleToMicrosTernaryOperator::Operation<interval_t, T, T, T>);
				break;
			case TimeBucket::BucketWidthType::CONVERTIBLE_TO_MONTHS:
				TernaryExecutor::Execute<interval_t, T, T, T>(
				    bucket_width_arg, ts_arg, origin_arg, result,
				    TimeBucket::OriginWidthConvertibleToMonthsTernaryOperator::Operation<interval_t, T, T, T>);
				break;
			case TimeBucket::BucketWidthType::UNCLASSIFIED:
				TernaryExecutor::Execute<interval_t, T, T, T>(
				    bucket_width_arg, ts_arg, origin_arg, result,
				    TimeBucket::OriginTernaryOperator::Operation<interval_t, T, T, T>);
				break;
			default:
				throw NotImplementedException("Bucket type not implemented for TIME_BUCKET");
			}
		}
	} else {
		TernaryExecutor::Execute<interval_t, T, T, T>(
		    bucket_width_arg, ts_arg, origin_arg, result,
		    TimeBucket::OriginTernaryOperator::Operation<interval_t, T, T, T>);
	}
}

} // namespace

// Names the bucket_width/timestamp/[offset|origin] arguments shared by time_bucket's per-type overloads.
static ScalarFunction NameBucketWidthTimestampArguments(ScalarFunction fun, const LogicalType &type) {
	fun.GetSignature().AddParameter("bucket_width", LogicalType::INTERVAL).AddParameter("timestamp", type);
	return fun;
}

static ScalarFunction NameBucketWidthTimestampOffsetArguments(ScalarFunction fun, const LogicalType &type) {
	fun.GetSignature()
	    .AddParameter("bucket_width", LogicalType::INTERVAL)
	    .AddParameter("timestamp", type)
	    .AddParameter("offset", LogicalType::INTERVAL);
	return fun;
}

static ScalarFunction NameBucketWidthTimestampOriginArguments(ScalarFunction fun, const LogicalType &type) {
	fun.GetSignature()
	    .AddParameter("bucket_width", LogicalType::INTERVAL)
	    .AddParameter("timestamp", type)
	    .AddParameter("origin", type);
	return fun;
}

ScalarFunctionSet TimeBucketFun::GetFunctions() {
	ScalarFunctionSet time_bucket;
	time_bucket.AddFunction(NameBucketWidthTimestampArguments(
	    ScalarFunction({}, LogicalType::DATE, TimeBucketFunction<date_t>), LogicalType::DATE));
	time_bucket.AddFunction(NameBucketWidthTimestampOffsetArguments(
	    ScalarFunction({}, LogicalType::DATE, TimeBucketOffsetFunction<date_t>), LogicalType::DATE));
	time_bucket.AddFunction(NameBucketWidthTimestampOriginArguments(
	    ScalarFunction({}, LogicalType::DATE, TimeBucketOriginFunction<date_t>), LogicalType::DATE));

	time_bucket.AddFunction(NameBucketWidthTimestampArguments(
	    ScalarFunction({}, LogicalType::TIMESTAMP, TimeBucketFunction<timestamp_t>), LogicalType::TIMESTAMP));
	time_bucket.AddFunction(NameBucketWidthTimestampOffsetArguments(
	    ScalarFunction({}, LogicalType::TIMESTAMP, TimeBucketOffsetFunction<timestamp_t>), LogicalType::TIMESTAMP));
	time_bucket.AddFunction(NameBucketWidthTimestampOriginArguments(
	    ScalarFunction({}, LogicalType::TIMESTAMP, TimeBucketOriginFunction<timestamp_t>), LogicalType::TIMESTAMP));

	time_bucket.ApplyToFunctions(
	    [](ScalarFunction &func) { func.SetArgProperties(1, ArgProperties().NonDecreasing()); });

	//	Not monotonic (wraps)
	time_bucket.AddFunction(NameBucketWidthTimestampArguments(
	    ScalarFunction({}, LogicalType::TIME, TimeBucketFunction<dtime_t>), LogicalType::TIME));
	time_bucket.AddFunction(NameBucketWidthTimestampOffsetArguments(
	    ScalarFunction({}, LogicalType::TIME, TimeBucketOffsetFunction<dtime_t>), LogicalType::TIME));
	time_bucket.AddFunction(NameBucketWidthTimestampOriginArguments(
	    ScalarFunction({}, LogicalType::TIME, TimeBucketOriginFunction<dtime_t>), LogicalType::TIME));

	time_bucket.SetFallible();

	return time_bucket;
}

} // namespace duckdb
