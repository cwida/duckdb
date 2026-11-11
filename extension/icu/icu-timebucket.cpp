#include "duckdb/common/exception.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/operator/subtract.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"
#include "duckdb/common/vector_operations/ternary_executor.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "include/icu-datefunc.hpp"

namespace duckdb {

struct ICUTimeBucket : public ICUDateFunc {
	// Use 2000-01-03 00:00:00 (Monday) as origin when bucket_width is days, hours, ... for TimescaleDB compatibility
	// There are 10959 days between 1970-01-01 and 2000-01-03
	constexpr static const int64_t DEFAULT_ORIGIN_MICROS_1 = 10959 * Interval::MICROS_PER_DAY;
	// Use 2000-01-01 as origin when bucket_width is months, years, ... for TimescaleDB compatibility
	// There are 10957 days between 1970-01-01 and 2000-01-01
	constexpr static const int64_t DEFAULT_ORIGIN_MICROS_2 = 10957 * Interval::MICROS_PER_DAY;

	enum struct BucketWidthType { CONVERTIBLE_TO_MICROS, CONVERTIBLE_TO_DAYS, CONVERTIBLE_TO_MONTHS, UNCLASSIFIED };

	static inline BucketWidthType ClassifyBucketWidth(const interval_t bucket_width) {
		if (bucket_width.months == 0 && bucket_width.days == 0 && bucket_width.micros > 0) {
			return BucketWidthType::CONVERTIBLE_TO_MICROS;
		} else if (bucket_width.months == 0 && bucket_width.days >= 0 && bucket_width.micros == 0) {
			return BucketWidthType::CONVERTIBLE_TO_DAYS;
		} else if (bucket_width.months > 0 && bucket_width.days == 0 && bucket_width.micros == 0) {
			return BucketWidthType::CONVERTIBLE_TO_MONTHS;
		} else {
			return BucketWidthType::UNCLASSIFIED;
		}
	}

	static inline BucketWidthType ClassifyBucketWidthErrorThrow(const interval_t bucket_width) {
		if (bucket_width.months == 0 && bucket_width.days == 0) {
			if (bucket_width.micros <= 0) {
				throw NotImplementedException("Period must be greater than 0");
			}
			return BucketWidthType::CONVERTIBLE_TO_MICROS;
		} else if (bucket_width.months == 0 && bucket_width.micros == 0) {
			if (bucket_width.days <= 0) {
				throw NotImplementedException("Period must be greater than 0");
			}
			return BucketWidthType::CONVERTIBLE_TO_DAYS;
		} else if (bucket_width.days == 0 && bucket_width.micros == 0) {
			if (bucket_width.months <= 0) {
				throw NotImplementedException("Period must be greater than 0");
			}
			return BucketWidthType::CONVERTIBLE_TO_MONTHS;
		} else if (bucket_width.months == 0) {
			throw NotImplementedException("Day intervals cannot have time component");
		} else {
			throw NotImplementedException("Month intervals cannot have day or time component");
		}
	}

	static inline timestamp_tz_t WidthConvertibleToMicrosCommon(int64_t bucket_width_micros, const timestamp_tz_t ts,
	                                                            const timestamp_tz_t origin, TZCalendar &calendar) {
		if (!bucket_width_micros) {
			throw OutOfRangeException("Can't bucket using zero microseconds");
		}
		int64_t ts_micros = SubtractOperatorOverflowCheck::Operation<int64_t, int64_t, int64_t>(ts.value, origin.value);
		int64_t result_micros = (ts_micros / bucket_width_micros) * bucket_width_micros;
		if (ts_micros < 0 && ts_micros % bucket_width_micros != 0) {
			result_micros =
			    SubtractOperatorOverflowCheck::Operation<int64_t, int64_t, int64_t>(result_micros, bucket_width_micros);
		}

		return Add(calendar, origin, interval_t {0, 0, result_micros});
	}

	//! Add resolves a local time that occurs twice (e.g. when the clocks are turned back) to its latter occurrence.
	//! If that is after ts, ts lies between the two occurrences, and its bucket starts at the former occurrence.
	static timestamp_tz_t FormerOccurrence(Calendar *calendar, const timestamp_tz_t bucket, const timestamp_tz_t ts) {
		auto get_offset = [&](const timestamp_tz_t instant) {
			SetTime(calendar, instant);
			return int64_t(ExtractField(calendar, CAL_ZONE_OFFSET)) + ExtractField(calendar, CAL_DST_OFFSET);
		};
		const auto ts_offset = get_offset(ts);
		const auto bucket_offset = get_offset(bucket);
		if (ts_offset <= bucket_offset) {
			return bucket;
		}
		const timestamp_tz_t former(bucket.value - (ts_offset - bucket_offset) * Interval::MICROS_PER_MSEC);
		if (ts < former || get_offset(former) != ts_offset) {
			return bucket;
		}
		return former;
	}

