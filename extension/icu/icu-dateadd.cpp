#include "include/icu-dateadd.hpp"

#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/common/operator/multiply.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "include/icu-datefunc.hpp"
#include "icu-helpers.hpp"

namespace duckdb {

static duckdb::unique_ptr<FunctionData> ICUBindIntervalMonths(BindScalarFunctionInput &input) {
	auto result = ICUDateFunc::Bind(input);

	auto &info = result->Cast<ICUDateFunc::BindData>();
	TZCalendar calendar(*info.calendar, info.cal_setting);
	if (!calendar.SupportsIntervals()) {
		throw NotImplementedException("INTERVALs do not work with 13 month calendars. Try using DATE_DIFF instead.");
	}
	return std::move(result);
}

struct ICUCalendarAdd {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA left, TB right, TZCalendar &calendar_p) {
		throw InternalException("Unimplemented type for ICUCalendarAdd");
	}
};

struct ICUCalendarSub : public ICUDateFunc {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA left, TB right, TZCalendar &calendar_p) {
		throw InternalException("Unimplemented type for ICUCalendarSub");
	}
};

struct ICUCalendarAge : public ICUDateFunc {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA left, TB right, TZCalendar &calendar_p) {
		throw InternalException("Unimplemented type for ICUCalendarAge");
	}
};

static inline void CalendarAddHour(Calendar *calendar, int64_t interval_hour) {
	if (interval_hour >= 0) {
		while (interval_hour > 0) {
			calendar->Add(CAL_HOUR, interval_hour > NumericLimits<int32_t>::Maximum()
			                            ? NumericLimits<int32_t>::Maximum()
			                            : static_cast<int32_t>(interval_hour));
			interval_hour -= NumericLimits<int32_t>::Maximum();
		}
	} else {
		while (interval_hour < 0) {
			calendar->Add(CAL_HOUR, interval_hour < NumericLimits<int32_t>::Minimum()
			                            ? NumericLimits<int32_t>::Minimum()
			                            : static_cast<int32_t>(interval_hour));
			interval_hour -= NumericLimits<int32_t>::Minimum();
		}
	}
}

template <>
timestamp_tz_t ICUCalendarAdd::Operation(timestamp_tz_t timestamp, interval_t interval, TZCalendar &calendar_p) {
	if (!timestamp.IsFinite()) {
		return timestamp;
	}
	auto calendar = calendar_p.GetCalendar();

	int64_t millis = timestamp.value / Interval::MICROS_PER_MSEC;
	int64_t micros = timestamp.value % Interval::MICROS_PER_MSEC;

	// Manually move the µs
	micros += interval.micros % Interval::MICROS_PER_MSEC;
	if (micros >= Interval::MICROS_PER_MSEC) {
		micros -= Interval::MICROS_PER_MSEC;
		++millis;
	} else if (micros < 0) {
		micros += Interval::MICROS_PER_MSEC;
		--millis;
	}

	// Now use the calendar to add the other parts
	calendar->SetTime(millis);

	// Break units apart to avoid overflow
	auto interval_h = interval.micros / Interval::MICROS_PER_MSEC;

	const auto interval_ms = static_cast<int32_t>(interval_h % Interval::MSECS_PER_SEC);
	interval_h /= Interval::MSECS_PER_SEC;

	const auto interval_s = static_cast<int32_t>(interval_h % Interval::SECS_PER_MINUTE);
	interval_h /= Interval::SECS_PER_MINUTE;

	const auto interval_m = static_cast<int32_t>(interval_h % Interval::MINS_PER_HOUR);
	interval_h /= Interval::MINS_PER_HOUR;

	if (interval.months < 0 || interval.days < 0 || interval.micros < 0) {
		// Add interval fields from lowest to highest (non-ragged to ragged)
		calendar->Add(CAL_MILLISECOND, interval_ms);
		calendar->Add(CAL_SECOND, interval_s);
		calendar->Add(CAL_MINUTE, interval_m);
		CalendarAddHour(calendar, interval_h);

		// PG Adds months before days
		calendar->Add(CAL_MONTH, interval.months);
		calendar->Add(CAL_DATE, interval.days);
	} else {
		// PG Adds months before days
		calendar->Add(CAL_MONTH, interval.months);
		calendar->Add(CAL_DATE, interval.days);

		// Add interval fields from highest to lowest (ragged to non-ragged)
		CalendarAddHour(calendar, interval_h);
		calendar->Add(CAL_MINUTE, interval_m);
		calendar->Add(CAL_SECOND, interval_s);
		calendar->Add(CAL_MILLISECOND, interval_ms);
	}

	return ICUDateFunc::GetTime(calendar, micros);
}

