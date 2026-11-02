#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "core_functions/scalar/date_functions.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/enums/date_part_specifier.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/conversion_exception.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/types/date_lookup_cache.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/function/scalar/nested_functions.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/optimizer/statistics_propagator.hpp"

namespace duckdb {

namespace {
DatePartSpecifier GetDateTypePartSpecifier(const string &specifier, const LogicalType &type) {
	const auto part = GetDatePartSpecifier(specifier);
	switch (type.id()) {
	case LogicalType::TIMESTAMP:
	case LogicalType::TIMESTAMP_S:
	case LogicalType::TIMESTAMP_MS:
	case LogicalType::TIMESTAMP_NS:
	case LogicalType::TIMESTAMP_TZ:
	case LogicalType::DATE:
		return part;
	case LogicalType::TIME:
	case LogicalType::TIME_NS:
	case LogicalType::TIME_TZ:
		switch (part) {
		case DatePartSpecifier::MICROSECONDS:
		case DatePartSpecifier::MILLISECONDS:
		case DatePartSpecifier::SECOND:
		case DatePartSpecifier::MINUTE:
		case DatePartSpecifier::HOUR:
		case DatePartSpecifier::EPOCH:
		case DatePartSpecifier::TIMEZONE:
		case DatePartSpecifier::TIMEZONE_HOUR:
		case DatePartSpecifier::TIMEZONE_MINUTE:
			return part;
		default:
			break;
		}
		break;
	case LogicalType::INTERVAL:
		switch (part) {
		case DatePartSpecifier::YEAR:
		case DatePartSpecifier::MONTH:
		case DatePartSpecifier::DAY:
		case DatePartSpecifier::DECADE:
		case DatePartSpecifier::CENTURY:
		case DatePartSpecifier::QUARTER:
		case DatePartSpecifier::MILLENNIUM:
		case DatePartSpecifier::MICROSECONDS:
		case DatePartSpecifier::MILLISECONDS:
		case DatePartSpecifier::SECOND:
		case DatePartSpecifier::MINUTE:
		case DatePartSpecifier::HOUR:
		case DatePartSpecifier::EPOCH:
			return part;
		default:
			break;
		}
		break;
	default:
		break;
	}

	const auto type_name = StringUtil::Lower(EnumUtil::ToString(type.id()));
	throw NotImplementedException("\"%s\" units \"%s\" not recognized", type_name, specifier);
}

string DatePartUnaryFunctionName(DatePartSpecifier part) {
	switch (part) {
	case DatePartSpecifier::DOW:
		return DayOfWeekFun::Name;
	case DatePartSpecifier::DOY:
		return DayOfYearFun::Name;
	case DatePartSpecifier::JULIAN_DAY:
		return JulianDayFun::Name;
	case DatePartSpecifier::MILLISECONDS:
		return MillisecondsFun::Name;
	case DatePartSpecifier::MICROSECONDS:
		return MicrosecondsFun::Name;
	default:
		return EnumUtil::ToChars(part);
	}
}

template <int64_t MIN, int64_t MAX, class T>
unique_ptr<BaseStatistics> PropagateSimpleDatePartStatistics(vector<BaseStatistics> &child_stats) {
	// we can only propagate simple date part statistics if the child has stats
	auto &nstats = child_stats[0];
	if (!NumericStats::HasMinMax(nstats)) {
		return nullptr;
	}
	auto min = NumericStats::GetMin<T>(nstats);
	auto max = NumericStats::GetMax<T>(nstats);
	if (min > max) {
		return nullptr;
	}
	// Infinities produce a NULL date part even though the input is not NULL,
	// so we cannot propagate the validity (and thus the stats) in that case
	if (!Value::IsFinite(min) || !Value::IsFinite(max)) {
		return nullptr;
	}
	// the min and max can never exceed these bounds
	auto result = NumericStats::CreateEmpty(LogicalType::BIGINT);
	result.CopyValidity(child_stats[0]);
	NumericStats::SetMin(result, Value::BIGINT(MIN));
	NumericStats::SetMax(result, Value::BIGINT(MAX));
	return result.ToUnique();
}

template <class OP>
struct DateCacheLocalState : public FunctionLocalState {
	explicit DateCacheLocalState() {
	}

	DateLookupCache<OP> cache;
};

template <class OP>
unique_ptr<FunctionLocalState> InitDateCacheLocalState(ExpressionState &state, const BoundFunctionExpression &expr,
                                                       FunctionData *bind_data) {
	return make_uniq<DateCacheLocalState<OP>>();
}

struct DatePart {
	struct AsTime {
		template <typename SRC, typename DST>
		static DST Operation(SRC src) {
			if (SRC::PRECISION > DST::PRECISION) {
				const int64_t scaling = SRC::PRECISION / DST::PRECISION;
				return DST(src.value / scaling);
			} else {
				const int64_t scaling = DST::PRECISION / SRC::PRECISION;
				return DST(src.value * scaling);
			}
		}
	};

	template <class T, class OP, class TR = int64_t>
	static unique_ptr<BaseStatistics> PropagateDatePartStatistics(vector<BaseStatistics> &child_stats,
	                                                              const LogicalType &stats_type = LogicalType::BIGINT) {
		// we can only propagate complex date part stats if the child has stats
		auto &nstats = child_stats[0];
		if (!NumericStats::HasMinMax(nstats)) {
			return nullptr;
		}
		// run the operator on both the min and the max, this gives us the [min, max] bound
		auto min = NumericStats::GetMin<T>(nstats);
		auto max = NumericStats::GetMax<T>(nstats);
		if (min > max) {
			return nullptr;
		}
		// Infinities prevent us from computing generic ranges
		if (!min.IsFinite() || !max.IsFinite()) {
			return nullptr;
		}
		TR min_part = OP::template Operation<T, TR>(min);
		TR max_part = OP::template Operation<T, TR>(max);
		auto result = NumericStats::CreateEmpty(stats_type);
		NumericStats::SetMin(result, Value(min_part));
		NumericStats::SetMax(result, Value(max_part));
		result.CopyValidity(child_stats[0]);
		return result.ToUnique();
	}

	//! Some parts (e.g. month) are monotone as long as the input stays within a single parent period (e.g. year)
	//! If so, evaluating the part at the endpoints gives exact bounds - otherwise fall back to the fixed [MIN, MAX]
	template <int64_t MIN, int64_t MAX, class T, class OP, class PARENT_OP>
	static unique_ptr<BaseStatistics> PropagatePartWithinParentStatistics(vector<BaseStatistics> &child_stats) {
		auto &nstats = child_stats[0];
		if (!NumericStats::HasMinMax(nstats)) {
			return nullptr;
		}
		auto min = NumericStats::GetMin<T>(nstats);
		auto max = NumericStats::GetMax<T>(nstats);
		// an invalid range gives no bounds to propagate
		if (min > max) {
			return nullptr;
		}
		// Infinities produce a NULL date part even though the input is not NULL,
		// so we cannot propagate the validity (and thus the stats) in that case
		if (!Value::IsFinite(min) || !Value::IsFinite(max)) {
			return nullptr;
		}
		int64_t part_min = MIN;
		int64_t part_max = MAX;
		// within a single parent period the part cannot wrap around, so the endpoints are exact bounds
		if (PARENT_OP::template Operation<T, int64_t>(min) == PARENT_OP::template Operation<T, int64_t>(max)) {
			part_min = OP::template Operation<T, int64_t>(min);
			part_max = OP::template Operation<T, int64_t>(max);
		}
		auto result = NumericStats::CreateEmpty(LogicalType::BIGINT);
		result.CopyValidity(child_stats[0]);
		NumericStats::SetMin(result, Value::BIGINT(part_min));
		NumericStats::SetMax(result, Value::BIGINT(part_max));
		return result.ToUnique();
	}

	template <typename OP>
	struct PartOperator {
		template <class TA, class TR, class DATA_TYPE>
		static inline TR Operation(TA input, ValidityMask &mask, idx_t idx, DATA_TYPE &data) {
			if (input.IsFinite()) {
				return OP::template Operation<TA, TR>(input);
			} else {
				mask.SetInvalid(idx);
				return TR();
			}
		}
	};

	template <class TA, class TR, class OP>
	static void UnaryFunction(DataChunk &input, ExpressionState &state, Vector &result) {
		D_ASSERT(input.ColumnCount() >= 1);
		using IOP = PartOperator<OP>;
		std::nullptr_t no_data = nullptr;
		UnaryExecutor::GenericExecute<TA, TR, IOP>(input.data[0], result, no_data, true);
	}

	struct YearOperator {
		template <class TA, class TR>
		static inline TR Operation(TA input) {
			return Date::ExtractYear(input);
		}

		template <class T>
		static unique_ptr<BaseStatistics> PropagateStatistics(ClientContext &context, FunctionStatisticsInput &input) {
			return PropagateDatePartStatistics<T, YearOperator>(input.child_stats);
		}
	};

	struct MonthOperator {
		template <class TA, class TR>
		static inline TR Operation(TA input) {
			return Date::ExtractMonth(input);
		}

		template <class T>
		static unique_ptr<BaseStatistics> PropagateStatistics(ClientContext &context, FunctionStatisticsInput &input) {
			// min/max of month operator is [1, 12]
			return PropagatePartWithinParentStatistics<1, 12, T, MonthOperator, YearOperator>(input.child_stats);
		}
	};

	//! Identifies the calendar month containing the input, used as the parent period for day statistics
	struct YearMonthParentOperator {
		template <class TA, class TR>
		static inline TR Operation(TA input) {
			return YearOperator::Operation<TA, TR>(input) * 12 + MonthOperator::Operation<TA, TR>(input);
		}
	};

	struct DayOperator {
		template <class TA, class TR>
		static inline TR Operation(TA input) {
			return Date::ExtractDay(input);
		}

		template <class T>
		static unique_ptr<BaseStatistics> PropagateStatistics(ClientContext &context, FunctionStatisticsInput &input) {
			// min/max of day operator is [1, 31]
			return PropagatePartWithinParentStatistics<1, 31, T, DayOperator, YearMonthParentOperator>(
			    input.child_stats);
		}
	};

	struct DecadeOperator {
		// From the PG docs: "The year field divided by 10"
		template <typename TR>
		static inline TR DecadeFromYear(TR yyyy) {
			return yyyy / 10;
		}

		template <class TA, class TR>
		static inline TR Operation(TA input) {
			return DecadeFromYear(YearOperator::Operation<TA, TR>(input));
		}

		template <class T>
		static unique_ptr<BaseStatistics> PropagateStatistics(ClientContext &context, FunctionStatisticsInput &input) {
			return PropagateDatePartStatistics<T, DecadeOperator>(input.child_stats);
		}
	};

	struct CenturyOperator {
		// From the PG docs:
		// "The first century starts at 0001-01-01 00:00:00 AD, although they did not know it at the time.
		// This definition applies to all Gregorian calendar countries.
		// There is no century number 0, you go from -1 century to 1 century.
		// If you disagree with this, please write your complaint to: Pope, Cathedral Saint-Peter of Roma, Vatican."
		// (To be fair, His Holiness had nothing to do with this -
		// it was the lack of zero in the counting systems of the time...)
		template <typename TR>
		static inline TR CenturyFromYear(TR yyyy) {
			if (yyyy > 0) {
				return ((yyyy - 1) / 100) + 1;
			} else {
				return (yyyy / 100) - 1;
			}
		}

		template <class TA, class TR>
		static inline TR Operation(TA input) {
			return CenturyFromYear(YearOperator::Operation<TA, TR>(input));
		}

		template <class T>
		static unique_ptr<BaseStatistics> PropagateStatistics(ClientContext &context, FunctionStatisticsInput &input) {
			return PropagateDatePartStatistics<T, CenturyOperator>(input.child_stats);
		}
	};

	struct MillenniumOperator {
		// See the century comment
		template <typename TR>
		static inline TR MillenniumFromYear(TR yyyy) {
			if (yyyy > 0) {
				return ((yyyy - 1) / 1000) + 1;
			} else {
				return (yyyy / 1000) - 1;
			}
		}

		template <class TA, class TR>
		static inline TR Operation(TA input) {
			return MillenniumFromYear<TR>(YearOperator::Operation<TA, TR>(input));
		}

		template <class T>
		static unique_ptr<BaseStatistics> PropagateStatistics(ClientContext &context, FunctionStatisticsInput &input) {
			return PropagateDatePartStatistics<T, MillenniumOperator>(input.child_stats);
		}
	};

	struct QuarterOperator {
		template <class TR>
		static inline TR QuarterFromMonth(TR mm) {
			return (mm - 1) / Interval::MONTHS_PER_QUARTER + 1;
		}

		template <class TA, class TR>
		static inline TR Operation(TA input) {
			return QuarterFromMonth(Date::ExtractMonth(input));
		}

		template <class T>
		static unique_ptr<BaseStatistics> PropagateStatistics(ClientContext &context, FunctionStatisticsInput &input) {
			// min/max of quarter operator is [1, 4]
			return PropagatePartWithinParentStatistics<1, 4, T, QuarterOperator, YearOperator>(input.child_stats);
		}
	};

	struct DayOfWeekOperator {
		template <class TR>
		static inline TR DayOfWeekFromISO(TR isodow) {
			// day of the week (Sunday = 0, Saturday = 6)
			// turn sunday into 0 by doing mod 7
			return isodow % 7;
		}

		template <class TA, class TR>
		static inline TR Operation(TA input) {
			return DayOfWeekFromISO(Date::ExtractISODayOfTheWeek(input));
		}

		template <class T>
		static unique_ptr<BaseStatistics> PropagateStatistics(ClientContext &context, FunctionStatisticsInput &input) {
			return PropagateSimpleDatePartStatistics<0, 6, T>(input.child_stats);
		}
	};

	struct ISODayOfWeekOperator {
		template <class TA, class TR>
		static inline TR Operation(TA input) {
			// isodow (Monday = 1, Sunday = 7)
			return Date::ExtractISODayOfTheWeek(input);
		}

		template <class T>
		static unique_ptr<BaseStatistics> PropagateStatistics(ClientContext &context, FunctionStatisticsInput &input) {
			return PropagateSimpleDatePartStatistics<1, 7, T>(input.child_stats);
		}
	};

	struct DayOfYearOperator {
		template <class TA, class TR>
		static inline TR Operation(TA input) {
			return Date::ExtractDayOfTheYear(input);
		}

		template <class T>
		static unique_ptr<BaseStatistics> PropagateStatistics(ClientContext &context, FunctionStatisticsInput &input) {
			return PropagatePartWithinParentStatistics<1, 366, T, DayOfYearOperator, YearOperator>(input.child_stats);
		}
	};

