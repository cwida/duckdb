#include "include/icu-datetrunc.hpp"
#include "include/icu-datefunc.hpp"
#include "include/icu-zone-offsets.hpp"

#include "duckdb/common/enums/date_part_specifier.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

#include <type_traits>

#include <cstring>

namespace duckdb {

struct ICUDateTrunc : public ICUDateFunc {
	static unique_ptr<FunctionData> Bind(BindScalarFunctionInput &input) {
		auto part_value = input.TryGetConstant(0);
		if (part_value && !part_value->IsNull()) {
			DatePartSpecifier part;
			if (TryGetDatePartSpecifier(part_value->GetValue<string>(), part) &&
			    (part == DatePartSpecifier::DECADE || part == DatePartSpecifier::CENTURY ||
			     part == DatePartSpecifier::MILLENNIUM || part == DatePartSpecifier::ERA)) {
				// date_trunc is not monotone for these parts because ICU years skip year 0 at the BC/AD boundary
				input.GetBoundFunction().SetArgProperties({});
			}
		}
		return ICUDateFunc::Bind(input);
	}

	static void PreserveOffsets(Calendar *calendar) {
		//	We have to extract _everything_ before setting anything
		//	Otherwise ICU will clear the fStamp fields
		//	This also means we must call this method first.

		//	Force reuse of offsets when reassembling truncated sub-hour times.
		const auto zone_offset = ExtractField(calendar, CAL_ZONE_OFFSET);
		const auto dst_offset = ExtractField(calendar, CAL_DST_OFFSET);

		calendar->Set(CAL_ZONE_OFFSET, zone_offset);
		calendar->Set(CAL_DST_OFFSET, dst_offset);
	}

	static void TruncMicrosecondInternal(Calendar *calendar, uint64_t &micros) {
	}

	static void TruncMicrosecond(Calendar *calendar, uint64_t &micros) {
		PreserveOffsets(calendar);
		TruncMicrosecondInternal(calendar, micros);
	}

	static void TruncMillisecondInternal(Calendar *calendar, uint64_t &micros) {
		TruncMicrosecondInternal(calendar, micros);
		micros = 0;
	}

	static void TruncMillisecond(Calendar *calendar, uint64_t &micros) {
		PreserveOffsets(calendar);
		TruncMillisecondInternal(calendar, micros);
	}

	static void TruncSecondInternal(Calendar *calendar, uint64_t &micros) {
		TruncMillisecondInternal(calendar, micros);
		calendar->Set(CAL_MILLISECOND, 0);
	}

	static void TruncSecond(Calendar *calendar, uint64_t &micros) {
		PreserveOffsets(calendar);
		TruncSecondInternal(calendar, micros);
	}

	static void TruncMinuteInternal(Calendar *calendar, uint64_t &micros) {
		TruncSecondInternal(calendar, micros);
		calendar->Set(CAL_SECOND, 0);
	}

	static void TruncMinute(Calendar *calendar, uint64_t &micros) {
		PreserveOffsets(calendar);
		TruncMinuteInternal(calendar, micros);
	}

	static void TruncHour(Calendar *calendar, uint64_t &micros) {
		TruncMinuteInternal(calendar, micros);
		calendar->Set(CAL_MINUTE, 0);
	}

	static void TruncDay(Calendar *calendar, uint64_t &micros) {
		TruncHour(calendar, micros);
		calendar->Set(CAL_HOUR_OF_DAY, 0);
	}

	static void TruncWeek(Calendar *calendar, uint64_t &micros) {
		calendar->SetFirstDayOfWeek(CAL_MONDAY);
		calendar->SetMinimalDaysInFirstWeek(4);
		TruncDay(calendar, micros);
		calendar->Set(CAL_DAY_OF_WEEK, CAL_MONDAY);
	}

	static void TruncMonth(Calendar *calendar, uint64_t &micros) {
		TruncDay(calendar, micros);
		calendar->Set(CAL_DATE, 1);
	}

	static void TruncQuarter(Calendar *calendar, uint64_t &micros) {
		TruncMonth(calendar, micros);
		auto mm = ExtractField(calendar, CAL_MONTH);
		calendar->Set(CAL_MONTH, (mm / 3) * 3);
	}