template <>
timestamp_tz_t ICUCalendarAdd::Operation(interval_t interval, timestamp_tz_t timestamp, TZCalendar &calendar) {
	return Operation<timestamp_tz_t, interval_t, timestamp_tz_t>(timestamp, interval, calendar);
}

template <>
timestamp_tz_t ICUCalendarSub::Operation(timestamp_tz_t timestamp, interval_t interval, TZCalendar &calendar) {
	const auto negated = Interval::Invert(interval);
	return ICUCalendarAdd::template Operation<timestamp_tz_t, interval_t, timestamp_tz_t>(timestamp, negated, calendar);
}

template <>
interval_t ICUCalendarSub::Operation(timestamp_tz_t end_date, timestamp_tz_t start_date, TZCalendar &calendar_p) {
	if (!end_date.IsFinite() || !start_date.IsFinite()) {
		throw InvalidInputException("Cannot subtract infinite timestamps");
	}
	if (start_date > end_date) {
		auto negated = Operation<timestamp_tz_t, timestamp_tz_t, interval_t>(start_date, end_date, calendar_p);
		return {-negated.months, -negated.days, -negated.micros};
	}
	auto calendar = calendar_p.GetCalendar();

	auto start_micros = ICUDateFunc::SetTime(calendar, start_date);
	auto end_micros = (uint64_t)(end_date.value % Interval::MICROS_PER_MSEC);

	// Borrow 1ms from end_date if we wrap. This works because start_date <= end_date
	// and if the µs are out of order, then there must be an extra ms.
	if (start_micros > (idx_t)end_micros) {
		end_date.value -= Interval::MICROS_PER_MSEC;
		end_micros += Interval::MICROS_PER_MSEC;
	}

	//	Timestamp differences do not use months, so start with days
	interval_t result;
	result.months = 0;
	result.days = SubtractField(calendar, CAL_DATE, end_date);

	auto hour_diff = SubtractField(calendar, CAL_HOUR_OF_DAY, end_date);
	auto min_diff = SubtractField(calendar, CAL_MINUTE, end_date);
	auto sec_diff = SubtractField(calendar, CAL_SECOND, end_date);
	auto ms_diff = SubtractField(calendar, CAL_MILLISECOND, end_date);
	auto micros_diff = UnsafeNumericCast<int32_t>(ms_diff * Interval::MICROS_PER_MSEC + (end_micros - start_micros));
	result.micros = Time::FromTime(hour_diff, min_diff, sec_diff, micros_diff).value;

	return result;
}

template <>
interval_t ICUCalendarAge::Operation(timestamp_tz_t end_date, timestamp_tz_t start_date, TZCalendar &calendar_p) {
	auto calendar = calendar_p.GetCalendar();
	if (calendar_p.IsGregorian()) {
		auto start_data = ICUHelpers::GetComponents(start_date, calendar);
		auto end_data = ICUHelpers::GetComponents(end_date, calendar);
		return Interval::GetAge(end_data, start_data, start_date > end_date);
	}
	// fallback for non-gregorian calendars, since Interval::GetAge does not handle
	if (start_date > end_date) {
		auto negated = Operation<timestamp_tz_t, timestamp_tz_t, interval_t>(start_date, end_date, calendar_p);
		return {-negated.months, -negated.days, -negated.micros};
	}

	auto start_micros = ICUDateFunc::SetTime(calendar, start_date);
	auto end_micros = (uint64_t)(end_date.value % Interval::MICROS_PER_MSEC);

	// Borrow 1ms from end_date if we wrap. This works because start_date <= end_date
	// and if the µs are out of order, then there must be an extra ms.
	if (start_micros > (idx_t)end_micros) {
		end_date.value -= Interval::MICROS_PER_MSEC;
		end_micros += Interval::MICROS_PER_MSEC;
	}

	//	Lunar calendars have uneven numbers of months, so we just diff months, not years
	interval_t result;
	result.months = SubtractField(calendar, CAL_MONTH, end_date);
	result.days = SubtractField(calendar, CAL_DATE, end_date);

	auto hour_diff = SubtractField(calendar, CAL_HOUR_OF_DAY, end_date);
	auto min_diff = SubtractField(calendar, CAL_MINUTE, end_date);
	auto sec_diff = SubtractField(calendar, CAL_SECOND, end_date);
	auto ms_diff = SubtractField(calendar, CAL_MILLISECOND, end_date);
	auto micros_diff = UnsafeNumericCast<int32_t>(ms_diff * Interval::MICROS_PER_MSEC + (end_micros - start_micros));
	result.micros = Time::FromTime(hour_diff, min_diff, sec_diff, micros_diff).value;

	return result;
}