	struct WeekOperator {
		template <class TA, class TR>
		static inline TR Operation(TA input) {
			return Date::ExtractISOWeekNumber(input);
		}

		template <class T>
		static unique_ptr<BaseStatistics> PropagateStatistics(ClientContext &context, FunctionStatisticsInput &input) {
			return PropagatePartWithinParentStatistics<1, 53, T, WeekOperator, ISOYearOperator>(input.child_stats);
		}
	};

	struct ISOYearOperator {
		template <class TA, class TR>
		static inline TR Operation(TA input) {
			return Date::ExtractISOYearNumber(input);
		}

		template <class T>
		static unique_ptr<BaseStatistics> PropagateStatistics(ClientContext &context, FunctionStatisticsInput &input) {
			return PropagateDatePartStatistics<T, ISOYearOperator>(input.child_stats);
		}
	};

	struct YearWeekOperator {
		template <class TR>
		static inline TR YearWeekFromParts(TR yyyy, TR ww) {
			return yyyy * 100 + ((yyyy > 0) ? ww : -ww);
		}

		template <class TA, class TR>
		static inline TR Operation(TA input) {
			int32_t yyyy, ww;
			Date::ExtractISOYearWeek(input, yyyy, ww);
			return YearWeekFromParts(yyyy, ww);
		}

		// ISO years >= 1 use yyyy * 100 + week. Year 0 and negative years subtract the week,
		// so the function decreases as the week number increases.
		static int32_t MinISOYear(date_t input) {
			return Date::ExtractISOYearNumber(input);
		}
		static int32_t MinISOYear(timestamp_t input) {
			return Date::ExtractISOYearNumber(Timestamp::GetDate(input));
		}
		template <class U>
		static int32_t MinISOYear(U) {
			return 0;
		}

		template <class T>
		static unique_ptr<BaseStatistics> PropagateStatistics(ClientContext &context, FunctionStatisticsInput &input) {
			// Endpoint bounds do not contain the values in between once the range reaches the
			// subtracted-week encoding. Positive ISO years are non-decreasing in the date.
			auto &nstats = input.child_stats[0];
			if (!NumericStats::HasMinMax(nstats)) {
				return nullptr;
			}
			auto min = NumericStats::GetMin<T>(nstats);
			auto max = NumericStats::GetMax<T>(nstats);
			if (min > max || !min.IsFinite() || !max.IsFinite() || MinISOYear(min) < 1) {
				return nullptr;
			}
			return PropagateDatePartStatistics<T, YearWeekOperator>(input.child_stats);
		}
	};

	struct EpochNanosecondsOperator {
		template <class TA, class TR>
		static inline TR Operation(TA input) {
			return Timestamp::GetEpochNanoSeconds(input);
		}

		template <class T>
		static unique_ptr<BaseStatistics> PropagateStatistics(ClientContext &context, FunctionStatisticsInput &input) {
			return PropagateDatePartStatistics<T, EpochNanosecondsOperator>(input.child_stats);
		}
	};

	struct EpochMicrosecondsOperator {
		template <class TA, class TR>
		static inline TR Operation(TA input) {
			return Timestamp::GetEpochMicroSeconds(input);
		}

		template <class T>
		static unique_ptr<BaseStatistics> PropagateStatistics(ClientContext &context, FunctionStatisticsInput &input) {
			return PropagateDatePartStatistics<T, EpochMicrosecondsOperator>(input.child_stats);
		}
	};

	struct EpochMillisOperator {
		template <class TA, class TR>
		static inline TR Operation(TA input) {
			return Cast::Operation<TA, TR>(input);
		}

		template <class T>
		static unique_ptr<BaseStatistics> PropagateStatistics(ClientContext &context, FunctionStatisticsInput &input) {
			return PropagateDatePartStatistics<T, EpochMillisOperator>(input.child_stats);
		}

		static void Inverse(DataChunk &input, ExpressionState &state, Vector &result) {
			D_ASSERT(input.ColumnCount() == 1);

			UnaryExecutor::Execute<int64_t, timestamp_t>(input.data[0], result, [&](int64_t input) {
				// millisecond amounts provided to epoch_ms should never be considered infinite
				// instead such values will just throw when converted to microseconds
				return Timestamp::FromEpochMsPossiblyInfinite(input);
			});
		}
	};

	struct NanosecondsOperator {
		template <class TA, class TR>
		static inline TR Operation(TA input) {
			return MicrosecondsOperator::Operation<TA, TR>(input) * Interval::NANOS_PER_MICRO;
		}

		template <class T>
		static unique_ptr<BaseStatistics> PropagateStatistics(ClientContext &context, FunctionStatisticsInput &input) {
			return PropagatePartWithinParentStatistics<0, 59999999999, T, NanosecondsOperator,
			                                           DayHourMinuteParentOperator>(input.child_stats);
		}
	};

	struct MicrosecondsOperator {
		template <class TA, class TR>
		static inline TR Operation(TA input) {
			return 0;
		}

		template <class T>
		static unique_ptr<BaseStatistics> PropagateStatistics(ClientContext &context, FunctionStatisticsInput &input) {
			return PropagatePartWithinParentStatistics<0, 59999999, T, MicrosecondsOperator,
			                                           DayHourMinuteParentOperator>(input.child_stats);
		}
	};

	struct MillisecondsOperator {
		template <class TA, class TR>
		static inline TR Operation(TA input) {
			return 0;
		}

		template <class T>
		static unique_ptr<BaseStatistics> PropagateStatistics(ClientContext &context, FunctionStatisticsInput &input) {
			return PropagatePartWithinParentStatistics<0, 59999, T, MillisecondsOperator, DayHourMinuteParentOperator>(
			    input.child_stats);
		}
	};

	struct SecondsOperator {
		template <class TA, class TR>
		static inline TR Operation(TA input) {
			return 0;
		}

		template <class T>
		static unique_ptr<BaseStatistics> PropagateStatistics(ClientContext &context, FunctionStatisticsInput &input) {
			return PropagatePartWithinParentStatistics<0, 59, T, SecondsOperator, DayHourMinuteParentOperator>(
			    input.child_stats);
		}
	};

	struct MinutesOperator {
		template <class TA, class TR>
		static inline TR Operation(TA input) {
			return 0;
		}

		template <class T>
		static unique_ptr<BaseStatistics> PropagateStatistics(ClientContext &context, FunctionStatisticsInput &input) {
			return PropagatePartWithinParentStatistics<0, 59, T, MinutesOperator, DayHourParentOperator>(
			    input.child_stats);
		}
	};

	struct HoursOperator {
		template <class TA, class TR>
		static inline TR Operation(TA input) {
			return 0;
		}

		template <class T>
		static unique_ptr<BaseStatistics> PropagateStatistics(ClientContext &context, FunctionStatisticsInput &input) {
			return PropagatePartWithinParentStatistics<0, 24, T, HoursOperator, DayParentOperator>(input.child_stats);
		}
	};

	//! Identifies the day containing the input, used as the parent period for hour statistics
	struct DayParentOperator {
		template <class TA, class TR>
		static inline TR Operation(TA input);
	};

	//! Identifies the hour containing the input, used as the parent period for minute statistics
	struct DayHourParentOperator {
		template <class TA, class TR>
		static inline TR Operation(TA input) {
			return DayParentOperator::Operation<TA, TR>(input) * 24 + HoursOperator::Operation<TA, TR>(input);
		}
	};

	//! Identifies the minute containing the input, used as the parent period for second statistics
	struct DayHourMinuteParentOperator {
		template <class TA, class TR>
		static inline TR Operation(TA input) {
			return DayHourParentOperator::Operation<TA, TR>(input) * 60 + MinutesOperator::Operation<TA, TR>(input);
		}
	};

	struct EpochOperator {
		template <class TA, class TR>
		static inline TR Operation(TA input) {
			return TR(Date::Epoch(input));
		}

		template <class T>
		static unique_ptr<BaseStatistics> PropagateStatistics(ClientContext &context, FunctionStatisticsInput &input) {
			return PropagateDatePartStatistics<T, EpochOperator, double>(input.child_stats, LogicalType::DOUBLE);
		}
	};

	struct EraOperator {
		template <class TR>
		static inline TR EraFromYear(TR yyyy) {
			return yyyy > 0 ? 1 : 0;
		}

		template <class TA, class TR>
		static inline TR Operation(TA input) {
			return EraFromYear(Date::ExtractYear(input));
		}

		template <class T>
		static unique_ptr<BaseStatistics> PropagateStatistics(ClientContext &context, FunctionStatisticsInput &input) {
			return PropagateSimpleDatePartStatistics<0, 1, T>(input.child_stats);
		}
	};

	struct TimezoneOperator {
		template <class TA, class TR>
		static inline TR Operation(TA input) {
			// Regular timestamps are UTC.
			return 0;
		}

		template <typename TA, typename TB, typename TR>
		static TR Operation(TA interval, TB timetz) {
			auto offset = interval.micros / Interval::MICROS_PER_SEC;
			if (offset < dtime_tz_t::MIN_OFFSET || offset > dtime_tz_t::MAX_OFFSET) {
				throw OutOfRangeException("Timezone offset out of range: %s", Interval::ToString(interval));
			}
			auto time = Time::NormalizeTimeTZ(timetz);
			date_t date(0);
			time = Interval::Add(time, interval, date);
			return TR(time, UnsafeNumericCast<int32_t>(offset));
		}

		template <typename TA, typename TB, typename TR>
		static void BinaryFunction(DataChunk &input, ExpressionState &state, Vector &result) {
			D_ASSERT(input.ColumnCount() == 2);
			const auto &offset = input.data[0];
			const auto &timetz = input.data[1];

			auto func = DatePart::TimezoneOperator::Operation<TA, TB, TR>;
			BinaryExecutor::Execute<TA, TB, TR>(offset, timetz, result, func);
		}

		template <class T>
		static unique_ptr<BaseStatistics> PropagateStatistics(ClientContext &context, FunctionStatisticsInput &input) {
			return PropagateSimpleDatePartStatistics<0, 0, T>(input.child_stats);
		}
	};

	struct TimezoneHourOperator {
		template <class TA, class TR>
		static inline TR Operation(TA input) {
			// Regular timestamps are UTC.
			return 0;
		}

		template <class T>
		static unique_ptr<BaseStatistics> PropagateStatistics(ClientContext &context, FunctionStatisticsInput &input) {
			return PropagateSimpleDatePartStatistics<0, 0, T>(input.child_stats);
		}
	};

	struct TimezoneMinuteOperator {
		template <class TA, class TR>
		static inline TR Operation(TA input) {
			// Regular timestamps are UTC.
			return 0;
		}

		template <class T>
		static unique_ptr<BaseStatistics> PropagateStatistics(ClientContext &context, FunctionStatisticsInput &input) {
			return PropagateSimpleDatePartStatistics<0, 0, T>(input.child_stats);
		}
	};

	struct JulianDayOperator {
		template <class TA, class TR>
		static inline TR Operation(TA input) {
			return Timestamp::GetJulianDay(input);
		}

		template <class T>
		static unique_ptr<BaseStatistics> PropagateStatistics(ClientContext &context, FunctionStatisticsInput &input) {
			return PropagateDatePartStatistics<T, JulianDayOperator, double>(input.child_stats, LogicalType::DOUBLE);
		}
	};

	struct StructOperator {
		using part_codes_t = vector<DatePartSpecifier>;
		using part_mask_t = uint64_t;

		enum MaskBits : uint8_t {
			YMD = 1 << 0,
			DOW = 1 << 1,
			DOY = 1 << 2,
			EPOCH = 1 << 3,
			TIME = 1 << 4,
			ZONE = 1 << 5,
			ISO = 1 << 6,
			JD = 1 << 7
		};

		static part_mask_t GetMask(const part_codes_t &part_codes) {
			part_mask_t mask = 0;
			for (const auto &part_code : part_codes) {
				switch (part_code) {
				case DatePartSpecifier::YEAR:
				case DatePartSpecifier::MONTH:
				case DatePartSpecifier::DAY:
				case DatePartSpecifier::DECADE:
				case DatePartSpecifier::CENTURY:
				case DatePartSpecifier::MILLENNIUM:
				case DatePartSpecifier::QUARTER:
				case DatePartSpecifier::ERA:
					mask |= YMD;
					break;
				case DatePartSpecifier::YEARWEEK:
				case DatePartSpecifier::WEEK:
				case DatePartSpecifier::ISOYEAR:
					mask |= ISO;
					break;
				case DatePartSpecifier::DOW:
				case DatePartSpecifier::ISODOW:
					mask |= DOW;
					break;
				case DatePartSpecifier::DOY:
					mask |= DOY;
					break;
				case DatePartSpecifier::EPOCH:
					mask |= EPOCH;
					break;
				case DatePartSpecifier::JULIAN_DAY:
					mask |= JD;
					break;
				case DatePartSpecifier::MICROSECONDS:
				case DatePartSpecifier::MILLISECONDS:
				case DatePartSpecifier::SECOND:
				case DatePartSpecifier::MINUTE:
				case DatePartSpecifier::HOUR:
					mask |= TIME;
					break;
				case DatePartSpecifier::TIMEZONE:
				case DatePartSpecifier::TIMEZONE_HOUR:
				case DatePartSpecifier::TIMEZONE_MINUTE:
					mask |= ZONE;
					break;
				case DatePartSpecifier::INVALID:
					throw InternalException("Invalid DatePartSpecifier for STRUCT mask!");
				}
			}
			return mask;
		}

		template <typename P>
		static inline P HasPartValue(vector<P> part_values, DatePartSpecifier part) {
			auto idx = size_t(part);
			if (IsBigintDatepart(part)) {
				return part_values[idx - size_t(DatePartSpecifier::BEGIN_BIGINT)];
			} else {
				return part_values[idx - size_t(DatePartSpecifier::BEGIN_DOUBLE)];
			}
		}

		using bigint_vec = vector<int64_t *>;
		using double_vec = vector<double *>;

