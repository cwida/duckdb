#include "duckdb/common/operator/add.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/operator/subtract.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/vector_operations/variadic_executor.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "include/icu-casts.hpp"
#include "include/icu-datefunc.hpp"
#include "include/icu-datetrunc.hpp"

#include <cmath>

namespace duckdb {

date_t ICUMakeDate::Operation(Calendar *calendar, timestamp_tz_t instant) {
	if (!instant.IsFinite()) {
		return Timestamp::GetDate(timestamp_t(instant));
	}

	// Extract the time zone parts
	SetTime(calendar, instant);
	const auto era = ExtractField(calendar, CAL_ERA);
	const auto year = ExtractField(calendar, CAL_YEAR);
	const auto mm = ExtractField(calendar, CAL_MONTH) + 1;
	const auto dd = ExtractField(calendar, CAL_DATE);

	const auto yyyy = era ? year : (-year + 1);
	date_t result;
	if (!Date::TryFromDate(yyyy, mm, dd, result)) {
		throw ConversionException("Unable to convert TIMESTAMPTZ to DATE");
	}

	return result;
}

date_t ICUMakeDate::ToDate(ClientContext &context, timestamp_tz_t instant) {
	ICUDateFunc::BindData data(context);
	return Operation(data.calendar.get(), instant);
}

bool ICUMakeDate::CastToDate(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	auto &cast_data = parameters.cast_data->Cast<CastData>();
	auto &info = cast_data.info->Cast<BindData>();
	CalendarPtr calendar(info.calendar->Copy());

	UnaryExecutor::Execute<timestamp_tz_t, date_t>(
	    source, result, count, [&](timestamp_tz_t input) { return Operation(calendar.get(), input); });
	return true;
}

bool ICUMakeDate::CastNsToDate(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	auto &cast_data = parameters.cast_data->Cast<CastData>();
	auto &info = cast_data.info->Cast<BindData>();
	CalendarPtr calendar(info.calendar->Copy());

	UnaryExecutor::Execute<timestamp_tz_ns_t, date_t>(source, result, count, [&](timestamp_tz_ns_t input) {
		if (!input.IsFinite()) {
			return Timestamp::GetDate(timestamp_t(input.value));
		}
		// the date only needs the microsecond instant: floor the nanoseconds so no instant moves past midnight
		const auto nanos = input.value;
		auto micros = nanos / Interval::NANOS_PER_MICRO;
		if (nanos % Interval::NANOS_PER_MICRO < 0) {
			micros--;
		}
		return Operation(calendar.get(), timestamp_tz_t(micros));
	});
	return true;
}

static unique_ptr<ICUDateFunc::CastData> BindCastToDateData(BindCastInput &input) {
	if (!input.context) {
		throw InternalException("Missing context for TIMESTAMPTZ to DATE cast.");
	}
	if (Settings::Get<DisableTimestamptzCastsSetting>(*input.context)) {
		throw BinderException("Casting from TIMESTAMP WITH TIME ZONE to DATE without an explicit time zone "
		                      "has been disabled  - use \"AT TIME ZONE ...\"");
	}
	return ICUDateFunc::BindCastData(input);
}

BoundCastInfo ICUMakeDate::BindCastToDate(BindCastInput &input, const LogicalType &source, const LogicalType &target) {
	return BoundCastInfo(CastToDate, BindCastToDateData(input));
}

BoundCastInfo ICUMakeDate::BindCastNsToDate(BindCastInput &input, const LogicalType &source,
                                            const LogicalType &target) {
	return BoundCastInfo(CastNsToDate, BindCastToDateData(input));
}

void ICUMakeDate::AddCasts(ExtensionLoader &loader) {
	loader.RegisterCastFunction(LogicalType::TIMESTAMP_TZ, LogicalType::DATE, BindCastToDate);
	loader.RegisterCastFunction(LogicalType::TIMESTAMP_TZ_NS, LogicalType::DATE, BindCastNsToDate);
}

struct ICUMakeTimestampTZFunc : public ICUDateFunc {
	template <typename T>
	static inline timestamp_tz_t Operation(Calendar *calendar, T yyyy, T mm, T dd, T hr, T mn, double ss) {
		const auto year = Cast::Operation<T, int32_t>(AddOperator::Operation<T, T, T>(yyyy, (yyyy < 0)));
		const auto month = Cast::Operation<T, int32_t>(SubtractOperatorOverflowCheck::Operation<T, T, T>(mm, 1));
		const auto day = Cast::Operation<T, int32_t>(dd);
		const auto hour = Cast::Operation<T, int32_t>(hr);
		const auto min = Cast::Operation<T, int32_t>(mn);

		const auto secs = Cast::Operation<double, int32_t>(ss);
		ss -= secs;
		ss *= Interval::MSECS_PER_SEC;
		const auto millis = int32_t(ss);
		int64_t micros = LossyNumericCast<int64_t, double>(std::round((ss - millis) * Interval::MICROS_PER_MSEC));

		calendar->Set(CAL_YEAR, year);
		calendar->Set(CAL_MONTH, month);
		calendar->Set(CAL_DATE, day);
		calendar->Set(CAL_HOUR_OF_DAY, hour);
		calendar->Set(CAL_MINUTE, min);
		calendar->Set(CAL_SECOND, secs);
		calendar->Set(CAL_MILLISECOND, millis);

		return GetTime(calendar, micros);
	}

