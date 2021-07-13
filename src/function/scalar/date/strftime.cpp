#include "duckdb/function/scalar/date_functions.hpp"

#include "duckdb/planner/expression/bound_function_expression.hpp"

#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/cast_helpers.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/to_string.hpp"

#include "duckdb/function/scalar/strftime.hpp"

#include "duckdb/common/vector_operations/unary_executor.hpp"

#include "duckdb/execution/expression_executor.hpp"

#include "re2/re2.h"

#include <cctype>

namespace duckdb {

idx_t StrfTimepecifierSize(StrTimeSpecifier specifier) {
	switch (specifier) {
	case StrTimeSpecifier::ABBREVIATED_WEEKDAY_NAME:
	case StrTimeSpecifier::ABBREVIATED_MONTH_NAME:
		return 3;
	case StrTimeSpecifier::WEEKDAY_DECIMAL:
		return 1;
	case StrTimeSpecifier::DAY_OF_MONTH_PADDED:
	case StrTimeSpecifier::MONTH_DECIMAL_PADDED:
	case StrTimeSpecifier::YEAR_WITHOUT_CENTURY_PADDED:
	case StrTimeSpecifier::HOUR_24_PADDED:
	case StrTimeSpecifier::HOUR_12_PADDED:
	case StrTimeSpecifier::MINUTE_PADDED:
	case StrTimeSpecifier::SECOND_PADDED:
	case StrTimeSpecifier::AM_PM:
	case StrTimeSpecifier::WEEK_NUMBER_PADDED_SUN_FIRST:
	case StrTimeSpecifier::WEEK_NUMBER_PADDED_MON_FIRST:
		return 2;
	case StrTimeSpecifier::MICROSECOND_PADDED:
		return 6;
	case StrTimeSpecifier::MILLISECOND_PADDED:
		return 3;
	case StrTimeSpecifier::DAY_OF_YEAR_PADDED:
		return 3;
	default:
		return 0;
	}
}

void StrTimeFormat::AddLiteral(string literal) {
	constant_size += literal.size();
	literals.push_back(move(literal));
}

void StrTimeFormat::AddFormatSpecifier(string preceding_literal, StrTimeSpecifier specifier) {
	AddLiteral(move(preceding_literal));
	specifiers.push_back(specifier);
}

void StrfTimeFormat::AddFormatSpecifier(string preceding_literal, StrTimeSpecifier specifier) {
	is_date_specifier.push_back(IsDateSpecifier(specifier));
	idx_t specifier_size = StrfTimepecifierSize(specifier);
	if (specifier_size == 0) {
		// variable length specifier
		var_length_specifiers.push_back(specifier);
	} else {
		// constant size specifier
		constant_size += specifier_size;
	}
	StrTimeFormat::AddFormatSpecifier(move(preceding_literal), specifier);
}

idx_t StrfTimeFormat::GetSpecifierLength(StrTimeSpecifier specifier, date_t date, dtime_t time) {
	switch (specifier) {
	case StrTimeSpecifier::FULL_WEEKDAY_NAME:
		return Date::DAY_NAMES[Date::ExtractISODayOfTheWeek(date) % 7].GetSize();
	case StrTimeSpecifier::FULL_MONTH_NAME:
		return Date::MONTH_NAMES[Date::ExtractMonth(date) - 1].GetSize();
	case StrTimeSpecifier::YEAR_DECIMAL: {
		auto year = Date::ExtractYear(date);
		return NumericHelper::SignedLength<int32_t, uint32_t>(year);
	}
	case StrTimeSpecifier::MONTH_DECIMAL: {
		idx_t len = 1;
		auto month = Date::ExtractMonth(date);
		len += month >= 10;
		return len;
	}
	case StrTimeSpecifier::UTC_OFFSET:
		// +00
		return 3;
	case StrTimeSpecifier::TZ_NAME:
		// empty for now
		return 0;
	case StrTimeSpecifier::HOUR_24_DECIMAL:
	case StrTimeSpecifier::HOUR_12_DECIMAL:
	case StrTimeSpecifier::MINUTE_DECIMAL:
	case StrTimeSpecifier::SECOND_DECIMAL: {
		// time specifiers
		idx_t len = 1;
		int32_t hour, min, sec, msec;
		Time::Convert(time, hour, min, sec, msec);
		switch (specifier) {
		case StrTimeSpecifier::HOUR_24_DECIMAL:
			len += hour >= 10;
			break;
		case StrTimeSpecifier::HOUR_12_DECIMAL:
			hour = hour % 12;
			if (hour == 0) {
				hour = 12;
			}
			len += hour >= 10;
			break;
		case StrTimeSpecifier::MINUTE_DECIMAL:
			len += min >= 10;
			break;
		case StrTimeSpecifier::SECOND_DECIMAL:
			len += sec >= 10;
			break;
		default:
			throw InternalException("Time specifier mismatch");
		}
		return len;
	}
	case StrTimeSpecifier::DAY_OF_MONTH:
		return NumericHelper::UnsignedLength<uint32_t>(Date::ExtractDay(date));
	case StrTimeSpecifier::DAY_OF_YEAR_DECIMAL:
		return NumericHelper::UnsignedLength<uint32_t>(Date::ExtractDayOfTheYear(date));
	case StrTimeSpecifier::YEAR_WITHOUT_CENTURY:
		return NumericHelper::UnsignedLength<uint32_t>(Date::ExtractYear(date) % 100);
	default:
		throw InternalException("Unimplemented specifier for GetSpecifierLength");
	}
}

//! Returns the total length of the date formatted by this format specifier
idx_t StrfTimeFormat::GetLength(date_t date, dtime_t time) {
	idx_t size = constant_size;
	if (!var_length_specifiers.empty()) {
		for (auto &specifier : var_length_specifiers) {
			size += GetSpecifierLength(specifier, date, time);
		}
	}
	return size;
}

char *StrfTimeFormat::WriteString(char *target, const string_t &str) {
	idx_t size = str.GetSize();
	memcpy(target, str.GetDataUnsafe(), size);
	return target + size;
}

// write a value in the range of 0..99 unpadded (e.g. "1", "2", ... "98", "99")
char *StrfTimeFormat::Write2(char *target, uint8_t value) {
	D_ASSERT(value < 100);
	if (value >= 10) {
		return WritePadded2(target, value);
	} else {
		*target = char(uint8_t('0') + value);
		return target + 1;
	}
}

// write a value in the range of 0..99 padded to 2 digits
char *StrfTimeFormat::WritePadded2(char *target, int32_t value) {
	D_ASSERT(value < 100);
	auto index = static_cast<unsigned>(value * 2);
	*target++ = duckdb_fmt::internal::data::digits[index];
	*target++ = duckdb_fmt::internal::data::digits[index + 1];
	return target;
}

// write a value in the range of 0..999 padded
char *StrfTimeFormat::WritePadded3(char *target, uint32_t value) {
	D_ASSERT(value < 1000);
	if (value >= 100) {
		WritePadded2(target + 1, value % 100);
		*target = char(uint8_t('0') + value / 100);
		return target + 3;
	} else {
		*target = '0';
		target++;
		return WritePadded2(target, value);
	}
}

// write a value in the range of 0..999999 padded to 6 digits
char *StrfTimeFormat::WritePadded(char *target, int32_t value, int32_t padding) {
	D_ASSERT(padding % 2 == 0);
	for (int i = 0; i < padding / 2; i++) {
		int decimals = value % 100;
		WritePadded2(target + padding - 2 * (i + 1), decimals);
		value /= 100;
	}
	return target + padding;
}

bool StrfTimeFormat::IsDateSpecifier(StrTimeSpecifier specifier) {
	switch (specifier) {
	case StrTimeSpecifier::ABBREVIATED_WEEKDAY_NAME:
	case StrTimeSpecifier::FULL_WEEKDAY_NAME:
	case StrTimeSpecifier::DAY_OF_YEAR_PADDED:
	case StrTimeSpecifier::DAY_OF_YEAR_DECIMAL:
	case StrTimeSpecifier::WEEK_NUMBER_PADDED_MON_FIRST:
	case StrTimeSpecifier::WEEK_NUMBER_PADDED_SUN_FIRST:
	case StrTimeSpecifier::WEEKDAY_DECIMAL:
		return true;
	default:
		return false;
	}
}

char *StrfTimeFormat::WriteDateSpecifier(StrTimeSpecifier specifier, date_t date, char *target) {
	switch (specifier) {
	case StrTimeSpecifier::ABBREVIATED_WEEKDAY_NAME: {
		auto dow = Date::ExtractISODayOfTheWeek(date);
		target = WriteString(target, Date::DAY_NAMES_ABBREVIATED[dow % 7]);
		break;
	}
	case StrTimeSpecifier::FULL_WEEKDAY_NAME: {
		auto dow = Date::ExtractISODayOfTheWeek(date);
		target = WriteString(target, Date::DAY_NAMES[dow % 7]);
		break;
	}
	case StrTimeSpecifier::WEEKDAY_DECIMAL: {
		auto dow = Date::ExtractISODayOfTheWeek(date);
		*target = char('0' + uint8_t(dow % 7));
		target++;
		break;
	}
	case StrTimeSpecifier::DAY_OF_YEAR_PADDED: {
		int32_t doy = Date::ExtractDayOfTheYear(date);
		target = WritePadded3(target, doy);
		break;
	}
	case StrTimeSpecifier::WEEK_NUMBER_PADDED_MON_FIRST:
		target = WritePadded2(target, Date::ExtractWeekNumberRegular(date, true));
		break;
	case StrTimeSpecifier::WEEK_NUMBER_PADDED_SUN_FIRST:
		target = WritePadded2(target, Date::ExtractWeekNumberRegular(date, false));
		break;
	case StrTimeSpecifier::DAY_OF_YEAR_DECIMAL: {
		uint32_t doy = Date::ExtractDayOfTheYear(date);
		target += NumericHelper::UnsignedLength<uint32_t>(doy);
		NumericHelper::FormatUnsigned(doy, target);
		break;
	}
	default:
		throw InternalException("Unimplemented date specifier for strftime");
	}
	return target;
}

char *StrfTimeFormat::WriteStandardSpecifier(StrTimeSpecifier specifier, int32_t data[], char *target) {
	// data contains [0] year, [1] month, [2] day, [3] hour, [4] minute, [5] second, [6] msec
	switch (specifier) {
	case StrTimeSpecifier::DAY_OF_MONTH_PADDED:
		target = WritePadded2(target, data[2]);
		break;
	case StrTimeSpecifier::ABBREVIATED_MONTH_NAME: {
		auto &month_name = Date::MONTH_NAMES_ABBREVIATED[data[1] - 1];
		return WriteString(target, month_name);
	}
	case StrTimeSpecifier::FULL_MONTH_NAME: {
		auto &month_name = Date::MONTH_NAMES[data[1] - 1];
		return WriteString(target, month_name);
	}
	case StrTimeSpecifier::MONTH_DECIMAL_PADDED:
		target = WritePadded2(target, data[1]);
		break;
	case StrTimeSpecifier::YEAR_WITHOUT_CENTURY_PADDED:
		target = WritePadded2(target, data[0] % 100);
		break;
	case StrTimeSpecifier::YEAR_DECIMAL:
		if (data[0] >= 0 && data[0] <= 9999) {
			target = WritePadded(target, data[0], 4);
		} else {
			int32_t year = data[0];
			if (data[0] < 0) {
				*target = '-';
				year = -year;
				target++;
			}
			auto len = NumericHelper::UnsignedLength<uint32_t>(year);
			NumericHelper::FormatUnsigned(year, target + len);
			target += len;
		}
		break;
	case StrTimeSpecifier::HOUR_24_PADDED: {
		target = WritePadded2(target, data[3]);
		break;
	}
	case StrTimeSpecifier::HOUR_12_PADDED: {
		int hour = data[3] % 12;
		if (hour == 0) {
			hour = 12;
		}
		target = WritePadded2(target, hour);
		break;
	}
	case StrTimeSpecifier::AM_PM:
		*target++ = data[3] >= 12 ? 'P' : 'A';
		*target++ = 'M';
		break;
	case StrTimeSpecifier::MINUTE_PADDED: {
		target = WritePadded2(target, data[4]);
		break;
	}
	case StrTimeSpecifier::SECOND_PADDED:
		target = WritePadded2(target, data[5]);
		break;
	case StrTimeSpecifier::MICROSECOND_PADDED:
		target = WritePadded(target, data[6], 6);
		break;
	case StrTimeSpecifier::MILLISECOND_PADDED:
		target = WritePadded3(target, data[6] / 1000);
		break;
	case StrTimeSpecifier::UTC_OFFSET:
		*target++ = '+';
		*target++ = '0';
		*target++ = '0';
		break;
	case StrTimeSpecifier::TZ_NAME:
		// always empty for now, FIXME when we have timestamp with tz
		break;
	case StrTimeSpecifier::DAY_OF_MONTH: {
		target = Write2(target, data[2] % 100);
		break;
	}
	case StrTimeSpecifier::MONTH_DECIMAL: {
		target = Write2(target, data[1]);
		break;
	}
	case StrTimeSpecifier::YEAR_WITHOUT_CENTURY: {
		target = Write2(target, data[0] % 100);
		break;
	}
	case StrTimeSpecifier::HOUR_24_DECIMAL: {
		target = Write2(target, data[3]);
		break;
	}
	case StrTimeSpecifier::HOUR_12_DECIMAL: {
		int hour = data[3] % 12;
		if (hour == 0) {
			hour = 12;
		}
		target = Write2(target, hour);
		break;
	}
	case StrTimeSpecifier::MINUTE_DECIMAL: {
		target = Write2(target, data[4]);
		break;
	}
	case StrTimeSpecifier::SECOND_DECIMAL: {
		target = Write2(target, data[5]);
		break;
	}
	default:
		throw InternalException("Unimplemented specifier for WriteStandardSpecifier in strftime");
	}
	return target;
}

void StrfTimeFormat::FormatString(date_t date, int32_t data[7], char *target) {
	idx_t i;
	for (i = 0; i < specifiers.size(); i++) {
		// first copy the current literal
		memcpy(target, literals[i].c_str(), literals[i].size());
		target += literals[i].size();
		// now copy the specifier
		if (is_date_specifier[i]) {
			target = WriteDateSpecifier(specifiers[i], date, target);
		} else {
			target = WriteStandardSpecifier(specifiers[i], data, target);
		}
	}
	// copy the final literal into the target
	memcpy(target, literals[i].c_str(), literals[i].size());
}

void StrfTimeFormat::FormatString(date_t date, dtime_t time, char *target) {
	int32_t data[7]; // year, month, day, hour, min, sec, msec
	Date::Convert(date, data[0], data[1], data[2]);
	Time::Convert(time, data[3], data[4], data[5], data[6]);

	FormatString(date, data, target);
}

string StrTimeFormat::ParseFormatSpecifier(string format_string, StrTimeFormat &format) {
	format.specifiers.clear();
	format.literals.clear();
	format.numeric_width.clear();
	format.constant_size = 0;
	idx_t pos = 0;
	string current_literal;
	for (idx_t i = 0; i < format_string.size(); i++) {
		if (format_string[i] == '%') {
			if (i + 1 == format_string.size()) {
				return "Trailing format character %";
			}
			if (i > pos) {
				// push the previous string to the current literal
				current_literal += format_string.substr(pos, i - pos);
			}
			char format_char = format_string[++i];
			if (format_char == '%') {
				// special case: %%
				// set the pos for the next literal and continue
				pos = i;
				continue;
			}
			StrTimeSpecifier specifier;
			if (format_char == '-' && i + 1 < format_string.size()) {
				format_char = format_string[++i];
				switch (format_char) {
				case 'd':
					specifier = StrTimeSpecifier::DAY_OF_MONTH;
					break;
				case 'm':
					specifier = StrTimeSpecifier::MONTH_DECIMAL;
					break;
				case 'y':
					specifier = StrTimeSpecifier::YEAR_WITHOUT_CENTURY;
					break;
				case 'H':
					specifier = StrTimeSpecifier::HOUR_24_DECIMAL;
					break;
				case 'I':
					specifier = StrTimeSpecifier::HOUR_12_DECIMAL;
					break;
				case 'M':
					specifier = StrTimeSpecifier::MINUTE_DECIMAL;
					break;
				case 'S':
					specifier = StrTimeSpecifier::SECOND_DECIMAL;
					break;
				case 'j':
					specifier = StrTimeSpecifier::DAY_OF_YEAR_DECIMAL;
					break;
				default:
					return "Unrecognized format for strftime/strptime: %-" + string(1, format_char);
				}
			} else {
				switch (format_char) {
				case 'a':
					specifier = StrTimeSpecifier::ABBREVIATED_WEEKDAY_NAME;
					break;
				case 'A':
					specifier = StrTimeSpecifier::FULL_WEEKDAY_NAME;
					break;
				case 'w':
					specifier = StrTimeSpecifier::WEEKDAY_DECIMAL;
					break;
				case 'd':
					specifier = StrTimeSpecifier::DAY_OF_MONTH_PADDED;
					break;
				case 'h':
				case 'b':
					specifier = StrTimeSpecifier::ABBREVIATED_MONTH_NAME;
					break;
				case 'B':
					specifier = StrTimeSpecifier::FULL_MONTH_NAME;
					break;
				case 'm':
					specifier = StrTimeSpecifier::MONTH_DECIMAL_PADDED;
					break;
				case 'y':
					specifier = StrTimeSpecifier::YEAR_WITHOUT_CENTURY_PADDED;
					break;
				case 'Y':
					specifier = StrTimeSpecifier::YEAR_DECIMAL;
					break;
				case 'H':
					specifier = StrTimeSpecifier::HOUR_24_PADDED;
					break;
				case 'I':
					specifier = StrTimeSpecifier::HOUR_12_PADDED;
					break;
				case 'p':
					specifier = StrTimeSpecifier::AM_PM;
					break;
				case 'M':
					specifier = StrTimeSpecifier::MINUTE_PADDED;
					break;
				case 'S':
					specifier = StrTimeSpecifier::SECOND_PADDED;
					break;
				case 'f':
					specifier = StrTimeSpecifier::MICROSECOND_PADDED;
					break;
				case 'g':
					specifier = StrTimeSpecifier::MILLISECOND_PADDED;
					break;
				case 'z':
					specifier = StrTimeSpecifier::UTC_OFFSET;
					break;
				case 'Z':
					specifier = StrTimeSpecifier::TZ_NAME;
					break;
				case 'j':
					specifier = StrTimeSpecifier::DAY_OF_YEAR_PADDED;
					break;
				case 'U':
					specifier = StrTimeSpecifier::WEEK_NUMBER_PADDED_SUN_FIRST;
					break;
				case 'W':
					specifier = StrTimeSpecifier::WEEK_NUMBER_PADDED_MON_FIRST;
					break;
				case 'c':
				case 'x':
				case 'X': {
					string subformat;
					if (format_char == 'c') {
						// %c: Locale’s appropriate date and time representation.
						// we push the ISO timestamp representation here
						subformat = "%Y-%m-%d %H:%M:%S";
					} else if (format_char == 'x') {
						// %x - Locale’s appropriate date representation.
						// we push the ISO date format here
						subformat = "%Y-%m-%d";
					} else if (format_char == 'X') {
						// %X - Locale’s appropriate time representation.
						// we push the ISO time format here
						subformat = "%H:%M:%S";
					}
					// parse the subformat in a separate format specifier
					StrfTimeFormat locale_format;
					string error = StrTimeFormat::ParseFormatSpecifier(subformat, locale_format);
					D_ASSERT(error.empty());
					// add the previous literal to the first literal of the subformat
					locale_format.literals[0] = move(current_literal) + locale_format.literals[0];
					current_literal = "";
					// now push the subformat into the current format specifier
					for (idx_t i = 0; i < locale_format.specifiers.size(); i++) {
						format.AddFormatSpecifier(move(locale_format.literals[i]), locale_format.specifiers[i]);
					}
					pos = i + 1;
					continue;
				}
				default:
					return "Unrecognized format for strftime/strptime: %" + string(1, format_char);
				}
			}
			format.AddFormatSpecifier(move(current_literal), specifier);
			current_literal = "";
			pos = i + 1;
		}
	}
	// add the final literal
	if (pos < format_string.size()) {
		current_literal += format_string.substr(pos, format_string.size() - pos);
	}
	format.AddLiteral(move(current_literal));
	return string();
}

struct StrfTimeBindData : public FunctionData {
	explicit StrfTimeBindData(StrfTimeFormat format) : format(move(format)) {
	}