	static void TruncYear(Calendar *calendar, uint64_t &micros) {
		TruncMonth(calendar, micros);
		calendar->Set(CAL_MONTH, CAL_JANUARY);
	}

	static void TruncISOYear(Calendar *calendar, uint64_t &micros) {
		TruncWeek(calendar, micros);
		calendar->Set(CAL_WEEK_OF_YEAR, 1);
	}

	static void TruncDecade(Calendar *calendar, uint64_t &micros) {
		TruncYear(calendar, micros);
		auto yyyy = ExtractField(calendar, CAL_YEAR) / 10;
		calendar->Set(CAL_YEAR, yyyy * 10);
	}

	static void TruncCentury(Calendar *calendar, uint64_t &micros) {
		TruncYear(calendar, micros);
		auto yyyy = ExtractField(calendar, CAL_YEAR) / 100;
		calendar->Set(CAL_YEAR, yyyy * 100);
	}

	static void TruncMillenium(Calendar *calendar, uint64_t &micros) {
		TruncYear(calendar, micros);
		auto yyyy = ExtractField(calendar, CAL_YEAR) / 1000;
		calendar->Set(CAL_YEAR, yyyy * 1000);
	}

	static void TruncEra(Calendar *calendar, uint64_t &micros) {
		TruncYear(calendar, micros);
		auto era = ExtractField(calendar, CAL_ERA);
		calendar->Set(CAL_YEAR, 0);
		calendar->Set(CAL_ERA, era);
	}

	static timestamp_tz_t TruncWithCalendar(Calendar *calendar, part_trunc_t truncator, timestamp_tz_t input) {
		auto micros = SetTime(calendar, input);
		truncator(calendar, micros);
		return GetTimeUnsafe(calendar, micros);
	}

	//! The length in µs of the parts that always span the same amount of wall time, or 0.
	static int64_t FixedPartLength(DatePartSpecifier part) {
		switch (part) {
		case DatePartSpecifier::DAY:
		case DatePartSpecifier::DOW:
		case DatePartSpecifier::ISODOW:
		case DatePartSpecifier::DOY:
		case DatePartSpecifier::JULIAN_DAY:
			return Interval::MICROS_PER_DAY;
		case DatePartSpecifier::HOUR:
			return Interval::MICROS_PER_HOUR;
		case DatePartSpecifier::MINUTE:
			return Interval::MICROS_PER_MINUTE;
		case DatePartSpecifier::SECOND:
		case DatePartSpecifier::EPOCH:
			return Interval::MICROS_PER_SEC;
		case DatePartSpecifier::MILLISECONDS:
			return Interval::MICROS_PER_MSEC;
		case DatePartSpecifier::MICROSECONDS:
			return 1;
		default:
			return 0;
		}
	}

	//! A constant part of fixed length, truncated with arithmetic on µs instead of calendar calls.
	struct ArithmeticTruncData : public BindData {
		ArithmeticTruncData(ClientContext &context, DatePartSpecifier part, int64_t unit_p)
		    : BindData(context), truncator(TruncationFactory(part)), unit(unit_p) {
		}

		//! Truncates the instants the arithmetic cannot handle
		part_trunc_t truncator;
		int64_t unit;
		//! Unset below a minute: offsets are whole seconds, so the instant truncates like its wall time.
		shared_ptr<const ZoneOffsets> offsets;

		unique_ptr<FunctionData> Copy() const override {
			return make_uniq<ArithmeticTruncData>(*this);
		}
	};

	//! The per-execution state of an arithmetic truncation.
	struct ArithmeticTruncator {
		explicit ArithmeticTruncator(const ArithmeticTruncData &data) : unit(data.unit), offsets(data.offsets.get()) {
		}

		const int64_t unit;
		const ZoneOffsets *const offsets;
		ZoneOffsets::Cursor instant_cursor;
		ZoneOffsets::Cursor wall_cursor;

		//! Offsets and units are under a day, so truncation moves an instant by less than three days.
		static constexpr int64_t MIN_SAFE = NumericLimits<int64_t>::Minimum() + 3 * Interval::MICROS_PER_DAY;
		static constexpr int64_t MAX_SAFE = NumericLimits<int64_t>::Maximum() - 3 * Interval::MICROS_PER_DAY;

		static int64_t Floor(int64_t value, int64_t unit) {
			const auto remainder = value % unit;
			return value - (remainder < 0 ? remainder + unit : remainder);
		}

