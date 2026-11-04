//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/csv_scanner/csv_reader_options.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/operator/csv_scanner/csv_buffer.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_option.hpp"
#include "duckdb/execution/operator/csv_scanner/state_machine_options.hpp"
#include "duckdb/common/map.hpp"
#include "duckdb/function/scalar/strftime_format.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/multi_file/multi_file_options.hpp"
#include "duckdb/execution/operator/csv_scanner/set_columns.hpp"

namespace duckdb {

struct DialectOptions {
	CSVStateMachineOptions state_machine_options;
	//! Expected number of columns
	idx_t num_cols = 0;
	//! Whether the file has a header line
	CSVOption<bool> header = false;
	//! The date format to use (if any is specified)
	map<LogicalTypeId, CSVOption<StrpTimeFormat>> date_format = {{LogicalTypeId::DATE, {}},
	                                                             {LogicalTypeId::TIMESTAMP, {}}};
	//! How many leading rows to skip
	CSVOption<idx_t> skip_rows = 0;
	idx_t rows_until_header = 0;
};

struct CSVReaderOptions {
	CSVReaderOptions() {
	}
	CSVReaderOptions(CSVOption<char> single_byte_delimiter, const CSVOption<string> &multi_byte_delimiter);
	//===--------------------------------------------------------------------===//
	// CommonCSVOptions
	//===--------------------------------------------------------------------===//
	//! See struct above.
	DialectOptions dialect_options;
	//! Whether we should ignore InvalidInput errors
	CSVOption<bool> ignore_errors = false;
	//! Whether we store CSV Errors in the rejects table or not
	CSVOption<bool> store_rejects = false;
	//! Rejects table name (Name of the table the store rejects errors)
	CSVOption<string> rejects_table_name = {"reject_errors"};
	//! Rejects Scan name  (Name of the table the store rejects scans)
	CSVOption<string> rejects_scan_name = {"reject_scans"};
	//! Rejects table entry limit (0 = no limit)
	idx_t rejects_limit = 0;
	//! Rejects line size limit (0 = no limit)
	idx_t rejects_line_size_limit = 10000;
	//! Number of samples to buffer
	idx_t buffer_sample_size = static_cast<idx_t>(STANDARD_VECTOR_SIZE * 50);
	//! Specifies the strings that represents a null value
	vector<string> null_str = {""};
	//! Whether file is compressed or not, and if so which compression type
	//! AUTO_DETECT (default; infer from file extension)
	FileCompressionType compression = FileCompressionType::AUTO_DETECT;
	//! Option to convert quoted values to NULL values
	bool allow_quoted_nulls = true;
	char comment = '\0';

	//! Thousands separator option (to be able to accept "100,000.220" as a double or decimal.
	char thousands_separator = '\0';

	//===--------------------------------------------------------------------===//
	// CSVAutoOptions
	//===--------------------------------------------------------------------===//
	//! SQL Type list mapping of name to SQL type index in sql_type_list
	identifier_map_t<idx_t> sql_types_per_column;
	//! User-defined SQL type list
	vector<LogicalType> sql_type_list;
	//! User-defined name list
	vector<Identifier> name_list;
	//! If the names and types were set by the columns parameter
	bool columns_set = false;
	//! Types considered as candidates for auto-detection ordered by ascending specificity (~ from low to high)
	vector<LogicalType> auto_type_candidates = {LogicalType::VARCHAR,   LogicalType::DOUBLE, LogicalType::BIGNUM,
	                                            LogicalType::HUGEINT,   LogicalType::BIGINT, LogicalType::TIMESTAMP_TZ,
	                                            LogicalType::TIMESTAMP, LogicalType::DATE,   LogicalType::TIME,
	                                            LogicalType::BOOLEAN,   LogicalType::SQLNULL};
	//! In case the sniffer found a mismatch error from user defined types or dialect
	string sniffer_user_mismatch_error;
	//! In case the sniffer found a mismatch error from user defined types or dialect
	vector<bool> was_type_manually_set;
	//===--------------------------------------------------------------------===//
	// ReadCSVOptions
	//===--------------------------------------------------------------------===//
	//! Maximum CSV line size: specified because if we reach this amount, we likely have wrong delimiters (default: 2MB)
	//! note that this is the guaranteed line length that will succeed, longer lines may be accepted if slightly above
	static constexpr idx_t max_line_size_default = 2000000;
	CSVOption<idx_t> maximum_line_size = max_line_size_default;
	//! Whether header names shall be normalized
	bool normalize_names = false;
	//! True, if column with that index must skip null check
	unordered_set<string> force_not_null_names;
	//! True, if column with that index must skip null check
	vector<bool> force_not_null;
	//! Result size of sniffing phases
	static constexpr idx_t sniff_size = 2048;

