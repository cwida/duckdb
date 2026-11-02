#include "include/icu-strptime.hpp"
#include "include/icu-datefunc.hpp"
#include "include/icu-helpers.hpp"

#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/types/cast_helpers.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/scalar/strftime_format.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

TimestampComponents ICUHelpers::GetComponents(timestamp_tz_t ts, Calendar *calendar) {
	// Get the parts in the given time zone
	uint64_t micros = ICUDateFunc::SetTime(calendar, ts);

	TimestampComponents ts_data;
	ts_data.year = ICUDateFunc::ExtractField(calendar, CAL_EXTENDED_YEAR);
	ts_data.month = ICUDateFunc::ExtractField(calendar, CAL_MONTH) + 1;
	ts_data.day = ICUDateFunc::ExtractField(calendar, CAL_DATE);

	ts_data.hour = ICUDateFunc::ExtractField(calendar, CAL_HOUR_OF_DAY);
	ts_data.minute = ICUDateFunc::ExtractField(calendar, CAL_MINUTE);
	ts_data.second = ICUDateFunc::ExtractField(calendar, CAL_SECOND);
	ts_data.microsecond = UnsafeNumericCast<int32_t>(
	    ICUDateFunc::ExtractField(calendar, CAL_MILLISECOND) * Interval::MICROS_PER_MSEC + micros);
	ts_data.nanosecond = 0;
	return ts_data;
}

TimestampComponents ICUHelpers::GetComponents(timestamp_tz_ns_t tsns, Calendar *calendar) {
	// Get the parts in the given time zone
	int64_t micros;
	int64_t nanos;
	Timestamp::SplitTime(tsns.value, Interval::NANOS_PER_MICRO, micros, nanos);

	auto ts_data = GetComponents(timestamp_tz_t(micros), calendar);
	ts_data.nanosecond = UnsafeNumericCast<int16_t>(nanos);
	return ts_data;
}

timestamp_t ICUHelpers::ToTimestamp(TimestampComponents data) {
	date_t date_val = Date::FromDate(data.year, data.month, data.day);
	dtime_t time_val = Time::FromTime(data.hour, data.minute, data.second, data.microsecond);
	return Timestamp::FromDatetime(date_val, time_val);
}
struct ICUStrptime : public ICUDateFunc {
	using ParseResult = StrpTimeFormat::ParseResult;

	struct ICUStrptimeBindData : public BindData {
		ICUStrptimeBindData(ClientContext &context, const StrpTimeFormat &format)
		    : BindData(context), formats(1, format) {
		}
		ICUStrptimeBindData(ClientContext &context, vector<StrpTimeFormat> formats_p)
		    : BindData(context), formats(std::move(formats_p)) {
		}
		ICUStrptimeBindData(const ICUStrptimeBindData &other) : BindData(other), formats(other.formats) {
		}

		vector<StrpTimeFormat> formats;

		bool Equals(const FunctionData &other_p) const override {
			auto &other = other_p.Cast<ICUStrptimeBindData>();
			if (formats.size() != other.formats.size()) {
				return false;
			}
			for (size_t i = 0; i < formats.size(); ++i) {
				if (formats[i].format_specifier != other.formats[i].format_specifier) {
					return false;
				}
			}
			return true;
		}
		duckdb::unique_ptr<FunctionData> Copy() const override {
			return make_uniq<ICUStrptimeBindData>(*this);
		}
	};

	static void ParseFormatSpecifier(string_t &format_specifier, StrpTimeFormat &format) {
		format.format_specifier = format_specifier.GetString();
		const auto error = StrTimeFormat::ParseFormatSpecifier(format.format_specifier, format);
		if (!error.empty()) {
			throw InvalidInputException("Failed to parse format specifier %s: %s", format.format_specifier, error);
		}
	}

