#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "include/icu-datepart.hpp"
#include "include/icu-datefunc.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/common/enums/date_part_specifier.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/optimizer/statistics_propagator.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

static string DatePartUnaryFunctionName(DatePartSpecifier part) {
	switch (part) {
	case DatePartSpecifier::DOW:
		return "dayofweek";
	case DatePartSpecifier::DOY:
		return "dayofyear";
	case DatePartSpecifier::JULIAN_DAY:
		return "julian";
	case DatePartSpecifier::MILLISECONDS:
		return "millisecond";
	case DatePartSpecifier::MICROSECONDS:
		return "microsecond";
	default:
		return StringUtil::Lower(EnumUtil::ToChars(part));
	}
}

struct ICUDatePart : public ICUDateFunc {
	typedef int64_t (*part_bigint_t)(Calendar *calendar, const uint64_t micros);
	typedef double (*part_double_t)(Calendar *calendar, const uint64_t micros);

	// Date part adapters
	static int64_t ExtractEra(Calendar *calendar, const uint64_t micros) {
		return ExtractField(calendar, CAL_ERA);
	}

	static int64_t ExtractYear(Calendar *calendar, const uint64_t micros) {
		return ExtractField(calendar, CAL_YEAR);
	}

	static int64_t ExtractDecade(Calendar *calendar, const uint64_t micros) {
		return ExtractYear(calendar, micros) / 10;
	}

	static int64_t ExtractCentury(Calendar *calendar, const uint64_t micros) {
		const auto era = ExtractEra(calendar, micros);
		const auto cccc = ((ExtractYear(calendar, micros) - 1) / 100) + 1;
		return era > 0 ? cccc : -cccc;
	}

	static int64_t ExtractMillenium(Calendar *calendar, const uint64_t micros) {
		const auto era = ExtractEra(calendar, micros);
		const auto mmmm = ((ExtractYear(calendar, micros) - 1) / 1000) + 1;
		return era > 0 ? mmmm : -mmmm;
	}

	static int64_t ExtractMonth(Calendar *calendar, const uint64_t micros) {
		return ExtractField(calendar, CAL_MONTH) + 1;
	}

	static int64_t ExtractQuarter(Calendar *calendar, const uint64_t micros) {
		return ExtractField(calendar, CAL_MONTH) / Interval::MONTHS_PER_QUARTER + 1;
	}

	static int64_t ExtractDay(Calendar *calendar, const uint64_t micros) {
		return ExtractField(calendar, CAL_DATE);
	}

	static int64_t ExtractDayOfWeek(Calendar *calendar, const uint64_t micros) {
		// [Sun(0), Sat(6)]
		return ExtractField(calendar, CAL_DAY_OF_WEEK) - CAL_SUNDAY;
	}

	static int64_t ExtractISODayOfWeek(Calendar *calendar, const uint64_t micros) {
		// [Mon(1), Sun(7)]
		return 1 + (ExtractField(calendar, CAL_DAY_OF_WEEK) + 7 - CAL_MONDAY) % 7;
	}

	static int64_t ExtractWeek(Calendar *calendar, const uint64_t micros) {
		calendar->SetFirstDayOfWeek(CAL_MONDAY);
		calendar->SetMinimalDaysInFirstWeek(4);
		return ExtractField(calendar, CAL_WEEK_OF_YEAR);
	}

	static int64_t ExtractISOYear(Calendar *calendar, const uint64_t micros) {
		calendar->SetFirstDayOfWeek(CAL_MONDAY);
		calendar->SetMinimalDaysInFirstWeek(4);
		return ExtractField(calendar, CAL_YEAR_WOY);
	}

	static int64_t ExtractYearWeek(Calendar *calendar, const uint64_t micros) {
		calendar->SetFirstDayOfWeek(CAL_MONDAY);
		calendar->SetMinimalDaysInFirstWeek(4);
		const auto iyyy = ExtractField(calendar, CAL_YEAR_WOY);
		const auto ww = ExtractField(calendar, CAL_WEEK_OF_YEAR);
		return iyyy * 100 + ((iyyy > 0) ? ww : -ww);
	}

	static int64_t ExtractDayOfYear(Calendar *calendar, const uint64_t micros) {
		return ExtractField(calendar, CAL_DAY_OF_YEAR);
	}

	static int64_t ExtractHour(Calendar *calendar, const uint64_t micros) {
		return ExtractField(calendar, CAL_HOUR_OF_DAY);
	}

