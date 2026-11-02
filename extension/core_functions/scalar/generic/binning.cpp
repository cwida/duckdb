#include "duckdb/common/exception.hpp"
#include "duckdb/common/hugeint.hpp"
#include "duckdb/common/operator/subtract.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/vector_operations/generic_executor.hpp"
#include "core_functions/scalar/generic_functions.hpp"

namespace duckdb {

namespace {

hugeint_t GetPreviousPowerOfTen(hugeint_t input) {
	hugeint_t power_of_ten = 1;
	while (power_of_ten < input) {
		power_of_ten *= 10;
	}
	return power_of_ten / 10;
}

enum class NiceRounding { CEILING, ROUND };

hugeint_t RoundToNumber(hugeint_t input, hugeint_t num, NiceRounding rounding) {
	if (num == 0) {
		return input;
	}
	if (rounding == NiceRounding::ROUND) {
		return (input + (num / 2)) / num * num;
	} else {
		return (input + (num - 1)) / num * num;
	}
}

hugeint_t MakeNumberNice(hugeint_t input, hugeint_t step, NiceRounding rounding) {
	// we consider numbers nice if they are divisible by 2 or 5 times the power-of-ten one lower than the current
	// e.g. 120 is a nice number because it is divisible by 20
	//      122 is not a nice number -> we make it nice by turning it into 120 [/20]
	//      153 is not a nice number -> we make it nice by turning it into 150 [/50]
	//      1220 is not a nice number -> we turn it into 1200                  [/200]
	// first figure out the previous power of 10 (i.e. for 67 we return 10)
	// now the power of ten is the power BELOW the current number
	// i.e. for 67, it is not 10
	// now we can get the 2 or 5 divisors
	hugeint_t power_of_ten = GetPreviousPowerOfTen(step);
	hugeint_t two = power_of_ten * 2;
	hugeint_t five = power_of_ten;
	if (power_of_ten * 3 <= step) {
		two *= 5;
	}
	if (power_of_ten * 2 <= step) {
		five *= 5;
	}

	// compute the closest round number by adding the divisor / 2 and truncating
	// do this for both divisors
	hugeint_t round_to_two = RoundToNumber(input, two, rounding);
	hugeint_t round_to_five = RoundToNumber(input, five, rounding);
	// now pick the closest number of the two (i.e. for 147 we pick 150, not 140)
	if (AbsValue(input - round_to_two) < AbsValue(input - round_to_five)) {
		return round_to_two;
	} else {
		return round_to_five;
	}
}

double GetPreviousPowerOfTen(double input) {
	double power_of_ten = 1;
	if (input < 1) {
		while (power_of_ten > input) {
			power_of_ten /= 10;
		}
		return power_of_ten;
	}
	while (power_of_ten < input) {
		power_of_ten *= 10;
	}
	return power_of_ten / 10;
}

double RoundToNumber(double input, double num, NiceRounding rounding) {
	double result;
	if (rounding == NiceRounding::ROUND) {
		result = std::round(input / num) * num;
	} else {
		result = std::ceil(input / num) * num;
	}
	if (!Value::IsFinite(result)) {
		return input;
	}
	return result;
}

double MakeNumberNice(double input, const double step, NiceRounding rounding) {
	if (input == 0) {
		return 0;
	}
	// now the power of ten is the power BELOW the current number
	// i.e. for 67, it is not 10
	// now we can get the 2 or 5 divisors
	double power_of_ten = GetPreviousPowerOfTen(step);
	double two = power_of_ten * 2;
	double five = power_of_ten;
	if (power_of_ten * 3 <= step) {
		two *= 5;
	}
	if (power_of_ten * 2 <= step) {
		five *= 5;
	}

	double round_to_two = RoundToNumber(input, two, rounding);
	double round_to_five = RoundToNumber(input, five, rounding);
	// now pick the closest number of the two (i.e. for 147 we pick 150, not 140)
	if (AbsValue(input - round_to_two) < AbsValue(input - round_to_five)) {
		return round_to_two;
	} else {
		return round_to_five;
	}
}

struct EquiWidthBinsInteger {
	static constexpr LogicalTypeId LOGICAL_TYPE = LogicalTypeId::BIGINT;