		template <class TA>
		static inline void Operation(bigint_vec &bigint_values, double_vec &double_values, const TA &input,
		                             const idx_t idx, const part_mask_t mask) {
			int64_t *bigint_data;
			// YMD calculations
			int32_t yyyy = 1970;
			int32_t mm = 0;
			int32_t dd = 1;
			if (mask & YMD) {
				Date::Convert(input, yyyy, mm, dd);
				bigint_data = HasPartValue(bigint_values, DatePartSpecifier::YEAR);
				if (bigint_data) {
					bigint_data[idx] = yyyy;
				}
				bigint_data = HasPartValue(bigint_values, DatePartSpecifier::MONTH);
				if (bigint_data) {
					bigint_data[idx] = mm;
				}
				bigint_data = HasPartValue(bigint_values, DatePartSpecifier::DAY);
				if (bigint_data) {
					bigint_data[idx] = dd;
				}
				bigint_data = HasPartValue(bigint_values, DatePartSpecifier::DECADE);
				if (bigint_data) {
					bigint_data[idx] = DecadeOperator::DecadeFromYear(yyyy);
				}
				bigint_data = HasPartValue(bigint_values, DatePartSpecifier::CENTURY);
				if (bigint_data) {
					bigint_data[idx] = CenturyOperator::CenturyFromYear(yyyy);
				}
				bigint_data = HasPartValue(bigint_values, DatePartSpecifier::MILLENNIUM);
				if (bigint_data) {
					bigint_data[idx] = MillenniumOperator::MillenniumFromYear(yyyy);
				}
				bigint_data = HasPartValue(bigint_values, DatePartSpecifier::QUARTER);
				if (bigint_data) {
					bigint_data[idx] = QuarterOperator::QuarterFromMonth(mm);
				}
				bigint_data = HasPartValue(bigint_values, DatePartSpecifier::ERA);
				if (bigint_data) {
					bigint_data[idx] = EraOperator::EraFromYear(yyyy);
				}
			}

			// Week calculations
			if (mask & DOW) {
				auto isodow = Date::ExtractISODayOfTheWeek(input);
				bigint_data = HasPartValue(bigint_values, DatePartSpecifier::DOW);
				if (bigint_data) {
					bigint_data[idx] = DayOfWeekOperator::DayOfWeekFromISO(isodow);
				}
				bigint_data = HasPartValue(bigint_values, DatePartSpecifier::ISODOW);
				if (bigint_data) {
					bigint_data[idx] = isodow;
				}
			}

			// ISO calculations
			if (mask & ISO) {
				int32_t ww = 0;
				int32_t iyyy = 0;
				Date::ExtractISOYearWeek(input, iyyy, ww);
				bigint_data = HasPartValue(bigint_values, DatePartSpecifier::WEEK);
				if (bigint_data) {
					bigint_data[idx] = ww;
				}
				bigint_data = HasPartValue(bigint_values, DatePartSpecifier::ISOYEAR);
				if (bigint_data) {
					bigint_data[idx] = iyyy;
				}
				bigint_data = HasPartValue(bigint_values, DatePartSpecifier::YEARWEEK);
				if (bigint_data) {
					bigint_data[idx] = YearWeekOperator::YearWeekFromParts(iyyy, ww);
				}
			}

			if (mask & EPOCH) {
				auto double_data = HasPartValue(double_values, DatePartSpecifier::EPOCH);
				if (double_data) {
					double_data[idx] = double(Date::Epoch(input));
				}
			}
			if (mask & DOY) {
				bigint_data = HasPartValue(bigint_values, DatePartSpecifier::DOY);
				if (bigint_data) {
					bigint_data[idx] = Date::ExtractDayOfTheYear(input);
				}
			}
			if (mask & JD) {
				auto double_data = HasPartValue(double_values, DatePartSpecifier::JULIAN_DAY);
				if (double_data) {
					double_data[idx] = double(Date::ExtractJulianDay(input));
				}
			}

			const part_mask_t time_mask = (mask & TIME) | (mask & ZONE);
			if (time_mask) {
				dtime_t zero(0);
				Operation(bigint_values, double_values, zero, idx, time_mask);
			}
		}
	};
};

template <class OP, class T>
void DatePartCachedFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &lstate = ExecuteFunctionState::GetFunctionState(state)->Cast<DateCacheLocalState<OP>>();
	UnaryExecutor::Execute<T, int64_t>(args.data[0], result,
	                                   [&](T input) { return lstate.cache.ExtractElement(input); });
}

template <>
int64_t DatePart::YearOperator::Operation(timestamp_t input) {
	return YearOperator::Operation<date_t, int64_t>(Timestamp::GetDate(input));
}

template <>
int64_t DatePart::YearOperator::Operation(interval_t input) {
	return input.months / Interval::MONTHS_PER_YEAR;
}

template <>
int64_t DatePart::YearOperator::Operation(dtime_t input) {
	throw NotImplementedException("\"time\" units \"year\" not recognized");
}

template <>
int64_t DatePart::YearOperator::Operation(dtime_ns_t input) {
	return YearOperator::Operation<dtime_t, int64_t>(AsTime::Operation<dtime_ns_t, dtime_t>(input));
}

template <>
int64_t DatePart::YearOperator::Operation(dtime_tz_t input) {
	return YearOperator::Operation<dtime_t, int64_t>(input.time());
}

template <>
int64_t DatePart::MonthOperator::Operation(timestamp_t input) {
	return MonthOperator::Operation<date_t, int64_t>(Timestamp::GetDate(input));
}

template <>
int64_t DatePart::MonthOperator::Operation(interval_t input) {
	return input.months % Interval::MONTHS_PER_YEAR;
}

template <>
int64_t DatePart::MonthOperator::Operation(dtime_t input) {
	throw NotImplementedException("\"time\" units \"month\" not recognized");
}

template <>
int64_t DatePart::MonthOperator::Operation(dtime_ns_t input) {
	return MonthOperator::Operation<dtime_t, int64_t>(AsTime::Operation<dtime_ns_t, dtime_t>(input));
}

template <>
int64_t DatePart::MonthOperator::Operation(dtime_tz_t input) {
	return MonthOperator::Operation<dtime_t, int64_t>(input.time());
}

template <>
int64_t DatePart::DayOperator::Operation(timestamp_t input) {
	return DayOperator::Operation<date_t, int64_t>(Timestamp::GetDate(input));
}

template <>
int64_t DatePart::DayOperator::Operation(interval_t input) {
	return input.days;
}

template <>
int64_t DatePart::DayOperator::Operation(dtime_t input) {
	throw NotImplementedException("\"time\" units \"day\" not recognized");
}

template <>
int64_t DatePart::DayOperator::Operation(dtime_ns_t input) {
	return DayOperator::Operation<dtime_t, int64_t>(AsTime::Operation<dtime_ns_t, dtime_t>(input));
}

template <>
int64_t DatePart::DayOperator::Operation(dtime_tz_t input) {
	return DayOperator::Operation<dtime_t, int64_t>(input.time());
}

template <>
int64_t DatePart::DecadeOperator::Operation(interval_t input) {
	return input.months / Interval::MONTHS_PER_DECADE;
}

template <>
int64_t DatePart::DecadeOperator::Operation(dtime_t input) {
	throw NotImplementedException("\"time\" units \"decade\" not recognized");
}

template <>
int64_t DatePart::DecadeOperator::Operation(dtime_tz_t input) {
	return DecadeOperator::Operation<dtime_t, int64_t>(input.time());
}

template <>
int64_t DatePart::CenturyOperator::Operation(interval_t input) {
	return input.months / Interval::MONTHS_PER_CENTURY;
}

template <>
int64_t DatePart::CenturyOperator::Operation(dtime_t input) {
	throw NotImplementedException("\"time\" units \"century\" not recognized");
}

template <>
int64_t DatePart::CenturyOperator::Operation(dtime_tz_t input) {
	return CenturyOperator::Operation<dtime_t, int64_t>(input.time());
}

template <>
int64_t DatePart::MillenniumOperator::Operation(interval_t input) {
	return input.months / Interval::MONTHS_PER_MILLENIUM;
}

template <>
int64_t DatePart::MillenniumOperator::Operation(dtime_t input) {
	throw NotImplementedException("\"time\" units \"millennium\" not recognized");
}

template <>
int64_t DatePart::MillenniumOperator::Operation(dtime_tz_t input) {
	return MillenniumOperator::Operation<dtime_t, int64_t>(input.time());
}

template <>
int64_t DatePart::QuarterOperator::Operation(timestamp_t input) {
	return QuarterOperator::Operation<date_t, int64_t>(Timestamp::GetDate(input));
}

template <>
int64_t DatePart::QuarterOperator::Operation(interval_t input) {
	return MonthOperator::Operation<interval_t, int64_t>(input) / Interval::MONTHS_PER_QUARTER + 1;
}

template <>
int64_t DatePart::QuarterOperator::Operation(dtime_t input) {
	throw NotImplementedException("\"time\" units \"quarter\" not recognized");
}

template <>
int64_t DatePart::QuarterOperator::Operation(dtime_ns_t input) {
	return QuarterOperator::Operation<dtime_t, int64_t>(AsTime::Operation<dtime_ns_t, dtime_t>(input));
}

template <>
int64_t DatePart::QuarterOperator::Operation(dtime_tz_t input) {
	return QuarterOperator::Operation<dtime_t, int64_t>(input.time());
}

template <>
int64_t DatePart::DayOfWeekOperator::Operation(timestamp_t input) {
	return DayOfWeekOperator::Operation<date_t, int64_t>(Timestamp::GetDate(input));
}

template <>
int64_t DatePart::DayOfWeekOperator::Operation(interval_t input) {
	throw NotImplementedException("interval units \"dow\" not recognized");
}

template <>
int64_t DatePart::DayOfWeekOperator::Operation(dtime_t input) {
	throw NotImplementedException("\"time\" units \"dow\" not recognized");
}

template <>
int64_t DatePart::DayOfWeekOperator::Operation(dtime_ns_t input) {
	return DayOfWeekOperator::Operation<dtime_t, int64_t>(AsTime::Operation<dtime_ns_t, dtime_t>(input));
}

template <>
int64_t DatePart::DayOfWeekOperator::Operation(dtime_tz_t input) {
	return DayOfWeekOperator::Operation<dtime_t, int64_t>(input.time());
}

template <>
int64_t DatePart::ISODayOfWeekOperator::Operation(timestamp_t input) {
	return ISODayOfWeekOperator::Operation<date_t, int64_t>(Timestamp::GetDate(input));
}

template <>
int64_t DatePart::ISODayOfWeekOperator::Operation(interval_t input) {
	throw NotImplementedException("interval units \"isodow\" not recognized");
}

template <>
int64_t DatePart::ISODayOfWeekOperator::Operation(dtime_t input) {
	throw NotImplementedException("\"time\" units \"isodow\" not recognized");
}

template <>
int64_t DatePart::ISODayOfWeekOperator::Operation(dtime_ns_t input) {
	return ISODayOfWeekOperator::Operation<dtime_t, int64_t>(AsTime::Operation<dtime_ns_t, dtime_t>(input));
}

template <>
int64_t DatePart::ISODayOfWeekOperator::Operation(dtime_tz_t input) {
	return ISODayOfWeekOperator::Operation<dtime_t, int64_t>(input.time());
}

template <>
int64_t DatePart::DayOfYearOperator::Operation(timestamp_t input) {
	return DayOfYearOperator::Operation<date_t, int64_t>(Timestamp::GetDate(input));
}

template <>
int64_t DatePart::DayOfYearOperator::Operation(interval_t input) {
	throw NotImplementedException("interval units \"doy\" not recognized");
}

template <>
int64_t DatePart::DayOfYearOperator::Operation(dtime_t input) {
	throw NotImplementedException("\"time\" units \"doy\" not recognized");
}

template <>
int64_t DatePart::DayOfYearOperator::Operation(dtime_ns_t input) {
	return DayOfYearOperator::Operation<dtime_t, int64_t>(AsTime::Operation<dtime_ns_t, dtime_t>(input));
}

template <>
int64_t DatePart::DayOfYearOperator::Operation(dtime_tz_t input) {
	return DayOfYearOperator::Operation<dtime_t, int64_t>(input.time());
}

template <>
int64_t DatePart::WeekOperator::Operation(timestamp_t input) {
	return WeekOperator::Operation<date_t, int64_t>(Timestamp::GetDate(input));
}

template <>
int64_t DatePart::WeekOperator::Operation(interval_t input) {
	throw NotImplementedException("interval units \"week\" not recognized");
}

template <>
int64_t DatePart::WeekOperator::Operation(dtime_t input) {
	throw NotImplementedException("\"time\" units \"week\" not recognized");
}

template <>
int64_t DatePart::WeekOperator::Operation(dtime_ns_t input) {
	return WeekOperator::Operation<dtime_t, int64_t>(AsTime::Operation<dtime_ns_t, dtime_t>(input));
}

template <>
int64_t DatePart::WeekOperator::Operation(dtime_tz_t input) {
	return WeekOperator::Operation<dtime_t, int64_t>(input.time());
}

template <>
int64_t DatePart::ISOYearOperator::Operation(timestamp_t input) {
	return ISOYearOperator::Operation<date_t, int64_t>(Timestamp::GetDate(input));
}

template <>
int64_t DatePart::ISOYearOperator::Operation(interval_t input) {
	throw NotImplementedException("interval units \"isoyear\" not recognized");
}

template <>
int64_t DatePart::ISOYearOperator::Operation(dtime_t input) {
	throw NotImplementedException("\"time\" units \"isoyear\" not recognized");
}

template <>
int64_t DatePart::ISOYearOperator::Operation(dtime_ns_t input) {
	return ISOYearOperator::Operation<dtime_t, int64_t>(AsTime::Operation<dtime_ns_t, dtime_t>(input));
}

template <>
int64_t DatePart::ISOYearOperator::Operation(dtime_tz_t input) {
	return ISOYearOperator::Operation<dtime_t, int64_t>(input.time());
}

template <>
int64_t DatePart::YearWeekOperator::Operation(timestamp_t input) {
	return YearWeekOperator::Operation<date_t, int64_t>(Timestamp::GetDate(input));
}

template <>
int64_t DatePart::YearWeekOperator::Operation(interval_t input) {
	const auto yyyy = YearOperator::Operation<interval_t, int64_t>(input);
	const auto ww = WeekOperator::Operation<interval_t, int64_t>(input);
	return YearWeekOperator::YearWeekFromParts<int64_t>(yyyy, ww);
}

template <>
int64_t DatePart::YearWeekOperator::Operation(dtime_t input) {
	throw NotImplementedException("\"time\" units \"yearweek\" not recognized");
}

template <>
int64_t DatePart::YearWeekOperator::Operation(dtime_ns_t input) {
	return YearWeekOperator::Operation<dtime_t, int64_t>(AsTime::Operation<dtime_ns_t, dtime_t>(input));
}

template <>
int64_t DatePart::YearWeekOperator::Operation(dtime_tz_t input) {
	return YearWeekOperator::Operation<dtime_t, int64_t>(input.time());
}

template <>
int64_t DatePart::EpochNanosecondsOperator::Operation(timestamp_t input) {
	D_ASSERT(input.IsFinite());
	return Timestamp::GetEpochNanoSeconds(input);
}

template <>
int64_t DatePart::EpochNanosecondsOperator::Operation(date_t input) {
	D_ASSERT(input.IsFinite());
	return Date::EpochNanoseconds(input);
}

template <>
int64_t DatePart::EpochNanosecondsOperator::Operation(interval_t input) {
	return Interval::GetNanoseconds(input);
}

template <>
int64_t DatePart::EpochNanosecondsOperator::Operation(dtime_t input) {
	return input.value * Interval::NANOS_PER_MICRO;
}

template <>
int64_t DatePart::EpochNanosecondsOperator::Operation(dtime_ns_t input) {
	return input.value;
}

template <>
int64_t DatePart::EpochNanosecondsOperator::Operation(dtime_tz_t input) {
	return DatePart::EpochNanosecondsOperator::Operation<dtime_t, int64_t>(input.time());
}

template <>
int64_t DatePart::EpochMicrosecondsOperator::Operation(date_t input) {
	return Date::EpochMicroseconds(input);
}

template <>
int64_t DatePart::EpochMicrosecondsOperator::Operation(interval_t input) {
	return Interval::GetMicro(input);
}

template <>
int64_t DatePart::EpochMillisOperator::Operation(timestamp_t input) {
	D_ASSERT(input.IsFinite());
	return Cast::Operation<timestamp_t, timestamp_ms_t>(input).value;
}

template <>
int64_t DatePart::EpochMicrosecondsOperator::Operation(dtime_t input) {
	return input.value;
}

template <>
int64_t DatePart::EpochMicrosecondsOperator::Operation(dtime_ns_t input) {
	return DatePart::EpochMicrosecondsOperator::Operation<dtime_t, int64_t>(
	    AsTime::Operation<dtime_ns_t, dtime_t>(input));
}

template <>
int64_t DatePart::EpochMicrosecondsOperator::Operation(dtime_tz_t input) {
	return DatePart::EpochMicrosecondsOperator::Operation<dtime_t, int64_t>(input.time());
}

template <>
int64_t DatePart::EpochMillisOperator::Operation(date_t input) {
	return Date::EpochMilliseconds(input);
}

template <>
int64_t DatePart::EpochMillisOperator::Operation(interval_t input) {
	return Interval::GetMilli(input);
}

template <>
int64_t DatePart::EpochMillisOperator::Operation(dtime_t input) {
	return input.value / Interval::MICROS_PER_MSEC;
}

template <>
int64_t DatePart::EpochMillisOperator::Operation(dtime_tz_t input) {
	return DatePart::EpochMillisOperator::Operation<dtime_t, int64_t>(input.time());
}

template <>
int64_t DatePart::NanosecondsOperator::Operation(timestamp_ns_t input) {
	if (!input.IsFinite()) {
		throw ConversionException("Can't get nanoseconds of infinite TIMESTAMP");
	}
	date_t date;
	dtime_t time;
	int32_t nanos;
	Timestamp::Convert(input, date, time, nanos);
	// remove everything but the second & nanosecond part
	return (time.value % Interval::MICROS_PER_MINUTE) * Interval::NANOS_PER_MICRO + nanos;
}

template <>
int64_t DatePart::NanosecondsOperator::Operation(dtime_ns_t input) {
	return input.value % Interval::NANOS_PER_MINUTE;
}

template <>
int64_t DatePart::MicrosecondsOperator::Operation(timestamp_t input) {
	D_ASSERT(input.IsFinite());
	auto time = Timestamp::GetTime(input);
	// remove everything but the second & microsecond part
	return time.value % Interval::MICROS_PER_MINUTE;
}

template <>
int64_t DatePart::MicrosecondsOperator::Operation(interval_t input) {
	// remove everything but the second & microsecond part
	return input.micros % Interval::MICROS_PER_MINUTE;
}

template <>
int64_t DatePart::MicrosecondsOperator::Operation(dtime_t input) {
	// remove everything but the second & microsecond part
	return input.value % Interval::MICROS_PER_MINUTE;
}

template <>
int64_t DatePart::MicrosecondsOperator::Operation(dtime_ns_t input) {
	return DatePart::MicrosecondsOperator::Operation<dtime_t, int64_t>(AsTime::Operation<dtime_ns_t, dtime_t>(input));
}

template <>
int64_t DatePart::MicrosecondsOperator::Operation(dtime_tz_t input) {
	return DatePart::MicrosecondsOperator::Operation<dtime_t, int64_t>(input.time());
}

template <>
int64_t DatePart::MillisecondsOperator::Operation(timestamp_t input) {
	D_ASSERT(input.IsFinite());
	return MicrosecondsOperator::Operation<timestamp_t, int64_t>(input) / Interval::MICROS_PER_MSEC;
}

template <>
int64_t DatePart::MillisecondsOperator::Operation(interval_t input) {
	return MicrosecondsOperator::Operation<interval_t, int64_t>(input) / Interval::MICROS_PER_MSEC;
}

template <>
int64_t DatePart::MillisecondsOperator::Operation(dtime_t input) {
	return MicrosecondsOperator::Operation<dtime_t, int64_t>(input) / Interval::MICROS_PER_MSEC;
}

template <>
int64_t DatePart::MillisecondsOperator::Operation(dtime_ns_t input) {
	return MillisecondsOperator::Operation<dtime_t, int64_t>(AsTime::Operation<dtime_ns_t, dtime_t>(input));
}

template <>
int64_t DatePart::MillisecondsOperator::Operation(dtime_tz_t input) {
	return DatePart::MillisecondsOperator::Operation<dtime_t, int64_t>(input.time());
}

template <>
int64_t DatePart::SecondsOperator::Operation(timestamp_t input) {
	D_ASSERT(input.IsFinite());
	return MicrosecondsOperator::Operation<timestamp_t, int64_t>(input) / Interval::MICROS_PER_SEC;
}

template <>
int64_t DatePart::SecondsOperator::Operation(interval_t input) {
	return MicrosecondsOperator::Operation<interval_t, int64_t>(input) / Interval::MICROS_PER_SEC;
}

template <>
int64_t DatePart::SecondsOperator::Operation(dtime_t input) {
	return MicrosecondsOperator::Operation<dtime_t, int64_t>(input) / Interval::MICROS_PER_SEC;
}

template <>
int64_t DatePart::SecondsOperator::Operation(dtime_ns_t input) {
	return SecondsOperator::Operation<dtime_t, int64_t>(AsTime::Operation<dtime_ns_t, dtime_t>(input));
}

template <>
int64_t DatePart::SecondsOperator::Operation(dtime_tz_t input) {
	return DatePart::SecondsOperator::Operation<dtime_t, int64_t>(input.time());
}

template <>
int64_t DatePart::MinutesOperator::Operation(timestamp_t input) {
	D_ASSERT(input.IsFinite());
	auto time = Timestamp::GetTime(input);
	// remove the hour part, and truncate to minutes
	return (time.value % Interval::MICROS_PER_HOUR) / Interval::MICROS_PER_MINUTE;
}

template <>
int64_t DatePart::MinutesOperator::Operation(interval_t input) {
	// remove the hour part, and truncate to minutes
	return (input.micros % Interval::MICROS_PER_HOUR) / Interval::MICROS_PER_MINUTE;
}

template <>
int64_t DatePart::MinutesOperator::Operation(dtime_t input) {
	// remove the hour part, and truncate to minutes
	return (input.value % Interval::MICROS_PER_HOUR) / Interval::MICROS_PER_MINUTE;
}

template <>
int64_t DatePart::MinutesOperator::Operation(dtime_ns_t input) {
	return MinutesOperator::Operation<dtime_t, int64_t>(AsTime::Operation<dtime_ns_t, dtime_t>(input));
}

template <>
int64_t DatePart::MinutesOperator::Operation(dtime_tz_t input) {
	return DatePart::MinutesOperator::Operation<dtime_t, int64_t>(input.time());
}

template <>
int64_t DatePart::HoursOperator::Operation(timestamp_t input) {
	D_ASSERT(input.IsFinite());
	return Timestamp::GetTime(input).value / Interval::MICROS_PER_HOUR;
}

template <>
int64_t DatePart::HoursOperator::Operation(interval_t input) {
	return input.micros / Interval::MICROS_PER_HOUR;
}

template <>
int64_t DatePart::HoursOperator::Operation(dtime_t input) {
	return input.value / Interval::MICROS_PER_HOUR;
}

template <>
int64_t DatePart::HoursOperator::Operation(dtime_ns_t input) {
	return HoursOperator::Operation<dtime_t, int64_t>(AsTime::Operation<dtime_ns_t, dtime_t>(input));
}

template <>
int64_t DatePart::HoursOperator::Operation(dtime_tz_t input) {
	return DatePart::HoursOperator::Operation<dtime_t, int64_t>(input.time());
}

template <>
int64_t DatePart::DayParentOperator::Operation(date_t input) {
	return input.days;
}

template <>
int64_t DatePart::DayParentOperator::Operation(timestamp_t input) {
	return Timestamp::GetDate(input).days;
}

// a time-of-day value always lies within a single day
template <>
int64_t DatePart::DayParentOperator::Operation(dtime_t input) {
	return 0;
}

template <>
int64_t DatePart::DayParentOperator::Operation(dtime_ns_t input) {
	return 0;
}

// TIME_TZ parts are not monotone under its UTC-based ordering, so only identical values share a parent
template <>
int64_t DatePart::DayParentOperator::Operation(dtime_tz_t input) {
	return NumericCast<int64_t>(input.bits);
}

template <>
int64_t DatePart::DayHourParentOperator::Operation(dtime_tz_t input) {
	return NumericCast<int64_t>(input.bits);
}

template <>
int64_t DatePart::DayHourMinuteParentOperator::Operation(dtime_tz_t input) {
	return NumericCast<int64_t>(input.bits);
}

template <>
int64_t DatePart::DayHourMinuteParentOperator::Operation(timestamp_ns_t input) {
	date_t date;
	dtime_t time;
	int32_t nanos;
	Timestamp::Convert(input, date, time, nanos);
	return int64_t(date.days) * 24 * 60 + time.value / Interval::MICROS_PER_MINUTE;
}

template <>
double DatePart::EpochOperator::Operation(timestamp_t input) {
	D_ASSERT(input.IsFinite());
	return double(Timestamp::GetEpochMicroSeconds(input)) / double(Interval::MICROS_PER_SEC);
}

template <>
double DatePart::EpochOperator::Operation(interval_t input) {
	int64_t interval_years = input.months / Interval::MONTHS_PER_YEAR;
	int64_t interval_days;
	interval_days = Interval::DAYS_PER_YEAR * interval_years;
	interval_days += Interval::DAYS_PER_MONTH * (input.months % Interval::MONTHS_PER_YEAR);
	interval_days += input.days;
	int64_t interval_epoch;
	interval_epoch = interval_days * Interval::SECS_PER_DAY;
	// we add 0.25 days per year to sort of account for leap days
	interval_epoch += interval_years * (Interval::SECS_PER_DAY / 4);
	return double(interval_epoch) + double(input.micros) / double(Interval::MICROS_PER_SEC);
}

//	TODO: We can't propagate interval statistics because we can't easily compare interval_t for order.
template <>
unique_ptr<BaseStatistics> DatePart::EpochOperator::PropagateStatistics<interval_t>(ClientContext &context,
                                                                                    FunctionStatisticsInput &input) {
	return nullptr;
}

template <>
double DatePart::EpochOperator::Operation(dtime_t input) {
	return double(input.value) / double(Interval::MICROS_PER_SEC);
}

template <>
double DatePart::EpochOperator::Operation(dtime_ns_t input) {
	return EpochOperator::Operation<dtime_t, double>(AsTime::Operation<dtime_ns_t, dtime_t>(input));
}

template <>
double DatePart::EpochOperator::Operation(dtime_tz_t input) {
	return DatePart::EpochOperator::Operation<dtime_t, double>(input.time());
}

template <>
unique_ptr<BaseStatistics> DatePart::EpochOperator::PropagateStatistics<dtime_t>(ClientContext &context,
                                                                                 FunctionStatisticsInput &input) {
	auto result = NumericStats::CreateEmpty(LogicalType::DOUBLE);
	result.CopyValidity(input.child_stats[0]);
	NumericStats::SetMin(result, Value::DOUBLE(0));
	NumericStats::SetMax(result, Value::DOUBLE(Interval::SECS_PER_DAY));
	return result.ToUnique();
}

template <>
int64_t DatePart::EraOperator::Operation(timestamp_t input) {
	D_ASSERT(input.IsFinite());
	return EraOperator::Operation<date_t, int64_t>(Timestamp::GetDate(input));
}

template <>
int64_t DatePart::EraOperator::Operation(interval_t input) {
	throw NotImplementedException("interval units \"era\" not recognized");
}

template <>
int64_t DatePart::EraOperator::Operation(dtime_t input) {
	throw NotImplementedException("\"time\" units \"era\" not recognized");
}

template <>
int64_t DatePart::EraOperator::Operation(dtime_ns_t input) {
	return EraOperator::Operation<dtime_t, int64_t>(AsTime::Operation<dtime_ns_t, dtime_t>(input));
}

template <>
int64_t DatePart::EraOperator::Operation(dtime_tz_t input) {
	return EraOperator::Operation<dtime_t, int64_t>(input.time());
}

template <>
int64_t DatePart::TimezoneOperator::Operation(date_t input) {
	throw NotImplementedException("\"date\" units \"timezone\" not recognized");
}

template <>
int64_t DatePart::TimezoneOperator::Operation(interval_t input) {
	throw NotImplementedException("\"interval\" units \"timezone\" not recognized");
}

template <>
int64_t DatePart::TimezoneOperator::Operation(dtime_tz_t input) {
	return input.offset();
}

template <>
unique_ptr<BaseStatistics> DatePart::TimezoneOperator::PropagateStatistics<dtime_tz_t>(ClientContext &context,
                                                                                       FunctionStatisticsInput &input) {
	return PropagateSimpleDatePartStatistics<-16 * 60 * 60 + 1, 16 * 60 * 60 - 1, dtime_tz_t>(input.child_stats);
}

template <>
int64_t DatePart::TimezoneHourOperator::Operation(date_t input) {
	throw NotImplementedException("\"date\" units \"timezone_hour\" not recognized");
}

template <>
int64_t DatePart::TimezoneHourOperator::Operation(interval_t input) {
	throw NotImplementedException("\"interval\" units \"timezone_hour\" not recognized");
}

template <>
int64_t DatePart::TimezoneHourOperator::Operation(dtime_tz_t input) {
	return input.offset() / Interval::SECS_PER_HOUR;
}

template <>
unique_ptr<BaseStatistics>
DatePart::TimezoneHourOperator::PropagateStatistics<dtime_tz_t>(ClientContext &context,
                                                                FunctionStatisticsInput &input) {
	return PropagateSimpleDatePartStatistics<-15, 15, dtime_tz_t>(input.child_stats);
}

template <>
int64_t DatePart::TimezoneMinuteOperator::Operation(date_t input) {
	throw NotImplementedException("\"date\" units \"timezone_minute\" not recognized");
}

template <>
int64_t DatePart::TimezoneMinuteOperator::Operation(interval_t input) {
	throw NotImplementedException("\"interval\" units \"timezone_minute\" not recognized");
}

template <>
int64_t DatePart::TimezoneMinuteOperator::Operation(dtime_tz_t input) {
	return (input.offset() / Interval::SECS_PER_MINUTE) % Interval::MINS_PER_HOUR;
}

template <>
unique_ptr<BaseStatistics>
DatePart::TimezoneMinuteOperator::PropagateStatistics<dtime_tz_t>(ClientContext &context,
                                                                  FunctionStatisticsInput &input) {
	return PropagateSimpleDatePartStatistics<-59, 59, dtime_tz_t>(input.child_stats);
}

template <>
double DatePart::JulianDayOperator::Operation(date_t input) {
	return double(Date::ExtractJulianDay(input));
}

template <>
double DatePart::JulianDayOperator::Operation(interval_t input) {
	throw NotImplementedException("\"interval\" units \"julian\" not recognized");
}

template <>
double DatePart::JulianDayOperator::Operation(dtime_t input) {
	throw NotImplementedException("\"time\" units \"julian\" not recognized");
}

template <>
double DatePart::JulianDayOperator::Operation(dtime_ns_t input) {
	throw NotImplementedException("\"time_ns\" units \"julian\" not recognized");
}

template <>
double DatePart::JulianDayOperator::Operation(dtime_tz_t input) {
	throw NotImplementedException("\"time_tz\" units \"julian\" not recognized");
}

template <>
void DatePart::StructOperator::Operation(bigint_vec &bigint_values, double_vec &double_values, const dtime_t &input,
                                         const idx_t idx, const part_mask_t mask) {
	int64_t *part_data;
	if (mask & TIME) {
		const auto micros = MicrosecondsOperator::Operation<dtime_t, int64_t>(input);
		part_data = HasPartValue(bigint_values, DatePartSpecifier::MICROSECONDS);
		if (part_data) {
			part_data[idx] = micros;
		}
		part_data = HasPartValue(bigint_values, DatePartSpecifier::MILLISECONDS);
		if (part_data) {
			part_data[idx] = micros / Interval::MICROS_PER_MSEC;
		}
		part_data = HasPartValue(bigint_values, DatePartSpecifier::SECOND);
		if (part_data) {
			part_data[idx] = micros / Interval::MICROS_PER_SEC;
		}
		part_data = HasPartValue(bigint_values, DatePartSpecifier::MINUTE);
		if (part_data) {
			part_data[idx] = MinutesOperator::Operation<dtime_t, int64_t>(input);
		}
		part_data = HasPartValue(bigint_values, DatePartSpecifier::HOUR);
		if (part_data) {
			part_data[idx] = HoursOperator::Operation<dtime_t, int64_t>(input);
		}
	}

	if (mask & EPOCH) {
		auto part_data = HasPartValue(double_values, DatePartSpecifier::EPOCH);
		if (part_data) {
			part_data[idx] = EpochOperator::Operation<dtime_t, double>(input);
		}
	}

	if (mask & ZONE) {
		part_data = HasPartValue(bigint_values, DatePartSpecifier::TIMEZONE);
		if (part_data) {
			part_data[idx] = 0;
		}
		part_data = HasPartValue(bigint_values, DatePartSpecifier::TIMEZONE_HOUR);
		if (part_data) {
			part_data[idx] = 0;
		}
		part_data = HasPartValue(bigint_values, DatePartSpecifier::TIMEZONE_MINUTE);
		if (part_data) {
			part_data[idx] = 0;
		}
	}
}

template <>
void DatePart::StructOperator::Operation(bigint_vec &bigint_values, double_vec &double_values, const dtime_ns_t &input,
                                         const idx_t idx, const part_mask_t mask) {
	int64_t *part_data;
	if (mask & TIME) {
		const auto micros = MicrosecondsOperator::Operation<dtime_ns_t, int64_t>(input);
		part_data = HasPartValue(bigint_values, DatePartSpecifier::MICROSECONDS);
		if (part_data) {
			part_data[idx] = micros;
		}
		part_data = HasPartValue(bigint_values, DatePartSpecifier::MILLISECONDS);
		if (part_data) {
			part_data[idx] = micros / Interval::MICROS_PER_MSEC;
		}
		part_data = HasPartValue(bigint_values, DatePartSpecifier::SECOND);
		if (part_data) {
			part_data[idx] = micros / Interval::MICROS_PER_SEC;
		}
		part_data = HasPartValue(bigint_values, DatePartSpecifier::MINUTE);
		if (part_data) {
			part_data[idx] = MinutesOperator::Operation<dtime_ns_t, int64_t>(input);
		}
		part_data = HasPartValue(bigint_values, DatePartSpecifier::HOUR);
		if (part_data) {
			part_data[idx] = HoursOperator::Operation<dtime_ns_t, int64_t>(input);
		}
	}

	if (mask & EPOCH) {
		auto part_data = HasPartValue(double_values, DatePartSpecifier::EPOCH);
		if (part_data) {
			part_data[idx] = EpochOperator::Operation<dtime_ns_t, double>(input);
		}
	}

	if (mask & ZONE) {
		part_data = HasPartValue(bigint_values, DatePartSpecifier::TIMEZONE);
		if (part_data) {
			part_data[idx] = 0;
		}
		part_data = HasPartValue(bigint_values, DatePartSpecifier::TIMEZONE_HOUR);
		if (part_data) {
			part_data[idx] = 0;
		}
		part_data = HasPartValue(bigint_values, DatePartSpecifier::TIMEZONE_MINUTE);
		if (part_data) {
			part_data[idx] = 0;
		}
	}
}

template <>
void DatePart::StructOperator::Operation(bigint_vec &bigint_values, double_vec &double_values, const dtime_tz_t &input,
                                         const idx_t idx, const part_mask_t mask) {
	int64_t *part_data;
	if (mask & TIME) {
		const auto micros = MicrosecondsOperator::Operation<dtime_tz_t, int64_t>(input);
		part_data = HasPartValue(bigint_values, DatePartSpecifier::MICROSECONDS);
		if (part_data) {
			part_data[idx] = micros;
		}
		part_data = HasPartValue(bigint_values, DatePartSpecifier::MILLISECONDS);
		if (part_data) {
			part_data[idx] = micros / Interval::MICROS_PER_MSEC;
		}
		part_data = HasPartValue(bigint_values, DatePartSpecifier::SECOND);
		if (part_data) {
			part_data[idx] = micros / Interval::MICROS_PER_SEC;
		}
		part_data = HasPartValue(bigint_values, DatePartSpecifier::MINUTE);
		if (part_data) {
			part_data[idx] = MinutesOperator::Operation<dtime_tz_t, int64_t>(input);
		}
		part_data = HasPartValue(bigint_values, DatePartSpecifier::HOUR);
		if (part_data) {
			part_data[idx] = HoursOperator::Operation<dtime_tz_t, int64_t>(input);
		}
	}

	if (mask & EPOCH) {
		auto part_data = HasPartValue(double_values, DatePartSpecifier::EPOCH);
		if (part_data) {
			part_data[idx] = EpochOperator::Operation<dtime_tz_t, double>(input);
		}
	}

	if (mask & ZONE) {
		part_data = HasPartValue(bigint_values, DatePartSpecifier::TIMEZONE);
		if (part_data) {
			part_data[idx] = TimezoneOperator::Operation<dtime_tz_t, int64_t>(input);
		}
		part_data = HasPartValue(bigint_values, DatePartSpecifier::TIMEZONE_HOUR);
		if (part_data) {
			part_data[idx] = TimezoneHourOperator::Operation<dtime_tz_t, int64_t>(input);
		}
		part_data = HasPartValue(bigint_values, DatePartSpecifier::TIMEZONE_MINUTE);
		if (part_data) {
			part_data[idx] = TimezoneMinuteOperator::Operation<dtime_tz_t, int64_t>(input);
		}
		return;
	}
}

template <>
void DatePart::StructOperator::Operation(bigint_vec &bigint_values, double_vec &double_values, const timestamp_t &input,
                                         const idx_t idx, const part_mask_t mask) {
	D_ASSERT(input.IsFinite());
	date_t d;
	dtime_t t;
	Timestamp::Convert(input, d, t);

	// Both define epoch, and the correct value is the sum.
	// So mask it out and compute it separately.
	Operation(bigint_values, double_values, d, idx, mask & ~UnsafeNumericCast<part_mask_t>(EPOCH));
	Operation(bigint_values, double_values, t, idx, mask & ~UnsafeNumericCast<part_mask_t>(EPOCH));

	if (mask & EPOCH) {
		auto part_data = HasPartValue(double_values, DatePartSpecifier::EPOCH);
		if (part_data) {
			part_data[idx] = EpochOperator::Operation<timestamp_t, double>(input);
		}
	}

	if (mask & JD) {
		auto part_data = HasPartValue(double_values, DatePartSpecifier::JULIAN_DAY);
		if (part_data) {
			part_data[idx] = JulianDayOperator::Operation<timestamp_t, double>(input);
		}
	}
}

template <>
void DatePart::StructOperator::Operation(bigint_vec &bigint_values, double_vec &double_values, const interval_t &input,
                                         const idx_t idx, const part_mask_t mask) {
	int64_t *part_data;
	if (mask & YMD) {
		const auto mm = input.months % Interval::MONTHS_PER_YEAR;
		part_data = HasPartValue(bigint_values, DatePartSpecifier::YEAR);
		if (part_data) {
			part_data[idx] = input.months / Interval::MONTHS_PER_YEAR;
		}
		part_data = HasPartValue(bigint_values, DatePartSpecifier::MONTH);
		if (part_data) {
			part_data[idx] = mm;
		}
		part_data = HasPartValue(bigint_values, DatePartSpecifier::DAY);
		if (part_data) {
			part_data[idx] = input.days;
		}
		part_data = HasPartValue(bigint_values, DatePartSpecifier::DECADE);
		if (part_data) {
			part_data[idx] = input.months / Interval::MONTHS_PER_DECADE;
		}
		part_data = HasPartValue(bigint_values, DatePartSpecifier::CENTURY);
		if (part_data) {
			part_data[idx] = input.months / Interval::MONTHS_PER_CENTURY;
		}
		part_data = HasPartValue(bigint_values, DatePartSpecifier::MILLENNIUM);
		if (part_data) {
			part_data[idx] = input.months / Interval::MONTHS_PER_MILLENIUM;
		}
		part_data = HasPartValue(bigint_values, DatePartSpecifier::QUARTER);
		if (part_data) {
			part_data[idx] = mm / Interval::MONTHS_PER_QUARTER + 1;
		}
	}

	if (mask & TIME) {
		const auto micros = MicrosecondsOperator::Operation<interval_t, int64_t>(input);
		part_data = HasPartValue(bigint_values, DatePartSpecifier::MICROSECONDS);
		if (part_data) {
			part_data[idx] = micros;
		}
		part_data = HasPartValue(bigint_values, DatePartSpecifier::MILLISECONDS);
		if (part_data) {
			part_data[idx] = micros / Interval::MICROS_PER_MSEC;
		}
		part_data = HasPartValue(bigint_values, DatePartSpecifier::SECOND);
		if (part_data) {
			part_data[idx] = micros / Interval::MICROS_PER_SEC;
		}
		part_data = HasPartValue(bigint_values, DatePartSpecifier::MINUTE);
		if (part_data) {
			part_data[idx] = MinutesOperator::Operation<interval_t, int64_t>(input);
		}
		part_data = HasPartValue(bigint_values, DatePartSpecifier::HOUR);
		if (part_data) {
			part_data[idx] = HoursOperator::Operation<interval_t, int64_t>(input);
		}
	}

	if (mask & EPOCH) {
		auto part_data = HasPartValue(double_values, DatePartSpecifier::EPOCH);
		if (part_data) {
			part_data[idx] = EpochOperator::Operation<interval_t, double>(input);
		}
	}
}

template <typename T>
int64_t ExtractElement(DatePartSpecifier type, T element) {
	switch (type) {
	case DatePartSpecifier::YEAR:
		return DatePart::YearOperator::template Operation<T, int64_t>(element);
	case DatePartSpecifier::MONTH:
		return DatePart::MonthOperator::template Operation<T, int64_t>(element);
	case DatePartSpecifier::DAY:
		return DatePart::DayOperator::template Operation<T, int64_t>(element);
	case DatePartSpecifier::DECADE:
		return DatePart::DecadeOperator::template Operation<T, int64_t>(element);
	case DatePartSpecifier::CENTURY:
		return DatePart::CenturyOperator::template Operation<T, int64_t>(element);
	case DatePartSpecifier::MILLENNIUM:
		return DatePart::MillenniumOperator::template Operation<T, int64_t>(element);
	case DatePartSpecifier::QUARTER:
		return DatePart::QuarterOperator::template Operation<T, int64_t>(element);
	case DatePartSpecifier::DOW:
		return DatePart::DayOfWeekOperator::template Operation<T, int64_t>(element);
	case DatePartSpecifier::ISODOW:
		return DatePart::ISODayOfWeekOperator::template Operation<T, int64_t>(element);
	case DatePartSpecifier::DOY:
		return DatePart::DayOfYearOperator::template Operation<T, int64_t>(element);
	case DatePartSpecifier::WEEK:
		return DatePart::WeekOperator::template Operation<T, int64_t>(element);
	case DatePartSpecifier::ISOYEAR:
		return DatePart::ISOYearOperator::template Operation<T, int64_t>(element);
	case DatePartSpecifier::YEARWEEK:
		return DatePart::YearWeekOperator::template Operation<T, int64_t>(element);
	case DatePartSpecifier::MICROSECONDS:
		return DatePart::MicrosecondsOperator::template Operation<T, int64_t>(element);
	case DatePartSpecifier::MILLISECONDS:
		return DatePart::MillisecondsOperator::template Operation<T, int64_t>(element);
	case DatePartSpecifier::SECOND:
		return DatePart::SecondsOperator::template Operation<T, int64_t>(element);
	case DatePartSpecifier::MINUTE:
		return DatePart::MinutesOperator::template Operation<T, int64_t>(element);
	case DatePartSpecifier::HOUR:
		return DatePart::HoursOperator::template Operation<T, int64_t>(element);
	case DatePartSpecifier::ERA:
		return DatePart::EraOperator::template Operation<T, int64_t>(element);
	case DatePartSpecifier::TIMEZONE:
		return DatePart::TimezoneOperator::template Operation<T, int64_t>(element);
	case DatePartSpecifier::TIMEZONE_HOUR:
		return DatePart::TimezoneHourOperator::template Operation<T, int64_t>(element);
	case DatePartSpecifier::TIMEZONE_MINUTE:
		return DatePart::TimezoneMinuteOperator::template Operation<T, int64_t>(element);
	default:
		throw NotImplementedException("Specifier type not implemented for DATEPART");
	}
}

template <typename T>
double ExtractDoubleElement(DatePartSpecifier type, T element) {
	switch (type) {
	case DatePartSpecifier::EPOCH:
		return DatePart::EpochOperator::template Operation<T, double>(element);
	case DatePartSpecifier::JULIAN_DAY:
		return DatePart::JulianDayOperator::template Operation<T, double>(element);
	default:
		throw NotImplementedException("Specifier type not implemented for DATEPART");
	}
}

template <typename T>
void DatePartFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	D_ASSERT(args.ColumnCount() == 2);
	const auto &spec_arg = args.data[0];
	const auto &date_arg = args.data[1];