	static uint64_t ToMicros(Calendar *calendar, const ParseResult &parsed, const StrpTimeFormat &format) {
		// Get the parts in the current time zone
		uint64_t micros = parsed.GetMicros();
		calendar->Set(CAL_EXTENDED_YEAR, parsed.data[0]); // strptime doesn't understand eras
		calendar->Set(CAL_MONTH, parsed.data[1] - 1);
		calendar->Set(CAL_DATE, parsed.data[2]);
		calendar->Set(CAL_HOUR_OF_DAY, parsed.data[3]);
		calendar->Set(CAL_MINUTE, parsed.data[4]);
		calendar->Set(CAL_SECOND, parsed.data[5]);
		calendar->Set(CAL_MILLISECOND, UnsafeNumericCast<int32_t>(micros / Interval::MICROS_PER_MSEC));
		micros %= Interval::MICROS_PER_MSEC;

		// This overrides the TZ setting, so only use it if an offset was parsed.
		// Note that we don't bother/worry about the DST setting because the two just combine.
		if (format.HasFormatSpecifier(StrTimeSpecifier::UTC_OFFSET)) {
			calendar->Set(CAL_ZONE_OFFSET, UnsafeNumericCast<int32_t>(parsed.data[7] * Interval::MSECS_PER_SEC));
		}

		return micros;
	}

	static uint64_t ToNanos(Calendar *calendar, const ParseResult &parsed, const StrpTimeFormat &format) {
		// Get the parts in the current time zone
		uint64_t nanos = parsed.data[6];
		calendar->Set(CAL_EXTENDED_YEAR, parsed.data[0]); // strptime doesn't understand eras
		calendar->Set(CAL_MONTH, parsed.data[1] - 1);
		calendar->Set(CAL_DATE, parsed.data[2]);
		calendar->Set(CAL_HOUR_OF_DAY, parsed.data[3]);
		calendar->Set(CAL_MINUTE, parsed.data[4]);
		calendar->Set(CAL_SECOND, parsed.data[5]);
		calendar->Set(CAL_MILLISECOND, UnsafeNumericCast<int32_t>(nanos / Interval::NANOS_PER_MSEC));
		nanos %= Interval::NANOS_PER_MSEC;

		// This overrides the TZ setting, so only use it if an offset was parsed.
		// Note that we don't bother/worry about the DST setting because the two just combine.
		if (format.HasFormatSpecifier(StrTimeSpecifier::UTC_OFFSET)) {
			calendar->Set(CAL_ZONE_OFFSET, UnsafeNumericCast<int32_t>(parsed.data[7] * Interval::MSECS_PER_SEC));
		}

		return nanos;
	}

	static inline void ParseOne(Calendar *calendar, CalendarCacheState &cache, string_t input,
	                            vector<StrpTimeFormat> &formats, timestamp_tz_t &result) {
		ParseResult parsed;
		for (auto &format : formats) {
			if (format.Parse(input, parsed)) {
				if (parsed.is_special) {
					result = timestamp_tz_t(parsed.ToTimestamp());
					return;
				} else {
					// Use the calendar of the parsed time zone, if any - the shared calendar keeps the session zone
					auto row_calendar = parsed.tz.empty() ? calendar : cache.GetCalendar(parsed.tz);
					result = GetTime(row_calendar, ToMicros(row_calendar, parsed, format));
					return;
				}
			}
		}

		throw InvalidInputException(parsed.FormatError(input, formats[0].format_specifier));
	}

	static inline void ParseOne(Calendar *calendar, CalendarCacheState &cache, string_t input,
	                            vector<StrpTimeFormat> &formats, timestamp_tz_ns_t &result) {
		ParseResult parsed;
		for (auto &format : formats) {
			if (format.Parse(input, parsed)) {
				if (parsed.is_special) {
					result.value = parsed.ToTimestamp().value;
					return;
				} else {
					// Use the calendar of the parsed time zone, if any - the shared calendar keeps the session zone
					auto row_calendar = parsed.tz.empty() ? calendar : cache.GetCalendar(parsed.tz);
					result = GetTimeNS(row_calendar, ToNanos(row_calendar, parsed, format));
					return;
				}
			}
		}

		throw InvalidInputException(parsed.FormatError(input, formats[0].format_specifier));
	}

