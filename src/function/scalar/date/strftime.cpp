#include "duckdb/function/scalar/strftime_format.hpp"

#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_parameter_expression.hpp"
#include "duckdb/function/scalar/date_functions.hpp"

#include <cctype>
#include <utility>

namespace duckdb {

StrpTimeFormat::StrpTimeFormat() {
}

StrpTimeFormat::StrpTimeFormat(const string &format_string) {
	if (format_string.empty()) {
		return;
	}
	StrTimeFormat::ParseFormatSpecifier(format_string, *this);
}

namespace {

struct StrfTimeBindData : public FunctionData {
	explicit StrfTimeBindData(StrfTimeFormat format_p, string format_string_p, bool is_null)
	    : format(std::move(format_p)), format_string(std::move(format_string_p)), is_null(is_null) {
	}

	StrfTimeFormat format;
	string format_string;
	bool is_null;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<StrfTimeBindData>(format, format_string, is_null);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<StrfTimeBindData>();
		return format_string == other.format_string;
	}
};

template <bool REVERSED>
static unique_ptr<FunctionData> StrfTimeBindFunction(BindScalarFunctionInput &input) {
	auto &arguments = input.GetArguments();
	auto format_idx = REVERSED ? 0U : 1U;
	auto &format_arg = arguments[format_idx];
	Value options_str = input.GetConstant(format_idx);
	auto format_string = options_str.GetValue<string>();
	StrfTimeFormat format;
	bool is_null = options_str.IsNull();
	if (!is_null) {
		string error = StrTimeFormat::ParseFormatSpecifier(format_string, format);
		if (!error.empty()) {
			throw InvalidInputException(*format_arg, "Failed to parse format specifier %s: %s", format_string, error);
		}
	}
	return make_uniq<StrfTimeBindData>(format, format_string, is_null);
}

template <bool REVERSED>
static void StrfTimeFunctionDate(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &info = func_expr.BindInfo()->Cast<StrfTimeBindData>();

	if (info.is_null) {
		ConstantVector::SetNull(result, count_t(args.size()));
		return;
	}
	info.format.ConvertDateVector(args.data[REVERSED ? 1 : 0], result);
}

template <bool REVERSED>
static void StrfTimeFunctionTimestamp(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &info = func_expr.BindInfo()->Cast<StrfTimeBindData>();

	if (info.is_null) {
		ConstantVector::SetNull(result, count_t(args.size()));
		return;
	}
	info.format.ConvertTimestampVector(args.data[REVERSED ? 1 : 0], result);
}

template <bool REVERSED>
static void StrfTimeFunctionTimestampNS(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &info = func_expr.BindInfo()->Cast<StrfTimeBindData>();

	if (info.is_null) {
		ConstantVector::SetNull(result, count_t(args.size()));
		return;
	}
	info.format.ConvertTimestampNSVector(args.data[REVERSED ? 1 : 0], result);
}

struct StrpTimeBindData : public FunctionData {
	StrpTimeBindData(const StrpTimeFormat &format, const string &format_string)
	    : formats(1, format), format_strings(1, format_string) {
	}

	StrpTimeBindData(vector<StrpTimeFormat> formats_p, vector<string> format_strings_p)
	    : formats(std::move(formats_p)), format_strings(std::move(format_strings_p)) {
	}

	vector<StrpTimeFormat> formats;
	vector<string> format_strings;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<StrpTimeBindData>(formats, format_strings);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<StrpTimeBindData>();
		return format_strings == other.format_strings;
	}
};

template <typename T>
inline T StrpTimeResult(StrpTimeFormat::ParseResult &parsed) {
	return parsed.ToTimestamp();
}

template <>
inline timestamp_ns_t StrpTimeResult(StrpTimeFormat::ParseResult &parsed) {
	return parsed.ToTimestampNS();
}