	BinaryExecutor::Execute<string_t, T, double>(
	    spec_arg, date_arg, result, [&](string_t specifier, T date) -> optional<double> {
		    if (date.IsFinite()) {
			    const auto part_code = GetDatePartSpecifier(specifier.GetString());
			    if (IsBigintDatepart(part_code)) {
				    return ExtractElement<T>(part_code, date);
			    } else {
				    return ExtractDoubleElement<T>(part_code, date);
			    }
		    } else {
			    return nullopt;
		    }
	    });
}

template <typename RESULT_TYPE, typename OP>
static scalar_function_t DatePartUnaryCallback(LogicalTypeId type) {
	scalar_function_t result = nullptr;
	switch (type) {
	case LogicalType::TIMESTAMP:
	case LogicalType::TIMESTAMP_S:
	case LogicalType::TIMESTAMP_MS:
	case LogicalType::TIMESTAMP_NS:
		result = DatePart::UnaryFunction<timestamp_t, RESULT_TYPE, OP>;
		break;
	case LogicalType::DATE:
		result = DatePart::UnaryFunction<date_t, RESULT_TYPE, OP>;
		break;
	case LogicalType::INTERVAL:
		result = DatePart::UnaryFunction<interval_t, RESULT_TYPE, OP>;
		break;
	case LogicalType::TIME:
		result = DatePart::UnaryFunction<dtime_t, RESULT_TYPE, OP>;
		break;
	case LogicalType::TIME_NS:
		result = DatePart::UnaryFunction<dtime_ns_t, RESULT_TYPE, OP>;
		break;
	case LogicalType::TIME_TZ:
		result = DatePart::UnaryFunction<dtime_tz_t, RESULT_TYPE, OP>;
		break;
	default:
		throw NotImplementedException("Unsupported temporal type for DATE_PART");
	}
	return result;
}