		bool TryTrunc(int64_t utc, int64_t &result) {
			if (utc < MIN_SAFE || utc > MAX_SAFE) {
				return false;
			}
			if (!offsets) {
				result = Floor(utc, unit);
				return true;
			}
			int64_t offset;
			if (!offsets->OffsetAtInstant(utc, instant_cursor, offset)) {
				return false;
			}
			const auto floored = Floor(utc + offset, unit);
			//	Like the calendar, keep the instant's offset below an hour (PreserveOffsets) and re-resolve it from an
			//	hour up.
			if (unit >= Interval::MICROS_PER_HOUR && !offsets->OffsetAtWallTime(floored, wall_cursor, offset)) {
				return false;
			}
			result = floored - offset;
			return true;
		}
	};

	template <typename T>
	static void ArithmeticTruncFunction(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &info = state.expr.Cast<BoundFunctionExpression>().BindInfo()->Cast<ArithmeticTruncData>();
		ArithmeticTruncator truncator(info);
		CalendarPtr calendar;
		UnaryExecutor::Execute<T, T>(args.data[1], result, [&](T input) {
			if (!input.IsFinite()) {
				return input;
			}
			int64_t truncated;
			if (truncator.TryTrunc(input.value, truncated)) {
				return T(truncated);
			}
			if (!calendar) {
				calendar = info.calendar->Copy();
			}
			return TruncWithCalendar(calendar.get(), info.truncator, input);
		});
	}

	template <typename T>
	static unique_ptr<FunctionData> BindDateTrunc(BindScalarFunctionInput &input) {
		if constexpr (!std::is_same<T, timestamp_tz_t>::value) {
			//	The arithmetic path works on µs, so TIMESTAMPTZ_NS stays on the calendar path.
			return Bind(input);
		} else {
			return BindArithmeticDateTrunc(input);
		}
	}

	static unique_ptr<FunctionData> BindArithmeticDateTrunc(BindScalarFunctionInput &input) {
		auto &context = input.GetClientContext();
		const auto part_value = input.TryGetConstant(0);
		DatePartSpecifier part;
		if (!part_value || part_value->IsNull() || !TryGetDatePartSpecifier(part_value->ToString(), part) ||
		    !FixedPartLength(part)) {
			return Bind(input);
		}

		auto data = make_uniq<ArithmeticTruncData>(context, part, FixedPartLength(part));
		if (data->unit >= Interval::MICROS_PER_MINUTE) {
			//	Only the offsets are modelled, so other calendars stay on the calendar path.
			if (std::strcmp(data->calendar->GetType(), "gregorian") == 0) {
				//	The zone the calendar was created with, see BindData::InitCalendar
				auto zone = TimeZone::TryCreate(data->tz_setting);
				if (!zone) {
					zone = TimeZone::TryCreate("UTC");
				}
				data->offsets = ZoneOffsets::Build(*zone);
			}
			if (!data->offsets) {
				return Bind(input);
			}
		}
		input.GetBoundFunction().SetFunctionCallback(ArithmeticTruncFunction<timestamp_tz_t>);
		return std::move(data);
	}

	template <typename T>
	static void ICUDateTruncFunction(DataChunk &args, ExpressionState &state, Vector &result) {
		D_ASSERT(args.ColumnCount() == 2);
		const auto &part_arg = args.data[0];
		const auto &date_arg = args.data[1];

		auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
		auto &info = func_expr.BindInfo()->Cast<BindData>();
		CalendarPtr calendar(info.calendar->Copy());

		if (part_arg.GetVectorType() == VectorType::CONSTANT_VECTOR) {
			// Common case of constant part.
			if (ConstantVector::IsNull(part_arg)) {
				throw InternalException("ICUDateTrunc called with constant NULL bucket width");
			}
			const auto specifier = ConstantVector::GetData<string_t>(part_arg)->GetString();
			auto truncator = TruncationFactory(GetDatePartSpecifier(specifier));
			UnaryExecutor::Execute<T, timestamp_tz_t>(date_arg, result, [&](T input) {
				if (input.IsFinite()) {
					return TruncWithCalendar(calendar.get(), truncator, TruncCast<T, timestamp_tz_t>(input));
				} else {
					return timestamp_tz_t(input.value);
				}
			});
		} else {
			BinaryExecutor::Execute<string_t, T, timestamp_tz_t>(
			    part_arg, date_arg, result, [&](string_t specifier, T input) {
				    if (input.IsFinite()) {
					    auto truncator = TruncationFactory(GetDatePartSpecifier(specifier.GetString()));
					    return TruncWithCalendar(calendar.get(), truncator, TruncCast<T, timestamp_tz_t>(input));
				    } else {
					    return timestamp_tz_t(input.value);
				    }
			    });
		}
	}