	StrfTimeFormat format;

	unique_ptr<FunctionData> Copy() override {
		return make_unique<StrfTimeBindData>(format);
	}
};

static unique_ptr<FunctionData> StrfTimeBindFunction(ClientContext &context, ScalarFunction &bound_function,
                                                     vector<unique_ptr<Expression>> &arguments) {
	if (!arguments[1]->IsScalar()) {
		throw InvalidInputException("strftime format must be a constant");
	}
	Value options_str = ExpressionExecutor::EvaluateScalar(*arguments[1]);
	StrfTimeFormat format;
	if (!options_str.is_null && options_str.type().id() == LogicalTypeId::VARCHAR) {
		auto format_string = options_str.GetValue<string>();
		string error = StrTimeFormat::ParseFormatSpecifier(format_string, format);
		if (!error.empty()) {
			throw InvalidInputException("Failed to parse format specifier %s: %s", format_string, error);
		}
	}
	return make_unique<StrfTimeBindData>(format);
}

static void StrfTimeFunctionDate(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = (BoundFunctionExpression &)state.expr;
	auto &info = (StrfTimeBindData &)*func_expr.bind_info;

	if (ConstantVector::IsNull(args.data[1])) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
		ConstantVector::SetNull(result, true);
		return;
	}

	dtime_t time(0);
	UnaryExecutor::Execute<date_t, string_t>(args.data[0], result, args.size(), [&](date_t date) {
		idx_t len = info.format.GetLength(date, time);
		string_t target = StringVector::EmptyString(result, len);
		info.format.FormatString(date, time, target.GetDataWriteable());
		target.Finalize();
		return target;
	});
}