static scalar_function_t DatePartUnaryCallback(DatePartSpecifier part_code, LogicalTypeId type) {
	switch (part_code) {
	case DatePartSpecifier::YEAR:
		return DatePartUnaryCallback<int64_t, DatePart::YearOperator>(type);
	case DatePartSpecifier::MONTH:
		return DatePartUnaryCallback<int64_t, DatePart::MonthOperator>(type);
	case DatePartSpecifier::DAY:
		return DatePartUnaryCallback<int64_t, DatePart::DayOperator>(type);
	case DatePartSpecifier::DECADE:
		return DatePartUnaryCallback<int64_t, DatePart::DecadeOperator>(type);
	case DatePartSpecifier::CENTURY:
		return DatePartUnaryCallback<int64_t, DatePart::CenturyOperator>(type);
	case DatePartSpecifier::MILLENNIUM:
		return DatePartUnaryCallback<int64_t, DatePart::MillenniumOperator>(type);
	case DatePartSpecifier::QUARTER:
		return DatePartUnaryCallback<int64_t, DatePart::QuarterOperator>(type);
	case DatePartSpecifier::DOW:
		return DatePartUnaryCallback<int64_t, DatePart::DayOfWeekOperator>(type);
	case DatePartSpecifier::ISODOW:
		return DatePartUnaryCallback<int64_t, DatePart::ISODayOfWeekOperator>(type);
	case DatePartSpecifier::DOY:
		return DatePartUnaryCallback<int64_t, DatePart::DayOfYearOperator>(type);
	case DatePartSpecifier::WEEK:
		return DatePartUnaryCallback<int64_t, DatePart::WeekOperator>(type);
	case DatePartSpecifier::ISOYEAR:
		return DatePartUnaryCallback<int64_t, DatePart::ISOYearOperator>(type);
	case DatePartSpecifier::YEARWEEK:
		return DatePartUnaryCallback<int64_t, DatePart::YearWeekOperator>(type);
	case DatePartSpecifier::MICROSECONDS:
		return DatePartUnaryCallback<int64_t, DatePart::MicrosecondsOperator>(type);
	case DatePartSpecifier::MILLISECONDS:
		return DatePartUnaryCallback<int64_t, DatePart::MillisecondsOperator>(type);
	case DatePartSpecifier::SECOND:
		return DatePartUnaryCallback<int64_t, DatePart::SecondsOperator>(type);
	case DatePartSpecifier::MINUTE:
		return DatePartUnaryCallback<int64_t, DatePart::MinutesOperator>(type);
	case DatePartSpecifier::HOUR:
		return DatePartUnaryCallback<int64_t, DatePart::HoursOperator>(type);
	case DatePartSpecifier::ERA:
		return DatePartUnaryCallback<int64_t, DatePart::EraOperator>(type);
	case DatePartSpecifier::TIMEZONE:
		return DatePartUnaryCallback<int64_t, DatePart::TimezoneOperator>(type);
	case DatePartSpecifier::TIMEZONE_HOUR:
		return DatePartUnaryCallback<int64_t, DatePart::TimezoneHourOperator>(type);
	case DatePartSpecifier::TIMEZONE_MINUTE:
		return DatePartUnaryCallback<int64_t, DatePart::TimezoneMinuteOperator>(type);
	case DatePartSpecifier::JULIAN_DAY:
		return DatePartUnaryCallback<double, DatePart::JulianDayOperator>(type);
	case DatePartSpecifier::EPOCH:
		return DatePartUnaryCallback<double, DatePart::EpochOperator>(type);
	case DatePartSpecifier::INVALID:
		throw NotImplementedException("Specifier type not implemented for DATEPART");
	}
	throw InternalException("Unrecognized DatePartSpecifier in DatePartUnaryCallback");
}

