#include "duckdb/common/exception.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/helper.hpp" // defines DUCKDB_EXPLICIT_FALLTHROUGH which fmt will use to annotate
#include "fmt/format.h"
#include "fmt/printf.h"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/uhugeint.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/common/types/string.hpp"
#include "duckdb/common/identifier.hpp"

namespace duckdb {

ExceptionFormatValue::ExceptionFormatValue(double dbl_val)
    : type(ExceptionFormatValueType::FORMAT_VALUE_TYPE_DOUBLE), dbl_val(dbl_val) {
}
ExceptionFormatValue::ExceptionFormatValue(int64_t int_val)
    : type(ExceptionFormatValueType::FORMAT_VALUE_TYPE_INTEGER), int_val(int_val) {
}
ExceptionFormatValue::ExceptionFormatValue(idx_t uint_val)
    : type(ExceptionFormatValueType::FORMAT_VALUE_TYPE_INTEGER), int_val(Hugeint::Convert(uint_val)) {
}
ExceptionFormatValue::ExceptionFormatValue(hugeint_t huge_val)
    : type(ExceptionFormatValueType::FORMAT_VALUE_TYPE_STRING), str_val(Hugeint::ToString(huge_val)) {
}
ExceptionFormatValue::ExceptionFormatValue(uhugeint_t uhuge_val)
    : type(ExceptionFormatValueType::FORMAT_VALUE_TYPE_STRING), str_val(Uhugeint::ToString(uhuge_val)) {
}
ExceptionFormatValue::ExceptionFormatValue(string str_val)
    : type(ExceptionFormatValueType::FORMAT_VALUE_TYPE_STRING), str_val(std::move(str_val)) {
}
ExceptionFormatValue::ExceptionFormatValue(const String &str_val) : ExceptionFormatValue(str_val.ToStdString()) {
}

template <>
ExceptionFormatValue ExceptionFormatValue::CreateFormatValue(const PhysicalType &value) {
	return ExceptionFormatValue(TypeIdToString(value));
}
template <>
ExceptionFormatValue ExceptionFormatValue::CreateFormatValue(const LogicalType &value) {
	return ExceptionFormatValue(value.ToString());
}
template <>
ExceptionFormatValue ExceptionFormatValue::CreateFormatValue(const float &value) {
	return ExceptionFormatValue(static_cast<double>(value));
}
template <>
ExceptionFormatValue ExceptionFormatValue::CreateFormatValue(const double &value) {
	return ExceptionFormatValue(value);
}
template <>
ExceptionFormatValue ExceptionFormatValue::CreateFormatValue(const string &value) {
	return ExceptionFormatValue(value);
}
template <>
ExceptionFormatValue ExceptionFormatValue::CreateFormatValue(const String &value) {
	return ExceptionFormatValue(value);
}
template <>
ExceptionFormatValue ExceptionFormatValue::CreateFormatValue(const Identifier &value) {
	ExceptionFormatValue result(SQLQuotedIdentifier::ToString(value));
	result.quoted_identifier = true;
	return result;
}

template <>
ExceptionFormatValue ExceptionFormatValue::CreateFormatValue(const SQLString &value) {
	return SQLString::ToString(value.raw_string);
}

template <>
ExceptionFormatValue ExceptionFormatValue::CreateFormatValue(const SQLIdentifier &value) {
	return SQLIdentifier::ToString(value.raw_string);
}

template <>
ExceptionFormatValue ExceptionFormatValue::CreateFormatValue(const char *const &value) {
	return ExceptionFormatValue(string(value));
}
template <>
ExceptionFormatValue ExceptionFormatValue::CreateFormatValue(char *const &value) {
	return ExceptionFormatValue(string(value));
}
template <>
ExceptionFormatValue ExceptionFormatValue::CreateFormatValue(const idx_t &value) {
	return ExceptionFormatValue(value);
}
template <>
ExceptionFormatValue ExceptionFormatValue::CreateFormatValue(const hugeint_t &value) {
	return ExceptionFormatValue(value);
}
template <>
ExceptionFormatValue ExceptionFormatValue::CreateFormatValue(const uhugeint_t &value) {
	return ExceptionFormatValue(value);
}

//! Whether a placeholder of the format string that receives a quoted identifier is itself wrapped in quotes
[[maybe_unused]] static bool HasDoubleQuotedIdentifier(const string &msg,
                                                       const std::vector<ExceptionFormatValue> &values) {
	idx_t arg_idx = 0;
	for (idx_t i = 0; i < msg.size(); i++) {
		if (msg[i] != '%') {
			continue;
		}
		if (i + 1 < msg.size() && msg[i + 1] == '%') {
			i++;
			continue;
		}
		// skip flags, width and precision up to the conversion character
		idx_t end = i + 1;
		while (end < msg.size() && !isalpha(msg[end])) {
			end++;
		}
		if (arg_idx < values.size() && values[arg_idx].quoted_identifier) {
			bool quote_before = i > 0 && msg[i - 1] == '"';
			bool quote_after = end + 1 < msg.size() && msg[end + 1] == '"';
			if (quote_before && quote_after) {
				return true;
			}
		}
		arg_idx++;
		i = end;
	}
	return false;
}

string ExceptionFormatValue::Format(const string &msg, std::vector<ExceptionFormatValue> &values) {
	// identifiers are quoted by the formatter, the format string must not quote them again
	D_ASSERT(!HasDoubleQuotedIdentifier(msg, values));
	try {
		std::vector<duckdb_fmt::basic_format_arg<duckdb_fmt::printf_context>> format_args;
		for (auto &val : values) {
			switch (val.type) {
			case ExceptionFormatValueType::FORMAT_VALUE_TYPE_DOUBLE:
				format_args.push_back(duckdb_fmt::internal::make_arg<duckdb_fmt::printf_context>(val.dbl_val));
				break;
			case ExceptionFormatValueType::FORMAT_VALUE_TYPE_INTEGER:
				format_args.push_back(duckdb_fmt::internal::make_arg<duckdb_fmt::printf_context>(val.int_val));
				break;
			case ExceptionFormatValueType::FORMAT_VALUE_TYPE_STRING:
				format_args.push_back(duckdb_fmt::internal::make_arg<duckdb_fmt::printf_context>(val.str_val));
				break;
			}
		}
		return duckdb_fmt::vsprintf(msg, duckdb_fmt::basic_format_args<duckdb_fmt::printf_context>(
		                                     format_args.data(), static_cast<int>(format_args.size())));
	} catch (std::exception &ex) { // LCOV_EXCL_START
		// work-around for oss-fuzz limiting memory which causes issues here
		if (StringUtil::Contains(ex.what(), "fuzz mode")) {
			throw InvalidInputException(msg);
		}
		throw InternalException(std::string("Primary exception: ") + msg +
		                        "\nSecondary exception in ExceptionFormatValue: " + ex.what());
	} // LCOV_EXCL_STOP
}

} // namespace duckdb