	static vector<PrimitiveType<int64_t>> Operation(const Expression &expr, int64_t input_min, int64_t input_max,
	                                                idx_t bin_count, bool nice_rounding) {
		vector<PrimitiveType<int64_t>> result;
		// to prevent integer truncation from affecting the bin boundaries we calculate them with numbers multiplied by
		// 1000 we then divide to get the actual boundaries
		const auto FACTOR = hugeint_t(1000);
		auto min = hugeint_t(input_min) * FACTOR;
		auto max = hugeint_t(input_max) * FACTOR;

		const hugeint_t span = max - min;
		hugeint_t step = span / Hugeint::Convert(bin_count);
		if (step == 0) {
			// the bin count exceeds the number of boundaries in the range - clamp to the smallest possible step
			step = 1;
		}
		if (nice_rounding) {
			// when doing nice rounding we try to make the max/step values nicer
			hugeint_t new_step = MakeNumberNice(step, step, NiceRounding::ROUND);
			hugeint_t new_max = RoundToNumber(max, new_step, NiceRounding::CEILING);
			if (new_max != min && new_step != 0) {
				max = new_max;
				step = new_step;
			}
			// we allow for more bins when doing nice rounding since the bin count is approximate
			bin_count *= 2;
		}
		for (hugeint_t bin_boundary = max; bin_boundary > min; bin_boundary -= step) {
			const hugeint_t target_boundary = bin_boundary / FACTOR;
			int64_t real_boundary = Hugeint::Cast<int64_t>(target_boundary);
			if (!result.empty()) {
				if (real_boundary < input_min || result.size() >= bin_count) {
					// we can never generate input_min
					break;
				}
				if (real_boundary == result.back().val) {
					// we cannot generate the same value multiple times in a row - skip this step
					continue;
				}
			}
			result.push_back(real_boundary);
		}
		return result;
	}
};

struct EquiWidthBinsDouble {
	static constexpr LogicalTypeId LOGICAL_TYPE = LogicalTypeId::DOUBLE;