	static int64_t ExtractMinute(Calendar *calendar, const uint64_t micros) {
		return ExtractField(calendar, CAL_MINUTE);
	}

	static int64_t ExtractSecond(Calendar *calendar, const uint64_t micros) {
		return ExtractField(calendar, CAL_SECOND);
	}

	static int64_t ExtractMillisecond(Calendar *calendar, const uint64_t micros) {
		return ExtractSecond(calendar, micros) * Interval::MSECS_PER_SEC + ExtractField(calendar, CAL_MILLISECOND);
	}

	static int64_t ExtractMicrosecond(Calendar *calendar, const uint64_t micros) {
		return ExtractMillisecond(calendar, micros) * Interval::MICROS_PER_MSEC + NumericCast<int64_t>(micros);
	}

	static double ExtractEpoch(Calendar *calendar, const uint64_t micros) {
		// the milliseconds carry the fraction of a second, so the division has to keep it
		auto result = double(calendar->GetTime()) / double(Interval::MSECS_PER_SEC);
		result += static_cast<double>(micros) / double(Interval::MICROS_PER_SEC);
		return result;
	}

	static int64_t ExtractTimezone(Calendar *calendar, const uint64_t micros) {
		auto millis = ExtractField(calendar, CAL_ZONE_OFFSET);
		millis += ExtractField(calendar, CAL_DST_OFFSET);
		return millis / Interval::MSECS_PER_SEC;
	}

	static int64_t ExtractTimezoneHour(Calendar *calendar, const uint64_t micros) {
		auto secs = ExtractTimezone(calendar, micros);
		return secs / Interval::SECS_PER_HOUR;
	}

	static int64_t ExtractTimezoneMinute(Calendar *calendar, const uint64_t micros) {
		auto secs = ExtractTimezone(calendar, micros);
		return (secs % Interval::SECS_PER_HOUR) / Interval::SECS_PER_MINUTE;
	}

	//	PG uses doubles for JDs so we can only use them with other double types
	static double ExtractJulianDay(Calendar *calendar, const uint64_t micros) {
		//	We need days + fraction
		auto days = ExtractField(calendar, CAL_JULIAN_DAY);
		auto frac = ExtractHour(calendar, micros);

		frac *= Interval::MINS_PER_HOUR;
		frac += ExtractMinute(calendar, micros);

		frac *= Interval::MICROS_PER_MINUTE;
		frac += ExtractMicrosecond(calendar, micros);

		double result = static_cast<double>(frac);
		result /= Interval::MICROS_PER_DAY;
		result += days;

		return result;
	}

	static part_bigint_t PartCodeBigintFactory(DatePartSpecifier part) {
		switch (part) {
		case DatePartSpecifier::YEAR:
			return ExtractYear;
		case DatePartSpecifier::MONTH:
			return ExtractMonth;
		case DatePartSpecifier::DAY:
			return ExtractDay;
		case DatePartSpecifier::DECADE:
			return ExtractDecade;
		case DatePartSpecifier::CENTURY:
			return ExtractCentury;
		case DatePartSpecifier::MILLENNIUM:
			return ExtractMillenium;
		case DatePartSpecifier::MICROSECONDS:
			return ExtractMicrosecond;
		case DatePartSpecifier::MILLISECONDS:
			return ExtractMillisecond;
		case DatePartSpecifier::SECOND:
			return ExtractSecond;
		case DatePartSpecifier::MINUTE:
			return ExtractMinute;
		case DatePartSpecifier::HOUR:
			return ExtractHour;
		case DatePartSpecifier::DOW:
			return ExtractDayOfWeek;
		case DatePartSpecifier::ISODOW:
			return ExtractISODayOfWeek;
		case DatePartSpecifier::WEEK:
			return ExtractWeek;
		case DatePartSpecifier::ISOYEAR:
			return ExtractISOYear;
		case DatePartSpecifier::DOY:
			return ExtractDayOfYear;
		case DatePartSpecifier::QUARTER:
			return ExtractQuarter;
		case DatePartSpecifier::YEARWEEK:
			return ExtractYearWeek;
		case DatePartSpecifier::ERA:
			return ExtractEra;
		case DatePartSpecifier::TIMEZONE:
			return ExtractTimezone;
		case DatePartSpecifier::TIMEZONE_HOUR:
			return ExtractTimezoneHour;
		case DatePartSpecifier::TIMEZONE_MINUTE:
			return ExtractTimezoneMinute;
		default:
			throw InternalException("Unsupported ICU BIGINT extractor");
		}
	}

