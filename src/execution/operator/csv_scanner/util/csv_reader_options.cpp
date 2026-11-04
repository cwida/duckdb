#include "duckdb/execution/operator/csv_scanner/csv_reader_options.hpp"
#include "duckdb/common/bind_helpers.hpp"
#include "duckdb/common/vector_size.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/operator/multiply.hpp"
#include "duckdb/common/multi_file/multi_file_reader.hpp"
#include "duckdb/common/set.hpp"
#include "duckdb/parser/keyword_helper.hpp"

namespace duckdb {

namespace {

void VerifyTypeListsMatch(const vector<LogicalType> &columns_types, const vector<LogicalType> &override_types) {
	if (columns_types.size() != override_types.size()) {
		throw BinderException(
		    "read_csv: the 'columns' option specifies %d column(s), but 'types'/'dtypes'/'column_types' "
		    "specifies %d type(s). When both are provided they must agree. "
		    "Consider removing the 'type' option.",
		    columns_types.size(), override_types.size());
	}
	for (idx_t i = 0; i < override_types.size(); i++) {
		if (columns_types[i] != override_types[i]) {
			throw BinderException(
			    "read_csv: column type mismatch at position %d: 'columns' specifies '%s' but "
			    "'types'/'dtypes'/'column_types' specifies '%s'. When both are provided they must agree. "
			    "Consider removing the 'type' option.",
			    i + 1, columns_types[i].ToString(), override_types[i].ToString());
		}
	}
}

} // namespace

CSVReaderOptions::CSVReaderOptions(const CSVOption<char> single_byte_delimiter,
                                   const CSVOption<string> &multi_byte_delimiter) {
	if (multi_byte_delimiter.GetValue().empty()) {
		const char single_byte_value = single_byte_delimiter.GetValue();
		const string value(1, single_byte_value);
		dialect_options.state_machine_options.delimiter = value;
	} else {
		dialect_options.state_machine_options.delimiter = multi_byte_delimiter;
	}
}
static bool ParseBoolean(const Value &value, const Identifier &loption);

static bool ParseBoolean(const vector<Value> &set, const Identifier &loption) {
	if (set.empty()) {
		// no option specified: default to true
		return true;
	}
	if (set.size() > 1) {
		throw BinderException("%s expects a single argument as a boolean value (e.g. TRUE or 1)", loption);
	}
	return ParseBoolean(set[0], loption);
}

static bool ParseBoolean(const Value &value, const Identifier &loption) {
	if (value.IsNull()) {
		throw BinderException("%s expects a non-null boolean value (e.g. TRUE or 1)", loption);
	}
	if (value.type().id() == LogicalTypeId::LIST) {
		auto &children = ListValue::GetChildren(value);
		return ParseBoolean(children, loption);
	}
	if (value.type() == LogicalType::FLOAT || value.type() == LogicalType::DOUBLE ||
	    value.type().id() == LogicalTypeId::DECIMAL) {
		throw BinderException("%s expects a boolean value (e.g. TRUE or 1)", loption);
	}
	return BooleanValue::Get(value.DefaultCastAs(LogicalType::BOOLEAN));
}

static string ParseString(const Value &value, const Identifier &loption) {
	if (value.IsNull()) {
		return string();
	}
	if (value.type().id() == LogicalTypeId::LIST) {
		auto &children = ListValue::GetChildren(value);
		if (children.size() != 1) {
			throw BinderException("%s expects a single argument as a string value", loption);
		}
		return ParseString(children[0], loption);
	}
	if (value.type().id() != LogicalTypeId::VARCHAR) {
		throw BinderException("%s expects a string argument!", loption);
	}
	return value.GetValue<string>();
}

static int64_t ParseInteger(const Value &value, const Identifier &loption) {
	if (value.IsNull()) {
		throw BinderException("%s expects a non-null integer value", loption);
	}
	if (value.type().id() == LogicalTypeId::LIST) {
		auto &children = ListValue::GetChildren(value);
		if (children.size() != 1) {
			// no option specified or multiple options specified
			throw BinderException("%s expects a single argument as an integer value", loption);
		}
		return ParseInteger(children[0], loption);
	}
	return value.GetValue<int64_t>();
}

bool CSVReaderOptions::GetHeader() const {
	return this->dialect_options.header.GetValue();
}

void CSVReaderOptions::SetHeader(bool input) {
	this->dialect_options.header.Set(input);
}

void CSVReaderOptions::SetCompression(const string &compression_p) {
	this->compression = FileCompressionType(compression_p);
}

string CSVReaderOptions::GetEscape() const {
	return std::string(1, this->dialect_options.state_machine_options.escape.GetValue());
}

void CSVReaderOptions::SetEscape(const string &input) {
	auto escape_str = input;
	if (escape_str.size() > 1) {
		throw InvalidInputException("The escape option cannot exceed a size of 1 byte.");
	}
	if (escape_str.empty()) {
		escape_str = string("\0", 1);
	}
	this->dialect_options.state_machine_options.escape.Set(escape_str[0]);
}

idx_t CSVReaderOptions::GetSkipRows() const {
	return NumericCast<idx_t>(this->dialect_options.skip_rows.GetValue());
}

void CSVReaderOptions::SetSkipRows(int64_t skip_rows) {
	if (skip_rows < 0) {
		throw InvalidInputException("skip_rows option from read_csv scanner, must be equal or higher than 0");
	}
	dialect_options.skip_rows.Set(NumericCast<idx_t>(skip_rows));
}

string CSVReaderOptions::GetDelimiter() const {
	return this->dialect_options.state_machine_options.delimiter.GetValue();
}

void CSVReaderOptions::SetDelimiter(const string &input) {
	auto delim_str = StringUtil::Replace(input, "\\t", "\t");
	if (delim_str.size() > 4) {
		throw InvalidInputException("The delimiter option cannot exceed a size of 4 bytes.");
	}
	if (this->dialect_options.state_machine_options.delimiter.IsSetByUser()) {
		// we can't know in which order delim and sep were specified, so we throw an exception here
		throw BinderException("CSV Reader function option delim and sep are aliases, only one can be supplied");
	}
	this->dialect_options.state_machine_options.delimiter.Set(delim_str);
}

string CSVReaderOptions::GetQuote() const {
	return std::string(1, this->dialect_options.state_machine_options.quote.GetValue());
}

void CSVReaderOptions::SetQuote(const string &quote_p) {
	auto quote_str = quote_p;
	if (quote_str.size() > 1) {
		throw InvalidInputException("The quote option cannot exceed a size of 1 byte.");
	}
	if (quote_str.empty()) {
		quote_str = string("\0", 1);
	}
	this->dialect_options.state_machine_options.quote.Set(quote_str[0]);
}

string CSVReaderOptions::GetComment() const {
	return std::string(1, this->dialect_options.state_machine_options.comment.GetValue());
}

void CSVReaderOptions::SetComment(const string &comment_p) {
	auto comment_str = comment_p;
	if (comment_str.size() > 1) {
		throw InvalidInputException("The comment option cannot exceed a size of 1 byte.");
	}
	if (comment_str.empty()) {
		comment_str = string("\0", 1);
	}
	this->dialect_options.state_machine_options.comment.Set(comment_str[0]);
}

string CSVReaderOptions::GetNewline() const {
	switch (dialect_options.state_machine_options.new_line.GetValue()) {
	case NewLineIdentifier::CARRY_ON:
		return "\\r\\n";
	case NewLineIdentifier::SINGLE_R:
		return "\\r";
	case NewLineIdentifier::SINGLE_N:
		return "\\n";
	case NewLineIdentifier::NOT_SET:
		return "";
	default:
		throw NotImplementedException("New line type not supported");
	}
}

void CSVReaderOptions::SetNewline(const string &input) {
	if (input == "\\n") {
		dialect_options.state_machine_options.new_line.Set(NewLineIdentifier::SINGLE_N);
	} else if (input == "\\r") {
		dialect_options.state_machine_options.new_line.Set(NewLineIdentifier::SINGLE_R);
	} else if (input == "\\r\\n") {
		dialect_options.state_machine_options.new_line.Set(NewLineIdentifier::CARRY_ON);
	} else {
		throw InvalidInputException("This is not accepted as a newline: " + input);
	}
}

bool CSVReaderOptions::GetRFC4180() const {
	return this->dialect_options.state_machine_options.strict_mode.GetValue();
}

void CSVReaderOptions::SetRFC4180(bool input) {
	this->dialect_options.state_machine_options.strict_mode.Set(input);
}

bool CSVReaderOptions::IgnoreErrors() const {
	return ignore_errors.GetValue() && !store_rejects.GetValue();
}

CSVOption<char> CSVReaderOptions::GetSingleByteDelimiter() const {
	auto &delim = dialect_options.state_machine_options.delimiter;
	return CSVOption<char>(delim.GetValue()[0], delim.IsSetByUser());
}

CSVOption<string> CSVReaderOptions::GetMultiByteDelimiter() const {
	return dialect_options.state_machine_options.delimiter;
}

void CSVReaderOptions::SetDateFormat(LogicalTypeId type, const string &format, bool read_format) {
	string error;
	const string auto_format = StringUtil::Lower(format);
	if (auto_format != "auto") {
		if (read_format) {
			StrpTimeFormat strpformat;
			error = StrTimeFormat::ParseFormatSpecifier(format, strpformat);
			dialect_options.date_format[type].Set(strpformat);
		} else {
			write_date_format[type] = Value(format);
		}
		if (!error.empty()) {
			throw InvalidInputException("Could not parse DATEFORMAT: %s", error.c_str());
		}
	}
}

void CSVReaderOptions::SetReadOption(const Identifier &loption, const Value &value,
                                     const vector<Identifier> &expected_names) {
	if (SetBaseOption(loption, value)) {
		return;
	}
	if (loption == "auto_detect") {
		auto_detect = ParseBoolean(value, loption);
	} else if (loption == "sample_size") {
		const auto sample_size_option = ParseInteger(value, loption);
		if (sample_size_option < 1 && sample_size_option != -1) {
			throw BinderException("Unsupported parameter for SAMPLE_SIZE: cannot be smaller than 1");
		}
		if (sample_size_option == -1) {
			// If -1, we basically read the whole thing
			sample_size_chunks = NumericLimits<idx_t>().Maximum();
		} else {
			sample_size_chunks = NumericCast<idx_t>(sample_size_option / STANDARD_VECTOR_SIZE);
			if (sample_size_option % STANDARD_VECTOR_SIZE != 0) {
				sample_size_chunks++;
			}
		}

	} else if (loption == "skip") {
		SetSkipRows(ParseInteger(value, loption));
	} else if (loption == "max_line_size" || loption == "maximum_line_size") {
		auto line_size = ParseInteger(value, loption);
		if (line_size < 0) {
			throw BinderException("Invalid value for MAX_LINE_SIZE parameter: it cannot be smaller than 0");
		}
		maximum_line_size.Set(NumericCast<idx_t>(line_size));
		if (buffer_size_option.IsSetByUser() && maximum_line_size.GetValue() > buffer_size_option.GetValue()) {
			throw InvalidInputException("Buffer Size of %d must be a higher value than the maximum line size %d",
			                            buffer_size_option.GetValue(), maximum_line_size.GetValue());
		}
	} else if (loption == "date_format" || loption == "dateformat") {
		string format = ParseString(value, loption);
		SetDateFormat(LogicalTypeId::DATE, format, true);
	} else if (loption == "timestamp_format" || loption == "timestampformat") {
		string format = ParseString(value, loption);
		SetDateFormat(LogicalTypeId::TIMESTAMP, format, true);
	} else if (loption == "ignore_errors") {
		ignore_errors.Set(ParseBoolean(value, loption));
	} else if (loption == "buffer_size") {
		auto buffer_size = ParseInteger(value, loption);
		if (buffer_size < 0) {
			throw InvalidInputException("Buffer Size option must be higher than 0");
		}
		buffer_size_option.Set(NumericCast<idx_t>(buffer_size));
		if (buffer_size_option == 0) {
			throw InvalidInputException("Buffer Size option must be higher than 0");
		}
		if (maximum_line_size.IsSetByUser() && maximum_line_size.GetValue() > buffer_size_option.GetValue()) {
			throw InvalidInputException("Buffer Size of %d must be a higher value than the maximum line size %d",
			                            buffer_size_option.GetValue(), maximum_line_size.GetValue());
		} else {
			maximum_line_size.Set(buffer_size_option.GetValue(), false);
		}
	} else if (loption == "decimal_separator") {
		decimal_separator = ParseString(value, loption);
		if (decimal_separator != "." && decimal_separator != ",") {
			throw BinderException("Unsupported parameter for DECIMAL_SEPARATOR: should be '.' or ','");
		}
	} else if (loption == "null_padding") {
		null_padding = ParseBoolean(value, loption);
	} else if (loption == "parallel") {
		parallel = ParseBoolean(value, loption);
	} else if (loption == "allow_quoted_nulls") {
		allow_quoted_nulls = ParseBoolean(value, loption);
	} else if (loption == "store_rejects") {
		store_rejects.Set(ParseBoolean(value, loption));
	} else if (loption == "force_not_null") {
		if (!expected_names.empty()) {
			force_not_null = ParseColumnList(value, expected_names, loption);
		} else {
			if (value.IsNull()) {
				throw BinderException("Invalid value for 'force_not_null' parameter");
			}
			// Get the list of columns to use as a recovery key
			auto &children = ListValue::GetChildren(value);
			for (auto &child : children) {
				auto col_name = child.GetValue<string>();
				force_not_null_names.insert(col_name);
			}
		}

	} else if (loption == "rejects_table") {
		// skip, handled in SetRejectsOptions
		auto table_name = ParseString(value, loption);
		if (table_name.empty()) {
			throw BinderException("REJECTS_TABLE option cannot be empty");
		}
		if (KeywordHelper::RequiresQuotes(table_name)) {
			throw BinderException("rejects_scan option: %s requires quotes to be used as an identifier", table_name);
		}
		rejects_table_name.Set(table_name);
	} else if (loption == "rejects_scan") {
		// skip, handled in SetRejectsOptions
		auto table_name = ParseString(value, loption);
		if (table_name.empty()) {
			throw BinderException("rejects_scan option cannot be empty");
		}
		if (KeywordHelper::RequiresQuotes(table_name)) {
			throw BinderException("rejects_scan option: %s requires quotes to be used as an identifier", table_name);
		}
		rejects_scan_name.Set(table_name);
	} else if (loption == "rejects_limit") {
		auto limit = ParseInteger(value, loption);
		if (limit < 0) {
			throw BinderException("Unsupported parameter for REJECTS_LIMIT: cannot be negative");
		}
		rejects_limit = NumericCast<idx_t>(limit);
	} else if (loption == "rejects_line_size_limit") {
		auto limit = ParseInteger(value, loption);
		if (limit < 0) {
			throw BinderException("Unsupported parameter for REJECTS_LINE_SIZE_LIMIT: cannot be negative");
		}
		rejects_line_size_limit = NumericCast<idx_t>(limit);
	} else if (loption == "encoding") {
		encoding = ParseString(value, loption);
	} else if (loption == "thousands") {
		if (!value.IsNull()) {
			auto thousands_separator_string = ParseString(value, loption);
			if (thousands_separator_string.size() > 1) {
				throw BinderException("Unsupported parameter for THOUSANDS: should be max one character");
			}
			if (!thousands_separator_string.empty()) {
				thousands_separator = thousands_separator_string[0];
			}
		}
	} else {
		throw BinderException("Unrecognized option for CSV reader %s", loption);
	}
}

void CSVReaderOptions::SetWriteOption(const Identifier &loption, const Value &value) {
	if (loption == "new_line") {
		// Steal this from SetBaseOption so we can write different newlines (e.g., format JSON ARRAY)
		write_newline = ParseString(value, loption);
		return;
	}

	if (SetBaseOption(loption, value, true)) {
		return;
	}

	if (loption == "force_quote") {
		force_quote = ParseColumnList(value, name_list, loption);
	} else if (loption == "date_format" || loption == "dateformat") {
		string format = ParseString(value, loption);
		SetDateFormat(LogicalTypeId::DATE, format, false);
	} else if (loption == "timestamp_format" || loption == "timestampformat") {
		string format = ParseString(value, loption);
		if (StringUtil::CIEquals(format, "iso")) {
			format = "%Y-%m-%dT%H:%M:%S.%fZ";
		}
		SetDateFormat(LogicalTypeId::TIMESTAMP, format, false);
		SetDateFormat(LogicalTypeId::TIMESTAMP_TZ, format, false);
	} else if (loption == "prefix") {
		prefix = ParseString(value, loption);
	} else if (loption == "suffix") {
		suffix = ParseString(value, loption);
	} else {
		throw BinderException("Unrecognized option CSV writer %s", loption);
	}
}

bool CSVReaderOptions::SetBaseOption(const Identifier &loption, const Value &value, bool write_option) {
	if (loption.StartsWith("delim") || loption.StartsWith("sep")) {
		SetDelimiter(ParseString(value, loption));
	} else if (loption == "quote") {
		SetQuote(ParseString(value, loption));
	} else if (loption == "comment") {
		SetComment(ParseString(value, loption));
	} else if (loption == "new_line") {
		SetNewline(ParseString(value, loption));
	} else if (loption == "escape") {
		SetEscape(ParseString(value, loption));
	} else if (loption == "header") {
		SetHeader(ParseBoolean(value, loption));
	} else if (loption == "nullstr" || loption == "null") {
		auto &child_type = value.type();
		null_str.clear();
		if (child_type.id() != LogicalTypeId::LIST && child_type.id() != LogicalTypeId::VARCHAR) {
			throw BinderException("CSV Reader function option %s requires a string or a list as input", loption);
		}
		if (!null_str.empty()) {
			throw BinderException("CSV Reader function option nullstr can only be supplied once");
		}
		if (child_type.id() == LogicalTypeId::LIST) {
			auto &list_child = ListType::GetChildType(child_type);
			const vector<Value> *children = nullptr;
			if (list_child.id() == LogicalTypeId::LIST) {
				// This can happen if it comes from a copy FROM/TO
				auto &list_grandchild = ListType::GetChildType(list_child);
				auto &children_ref = ListValue::GetChildren(value);
				if (list_grandchild.id() != LogicalTypeId::VARCHAR || children_ref.size() != 1) {
					throw BinderException("CSV Reader function option %s requires a non-empty list of possible null "
					                      "strings (varchar) as input",
					                      loption);
				}
				children = &ListValue::GetChildren(children_ref.back());
			} else if (list_child.id() != LogicalTypeId::VARCHAR) {
				throw BinderException("CSV Reader function option %s requires a non-empty list of possible null "
				                      "strings (varchar) as input",
				                      loption);
			}
			if (!children) {
				children = &ListValue::GetChildren(value);
			}
			if (children->empty()) {
				throw BinderException("CSV Reader function option %s requires a non-empty list of possible null "
				                      "strings (varchar) as input",
				                      loption);
			}
			for (auto &child : *children) {
				if (child.IsNull()) {
					throw BinderException(
					    "CSV Reader function option %s does not accept NULL values as a valid nullstr option", loption);
				}
				null_str.push_back(StringValue::Get(child));
			}
		} else {
			null_str.push_back(StringValue::Get(ParseString(value, loption)));
		}
		if (null_str.size() > 1 && write_option) {
			throw BinderException("CSV Writer function option %s only accepts one nullstr value.", loption);
		}

	} else if (loption == "compression") {
		SetCompression(ParseString(value, loption));
	} else if (loption == "strict_mode") {
		SetRFC4180(ParseBoolean(value, loption));
	} else {
		// unrecognized option in base CSV
		return false;
	}
	return true;
}

template <class T>
string FormatOptionLine(const string &name, const CSVOption<T> &option) {
	return name + " = " + option.FormatValue() + " " + option.FormatSet() + "\n  ";
}

bool CSVReaderOptions::WasTypeManuallySet(idx_t i) const {
	if (i >= was_type_manually_set.size()) {
		return false;
	}
	return was_type_manually_set[i];
}

string CSVReaderOptions::ToString(const String &current_file_path) const {
	auto &delimiter = dialect_options.state_machine_options.delimiter;
	auto &quote = dialect_options.state_machine_options.quote;
	auto &escape = dialect_options.state_machine_options.escape;
	auto &comment = dialect_options.state_machine_options.comment;
	auto &new_line = dialect_options.state_machine_options.new_line;
	auto &strict_mode = dialect_options.state_machine_options.strict_mode;
	auto &skip_rows = dialect_options.skip_rows;

	auto &header = dialect_options.header;
	string error = "  file = " + current_file_path.ToStdString() + "\n  ";
	// Let's first print options that can either be set by the user or by the sniffer
	// delimiter
	error += FormatOptionLine("delimiter", delimiter);
	// quote
	error += FormatOptionLine("quote", quote);
	// escape
	error += FormatOptionLine("escape", escape);
	// newline
	error += FormatOptionLine("new_line", new_line);
	// has_header
	error += FormatOptionLine("header", header);
	// skip_rows
	error += FormatOptionLine("skip_rows", skip_rows);
	// comment
	error += FormatOptionLine("comment", comment);
	// strict_mode
	error += FormatOptionLine("strict_mode", strict_mode);
	// date format
	error += FormatOptionLine("date_format", dialect_options.date_format.at(LogicalType::DATE));
	// timestamp format
	error += FormatOptionLine("timestamp_format", dialect_options.date_format.at(LogicalType::TIMESTAMP));

	// Now we do options that can only be set by the user, that might hold some general significance
	// null padding
	error += "null_padding = " + std::to_string(null_padding) + "\n  ";
	// sample_size
	error += "sample_size = " + std::to_string(sample_size_chunks * STANDARD_VECTOR_SIZE) + "\n  ";
	// ignore_errors
	error += "ignore_errors = " + ignore_errors.FormatValue() + "\n  ";
	// all_varchar
	error += "all_varchar = " + std::to_string(all_varchar) + "\n";

	// Add information regarding sniffer mismatches (if any)
	error += sniffer_user_mismatch_error;
	return error;
}

static Value StringVectorToValue(const vector<Identifier> &vec) {
	vector<Value> content;
	content.reserve(vec.size());
	for (auto &item : vec) {
		content.push_back(Value(item));
	}
	return Value::LIST(LogicalType::VARCHAR, std::move(content));
}

static uint8_t GetCandidateSpecificity(const LogicalType &candidate_type) {
	//! Const ht with accepted auto_types and their weights in specificity
	const duckdb::unordered_map<uint8_t, uint8_t> auto_type_candidates_specificity {
	    {static_cast<uint8_t>(LogicalTypeId::VARCHAR), 0},       {static_cast<uint8_t>(LogicalTypeId::DOUBLE), 1},
	    {static_cast<uint8_t>(LogicalTypeId::FLOAT), 2},         {static_cast<uint8_t>(LogicalTypeId::DECIMAL), 3},
	    {static_cast<uint8_t>(LogicalTypeId::BIGNUM), 4},        {static_cast<uint8_t>(LogicalTypeId::HUGEINT), 5},
	    {static_cast<uint8_t>(LogicalTypeId::BIGINT), 6},        {static_cast<uint8_t>(LogicalTypeId::INTEGER), 7},
	    {static_cast<uint8_t>(LogicalTypeId::SMALLINT), 8},      {static_cast<uint8_t>(LogicalTypeId::TINYINT), 9},
	    {static_cast<uint8_t>(LogicalTypeId::TIMESTAMP_TZ), 10}, {static_cast<uint8_t>(LogicalTypeId::TIMESTAMP), 11},
	    {static_cast<uint8_t>(LogicalTypeId::DATE), 12},         {static_cast<uint8_t>(LogicalTypeId::TIME), 13},
	    {static_cast<uint8_t>(LogicalTypeId::BOOLEAN), 14},      {static_cast<uint8_t>(LogicalTypeId::SQLNULL), 15}};

	auto id = static_cast<uint8_t>(candidate_type.id());
	auto it = auto_type_candidates_specificity.find(id);
	if (it == auto_type_candidates_specificity.end()) {
		throw BinderException("Auto Type Candidate of type %s is not accepted as a valid input",
		                      EnumUtil::ToString(candidate_type.id()));
	}
	return it->second;
}
bool StoreUserDefinedParameter(const Identifier &option) {
	if (option == "column_types" || option == "types" || option == "dtypes" || option == "auto_detect" ||
	    option == "auto_type_candidates" || option == "columns" || option == "names") {
		// We don't store options related to types, names and auto-detection since these are either irrelevant to our
		// prompt or are covered by the columns option.
		return false;
	}
	return true;
}

void CSVReaderOptions::Verify(MultiFileOptions &file_options) {
	if (rejects_table_name.IsSetByUser() && !store_rejects.GetValue() && store_rejects.IsSetByUser()) {
		throw BinderException("REJECTS_TABLE option is only supported when store_rejects is not manually set to false");
	}
	if (rejects_scan_name.IsSetByUser() && !store_rejects.GetValue() && store_rejects.IsSetByUser()) {
		throw BinderException("REJECTS_SCAN option is only supported when store_rejects is not manually set to false");
	}
	if (rejects_scan_name.IsSetByUser() || rejects_table_name.IsSetByUser()) {
		// Ensure we set store_rejects to true automagically
		store_rejects.Set(true, false);
	}
	// Validate rejects_table options
	if (store_rejects.GetValue()) {
		if (!ignore_errors.GetValue() && ignore_errors.IsSetByUser()) {
			throw BinderException(
			    "STORE_REJECTS option is only supported when IGNORE_ERRORS is not manually set to false");
		}
		// Ensure we set ignore errors to true automagically
		ignore_errors.Set(true, false);
		if (file_options.union_by_name) {
			throw BinderException("REJECTS_TABLE option is not supported when UNION_BY_NAME is set to true");
		}
	}
	if (rejects_limit != 0 && !store_rejects.GetValue()) {
		throw BinderException("REJECTS_LIMIT option is only supported when REJECTS_TABLE is set to a table name");
	}
	// Validate CSV Buffer and max_line_size do not conflict.
	if (buffer_size_option.IsSetByUser() && maximum_line_size.IsSetByUser()) {
		if (buffer_size_option.GetValue() < maximum_line_size.GetValue()) {
			throw BinderException("BUFFER_SIZE option was set to %d, while MAX_LINE_SIZE was set to %d. BUFFER_SIZE "
			                      "must have always be set to value bigger than MAX_LINE_SIZE",
			                      buffer_size_option.GetValue(), maximum_line_size.GetValue());
		}
	} else if (maximum_line_size.IsSetByUser() && maximum_line_size.GetValue() > max_line_size_default) {
		// If the max line size is set by the user and bigger than we have by default, we make it part of our buffer
		// size decision.
		idx_t buffer_size;
		if (!TryMultiplyOperator::Operation<idx_t, idx_t, idx_t>(CSVBuffer::ROWS_PER_BUFFER,
		                                                         maximum_line_size.GetValue(), buffer_size)) {
			throw BinderException("MAX_LINE_SIZE option was set to %d, which is too large",
			                      maximum_line_size.GetValue());
		}
		buffer_size_option.Set(buffer_size, false);
	}
}

bool GetBooleanValue(const Identifier &loption, const Value &val) {
	if (val.IsNull()) {
		throw BinderException("read_csv %s cannot be NULL", loption);
	}
	return BooleanValue::Get(val);
}

string CSVReaderOptions::GetUserDefinedParameters() const {
	string result;
	for (auto &udf_parameter : user_defined_parameters) {
		if (!result.empty()) {
			result += ", ";
		}
		result += udf_parameter.first + "=" + udf_parameter.second;
	}
	return result;
}

void CSVReaderOptions::FromNamedParameters(const named_argument_map_t &in, ClientContext &context,
                                           MultiFileOptions &file_options) {
	for (auto &kv : in) {
		if (MultiFileReader().ParseOption(kv.first, kv.second, file_options, context)) {
			continue;
		}
		ParseOption(context, kv.first, kv.second);
	}
}

void CSVReaderOptions::ParseOption(ClientContext &context, const Identifier &key, const Value &val) {
	// skip variables that are specific to auto-detection
	if (StoreUserDefinedParameter(key)) {
		// the parameter list is rendered back into a SQL prompt by GetUserDefinedParameters, so it is stored
		// normalized to lowercase rather than echoing back whatever casing the user wrote
		user_defined_parameters[Identifier(StringUtil::Lower(key.GetIdentifierName()))] = val.ToSQLString();
	}
	if (key == "columns") {
		if (!name_list.empty()) {
			throw BinderException("read_csv column_names/names can only be supplied once");
		}
		columns_set = true;
		auto &child_type = val.type();
		if (child_type.id() != LogicalTypeId::STRUCT) {
			throw BinderException("read_csv columns requires a struct as input");
		}
		if (val.IsNull()) {
			throw BinderException("read_csv columns cannot be NULL");
		}
		auto &struct_children = StructValue::GetChildren(val);
		D_ASSERT(StructType::GetChildCount(child_type) == struct_children.size());

		// Parse into temporary lists first
		vector<Identifier> parsed_names;
		vector<LogicalType> parsed_types;
		identifier_map_t<idx_t> parsed_types_per_column;
		for (idx_t i = 0; i < struct_children.size(); i++) {
			auto &name = StructType::GetChildName(child_type, i);
			auto &val = struct_children[i];
			parsed_names.emplace_back(name);
			if (val.type().id() != LogicalTypeId::VARCHAR) {
				throw BinderException("read_csv requires a type specification as string");
			}
			if (val.IsNull()) {
				throw BinderException("read_csv columns cannot have a NULL type");
			}
			parsed_types_per_column[name] = i;
			parsed_types.emplace_back(TransformStringToLogicalType(StringValue::Get(val), context));
		}
		if (parsed_names.empty()) {
			throw BinderException("read_csv requires at least a single column as input!");
		}
		// If types were already set by 'types'/'dtypes'/'column_types', verify they match
		if (!sql_type_list.empty()) {
			VerifyTypeListsMatch(parsed_types, sql_type_list);
		}
		name_list = std::move(parsed_names);
		sql_type_list = std::move(parsed_types);
		sql_types_per_column = std::move(parsed_types_per_column);
	} else if (key == "auto_type_candidates") {
		auto_type_candidates.clear();
		map<uint8_t, LogicalType> candidate_types;
		// We always have the extremes of Null and Varchar, so we can default to varchar if the
		// sniffer is not able to confidently detect that column type
		candidate_types[GetCandidateSpecificity(LogicalType::VARCHAR)] = LogicalType::VARCHAR;
		candidate_types[GetCandidateSpecificity(LogicalType::SQLNULL)] = LogicalType::SQLNULL;

		auto &child_type = val.type();
		if (child_type.id() != LogicalTypeId::LIST) {
			throw BinderException("read_csv auto_types requires a list as input");
		}
		if (val.IsNull()) {
			throw BinderException("auto_type_candidates cannot be NULL");
		}
		auto &list_children = ListValue::GetChildren(val);
		if (list_children.empty()) {
			throw BinderException("auto_type_candidates requires at least one type");
		}
		for (auto &child : list_children) {
			if (child.type().id() != LogicalTypeId::VARCHAR) {
				throw BinderException("auto_type_candidates requires a type specification as string");
			}
			if (child.IsNull()) {
				throw BinderException("auto_type_candidates cannot contain NULL types");
			}
			auto candidate_type = TransformStringToLogicalType(StringValue::Get(child), context);
			candidate_types[GetCandidateSpecificity(candidate_type)] = candidate_type;
		}
		for (auto &candidate_type : candidate_types) {
			auto_type_candidates.emplace_back(candidate_type.second);
		}
	} else if (key == "column_names" || key == "names") {
		identifier_set_t column_names;
		if (!name_list.empty()) {
			throw BinderException("read_csv column_names/names can only be supplied once");
		}
		if (val.IsNull()) {
			throw BinderException("read_csv %s cannot be NULL", key);
		}
		auto &children = ListValue::GetChildren(val);
		for (auto &child : children) {
			if (child.IsNull()) {
				throw BinderException("read_csv %s parameter cannot have a NULL value", key);
			}
			name_list.emplace_back(StringValue::Get(child));
		}
		for (auto &name : name_list) {
			bool empty = true;
			for (auto &c : name.GetIdentifierName()) {
				if (!StringUtil::CharacterIsSpace(c)) {
					empty = false;
					break;
				}
			}
			if (empty) {
				throw BinderException("read_csv %s cannot have empty (or all whitespace) value", key);
			}
			if (column_names.find(name) != column_names.end()) {
				throw BinderException("read_csv %s must have unique values. %s is repeated.", key, name);
			}
			column_names.insert(name);
		}
	} else if (key == "column_types" || key == "types" || key == "dtypes") {
		auto &child_type = val.type();
		if (child_type.id() != LogicalTypeId::STRUCT && child_type.id() != LogicalTypeId::LIST) {
			throw BinderException("read_csv %s requires a struct or list as input", key);
		}
		if (!columns_set && !sql_type_list.empty()) {
			throw BinderException("read_csv column_types/types/dtypes can only be supplied once");
		}
		if (val.IsNull()) {
			throw BinderException("read_csv %s cannot be NULL", key);
		}

		// Parse into temporary lists first
		vector<string> sql_type_names;
		identifier_map_t<idx_t> parsed_types_per_column;
		if (child_type.id() == LogicalTypeId::STRUCT) {
			auto &struct_children = StructValue::GetChildren(val);
			D_ASSERT(StructType::GetChildCount(child_type) == struct_children.size());
			for (idx_t i = 0; i < struct_children.size(); i++) {
				auto &name = StructType::GetChildName(child_type, i);
				auto &val = struct_children[i];
				if (val.type().id() != LogicalTypeId::VARCHAR) {
					throw BinderException("read_csv %s requires a type specification as string", key);
				}
				if (val.IsNull()) {
					throw BinderException("read_csv %s cannot have a NULL type", key);
				}
				sql_type_names.push_back(StringValue::Get(val));
				parsed_types_per_column[name] = i;
			}
		} else {
			auto &list_child = ListType::GetChildType(child_type);
			if (list_child.id() != LogicalTypeId::VARCHAR) {
				throw BinderException("read_csv %s requires a list of types (varchar) as input", key);
			}
			auto &children = ListValue::GetChildren(val);
			for (auto &child : children) {
				if (child.IsNull()) {
					throw BinderException("read_csv %s cannot have a NULL type", key);
				}
				sql_type_names.push_back(StringValue::Get(child));
			}
		}
		vector<LogicalType> parsed_types;
		parsed_types.reserve(sql_type_names.size());
		for (auto &sql_type : sql_type_names) {
			auto def_type = TransformStringToLogicalType(sql_type, context);
			if (def_type.id() == LogicalTypeId::UNBOUND) {
				throw BinderException("Unrecognized type \"%s\" for read_csv %s definition", sql_type, key);
			}
			parsed_types.push_back(std::move(def_type));
		}

		// If columns already set sql_type_list, verify they match but keep columns' mappings
		if (columns_set && !sql_type_list.empty()) {
			VerifyTypeListsMatch(sql_type_list, parsed_types);
		} else {
			sql_type_list = std::move(parsed_types);
			if (!parsed_types_per_column.empty()) {
				sql_types_per_column = std::move(parsed_types_per_column);
			}
		}
	} else if (key == "all_varchar") {
		all_varchar = GetBooleanValue(key, val);
	} else if (key == "files_to_sniff") {
		files_to_sniff = ParseInteger(val, key);
		if (files_to_sniff < 1 && files_to_sniff != -1) {
			throw BinderException(
			    "Unsupported parameter for files_to_sniff: value must be -1 for all files or higher than one.");
		}
	} else if (key == "normalize_names") {
		normalize_names = GetBooleanValue(key, val);
	} else {
		SetReadOption(key, val, name_list);
	}
}
//! This function is used to remember options set by the sniffer, for use in ReadCSVRelation
void CSVReaderOptions::ToNamedParameters(named_parameter_map_t &named_params) const {
	auto &delimiter = dialect_options.state_machine_options.delimiter;
	auto &quote = dialect_options.state_machine_options.quote;
	auto &escape = dialect_options.state_machine_options.escape;
	auto &comment = dialect_options.state_machine_options.comment;
	auto &strict_mode = dialect_options.state_machine_options.strict_mode;
	auto &header = dialect_options.header;
	if (delimiter.IsSetByUser()) {
		named_params["delim"] = Value(GetDelimiter());
	}
	if (dialect_options.state_machine_options.new_line.IsSetByUser()) {
		named_params["new_line"] = Value(GetNewline());
	}
	if (quote.IsSetByUser()) {
		named_params["quote"] = Value(GetQuote());
	}
	if (escape.IsSetByUser()) {
		named_params["escape"] = Value(GetEscape());
	}
	if (comment.IsSetByUser()) {
		named_params["comment"] = Value(GetComment());
	}
	if (header.IsSetByUser()) {
		named_params["header"] = Value(GetHeader());
	}
	if (strict_mode.IsSetByUser()) {
		named_params["strict_mode"] = Value(GetRFC4180());
	}
	named_params["max_line_size"] = Value::BIGINT(NumericCast<int64_t>(maximum_line_size.GetValue()));
	if (dialect_options.skip_rows.IsSetByUser()) {
		named_params["skip"] = Value::BIGINT(NumericCast<int64_t>(GetSkipRows()));
	}
	named_params["null_padding"] = Value::BOOLEAN(null_padding);
	named_params["parallel"] = Value::BOOLEAN(parallel);
	if (!dialect_options.date_format.at(LogicalType::DATE).GetValue().format_specifier.empty()) {
		named_params["dateformat"] =
		    Value(dialect_options.date_format.at(LogicalType::DATE).GetValue().format_specifier);
	}
	if (!dialect_options.date_format.at(LogicalType::TIMESTAMP).GetValue().format_specifier.empty()) {
		named_params["timestampformat"] =
		    Value(dialect_options.date_format.at(LogicalType::TIMESTAMP).GetValue().format_specifier);
	}

	named_params["normalize_names"] = Value::BOOLEAN(normalize_names);
	if (!name_list.empty() && !named_params.count("columns") && !named_params.count("column_names") &&
	    !named_params.count("names")) {
		named_params["column_names"] = StringVectorToValue(name_list);
	}
	named_params["all_varchar"] = Value::BOOLEAN(all_varchar);
}

} // namespace duckdb