	template <typename TA>
	static ScalarFunction GetDateTruncFunction(const LogicalTypeId &type) {
		ScalarFunction fun({}, LogicalType::TIMESTAMP_TZ, ICUDateTruncFunction<TA>, BindDateTrunc<TA>);
		fun.GetSignature().AddParameter("part", LogicalType::VARCHAR).AddParameter("timestamp", type);
		return fun;
	}

	static void AddBinaryTimestampFunction(const Identifier &name, ExtensionLoader &loader) {
		ScalarFunctionSet set {name};
		set.AddFunction(GetDateTruncFunction<timestamp_tz_t>(LogicalType::TIMESTAMP_TZ));
		set.AddFunction(GetDateTruncFunction<timestamp_tz_ns_t>(LogicalType::TIMESTAMP_TZ_NS));
		// throws for unrecognized part specifiers and for dates that overflow the timestamp range
		set.SetFallible();
		set.SetArgProperties(1, ArgProperties().NonDecreasing());
		loader.RegisterFunction(set);
	}
};

ICUDateFunc::part_trunc_t ICUDateFunc::TruncationFactory(DatePartSpecifier type) {
	switch (type) {
	case DatePartSpecifier::ERA:
		return ICUDateTrunc::TruncEra;
	case DatePartSpecifier::MILLENNIUM:
		return ICUDateTrunc::TruncMillenium;
	case DatePartSpecifier::CENTURY:
		return ICUDateTrunc::TruncCentury;
	case DatePartSpecifier::DECADE:
		return ICUDateTrunc::TruncDecade;
	case DatePartSpecifier::YEAR:
		return ICUDateTrunc::TruncYear;
	case DatePartSpecifier::QUARTER:
		return ICUDateTrunc::TruncQuarter;
	case DatePartSpecifier::MONTH:
		return ICUDateTrunc::TruncMonth;
	case DatePartSpecifier::WEEK:
	case DatePartSpecifier::YEARWEEK:
		return ICUDateTrunc::TruncWeek;
	case DatePartSpecifier::ISOYEAR:
		return ICUDateTrunc::TruncISOYear;
	case DatePartSpecifier::DAY:
	case DatePartSpecifier::DOW:
	case DatePartSpecifier::ISODOW:
	case DatePartSpecifier::DOY:
	case DatePartSpecifier::JULIAN_DAY:
		return ICUDateTrunc::TruncDay;
	case DatePartSpecifier::HOUR:
		return ICUDateTrunc::TruncHour;
	case DatePartSpecifier::MINUTE:
		return ICUDateTrunc::TruncMinute;
	case DatePartSpecifier::SECOND:
	case DatePartSpecifier::EPOCH:
		return ICUDateTrunc::TruncSecond;
	case DatePartSpecifier::MILLISECONDS:
		return ICUDateTrunc::TruncMillisecond;
	case DatePartSpecifier::MICROSECONDS:
		return ICUDateTrunc::TruncMicrosecond;
	default:
		throw NotImplementedException("Specifier type not implemented for ICU DATETRUNC");
	}
}

timestamp_tz_t ICUDateFunc::CurrentMidnight(Calendar *calendar, ExpressionState &state) {
	const timestamp_tz_t current_timestamp(MetaTransaction::Get(state.GetContext()).start_timestamp);
	auto current_micros = SetTime(calendar, current_timestamp);
	ICUDateTrunc::TruncDay(calendar, current_micros);
	return GetTime(calendar);
}

void RegisterICUDateTruncFunctions(ExtensionLoader &loader) {
	ICUDateTrunc::AddBinaryTimestampFunction("date_trunc", loader);
	ICUDateTrunc::AddBinaryTimestampFunction("datetrunc", loader);
}

} // namespace duckdb