static void StrfTimeFunctionTimestamp(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = (BoundFunctionExpression &)state.expr;
	auto &info = (StrfTimeBindData &)*func_expr.bind_info;

	if (ConstantVector::IsNull(args.data[1])) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
		ConstantVector::SetNull(result, true);
		return;
	}

	UnaryExecutor::Execute<timestamp_t, string_t>(args.data[0], result, args.size(), [&](timestamp_t timestamp) {
		date_t date;
		dtime_t time;
		Timestamp::Convert(timestamp, date, time);
		idx_t len = info.format.GetLength(date, time);
		string_t target = StringVector::EmptyString(result, len);
		info.format.FormatString(date, time, target.GetDataWriteable());
		target.Finalize();
		return target;
	});
}

void StrfTimeFun::RegisterFunction(BuiltinFunctions &set) {
	ScalarFunctionSet strftime("strftime");

	strftime.AddFunction(ScalarFunction({LogicalType::DATE, LogicalType::VARCHAR}, LogicalType::VARCHAR,
	                                    StrfTimeFunctionDate, false, StrfTimeBindFunction));

	strftime.AddFunction(ScalarFunction({LogicalType::TIMESTAMP, LogicalType::VARCHAR}, LogicalType::VARCHAR,
	                                    StrfTimeFunctionTimestamp, false, StrfTimeBindFunction));

	set.AddFunction(strftime);
}

