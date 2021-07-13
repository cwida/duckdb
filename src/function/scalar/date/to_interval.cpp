#include "duckdb/function/scalar/date_functions.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/operator/multiply.hpp"

namespace duckdb {

struct ToYearsOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		interval_t result;
		result.days = 0;
		result.micros = 0;
		if (!TryMultiplyOperator::Operation<int32_t, int32_t, int32_t>(input, Interval::MONTHS_PER_YEAR,
		                                                               result.months)) {
			throw OutOfRangeException("Interval value %d years out of range", input);
		}
		return result;
	}
};

struct ToMonthsOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		interval_t result;
		result.months = input;
		result.days = 0;
		result.micros = 0;
		return result;
	}
};

struct ToDaysOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		interval_t result;
		result.months = 0;
		result.days = input;
		result.micros = 0;
		return result;
	}
};

struct ToHoursOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		interval_t result;
		result.months = 0;
		result.days = 0;
		if (!TryMultiplyOperator::Operation<int64_t, int64_t, int64_t>(input, Interval::MICROS_PER_HOUR,
		                                                               result.micros)) {
			throw OutOfRangeException("Interval value %d hours out of range", input);
		}
		return result;
	}
};

struct ToMinutesOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		interval_t result;
		result.months = 0;
		result.days = 0;
		if (!TryMultiplyOperator::Operation<int64_t, int64_t, int64_t>(input, Interval::MICROS_PER_MINUTE,
		                                                               result.micros)) {
			throw OutOfRangeException("Interval value %d minutes out of range", input);
		}
		return result;
	}
};

struct ToSecondsOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		interval_t result;
		result.months = 0;
		result.days = 0;
		if (!TryMultiplyOperator::Operation<int64_t, int64_t, int64_t>(input, Interval::MICROS_PER_SEC,
		                                                               result.micros)) {
			throw OutOfRangeException("Interval value %d seconds out of range", input);
		}
		return result;
	}
};

struct ToMilliSecondsOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		interval_t result;
		result.months = 0;
		result.days = 0;
		if (!TryMultiplyOperator::Operation<int64_t, int64_t, int64_t>(input, Interval::MICROS_PER_MSEC,
		                                                               result.micros)) {
			throw OutOfRangeException("Interval value %d milliseconds out of range", input);
		}
		return result;
	}
};

struct ToMicroSecondsOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		interval_t result;
		result.months = 0;
		result.days = 0;
		result.micros = input;
		return result;
	}
};

void ToIntervalFun::RegisterFunction(BuiltinFunctions &set) {
	// register the individual operators
	set.AddFunction(ScalarFunction("to_years", {LogicalType::INTEGER}, LogicalType::INTERVAL,
	                               ScalarFunction::UnaryFunction<int32_t, interval_t, ToYearsOperator>));
	set.AddFunction(ScalarFunction("to_months", {LogicalType::INTEGER}, LogicalType::INTERVAL,
	                               ScalarFunction::UnaryFunction<int32_t, interval_t, ToMonthsOperator>));
	set.AddFunction(ScalarFunction("to_days", {LogicalType::INTEGER}, LogicalType::INTERVAL,
	                               ScalarFunction::UnaryFunction<int32_t, interval_t, ToDaysOperator>));
	set.AddFunction(ScalarFunction("to_hours", {LogicalType::BIGINT}, LogicalType::INTERVAL,
	                               ScalarFunction::UnaryFunction<int64_t, interval_t, ToHoursOperator>));
	set.AddFunction(ScalarFunction("to_minutes", {LogicalType::BIGINT}, LogicalType::INTERVAL,
	                               ScalarFunction::UnaryFunction<int64_t, interval_t, ToMinutesOperator>));
	set.AddFunction(ScalarFunction("to_seconds", {LogicalType::BIGINT}, LogicalType::INTERVAL,
	                               ScalarFunction::UnaryFunction<int64_t, interval_t, ToSecondsOperator>));
	set.AddFunction(ScalarFunction("to_milliseconds", {LogicalType::BIGINT}, LogicalType::INTERVAL,
	                               ScalarFunction::UnaryFunction<int64_t, interval_t, ToMilliSecondsOperator>));
	set.AddFunction(ScalarFunction("to_microseconds", {LogicalType::BIGINT}, LogicalType::INTERVAL,
	                               ScalarFunction::UnaryFunction<int64_t, interval_t, ToMicroSecondsOperator>));
}

} // namespace duckdb