	template <typename T>
	static void Parse(DataChunk &args, ExpressionState &state, Vector &result) {
		D_ASSERT(args.ColumnCount() == 2);
		const auto &str_arg = args.data[0];
		const auto &fmt_arg = args.data[1];

		auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
		auto &info = func_expr.BindInfo()->Cast<ICUStrptimeBindData>();
		auto &cache = ExecuteFunctionState::GetFunctionState(state)->Cast<CalendarCacheState>();
		CalendarPtr calendar_ptr(info.calendar->Copy());
		auto calendar = calendar_ptr.get();

		D_ASSERT(fmt_arg.GetVectorType() == VectorType::CONSTANT_VECTOR);
		UnaryExecutor::Execute<string_t, T>(str_arg, result, [&](string_t input) {
			T parsed;
			ParseOne(calendar, cache, input, info.formats, parsed);
			return parsed;
		});
	}

	static inline bool TryParseOne(Calendar *calendar, CalendarCacheState &cache, string_t input,
	                               vector<StrpTimeFormat> &formats, timestamp_tz_t &result) {
		ParseResult parsed;
		for (auto &format : formats) {
			if (format.Parse(input, parsed)) {
				if (parsed.is_special) {
					result.value = parsed.ToTimestamp().value;
					return true;
				} else {
					auto row_calendar = parsed.tz.empty() ? calendar : cache.TryGetCalendar(parsed.tz);
					if (row_calendar && TryGetTime(row_calendar, ToMicros(row_calendar, parsed, format), result)) {
						return true;
					}
				}
			}
		}

		return false;
	}

	static inline bool TryParseOne(Calendar *calendar, CalendarCacheState &cache, string_t input,
	                               vector<StrpTimeFormat> &formats, timestamp_tz_ns_t &result) {
		ParseResult parsed;
		for (auto &format : formats) {
			if (format.Parse(input, parsed)) {
				if (parsed.is_special) {
					result.value = parsed.ToTimestamp().value;
					return true;
				} else {
					auto row_calendar = parsed.tz.empty() ? calendar : cache.TryGetCalendar(parsed.tz);
					if (row_calendar && TryGetTimeNS(row_calendar, ToNanos(row_calendar, parsed, format), result)) {
						return true;
					}
				}
			}
		}

		return false;
	}

	template <typename T>
	static void TryParse(DataChunk &args, ExpressionState &state, Vector &result) {
		D_ASSERT(args.ColumnCount() == 2);
		const auto &str_arg = args.data[0];
		const auto &fmt_arg = args.data[1];

		auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
		auto &info = func_expr.BindInfo()->Cast<ICUStrptimeBindData>();
		auto &cache = ExecuteFunctionState::GetFunctionState(state)->Cast<CalendarCacheState>();
		CalendarPtr calendar_ptr(info.calendar->Copy());
		auto calendar = calendar_ptr.get();

		D_ASSERT(fmt_arg.GetVectorType() == VectorType::CONSTANT_VECTOR);

		if (ConstantVector::IsNull(fmt_arg)) {
			ConstantVector::SetNull(result, count_t(args.size()));
		} else {
			UnaryExecutor::Execute<string_t, T>(str_arg, result, [&](string_t input) -> optional<T> {
				T result;
				if (TryParseOne(calendar, cache, input, info.formats, result)) {
					return result;
				}
				return nullopt;
			});
		}
	}

	static bind_scalar_function_t bind_strptime; // NOLINT

