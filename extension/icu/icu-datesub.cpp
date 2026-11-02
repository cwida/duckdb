#include "include/icu-datesub.hpp"
#include "include/icu-datefunc.hpp"

#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/common/enums/date_part_specifier.hpp"
#include "duckdb/common/operator/subtract.hpp"
#include "duckdb/optimizer/statistics_propagator.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

struct ICUCalendarSub : public ICUDateFunc {
	static unique_ptr<FunctionData> Bind(BindScalarFunctionInput &input) {
		auto part_value = input.TryGetConstant(0);
		if (part_value && !part_value->IsNull()) {
			DatePartSpecifier part;
			if (TryGetDatePartSpecifier(part_value->GetValue<string>(), part) && part == DatePartSpecifier::ERA) {
				// date_sub is not monotone for eras because era boundaries can occur partway through a year
				input.GetBoundFunction().SetArgProperties({});
			}
		}
		return ICUDateFunc::Bind(input);
	}

	//	ICU only has 32 bit precision for date parts, so it can overflow a high resolution.
	//	Since there is no difference between ICU and the obvious calculations,
	//	we make these using the DuckDB internal type.
	static int64_t SubtractMicrosecond(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
		// the span of two representable timestamps does not always fit in one, so this needs the same
		// overflow check the non-timezone date_diff uses
		return SubtractOperatorOverflowCheck::Operation<int64_t, int64_t, int64_t>(end_date.value, start_date.value);
	}

	template <int64_t MICROS_PER_UNIT>
	static int64_t SubtractUnits(timestamp_tz_t start_date, timestamp_tz_t end_date) {
		int64_t delta_us;
		if (TrySubtractOperator::Operation(end_date.value, start_date.value, delta_us)) {
			return delta_us / MICROS_PER_UNIT;
		}
		return Interval::GetDifferenceInUnits(start_date.value, end_date.value, MICROS_PER_UNIT);
	}

	static int64_t SubtractMillisecond(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
		return SubtractUnits<Interval::MICROS_PER_MSEC>(start_date, end_date);
	}

	static int64_t SubtractSecond(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
		return SubtractUnits<Interval::MICROS_PER_SEC>(start_date, end_date);
	}

	static int64_t SubtractMinute(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
		return SubtractUnits<Interval::MICROS_PER_MINUTE>(start_date, end_date);
	}

	static int64_t SubtractHour(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
		SetTime(calendar, start_date);
		return SubtractField(calendar, CAL_HOUR_OF_DAY, end_date);
	}

	static int64_t SubtractDay(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
		SetTime(calendar, start_date);
		return SubtractField(calendar, CAL_DATE, end_date);
	}

	static int64_t SubtractWeek(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
		calendar->SetFirstDayOfWeek(CAL_MONDAY);
		calendar->SetMinimalDaysInFirstWeek(4);
		SetTime(calendar, start_date);
		return SubtractField(calendar, CAL_WEEK_OF_YEAR, end_date);
	}

	static int64_t SubtractMonth(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
		SetTime(calendar, start_date);
		return SubtractField(calendar, CAL_MONTH, end_date);
	}

	static int64_t SubtractQuarter(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
		// No ICU part for this, so do it manually.
		// This will not work for lunar calendars!
		return SubtractMonth(calendar, start_date, end_date) / 3;
	}

	static int64_t SubtractYear(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
		SetTime(calendar, start_date);
		return SubtractField(calendar, CAL_YEAR, end_date);
	}

	static int64_t SubtractISOYear(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
		calendar->SetFirstDayOfWeek(CAL_MONDAY);
		calendar->SetMinimalDaysInFirstWeek(4);
		SetTime(calendar, start_date);
		return SubtractField(calendar, CAL_YEAR_WOY, end_date);
	}

	static int64_t SubtractDecade(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
		// No ICU part for this, so do it manually.
		return SubtractYear(calendar, start_date, end_date) / 10;
	}

	static int64_t SubtractCentury(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
		// No ICU part for this, so do it manually.
		return SubtractYear(calendar, start_date, end_date) / 100;
	}

