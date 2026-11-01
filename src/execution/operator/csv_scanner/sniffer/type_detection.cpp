#include "duckdb/common/algorithm.hpp"
#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/common/exception/conversion_exception.hpp"
#include "duckdb/common/operator/decimal_cast_operators.hpp"
#include "duckdb/common/operator/double_cast_operator.hpp"
#include "duckdb/common/operator/integer_cast_operator.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/types/bignum.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/execution/operator/csv_scanner/sniffer/csv_sniffer.hpp"

namespace duckdb {
struct TryCastFloatingOperator {
	template <class OP, class T>
	static bool Operation(string_t input) {
		T result;
		string error_message;
		CastParameters parameters(false, &error_message);
		return OP::Operation(input, result, parameters);
	}
};

static bool StartsWithNumericDate(string &separator, const string_t &value) {
	auto begin = value.GetData();
	auto end = begin + value.GetSize();

	//	StrpTimeFormat::Parse will skip whitespace, so we can too
	auto field1 = std::find_if_not(begin, end, StringUtil::CharacterIsSpace);
	if (field1 == end) {
		return false;
	}

	//	first numeric field must start immediately
	if (!StringUtil::CharacterIsDigit(*field1)) {
		return false;
	}
	auto literal1 = std::find_if_not(field1, end, StringUtil::CharacterIsDigit);
	if (literal1 == end) {
		return false;
	}

	//	second numeric field must exist
	auto field2 = std::find_if(literal1, end, StringUtil::CharacterIsDigit);
	if (field2 == end) {
		return false;
	}
	auto literal2 = std::find_if_not(field2, end, StringUtil::CharacterIsDigit);
	if (literal2 == end) {
		return false;
	}

	//	third numeric field must exist
	auto field3 = std::find_if(literal2, end, StringUtil::CharacterIsDigit);
	if (field3 == end) {
		return false;
	}

	//	second literal must match first
	if (((field3 - literal2) != (field2 - literal1)) ||
	    strncmp(literal1, literal2, NumericCast<size_t>((field2 - literal1))) != 0) {
		return false;
	}

	//	copy the literal as the separator, escaping percent signs
	separator.clear();
	while (literal1 < field2) {
		const auto literal_char = *literal1++;
		if (literal_char == '%') {
			separator.push_back(literal_char);
		}
		separator.push_back(literal_char);
	}

	return true;
}

string GenerateDateFormat(const string &separator, const char *format_template) {
	string format_specifier = format_template;
	const auto amount_of_dashes = NumericCast<idx_t>(std::count(format_specifier.begin(), format_specifier.end(), '-'));
	// All our date formats must have at least one -
	D_ASSERT(amount_of_dashes);
	string result;
	result.reserve(format_specifier.size() - amount_of_dashes + (amount_of_dashes * separator.size()));
	for (const auto &character : format_specifier) {
		if (character == '-') {
			result += separator;
		} else {
			result += character;
		}
	}
	return result;
}

void CSVSniffer::SetDateFormat(CSVStateMachine &candidate, const string &format_specifier,
                               const LogicalTypeId &sql_type) {
	StrpTimeFormat strpformat;
	StrTimeFormat::ParseFormatSpecifier(format_specifier, strpformat);
	candidate.dialect_options.date_format[sql_type].Set(strpformat, false);
}

idx_t CSVSniffer::LinesSniffed() const {
	return lines_sniffed;
}

bool CSVSniffer::EmptyOrOnlyHeader() const {
	return (single_row_file && best_candidate->state_machine->dialect_options.header.GetValue()) || lines_sniffed == 0;
}

bool CSVSniffer::CanYouCastBignum(const char *value_ptr, idx_t value_size) {
	while (value_size > 0 && StringUtil::CharacterIsSpace(*value_ptr)) {
		value_ptr++;
		value_size--;
	}
	if (value_size == 0 || *value_ptr == '+') {
		return false;
	}
	auto trimmed_size = value_size;
	while (trimmed_size > 0 && StringUtil::CharacterIsSpace(value_ptr[trimmed_size - 1])) {
		trimmed_size--;
	}
	if (trimmed_size == 0) {
		return false;
	}
	idx_t digit_pos = value_ptr[0] == '-' ? 1 : 0;
	if (digit_pos == trimmed_size) {
		return false;
	}
	if (!StringUtil::CharacterIsDigit(value_ptr[digit_pos])) {
		return false;
	}
	if (digit_pos + 1 < trimmed_size && value_ptr[digit_pos] == '0' &&
	    StringUtil::CharacterIsDigit(value_ptr[digit_pos + 1])) {
		return false;
	}
	for (idx_t pos = digit_pos + 1; pos < trimmed_size; pos++) {
		if (!StringUtil::CharacterIsDigit(value_ptr[pos])) {
			return false;
		}
	}
	try {
		Bignum::VarcharToBignum(string_t(value_ptr, NumericCast<uint32_t>(trimmed_size)));
		return true;
	} catch (const ConversionException &) {
		return false;
	}
}

bool CSVSniffer::CanYouCastIt(ClientContext &context, const string_t value, const LogicalType &type,
                              const DialectOptions &dialect_options, const bool is_null, const char decimal_separator,
                              const char thousands_separator) {
	if (is_null) {
		return true;
	}
	auto value_ptr = value.GetData();
	auto value_size = value.GetSize();
	string strip_thousands;
	if ((type.IsNumeric() || type.id() == LogicalTypeId::BIGNUM) && thousands_separator != '\0') {
		// If we have a thousands separator we should try to use that
		strip_thousands = BaseScanner::RemoveSeparator(value_ptr, value_size, thousands_separator);
		value_ptr = strip_thousands.c_str();
		value_size = strip_thousands.size();
	}
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN: {
		bool dummy_value;
		return TryCastStringBool(value_ptr, value_size, dummy_value, true);
	}
	case LogicalTypeId::TINYINT: {
		int8_t dummy_value;
		return TrySimpleIntegerCast(value_ptr, value_size, dummy_value, false);
	}
	case LogicalTypeId::SMALLINT: {
		int16_t dummy_value;
		return TrySimpleIntegerCast(value_ptr, value_size, dummy_value, true);
	}
	case LogicalTypeId::INTEGER: {
		int32_t dummy_value;
		return TrySimpleIntegerCast(value_ptr, value_size, dummy_value, true);
	}
	case LogicalTypeId::BIGINT: {
		int64_t dummy_value;
		return TrySimpleIntegerCast(value_ptr, value_size, dummy_value, true);
	}
	case LogicalTypeId::HUGEINT: {
		hugeint_t dummy_value;
		return TryCast::Operation<string_t, hugeint_t>(string_t(value_ptr, NumericCast<uint32_t>(value_size)),
		                                               dummy_value, true);
	}
	case LogicalTypeId::UTINYINT: {
		uint8_t dummy_value;
		return TrySimpleIntegerCast(value_ptr, value_size, dummy_value, true);
	}
	case LogicalTypeId::USMALLINT: {
		uint16_t dummy_value;
		return TrySimpleIntegerCast(value_ptr, value_size, dummy_value, true);
	}
	case LogicalTypeId::UINTEGER: {
		uint32_t dummy_value;
		return TrySimpleIntegerCast(value_ptr, value_size, dummy_value, true);
	}
	case LogicalTypeId::UBIGINT: {
		uint64_t dummy_value;
		return TrySimpleIntegerCast(value_ptr, value_size, dummy_value, true);
	}
	case LogicalTypeId::UHUGEINT: {
		uhugeint_t dummy_value;
		return TryCast::Operation<string_t, uhugeint_t>(string_t(value_ptr, NumericCast<uint32_t>(value_size)),
		                                                dummy_value, true);
	}
	case LogicalTypeId::BIGNUM:
		return CanYouCastBignum(value_ptr, value_size);
	case LogicalTypeId::DOUBLE: {
		double dummy_value;
		return TryDoubleCast<double>(value_ptr, value_size, dummy_value, true, decimal_separator);
	}
	case LogicalTypeId::FLOAT: {
		float dummy_value;
		return TryDoubleCast<float>(value_ptr, value_size, dummy_value, true, decimal_separator);
	}
	case LogicalTypeId::DATE: {
		if (!dialect_options.date_format.find(LogicalTypeId::DATE)->second.GetValue().Empty()) {
			date_t result;
			string error_message;
			return dialect_options.date_format.find(LogicalTypeId::DATE)
			    ->second.GetValue()
			    .TryParseDate(value, result, error_message);
		}
		idx_t pos;
		bool special;
		date_t dummy_value;
		return Date::TryConvertDate(value_ptr, value_size, pos, dummy_value, special, true) == DateCastResult::SUCCESS;
	}
	case LogicalTypeId::TIMESTAMP: {
		timestamp_t dummy_value;
		if (!dialect_options.date_format.find(LogicalTypeId::TIMESTAMP)->second.GetValue().Empty()) {
			string error_message;
			return dialect_options.date_format.find(LogicalTypeId::TIMESTAMP)
			    ->second.GetValue()
			    .TryParseTimestamp(value, dummy_value, error_message);
		}
		return Timestamp::TryConvertTimestamp(value_ptr, value_size, dummy_value, false, nullptr, true) ==
		       TimestampCastResult::SUCCESS;
	}
	case LogicalTypeId::TIME: {
		idx_t pos;
		dtime_t dummy_value;
		return Time::TryConvertTime(value_ptr, value_size, pos, dummy_value, true);
	}
	case LogicalTypeId::DECIMAL: {
		uint8_t width, scale;
		type.GetDecimalProperties(width, scale);
		if (decimal_separator == ',') {
			switch (type.InternalType()) {
			case PhysicalType::INT16: {
				int16_t dummy_value;
				return TryDecimalStringCast<int16_t, ','>(value_ptr, value_size, dummy_value, width, scale);
			}

			case PhysicalType::INT32: {
				int32_t dummy_value;
				return TryDecimalStringCast<int32_t, ','>(value_ptr, value_size, dummy_value, width, scale);
			}

			case PhysicalType::INT64: {
				int64_t dummy_value;
				return TryDecimalStringCast<int64_t, ','>(value_ptr, value_size, dummy_value, width, scale);
			}

			case PhysicalType::INT128: {
				hugeint_t dummy_value;
				return TryDecimalStringCast<hugeint_t, ','>(value_ptr, value_size, dummy_value, width, scale);
			}

			default:
				throw InternalException("Invalid Physical Type for Decimal Value. Physical Type: " +
				                        TypeIdToString(type.InternalType()));
			}

		} else if (decimal_separator == '.') {
			switch (type.InternalType()) {
			case PhysicalType::INT16: {
				int16_t dummy_value;
				return TryDecimalStringCast(value_ptr, value_size, dummy_value, width, scale);
			}

			case PhysicalType::INT32: {
				int32_t dummy_value;
				return TryDecimalStringCast(value_ptr, value_size, dummy_value, width, scale);
			}

			case PhysicalType::INT64: {
				int64_t dummy_value;
				return TryDecimalStringCast(value_ptr, value_size, dummy_value, width, scale);
			}

			case PhysicalType::INT128: {
				hugeint_t dummy_value;
				return TryDecimalStringCast(value_ptr, value_size, dummy_value, width, scale);
			}

			default:
				throw InternalException("Invalid Physical Type for Decimal Value. Physical Type: " +
				                        TypeIdToString(type.InternalType()));
			}
		}
		throw InvalidInputException("Decimals can only have ',' and '.' as decimal separators");
	}
	case LogicalTypeId::VARCHAR:
		return true;
	default: {
		// We do Value Try Cast for non-basic types.
		string error_message;
		Value str_value(value);
		auto new_value = str_value.TryCastAs(context, type, &error_message, true);
		return new_value.has_value() && error_message.empty();
	}
	}
}