	static duckdb::unique_ptr<FunctionData> StrpTimeBindFunction(BindScalarFunctionInput &input) {
		auto &context = input.GetClientContext();
		auto &bound_function = input.GetBoundFunction();
		auto &arguments = input.GetArguments();

		const bool is_try = (bound_function.GetName() == "try_strptime");
		scalar_function_t function = is_try ? TryParse<timestamp_tz_t> : Parse<timestamp_tz_t>;
		Value format_value = input.GetConstant(1);
		string format_string;
		StrpTimeFormat format;
		bool has_tz = false;
		bool has_ns = false;
		if (format_value.IsNull()) {
			;
		} else if (format_value.type().id() == LogicalTypeId::VARCHAR) {
			format_string = format_value.ToString();
			format.format_specifier = format_string;
			string error = StrTimeFormat::ParseFormatSpecifier(format_string, format);
			if (!error.empty()) {
				throw InvalidInputException("Failed to parse format specifier %s: %s", format_string, error);
			}

			// If we have a time zone, we should use ICU for parsing and return a TSTZ instead.
			has_tz = has_tz || format.HasFormatSpecifier(StrTimeSpecifier::TZ_NAME);
			has_ns = has_ns || format.HasFormatSpecifier(StrTimeSpecifier::NANOSECOND_PADDED);
			if (has_tz) {
				if (has_ns) {
					function = is_try ? TryParse<timestamp_tz_ns_t> : Parse<timestamp_tz_ns_t>;
				}
				bound_function.SetFunctionCallback(function);
				bound_function.SetInitStateCallback(InitCalendarCache);
				bound_function.SetReturnType(has_ns ? LogicalType::TIMESTAMP_TZ_NS : LogicalType::TIMESTAMP_TZ);
				bound_function.SetStability(FunctionStability::CONSISTENT_WITHIN_QUERY);
				return make_uniq<ICUStrptimeBindData>(context, format);
			}
		} else if (format_value.type() == LogicalType::LIST(LogicalType::VARCHAR)) {
			const auto &children = ListValue::GetChildren(format_value);
			if (children.empty()) {
				throw InvalidInputException("strptime format list must not be empty");
			}
			vector<StrpTimeFormat> formats;
			for (const auto &child : children) {
				format_string = child.ToString();
				format.format_specifier = format_string;
				string error = StrTimeFormat::ParseFormatSpecifier(format_string, format);
				if (!error.empty()) {
					throw InvalidInputException("Failed to parse format specifier %s: %s", format_string, error);
				}
				// If any format has UTC offsets or names, then we have to produce TSTZ
				has_tz = has_tz || format.HasFormatSpecifier(StrTimeSpecifier::TZ_NAME);
				has_tz = has_tz || format.HasFormatSpecifier(StrTimeSpecifier::UTC_OFFSET);
				has_ns = has_ns || format.HasFormatSpecifier(StrTimeSpecifier::NANOSECOND_PADDED);
				formats.emplace_back(format);
			}
			if (has_tz) {
				if (has_ns) {
					function = is_try ? TryParse<timestamp_tz_ns_t> : Parse<timestamp_tz_ns_t>;
				}
				bound_function.SetFunctionCallback(function);
				bound_function.SetInitStateCallback(InitCalendarCache);
				bound_function.SetReturnType(has_ns ? LogicalType::TIMESTAMP_TZ_NS : LogicalType::TIMESTAMP_TZ);
				bound_function.SetStability(FunctionStability::CONSISTENT_WITHIN_QUERY);
				return make_uniq<ICUStrptimeBindData>(context, formats);
			}
		}

		// Fall back to faster, non-TZ parsing
		bound_function.SetBindCallback(bind_strptime);
		BindScalarFunctionInput new_input(context, bound_function, arguments,
		                                  input.HasBinder() ? &input.GetBinder() : nullptr);
		return bound_function.GetBindCallback()(new_input);
	}

	static void TailPatch(const Identifier &name, ExtensionLoader &loader, const vector<LogicalType> &types) {
		// Find the old function
		auto &scalar_function = loader.GetFunction(name);
		auto &functions = scalar_function.functions.functions;
		optional_idx best_index;
		for (idx_t i = 0; i < functions.size(); i++) {
			const auto &sig = functions[i]->GetSignature();
			if (sig.GetParameterCount() != types.size()) {
				continue;
			}

			auto match = true;
			for (idx_t j = 0; j < sig.GetParameterCount(); j++) {
				if (sig.GetParameter(j).GetType() != types[j]) {
					match = false;
					break;
				}
			}
			if (!match) {
				continue;
			}

			best_index = i;
			break;
		}
		if (!best_index.IsValid()) {
			throw InternalException("ICU - Function for TailPatch not found");
		}
		// the overloads are immutable - swap in a patched copy
		auto patched = make_shared_ptr<ScalarFunction>(*functions[best_index.GetIndex()]);
		bind_strptime = patched->GetBindCallback();
		patched->SetBindCallback(StrpTimeBindFunction);
		functions[best_index.GetIndex()] = std::move(patched);
	}