	static int64_t SubtractMillenium(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
		// No ICU part for this, so do it manually.
		return SubtractYear(calendar, start_date, end_date) / 1000;
	}

	static int64_t SubtractEra(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
		SetTime(calendar, start_date);
		return SubtractField(calendar, CAL_ERA, end_date);
	}

	template <typename T>
	static void ICUDateSubFunction(DataChunk &args, ExpressionState &state, Vector &result) {
		D_ASSERT(args.ColumnCount() == 3);
		const auto &part_arg = args.data[0];
		const auto &startdate_arg = args.data[1];
		const auto &enddate_arg = args.data[2];

		auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
		auto &info = func_expr.BindInfo()->Cast<BindData>();
		CalendarPtr calendar(info.calendar->Copy());

		if (part_arg.GetVectorType() == VectorType::CONSTANT_VECTOR) {
			// Common case of constant part.
			if (ConstantVector::IsNull(part_arg)) {
				throw InternalException("ICUDateSub called with constant NULL bucket width");
			}
			const auto specifier = ConstantVector::GetData<string_t>(part_arg)->GetString();
			auto part_func = SubtractFactory(GetDatePartSpecifier(specifier));
			BinaryExecutor::Execute<T, T, int64_t>(startdate_arg, enddate_arg, result,
			                                       [&](T start_date, T end_date) -> optional<int64_t> {
				                                       if (start_date.IsFinite() && end_date.IsFinite()) {
					                                       return part_func(calendar.get(), start_date, end_date);
				                                       } else {
					                                       return nullopt;
				                                       }
			                                       });
		} else {
			TernaryExecutor::Execute<string_t, T, T, int64_t>(
			    part_arg, startdate_arg, enddate_arg, result,
			    [&](string_t specifier, T start_date, T end_date) -> optional<int64_t> {
				    if (start_date.IsFinite() && end_date.IsFinite()) {
					    auto part_func = SubtractFactory(GetDatePartSpecifier(specifier.GetString()));
					    return part_func(calendar.get(), start_date, end_date);
				    } else {
					    return nullopt;
				    }
			    });
		}
	}

	template <typename TA>
	static ScalarFunction GetFunction(const LogicalTypeId &type) {
		ScalarFunction fun({}, LogicalType::BIGINT, ICUDateSubFunction<TA>, Bind);
		fun.GetSignature()
		    .AddParameter("part", LogicalType::VARCHAR)
		    .AddParameter("startdate", type)
		    .AddParameter("enddate", type);
		return fun;
	}

	static void AddFunctions(const Identifier &name, ExtensionLoader &loader) {
		ScalarFunctionSet set {name};
		set.AddFunction(GetFunction<timestamp_tz_t>(LogicalType::TIMESTAMP_TZ));
		// throws for unrecognized part specifiers and for dates that overflow the timestamp range
		set.SetFallible();
		set.SetStability(FunctionStability::CONSISTENT_WITHIN_QUERY);
		set.SetArgProperties(1, ArgProperties().NonIncreasing());
		set.SetArgProperties(2, ArgProperties().NonDecreasing());
		set.SetStatisticsCallback(StatisticsPropagator::PropagateFiniteMonotoneBounds);
		loader.RegisterFunction(set);
	}
};