template <typename T>
inline bool StrpTimeTryResult(StrpTimeFormat &format, string_t &input, T &result, string &error) {
	return format.TryParseTimestamp(input, result, error);
}

template <>
inline bool StrpTimeTryResult(StrpTimeFormat &format, string_t &input, timestamp_ns_t &result, string &error) {
	return format.TryParseTimestampNS(input, result, error);
}

struct StrpTimeFunction {
	template <typename T>
	static void Parse(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
		auto &info = func_expr.BindInfo()->Cast<StrpTimeBindData>();

		//	There is a bizarre situation where the format column is foldable but not constant
		//	(i.e., the statistics tell us it has only one value)
		//	We have to check whether that value is NULL
		auto format_entries = args.data[1].Validity();

		if (!format_entries.IsValid(0)) {
			ConstantVector::SetNull(result, count_t(args.size()));
			return;
		}
		UnaryExecutor::Execute<string_t, T>(args.data[0], result, [&](string_t input) {
			StrpTimeFormat::ParseResult result;
			for (auto &format : info.formats) {
				if (format.Parse(input, result)) {
					return StrpTimeResult<T>(result);
				}
			}
			throw InvalidInputException(result.FormatError(input, info.formats[0].format_specifier));
		});
	}

	template <typename T>
	static void TryParse(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
		auto &info = func_expr.BindInfo()->Cast<StrpTimeBindData>();

		if (args.data[1].GetVectorType() == VectorType::CONSTANT_VECTOR && ConstantVector::IsNull(args.data[1])) {
			ConstantVector::SetNull(result, count_t(args.size()));
			return;
		}

		UnaryExecutor::Execute<string_t, T>(args.data[0], result, [&](string_t input) -> optional<T> {
			T result;
			string error;
			for (auto &format : info.formats) {
				if (StrpTimeTryResult(format, input, result, error)) {
					return result;
				}
			}

			return nullopt;
		});
	}

	static unique_ptr<FunctionData> Bind(BindScalarFunctionInput &input) {
		auto &bound_function = input.GetBoundFunction();
		auto &arguments = input.GetArguments();
		Value format_value = input.GetConstant(1);
		string format_string;
		StrpTimeFormat format;
		if (format_value.IsNull()) {
			return make_uniq<StrpTimeBindData>(format, format_string);
		} else if (format_value.type().id() == LogicalTypeId::VARCHAR) {
			format_string = format_value.ToString();
			format.format_specifier = format_string;
			string error = StrTimeFormat::ParseFormatSpecifier(format_string, format);
			if (!error.empty()) {
				throw InvalidInputException(*arguments[0], "Failed to parse format specifier %s: %s", format_string,
				                            error);
			}
			if (format.HasFormatSpecifier(StrTimeSpecifier::NANOSECOND_PADDED)) {
				if (format.HasFormatSpecifier(StrTimeSpecifier::UTC_OFFSET)) {
					bound_function.SetReturnType(LogicalType::TIMESTAMP_TZ_NS);
				} else {
					bound_function.SetReturnType(LogicalType::TIMESTAMP_NS);
				}
				if (bound_function.GetName() == "strptime") {
					bound_function.SetFunctionCallback(Parse<timestamp_ns_t>);
				} else {
					bound_function.SetFunctionCallback(TryParse<timestamp_ns_t>);
				}
			} else if (format.HasFormatSpecifier(StrTimeSpecifier::UTC_OFFSET)) {
				bound_function.SetReturnType(LogicalType::TIMESTAMP_TZ);
			}
			return make_uniq<StrpTimeBindData>(format, format_string);
		} else if (format_value.type() == LogicalType::LIST(LogicalType::VARCHAR)) {
			const auto &children = ListValue::GetChildren(format_value);
			if (children.empty()) {
				throw InvalidInputException(*arguments[0], "strptime format list must not be empty");
			}
			vector<string> format_strings;
			vector<StrpTimeFormat> formats;
			bool has_offset = false;
			bool has_nanos = false;

			for (const auto &child : children) {
				format_string = child.ToString();
				format.format_specifier = format_string;
				string error = StrTimeFormat::ParseFormatSpecifier(format_string, format);
				if (!error.empty()) {
					throw InvalidInputException(*arguments[0], "Failed to parse format specifier %s: %s", format_string,
					                            error);
				}
				has_offset = has_offset || format.HasFormatSpecifier(StrTimeSpecifier::UTC_OFFSET);
				has_nanos = has_nanos || format.HasFormatSpecifier(StrTimeSpecifier::NANOSECOND_PADDED);
				format_strings.emplace_back(format_string);
				formats.emplace_back(format);
			}

			if (has_nanos) {
				// If any format has nanoseconds, then we have to produce TSNS
				if (has_offset) {
					// If any format has UTC offsets, then we have to produce TSTZ
					bound_function.SetReturnType(LogicalType::TIMESTAMP_TZ_NS);
				} else {
					bound_function.SetReturnType(LogicalType::TIMESTAMP_NS);
				}
				if (bound_function.GetName() == "strptime") {
					bound_function.SetFunctionCallback(Parse<timestamp_ns_t>);
				} else {
					bound_function.SetFunctionCallback(TryParse<timestamp_ns_t>);
				}
			} else if (has_offset) {
				// If any format has UTC offsets, then we have to produce TSTZ
				bound_function.SetReturnType(LogicalType::TIMESTAMP_TZ);
			}
			return make_uniq<StrpTimeBindData>(formats, format_strings);
		} else {
			throw InvalidInputException(*arguments[0], "strptime format must be a string");
		}
	}
};

} // namespace