	static part_double_t PartCodeDoubleFactory(DatePartSpecifier part) {
		switch (part) {
		case DatePartSpecifier::EPOCH:
			return ExtractEpoch;
		case DatePartSpecifier::JULIAN_DAY:
			return ExtractJulianDay;
		default:
			throw InternalException("Unsupported ICU DOUBLE extractor");
		}
	}

	static date_t MakeLastDay(Calendar *calendar, const uint64_t micros) {
		// Set the calendar to midnight on the last day of the month
		calendar->Set(CAL_MILLISECOND, 0);
		calendar->Set(CAL_SECOND, 0);
		calendar->Set(CAL_MINUTE, 0);
		calendar->Set(CAL_HOUR_OF_DAY, 0);

		const auto dd = calendar->GetActualMaximum(CAL_DATE);
		if (calendar->HasFailed()) {
			throw InternalException("Unable to extract last day.");
		}

		calendar->Set(CAL_DATE, dd);

		//	Offset to UTC
		auto millis = calendar->GetTime();
		millis += ExtractField(calendar, CAL_ZONE_OFFSET);
		millis += ExtractField(calendar, CAL_DST_OFFSET);

		return Date::EpochToDate(millis / Interval::MSECS_PER_SEC);
	}

	static string_t MonthName(Calendar *calendar, const uint64_t micros) {
		const auto mm = ExtractMonth(calendar, micros) - 1;
		if (mm == 12) {
			return "Undecimber";
		}
		return Date::MONTH_NAMES[mm];
	}

	static string_t DayName(Calendar *calendar, const uint64_t micros) {
		return Date::DAY_NAMES[ExtractDayOfWeek(calendar, micros)];
	}

	template <typename RESULT_TYPE>
	struct BindAdapterData : public BindData {
		using result_t = RESULT_TYPE;
		typedef result_t (*adapter_t)(Calendar *calendar, const uint64_t micros);
		using adapters_t = vector<adapter_t>;

		BindAdapterData(ClientContext &context, adapter_t adapter_p) : BindData(context), adapters(1, adapter_p) {
		}
		BindAdapterData(ClientContext &context, adapters_t &adapters_p) : BindData(context), adapters(adapters_p) {
		}
		BindAdapterData(const BindAdapterData &other) : BindData(other), adapters(other.adapters) {
		}

		adapters_t adapters;

		bool Equals(const FunctionData &other_p) const override {
			const auto &other = other_p.Cast<BindAdapterData>();
			return BindData::Equals(other_p) && adapters == other.adapters;
		}

		duckdb::unique_ptr<FunctionData> Copy() const override {
			return make_uniq<BindAdapterData>(*this);
		}
	};

	template <typename INPUT_TYPE, typename RESULT_TYPE>
	static void UnaryTimestampFunction(DataChunk &args, ExpressionState &state, Vector &result) {
		using BIND_TYPE = BindAdapterData<RESULT_TYPE>;
		D_ASSERT(args.ColumnCount() == 1);
		const auto &date_arg = args.data[0];

		auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
		auto &info = func_expr.BindInfo()->Cast<BIND_TYPE>();
		CalendarPtr calendar_ptr(info.calendar->Copy());
		auto calendar = calendar_ptr.get();

		UnaryExecutor::Execute<INPUT_TYPE, RESULT_TYPE>(
		    date_arg, result, [&](INPUT_TYPE input) -> optional<RESULT_TYPE> {
			    if (input.IsFinite()) {
				    const auto micros = SetTime(calendar, TruncCast<INPUT_TYPE, timestamp_tz_t>(input));
				    return info.adapters[0](calendar, micros);
			    } else {
				    return nullopt;
			    }
		    });
	}