ICUDateFunc::part_sub_t ICUDateFunc::SubtractFactory(DatePartSpecifier type) {
	switch (type) {
	case DatePartSpecifier::MILLENNIUM:
		return ICUCalendarSub::SubtractMillenium;
	case DatePartSpecifier::CENTURY:
		return ICUCalendarSub::SubtractCentury;
	case DatePartSpecifier::DECADE:
		return ICUCalendarSub::SubtractDecade;
	case DatePartSpecifier::YEAR:
		return ICUCalendarSub::SubtractYear;
	case DatePartSpecifier::QUARTER:
		return ICUCalendarSub::SubtractQuarter;
	case DatePartSpecifier::MONTH:
		return ICUCalendarSub::SubtractMonth;
	case DatePartSpecifier::WEEK:
	case DatePartSpecifier::YEARWEEK:
		return ICUCalendarSub::SubtractWeek;
	case DatePartSpecifier::ISOYEAR:
		return ICUCalendarSub::SubtractISOYear;
	case DatePartSpecifier::DAY:
	case DatePartSpecifier::DOW:
	case DatePartSpecifier::ISODOW:
	case DatePartSpecifier::DOY:
	case DatePartSpecifier::JULIAN_DAY:
		return ICUCalendarSub::SubtractDay;
	case DatePartSpecifier::HOUR:
		return ICUCalendarSub::SubtractHour;
	case DatePartSpecifier::MINUTE:
		return ICUCalendarSub::SubtractMinute;
	case DatePartSpecifier::SECOND:
	case DatePartSpecifier::EPOCH:
		return ICUCalendarSub::SubtractSecond;
	case DatePartSpecifier::MILLISECONDS:
		return ICUCalendarSub::SubtractMillisecond;
	case DatePartSpecifier::MICROSECONDS:
		return ICUCalendarSub::SubtractMicrosecond;
	case DatePartSpecifier::ERA:
		return ICUCalendarSub::SubtractEra;
	default:
		throw NotImplementedException("Specifier type not implemented for ICU subtraction");
	}
}

// MS-SQL differences can be computed using ICU by truncating both arguments
// to the desired part precision and then applying ICU subtraction/difference
struct ICUCalendarDiff : public ICUDateFunc {
	static unique_ptr<FunctionData> Bind(BindScalarFunctionInput &input) {
		auto part_value = input.TryGetConstant(0);
		if (part_value && !part_value->IsNull()) {
			DatePartSpecifier part;
			if (TryGetDatePartSpecifier(part_value->GetValue<string>(), part) &&
			    (part == DatePartSpecifier::DECADE || part == DatePartSpecifier::CENTURY ||
			     part == DatePartSpecifier::MILLENNIUM)) {
				// date_diff is not monotone for these parts because ICU years skip year 0 at the BC/AD boundary
				input.GetBoundFunction().SetArgProperties({});
			}
		}
		return ICUDateFunc::Bind(input);
	}

	static int64_t DifferenceEra(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
		SetTime(calendar, start_date);
		const auto start_era = ExtractField(calendar, CAL_ERA);
		SetTime(calendar, end_date);
		return int64_t(ExtractField(calendar, CAL_ERA)) - start_era;
	}

	static int64_t DifferenceEra(Calendar *calendar, timestamp_tz_ns_t start_date, timestamp_tz_ns_t end_date) {
		SetTimeNS(calendar, start_date);
		const auto start_era = ExtractField(calendar, CAL_ERA);
		SetTimeNS(calendar, end_date);
		return int64_t(ExtractField(calendar, CAL_ERA)) - start_era;
	}

	static timestamp_tz_t TruncateForDiff(Calendar *calendar, timestamp_tz_t date, part_trunc_t trunc_func) {
		auto micros = SetTime(calendar, date);
		trunc_func(calendar, micros);
		return GetTimeUnsafe(calendar, micros);
	}

	static timestamp_tz_t TruncateForDiff(Calendar *calendar, timestamp_tz_ns_t date, part_trunc_t trunc_func) {
		auto nanos = SetTimeNS(calendar, date);
		// Adapt TIMESTAMPTZ_NS to the existing microsecond-or-coarser date_diff path.
		uint64_t micros = nanos / Interval::NANOS_PER_MICRO;
		trunc_func(calendar, micros);
		return GetTimeUnsafe(calendar, micros);
	}

	template <typename T>
	static int64_t DifferenceFunc(Calendar *calendar, T start_date, T end_date, part_trunc_t trunc_func,
	                              part_sub_t sub_func) {
		// Truncate the two arguments. This is safe because we will stay in range
		auto start_micros = TruncateForDiff(calendar, start_date, trunc_func);
		auto end_micros = TruncateForDiff(calendar, end_date, trunc_func);

		// Now use ICU difference
		return sub_func(calendar, start_micros, end_micros);
	}