ScalarFunctionSet StrfTimeFun::GetFunctions() {
	ScalarFunctionSet strftime("strftime");

	ScalarFunction data_format({}, LogicalType::VARCHAR, StrfTimeFunctionDate<false>, StrfTimeBindFunction<false>);
	data_format.GetSignature().AddParameter("data", LogicalType::DATE).AddParameter("format", LogicalType::VARCHAR);
	strftime.AddFunction(data_format);

	ScalarFunction format_data({}, LogicalType::VARCHAR, StrfTimeFunctionDate<true>, StrfTimeBindFunction<true>);
	format_data.GetSignature().AddParameter("format", LogicalType::VARCHAR).AddParameter("data", LogicalType::DATE);
	strftime.AddFunction(format_data);

	ScalarFunction ts_data_format({}, LogicalType::VARCHAR, StrfTimeFunctionTimestamp<false>,
	                              StrfTimeBindFunction<false>);
	ts_data_format.GetSignature()
	    .AddParameter("data", LogicalType::TIMESTAMP)
	    .AddParameter("format", LogicalType::VARCHAR);
	strftime.AddFunction(ts_data_format);

	ScalarFunction ts_format_data({}, LogicalType::VARCHAR, StrfTimeFunctionTimestamp<true>,
	                              StrfTimeBindFunction<true>);
	ts_format_data.GetSignature()
	    .AddParameter("format", LogicalType::VARCHAR)
	    .AddParameter("data", LogicalType::TIMESTAMP);
	strftime.AddFunction(ts_format_data);

	ScalarFunction ts_ns_data_format({}, LogicalType::VARCHAR, StrfTimeFunctionTimestampNS<false>,
	                                 StrfTimeBindFunction<false>);
	ts_ns_data_format.GetSignature()
	    .AddParameter("data", LogicalType::TIMESTAMP_NS)
	    .AddParameter("format", LogicalType::VARCHAR);
	strftime.AddFunction(ts_ns_data_format);

	ScalarFunction ts_ns_format_data({}, LogicalType::VARCHAR, StrfTimeFunctionTimestampNS<true>,
	                                 StrfTimeBindFunction<true>);
	ts_ns_format_data.GetSignature()
	    .AddParameter("format", LogicalType::VARCHAR)
	    .AddParameter("data", LogicalType::TIMESTAMP_NS);
	strftime.AddFunction(ts_ns_format_data);

	ScalarFunction ts_tz_data_format({}, LogicalType::VARCHAR, StrfTimeFunctionTimestamp<false>,
	                                 StrfTimeBindFunction<false>);
	ts_tz_data_format.GetSignature()
	    .AddParameter("data", LogicalType::TIMESTAMP_TZ)
	    .AddParameter("format", LogicalType::VARCHAR);
	strftime.AddFunction(ts_tz_data_format);

	ScalarFunction ts_tz_format_data({}, LogicalType::VARCHAR, StrfTimeFunctionTimestamp<true>,
	                                 StrfTimeBindFunction<true>);
	ts_tz_format_data.GetSignature()
	    .AddParameter("format", LogicalType::VARCHAR)
	    .AddParameter("data", LogicalType::TIMESTAMP_TZ);
	strftime.AddFunction(ts_tz_format_data);

	ScalarFunction ts_tz_ns_data_format({}, LogicalType::VARCHAR, StrfTimeFunctionTimestampNS<false>,
	                                    StrfTimeBindFunction<false>);
	ts_tz_ns_data_format.GetSignature()
	    .AddParameter("data", LogicalType::TIMESTAMP_TZ_NS)
	    .AddParameter("format", LogicalType::VARCHAR);
	strftime.AddFunction(ts_tz_ns_data_format);

	ScalarFunction ts_tz_ns_format_data({}, LogicalType::VARCHAR, StrfTimeFunctionTimestampNS<true>,
	                                    StrfTimeBindFunction<true>);
	ts_tz_ns_format_data.GetSignature()
	    .AddParameter("format", LogicalType::VARCHAR)
	    .AddParameter("data", LogicalType::TIMESTAMP_TZ_NS);
	strftime.AddFunction(ts_tz_ns_format_data);

	// throws for unsupported format specifiers
	strftime.SetFallible();
	return strftime;
}
ScalarFunctionSet StrpTimeFun::GetFunctions() {
	ScalarFunctionSet strptime("strptime");

	const auto list_type = LogicalType::LIST(LogicalType::VARCHAR);
	auto fun = ScalarFunction({}, LogicalType::TIMESTAMP, StrpTimeFunction::Parse<timestamp_t>, StrpTimeFunction::Bind);
	fun.GetSignature().AddParameter("text", LogicalType::VARCHAR).AddParameter("format", LogicalType::VARCHAR);
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	fun.SetFallible();
	strptime.AddFunction(fun);

	fun = ScalarFunction({}, LogicalType::TIMESTAMP, StrpTimeFunction::Parse<timestamp_t>, StrpTimeFunction::Bind);
	fun.GetSignature().AddParameter("text", LogicalType::VARCHAR).AddParameter("format-list", list_type);
	fun.SetFallible();
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	strptime.AddFunction(fun);
	return strptime;
}

ScalarFunctionSet TryStrpTimeFun::GetFunctions() {
	ScalarFunctionSet try_strptime("try_strptime");

	const auto list_type = LogicalType::LIST(LogicalType::VARCHAR);
	auto fun =
	    ScalarFunction({}, LogicalType::TIMESTAMP, StrpTimeFunction::TryParse<timestamp_t>, StrpTimeFunction::Bind);
	fun.GetSignature().AddParameter("text", LogicalType::VARCHAR).AddParameter("format", LogicalType::VARCHAR);
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	try_strptime.AddFunction(fun);

	fun = ScalarFunction({}, LogicalType::TIMESTAMP, StrpTimeFunction::TryParse<timestamp_t>, StrpTimeFunction::Bind);
	fun.GetSignature().AddParameter("text", LogicalType::VARCHAR).AddParameter("format", list_type);
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	try_strptime.AddFunction(fun);

	return try_strptime;
}

} // namespace duckdb