	static vector<PrimitiveType<double>> Operation(const Expression &expr, double min, double input_max,
	                                               idx_t bin_count, bool nice_rounding) {
		double max = input_max;
		if (!Value::IsFinite(min) || !Value::IsFinite(max)) {
			throw InvalidInputException("equi_width_bucket does not support infinite or nan as min/max value");
		}
		vector<PrimitiveType<double>> result;
		const double span = max - min;
		double step;
		if (!Value::IsFinite(span)) {
			// max - min does not fit
			step = max / static_cast<double>(bin_count) - min / static_cast<double>(bin_count);
		} else {
			step = span / static_cast<double>(bin_count);
		}
		const double step_power_of_ten = GetPreviousPowerOfTen(step);
		if (nice_rounding) {
			// when doing nice rounding we try to make the max/step values nicer
			step = MakeNumberNice(step, step, NiceRounding::ROUND);
			max = RoundToNumber(input_max, step, NiceRounding::CEILING);
			// we allow for more bins when doing nice rounding since the bin count is approximate
			bin_count *= 2;
		}

		const double round_multiplication = 10 / step_power_of_ten;
		if (max - step >= max || (nice_rounding && !Value::IsFinite(round_multiplication))) {
			// the span is too small to compute a step size - return only the max boundary
			result.push_back(max);
			return result;
		}
		for (double bin_boundary = max; bin_boundary > min; bin_boundary -= step) {
			// because floating point addition adds inaccuracies, we add rounding at every step
			double real_boundary = bin_boundary;
			if (nice_rounding) {
				real_boundary = std::round(bin_boundary * round_multiplication) / round_multiplication;
			}
			if (!result.empty() && result.back().val == real_boundary) {
				// skip this step
				continue;
			}
			if (real_boundary <= min || result.size() >= bin_count) {
				// we can never generate below input_min
				break;
			}
			result.push_back(real_boundary);
		}
		return result;
	}
};

void NextMonth(int32_t &year, int32_t &month) {
	month++;
	if (month == 13) {
		year++;
		month = 1;
	}
}

void NextDay(int32_t &year, int32_t &month, int32_t &day) {
	day++;
	if (!Date::IsValid(year, month, day)) {
		// day is out of range for month, move to next month
		NextMonth(year, month);
		day = 1;
	}
}

void NextHour(int32_t &year, int32_t &month, int32_t &day, int32_t &hour) {
	hour++;
	if (hour >= 24) {
		NextDay(year, month, day);
		hour = 0;
	}
}

void NextMinute(int32_t &year, int32_t &month, int32_t &day, int32_t &hour, int32_t &minute) {
	minute++;
	if (minute >= 60) {
		NextHour(year, month, day, hour);
		minute = 0;
	}
}

void NextSecond(int32_t &year, int32_t &month, int32_t &day, int32_t &hour, int32_t &minute, int32_t &sec) {
	sec++;
	if (sec >= 60) {
		NextMinute(year, month, day, hour, minute);
		sec = 0;
	}
}

timestamp_t MakeTimestampNice(int32_t year, int32_t month, int32_t day, int32_t hour, int32_t minute, int32_t sec,
                              int32_t micros, interval_t step) {
	// how to make a timestamp nice depends on the step
	if (step.months >= 12) {
		// if the step involves one year or more, ceil to months
		// set time component to 00:00:00.00
		if (day > 1 || hour > 0 || minute > 0 || sec > 0 || micros > 0) {
			// move to next month
			NextMonth(year, month);
			hour = minute = sec = micros = 0;
			day = 1;
		}
	} else if (step.months > 0 || step.days >= 1) {
		// if the step involves more than one day, ceil to days
		if (hour > 0 || minute > 0 || sec > 0 || micros > 0) {
			NextDay(year, month, day);
			hour = minute = sec = micros = 0;
		}
	} else if (step.days > 0 || step.micros >= Interval::MICROS_PER_HOUR) {
		// if the step involves more than one hour, ceil to hours
		if (minute > 0 || sec > 0 || micros > 0) {
			NextHour(year, month, day, hour);
			minute = sec = micros = 0;
		}
	} else if (step.micros >= Interval::MICROS_PER_MINUTE) {
		// if the step involves more than one minute, ceil to minutes
		if (sec > 0 || micros > 0) {
			NextMinute(year, month, day, hour, minute);
			sec = micros = 0;
		}
	} else if (step.micros >= Interval::MICROS_PER_SEC) {
		// if the step involves more than one second, ceil to seconds
		if (micros > 0) {
			NextSecond(year, month, day, hour, minute, sec);
			micros = 0;
		}
	}
	return Timestamp::FromDatetime(Date::FromDate(year, month, day), Time::FromTime(hour, minute, sec, micros));
}

int64_t RoundNumberToDivisor(int64_t number, int64_t divisor) {
	return (number + (divisor / 2)) / divisor * divisor;
}

interval_t MakeIntervalNice(interval_t interval) {
	if (interval.months >= 6) {
		// if we have more than 6 months, we don't care about days
		interval.days = 0;
		interval.micros = 0;
	} else if (interval.months > 0 || interval.days >= 5) {
		// if we have any months or more than 5 days, we don't care about micros
		interval.micros = 0;
	} else if (interval.days > 0 || interval.micros >= 6 * Interval::MICROS_PER_HOUR) {
		// if we any days or more than 6 hours, we want micros to be roundable by hours at least
		interval.micros = RoundNumberToDivisor(interval.micros, Interval::MICROS_PER_HOUR);
	} else if (interval.micros >= Interval::MICROS_PER_HOUR) {
		// if we have more than an hour, we want micros to be divisible by quarter hours
		interval.micros = RoundNumberToDivisor(interval.micros, Interval::MICROS_PER_MINUTE * 15);
	} else if (interval.micros >= Interval::MICROS_PER_MINUTE * 10) {
		// if we have more than 10 minutes, we want micros to be divisible by minutes
		interval.micros = RoundNumberToDivisor(interval.micros, Interval::MICROS_PER_MINUTE);
	} else if (interval.micros >= Interval::MICROS_PER_MINUTE) {
		// if we have more than a minute, we want micros to be divisible by quarter minutes
		interval.micros = RoundNumberToDivisor(interval.micros, Interval::MICROS_PER_SEC * 15);
	} else if (interval.micros >= Interval::MICROS_PER_SEC * 10) {
		// if we have more than 10 seconds, we want micros to be divisible by seconds
		interval.micros = RoundNumberToDivisor(interval.micros, Interval::MICROS_PER_SEC);
	}
	return interval;
}

void GetTimestampComponents(timestamp_t input, int32_t &year, int32_t &month, int32_t &day, int32_t &hour,
                            int32_t &minute, int32_t &sec, int32_t &micros) {
	date_t date;
	dtime_t time;

	Timestamp::Convert(input, date, time);
	Date::Convert(date, year, month, day);
	Time::Convert(time, hour, minute, sec, micros);
}

struct EquiWidthBinsTimestamp {
	static constexpr LogicalTypeId LOGICAL_TYPE = LogicalTypeId::TIMESTAMP;