	static inline timestamp_tz_t WidthConvertibleToDaysCommon(int32_t bucket_width_days, const timestamp_tz_t ts,
	                                                          const timestamp_tz_t origin, TZCalendar &calendar) {
		if (!bucket_width_days) {
			throw OutOfRangeException("Can't bucket using zero days");
		}
		const auto sub_days = SubtractFactory(DatePartSpecifier::DAY);

		int64_t ts_days = sub_days(calendar.GetCalendar(), origin, ts);
		int64_t result_days = (ts_days / bucket_width_days) * bucket_width_days;
		if (result_days < NumericLimits<int32_t>::Minimum() || result_days > NumericLimits<int32_t>::Maximum()) {
			throw OutOfRangeException("Timestamp out of range");
		}
		auto bucket = Add(calendar, origin, interval_t {0, static_cast<int32_t>(result_days), 0});
		if (ts < bucket) {
			bucket = FormerOccurrence(calendar.GetCalendar(), bucket, ts);
		}
		while (ts < bucket) {
			bucket = Add(calendar, bucket, interval_t {0, -bucket_width_days, 0});
		}

		return bucket;
	}

	static inline timestamp_tz_t WidthConvertibleToMonthsCommon(int32_t bucket_width_months, const timestamp_tz_t ts,
	                                                            const timestamp_tz_t origin, TZCalendar &calendar_p) {
		if (!bucket_width_months) {
			throw OutOfRangeException("Can't bucket using zero months");
		}
		const auto trunc_months = TruncationFactory(DatePartSpecifier::MONTH);
		const auto sub_months = SubtractFactory(DatePartSpecifier::MONTH);

		auto calendar = calendar_p.GetCalendar();
		uint64_t tmp_micros = SetTime(calendar, ts);
		trunc_months(calendar, tmp_micros);
		auto truncated_ts = GetTimeUnsafe(calendar, tmp_micros);

		tmp_micros = SetTime(calendar, origin);
		trunc_months(calendar, tmp_micros);
		auto truncated_origin = GetTimeUnsafe(calendar, tmp_micros);

		int32_t ts_months =
		    NumericCast<int64_t, int32_t>(sub_months(calendar, truncated_origin, truncated_ts)); // NOLINT
		auto result_months = (ts_months / bucket_width_months) * bucket_width_months;
		if (result_months < NumericLimits<int32_t>::Minimum() || result_months > NumericLimits<int32_t>::Maximum()) {
			throw OutOfRangeException("Timestamp out of range");
		}
		if (ts_months < 0 && ts_months % bucket_width_months != 0) {
			result_months =
			    SubtractOperatorOverflowCheck::Operation<int32_t, int32_t, int32_t>(result_months, bucket_width_months);
		}

		auto bucket = Add(calendar_p, truncated_origin, interval_t {static_cast<int32_t>(result_months), 0, 0});
		if (ts < bucket) {
			bucket = FormerOccurrence(calendar, bucket, ts);
		}
		while (ts < bucket) {
			bucket = Add(calendar_p, bucket, interval_t {-bucket_width_months, 0, 0});
		}
		return bucket;
	}

	template <typename TA, typename TB, typename TR, typename OP>
	static void ExecuteBinary(DataChunk &args, ExpressionState &state, Vector &result) {
		D_ASSERT(args.ColumnCount() == 2);

		auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
		auto &info = func_expr.BindInfo()->Cast<BindData>();
		CalendarPtr calendar(info.calendar->Copy());

		BinaryExecutor::Execute<TA, TB, TR>(args.data[0], args.data[1], result, [&](TA left, TB right) {
			return OP::template Operation<TA, TB, TR>(left, right, calendar);
		});
	}

	template <typename TA, typename TB, typename TC, typename TR, typename OP>
	static void ExecuteTernary(DataChunk &args, ExpressionState &state, Vector &result) {
		D_ASSERT(args.ColumnCount() == 3);

		auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
		auto &info = func_expr.BindInfo()->Cast<BindData>();
		CalendarPtr calendar(info.calendar->Copy());

		TernaryExecutor::Execute<TA, TB, TC, TR>(
		    args.data[0], args.data[1], args.data[2], result, [&](TA ta, TB tb, TC tc) {
			    return OP::template Operation<TA, TB, TC, TR>(args.data[0], args.data[1], args.data[2], calendar.get());
		    });
	}

	struct WidthConvertibleToMicrosBinaryOperator {
		static inline timestamp_tz_t Operation(interval_t bucket_width, timestamp_tz_t ts, TZCalendar &calendar) {
			if (!ts.IsFinite()) {
				return ts;
			}
			const timestamp_tz_t origin(DEFAULT_ORIGIN_MICROS_1);
			return WidthConvertibleToMicrosCommon(bucket_width.micros, ts, origin, calendar);
		}
	};