	static part_trunc_t DiffTruncationFactory(DatePartSpecifier type) {
		switch (type) {
		case DatePartSpecifier::WEEK:
			//	Weeks are computed without anchors
			return TruncationFactory(DatePartSpecifier::DAY);
		default:
			break;
		}
		return TruncationFactory(type);
	}

	template <typename T>
	static void ICUDateDiffFunction(DataChunk &args, ExpressionState &state, Vector &result) {
		D_ASSERT(args.ColumnCount() == 3);
		const auto &part_arg = args.data[0];
		const auto &startdate_arg = args.data[1];
		const auto &enddate_arg = args.data[2];

		auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
		auto &info = func_expr.BindInfo()->Cast<BindData>();
		CalendarPtr calendar_ptr(info.calendar->Copy());
		auto calendar = calendar_ptr.get();

		if (part_arg.GetVectorType() == VectorType::CONSTANT_VECTOR) {
			// Common case of constant part.
			if (ConstantVector::IsNull(part_arg)) {
				ConstantVector::SetNull(result, true);
			} else {
				const auto specifier = ConstantVector::GetData<string_t>(part_arg)->GetString();
				const auto part = GetDatePartSpecifier(specifier);
				auto trunc_func = DiffTruncationFactory(part);
				auto sub_func = SubtractFactory(part);
				BinaryExecutor::Execute<T, T, int64_t>(
				    startdate_arg, enddate_arg, result, [&](T start_date, T end_date) -> optional<int64_t> {
					    if (start_date.IsFinite() && end_date.IsFinite()) {
						    if (part == DatePartSpecifier::ERA) {
							    return DifferenceEra(calendar, start_date, end_date);
						    }
						    return DifferenceFunc(calendar, start_date, end_date, trunc_func, sub_func);
					    } else {
						    return nullopt;
					    }
				    });
			}
		} else {
			TernaryExecutor::Execute<string_t, T, T, int64_t>(
			    part_arg, startdate_arg, enddate_arg, result,
			    [&](string_t specifier, T start_date, T end_date) -> optional<int64_t> {
				    if (start_date.IsFinite() && end_date.IsFinite()) {
					    const auto part = GetDatePartSpecifier(specifier.GetString());
					    if (part == DatePartSpecifier::ERA) {
						    return DifferenceEra(calendar, start_date, end_date);
					    }
					    auto trunc_func = DiffTruncationFactory(part);
					    auto sub_func = SubtractFactory(part);
					    return DifferenceFunc(calendar, start_date, end_date, trunc_func, sub_func);
				    } else {
					    return nullopt;
				    }
			    });
		}
	}

	template <typename TA>
	static ScalarFunction GetFunction(const LogicalTypeId &type) {
		ScalarFunction fun({}, LogicalType::BIGINT, ICUDateDiffFunction<TA>, Bind);
		fun.GetSignature()
		    .AddParameter("part", LogicalType::VARCHAR)
		    .AddParameter("startdate", type)
		    .AddParameter("enddate", type);
		return fun;
	}

	static void AddFunctions(const Identifier &name, ExtensionLoader &loader) {
		ScalarFunctionSet set {name};
		set.AddFunction(GetFunction<timestamp_tz_t>(LogicalType::TIMESTAMP_TZ));
		set.AddFunction(GetFunction<timestamp_tz_ns_t>(LogicalType::TIMESTAMP_TZ_NS));
		// throws for unrecognized part specifiers and for dates that overflow the timestamp range
		set.SetFallible();
		set.SetStability(FunctionStability::CONSISTENT_WITHIN_QUERY);
		set.SetArgProperties(1, ArgProperties().NonIncreasing());
		set.SetArgProperties(2, ArgProperties().NonDecreasing());
		set.SetStatisticsCallback(StatisticsPropagator::PropagateFiniteMonotoneBounds);
		loader.RegisterFunction(set);
	}
};

void RegisterICUDateSubFunctions(ExtensionLoader &loader) {
	ICUCalendarSub::AddFunctions("date_sub", loader);
	ICUCalendarSub::AddFunctions("datesub", loader);

	ICUCalendarDiff::AddFunctions("date_diff", loader);
	ICUCalendarDiff::AddFunctions("datediff", loader);
}

} // namespace duckdb