void CSVSniffer::InitializeDateAndTimeStampDetection(CSVStateMachine &candidate, const string &separator,
                                                     const LogicalType &sql_type) {
	auto &format_candidate = format_candidates[sql_type.id()];
	if (!format_candidate.initialized) {
		format_candidate.initialized = true;
		// if user set a format, we add that as well
		auto user_format = options.dialect_options.date_format.find(sql_type.id());
		if (user_format->second.IsSetByUser()) {
			format_candidate.format.emplace_back(user_format->second.GetValue().format_specifier);
		} else {
			auto entry = format_template_candidates.find(sql_type.id());
			if (entry != format_template_candidates.end()) {
				const auto &format_template_list = entry->second;
				for (const auto &t : format_template_list) {
					const auto format_string = GenerateDateFormat(separator, t);
					// don't parse ISO 8601
					if (format_string.find("%Y-%m-%d") == string::npos) {
						format_candidate.format.emplace_back(format_string);
					}
				}
			}
		}
		// order by preference
		original_format_candidates = format_candidates;
	}
	//	initialise the first candidate
	//	all formats are constructed to be valid
	SetDateFormat(candidate, format_candidate.format.back(), sql_type.id());
}

bool ValidSeparator(const string &separator) {
	// We use https://en.wikipedia.org/wiki/List_of_date_formats_by_country as reference
	return separator == "-" || separator == "." || separator == "/" || separator == " ";
}
void CSVSniffer::DetectDateAndTimeStampFormats(CSVStateMachine &candidate, const LogicalType &sql_type,
                                               const string &separator, const string_t &dummy_val) {
	if (!ValidSeparator(separator)) {
		return;
	}
	// If it is the first time running date/timestamp detection we must initialize the format variables
	InitializeDateAndTimeStampDetection(candidate, separator, sql_type);
	// generate date format candidates the first time through
	auto &type_format_candidates = format_candidates[sql_type.id()].format;
	// check all formats and keep the first one that works
	StrpTimeFormat::ParseResult result;
	auto save_format_candidates = type_format_candidates;
	const bool had_format_candidates = !save_format_candidates.empty();
	const bool initial_format_candidates =
	    save_format_candidates.size() == original_format_candidates.at(sql_type.id()).format.size();
	const bool is_set_by_user = options.dialect_options.date_format.find(sql_type.id())->second.IsSetByUser();
	while (!type_format_candidates.empty() && !is_set_by_user) {
		//	avoid using exceptions for flow control...
		auto &current_format = candidate.dialect_options.date_format[sql_type.id()].GetValue();
		if (current_format.Parse(dummy_val, result, true)) {
			format_candidates[sql_type.id()].had_match = true;
			break;
		}
		//	doesn't work - move to the next one
		type_format_candidates.pop_back();
		if (!type_format_candidates.empty()) {
			SetDateFormat(candidate, type_format_candidates.back(), sql_type.id());
		}
	}
	//	if none match, then this is not a value of type sql_type,
	if (type_format_candidates.empty()) {
		//	so restore the candidates that did work.
		//	or throw them out if they were generated by this value.
		if (had_format_candidates) {
			if (initial_format_candidates && !format_candidates[sql_type.id()].had_match) {
				// we reset the whole thing because we tried to sniff the wrong type.
				format_candidates[sql_type.id()].initialized = false;
				format_candidates[sql_type.id()].format.clear();
				SetDateFormat(candidate, "", sql_type.id());
				return;
			}
			type_format_candidates.swap(save_format_candidates);
			SetDateFormat(candidate, type_format_candidates.back(), sql_type.id());
		}
	}
}