	struct WidthConvertibleToDaysBinaryOperator {
		static inline timestamp_tz_t Operation(interval_t bucket_width, timestamp_tz_t ts, TZCalendar &calendar) {
			if (!ts.IsFinite()) {
				return ts;
			}
			const timestamp_tz_t origin(DEFAULT_ORIGIN_MICROS_1);
			;
			return WidthConvertibleToDaysCommon(bucket_width.days, ts, origin, calendar);
		}
	};

	struct WidthConvertibleToMonthsBinaryOperator {
		static inline timestamp_tz_t Operation(interval_t bucket_width, timestamp_tz_t ts, TZCalendar &calendar) {
			if (!ts.IsFinite()) {
				return ts;
			}
			const timestamp_tz_t origin(DEFAULT_ORIGIN_MICROS_2);
			return WidthConvertibleToMonthsCommon(bucket_width.months, ts, origin, calendar);
		}
	};

	struct BinaryOperator {
		static inline timestamp_tz_t Operation(interval_t bucket_width, timestamp_tz_t ts, TZCalendar &calendar) {
			BucketWidthType bucket_width_type = ClassifyBucketWidthErrorThrow(bucket_width);
			switch (bucket_width_type) {
			case BucketWidthType::CONVERTIBLE_TO_MICROS:
				return WidthConvertibleToMicrosBinaryOperator::Operation(bucket_width, ts, calendar);
			case BucketWidthType::CONVERTIBLE_TO_DAYS:
				return WidthConvertibleToDaysBinaryOperator::Operation(bucket_width, ts, calendar);
			case BucketWidthType::CONVERTIBLE_TO_MONTHS:
				return WidthConvertibleToMonthsBinaryOperator::Operation(bucket_width, ts, calendar);
			default:
				throw NotImplementedException("Bucket type not implemented for ICU TIME_BUCKET");
			}
		}
	};

	struct OffsetWidthConvertibleToMicrosTernaryOperator {
		static inline timestamp_tz_t Operation(interval_t bucket_width, timestamp_tz_t ts, interval_t offset,
		                                       TZCalendar &calendar) {
			if (!ts.IsFinite()) {
				return ts;
			}
			const timestamp_tz_t origin(DEFAULT_ORIGIN_MICROS_1);
			return Add(calendar,
			           WidthConvertibleToMicrosCommon(bucket_width.micros, Sub(calendar, ts, offset), origin, calendar),
			           offset);
		}
	};

	struct OffsetWidthConvertibleToDaysTernaryOperator {
		static inline timestamp_tz_t Operation(interval_t bucket_width, timestamp_tz_t ts, interval_t offset,
		                                       TZCalendar &calendar) {
			if (!ts.IsFinite()) {
				return ts;
			}
			const timestamp_tz_t origin(DEFAULT_ORIGIN_MICROS_1);
			return Add(calendar,
			           WidthConvertibleToDaysCommon(bucket_width.days, Sub(calendar, ts, offset), origin, calendar),
			           offset);
		}
	};

	struct OffsetWidthConvertibleToMonthsTernaryOperator {
		static inline timestamp_tz_t Operation(interval_t bucket_width, timestamp_tz_t ts, interval_t offset,
		                                       TZCalendar &calendar) {
			if (!ts.IsFinite()) {
				return ts;
			}
			const timestamp_tz_t origin(DEFAULT_ORIGIN_MICROS_2);
			return Add(calendar,
			           WidthConvertibleToMonthsCommon(bucket_width.months, Sub(calendar, ts, offset), origin, calendar),
			           offset);
		}
	};

	struct OffsetTernaryOperator {
		static inline timestamp_tz_t Operation(interval_t bucket_width, timestamp_tz_t ts, interval_t offset,
		                                       TZCalendar &calendar) {
			BucketWidthType bucket_width_type = ClassifyBucketWidthErrorThrow(bucket_width);
			switch (bucket_width_type) {
			case BucketWidthType::CONVERTIBLE_TO_MICROS:
				return OffsetWidthConvertibleToMicrosTernaryOperator::Operation(bucket_width, ts, offset, calendar);
			case BucketWidthType::CONVERTIBLE_TO_DAYS:
				return OffsetWidthConvertibleToDaysTernaryOperator::Operation(bucket_width, ts, offset, calendar);
			case BucketWidthType::CONVERTIBLE_TO_MONTHS:
				return OffsetWidthConvertibleToMonthsTernaryOperator::Operation(bucket_width, ts, offset, calendar);
			default:
				throw NotImplementedException("Bucket type not implemented for ICU TIME_BUCKET");
			}
		}
	};

	struct OriginWidthConvertibleToMicrosTernaryOperator {
		static inline timestamp_tz_t Operation(interval_t bucket_width, timestamp_tz_t ts, timestamp_tz_t origin,
		                                       TZCalendar &calendar) {
			if (!ts.IsFinite()) {
				return ts;
			}
			return WidthConvertibleToMicrosCommon(bucket_width.micros, ts, origin, calendar);
		}
	};