	template <typename T>
	static void FromMicros(DataChunk &input, ExpressionState &state, Vector &result) {
		UnaryExecutor::Execute<T, timestamp_t>(input.data[0], result, [&](T micros) {
			const auto result = timestamp_t(micros);
			if (!result.IsFinite()) {
				throw ConversionException("Timestamp microseconds out of range: %ld", micros);
			}
			return result;
		});
	}

	template <typename T>
	static void Execute(DataChunk &input, ExpressionState &state, Vector &result) {
		auto &cache = ExecuteFunctionState::GetFunctionState(state)->Cast<CalendarCacheState>();

		// Three cases: no TZ, constant TZ, variable TZ
		if (input.ColumnCount() == 6) {
			auto calendar = cache.GetBaseCalendar();
			VariadicExecutor::Execute<timestamp_tz_t, T, T, T, T, T, double>(
			    input, result, [&](T yyyy, T mm, T dd, T hr, T mn, double ss) {
				    return Operation<T>(calendar, yyyy, mm, dd, hr, mn, ss);
			    });
		} else {
			D_ASSERT(input.ColumnCount() == 7);
			auto &tz_vec = input.data.back();
			if (tz_vec.GetVectorType() == VectorType::CONSTANT_VECTOR) {
				if (ConstantVector::IsNull(tz_vec)) {
					throw InternalException("ICUMakeTimestamp called with constant NULL tz");
				}
				auto calendar = cache.GetCalendar(*ConstantVector::GetData<string_t>(tz_vec));
				VariadicExecutor::Execute<timestamp_tz_t, T, T, T, T, T, double>(
				    input, result, [&](T yyyy, T mm, T dd, T hr, T mn, double ss) {
					    return Operation<T>(calendar, yyyy, mm, dd, hr, mn, ss);
				    });
			} else {
				VariadicExecutor::Execute<timestamp_tz_t, T, T, T, T, T, double, string_t>(
				    input, result, [&](T yyyy, T mm, T dd, T hr, T mn, double ss, string_t tz_id) {
					    return Operation<T>(cache.GetCalendar(tz_id), yyyy, mm, dd, hr, mn, ss);
				    });
			}
		}
	}

	template <typename TA>
	static ScalarFunction GetSenaryFunction(const LogicalTypeId &type) {
		ScalarFunction function({}, LogicalType::TIMESTAMP_TZ, Execute<TA>, Bind);
		function.GetSignature()
		    .AddParameter("year", type)
		    .AddParameter("month", type)
		    .AddParameter("day", type)
		    .AddParameter("hour", type)
		    .AddParameter("minute", type)
		    .AddParameter("seconds", LogicalType::DOUBLE);
		function.SetFallible();
		function.SetStability(FunctionStability::CONSISTENT_WITHIN_QUERY);
		function.SetInitStateCallback(InitCalendarCache);
		return function;
	}

	template <typename TA>
	static ScalarFunction GetSeptenaryFunction(const LogicalTypeId &type) {
		ScalarFunction function({}, LogicalType::TIMESTAMP_TZ, Execute<TA>, Bind);
		function.GetSignature()
		    .AddParameter("year", type)
		    .AddParameter("month", type)
		    .AddParameter("day", type)
		    .AddParameter("hour", type)
		    .AddParameter("minute", type)
		    .AddParameter("seconds", LogicalType::DOUBLE)
		    .AddParameter("timezone", LogicalType::VARCHAR);
		function.SetFallible();
		function.SetStability(FunctionStability::CONSISTENT_WITHIN_QUERY);
		function.SetInitStateCallback(InitCalendarCache);
		return function;
	}

	static void AddFunction(const Identifier &name, ExtensionLoader &loader) {
		ScalarFunctionSet set {name};
		set.AddFunction(GetSenaryFunction<int64_t>(LogicalType::BIGINT));
		set.AddFunction(GetSeptenaryFunction<int64_t>(LogicalType::BIGINT));
		ScalarFunction function({}, LogicalType::TIMESTAMP_TZ, FromMicros<int64_t>);
		function.GetSignature().AddParameter("micros", LogicalType::BIGINT);
		function.SetFallible();
		set.AddFunction(function);
		loader.RegisterFunction(set);
	}
};

void RegisterICUMakeDateFunctions(ExtensionLoader &loader) {
	ICUMakeTimestampTZFunc::AddFunction("make_timestamptz", loader);
	ICUMakeDate::AddCasts(loader);
}

} // namespace duckdb
