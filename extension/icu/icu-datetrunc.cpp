#include "include/icu-datetrunc.hpp"
#include "include/icu-datefunc.hpp"

#include "duckdb/common/enums/date_part_specifier.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

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
					auto micros = SetTime(calendar.get(), TruncCast<T, timestamp_tz_t>(input));
					truncator(calendar.get(), micros);
					return GetTimeUnsafe(calendar.get(), micros);
				} else {
					return timestamp_tz_t(input.value);
				}
			});
		} else {
			BinaryExecutor::Execute<string_t, T, timestamp_tz_t>(
			    part_arg, date_arg, result, [&](string_t specifier, T input) {
				    if (input.IsFinite()) {
					    auto truncator = TruncationFactory(GetDatePartSpecifier(specifier.GetString()));
					    auto micros = SetTime(calendar.get(), TruncCast<T, timestamp_tz_t>(input));
					    truncator(calendar.get(), micros);
					    return GetTimeUnsafe(calendar.get(), micros);
				    } else {
					    return timestamp_tz_t(input.value);
				    }
			    });
		}
	}

	template <typename TA>
	static ScalarFunction GetDateTruncFunction(const LogicalTypeId &type) {
		ScalarFunction fun({}, LogicalType::TIMESTAMP_TZ, ICUDateTruncFunction<TA>, Bind);
		fun.GetSignature().AddParameter("part", LogicalType::VARCHAR).AddParameter("timestamp", type);
		return fun;
	}

	static void AddBinaryTimestampFunction(const Identifier &name, ExtensionLoader &loader) {
		ScalarFunctionSet set {name};
		set.AddFunction(GetDateTruncFunction<timestamp_tz_t>(LogicalType::TIMESTAMP_TZ));
		set.AddFunction(GetDateTruncFunction<timestamp_tz_ns_t>(LogicalType::TIMESTAMP_TZ_NS));
		// throws for unrecognized part specifiers and for dates that overflow the timestamp range
		set.SetFallible();
		set.SetStability(FunctionStability::CONSISTENT_WITHIN_QUERY);
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