	struct OriginWidthConvertibleToDaysTernaryOperator {
		static inline timestamp_tz_t Operation(interval_t bucket_width, timestamp_tz_t ts, timestamp_tz_t origin,
		                                       TZCalendar &calendar) {
			if (!ts.IsFinite()) {
				return ts;
			}
			return WidthConvertibleToDaysCommon(bucket_width.days, ts, origin, calendar);
		}
	};

	struct OriginWidthConvertibleToMonthsTernaryOperator {
		static inline timestamp_tz_t Operation(interval_t bucket_width, timestamp_tz_t ts, timestamp_tz_t origin,
		                                       TZCalendar &calendar) {
			if (!ts.IsFinite()) {
				return ts;
			}
			return WidthConvertibleToMonthsCommon(bucket_width.months, ts, origin, calendar);
		}
	};

	struct OriginTernaryOperator {
		static inline optional<timestamp_tz_t> Operation(interval_t bucket_width, timestamp_tz_t ts,
		                                                 timestamp_tz_t origin, TZCalendar &calendar) {
			if (!origin.IsFinite()) {
				return nullopt;
			}
			BucketWidthType bucket_width_type = ClassifyBucketWidthErrorThrow(bucket_width);
			switch (bucket_width_type) {
			case BucketWidthType::CONVERTIBLE_TO_MICROS:
				return OriginWidthConvertibleToMicrosTernaryOperator::Operation(bucket_width, ts, origin, calendar);
			case BucketWidthType::CONVERTIBLE_TO_DAYS:
				return OriginWidthConvertibleToDaysTernaryOperator::Operation(bucket_width, ts, origin, calendar);
			case BucketWidthType::CONVERTIBLE_TO_MONTHS:
				return OriginWidthConvertibleToMonthsTernaryOperator::Operation(bucket_width, ts, origin, calendar);
			default:
				throw NotImplementedException("Bucket type not implemented for ICU TIME_BUCKET");
			}
		}
	};

	struct TimeZoneWidthConvertibleToMicrosBinaryOperator {
		static inline timestamp_tz_t Operation(interval_t bucket_width, timestamp_tz_t ts, timestamp_tz_t origin,
		                                       TZCalendar &calendar) {
			if (!ts.IsFinite()) {
				return ts;
			}
			return WidthConvertibleToMicrosCommon(bucket_width.micros, ts, origin, calendar);
		}
	};

	struct TimeZoneWidthConvertibleToDaysBinaryOperator {
		static inline timestamp_tz_t Operation(interval_t bucket_width, timestamp_tz_t ts, timestamp_tz_t origin,
		                                       TZCalendar &calendar) {
			if (!ts.IsFinite()) {
				return ts;
			}
			return WidthConvertibleToDaysCommon(bucket_width.days, ts, origin, calendar);
		}
	};

	struct TimeZoneWidthConvertibleToMonthsBinaryOperator {
		static inline timestamp_tz_t Operation(interval_t bucket_width, timestamp_tz_t ts, timestamp_tz_t origin,
		                                       TZCalendar &calendar) {
			if (!ts.IsFinite()) {
				return ts;
			}
			return WidthConvertibleToMonthsCommon(bucket_width.months, ts, origin, calendar);
		}
	};

	struct TimeZoneTernaryOperator {
		static inline timestamp_tz_t Operation(interval_t bucket_width, timestamp_tz_t ts, string_t tz,
		                                       TZCalendar &calendar_p, CalendarCacheState &cache) {
			auto calendar = calendar_p.GetCalendar();
			cache.SetTimeZone(calendar, tz);

			timestamp_tz_t origin;
			BucketWidthType bucket_width_type = ClassifyBucketWidthErrorThrow(bucket_width);
			switch (bucket_width_type) {
			case BucketWidthType::CONVERTIBLE_TO_MICROS:
				origin = ICUDateFunc::FromNaive(calendar, Timestamp::FromEpochMicroSeconds(DEFAULT_ORIGIN_MICROS_1));
				return TimeZoneWidthConvertibleToMicrosBinaryOperator::Operation(bucket_width, ts, origin, calendar_p);
			case BucketWidthType::CONVERTIBLE_TO_DAYS:
				origin = ICUDateFunc::FromNaive(calendar, Timestamp::FromEpochMicroSeconds(DEFAULT_ORIGIN_MICROS_1));
				return TimeZoneWidthConvertibleToDaysBinaryOperator::Operation(bucket_width, ts, origin, calendar_p);
			case BucketWidthType::CONVERTIBLE_TO_MONTHS:
				origin = ICUDateFunc::FromNaive(calendar, Timestamp::FromEpochMicroSeconds(DEFAULT_ORIGIN_MICROS_2));
				return TimeZoneWidthConvertibleToMonthsBinaryOperator::Operation(bucket_width, ts, origin, calendar_p);
			default:
				throw NotImplementedException("Bucket type not implemented for ICU TIME_BUCKET");
			}
		}
	};