void StrpTimeFormat::AddFormatSpecifier(string preceding_literal, StrTimeSpecifier specifier) {
	numeric_width.push_back(NumericSpecifierWidth(specifier));
	StrTimeFormat::AddFormatSpecifier(move(preceding_literal), specifier);
}

int StrpTimeFormat::NumericSpecifierWidth(StrTimeSpecifier specifier) {
	switch (specifier) {
	case StrTimeSpecifier::WEEKDAY_DECIMAL:
		return 1;
	case StrTimeSpecifier::DAY_OF_MONTH_PADDED:
	case StrTimeSpecifier::DAY_OF_MONTH:
	case StrTimeSpecifier::MONTH_DECIMAL_PADDED:
	case StrTimeSpecifier::MONTH_DECIMAL:
	case StrTimeSpecifier::YEAR_WITHOUT_CENTURY_PADDED:
	case StrTimeSpecifier::YEAR_WITHOUT_CENTURY:
	case StrTimeSpecifier::HOUR_24_PADDED:
	case StrTimeSpecifier::HOUR_24_DECIMAL:
	case StrTimeSpecifier::HOUR_12_PADDED:
	case StrTimeSpecifier::HOUR_12_DECIMAL:
	case StrTimeSpecifier::MINUTE_PADDED:
	case StrTimeSpecifier::MINUTE_DECIMAL:
	case StrTimeSpecifier::SECOND_PADDED:
	case StrTimeSpecifier::SECOND_DECIMAL:
	case StrTimeSpecifier::WEEK_NUMBER_PADDED_SUN_FIRST:
	case StrTimeSpecifier::WEEK_NUMBER_PADDED_MON_FIRST:
		return 2;
	case StrTimeSpecifier::MILLISECOND_PADDED:
	case StrTimeSpecifier::DAY_OF_YEAR_PADDED:
	case StrTimeSpecifier::DAY_OF_YEAR_DECIMAL:
		return 3;
	case StrTimeSpecifier::YEAR_DECIMAL:
		return 4;
	case StrTimeSpecifier::MICROSECOND_PADDED:
		return 6;
	default:
		return -1;
	}
}