void CSVSniffer::SniffTypes(DataChunk &data_chunk, CSVStateMachine &state_machine,
                            unordered_map<idx_t, vector<LogicalType>> &info_sql_types_candidates,
                            idx_t start_idx_detection) {
	const idx_t chunk_size = data_chunk.size();
	HasType has_type;
	for (idx_t col_idx = 0; col_idx < data_chunk.ColumnCount(); col_idx++) {
		auto &cur_vector = data_chunk.data[col_idx];
		D_ASSERT(cur_vector.GetVectorType() == VectorType::FLAT_VECTOR);
		D_ASSERT(cur_vector.GetType() == LogicalType::VARCHAR);
		auto vector_data = FlatVector::GetData<string_t>(cur_vector);
		auto null_mask = FlatVector::ValidityMutable(cur_vector);
		auto &col_type_candidates = info_sql_types_candidates[col_idx];
		for (idx_t row_idx = start_idx_detection; row_idx < chunk_size; row_idx++) {
			// col_type_candidates can't be empty since anything in a CSV file should at least be a string
			// and we validate utf-8 compatibility when creating the type
			D_ASSERT(!col_type_candidates.empty());
			auto cur_top_candidate = col_type_candidates.back();
			// try cast from string to sql_type
			while (col_type_candidates.size() > 1) {
				const auto &sql_type = col_type_candidates.back();
				// try formatting for date types if the user did not specify one, and it starts with numeric
				// values.
				string separator;
				// If Value is not Null, Has a numeric date format, and the current investigated candidate is
				// either a timestamp or a date
				if (null_mask.RowIsValid(row_idx) && StartsWithNumericDate(separator, vector_data[row_idx]) &&
				    ((col_type_candidates.back().id() == LogicalTypeId::TIMESTAMP && !has_type.timestamp) ||
				     (col_type_candidates.back().id() == LogicalTypeId::DATE && !has_type.date))) {
					DetectDateAndTimeStampFormats(state_machine, sql_type, separator, vector_data[row_idx]);
				}
				// try cast from string to sql_type
				if (sql_type == LogicalType::VARCHAR) {
					// Nothing to convert it to
					continue;
				}
				if (CanYouCastIt(buffer_manager->context, vector_data[row_idx], sql_type, state_machine.dialect_options,
				                 !null_mask.RowIsValid(row_idx), state_machine.options.decimal_separator[0],
				                 state_machine.options.thousands_separator)) {
					break;
				}
				D_ASSERT(cur_top_candidate.id() == LogicalTypeId::VARCHAR || col_type_candidates.size() > 1);
				if (row_idx != start_idx_detection &&
				    (cur_top_candidate == LogicalType::BOOLEAN || cur_top_candidate == LogicalType::DATE ||
				     cur_top_candidate == LogicalType::TIME ||
				     (cur_top_candidate == LogicalType::TIMESTAMP &&
				      col_type_candidates[col_type_candidates.size() - 2] != LogicalType::TIMESTAMP_TZ) ||
				     cur_top_candidate == LogicalType::TIMESTAMP_TZ)) {
					// If we thought this was a boolean value (i.e., T,F, True, False) and it is not, we
					// immediately pop to varchar.
					while (col_type_candidates.back() != LogicalType::VARCHAR) {
						col_type_candidates.pop_back();
					}
					break;
				}
				col_type_candidates.pop_back();
			}
		}
		if (col_type_candidates.back().id() == LogicalTypeId::DATE) {
			has_type.date = true;
		}
		if (col_type_candidates.back().id() == LogicalTypeId::TIMESTAMP) {
			has_type.timestamp = true;
		}
	}
}