	template <typename TB>
	static void ICUTimeBucketFunction(DataChunk &args, ExpressionState &state, Vector &result) {
		D_ASSERT(args.ColumnCount() == 2);

		auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
		auto &info = func_expr.BindInfo()->Cast<BindData>();
		auto &cache = ExecuteFunctionState::GetFunctionState(state)->Cast<CalendarCacheState>();
		TZCalendar calendar(*info.calendar, info.cal_setting);
		cache.SetTimeZone(calendar.GetCalendar(), string_t("UTC"));

		const auto &bucket_width_arg = args.data[0];
		const auto &ts_arg = args.data[1];

		if (bucket_width_arg.GetVectorType() == VectorType::CONSTANT_VECTOR) {
			if (ConstantVector::IsNull(bucket_width_arg)) {
				throw InternalException("ICUTimeBucket called with constant NULL bucket");
			}
			interval_t bucket_width = *ConstantVector::GetData<interval_t>(bucket_width_arg);
			BucketWidthType bucket_width_type = ClassifyBucketWidth(bucket_width);
			switch (bucket_width_type) {
			case BucketWidthType::CONVERTIBLE_TO_MICROS:
				BinaryExecutor::Execute<interval_t, TB, timestamp_tz_t>(
				    bucket_width_arg, ts_arg, result, [&](interval_t bucket_width, TB tb) {
					    const auto ts = TruncCast<TB, timestamp_tz_t>(tb);
					    return WidthConvertibleToMicrosBinaryOperator::Operation(bucket_width, ts, calendar);
				    });
				break;
			case BucketWidthType::CONVERTIBLE_TO_DAYS:
				BinaryExecutor::Execute<interval_t, TB, timestamp_tz_t>(
				    bucket_width_arg, ts_arg, result, [&](interval_t bucket_width, TB tb) {
					    const auto ts = TruncCast<TB, timestamp_tz_t>(tb);
					    return WidthConvertibleToDaysBinaryOperator::Operation(bucket_width, ts, calendar);
				    });
				break;
			case BucketWidthType::CONVERTIBLE_TO_MONTHS:
				BinaryExecutor::Execute<interval_t, TB, timestamp_tz_t>(
				    bucket_width_arg, ts_arg, result, [&](interval_t bucket_width, TB tb) {
					    const auto ts = TruncCast<TB, timestamp_tz_t>(tb);
					    return WidthConvertibleToMonthsBinaryOperator::Operation(bucket_width, ts, calendar);
				    });
				break;
			case BucketWidthType::UNCLASSIFIED:
				BinaryExecutor::Execute<interval_t, TB, timestamp_tz_t>(
				    bucket_width_arg, ts_arg, result, [&](interval_t bucket_width, TB tb) {
					    const auto ts = TruncCast<TB, timestamp_tz_t>(tb);
					    return BinaryOperator::Operation(bucket_width, ts, calendar);
				    });
				break;
			default:
				throw NotImplementedException("Bucket type not implemented for ICU TIME_BUCKET");
			}
		} else {
			BinaryExecutor::Execute<interval_t, TB, timestamp_tz_t>(
			    bucket_width_arg, ts_arg, result, [&](interval_t bucket_width, TB tb) {
				    const auto ts = TruncCast<TB, timestamp_tz_t>(tb);
				    return BinaryOperator::Operation(bucket_width, ts, calendar);
			    });
		}
	}