	//! In case this is a glob or list of multiple files, how many shall be used to sniff.
	//! -1 means all
	int64_t files_to_sniff = 10;

	//! Number of sample chunks used in auto-detection
	idx_t sample_size_chunks = 20480 / sniff_size;
	//! Consider all columns to be of type varchar
	bool all_varchar = false;
	//! Whether to automatically detect dialect and datatypes
	bool auto_detect = true;
	//! The file path of the CSV file to read
	string file_path;
	//! Buffer Size (Parallel Scan)
	CSVOption<idx_t> buffer_size_option = CSVBuffer::ROWS_PER_BUFFER * max_line_size_default;
	//! Decimal separator when reading as numeric
	string decimal_separator = ".";
	//! Whether  to pad rows that do not have enough columns with NULL values
	bool null_padding = false;
	//! If we should attempt to run parallel scanning over one file
	bool parallel = true;

	//! By default, our encoding is always UTF-8
	string encoding = "utf-8";
	//! User defined parameters
	identifier_tree_t<string> user_defined_parameters;

	//! Returns a list of user-defined parameters in string format
	string GetUserDefinedParameters() const;

	//===--------------------------------------------------------------------===//
	// WriteCSVOptions
	//===--------------------------------------------------------------------===//
	//! True, if column with that index must be quoted
	vector<bool> force_quote;
	//! Prefix/suffix/custom newline the entire file once (enables writing of files as JSON arrays)
	string prefix;
	string suffix;
	string write_newline;

	//! The date format to use for writing (if any is specified)
	map<LogicalTypeId, Value> write_date_format = {{LogicalTypeId::DATE, Value()}, {LogicalTypeId::TIMESTAMP, Value()}};
	//! Whether  a type format is specified
	map<LogicalTypeId, bool> has_format = {{LogicalTypeId::DATE, false}, {LogicalTypeId::TIMESTAMP, false}};
	//! If this reader is a multifile reader
	bool multi_file_reader = false;

	void SetCompression(const string &compression);

	bool GetHeader() const;
	void SetHeader(bool has_header);

	string GetEscape() const;
	void SetEscape(const string &escape);

	idx_t GetSkipRows() const;
	void SetSkipRows(int64_t rows);

	void SetQuote(const string &quote);
	string GetQuote() const;
	void SetComment(const string &comment);
	string GetComment() const;
	void SetDelimiter(const string &delimiter);
	string GetDelimiter() const;

	//! If we can safely ignore errors (i.e., they are being ignored and not being stored in a rejects table)
	bool IgnoreErrors() const;

	string GetNewline() const;
	void SetNewline(const string &input);

	bool GetRFC4180() const;
	void SetRFC4180(bool rfc4180);

	CSVOption<char> GetSingleByteDelimiter() const;
	CSVOption<string> GetMultiByteDelimiter() const;

	//! Set an option that is supported by both reading and writing functions, called by
	//! the SetReadOption and SetWriteOption methods
	bool SetBaseOption(const Identifier &loption, const Value &value, bool write_option = false);

	//! loption - lowercase string
	//! set - argument(s) to the option
	//! expected_names - names expected if the option is "columns"
	void SetReadOption(const Identifier &loption, const Value &value, const vector<Identifier> &expected_names);
	void SetWriteOption(const Identifier &loption, const Value &value);
	void SetDateFormat(LogicalTypeId type, const string &format, bool read_format);
	void ToNamedParameters(named_parameter_map_t &out) const;
	void FromNamedParameters(const named_argument_map_t &in, ClientContext &context, MultiFileOptions &file_options);
	void ParseOption(ClientContext &context, const Identifier &key, const Value &val);
	//! Verify options are not conflicting
	void Verify(MultiFileOptions &file_options);

	string ToString(const String &current_file_path) const;
	//! If the type for column with idx i was manually set
	bool WasTypeManuallySet(idx_t i) const;

	string NewLineIdentifierToString() const {
		switch (dialect_options.state_machine_options.new_line.GetValue()) {
		case NewLineIdentifier::SINGLE_N:
			return "\\n";
		case NewLineIdentifier::SINGLE_R:
			return "\\r";
		case NewLineIdentifier::CARRY_ON:
			return "\\r\\n";
		default:
			return "";
		}
	}
};
} // namespace duckdb