	static void AddBinaryTimestampFunction(const Identifier &name, ExtensionLoader &loader) {
		vector<LogicalType> types {LogicalType::VARCHAR, LogicalType::VARCHAR};
		TailPatch(name, loader, types);

		types[1] = LogicalType::LIST(LogicalType::VARCHAR);
		TailPatch(name, loader, types);
	}

	static optional<timestamp_tz_t> VarcharToTimestampTZUS(CalendarPtr &cal, CalendarCacheState &cache, string_t input,
	                                                       CastParameters &parameters,
	                                                       optional_ptr<int32_t> nanos = nullptr) {
		timestamp_t us;
		const auto str = input.GetData();
		const auto len = input.GetSize();
		string_t tz(nullptr, 0);
		bool has_offset = false;
		auto success = Timestamp::TryConvertTimestampTZ(str, len, us, true, has_offset, tz, nanos);
		if (success != TimestampCastResult::SUCCESS) {
			string msg;
			if (success == TimestampCastResult::ERROR_RANGE) {
				msg = Timestamp::RangeError(string(str, len));
			} else {
				msg = Timestamp::FormatError(string(str, len));
			}
			HandleCastError::AssignError(msg, parameters);
			return nullopt;
		} else if (!has_offset) {
			// Convert parts to a TZ (default or parsed) if no offset was provided
			auto calendar = cal.get();

			// Use the calendar of the time zone if one was provided - the shared calendar keeps the session zone
			if (tz.GetSize()) {
				string error_msg;
				calendar = cache.TryGetCalendar(tz, &error_msg);
				if (!calendar) {
					HandleCastError::AssignError(error_msg, parameters);
					return nullopt;
				}
			}

			// Now get the parts in the given time zone
			timestamp_tz_t result;
			string error_msg;
			if (!TryFromNaive(calendar, us, result, error_msg)) {
				HandleCastError::AssignError(error_msg, parameters);
				return nullopt;
			}
			return result;
		}

		return timestamp_tz_t(us);
	}

	static bool VarcharToTimestampTZ(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
		auto &cast_data = parameters.cast_data->Cast<CastData>();
		auto &info = cast_data.info->Cast<BindData>();
		auto &cache = parameters.local_state->Cast<CalendarCacheState>();
		CalendarPtr cal(info.calendar->Copy());

		bool all_converted = true;
		UnaryExecutor::Execute<string_t, timestamp_tz_t>(source, result, count, [&](string_t input) {
			auto converted = VarcharToTimestampTZUS(cal, cache, input, parameters);
			all_converted = all_converted && converted;
			return converted;
		});
		return all_converted;
	}

	static bool VarcharToTimestampTZNS(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
		auto &cast_data = parameters.cast_data->Cast<CastData>();
		auto &info = cast_data.info->Cast<BindData>();
		auto &cache = parameters.local_state->Cast<CalendarCacheState>();
		CalendarPtr cal(info.calendar->Copy());

		bool all_converted = true;
		UnaryExecutor::Execute<string_t, timestamp_tz_ns_t>(
		    source, result, count, [&](string_t input) -> optional<timestamp_tz_ns_t> {
			    int32_t nanos = 0;
			    auto ts_us = VarcharToTimestampTZUS(cal, cache, input, parameters, &nanos);
			    if (!ts_us) {
				    all_converted = false;
				    return nullopt;
			    }

			    timestamp_t us(ts_us->value);
			    timestamp_ns_t result;
			    if (!Timestamp::TryFromTimestampNanos(us, nanos, result)) {
				    HandleCastError::AssignError(Timestamp::RangeError(input), parameters);
				    all_converted = false;
				    return nullopt;
			    }

			    return timestamp_tz_ns_t(result);
		    });
		return all_converted;
	}