template <typename OP>
static function_statistics_t DatePartUnaryStatistics(LogicalTypeId type) {
	switch (type) {
	case LogicalType::TIMESTAMP:
	case LogicalType::TIMESTAMP_S:
	case LogicalType::TIMESTAMP_MS:
	case LogicalType::TIMESTAMP_NS:
		return OP::template PropagateStatistics<timestamp_t>;
	case LogicalType::DATE:
		return OP::template PropagateStatistics<date_t>;
	case LogicalType::INTERVAL:
		//	Interval part statistics are not supported.
		return nullptr;
	case LogicalType::TIME:
		return OP::template PropagateStatistics<dtime_t>;
	case LogicalType::TIME_NS:
		return OP::template PropagateStatistics<dtime_ns_t>;
	case LogicalType::TIME_TZ:
		return OP::template PropagateStatistics<dtime_tz_t>;
	default:
		throw NotImplementedException("Unsupported temporal type for DATE_PART");
	}
}

static function_statistics_t DatePartUnaryStatistics(DatePartSpecifier part_code, LogicalTypeId type) {
	switch (part_code) {
	case DatePartSpecifier::YEAR:
		return DatePartUnaryStatistics<DatePart::YearOperator>(type);
	case DatePartSpecifier::MONTH:
		return DatePartUnaryStatistics<DatePart::MonthOperator>(type);
	case DatePartSpecifier::DAY:
		return DatePartUnaryStatistics<DatePart::DayOperator>(type);
	case DatePartSpecifier::DECADE:
		return DatePartUnaryStatistics<DatePart::DecadeOperator>(type);
	case DatePartSpecifier::CENTURY:
		return DatePartUnaryStatistics<DatePart::CenturyOperator>(type);
	case DatePartSpecifier::MILLENNIUM:
		return DatePartUnaryStatistics<DatePart::MillenniumOperator>(type);
	case DatePartSpecifier::QUARTER:
		return DatePartUnaryStatistics<DatePart::QuarterOperator>(type);
	case DatePartSpecifier::DOW:
		return DatePartUnaryStatistics<DatePart::DayOfWeekOperator>(type);
	case DatePartSpecifier::ISODOW:
		return DatePartUnaryStatistics<DatePart::ISODayOfWeekOperator>(type);
	case DatePartSpecifier::DOY:
		return DatePartUnaryStatistics<DatePart::DayOfYearOperator>(type);
	case DatePartSpecifier::WEEK:
		return DatePartUnaryStatistics<DatePart::WeekOperator>(type);
	case DatePartSpecifier::ISOYEAR:
		return DatePartUnaryStatistics<DatePart::ISOYearOperator>(type);
	case DatePartSpecifier::YEARWEEK:
		return DatePartUnaryStatistics<DatePart::YearWeekOperator>(type);
	case DatePartSpecifier::MICROSECONDS:
		return DatePartUnaryStatistics<DatePart::MicrosecondsOperator>(type);
	case DatePartSpecifier::MILLISECONDS:
		return DatePartUnaryStatistics<DatePart::MillisecondsOperator>(type);
	case DatePartSpecifier::SECOND:
		return DatePartUnaryStatistics<DatePart::SecondsOperator>(type);
	case DatePartSpecifier::MINUTE:
		return DatePartUnaryStatistics<DatePart::MinutesOperator>(type);
	case DatePartSpecifier::HOUR:
		return DatePartUnaryStatistics<DatePart::HoursOperator>(type);
	case DatePartSpecifier::ERA:
		return DatePartUnaryStatistics<DatePart::EraOperator>(type);
	case DatePartSpecifier::TIMEZONE:
		return DatePartUnaryStatistics<DatePart::TimezoneOperator>(type);
	case DatePartSpecifier::TIMEZONE_HOUR:
		return DatePartUnaryStatistics<DatePart::TimezoneHourOperator>(type);
	case DatePartSpecifier::TIMEZONE_MINUTE:
		return DatePartUnaryStatistics<DatePart::TimezoneMinuteOperator>(type);
	case DatePartSpecifier::JULIAN_DAY:
		return DatePartUnaryStatistics<DatePart::JulianDayOperator>(type);
	case DatePartSpecifier::EPOCH:
		return DatePartUnaryStatistics<DatePart::EpochOperator>(type);
	case DatePartSpecifier::INVALID:
		throw NotImplementedException("Specifier type not implemented for DATEPART");
	}
	throw InternalException("Unrecognized DatePartSpecifier in DatePartUnaryStatistics");
}

unique_ptr<FunctionData> DatePartBind(BindScalarFunctionInput &input) {
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	auto part_constant = input.TryGetConstant(0);
	if (!part_constant) {
		return nullptr;
	}

	//	Replace constant parts with unary function equivalents.
	const auto part_name = part_constant->ToString();
	arguments.erase(arguments.begin());
	bound_function.GetArguments().erase(bound_function.GetArguments().begin());
	const auto type = (arguments[0]->GetReturnType().id());
	const auto part_code = GetDateTypePartSpecifier(part_name, type);
	bound_function.SetName(Identifier(StringUtil::Lower(DatePartUnaryFunctionName(part_code))));

	if (IsBigintDatepart(part_code)) {
		bound_function.SetReturnType(LogicalType::BIGINT);
	} else {
		bound_function.SetReturnType(LogicalType::DOUBLE);
	}
	bound_function.SetFunctionCallback(DatePartUnaryCallback(part_code, type));
	bound_function.SetStatisticsCallback(DatePartUnaryStatistics(part_code, type));

	return nullptr;
}

template <init_local_state_t DATE_CACHE = nullptr>
ScalarFunctionSet GetGenericDatePartFunction(scalar_function_t date_func, scalar_function_t ts_func,
                                             scalar_function_t interval_func, function_statistics_t date_stats,
                                             function_statistics_t ts_stats) {
	ScalarFunctionSet operator_set;
	ScalarFunction date_fun({}, LogicalType::BIGINT, std::move(date_func), nullptr, date_stats, DATE_CACHE);
	date_fun.GetSignature().AddParameter("ts", LogicalType::DATE);
	operator_set.AddFunction(date_fun);
	ScalarFunction ts_fun({}, LogicalType::BIGINT, std::move(ts_func), nullptr, ts_stats, DATE_CACHE);
	ts_fun.GetSignature().AddParameter("ts", LogicalType::TIMESTAMP);
	operator_set.AddFunction(ts_fun);
	ScalarFunction interval_fun({}, LogicalType::BIGINT, std::move(interval_func));
	interval_fun.GetSignature().AddParameter("ts", LogicalType::INTERVAL);
	operator_set.AddFunction(interval_fun);
	operator_set.SetFallible();
	return operator_set;
}

template <class OP>
ScalarFunctionSet GetDatePartFunction() {
	return GetGenericDatePartFunction(
	    DatePart::UnaryFunction<date_t, int64_t, OP>, DatePart::UnaryFunction<timestamp_t, int64_t, OP>,
	    ScalarFunction::UnaryFunction<interval_t, int64_t, OP>, OP::template PropagateStatistics<date_t>,
	    OP::template PropagateStatistics<timestamp_t>);
}