struct ICUDateAdd : public ICUDateFunc {
	template <typename TA, typename TR, typename OP>
	static void ExecuteUnary(DataChunk &args, ExpressionState &state, Vector &result) {
		D_ASSERT(args.ColumnCount() == 1);

		auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
		auto &info = func_expr.BindInfo()->Cast<BindData>();
		TZCalendar calendar(*info.calendar, info.cal_setting);

		//	Subtract argument from current_date (at midnight)
		const auto end_date = CurrentMidnight(calendar.GetCalendar(), state);

		UnaryExecutor::Execute<TA, TR>(args.data[0], result, [&](TA start_date) {
			return OP::template Operation<timestamp_tz_t, TA, TR>(end_date, start_date, calendar);
		});
	}

	template <typename TA, typename TR, typename OP>
	inline static ScalarFunction GetUnaryDateFunction(const LogicalTypeId &left_type,
	                                                  const LogicalTypeId &result_type) {
		return ScalarFunction({left_type}, result_type, ExecuteUnary<TA, TR, OP>, ICUBindIntervalMonths);
	}

	template <typename TA, typename TB, typename TR, typename OP>
	static void ExecuteBinary(DataChunk &args, ExpressionState &state, Vector &result) {
		D_ASSERT(args.ColumnCount() == 2);

		auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
		auto &info = func_expr.BindInfo()->Cast<BindData>();
		TZCalendar calendar(*info.calendar, info.cal_setting);

		BinaryExecutor::Execute<TA, TB, TR>(args.data[0], args.data[1], result, [&](TA left, TB right) {
			return OP::template Operation<TA, TB, TR>(left, right, calendar);
		});
	}

	template <typename TA, typename TB, typename TR, typename OP>
	inline static ScalarFunction GetBinaryDateFunction(const LogicalTypeId &left_type, const LogicalTypeId &right_type,
	                                                   const LogicalTypeId &result_type) {
		return ScalarFunction({left_type, right_type}, result_type, ExecuteBinary<TA, TB, TR, OP>,
		                      ICUBindIntervalMonths);
	}

	template <typename TA, typename TB, typename OP>
	static ScalarFunction GetDateAddFunction(const LogicalTypeId &left_type, const LogicalTypeId &right_type) {
		return GetBinaryDateFunction<TA, TB, timestamp_tz_t, OP>(left_type, right_type, LogicalType::TIMESTAMP_TZ);
	}

	static void AddDateAddOperators(const Identifier &name, ExtensionLoader &loader) {
		//	temporal + interval
		ScalarFunctionSet set {name};
		auto tstz_interval_fun = GetDateAddFunction<timestamp_tz_t, interval_t, ICUCalendarAdd>(
		    LogicalType::TIMESTAMP_TZ, LogicalType::INTERVAL);
		tstz_interval_fun.GetSignature().GetParameter(0).SetName("left");
		tstz_interval_fun.GetSignature().GetParameter(1).SetName("right");
		set.AddFunction(tstz_interval_fun);
		auto interval_tstz_fun = GetDateAddFunction<interval_t, timestamp_tz_t, ICUCalendarAdd>(
		    LogicalType::INTERVAL, LogicalType::TIMESTAMP_TZ);
		interval_tstz_fun.GetSignature().GetParameter(0).SetName("left");
		interval_tstz_fun.GetSignature().GetParameter(1).SetName("right");
		set.AddFunction(interval_tstz_fun);
		// throws for dates that overflow the timestamp range
		set.SetFallible();
		set.SetStability(FunctionStability::CONSISTENT_WITHIN_QUERY);
		loader.RegisterFunction(set);
	}