	static bool VarcharToTimeTZ(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
		auto &cast_data = parameters.cast_data->Cast<CastData>();
		auto &info = cast_data.info->Cast<BindData>();
		CalendarPtr cal(info.calendar->Copy());

		bool all_converted = true;
		UnaryExecutor::Execute<string_t, dtime_tz_t>(
		    source, result, count, [&](string_t input) -> optional<dtime_tz_t> {
			    dtime_tz_t result;
			    const auto str = input.GetData();
			    const auto len = input.GetSize();
			    bool has_offset = false;
			    idx_t pos = 0;
			    if (!Time::TryConvertTimeTZ(str, len, pos, result, has_offset, false)) {
				    auto msg = Time::ConversionError(string(str, len));
				    HandleCastError::AssignError(msg, parameters);
				    all_converted = false;
				    return nullopt;
			    } else if (!has_offset) {
				    // Convert parts to a TZ (default or parsed) if no offset was provided
				    auto calendar = cal.get();

				    // Extract the offset from the calendar
				    int32_t offset;
				    if (!TryGetTimeTZOffset(calendar, offset)) {
					    HandleCastError::AssignError(TimeTZOffsetError(offset), parameters);
					    all_converted = false;
					    return nullopt;
				    }

				    // Apply it to the offset +00 time we parsed.
				    result = dtime_tz_t(result.time(), offset);
			    }

			    return result;
		    });
		return all_converted;
	}

	static BoundCastInfo BindCastFromVarchar(BindCastInput &input, const LogicalType &source,
	                                         const LogicalType &target) {
		if (!input.context) {
			throw InternalException("Missing context for VARCHAR to TIME/TIMESTAMPTZ cast.");
		}

		auto cast_data = BindCastData(input);

		switch (target.id()) {
		case LogicalTypeId::TIMESTAMP_TZ:
			return BoundCastInfo(VarcharToTimestampTZ, std::move(cast_data), InitCastCalendarCache);
		case LogicalTypeId::TIMESTAMP_TZ_NS:
			return BoundCastInfo(VarcharToTimestampTZNS, std::move(cast_data), InitCastCalendarCache);
		case LogicalTypeId::TIME_TZ:
			return BoundCastInfo(VarcharToTimeTZ, std::move(cast_data));
		default:
			throw InternalException("Unsupported type for VARCHAR to TIME/TIMESTAMPTZ cast.");
		}
	}

	static void AddCasts(ExtensionLoader &loader) {
		loader.RegisterCastFunction(LogicalType::VARCHAR, LogicalType::TIMESTAMP_TZ, BindCastFromVarchar);
		loader.RegisterCastFunction(LogicalType::VARCHAR, LogicalType::TIMESTAMP_TZ_NS, BindCastFromVarchar);
		loader.RegisterCastFunction(LogicalType::VARCHAR, LogicalType::TIME_TZ, BindCastFromVarchar);
	}
};

bind_scalar_function_t ICUStrptime::bind_strptime = nullptr; // NOLINT

struct ICUStrftime : public ICUDateFunc {
	static void ParseFormatSpecifier(const string_t &format_str, StrfTimeFormat &format) {
		const auto format_specifier = format_str.GetString();
		const auto error = StrTimeFormat::ParseFormatSpecifier(format_specifier, format);
		if (!error.empty()) {
			throw InvalidInputException("Failed to parse format specifier %s: %s", format_specifier, error);
		}
	}

	static string_t Operation(Calendar *calendar, timestamp_tz_t input, const char *tz_name, StrfTimeFormat &format,
	                          Vector &result) {
		// Get the parts in the given time zone
		uint64_t micros = SetTime(calendar, input);

		int32_t data[8];
		data[0] = ExtractField(calendar, CAL_EXTENDED_YEAR); // strftime doesn't understand eras.
		data[1] = ExtractField(calendar, CAL_MONTH) + 1;
		data[2] = ExtractField(calendar, CAL_DATE);
		data[3] = ExtractField(calendar, CAL_HOUR_OF_DAY);
		data[4] = ExtractField(calendar, CAL_MINUTE);
		data[5] = ExtractField(calendar, CAL_SECOND);
		data[6] =
		    UnsafeNumericCast<int32_t>(ExtractField(calendar, CAL_MILLISECOND) * Interval::MICROS_PER_MSEC + micros);

		data[7] = ExtractField(calendar, CAL_ZONE_OFFSET) + ExtractField(calendar, CAL_DST_OFFSET);
		data[7] /= Interval::MSECS_PER_SEC;

		const auto date = Date::FromDate(data[0], data[1], data[2]);
		const auto time = Time::FromTime(data[3], data[4], data[5], data[6]);

		const auto len = format.GetLength(date, time, data[7], tz_name);
		string_t target = StringVector::EmptyString(result, len);
		format.FormatString(date, data, tz_name, target.GetDataWriteable());
		target.Finalize();

		return target;
	}