ScalarFunctionSet GetGenericTimePartFunction(const LogicalType &result_type, scalar_function_t date_func,
                                             scalar_function_t ts_func, scalar_function_t interval_func,
                                             scalar_function_t time_func, scalar_function_t time_ns_func,
                                             scalar_function_t timetz_func, function_statistics_t date_stats,
                                             function_statistics_t ts_stats, function_statistics_t time_stats,
                                             function_statistics_t time_ns_stats, function_statistics_t timetz_stats,
                                             const Identifier &param_name = "ts") {
	ScalarFunctionSet operator_set;
	ScalarFunction date_fun({}, result_type, std::move(date_func), nullptr, date_stats);
	date_fun.GetSignature().AddParameter(param_name, LogicalType::DATE);
	operator_set.AddFunction(date_fun);
	ScalarFunction ts_fun({}, result_type, std::move(ts_func), nullptr, ts_stats);
	ts_fun.GetSignature().AddParameter(param_name, LogicalType::TIMESTAMP);
	operator_set.AddFunction(ts_fun);
	ScalarFunction interval_fun({}, result_type, std::move(interval_func));
	interval_fun.GetSignature().AddParameter(param_name, LogicalType::INTERVAL);
	operator_set.AddFunction(interval_fun);
	ScalarFunction time_fun({}, result_type, std::move(time_func), nullptr, time_stats);
	time_fun.GetSignature().AddParameter(param_name, LogicalType::TIME);
	operator_set.AddFunction(time_fun);
	ScalarFunction time_ns_fun({}, result_type, std::move(time_ns_func), nullptr, time_ns_stats);
	time_ns_fun.GetSignature().AddParameter(param_name, LogicalType::TIME_NS);
	operator_set.AddFunction(time_ns_fun);
	ScalarFunction timetz_fun({}, result_type, std::move(timetz_func), nullptr, timetz_stats);
	timetz_fun.GetSignature().AddParameter(param_name, LogicalType::TIME_TZ);
	operator_set.AddFunction(timetz_fun);
	return operator_set;
}

template <class OP, class TR = int64_t>
ScalarFunctionSet GetTimePartFunction(const LogicalType &result_type = LogicalType::BIGINT,
                                      const Identifier &param_name = "ts") {
	return GetGenericTimePartFunction(
	    result_type, DatePart::UnaryFunction<date_t, TR, OP>, DatePart::UnaryFunction<timestamp_t, TR, OP>,
	    ScalarFunction::UnaryFunction<interval_t, TR, OP>, ScalarFunction::UnaryFunction<dtime_t, TR, OP>,
	    ScalarFunction::UnaryFunction<dtime_ns_t, TR, OP>, ScalarFunction::UnaryFunction<dtime_tz_t, TR, OP>,
	    OP::template PropagateStatistics<date_t>, OP::template PropagateStatistics<timestamp_t>,
	    OP::template PropagateStatistics<dtime_t>, OP::template PropagateStatistics<dtime_ns_t>,
	    OP::template PropagateStatistics<dtime_tz_t>, param_name);
}

struct LastDayOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		int32_t yyyy, mm, dd;
		Date::Convert(input, yyyy, mm, dd);
		yyyy += (mm / 12);
		mm %= 12;
		++mm;
		return Date::FromDate(yyyy, mm, 1) - 1;
	}
};

template <>
date_t LastDayOperator::Operation(timestamp_t input) {
	return LastDayOperator::Operation<date_t, date_t>(Timestamp::GetDate(input));
}

struct MonthNameOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		return Date::MONTH_NAMES[DatePart::MonthOperator::Operation<TA, int64_t>(input) - 1];
	}
};

struct DayNameOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		return Date::DAY_NAMES[DatePart::DayOfWeekOperator::Operation<TA, int64_t>(input)];
	}
};

struct StructDatePart {
	using part_codes_t = vector<DatePartSpecifier>;

	struct BindData : public VariableReturnBindData {
		part_codes_t part_codes;

		explicit BindData(const LogicalType &stype, const part_codes_t &part_codes_p)
		    : VariableReturnBindData(stype), part_codes(part_codes_p) {
		}

		unique_ptr<FunctionData> Copy() const override {
			return make_uniq<BindData>(stype, part_codes);
		}
	};

	static unique_ptr<FunctionData> Bind(BindScalarFunctionInput &input) {
		auto &bound_function = input.GetBoundFunction();
		auto &arguments = input.GetArguments();
		// collect names and deconflict, construct return type
		auto parts_list = input.GetConstant(0);

		case_insensitive_set_t name_collision_set;
		child_list_t<LogicalType> struct_children;
		part_codes_t part_codes;
		if (parts_list.type().id() == LogicalTypeId::LIST) {
			auto &list_children = ListValue::GetChildren(parts_list);
			if (list_children.empty()) {
				throw BinderException("%s requires non-empty lists of part names", bound_function.GetName());
			}
			for (const auto &part_value : list_children) {
				if (part_value.IsNull()) {
					throw BinderException("NULL struct entry name in %s", bound_function.GetName());
				}
				const auto part_name = part_value.ToString();
				const auto part_code = GetDateTypePartSpecifier(part_name, arguments[1]->GetReturnType());
				if (name_collision_set.find(part_name) != name_collision_set.end()) {
					throw BinderException("Duplicate struct entry name \"%s\" in %s", part_name,
					                      bound_function.GetName());
				}
				name_collision_set.insert(part_name);
				part_codes.emplace_back(part_code);
				const auto part_type = IsBigintDatepart(part_code) ? LogicalType::BIGINT : LogicalType::DOUBLE;
				struct_children.emplace_back(make_pair(part_name, part_type));
			}
		} else {
			throw BinderException("%s can only take constant lists of part names", bound_function.GetName());
		}

		bound_function.SetReturnType(LogicalType::STRUCT(struct_children));
		return make_uniq<BindData>(bound_function.GetReturnType(), part_codes);
	}

	template <typename INPUT_TYPE>
	static void Function(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
		auto &info = func_expr.BindInfo()->Cast<BindData>();
		// the part list is folded into the bind data during the bind - only the trailing temporal argument is read.
		// plans written by versions that erased the part list have a single argument
		D_ASSERT(args.ColumnCount() == 1 || args.ColumnCount() == 2);
		const auto count = args.size();
		const Vector &input = args.data[args.ColumnCount() - 1];

		//	Type counts
		const auto BIGINT_COUNT = size_t(DatePartSpecifier::BEGIN_DOUBLE) - size_t(DatePartSpecifier::BEGIN_BIGINT);
		const auto DOUBLE_COUNT = size_t(DatePartSpecifier::BEGIN_INVALID) - size_t(DatePartSpecifier::BEGIN_DOUBLE);
		DatePart::StructOperator::bigint_vec bigint_values(BIGINT_COUNT, nullptr);
		DatePart::StructOperator::double_vec double_values(DOUBLE_COUNT, nullptr);
		const auto part_mask = DatePart::StructOperator::GetMask(info.part_codes);

		auto &child_entries = StructVector::GetEntries(result);

		// The first computer of a part "owns" it
		// and other requesters just reference the owner
		vector<size_t> owners(int(DatePartSpecifier::JULIAN_DAY) + 1, child_entries.size());
		for (size_t col = 0; col < child_entries.size(); ++col) {
			const auto part_index = size_t(info.part_codes[col]);
			if (owners[part_index] == child_entries.size()) {
				owners[part_index] = col;
			}
		}

		auto entries = input.Values<INPUT_TYPE>();

		// Start with a valid flat vector
		result.SetVectorType(VectorType::FLAT_VECTOR);
		auto &res_valid = FlatVector::ValidityMutable(result);
		if (res_valid.GetData()) {
			res_valid.SetAllValid(count);
		}

		// Start with valid children
		for (size_t col = 0; col < child_entries.size(); ++col) {
			auto &child_entry = child_entries[col];
			child_entry.SetVectorType(VectorType::FLAT_VECTOR);
			auto &child_validity = FlatVector::ValidityMutable(child_entry);
			if (child_validity.GetData()) {
				child_validity.SetAllValid(count);
			}

			// Pre-multiplex
			const auto part_index = size_t(info.part_codes[col]);
			if (owners[part_index] == col) {
				if (IsBigintDatepart(info.part_codes[col])) {
					bigint_values[part_index - size_t(DatePartSpecifier::BEGIN_BIGINT)] =
					    FlatVector::GetDataMutable<int64_t>(child_entry);
				} else {
					double_values[part_index - size_t(DatePartSpecifier::BEGIN_DOUBLE)] =
					    FlatVector::GetDataMutable<double>(child_entry);
				}
			}
		}

		for (idx_t i = 0; i < count; ++i) {
			auto entry = entries[i];
			if (entry.IsValid()) {
				if (Value::IsFinite(entry.GetValue())) {
					DatePart::StructOperator::Operation(bigint_values, double_values, entry.GetValue(), i, part_mask);
				} else {
					for (auto &child_entry : child_entries) {
						FlatVector::ValidityMutable(child_entry).SetInvalid(i);
					}
				}
			} else {
				res_valid.SetInvalid(i);
				for (auto &child_entry : child_entries) {
					FlatVector::ValidityMutable(child_entry).SetInvalid(i);
				}
			}
		}

		// Reference any duplicate parts
		for (size_t col = 0; col < child_entries.size(); ++col) {
			const auto part_index = size_t(info.part_codes[col]);
			const auto owner = owners[part_index];
			if (owner != col) {
				child_entries[col].Reference(child_entries[owner]);
			}
		}

		result.Verify();
	}

	static void SerializeFunction(Serializer &serializer, const optional_ptr<FunctionData> bind_data_p,
	                              const BoundScalarFunction &function) {
		D_ASSERT(bind_data_p);
		auto &info = bind_data_p->Cast<BindData>();
		serializer.WriteProperty(100, "stype", info.stype);
		serializer.WriteProperty(101, "part_codes", info.part_codes);
	}

	static unique_ptr<FunctionData> DeserializeFunction(Deserializer &deserializer,
	                                                    BoundScalarFunction &bound_function) {
		auto stype = deserializer.ReadProperty<LogicalType>(100, "stype");
		auto part_codes = deserializer.ReadProperty<vector<DatePartSpecifier>>(101, "part_codes");
		return make_uniq<BindData>(std::move(stype), std::move(part_codes));
	}

	template <typename INPUT_TYPE>
	static ScalarFunction GetFunction(const LogicalType &temporal_type) {
		auto part_type = LogicalType::LIST(LogicalType::VARCHAR);
		auto result_type = LogicalType::STRUCT({});
		ScalarFunction result({}, result_type, Function<INPUT_TYPE>, Bind);
		result.GetSignature().AddParameter("part_list", part_type).AddParameter("ts", temporal_type);
		result.SetSerializeCallback(SerializeFunction);
		result.SetDeserializeCallback(DeserializeFunction);
		return result;
	}
};
template <class OP>
ScalarFunctionSet GetCachedDatepartFunction() {
	return GetGenericDatePartFunction<InitDateCacheLocalState<OP>>(
	    DatePartCachedFunction<OP, date_t>, DatePartCachedFunction<OP, timestamp_t>,
	    ScalarFunction::UnaryFunction<interval_t, int64_t, OP>, OP::template PropagateStatistics<date_t>,
	    OP::template PropagateStatistics<timestamp_t>);
}

void SetNonDecreasingExceptInterval(ScalarFunctionSet &functions) {
	functions.ApplyToFunctions([](ScalarFunction &function) {
		// Interval components need not preserve the normalized interval ordering.
		if (function.GetSignature().GetParameter(0).GetType() != LogicalType::INTERVAL) {
			function.SetUnaryArgProperties(ArgProperties().NonDecreasing());
		}
	});
}

} // namespace

ScalarFunctionSet YearFun::GetFunctions() {
	auto set = GetCachedDatepartFunction<DatePart::YearOperator>();
	SetNonDecreasingExceptInterval(set);
	return set;
}

ScalarFunctionSet MonthFun::GetFunctions() {
	return GetCachedDatepartFunction<DatePart::MonthOperator>();
}

ScalarFunctionSet DayFun::GetFunctions() {
	return GetCachedDatepartFunction<DatePart::DayOperator>();
}

ScalarFunctionSet DecadeFun::GetFunctions() {
	auto set = GetDatePartFunction<DatePart::DecadeOperator>();
	SetNonDecreasingExceptInterval(set);
	return set;
}

ScalarFunctionSet CenturyFun::GetFunctions() {
	auto set = GetDatePartFunction<DatePart::CenturyOperator>();
	SetNonDecreasingExceptInterval(set);
	return set;
}

ScalarFunctionSet MillenniumFun::GetFunctions() {
	auto set = GetDatePartFunction<DatePart::MillenniumOperator>();
	SetNonDecreasingExceptInterval(set);
	return set;
}

ScalarFunctionSet QuarterFun::GetFunctions() {
	return GetDatePartFunction<DatePart::QuarterOperator>();
}

ScalarFunctionSet DayOfWeekFun::GetFunctions() {
	auto set = GetDatePartFunction<DatePart::DayOfWeekOperator>();
	set.SetFallible();
	return set;
}

ScalarFunctionSet ISODayOfWeekFun::GetFunctions() {
	return GetDatePartFunction<DatePart::ISODayOfWeekOperator>();
}

ScalarFunctionSet DayOfYearFun::GetFunctions() {
	return GetDatePartFunction<DatePart::DayOfYearOperator>();
}

ScalarFunctionSet WeekFun::GetFunctions() {
	return GetDatePartFunction<DatePart::WeekOperator>();
}

ScalarFunctionSet ISOYearFun::GetFunctions() {
	auto set = GetDatePartFunction<DatePart::ISOYearOperator>();
	set.SetUnaryArgProperties(ArgProperties().NonDecreasing());
	return set;
}

ScalarFunctionSet EraFun::GetFunctions() {
	auto set = GetDatePartFunction<DatePart::EraOperator>();
	set.SetUnaryArgProperties(ArgProperties().NonDecreasing());
	return set;
}

ScalarFunctionSet TimezoneFun::GetFunctions() {
	auto operator_set = GetTimePartFunction<DatePart::TimezoneOperator>();

	//	PG also defines timezone(INTERVAL, TIME_TZ) => TIME_TZ
	ScalarFunction function({}, LogicalType::TIME_TZ,
	                        DatePart::TimezoneOperator::BinaryFunction<interval_t, dtime_tz_t, dtime_tz_t>);
	function.GetSignature().AddParameter("offset", LogicalType::INTERVAL).AddParameter("time_tz", LogicalType::TIME_TZ);

	operator_set.AddFunction(function);

	operator_set.SetFallible();

	return operator_set;
}

//! timezone_hour and timezone_minute throw on every DATE and INTERVAL, but not on the other types
static ScalarFunctionSet SetDateAndIntervalFallible(ScalarFunctionSet set) {
	set.ApplyToFunctions([](ScalarFunction &function) {
		auto type = function.GetSignature().GetParameter(0).GetType().id();
		if (type == LogicalTypeId::DATE || type == LogicalTypeId::INTERVAL) {
			function.SetFallible();
		}
	});
	return set;
}