	template <typename TB>
	static void ICUTimeBucketOffsetFunction(DataChunk &args, ExpressionState &state, Vector &result) {
		D_ASSERT(args.ColumnCount() == 3);

		auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
		auto &info = func_expr.BindInfo()->Cast<BindData>();
		auto &cache = ExecuteFunctionState::GetFunctionState(state)->Cast<CalendarCacheState>();
		TZCalendar calendar(*info.calendar, info.cal_setting);
		cache.SetTimeZone(calendar.GetCalendar(), string_t("UTC"));

		const auto &bucket_width_arg = args.data[0];
		const auto &ts_arg = args.data[1];
		const auto &offset_arg = args.data[2];

		if (bucket_width_arg.GetVectorType() == VectorType::CONSTANT_VECTOR) {
			if (ConstantVector::IsNull(bucket_width_arg)) {
				throw InternalException("ICUTimeBucketOffset called with constant NULL bucket");
			}
			interval_t bucket_width = *ConstantVector::GetData<interval_t>(bucket_width_arg);
			BucketWidthType bucket_width_type = ClassifyBucketWidth(bucket_width);
			switch (bucket_width_type) {
			case BucketWidthType::CONVERTIBLE_TO_MICROS:
				TernaryExecutor::Execute<interval_t, TB, interval_t, timestamp_tz_t>(
				    bucket_width_arg, ts_arg, offset_arg, result,
				    [&](interval_t bucket_width, TB tb, interval_t offset) {
					    const auto ts = TruncCast<TB, timestamp_tz_t>(tb);
					    return OffsetWidthConvertibleToMicrosTernaryOperator::Operation(bucket_width, ts, offset,
					                                                                    calendar);
				    });
				break;
			case BucketWidthType::CONVERTIBLE_TO_DAYS:
				TernaryExecutor::Execute<interval_t, TB, interval_t, timestamp_tz_t>(
				    bucket_width_arg, ts_arg, offset_arg, result,
				    [&](interval_t bucket_width, TB tb, interval_t offset) {
					    const auto ts = TruncCast<TB, timestamp_tz_t>(tb);
					    return OffsetWidthConvertibleToDaysTernaryOperator::Operation(bucket_width, ts, offset,
					                                                                  calendar);
				    });
				break;
			case BucketWidthType::CONVERTIBLE_TO_MONTHS:
				TernaryExecutor::Execute<interval_t, TB, interval_t, timestamp_tz_t>(
				    bucket_width_arg, ts_arg, offset_arg, result,
				    [&](interval_t bucket_width, TB tb, interval_t offset) {
					    const auto ts = TruncCast<TB, timestamp_tz_t>(tb);
					    return OffsetWidthConvertibleToMonthsTernaryOperator::Operation(bucket_width, ts, offset,
					                                                                    calendar);
				    });
				break;
			case BucketWidthType::UNCLASSIFIED:
				TernaryExecutor::Execute<interval_t, TB, interval_t, timestamp_tz_t>(
				    bucket_width_arg, ts_arg, offset_arg, result,
				    [&](interval_t bucket_width, TB tb, interval_t offset) {
					    const auto ts = TruncCast<TB, timestamp_tz_t>(tb);
					    return OffsetTernaryOperator::Operation(bucket_width, ts, offset, calendar);
				    });
				break;
			default:
				throw NotImplementedException("Bucket type not implemented for ICU TIME_BUCKET");
			}
		} else {
			TernaryExecutor::Execute<interval_t, timestamp_tz_t, interval_t, timestamp_tz_t>(
			    bucket_width_arg, ts_arg, offset_arg, result,
			    [&](interval_t bucket_width, timestamp_tz_t ts, interval_t offset) {
				    return OffsetTernaryOperator::Operation(bucket_width, ts, offset, calendar);
			    });
		}
	}

	template <typename TB>
	static void ICUTimeBucketOriginFunction(DataChunk &args, ExpressionState &state, Vector &result) {
		D_ASSERT(args.ColumnCount() == 3);

		auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
		auto &info = func_expr.BindInfo()->Cast<BindData>();
		auto &cache = ExecuteFunctionState::GetFunctionState(state)->Cast<CalendarCacheState>();
		TZCalendar calendar(*info.calendar, info.cal_setting);
		cache.SetTimeZone(calendar.GetCalendar(), string_t("UTC"));

		const auto &bucket_width_arg = args.data[0];
		const auto &ts_arg = args.data[1];
		const auto &origin_arg = args.data[2];

		if (bucket_width_arg.GetVectorType() == VectorType::CONSTANT_VECTOR &&
		    origin_arg.GetVectorType() == VectorType::CONSTANT_VECTOR) {
			if (ConstantVector::IsNull(bucket_width_arg) || ConstantVector::IsNull(origin_arg) ||
			    !Value::IsFinite(*ConstantVector::GetData<timestamp_tz_t>(origin_arg))) {
				ConstantVector::SetNull(result, count_t(args.size()));
			} else {
				interval_t bucket_width = *ConstantVector::GetData<interval_t>(bucket_width_arg);
				BucketWidthType bucket_width_type = ClassifyBucketWidth(bucket_width);
				switch (bucket_width_type) {
				case BucketWidthType::CONVERTIBLE_TO_MICROS:
					TernaryExecutor::Execute<interval_t, TB, timestamp_tz_t, timestamp_tz_t>(
					    bucket_width_arg, ts_arg, origin_arg, result,
					    [&](interval_t bucket_width, TB tb, timestamp_tz_t origin) {
						    const auto ts = TruncCast<TB, timestamp_tz_t>(tb);
						    return OriginWidthConvertibleToMicrosTernaryOperator::Operation(bucket_width, ts, origin,
						                                                                    calendar);
					    });
					break;
				case BucketWidthType::CONVERTIBLE_TO_DAYS:
					TernaryExecutor::Execute<interval_t, TB, timestamp_tz_t, timestamp_tz_t>(
					    bucket_width_arg, ts_arg, origin_arg, result,
					    [&](interval_t bucket_width, TB tb, timestamp_tz_t origin) {
						    const auto ts = TruncCast<TB, timestamp_tz_t>(tb);
						    return OriginWidthConvertibleToDaysTernaryOperator::Operation(bucket_width, ts, origin,
						                                                                  calendar);
					    });
					break;
				case BucketWidthType::CONVERTIBLE_TO_MONTHS:
					TernaryExecutor::Execute<interval_t, TB, timestamp_tz_t, timestamp_tz_t>(
					    bucket_width_arg, ts_arg, origin_arg, result,
					    [&](interval_t bucket_width, TB tb, timestamp_tz_t origin) {
						    const auto ts = TruncCast<TB, timestamp_tz_t>(tb);
						    return OriginWidthConvertibleToMonthsTernaryOperator::Operation(bucket_width, ts, origin,
						                                                                    calendar);
					    });
					break;
				case BucketWidthType::UNCLASSIFIED:
					TernaryExecutor::Execute<interval_t, TB, timestamp_tz_t, timestamp_tz_t>(
					    bucket_width_arg, ts_arg, origin_arg, result,
					    [&](interval_t bucket_width, TB tb, timestamp_tz_t origin) -> optional<timestamp_tz_t> {
						    const auto ts = TruncCast<TB, timestamp_tz_t>(tb);
						    return OriginTernaryOperator::Operation(bucket_width, ts, origin, calendar);
					    });
					break;
				default:
					throw NotImplementedException("Bucket type not implemented for ICU TIME_BUCKET");
				}
			}
		} else {
			TernaryExecutor::Execute<interval_t, TB, timestamp_tz_t, timestamp_tz_t>(
			    bucket_width_arg, ts_arg, origin_arg, result,
			    [&](interval_t bucket_width, TB tb, timestamp_tz_t origin) -> optional<timestamp_tz_t> {
				    const auto ts = TruncCast<TB, timestamp_tz_t>(tb);
				    return OriginTernaryOperator::Operation(bucket_width, ts, origin, calendar);
			    });
		}
	}