	static string_t Operation(Calendar *calendar, timestamp_tz_ns_t input, const char *tz_name, StrfTimeFormat &format,
	                          Vector &result) {
		// Get the parts in the given time zone
		uint64_t nanos = SetTimeNS(calendar, input);

		int32_t data[8];
		data[0] = ExtractField(calendar, CAL_EXTENDED_YEAR); // strftime doesn't understand eras.
		data[1] = ExtractField(calendar, CAL_MONTH) + 1;
		data[2] = ExtractField(calendar, CAL_DATE);
		data[3] = ExtractField(calendar, CAL_HOUR_OF_DAY);
		data[4] = ExtractField(calendar, CAL_MINUTE);
		data[5] = ExtractField(calendar, CAL_SECOND);
		data[6] =
		    UnsafeNumericCast<int32_t>(ExtractField(calendar, CAL_MILLISECOND) * Interval::NANOS_PER_MSEC + nanos);

		data[7] = ExtractField(calendar, CAL_ZONE_OFFSET) + ExtractField(calendar, CAL_DST_OFFSET);
		data[7] /= Interval::MSECS_PER_SEC;

		const auto date = Date::FromDate(data[0], data[1], data[2]);

		const auto len = format.GetLength(date, data, tz_name);
		string_t target = StringVector::EmptyString(result, len);
		format.FormatStringNS(date, data, tz_name, target.GetDataWriteable());
		target.Finalize();

		return target;
	}

	template <typename T>
	static void ICUStrftimeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
		D_ASSERT(args.ColumnCount() == 2);
		const auto &src_arg = args.data[0];
		const auto &fmt_arg = args.data[1];

		auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
		auto &info = func_expr.BindInfo()->Cast<BindData>();
		CalendarPtr calendar(info.calendar->Copy());
		const auto tz_name = info.tz_setting.c_str();