	template <typename INPUT_TYPE, typename RESULT_TYPE>
	static void BinaryTimestampFunction(DataChunk &args, ExpressionState &state, Vector &result) {
		using BIND_TYPE = BindAdapterData<RESULT_TYPE>;
		D_ASSERT(args.ColumnCount() == 2);
		const auto &part_arg = args.data[0];
		const auto &date_arg = args.data[1];

		auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
		auto &info = func_expr.BindInfo()->Cast<BIND_TYPE>();
		CalendarPtr calendar_ptr(info.calendar->Copy());
		auto calendar = calendar_ptr.get();

		BinaryExecutor::Execute<string_t, INPUT_TYPE, RESULT_TYPE>(
		    part_arg, date_arg, result, [&](string_t specifier, INPUT_TYPE input) -> optional<RESULT_TYPE> {
			    if (input.IsFinite()) {
				    const auto micros = SetTime(calendar, TruncCast<INPUT_TYPE, timestamp_tz_t>(input));
				    const auto part_code = GetDatePartSpecifier(specifier.GetString());
				    if (IsBigintDatepart(part_code)) {
					    auto adapter = PartCodeBigintFactory(GetDatePartSpecifier(specifier.GetString()));
					    return adapter(calendar, micros);
				    } else {
					    auto adapter = PartCodeDoubleFactory(GetDatePartSpecifier(specifier.GetString()));
					    return adapter(calendar, micros);
				    }
			    } else {
				    return nullopt;
			    }
		    });
	}

	struct BindStructData : public BindData {
		using part_codes_t = vector<DatePartSpecifier>;
		using bigints_t = vector<part_bigint_t>;
		using doubles_t = vector<part_double_t>;

		BindStructData(ClientContext &context, part_codes_t part_codes_p)
		    : BindData(context), part_codes(std::move(part_codes_p)) {
			InitFactories();
		}
		BindStructData(const string &tz_setting_p, const string &cal_setting_p, part_codes_t part_codes_p)
		    : BindData(tz_setting_p, cal_setting_p), part_codes(std::move(part_codes_p)) {
			InitFactories();
		}
		BindStructData(const BindStructData &other)
		    : BindData(other), part_codes(other.part_codes), bigints(other.bigints), doubles(other.doubles) {
		}

		part_codes_t part_codes;
		bigints_t bigints;
		doubles_t doubles;

		bool Equals(const FunctionData &other_p) const override {
			const auto &other = other_p.Cast<BindStructData>();
			return BindData::Equals(other_p) && part_codes == other.part_codes;
		}

		duckdb::unique_ptr<FunctionData> Copy() const override {
			return make_uniq<BindStructData>(*this);
		}

		void InitFactories() {
			bigints.clear();
			bigints.resize(part_codes.size(), nullptr);
			doubles.clear();
			doubles.resize(part_codes.size(), nullptr);
			for (size_t col = 0; col < part_codes.size(); ++col) {
				const auto part_code = part_codes[col];
				if (IsBigintDatepart(part_code)) {
					bigints[col] = PartCodeBigintFactory(part_code);
				} else {
					doubles[col] = PartCodeDoubleFactory(part_code);
				}
			}
		}
	};