	template <typename TB>
	static void ICUTimeBucketTimeZoneFunction(DataChunk &args, ExpressionState &state, Vector &result) {
		D_ASSERT(args.ColumnCount() == 3);

		auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
		auto &info = func_expr.BindInfo()->Cast<BindData>();
		auto &cache = ExecuteFunctionState::GetFunctionState(state)->Cast<CalendarCacheState>();
		TZCalendar calendar(*info.calendar, info.cal_setting);

		const auto &bucket_width_arg = args.data[0];
		const auto &ts_arg = args.data[1];
		const auto &tz_arg = args.data[2];

		if (bucket_width_arg.GetVectorType() == VectorType::CONSTANT_VECTOR &&
		    tz_arg.GetVectorType() == VectorType::CONSTANT_VECTOR) {
			if (ConstantVector::IsNull(bucket_width_arg) || ConstantVector::IsNull(tz_arg)) {
				throw InternalException("ICUTimeBucketTimeZone called with constant NULL bucket width or tz");
			}
			interval_t bucket_width = *ConstantVector::GetData<interval_t>(bucket_width_arg);
			cache.SetTimeZone(calendar.GetCalendar(), *ConstantVector::GetData<string_t>(tz_arg));
			timestamp_tz_t origin;
			BucketWidthType bucket_width_type = ClassifyBucketWidth(bucket_width);
			switch (bucket_width_type) {
			case BucketWidthType::CONVERTIBLE_TO_MICROS:
				origin = ICUDateFunc::FromNaive(calendar.GetCalendar(),
				                                Timestamp::FromEpochMicroSeconds(DEFAULT_ORIGIN_MICROS_1));
				BinaryExecutor::Execute<interval_t, TB, timestamp_tz_t>(
				    bucket_width_arg, ts_arg, result, [&](interval_t bucket_width, TB tb) {
					    const auto ts = TruncCast<TB, timestamp_tz_t>(tb);
					    return TimeZoneWidthConvertibleToMicrosBinaryOperator::Operation(bucket_width, ts, origin,
					                                                                     calendar);
				    });
				break;
			case BucketWidthType::CONVERTIBLE_TO_DAYS:
				origin = ICUDateFunc::FromNaive(calendar.GetCalendar(),
				                                Timestamp::FromEpochMicroSeconds(DEFAULT_ORIGIN_MICROS_1));
				BinaryExecutor::Execute<interval_t, TB, timestamp_tz_t>(
				    bucket_width_arg, ts_arg, result, [&](interval_t bucket_width, TB tb) {
					    const auto ts = TruncCast<TB, timestamp_tz_t>(tb);
					    return TimeZoneWidthConvertibleToDaysBinaryOperator::Operation(bucket_width, ts, origin,
					                                                                   calendar);
				    });
				break;
			case BucketWidthType::CONVERTIBLE_TO_MONTHS:
				origin = ICUDateFunc::FromNaive(calendar.GetCalendar(),
				                                Timestamp::FromEpochMicroSeconds(DEFAULT_ORIGIN_MICROS_2));
				BinaryExecutor::Execute<interval_t, TB, timestamp_tz_t>(
				    bucket_width_arg, ts_arg, result, [&](interval_t bucket_width, TB tb) {
					    const auto ts = TruncCast<TB, timestamp_tz_t>(tb);
					    return TimeZoneWidthConvertibleToMonthsBinaryOperator::Operation(bucket_width, ts, origin,
					                                                                     calendar);
				    });
				break;
			case BucketWidthType::UNCLASSIFIED:
				TernaryExecutor::Execute<interval_t, TB, string_t, timestamp_tz_t>(
				    bucket_width_arg, ts_arg, tz_arg, result, [&](interval_t bucket_width, TB tb, string_t tz) {
					    const auto ts = TruncCast<TB, timestamp_tz_t>(tb);
					    return TimeZoneTernaryOperator::Operation(bucket_width, ts, tz, calendar, cache);
				    });
				break;
			default:
				throw NotImplementedException("Bucket type not implemented for ICU TIME_BUCKET");
			}
		} else {
			TernaryExecutor::Execute<interval_t, TB, string_t, timestamp_tz_t>(
			    bucket_width_arg, ts_arg, tz_arg, result, [&](interval_t bucket_width, TB tb, string_t tz) {
				    const auto ts = TruncCast<TB, timestamp_tz_t>(tb);
				    return TimeZoneTernaryOperator::Operation(bucket_width, ts, tz, calendar, cache);
			    });
		}
	}