ScalarFunctionSet TimezoneHourFun::GetFunctions() {
	return SetDateAndIntervalFallible(GetTimePartFunction<DatePart::TimezoneHourOperator>());
}

ScalarFunctionSet TimezoneMinuteFun::GetFunctions() {
	return SetDateAndIntervalFallible(GetTimePartFunction<DatePart::TimezoneMinuteOperator>());
}

ScalarFunctionSet EpochFun::GetFunctions() {
	auto set = GetTimePartFunction<DatePart::EpochOperator, double>(LogicalType::DOUBLE, "temporal");
	SetNonDecreasingExceptInterval(set);
	return set;
}

ScalarFunctionSet EpochNsFun::GetFunctions() {
	using OP = DatePart::EpochNanosecondsOperator;
	auto operator_set = GetTimePartFunction<OP>(LogicalType::BIGINT, "temporal");

	//	TIMESTAMP WITH TIME ZONE has the same representation as TIMESTAMP so no need to defer to ICU
	auto tstz_func = DatePart::UnaryFunction<timestamp_t, int64_t, OP>;
	auto tstz_stats = OP::template PropagateStatistics<timestamp_t>;
	ScalarFunction tstz_fun({}, LogicalType::BIGINT, tstz_func, nullptr, tstz_stats);
	tstz_fun.GetSignature().AddParameter("temporal", LogicalType::TIMESTAMP_TZ);
	operator_set.AddFunction(tstz_fun);
	auto tsns_func = DatePart::UnaryFunction<timestamp_ns_t, int64_t, OP>;
	auto tsns_stats = StatisticsPropagator::PropagateFiniteMonotoneBounds;
	ScalarFunction tsns_fun({}, LogicalType::BIGINT, tsns_func, nullptr, tsns_stats);
	tsns_fun.GetSignature().AddParameter("temporal", LogicalType::TIMESTAMP_NS);
	operator_set.AddFunction(tsns_fun);
	ScalarFunction tstz_ns_fun({}, LogicalType::BIGINT, tsns_func, nullptr, tsns_stats);
	tstz_ns_fun.GetSignature().AddParameter("temporal", LogicalType::TIMESTAMP_TZ_NS);
	operator_set.AddFunction(tstz_ns_fun);
	operator_set.SetUnaryArgProperties(ArgProperties().NonDecreasing());
	// these overflow at the representable extremes, so the failure must be reportable
	operator_set.SetFallible();
	return operator_set;
}

ScalarFunctionSet EpochUsFun::GetFunctions() {
	using OP = DatePart::EpochMicrosecondsOperator;
	auto operator_set = GetTimePartFunction<OP>(LogicalType::BIGINT, "temporal");

	//	TIMESTAMP WITH TIME ZONE has the same representation as TIMESTAMP so no need to defer to ICU
	auto tstz_func = DatePart::UnaryFunction<timestamp_t, int64_t, OP>;
	auto tstz_stats = OP::template PropagateStatistics<timestamp_t>;
	ScalarFunction tstz_fun({}, LogicalType::BIGINT, tstz_func, nullptr, tstz_stats);
	tstz_fun.GetSignature().AddParameter("temporal", LogicalType::TIMESTAMP_TZ);
	operator_set.AddFunction(tstz_fun);
	operator_set.SetUnaryArgProperties(ArgProperties().NonDecreasing());
	// these overflow at the representable extremes, so the failure must be reportable
	operator_set.SetFallible();
	return operator_set;
}

ScalarFunctionSet EpochMsFun::GetFunctions() {
	using OP = DatePart::EpochMillisOperator;
	auto operator_set = GetTimePartFunction<OP>(LogicalType::BIGINT, "temporal");

	//	TIMESTAMP WITH TIME ZONE has the same representation as TIMESTAMP so no need to defer to ICU
	auto tstz_func = DatePart::UnaryFunction<timestamp_t, int64_t, OP>;
	auto tstz_stats = OP::template PropagateStatistics<timestamp_t>;
	ScalarFunction tstz_fun({}, LogicalType::BIGINT, tstz_func, nullptr, tstz_stats);
	tstz_fun.GetSignature().AddParameter("temporal", LogicalType::TIMESTAMP_TZ);
	operator_set.AddFunction(tstz_fun);

	//	Deprecated inverse BIGINT => TIMESTAMP
	ScalarFunction inverse_fun({}, LogicalType::TIMESTAMP, DatePart::EpochMillisOperator::Inverse);
	inverse_fun.GetSignature().AddParameter("temporal", LogicalType::BIGINT);
	operator_set.AddFunction(inverse_fun);

	SetNonDecreasingExceptInterval(operator_set);
	// these overflow at the representable extremes, so the failure must be reportable
	operator_set.SetFallible();
	return operator_set;
}

ScalarFunctionSet MakeTimestampMsFun::GetFunctions() {
	ScalarFunctionSet operator_set("make_timestamp_ms");
	ScalarFunction inverse_fun({}, LogicalType::TIMESTAMP, DatePart::EpochMillisOperator::Inverse);
	inverse_fun.GetSignature().AddParameter("nanos", LogicalType::BIGINT);
	operator_set.AddFunction(inverse_fun);
	operator_set.SetUnaryArgProperties(ArgProperties().NonDecreasing());
	// these overflow at the representable extremes, so the failure must be reportable
	operator_set.SetFallible();
	return operator_set;
}

ScalarFunctionSet NanosecondsFun::GetFunctions() {
	using OP = DatePart::NanosecondsOperator;
	using TR = int64_t;
	const LogicalType &result_type = LogicalType::BIGINT;
	auto operator_set = GetTimePartFunction<OP, TR>(result_type, "tsns");

	auto ns_func = DatePart::UnaryFunction<timestamp_ns_t, TR, OP>;
	auto ns_stats = OP::template PropagateStatistics<timestamp_ns_t>;
	ScalarFunction ns_fun({}, result_type, ns_func, nullptr, ns_stats);
	ns_fun.GetSignature().AddParameter("tsns", LogicalType::TIMESTAMP_NS);
	operator_set.AddFunction(ns_fun);
	ScalarFunction tz_ns_fun({}, result_type, ns_func, nullptr, ns_stats);
	tz_ns_fun.GetSignature().AddParameter("tsns", LogicalType::TIMESTAMP_TZ_NS);
	operator_set.AddFunction(tz_ns_fun);

	//	TIMESTAMP WITH TIME ZONE has the same representation as TIMESTAMP so no need to defer to ICU
	auto tstz_func = DatePart::UnaryFunction<timestamp_t, TR, OP>;
	auto tstz_stats = OP::template PropagateStatistics<timestamp_t>;
	ScalarFunction tstz_fun({}, result_type, tstz_func, nullptr, tstz_stats);
	tstz_fun.GetSignature().AddParameter("tsns", LogicalType::TIMESTAMP_TZ);
	operator_set.AddFunction(tstz_fun);

	return operator_set;
}

ScalarFunctionSet MicrosecondsFun::GetFunctions() {
	return GetTimePartFunction<DatePart::MicrosecondsOperator>();
}

ScalarFunctionSet MillisecondsFun::GetFunctions() {
	return GetTimePartFunction<DatePart::MillisecondsOperator>();
}

ScalarFunctionSet SecondsFun::GetFunctions() {
	return GetTimePartFunction<DatePart::SecondsOperator>();
}

ScalarFunctionSet MinutesFun::GetFunctions() {
	return GetTimePartFunction<DatePart::MinutesOperator>();
}

ScalarFunctionSet HoursFun::GetFunctions() {
	return GetTimePartFunction<DatePart::HoursOperator>();
}

ScalarFunctionSet YearWeekFun::GetFunctions() {
	// Not monotone on the full date domain: see YearWeekOperator::YearWeekFromParts.
	return GetDatePartFunction<DatePart::YearWeekOperator>();
}

ScalarFunctionSet DayOfMonthFun::GetFunctions() {
	return GetDatePartFunction<DatePart::DayOperator>();
}

ScalarFunctionSet WeekDayFun::GetFunctions() {
	return GetDatePartFunction<DatePart::DayOfWeekOperator>();
}

ScalarFunctionSet WeekOfYearFun::GetFunctions() {
	return GetDatePartFunction<DatePart::WeekOperator>();
}

ScalarFunctionSet LastDayFun::GetFunctions() {
	ScalarFunctionSet last_day;
	ScalarFunction date_fun({}, LogicalType::DATE, DatePart::UnaryFunction<date_t, date_t, LastDayOperator>);
	date_fun.GetSignature().AddParameter("ts", LogicalType::DATE);
	last_day.AddFunction(date_fun);
	ScalarFunction ts_fun({}, LogicalType::DATE, DatePart::UnaryFunction<timestamp_t, date_t, LastDayOperator>);
	ts_fun.GetSignature().AddParameter("ts", LogicalType::TIMESTAMP);
	last_day.AddFunction(ts_fun);
	last_day.SetFallible();
	last_day.SetUnaryArgProperties(ArgProperties().NonDecreasing());
	last_day.SetStatisticsCallback(StatisticsPropagator::PropagateFiniteMonotoneBounds);
	return last_day;
}

ScalarFunctionSet MonthNameFun::GetFunctions() {
	ScalarFunctionSet monthname;
	ScalarFunction date_fun({}, LogicalType::VARCHAR, DatePart::UnaryFunction<date_t, string_t, MonthNameOperator>);
	date_fun.GetSignature().AddParameter("ts", LogicalType::DATE);
	monthname.AddFunction(date_fun);
	ScalarFunction ts_fun({}, LogicalType::VARCHAR, DatePart::UnaryFunction<timestamp_t, string_t, MonthNameOperator>);
	ts_fun.GetSignature().AddParameter("ts", LogicalType::TIMESTAMP);
	monthname.AddFunction(ts_fun);
	return monthname;
}

ScalarFunctionSet DayNameFun::GetFunctions() {
	ScalarFunctionSet dayname;
	ScalarFunction date_fun({}, LogicalType::VARCHAR, DatePart::UnaryFunction<date_t, string_t, DayNameOperator>);
	date_fun.GetSignature().AddParameter("ts", LogicalType::DATE);
	dayname.AddFunction(date_fun);
	ScalarFunction ts_fun({}, LogicalType::VARCHAR, DatePart::UnaryFunction<timestamp_t, string_t, DayNameOperator>);
	ts_fun.GetSignature().AddParameter("ts", LogicalType::TIMESTAMP);
	dayname.AddFunction(ts_fun);
	return dayname;
}

ScalarFunctionSet JulianDayFun::GetFunctions() {
	using OP = DatePart::JulianDayOperator;

	ScalarFunctionSet operator_set;
	auto date_func = DatePart::UnaryFunction<date_t, double, OP>;
	auto date_stats = OP::template PropagateStatistics<date_t>;
	ScalarFunction date_fun({}, LogicalType::DOUBLE, date_func, nullptr, date_stats);
	date_fun.GetSignature().AddParameter("ts", LogicalType::DATE);
	operator_set.AddFunction(date_fun);
	auto ts_func = DatePart::UnaryFunction<timestamp_t, double, OP>;
	auto ts_stats = OP::template PropagateStatistics<timestamp_t>;
	ScalarFunction ts_fun({}, LogicalType::DOUBLE, ts_func, nullptr, ts_stats);
	ts_fun.GetSignature().AddParameter("ts", LogicalType::TIMESTAMP);
	operator_set.AddFunction(ts_fun);

	return operator_set;
}

//! Binding date_part with a constant part replaces it with the unary part function (year, month, ...), so the bound
//! call has one argument and must be rendered under the replacement's own name
static unique_ptr<ParsedExpression> DatePartUnbind(FunctionUnbindInput &input) {
	auto &function = input.expression.Function();
	if (input.children.size() == 1) {
		return make_uniq<FunctionExpression>(function.GetQualifiedName(), std::move(input.children));
	}
	if (input.children.size() != 2) {
		return nullptr;
	}
	return make_uniq<FunctionExpression>(function.GetDefinition()->GetQualifiedName(), std::move(input.children));
}

// Names the "part,ts" pair shared by date_part's per-type overloads.
static ScalarFunction NamePartTsArguments(ScalarFunction fun, const LogicalType &type) {
	fun.GetSignature().AddParameter("part", LogicalType::VARCHAR).AddParameter("ts", type);
	fun.SetUnbindCallback(DatePartUnbind);
	return fun;
}

ScalarFunctionSet DatePartFun::GetFunctions() {
	ScalarFunctionSet date_part;
	date_part.AddFunction(NamePartTsArguments(
	    ScalarFunction({}, LogicalType::DOUBLE, DatePartFunction<date_t>, DatePartBind), LogicalType::DATE));
	date_part.AddFunction(NamePartTsArguments(
	    ScalarFunction({}, LogicalType::DOUBLE, DatePartFunction<timestamp_t>, DatePartBind), LogicalType::TIMESTAMP));
	date_part.AddFunction(NamePartTsArguments(
	    ScalarFunction({}, LogicalType::DOUBLE, DatePartFunction<dtime_t>, DatePartBind), LogicalType::TIME));
	date_part.AddFunction(NamePartTsArguments(
	    ScalarFunction({}, LogicalType::DOUBLE, DatePartFunction<dtime_ns_t>, DatePartBind), LogicalType::TIME_NS));
	date_part.AddFunction(NamePartTsArguments(
	    ScalarFunction({}, LogicalType::DOUBLE, DatePartFunction<interval_t>, DatePartBind), LogicalType::INTERVAL));
	date_part.AddFunction(NamePartTsArguments(
	    ScalarFunction({}, LogicalType::DOUBLE, DatePartFunction<dtime_tz_t>, DatePartBind), LogicalType::TIME_TZ));

	// struct variants
	date_part.AddFunction(StructDatePart::GetFunction<date_t>(LogicalType::DATE));
	date_part.AddFunction(StructDatePart::GetFunction<timestamp_t>(LogicalType::TIMESTAMP));
	date_part.AddFunction(StructDatePart::GetFunction<dtime_t>(LogicalType::TIME));
	date_part.AddFunction(StructDatePart::GetFunction<dtime_ns_t>(LogicalType::TIME_NS));
	date_part.AddFunction(StructDatePart::GetFunction<interval_t>(LogicalType::INTERVAL));
	date_part.AddFunction(StructDatePart::GetFunction<dtime_tz_t>(LogicalType::TIME_TZ));

	date_part.SetFallible();

	return date_part;
}

} // namespace duckdb