enum class TimeSpecifierAMOrPM : uint8_t { TIME_SPECIFIER_NONE = 0, TIME_SPECIFIER_AM = 1, TIME_SPECIFIER_PM = 2 };

int32_t StrpTimeFormat::TryParseCollection(const char *data, idx_t &pos, idx_t size, const string_t collection[],
                                           idx_t collection_count) {
	for (idx_t c = 0; c < collection_count; c++) {
		auto &entry = collection[c];
		auto entry_data = entry.GetDataUnsafe();
		auto entry_size = entry.GetSize();
		// check if this entry matches
		if (pos + entry_size > size) {
			// too big: can't match
			continue;
		}
		// compare the characters
		idx_t i;
		for (i = 0; i < entry_size; i++) {
			if (std::tolower(entry_data[i]) != std::tolower(data[pos + i])) {
				break;
			}
		}
		if (i == entry_size) {
			// full match
			pos += entry_size;
			return c;
		}
	}
	return -1;
}

//! Parses a timestamp using the given specifier
bool StrpTimeFormat::Parse(string_t str, ParseResult &result) {
	auto &result_data = result.data;
	auto &error_message = result.error_message;
	auto &error_position = result.error_position;

	// initialize the result
	result_data[0] = 1900;
	result_data[1] = 1;
	result_data[2] = 1;
	result_data[3] = 0;
	result_data[4] = 0;
	result_data[5] = 0;
	result_data[6] = 0;

	auto data = str.GetDataUnsafe();
	idx_t size = str.GetSize();
	// skip leading spaces
	while (StringUtil::CharacterIsSpace(*data)) {
		data++;
		size--;
	}
	idx_t pos = 0;
	TimeSpecifierAMOrPM ampm = TimeSpecifierAMOrPM::TIME_SPECIFIER_NONE;

	// Year offset state (Year+W/j)
	auto offset_specifier = StrTimeSpecifier::WEEKDAY_DECIMAL;
	uint64_t weekno = 0;
	uint64_t weekday = 0;
	uint64_t yearday = 0;

	for (idx_t i = 0;; i++) {
		// first compare the literal
		if (literals[i].size() > (size - pos) || memcmp(data + pos, literals[i].c_str(), literals[i].size()) != 0) {
			// literal does not match
			error_message = "Literal does not match, expected " + literals[i];
			error_position = pos;
			return false;
		}
		pos += literals[i].size();
		if (i == specifiers.size()) {
			break;
		}
		// now parse the specifier
		if (numeric_width[i] > 0) {
			// numeric specifier: parse a number
			uint64_t number = 0;
			size_t start_pos = pos;
			size_t end_pos = start_pos + numeric_width[i];
			while (pos < size && pos < end_pos && StringUtil::CharacterIsDigit(data[pos])) {
				number = number * 10 + data[pos] - '0';
				pos++;
			}
			if (pos == start_pos) {
				// expected a number here
				error_message = "Expected a number";
				error_position = start_pos;
				return false;
			}
			switch (specifiers[i]) {
			case StrTimeSpecifier::DAY_OF_MONTH_PADDED:
			case StrTimeSpecifier::DAY_OF_MONTH:
				if (number < 1 || number > 31) {
					error_message = "Day out of range, expected a value between 1 and 31";
					error_position = start_pos;
					return false;
				}
				// day of the month
				result_data[2] = number;
				offset_specifier = specifiers[i];
				break;
			case StrTimeSpecifier::MONTH_DECIMAL_PADDED:
			case StrTimeSpecifier::MONTH_DECIMAL:
				if (number < 1 || number > 12) {
					error_message = "Month out of range, expected a value between 1 and 12";
					error_position = start_pos;
					return false;
				}
				// month number
				result_data[1] = number;
				offset_specifier = specifiers[i];
				break;
			case StrTimeSpecifier::YEAR_WITHOUT_CENTURY_PADDED:
			case StrTimeSpecifier::YEAR_WITHOUT_CENTURY:
				// year without century..
				// Python uses 69 as a crossover point (i.e. >= 69 is 19.., < 69 is 20..)
				if (number >= 100) {
					// %y only supports numbers between [0..99]
					error_message = "Year without century out of range, expected a value between 0 and 99";
					error_position = start_pos;
					return false;
				}
				if (number >= 69) {
					result_data[0] = int32_t(1900 + number);
				} else {
					result_data[0] = int32_t(2000 + number);
				}
				break;
			case StrTimeSpecifier::YEAR_DECIMAL:
				// year as full number
				result_data[0] = number;
				break;
			case StrTimeSpecifier::HOUR_24_PADDED:
			case StrTimeSpecifier::HOUR_24_DECIMAL:
				if (number >= 24) {
					error_message = "Hour out of range, expected a value between 0 and 23";
					error_position = start_pos;
					return false;
				}
				// hour as full number
				result_data[3] = number;
				break;
			case StrTimeSpecifier::HOUR_12_PADDED:
			case StrTimeSpecifier::HOUR_12_DECIMAL:
				if (number < 1 || number > 12) {
					error_message = "Hour12 out of range, expected a value between 1 and 12";
					error_position = start_pos;
					return false;
				}
				// 12-hour number: start off by just storing the number
				result_data[3] = number;
				break;
			case StrTimeSpecifier::MINUTE_PADDED:
			case StrTimeSpecifier::MINUTE_DECIMAL:
				if (number >= 60) {
					error_message = "Minutes out of range, expected a value between 0 and 59";
					error_position = start_pos;
					return false;
				}
				// minutes
				result_data[4] = number;
				break;
			case StrTimeSpecifier::SECOND_PADDED:
			case StrTimeSpecifier::SECOND_DECIMAL:
				if (number >= 60) {
					error_message = "Seconds out of range, expected a value between 0 and 59";
					error_position = start_pos;
					return false;
				}
				// seconds
				result_data[5] = number;
				break;
			case StrTimeSpecifier::MICROSECOND_PADDED:
				if (number >= 1000000ULL) {
					error_message = "Microseconds out of range, expected a value between 0 and 999999";
					error_position = start_pos;
					return false;
				}
				// milliseconds
				result_data[6] = number;
				break;
			case StrTimeSpecifier::MILLISECOND_PADDED:
				if (number >= 1000ULL) {
					error_message = "Milliseconds out of range, expected a value between 0 and 999";
					error_position = start_pos;
					return false;
				}
				// milliseconds
				result_data[6] = number * 1000;
				break;
			case StrTimeSpecifier::WEEK_NUMBER_PADDED_SUN_FIRST:
			case StrTimeSpecifier::WEEK_NUMBER_PADDED_MON_FIRST:
				// m/d overrides WU/w but does not conflict
				switch (offset_specifier) {
				case StrTimeSpecifier::DAY_OF_MONTH_PADDED:
				case StrTimeSpecifier::DAY_OF_MONTH:
				case StrTimeSpecifier::MONTH_DECIMAL_PADDED:
				case StrTimeSpecifier::MONTH_DECIMAL:
					// Just validate, don't use
					break;
				case StrTimeSpecifier::WEEKDAY_DECIMAL:
					// First offset specifier
					offset_specifier = specifiers[i];
					break;
				default:
					error_message = "Multiple year offsets specified";
					error_position = start_pos;
					return false;
				}
				if (number > 53) {
					error_message = "Week out of range, expected a value between 0 and 53";
					error_position = start_pos;
					return false;
				}
				weekno = number;
				break;
			case StrTimeSpecifier::WEEKDAY_DECIMAL:
				if (number > 6) {
					error_message = "Weekday out of range, expected a value between 0 and 6";
					error_position = start_pos;
					return false;
				}
				weekday = number;
				break;
			case StrTimeSpecifier::DAY_OF_YEAR_PADDED:
			case StrTimeSpecifier::DAY_OF_YEAR_DECIMAL:
				// m/d overrides j but does not conflict
				switch (offset_specifier) {
				case StrTimeSpecifier::DAY_OF_MONTH_PADDED:
				case StrTimeSpecifier::DAY_OF_MONTH:
				case StrTimeSpecifier::MONTH_DECIMAL_PADDED:
				case StrTimeSpecifier::MONTH_DECIMAL:
					// Just validate, don't use
					break;
				case StrTimeSpecifier::WEEKDAY_DECIMAL:
					// First offset specifier
					offset_specifier = specifiers[i];
					break;
				default:
					error_message = "Multiple year offsets specified";
					error_position = start_pos;
					return false;
				}
				if (number < 1 || number > 366) {
					error_message = "Year day out of range, expected a value between 1 and 366";
					error_position = start_pos;
					return false;
				}
				yearday = number;
				break;
			default:
				throw NotImplementedException("Unsupported specifier for strptime");
			}
		} else {
			switch (specifiers[i]) {
			case StrTimeSpecifier::AM_PM: {
				// parse the next 2 characters
				if (pos + 2 > size) {
					// no characters left to parse
					error_message = "Expected AM/PM";
					error_position = pos;
					return false;
				}
				char pa_char = char(std::tolower(data[pos]));
				char m_char = char(std::tolower(data[pos + 1]));
				if (m_char != 'm') {
					error_message = "Expected AM/PM";
					error_position = pos;
					return false;
				}
				if (pa_char == 'p') {
					ampm = TimeSpecifierAMOrPM::TIME_SPECIFIER_PM;
				} else if (pa_char == 'a') {
					ampm = TimeSpecifierAMOrPM::TIME_SPECIFIER_AM;
				} else {
					error_message = "Expected AM/PM";
					error_position = pos;
					return false;
				}
				pos += 2;
				break;
			}
			// we parse weekday names, but we don't use them as information
			case StrTimeSpecifier::ABBREVIATED_WEEKDAY_NAME:
				if (TryParseCollection(data, pos, size, Date::DAY_NAMES_ABBREVIATED, 7) < 0) {
					error_message = "Expected an abbreviated day name (Mon, Tue, Wed, Thu, Fri, Sat, Sun)";
					error_position = pos;
					return false;
				}
				break;
			case StrTimeSpecifier::FULL_WEEKDAY_NAME:
				if (TryParseCollection(data, pos, size, Date::DAY_NAMES, 7) < 0) {
					error_message = "Expected a full day name (Monday, Tuesday, etc...)";
					error_position = pos;
					return false;
				}
				break;
			case StrTimeSpecifier::ABBREVIATED_MONTH_NAME: {
				int32_t month = TryParseCollection(data, pos, size, Date::MONTH_NAMES_ABBREVIATED, 12);
				if (month < 0) {
					error_message = "Expected an abbreviated month name (Jan, Feb, Mar, etc..)";
					error_position = pos;
					return false;
				}
				result_data[1] = month + 1;
				break;
			}
			case StrTimeSpecifier::FULL_MONTH_NAME: {
				int32_t month = TryParseCollection(data, pos, size, Date::MONTH_NAMES, 12);
				if (month < 0) {
					error_message = "Expected a full month name (January, February, etc...)";
					error_position = pos;
					return false;
				}
				result_data[1] = month + 1;
				break;
			}
			case StrTimeSpecifier::UTC_OFFSET: {
				int hour_offset, minute_offset;
				if (!Timestamp::TryParseUTCOffset(data, pos, size, hour_offset, minute_offset)) {
					error_message = "Expected +HH[MM] or -HH[MM]";
					error_position = pos;
					return false;
				}
				result_data[3] -= hour_offset;
				result_data[4] -= minute_offset;
				break;
			}

			default:
				throw NotImplementedException("Unsupported specifier for strptime");
			}
		}
	}
	// skip trailing spaces
	while (pos < size && StringUtil::CharacterIsSpace(data[pos])) {
		pos++;
	}
	if (pos != size) {
		error_message = "Full specifier did not match: trailing characters";
		error_position = pos;
		return false;
	}
	if (ampm != TimeSpecifierAMOrPM::TIME_SPECIFIER_NONE) {
		if (result_data[3] > 12) {
			error_message =
			    "Invalid hour: " + to_string(result_data[3]) + " AM/PM, expected an hour within the range [0..12]";
			return false;
		}
		// adjust the hours based on the AM or PM specifier
		if (ampm == TimeSpecifierAMOrPM::TIME_SPECIFIER_AM) {
			// AM: 12AM=0, 1AM=1, 2AM=2, ..., 11AM=11
			if (result_data[3] == 12) {
				result_data[3] = 0;
			}
		} else {
			// PM: 12PM=12, 1PM=13, 2PM=14, ..., 11PM=23
			if (result_data[3] != 12) {
				result_data[3] += 12;
			}
		}
	}
	switch (offset_specifier) {
	case StrTimeSpecifier::WEEK_NUMBER_PADDED_SUN_FIRST:
	case StrTimeSpecifier::WEEK_NUMBER_PADDED_MON_FIRST: {
		// Adjust weekday to be 0-based for the week type
		weekday = (weekday + 7 - int(offset_specifier == StrTimeSpecifier::WEEK_NUMBER_PADDED_MON_FIRST)) % 7;
		// Get the start of week 1, move back 7 days and then weekno * 7 + weekday gives the date
		const auto jan1 = Date::FromDate(result_data[0], 1, 1);
		auto yeardate = Date::GetMondayOfCurrentWeek(jan1);
		yeardate -= int(offset_specifier == StrTimeSpecifier::WEEK_NUMBER_PADDED_SUN_FIRST);
		// Is there a week 0?
		yeardate -= 7 * int(yeardate >= jan1);
		yeardate += weekno * 7 + weekday;
		Date::Convert(yeardate, result_data[0], result_data[1], result_data[2]);
		break;
	}
	case StrTimeSpecifier::DAY_OF_YEAR_PADDED:
	case StrTimeSpecifier::DAY_OF_YEAR_DECIMAL: {
		auto yeardate = Date::FromDate(result_data[0], 1, 1);
		yeardate += yearday - 1;
		Date::Convert(yeardate, result_data[0], result_data[1], result_data[2]);
		break;
	}
	case StrTimeSpecifier::DAY_OF_MONTH_PADDED:
	case StrTimeSpecifier::DAY_OF_MONTH:
	case StrTimeSpecifier::MONTH_DECIMAL_PADDED:
	case StrTimeSpecifier::MONTH_DECIMAL:
		// m/d overrides UWw/j
		break;
	default:
		D_ASSERT(offset_specifier == StrTimeSpecifier::WEEKDAY_DECIMAL);
		break;
	}

	return true;
}