	static vector<PrimitiveType<timestamp_t>> Operation(const Expression &expr, timestamp_t input_min,
	                                                    timestamp_t input_max, idx_t bin_count, bool nice_rounding) {
		if (!Value::IsFinite(input_min) || !Value::IsFinite(input_max)) {
			throw InvalidInputException(expr, "equi_width_bucket does not support infinite or nan as min/max value");
		}

		if (!nice_rounding) {
			// if we are not doing nice rounding it is pretty simple - just interpolate between the timestamp values
			auto interpolated_values =
			    EquiWidthBinsInteger::Operation(expr, input_min.value, input_max.value, bin_count, false);

			vector<PrimitiveType<timestamp_t>> result;
			for (auto &val : interpolated_values) {
				result.push_back(timestamp_t(val.val));
			}
			return result;
		}
		// fetch the components of the timestamps
		int32_t min_year, min_month, min_day, min_hour, min_minute, min_sec, min_micros;
		int32_t max_year, max_month, max_day, max_hour, max_minute, max_sec, max_micros;
		GetTimestampComponents(input_min, min_year, min_month, min_day, min_hour, min_minute, min_sec, min_micros);
		GetTimestampComponents(input_max, max_year, max_month, max_day, max_hour, max_minute, max_sec, max_micros);

		// get the interval differences per component
		// note: these can be negative (except for the largest non-zero difference)
		interval_t interval_diff;
		interval_diff.months = (max_year - min_year) * Interval::MONTHS_PER_YEAR + (max_month - min_month);
		interval_diff.days = max_day - min_day;
		interval_diff.micros = (max_hour - min_hour) * Interval::MICROS_PER_HOUR +
		                       (max_minute - min_minute) * Interval::MICROS_PER_MINUTE +
		                       (max_sec - min_sec) * Interval::MICROS_PER_SEC + (max_micros - min_micros);

		double step_months = static_cast<double>(interval_diff.months) / static_cast<double>(bin_count);
		double step_days = static_cast<double>(interval_diff.days) / static_cast<int32_t>(bin_count);
		double step_micros = static_cast<double>(interval_diff.micros) / static_cast<double>(bin_count);
		// since we truncate the months/days, propagate any fractional component to the unit below (i.e. 0.2 months
		// becomes 6 days)
		if (step_months > 0) {
			double overflow_months = step_months - std::floor(step_months);
			step_days += overflow_months * Interval::DAYS_PER_MONTH;
		}
		if (step_days > 0) {
			double overflow_days = step_days - std::floor(step_days);
			step_micros += overflow_days * Interval::MICROS_PER_DAY;
		}
		interval_t step;
		step.months = static_cast<int32_t>(step_months);
		step.days = static_cast<int32_t>(step_days);
		step.micros = static_cast<int64_t>(step_micros);

		// now we make the max, and the step nice
		step = MakeIntervalNice(step);
		timestamp_t timestamp_val =
		    MakeTimestampNice(max_year, max_month, max_day, max_hour, max_minute, max_sec, max_micros, step);
		if (step.months <= 0 && step.days <= 0 && step.micros <= 0) {
			// interval must be at least one microsecond
			step.months = step.days = 0;
			step.micros = 1;
		}

		vector<PrimitiveType<timestamp_t>> result;
		while (timestamp_val.value >= input_min.value && result.size() < bin_count) {
			result.push_back(timestamp_val);
			timestamp_val = SubtractOperator::Operation<timestamp_t, interval_t, timestamp_t>(timestamp_val, step);
		}
		return result;
	}
};

void EquiWidthResolveTypes(ResolveScalarFunctionTypesInput &input) {
	auto &bound_function = input.GetBoundFunction();
	auto &max_type = input.GetArgumentType(1);
	// while internally the bins are computed over a unified type
	// the equi_width_bins function returns the same type as the input MAX
	LogicalType child_type;
	switch (max_type.id()) {
	case LogicalTypeId::UNKNOWN:
	case LogicalTypeId::SQLNULL:
		return;
	case LogicalTypeId::DECIMAL:
		// for decimals we promote to double because
		child_type = LogicalType::DOUBLE;
		break;
	default:
		child_type = max_type;
		break;
	}
	bound_function.SetReturnType(LogicalType::LIST(child_type));
}

template <class T, class OP>
void EquiWidthBinFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	static constexpr int64_t MAX_BIN_COUNT = 1000000;
	const auto &min_arg = args.data[0];
	const auto &max_arg = args.data[1];
	const auto &bin_count = args.data[2];
	const auto &nice_rounding = args.data[3];