	template <typename TA, typename OP>
	static ScalarFunction GetUnaryAgeFunction(const LogicalTypeId &left_type) {
		return GetUnaryDateFunction<TA, interval_t, OP>(left_type, LogicalType::INTERVAL);
	}

	template <typename TA, typename TB, typename OP>
	static ScalarFunction GetBinaryAgeFunction(const LogicalTypeId &left_type, const LogicalTypeId &right_type) {
		return GetBinaryDateFunction<TA, TB, interval_t, OP>(left_type, right_type, LogicalType::INTERVAL);
	}

	static void AddDateSubOperators(const Identifier &name, ExtensionLoader &loader) {
		//	temporal - interval
		ScalarFunctionSet set {name};
		auto tstz_interval_fun = GetDateAddFunction<timestamp_tz_t, interval_t, ICUCalendarSub>(
		    LogicalType::TIMESTAMP_TZ, LogicalType::INTERVAL);
		tstz_interval_fun.GetSignature().GetParameter(0).SetName("left");
		tstz_interval_fun.GetSignature().GetParameter(1).SetName("right");
		set.AddFunction(tstz_interval_fun);

		//	temporal - temporal
		auto tstz_tstz_fun = GetBinaryAgeFunction<timestamp_tz_t, timestamp_tz_t, ICUCalendarSub>(
		    LogicalType::TIMESTAMP_TZ, LogicalType::TIMESTAMP_TZ);
		tstz_tstz_fun.GetSignature().GetParameter(0).SetName("left");
		tstz_tstz_fun.GetSignature().GetParameter(1).SetName("right");
		set.AddFunction(tstz_tstz_fun);
		// throws for dates that overflow the timestamp range
		set.SetFallible();
		set.SetStability(FunctionStability::CONSISTENT_WITHIN_QUERY);
		loader.RegisterFunction(set);
	}

	static void AddDateAgeFunctions(const Identifier &name, ExtensionLoader &loader) {
		//	age(temporal, temporal)
		ScalarFunctionSet set {name};
		auto binary_fun = GetBinaryAgeFunction<timestamp_tz_t, timestamp_tz_t, ICUCalendarAge>(
		    LogicalType::TIMESTAMP_TZ, LogicalType::TIMESTAMP_TZ);
		binary_fun.GetSignature().GetParameter(0).SetName("timestamp1");
		binary_fun.GetSignature().GetParameter(1).SetName("timestamp2");
		binary_fun.SetStability(FunctionStability::CONSISTENT_WITHIN_QUERY);
		set.AddFunction(binary_fun);
		auto unary_fun = GetUnaryAgeFunction<timestamp_tz_t, ICUCalendarAge>(LogicalType::TIMESTAMP_TZ);
		unary_fun.GetSignature().GetParameter(0).SetName("timestamp");
		// the age is taken at the start of the transaction
		unary_fun.SetStability(FunctionStability::CONSISTENT_WITHIN_QUERY);
		set.AddFunction(unary_fun);
		// throws for dates that overflow the timestamp range
		set.SetFallible();
		loader.RegisterFunction(set);
	}
};

timestamp_tz_t ICUDateFunc::Add(TZCalendar &calendar, timestamp_tz_t timestamp, interval_t interval) {
	return ICUCalendarAdd::Operation<timestamp_tz_t, interval_t, timestamp_tz_t>(timestamp, interval, calendar);
}

timestamp_tz_t ICUDateFunc::Sub(TZCalendar &calendar, timestamp_tz_t timestamp, interval_t interval) {
	return ICUCalendarSub::Operation<timestamp_tz_t, interval_t, timestamp_tz_t>(timestamp, interval, calendar);
}

interval_t ICUDateFunc::Sub(TZCalendar &calendar, timestamp_tz_t end_date, timestamp_tz_t start_date) {
	return ICUCalendarSub::Operation<timestamp_tz_t, timestamp_tz_t, interval_t>(end_date, start_date, calendar);
}

void RegisterICUDateAddFunctions(ExtensionLoader &loader) {
	ICUDateAdd::AddDateAddOperators("+", loader);
	ICUDateAdd::AddDateSubOperators("-", loader);
	ICUDateAdd::AddDateAgeFunctions("age", loader);
}

} // namespace duckdb