struct StrpTimeBindData : public FunctionData {
	explicit StrpTimeBindData(StrpTimeFormat format) : format(move(format)) {
	}

	StrpTimeFormat format;

	unique_ptr<FunctionData> Copy() override {
		return make_unique<StrpTimeBindData>(format);
	}
};

static unique_ptr<FunctionData> StrpTimeBindFunction(ClientContext &context, ScalarFunction &bound_function,
                                                     vector<unique_ptr<Expression>> &arguments) {
	if (!arguments[1]->IsScalar()) {
		throw InvalidInputException("strftime format must be a constant");
	}
	Value options_str = ExpressionExecutor::EvaluateScalar(*arguments[1]);
	StrpTimeFormat format;
	if (!options_str.is_null && options_str.type().id() == LogicalTypeId::VARCHAR) {
		string format_string = options_str.ToString();
		format.format_specifier = format_string;
		string error = StrTimeFormat::ParseFormatSpecifier(format_string, format);
		if (!error.empty()) {
			throw InvalidInputException("Failed to parse format specifier %s: %s", format_string, error);
		}
	}
	return make_unique<StrpTimeBindData>(format);
}

string StrpTimeFormat::FormatStrpTimeError(const string &input, idx_t position) {
	if (position == INVALID_INDEX) {
		return string();
	}
	return input + "\n" + string(position, ' ') + "^";
}