// If we have a predefined date/timestamp format we set it
void CSVSniffer::SetUserDefinedDateTimeFormat(CSVStateMachine &candidate) const {
	const vector<LogicalTypeId> data_time_formats {LogicalTypeId::DATE, LogicalTypeId::TIMESTAMP};
	for (auto &date_time_format : data_time_formats) {
		auto &user_option = options.dialect_options.date_format.at(date_time_format);
		if (user_option.IsSetByUser()) {
			SetDateFormat(candidate, user_option.GetValue().format_specifier, date_time_format);
		}
	}
}
void CSVSniffer::DetectTypes() {
	idx_t min_varchar_cols = max_columns_found + 1;
	idx_t min_errors = NumericLimits<idx_t>::Maximum();
	vector<LogicalType> return_types;
	// check which info candidate leads to the minimum number of non-varchar columns...
	for (auto &candidate_cc : candidates) {
		auto &sniffing_state_machine = candidate_cc->GetStateMachine();
		unordered_map<idx_t, vector<LogicalType>> info_sql_types_candidates;
		for (idx_t i = 0; i < max_columns_found; i++) {
			info_sql_types_candidates[i] = sniffing_state_machine.options.auto_type_candidates;
		}
		D_ASSERT(max_columns_found > 0);

		// Set all return_types to VARCHAR, so we can do datatype detection based on VARCHAR values
		return_types.clear();
		return_types.assign(max_columns_found, LogicalType::VARCHAR);

		// Reset candidate for parsing
		auto candidate = candidate_cc->UpgradeToStringValueScanner();
		SetUserDefinedDateTimeFormat(*candidate->state_machine);
		// Parse chunk and read csv with info-candidate
		auto &data_chunk = candidate->ParseChunk().ToChunk();
		if (candidate->error_handler->AnyErrors() && !candidate->error_handler->HasError(MAXIMUM_LINE_SIZE) &&
		    !candidate->state_machine->options.ignore_errors.GetValue()) {
			continue;
		}
		idx_t start_idx_detection = 0;
		idx_t chunk_size = data_chunk.size();
		if (chunk_size > 1 &&
		    (!options.dialect_options.header.IsSetByUser() ||
		     (options.dialect_options.header.IsSetByUser() && options.dialect_options.header.GetValue()))) {
			// This means we have more than one row, hence we can use the first row to detect if we have a header
			start_idx_detection = 1;
		}
		// First line where we start our type detection
		SniffTypes(data_chunk, sniffing_state_machine, info_sql_types_candidates, start_idx_detection);

		// Count the number of varchar columns
		idx_t varchar_cols = 0;
		for (idx_t col = 0; col < info_sql_types_candidates.size(); col++) {
			auto &col_type_candidates = info_sql_types_candidates[col];
			// check the number of varchar columns
			const auto &col_type = col_type_candidates.back();
			if (col_type == LogicalType::VARCHAR) {
				varchar_cols++;
			}
		}

		// it's good if the dialect creates more non-varchar columns
		const bool has_less_varchar_cols = varchar_cols < min_varchar_cols;
		// but only if we sacrifice < 30% of best_num_cols.
		const bool acceptable_best_num_cols =
		    static_cast<double>(info_sql_types_candidates.size()) > static_cast<double>(max_columns_found) * 0.7;
		const idx_t number_of_errors = candidate->error_handler->GetSize();
		const bool better_strictness = best_candidate_is_strict ? !candidate->used_unstrictness : true;
		const bool acceptable_candidate = has_less_varchar_cols && acceptable_best_num_cols && better_strictness;
		// If we escaped an unquoted character when strict is false.
		if (!best_candidate ||
		    (acceptable_candidate && (!options.ignore_errors.GetValue() || number_of_errors < min_errors))) {
			min_errors = number_of_errors;
			best_header_row.clear();
			// we have a new best_options candidate
			best_candidate = std::move(candidate);
			// Count the chosen type sample and its inferred preamble once.
			const idx_t inferred_skip_rows =
			    options.dialect_options.skip_rows.IsSetByUser()
			        ? 0
			        : best_candidate->GetStateMachine().dialect_options.skip_rows.GetValue();
			lines_sniffed = chunk_size + inferred_skip_rows;
			min_varchar_cols = varchar_cols;
			best_candidate_is_strict = !best_candidate->used_unstrictness;
			best_sql_types_candidates_per_column_idx = info_sql_types_candidates;
			for (auto &format_candidate : format_candidates) {
				best_format_candidates[format_candidate.first] = format_candidate.second.format;
			}
			if (chunk_size > 0) {
				single_row_file = chunk_size == 1;
				for (idx_t col_idx = 0; col_idx < data_chunk.ColumnCount(); col_idx++) {
					auto &cur_vector = data_chunk.data[col_idx];
					auto vector_data = FlatVector::GetData<string_t>(cur_vector);
					auto null_mask = FlatVector::ValidityMutable(cur_vector);
					if (null_mask.RowIsValid(0)) {
						auto value = HeaderValue(vector_data[0]);
						best_header_row.push_back(value);
					} else {
						best_header_row.push_back({});
					}
				}
			}
		}
	}
	if (!best_candidate) {
		DialectCandidates dialect_candidates(options.dialect_options.state_machine_options);
		auto error = CSVError::SniffingError(options, dialect_candidates.Print(), max_columns_found, set_columns, true);
		error_handler->Error(error, true);
	}
	// Assert that it's all good at this point.
	D_ASSERT(best_candidate && !best_format_candidates.empty());
}

} // namespace duckdb