	Vector intermediate_result(LogicalType::LIST(OP::LOGICAL_TYPE));
	GenericExecutor::ExecuteQuaternary<PrimitiveType<T>, PrimitiveType<T>, PrimitiveType<int64_t>, PrimitiveType<bool>,
	                                   GenericListType<PrimitiveType<T>>>(
	    min_arg, max_arg, bin_count, nice_rounding, intermediate_result,
	    [&](PrimitiveType<T> min_p, PrimitiveType<T> max_p, PrimitiveType<int64_t> bins_p,
	        PrimitiveType<bool> nice_rounding_p) {
		    if (max_p.val < min_p.val) {
			    throw InvalidInputException(state.expr,
			                                "Invalid input for bin function - max value is smaller than min value");
		    }
		    if (bins_p.val <= 0) {
			    throw InvalidInputException(state.expr, "Invalid input for bin function - there must be > 0 bins");
		    }
		    if (bins_p.val > MAX_BIN_COUNT) {
			    throw InvalidInputException(state.expr, "Invalid input for bin function - max bin count of %d exceeded",
			                                MAX_BIN_COUNT);
		    }
		    GenericListType<PrimitiveType<T>> result_bins;
		    if (max_p.val == min_p.val) {
			    // if max = min return a single bucket
			    result_bins.values.push_back(max_p.val);
		    } else {
			    result_bins.values = OP::Operation(state.expr, min_p.val, max_p.val, static_cast<idx_t>(bins_p.val),
			                                       nice_rounding_p.val);
			    // last bin should always be the input max
			    if (result_bins.values[0].val < max_p.val) {
				    result_bins.values[0].val = max_p.val;
			    }
			    std::reverse(result_bins.values.begin(), result_bins.values.end());
		    }
		    return result_bins;
	    });
	VectorOperations::DefaultCast(intermediate_result, result, args.size());
}

void UnsupportedEquiWidth(DataChunk &args, ExpressionState &state, Vector &) {
	throw BinderException(state.expr, "Unsupported type \"%s\" for equi_width_bins", args.data[0].GetType());
}

void EquiWidthBinSerialize(Serializer &, const optional_ptr<FunctionData>, const BoundScalarFunction &) {
	return;
}

unique_ptr<FunctionData> EquiWidthBinDeserialize(Deserializer &deserializer, BoundScalarFunction &function) {
	function.SetReturnType(deserializer.Get<const LogicalType &>());
	return nullptr;
}

} // namespace

static void AddEquiWidthBinFunction(ScalarFunctionSet &functions, const LogicalType &min_max_type,
                                    scalar_function_t function) {
	ScalarFunction fun({}, LogicalType::LIST(LogicalType::ANY), std::move(function));
	fun.SetResolveTypesCallback(EquiWidthResolveTypes);
	fun.GetSignature()
	    .AddParameter("min", min_max_type)
	    .AddParameter("max", min_max_type)
	    .AddParameter("bin_count", LogicalType::BIGINT)
	    .AddParameter("nice_rounding", LogicalType::BOOLEAN);
	functions.AddFunction(std::move(fun));
}

ScalarFunctionSet EquiWidthBinsFun::GetFunctions() {
	ScalarFunctionSet functions("equi_width_bins");
	AddEquiWidthBinFunction(functions, LogicalType::BIGINT, EquiWidthBinFunction<int64_t, EquiWidthBinsInteger>);
	AddEquiWidthBinFunction(functions, LogicalType::DOUBLE, EquiWidthBinFunction<double, EquiWidthBinsDouble>);
	AddEquiWidthBinFunction(functions, LogicalType::TIMESTAMP,
	                        EquiWidthBinFunction<timestamp_t, EquiWidthBinsTimestamp>);
	AddEquiWidthBinFunction(functions, LogicalType::ANY_PARAMS(LogicalType::ANY, 150), UnsupportedEquiWidth);
	functions.ApplyToFunctions([](ScalarFunction &function) {
		function.SetSerializeCallback(EquiWidthBinSerialize);
		function.SetDeserializeCallback(EquiWidthBinDeserialize);
		function.SetFallible();
	});
	return functions;
}

} // namespace duckdb