date_t StrpTimeFormat::ParseDate(string_t input) {
	ParseResult result;
	if (!Parse(input, result)) {
		throw InvalidInputException(
		    "Could not parse string \"%s\" according to format specifier \"%s\"\n%s\nError: %s", input.GetString(),
		    format_specifier, FormatStrpTimeError(input.GetString(), result.error_position), result.error_message);
	}
	return Date::FromDate(result.data[0], result.data[1], result.data[2]);
}

timestamp_t StrpTimeFormat::ParseTimestamp(string_t input) {
	ParseResult result;
	if (!Parse(input, result)) {
		throw InvalidInputException(
		    "Could not parse string \"%s\" according to format specifier \"%s\"\n%s\nError: %s", input.GetString(),
		    format_specifier, FormatStrpTimeError(input.GetString(), result.error_position), result.error_message);
	}
	date_t date = Date::FromDate(result.data[0], result.data[1], result.data[2]);
	dtime_t time = Time::FromTime(result.data[3], result.data[4], result.data[5], result.data[6]);
	return Timestamp::FromDatetime(date, time);
}

static void StrpTimeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = (BoundFunctionExpression &)state.expr;
	auto &info = (StrpTimeBindData &)*func_expr.bind_info;

	if (ConstantVector::IsNull(args.data[1])) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
		ConstantVector::SetNull(result, true);
		return;
	}
	UnaryExecutor::Execute<string_t, timestamp_t>(args.data[0], result, args.size(),
	                                              [&](string_t input) { return info.format.ParseTimestamp(input); });
}

void StrpTimeFun::RegisterFunction(BuiltinFunctions &set) {
	ScalarFunctionSet strptime("strptime");

	strptime.AddFunction(ScalarFunction({LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::TIMESTAMP,
	                                    StrpTimeFunction, false, StrpTimeBindFunction));

	set.AddFunction(strptime);
}

} // namespace duckdb