	template <typename INPUT_TYPE>
	static void StructFunction(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
		auto &info = func_expr.BindInfo()->Cast<BindStructData>();
		CalendarPtr calendar_ptr(info.calendar->Copy());
		auto calendar = calendar_ptr.get();

		// the part list is folded into the bind data during the bind - only the trailing temporal argument is read.
		// plans written by versions that erased the part list have a single argument
		D_ASSERT(args.ColumnCount() == 1 || args.ColumnCount() == 2);
		const auto count = args.size();
		const Vector &input = args.data[args.ColumnCount() - 1];
		auto entries = input.Values<INPUT_TYPE>();

		result.SetVectorType(VectorType::FLAT_VECTOR);
		auto &child_entries = StructVector::GetEntries(result);
		for (auto &child_entry : child_entries) {
			child_entry.SetVectorType(VectorType::FLAT_VECTOR);
		}

		auto &res_valid = FlatVector::ValidityMutable(result);
		for (idx_t i = 0; i < count; ++i) {
			auto entry = entries[i];
			if (entry.IsValid()) {
				res_valid.SetValid(i);
				auto micros = SetTime(calendar, TruncCast<INPUT_TYPE, timestamp_tz_t>(entry.GetValue()));
				const auto is_finite = entry.GetValue().IsFinite();
				for (size_t col = 0; col < child_entries.size(); ++col) {
					auto &child_entry = child_entries[col];
					if (is_finite) {
						FlatVector::ValidityMutable(child_entry).SetValid(i);
						if (IsBigintDatepart(info.part_codes[col])) {
							auto pdata = FlatVector::GetDataMutable<int64_t>(child_entry);
							auto adapter = info.bigints[col];
							pdata[i] = adapter(calendar, micros);
						} else {
							auto pdata = FlatVector::GetDataMutable<double>(child_entry);
							auto adapter = info.doubles[col];
							pdata[i] = adapter(calendar, micros);
						}
					} else {
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

		result.Verify();
	}

	template <typename BIND_TYPE>
	static duckdb::unique_ptr<FunctionData> BindAdapter(ClientContext &context, BoundScalarFunction &bound_function,
	                                                    vector<duckdb::unique_ptr<Expression>> &arguments,
	                                                    typename BIND_TYPE::adapter_t adapter) {
		return make_uniq<BIND_TYPE>(context, adapter);
	}

	static duckdb::unique_ptr<FunctionData> BindUnaryDatePart(BindScalarFunctionInput &input) {
		auto &bound_function = input.GetBoundFunction();
		auto &context = input.GetClientContext();
		auto &arguments = input.GetArguments();

		const auto part_code = GetDatePartSpecifier(bound_function.GetName().GetIdentifierName());
		if (IsBigintDatepart(part_code)) {
			using data_t = BindAdapterData<int64_t>;
			auto adapter = PartCodeBigintFactory(part_code);
			return BindAdapter<data_t>(context, bound_function, arguments, adapter);
		} else {
			using data_t = BindAdapterData<double>;
			auto adapter = PartCodeDoubleFactory(part_code);
			return BindAdapter<data_t>(context, bound_function, arguments, adapter);
		}
	}

	static duckdb::unique_ptr<FunctionData> BindBinaryDatePart(BindScalarFunctionInput &input) {
		auto &bound_function = input.GetBoundFunction();
		auto &context = input.GetClientContext();
		auto &arguments = input.GetArguments();

		// Bind the equivalent unary function for a constant first argument.
		do {
			auto part_constant = input.TryGetConstant(0);
			if (!part_constant || part_constant->IsNull()) {
				break;
			}

			const auto part_name = part_constant->ToString();
			const auto part_code = GetDatePartSpecifier(part_name);
			arguments.erase(arguments.begin());
			auto unary_name = bound_function.GetDefinition()->GetQualifiedName().WithName(
			    Identifier(DatePartUnaryFunctionName(part_code)));
			auto &entry = Catalog::GetEntry<ScalarFunctionCatalogEntry>(context, unary_name);
			auto unary_function = entry.functions.GetFunctionByArguments(context, {arguments[0]->GetReturnType()});
			FunctionBinder binder(context);
			auto unary_expression = binder.BindScalarFunction(unary_function, std::move(arguments));
			auto &unary = unary_expression->Cast<BoundFunctionExpression>();
			bound_function = std::move(unary.FunctionMutable());
			arguments = std::move(unary.GetChildrenMutable());
			return std::move(unary.BindInfoMutable());
		} while (false);

		using data_t = BindAdapterData<double>;
		return BindAdapter<data_t>(context, bound_function, arguments, nullptr);
	}

	//! The epoch is read from the instant itself, every other part from the calendar of the settings
	static FunctionStability PartCodeStability(DatePartSpecifier part_code) {
		return part_code == DatePartSpecifier::EPOCH ? FunctionStability::CONSISTENT
		                                             : FunctionStability::CONSISTENT_WITHIN_QUERY;
	}

	static void SetPartCodesStability(BoundScalarFunction &bound_function,
	                                  const vector<DatePartSpecifier> &part_codes) {
		for (auto part_code : part_codes) {
			if (PartCodeStability(part_code) != FunctionStability::CONSISTENT) {
				return;
			}
		}
		bound_function.SetStability(FunctionStability::CONSISTENT);
	}

	static duckdb::unique_ptr<FunctionData> BindStruct(BindScalarFunctionInput &input) {
		auto &bound_function = input.GetBoundFunction();
		auto &context = input.GetClientContext();
		auto &arguments = input.GetArguments();

		// collect names and deconflict, construct return type
		auto parts_list = input.GetConstant(0);

		case_insensitive_set_t name_collision_set;
		child_list_t<LogicalType> struct_children;
		BindStructData::part_codes_t part_codes;
		if (parts_list.type().id() == LogicalTypeId::LIST) {
			auto &list_children = ListValue::GetChildren(parts_list);
			if (list_children.empty()) {
				throw BinderException("%s requires non-empty lists of part names", bound_function.GetName());
			}

			for (size_t col = 0; col < list_children.size(); ++col) {
				const auto &part_value = list_children[col];
				if (part_value.IsNull()) {
					throw BinderException("NULL struct entry name in %s", bound_function.GetName());
				}
				const auto part_name = part_value.ToString();
				const auto part_code = GetDatePartSpecifier(part_name);
				if (name_collision_set.find(part_name) != name_collision_set.end()) {
					throw BinderException("Duplicate struct entry name \"%s\" in %s", part_name,
					                      bound_function.GetName());
				}
				name_collision_set.insert(part_name);
				part_codes.emplace_back(part_code);
				if (IsBigintDatepart(part_code)) {
					struct_children.emplace_back(make_pair(part_name, LogicalType::BIGINT));
				} else {
					struct_children.emplace_back(make_pair(part_name, LogicalType::DOUBLE));
				}
			}
		} else {
			throw BinderException("%s can only take constant lists of part names", bound_function.GetName());
		}

		bound_function.SetReturnType(LogicalType::STRUCT(std::move(struct_children)));
		SetPartCodesStability(bound_function, part_codes);
		return make_uniq<BindStructData>(context, std::move(part_codes));
	}

	static void SerializeStructFunction(Serializer &serializer, const optional_ptr<FunctionData> bind_data,
	                                    const BoundScalarFunction &function) {
		D_ASSERT(bind_data);
		auto &info = bind_data->Cast<BindStructData>();
		serializer.WriteProperty(100, "tz_setting", info.tz_setting);
		serializer.WriteProperty(101, "cal_setting", info.cal_setting);
		serializer.WriteProperty(102, "part_codes", info.part_codes);
	}

	static duckdb::unique_ptr<FunctionData> DeserializeStructFunction(Deserializer &deserializer,
	                                                                  BoundScalarFunction &bound_function) {
		auto tz_setting = deserializer.ReadProperty<string>(100, "tz_setting");
		auto cal_setting = deserializer.ReadProperty<string>(101, "cal_setting");
		auto part_codes = deserializer.ReadProperty<vector<DatePartSpecifier>>(102, "part_codes");
		SetPartCodesStability(bound_function, part_codes);
		return make_uniq<BindStructData>(tz_setting, cal_setting, std::move(part_codes));
	}

	template <typename INPUT_TYPE, typename RESULT_TYPE>
	static ScalarFunction GetUnaryPartCodeFunction(const LogicalType &temporal_type,
	                                               const LogicalType &result_type = LogicalType::BIGINT) {
		ScalarFunction fun({}, result_type, UnaryTimestampFunction<INPUT_TYPE, RESULT_TYPE>, BindUnaryDatePart);
		fun.GetSignature().AddParameter("ts", temporal_type);
		return fun;
	}

	template <typename RESULT_TYPE = int64_t>
	static void AddUnaryPartCodeFunctions(const Identifier &name, ExtensionLoader &loader,
	                                      const LogicalType &result_type = LogicalType::BIGINT,
	                                      ArgProperties unary_arg0_props = {}) {
		ScalarFunctionSet set {name};
		set.AddFunction(GetUnaryPartCodeFunction<timestamp_tz_t, RESULT_TYPE>(LogicalType::TIMESTAMP_TZ, result_type));
		set.AddFunction(
		    GetUnaryPartCodeFunction<timestamp_tz_ns_t, RESULT_TYPE>(LogicalType::TIMESTAMP_TZ_NS, result_type));
		set.SetFallible();
		set.SetStability(PartCodeStability(GetDatePartSpecifier(name.GetIdentifierName())));
		set.SetUnaryArgProperties(unary_arg0_props);
		if (IsKnownMonotonic(unary_arg0_props.monotonicity)) {
			set.SetStatisticsCallback(StatisticsPropagator::PropagateFiniteMonotoneBounds);
		}
		loader.RegisterFunction(set);
	}

	template <typename INPUT_TYPE, typename RESULT_TYPE>
	static ScalarFunction GetBinaryPartCodeFunction(const LogicalType &temporal_type) {
		ScalarFunction fun({}, LogicalType::DOUBLE, BinaryTimestampFunction<INPUT_TYPE, RESULT_TYPE>,
		                   BindBinaryDatePart);
		fun.GetSignature().AddParameter("part", LogicalType::VARCHAR).AddParameter("ts", temporal_type);
		return fun;
	}

	template <typename INPUT_TYPE>
	static ScalarFunction GetStructFunction(const LogicalType &temporal_type) {
		auto part_type = LogicalType::LIST(LogicalType::VARCHAR);
		auto result_type = LogicalType::STRUCT({});
		ScalarFunction result({}, result_type, StructFunction<INPUT_TYPE>, BindStruct);
		result.GetSignature().AddParameter("part_list", part_type).AddParameter("ts", temporal_type);
		result.SetSerializeCallback(SerializeStructFunction);
		result.SetDeserializeCallback(DeserializeStructFunction);
		return result;
	}

	static void AddDatePartFunctions(const Identifier &name, ExtensionLoader &loader) {
		ScalarFunctionSet set {name};
		set.AddFunction(GetBinaryPartCodeFunction<timestamp_tz_t, double>(LogicalType::TIMESTAMP_TZ));
		set.AddFunction(GetStructFunction<timestamp_tz_t>(LogicalType::TIMESTAMP_TZ));
		set.AddFunction(GetBinaryPartCodeFunction<timestamp_tz_ns_t, double>(LogicalType::TIMESTAMP_TZ_NS));
		set.AddFunction(GetStructFunction<timestamp_tz_ns_t>(LogicalType::TIMESTAMP_TZ_NS));
		set.SetFallible();
		set.SetStability(FunctionStability::CONSISTENT_WITHIN_QUERY);
		loader.RegisterFunction(set);
	}

	static duckdb::unique_ptr<FunctionData> BindLastDate(BindScalarFunctionInput &input) {
		auto &bound_function = input.GetBoundFunction();
		auto &context = input.GetClientContext();
		auto &arguments = input.GetArguments();

		using data_t = BindAdapterData<date_t>;
		return BindAdapter<data_t>(context, bound_function, arguments, MakeLastDay);
	}

	template <typename INPUT_TYPE>
	static ScalarFunction GetLastDayFunction(const LogicalType &temporal_type) {
		ScalarFunction fun({}, LogicalType::DATE, UnaryTimestampFunction<INPUT_TYPE, date_t>, BindLastDate);
		fun.GetSignature().AddParameter("ts", temporal_type);
		return fun;
	}
	static void AddLastDayFunctions(const Identifier &name, ExtensionLoader &loader) {
		ScalarFunctionSet set {name};
		set.AddFunction(GetLastDayFunction<timestamp_tz_t>(LogicalType::TIMESTAMP_TZ));
		set.AddFunction(GetLastDayFunction<timestamp_tz_ns_t>(LogicalType::TIMESTAMP_TZ_NS));
		set.SetFallible();
		set.SetStability(FunctionStability::CONSISTENT_WITHIN_QUERY);
		loader.RegisterFunction(set);
	}

	static unique_ptr<FunctionData> BindMonthName(BindScalarFunctionInput &input) {
		auto &context = input.GetClientContext();
		auto &bound_function = input.GetBoundFunction();
		auto &arguments = input.GetArguments();
		using data_t = BindAdapterData<string_t>;
		return BindAdapter<data_t>(context, bound_function, arguments, MonthName);
	}

	template <typename INPUT_TYPE>
	static ScalarFunction GetMonthNameFunction(const LogicalType &temporal_type) {
		ScalarFunction fun({}, LogicalType::VARCHAR, UnaryTimestampFunction<INPUT_TYPE, string_t>, BindMonthName);
		fun.GetSignature().AddParameter("ts", temporal_type);
		return fun;
	}
	static void AddMonthNameFunctions(const Identifier &name, ExtensionLoader &loader) {
		ScalarFunctionSet set {name};
		set.AddFunction(GetMonthNameFunction<timestamp_tz_t>(LogicalType::TIMESTAMP_TZ));
		set.AddFunction(GetMonthNameFunction<timestamp_tz_ns_t>(LogicalType::TIMESTAMP_TZ_NS));
		set.SetFallible();
		set.SetStability(FunctionStability::CONSISTENT_WITHIN_QUERY);
		loader.RegisterFunction(set);
	}

	static unique_ptr<FunctionData> BindDayName(BindScalarFunctionInput &input) {
		auto &context = input.GetClientContext();
		auto &bound_function = input.GetBoundFunction();
		auto &arguments = input.GetArguments();
		using data_t = BindAdapterData<string_t>;
		return BindAdapter<data_t>(context, bound_function, arguments, DayName);
	}

	template <typename INPUT_TYPE>
	static ScalarFunction GetDayNameFunction(const LogicalType &temporal_type) {
		ScalarFunction fun({}, LogicalType::VARCHAR, UnaryTimestampFunction<INPUT_TYPE, string_t>, BindDayName);
		fun.GetSignature().AddParameter("ts", temporal_type);
		return fun;
	}
	static void AddDayNameFunctions(const Identifier &name, ExtensionLoader &loader) {
		ScalarFunctionSet set {name};
		set.AddFunction(GetDayNameFunction<timestamp_tz_t>(LogicalType::TIMESTAMP_TZ));
		set.AddFunction(GetDayNameFunction<timestamp_tz_ns_t>(LogicalType::TIMESTAMP_TZ_NS));
		set.SetFallible();
		set.SetStability(FunctionStability::CONSISTENT_WITHIN_QUERY);
		loader.RegisterFunction(set);
	}
};

void RegisterICUDatePartFunctions(ExtensionLoader &loader) {
	// register the individual operators

	// year/decade use CAL_YEAR (year-of-era, positive in both BC and AD), which is non-monotonic
	// across the BC/AD flip; leave them unannotated. century/millennium are negated in era 0, which is the only era
	// of some calendars, and yearweek decreases within negative years, so these are unannotated as well.

	//	BIGINTs
	ICUDatePart::AddUnaryPartCodeFunctions("era", loader, LogicalType::BIGINT, ArgProperties().NonDecreasing());
	ICUDatePart::AddUnaryPartCodeFunctions("year", loader);
	ICUDatePart::AddUnaryPartCodeFunctions("month", loader);
	ICUDatePart::AddUnaryPartCodeFunctions("day", loader);
	ICUDatePart::AddUnaryPartCodeFunctions("decade", loader);
	ICUDatePart::AddUnaryPartCodeFunctions("century", loader);
	ICUDatePart::AddUnaryPartCodeFunctions("millennium", loader);
	ICUDatePart::AddUnaryPartCodeFunctions("microsecond", loader);
	ICUDatePart::AddUnaryPartCodeFunctions("millisecond", loader);
	ICUDatePart::AddUnaryPartCodeFunctions("second", loader);
	ICUDatePart::AddUnaryPartCodeFunctions("minute", loader);
	ICUDatePart::AddUnaryPartCodeFunctions("hour", loader);
	ICUDatePart::AddUnaryPartCodeFunctions("dayofweek", loader);
	ICUDatePart::AddUnaryPartCodeFunctions("isodow", loader);
	ICUDatePart::AddUnaryPartCodeFunctions("week", loader); //  Note that WeekOperator is ISO-8601, not US
	ICUDatePart::AddUnaryPartCodeFunctions("dayofyear", loader);
	ICUDatePart::AddUnaryPartCodeFunctions("quarter", loader);
	ICUDatePart::AddUnaryPartCodeFunctions("isoyear", loader, LogicalType::BIGINT, ArgProperties().NonDecreasing());
	ICUDatePart::AddUnaryPartCodeFunctions("timezone", loader);
	ICUDatePart::AddUnaryPartCodeFunctions("timezone_hour", loader);
	ICUDatePart::AddUnaryPartCodeFunctions("timezone_minute", loader);

	//	DOUBLEs
	ICUDatePart::AddUnaryPartCodeFunctions<double>("epoch", loader, LogicalType::DOUBLE);
	ICUDatePart::AddUnaryPartCodeFunctions<double>("julian", loader, LogicalType::DOUBLE);

	//  register combinations
	// yearweek subtracts the week for ISO year <= 0, so it is not monotone across that range
	ICUDatePart::AddUnaryPartCodeFunctions("yearweek", loader, LogicalType::BIGINT);

	//  register various aliases
	ICUDatePart::AddUnaryPartCodeFunctions("dayofmonth", loader);
	ICUDatePart::AddUnaryPartCodeFunctions("weekday", loader);
	ICUDatePart::AddUnaryPartCodeFunctions("weekofyear", loader);

	//  register the last_day function
	ICUDatePart::AddLastDayFunctions("last_day", loader);

	// register the dayname/monthname functions
	ICUDatePart::AddMonthNameFunctions("monthname", loader);
	ICUDatePart::AddDayNameFunctions("dayname", loader);

	// finally the actual date_part function
	ICUDatePart::AddDatePartFunctions("date_part", loader);
	ICUDatePart::AddDatePartFunctions("datepart", loader);
}

} // namespace duckdb