		if (fmt_arg.GetVectorType() == VectorType::CONSTANT_VECTOR) {
			// Common case of constant part.
			if (ConstantVector::IsNull(fmt_arg)) {
				throw InternalException("ICUStrfTime called with constant NULL format");
			}
			StrfTimeFormat format;
			ParseFormatSpecifier(*ConstantVector::GetData<string_t>(fmt_arg), format);

			UnaryExecutor::Execute<T, string_t>(src_arg, result, [&](T input) {
				if (input.IsFinite()) {
					return Operation(calendar.get(), input, tz_name, format, result);
				} else {
					return StringVector::AddString(result, Date::ToInfinity(input));
				}
			});
		} else {
			BinaryExecutor::Execute<T, string_t, string_t>(
			    src_arg, fmt_arg, result, [&](T input, string_t format_specifier) {
				    if (input.IsFinite()) {
					    StrfTimeFormat format;
					    ParseFormatSpecifier(format_specifier, format);

					    return Operation(calendar.get(), input, tz_name, format, result);
				    } else {
					    return StringVector::AddString(result, Date::ToInfinity(input));
				    }
			    });
		}
	}

	static void AddBinaryTimestampFunction(const Identifier &name, ExtensionLoader &loader) {
		ScalarFunctionSet set {name};
		ScalarFunction tstz_fun({}, LogicalType::VARCHAR, ICUStrftimeFunction<timestamp_tz_t>, Bind);
		tstz_fun.GetSignature()
		    .AddParameter("data", LogicalType::TIMESTAMP_TZ)
		    .AddParameter("format", LogicalType::VARCHAR);
		set.AddFunction(tstz_fun);
		ScalarFunction tstz_ns_fun({}, LogicalType::VARCHAR, ICUStrftimeFunction<timestamp_tz_ns_t>, Bind);
		tstz_ns_fun.GetSignature()
		    .AddParameter("data", LogicalType::TIMESTAMP_TZ_NS)
		    .AddParameter("format", LogicalType::VARCHAR);
		set.AddFunction(tstz_ns_fun);
		// throws for unsupported format specifiers
		set.SetFallible();
		set.SetStability(FunctionStability::CONSISTENT_WITHIN_QUERY);
		loader.RegisterFunction(set);
	}

	template <typename T>
	static string_t CastOperation(Calendar *calendar, T input, Vector &result) {
		// Infinity is always formatted the same way
		if (!input.IsFinite()) {
			return StringVector::AddString(result, Date::ToInfinity(input));
		}

		// decompose the timestamp
		auto ts_data = ICUHelpers::GetComponents(input, calendar);

		idx_t year_length;
		bool add_bc;
		const auto date_len = DateToStringCast::YearLength(ts_data.year, year_length, add_bc);

		char micro_buffer[9];
		const auto time_len = TimeToStringCast::MicrosLength(ts_data.microsecond, micro_buffer, ts_data.nanosecond);

		auto offset = ExtractField(calendar, CAL_ZONE_OFFSET) + ExtractField(calendar, CAL_DST_OFFSET);
		offset /= Interval::MSECS_PER_SEC;
		int hour_offset = offset / Interval::SECS_PER_HOUR;
		int minute_offset = (offset % Interval::SECS_PER_HOUR) / Interval::SECS_PER_MINUTE;
		int second_offset = offset % Interval::SECS_PER_MINUTE;
		auto offset_str = Time::ToUTCOffset(hour_offset, minute_offset, second_offset);
		const auto offset_len = offset_str.size();

		const auto len = date_len + 1 + time_len + offset_len;
		string_t target = StringVector::EmptyString(result, len);
		auto buffer = target.GetDataWriteable();

		DateToStringCast::Format(buffer, ts_data.year, ts_data.month, ts_data.day, year_length, add_bc);
		buffer += date_len;
		*buffer++ = ' ';

		TimeToStringCast::Format(buffer, time_len, ts_data.hour, ts_data.minute, ts_data.second, 0, micro_buffer);
		buffer += time_len;

		memcpy(buffer, offset_str.c_str(), offset_len);
		buffer += offset_len;

		target.Finalize();

		return target;
	}

	template <typename T>
	static bool CastToVarchar(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
		auto &cast_data = parameters.cast_data->Cast<CastData>();
		auto &info = cast_data.info->Cast<BindData>();
		CalendarPtr calendar(info.calendar->Copy());

		UnaryExecutor::Execute<T, string_t>(source, result, count,
		                                    [&](T input) { return CastOperation<T>(calendar.get(), input, result); });
		return true;
	}

	static BoundCastInfo BindCastToVarchar(BindCastInput &input, const LogicalType &source, const LogicalType &target) {
		if (!input.context) {
			throw InternalException("Missing context for TIMESTAMPTZ to VARCHAR cast.");
		}

		auto cast_data = BindCastData(input);

		switch (source.id()) {
		case LogicalTypeId::TIMESTAMP_TZ:
			return BoundCastInfo(CastToVarchar<timestamp_tz_t>, std::move(cast_data));
		case LogicalTypeId::TIMESTAMP_TZ_NS:
			return BoundCastInfo(CastToVarchar<timestamp_tz_ns_t>, std::move(cast_data));
		default:
			throw InternalException("Unexpected TIMESTAMPTZ type to VARCHAR cast.");
		}
	}

	static void AddCasts(ExtensionLoader &loader) {
		loader.RegisterCastFunction(LogicalType::TIMESTAMP_TZ, LogicalType::VARCHAR, BindCastToVarchar);
		loader.RegisterCastFunction(LogicalType::TIMESTAMP_TZ_NS, LogicalType::VARCHAR, BindCastToVarchar);
	}
};

void RegisterICUStrptimeFunctions(ExtensionLoader &loader) {
	ICUStrptime::AddBinaryTimestampFunction("strptime", loader);
	ICUStrptime::AddBinaryTimestampFunction("try_strptime", loader);

	ICUStrftime::AddBinaryTimestampFunction("strftime", loader);

	// Add string casts
	ICUStrptime::AddCasts(loader);
	ICUStrftime::AddCasts(loader);
}

} // namespace duckdb