	template <typename TB>
	static ScalarFunction GetTimeBucketFunction(const LogicalTypeId &type) {
		ScalarFunction fun({}, LogicalType::TIMESTAMP_TZ, ICUTimeBucketFunction<TB>, Bind);
		fun.GetSignature().AddParameter("bucket_width", LogicalType::INTERVAL).AddParameter("timestamp", type);
		return fun;
	}

	template <typename TB>
	static ScalarFunction GetTimeBucketOffsetFunction(const LogicalTypeId &type) {
		ScalarFunction fun({}, LogicalType::TIMESTAMP_TZ, ICUTimeBucketOffsetFunction<TB>, Bind);
		fun.GetSignature()
		    .AddParameter("bucket_width", LogicalType::INTERVAL)
		    .AddParameter("timestamp", type)
		    .AddParameter("offset", LogicalType::INTERVAL);
		return fun;
	}

	template <typename TB>
	static ScalarFunction GetTimeBucketOriginFunction(const LogicalTypeId &type) {
		ScalarFunction fun({}, LogicalType::TIMESTAMP_TZ, ICUTimeBucketOriginFunction<TB>, Bind);
		fun.GetSignature()
		    .AddParameter("bucket_width", LogicalType::INTERVAL)
		    .AddParameter("timestamp", type)
		    .AddParameter("origin", LogicalType::TIMESTAMP_TZ);
		return fun;
	}

	template <typename TB>
	static ScalarFunction GetTimeBucketTimeZoneFunction(const LogicalTypeId &type) {
		ScalarFunction fun({}, LogicalType::TIMESTAMP_TZ, ICUTimeBucketTimeZoneFunction<TB>, Bind);
		fun.GetSignature()
		    .AddParameter("bucket_width", LogicalType::INTERVAL)
		    .AddParameter("timestamp", type)
		    .AddParameter("timezone", LogicalType::VARCHAR);
		return fun;
	}

	static void AddTimeBucketFunction(ExtensionLoader &loader) {
		ScalarFunctionSet set("time_bucket");
		set.AddFunction(GetTimeBucketFunction<timestamp_tz_t>(LogicalType::TIMESTAMP_TZ));
		set.AddFunction(GetTimeBucketFunction<timestamp_tz_ns_t>(LogicalType::TIMESTAMP_TZ_NS));

		set.AddFunction(GetTimeBucketOffsetFunction<timestamp_tz_t>(LogicalType::TIMESTAMP_TZ));
		set.AddFunction(GetTimeBucketOffsetFunction<timestamp_tz_ns_t>(LogicalType::TIMESTAMP_TZ_NS));

		set.AddFunction(GetTimeBucketOriginFunction<timestamp_tz_t>(LogicalType::TIMESTAMP_TZ));
		set.AddFunction(GetTimeBucketOriginFunction<timestamp_tz_ns_t>(LogicalType::TIMESTAMP_TZ_NS));

		set.AddFunction(GetTimeBucketTimeZoneFunction<timestamp_tz_t>(LogicalType::TIMESTAMP_TZ));
		set.AddFunction(GetTimeBucketTimeZoneFunction<timestamp_tz_ns_t>(LogicalType::TIMESTAMP_TZ_NS));

		set.ApplyToFunctions([](ScalarFunction &func) {
			func.SetFallible();
			func.SetStability(FunctionStability::CONSISTENT_WITHIN_QUERY);
			func.SetInitStateCallback(InitCalendarCache);
			func.SetArgProperties(1, ArgProperties().NonDecreasing());
		});
		loader.RegisterFunction(set);
	}
};

void RegisterICUTimeBucketFunctions(ExtensionLoader &loader) {
	ICUTimeBucket::AddTimeBucketFunction(loader);
}

} // namespace duckdb
