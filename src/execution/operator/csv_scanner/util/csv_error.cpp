#include "duckdb/execution/operator/csv_scanner/csv_error.hpp"
#include "duckdb/common/sql_identifier.hpp"

#include "utf8proc_wrapper.hpp"
#include "duckdb/common/exception/conversion_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table/read_csv.hpp"
#include "duckdb/execution/operator/persistent/csv_rejects_table.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_file_scanner.hpp"
#include "duckdb/main/appender.hpp"
#include "duckdb/common/multi_file/multi_file_function.hpp"
#include <sstream>

namespace duckdb {

LinesPerBoundary::LinesPerBoundary() {
}
LinesPerBoundary::LinesPerBoundary(idx_t boundary_idx_p, idx_t lines_in_batch_p)
    : boundary_idx(boundary_idx_p), lines_in_batch(lines_in_batch_p) {
}

CSVErrorHandler::CSVErrorHandler(bool ignore_errors_p, bool store_ignored_errors_p)
    : ignore_errors(ignore_errors_p), store_ignored_errors(store_ignored_errors_p) {
}

void CSVErrorHandler::ThrowError(const CSVError &csv_error) {
	auto error_to_throw = csv_error;
	idx_t error_to_throw_row = GetLineInternal(error_to_throw.error_info);
	if (PrintLineNumber(error_to_throw) && !errors.empty()) {
		// We stored a previous error here, we pick the one that happens the earliest to throw
		for (const auto &error : errors) {
			if (CanGetLine(error.GetBoundaryIndex())) {
				idx_t cur_error_to_throw = GetLineInternal(error.error_info);
				if (cur_error_to_throw < error_to_throw_row) {
					error_to_throw = error;
					error_to_throw_row = cur_error_to_throw;
				}
			}
		}
	}
	std::ostringstream error;
	if (PrintLineNumber(error_to_throw)) {
		error << "CSV Error on Line: " << error_to_throw_row << '\n';
		if (!error_to_throw.csv_row.empty()) {
			error << "Original Line: " << error_to_throw.csv_row << '\n';
		}
	}
	if (error_to_throw.full_error_message.empty()) {
		error << error_to_throw.error_message;
	} else {
		error << error_to_throw.full_error_message;
	}
	switch (csv_error.type) {
	case CAST_ERROR:
		throw ConversionException(error.str());
	case COLUMN_NAME_TYPE_MISMATCH:
		throw BinderException(error.str());
	case NULLPADDED_QUOTED_NEW_VALUE:
		throw ParameterNotAllowedException(error.str());
	default:
		throw InvalidInputException(error.str());
	}
}

void CSVErrorHandler::Error(const CSVError &csv_error, bool force_error) {
	lock_guard<mutex> parallel_lock(main_mutex);
	if (!force_error && ignore_errors) {
		// We are ignoring this error - it is only kept if something (e.g. the rejects table) will read it
		if (store_ignored_errors) {
			errors.push_back(csv_error);
		}
		return;
	}
	if (!force_error && PrintLineNumber(csv_error) && !CanGetLine(csv_error.GetBoundaryIndex())) {
		// We store this error, we can't throw it now
		errors.push_back(csv_error);
		return;
	}
	// Otherwise we can throw directly
	ThrowError(csv_error);
}

void CSVErrorHandler::ErrorIfNeeded() {
	lock_guard<mutex> parallel_lock(main_mutex);
	if (ignore_errors || errors.empty()) {
		// Nothing to error
		return;
	}

	if (CanGetLine(errors[0].error_info.boundary_idx)) {
		ThrowError(errors[0]);
	}
}

void CSVErrorHandler::ErrorIfAny() {
	lock_guard<mutex> parallel_lock(main_mutex);
	if (ignore_errors || errors.empty()) {
		// Nothing to error
		return;
	}
	if (!CanGetLine(errors[0].error_info.boundary_idx)) {
		throw InternalException("Failed to get error information for boundary index");
	}
	ThrowError(errors[0]);
}

void CSVErrorHandler::ErrorIfTypeExists(CSVErrorType error_type) {
	lock_guard<mutex> parallel_lock(main_mutex);
	for (auto &error : errors) {
		if (error.type == error_type) {
			// If it's a maximum line size error, we can do it now.
			ThrowError(error);
		}
	}
}

void CSVErrorHandler::Insert(idx_t boundary_idx, idx_t rows) {
	lock_guard<mutex> parallel_lock(main_mutex);
	if (lines_per_batch_map.find(boundary_idx) == lines_per_batch_map.end()) {
		lines_per_batch_map[boundary_idx] = {boundary_idx, rows};
	} else {
		lines_per_batch_map[boundary_idx].lines_in_batch += rows;
	}
}

void CSVErrorHandler::NewMaxLineSize(idx_t scan_line_size) {
	lock_guard<mutex> parallel_lock(main_mutex);
	max_line_length = std::max(scan_line_size, max_line_length);
}

bool CSVErrorHandler::AnyErrors() {
	lock_guard<mutex> parallel_lock(main_mutex);
	return !errors.empty();
}

bool CSVErrorHandler::HasError(const CSVErrorType error_type) {
	lock_guard<mutex> parallel_lock(main_mutex);
	for (const auto &er : errors) {
		if (er.type == error_type) {
			return true;
		}
	}
	return false;
}

CSVError CSVErrorHandler::GetFirstError(CSVErrorType error_type) {
	lock_guard<mutex> parallel_lock(main_mutex);
	for (const auto &er : errors) {
		if (er.type == error_type) {
			return er;
		}
	}
	throw InternalException("CSVErrorHandler::GetFirstError was called without having an appropriate error type");
}

idx_t CSVErrorHandler::GetSize() {
	lock_guard<mutex> parallel_lock(main_mutex);
	return errors.size();
}

bool IsCSVErrorAcceptedReject(CSVErrorType type) {
	switch (type) {
	case CSVErrorType::INVALID_STATE:
	case CSVErrorType::CAST_ERROR:
	case CSVErrorType::TOO_MANY_COLUMNS:
	case CSVErrorType::TOO_FEW_COLUMNS:
	case CSVErrorType::MAXIMUM_LINE_SIZE:
	case CSVErrorType::UNTERMINATED_QUOTES:
	case CSVErrorType::INVALID_ENCODING:
		return true;
	default:
		return false;
	}
}
string CSVErrorTypeToEnum(CSVErrorType type) {
	switch (type) {
	case CSVErrorType::CAST_ERROR:
		return "CAST";
	case CSVErrorType::TOO_FEW_COLUMNS:
		return "MISSING COLUMNS";
	case CSVErrorType::TOO_MANY_COLUMNS:
		return "TOO MANY COLUMNS";
	case CSVErrorType::MAXIMUM_LINE_SIZE:
		return "LINE SIZE OVER MAXIMUM";
	case CSVErrorType::UNTERMINATED_QUOTES:
		return "UNQUOTED VALUE";
	case CSVErrorType::INVALID_ENCODING:
		return "INVALID ENCODING";
	case CSVErrorType::INVALID_STATE:
		return "INVALID STATE";
	default:
		throw InternalException("CSV Error is not valid to be stored in a Rejects Table");
	}
}

void CSVErrorHandler::FillRejectsTable(InternalAppender &errors_appender, const idx_t file_idx, const idx_t scan_idx,
                                       const CSVFileScan &file, CSVRejectsTable &rejects,
                                       const vector<Identifier> &column_names, const idx_t limit) {
	lock_guard<mutex> parallel_lock(main_mutex);
	// We first insert the file into the file scans table
	for (auto &error : file.error_handler->errors) {
		if (!IsCSVErrorAcceptedReject(error.type)) {
			continue;
		}
		// short circuit if we already have too many rejects
		if (limit == 0 || rejects.count < limit) {
			if (limit != 0 && rejects.count >= limit) {
				break;
			}
			rejects.count++;
			const auto row_line = file.error_handler->GetLineInternal(error.error_info);
			const auto col_idx = error.column_idx;
			// Add the row to the rejects table
			errors_appender.BeginRow();
			// 1. Scan ID
			errors_appender.Append(scan_idx);
			// 2. File ID
			errors_appender.Append(file_idx);
			// 3. Row Line
			errors_appender.Append(row_line);
			// 4. Byte Position of the row error
			errors_appender.Append(error.row_byte_position + 1);
			// 5. Byte Position where error occurred
			if (!error.byte_position.IsValid()) {
				// This means this error comes from a flush, and we don't support this yet, so we give it
				// a null
				errors_appender.Append(Value());
			} else {
				errors_appender.Append(error.byte_position.GetIndex() + 1);
			}
			// 6. Column Index
			if (error.type == CSVErrorType::MAXIMUM_LINE_SIZE) {
				errors_appender.Append(Value());
			} else {
				errors_appender.Append(col_idx + 1);
			}
			// 7. Column Name (If Applicable)
			switch (error.type) {
			case CSVErrorType::TOO_MANY_COLUMNS:
			case CSVErrorType::MAXIMUM_LINE_SIZE:
				errors_appender.Append(Value());
				break;
			case CSVErrorType::TOO_FEW_COLUMNS:
				if (col_idx + 1 < column_names.size()) {
					errors_appender.Append(string_t(column_names[col_idx + 1].GetIdentifierName()));
				} else {
					errors_appender.Append(Value());
				}
				break;
			default:
				if (col_idx < column_names.size()) {
					errors_appender.Append(string_t(column_names[col_idx].GetIdentifierName()));
				} else {
					errors_appender.Append(Value());
				}
			}
			// 8. Error Type
			errors_appender.Append(string_t(CSVErrorTypeToEnum(error.type)));
			// 9. Original CSV Line
			errors_appender.Append(string_t(error.csv_row));
			// 10. Full Error Message
			errors_appender.Append(string_t(error.error_message));
			errors_appender.EndRow();
		}
	}
}

idx_t CSVErrorHandler::GetMaxLineLength() {
	lock_guard<mutex> parallel_lock(main_mutex);
	return max_line_length;
}

void CSVErrorHandler::DontPrintErrorLine() {
	lock_guard<mutex> parallel_lock(main_mutex);
	print_line = false;
}

void CSVErrorHandler::SetIgnoreErrors(bool ignore_errors_p) {
	lock_guard<mutex> parallel_lock(main_mutex);
	ignore_errors = ignore_errors_p;
}

CSVError::CSVError(string error_message_p, CSVErrorType type_p, LinesPerBoundary error_info_p)
    : error_message(std::move(error_message_p)), type(type_p), error_info(error_info_p) {
}

CSVError::CSVError(string error_message_p, CSVErrorType type_p, idx_t column_idx_p, string csv_row_p,
                   LinesPerBoundary error_info_p, idx_t row_byte_position, optional_idx byte_position_p,
                   const CSVReaderOptions &reader_options, const string &fixes, const String &current_path)
    : error_message(std::move(error_message_p)), type(type_p), column_idx(column_idx_p), csv_row(std::move(csv_row_p)),
      error_info(error_info_p), row_byte_position(row_byte_position), byte_position(byte_position_p) {
	// What were the options
	std::ostringstream error;
	if (reader_options.ignore_errors.GetValue()) {
		RemoveNewLine(error_message);
	}
	// Cap the csv row for performance reasons.
	if (reader_options.rejects_line_size_limit > 0 && csv_row.size() > reader_options.rejects_line_size_limit) {
		csv_row.erase(csv_row.begin() + NumericCast<int64_t>(reader_options.rejects_line_size_limit), csv_row.end());
		// truncating might add invalid UTF8 at the tail end - make it valid again
		Utf8Proc::MakeValid(csv_row.data(), csv_row.size(), '.');
	}
	error << error_message << '\n';
	error << fixes << '\n';
	error << reader_options.ToString(current_path);
	error << '\n';
	full_error_message = error.str();
}

CSVError CSVError::ColumnTypesError(identifier_map_t<idx_t> sql_types_per_column, const vector<Identifier> &names) {
	for (idx_t i = 0; i < names.size(); i++) {
		auto it = sql_types_per_column.find(names[i]);
		if (it != sql_types_per_column.end()) {
			sql_types_per_column.erase(names[i]);
		}
	}
	if (sql_types_per_column.empty()) {
		return CSVError("", COLUMN_NAME_TYPE_MISMATCH, {});
	}
	string exception = "COLUMN_TYPES error: Columns with names: ";
	for (auto &col : sql_types_per_column) {
		exception += "\"" + col.first + "\",";
	}
	exception.pop_back();
	exception += " do not exist in the CSV File";
	return CSVError(exception, COLUMN_NAME_TYPE_MISMATCH, {});
}

void CSVError::RemoveNewLine(string &error) {
	error = StringUtil::Split(error, "\n")[0];
}

CSVError CSVError::CastError(const CSVReaderOptions &options, const string &column_name, string &cast_error,
                             idx_t column_idx, string &csv_row, LinesPerBoundary error_info, idx_t row_byte_position,
                             optional_idx byte_position, LogicalTypeId type, const String &current_path) {
	std::ostringstream error;
	// Which column
	error << "Error when converting column \"" << column_name << "\". ";
	// What was the cast error
	error << cast_error << '\n';
	std::ostringstream how_to_fix_it;
	how_to_fix_it << "Column " << column_name << " is being converted as type " << LogicalTypeIdToString(type) << '\n';
	if (!options.WasTypeManuallySet(column_idx)) {
		how_to_fix_it << "This type was auto-detected from the CSV file." << '\n';
		how_to_fix_it << "Possible solutions:" << '\n';
		how_to_fix_it << "* Override the type for this column manually by setting the type explicitly, e.g., types={"
		              << SQLString::ToString(column_name) << ": 'VARCHAR'}" << '\n';
		how_to_fix_it
		    << "* Set the sample size to a larger value to enable the auto-detection to scan more values, e.g., "
		       "sample_size=-1"
		    << '\n';
		how_to_fix_it << "* Use a COPY statement to automatically derive types from an existing table." << '\n';
	} else {
		how_to_fix_it
		    << "This type was either manually set or derived from an existing table. Select a different type to "
		       "correctly parse this column."
		    << '\n';
	}
	how_to_fix_it << "* Check whether the null string value is set correctly (e.g., nullstr = 'N/A')" << '\n';

	return CSVError(error.str(), CAST_ERROR, column_idx, csv_row, error_info, row_byte_position, byte_position, options,
	                how_to_fix_it.str(), current_path);
}

CSVError CSVError::LineSizeError(const CSVReaderOptions &options, LinesPerBoundary error_info, string &csv_row,
                                 idx_t byte_position, const String &current_path) {
	std::ostringstream error;
	error << "Maximum line size of " << options.maximum_line_size.GetValue() << " bytes exceeded. ";
	error << "Actual Size:" << csv_row.size() << " bytes." << '\n';

	std::ostringstream how_to_fix_it;
	how_to_fix_it << "Possible Solution: Change the maximum length size, e.g., max_line_size=" << csv_row.size() + 2
	              << "\n";

	return CSVError(error.str(), MAXIMUM_LINE_SIZE, 0, csv_row, error_info, byte_position, byte_position, options,
	                how_to_fix_it.str(), current_path);
}

CSVError CSVError::InvalidState(const CSVReaderOptions &options, idx_t current_column, LinesPerBoundary error_info,
                                string &csv_row, idx_t row_byte_position, optional_idx byte_position,
                                const String &current_path) {
	std::ostringstream error;
	error << "The CSV Parser state machine reached an invalid state.\nThis can happen when is not possible to parse "
	         "your CSV File with the given options, or the CSV File is not RFC 4180 compliant ";
	std::ostringstream how_to_fix_it;
	if (options.dialect_options.state_machine_options.strict_mode.GetValue()) {
		how_to_fix_it << "Possible fixes:" << '\n';
		how_to_fix_it << "* Disable the parser's strict mode (strict_mode=false) to allow reading rows that do not "
		                 "comply with the CSV standard."
		              << '\n';
	}
	return CSVError(error.str(), INVALID_STATE, current_column, csv_row, error_info, row_byte_position, byte_position,
	                options, how_to_fix_it.str(), current_path);
}
CSVError CSVError::HeaderSniffingError(const CSVReaderOptions &options, const vector<HeaderValue> &best_header_row,
                                       const idx_t column_count, const string &delimiter) {
	std::ostringstream error;
	// 1. Which file
	error << "Error when sniffing file \"" << options.file_path << "\"." << '\n';
	// 2. What's the error
	error << "It was not possible to detect the CSV Header, due to the header having less columns than expected"
	      << '\n';
	// 2.1 What's the expected number of columns
	error << "Number of expected columns: " << column_count << ". Actual number of columns " << best_header_row.size()
	      << '\n';
	// 2.2 What was the detected row
	error << "Detected row as Header:" << '\n';
	for (idx_t i = 0; i < best_header_row.size(); i++) {
		if (best_header_row[i].is_null) {
			error << "NULL";
		} else {
			error << best_header_row[i].value;
		}
		if (i < best_header_row.size() - 1) {
			error << delimiter << " ";
		}
	}
	error << "\n";

	// 3. Suggest how to fix it!
	error << "Possible fixes:" << '\n';
	if (options.dialect_options.state_machine_options.strict_mode.GetValue()) {
		error << "* Disable the parser's strict mode (strict_mode=false) to allow reading rows that do not comply with "
		         "the CSV standard."
		      << '\n';
	}
	// header
	if (!options.dialect_options.header.IsSetByUser()) {
		error << "* Set header (header = true) if your CSV has a header, or (header = false) if it doesn't" << '\n';
	} else {
		error << "* Header is set to \'" << options.dialect_options.header.GetValue() << "\'. Consider unsetting it."
		      << '\n';
	}
	// skip_rows
	if (!options.dialect_options.skip_rows.IsSetByUser()) {
		error << "* Set skip (skip=${n}) to skip ${n} lines at the top of the file" << '\n';
	} else {
		error << "* Skip is set to \'" << options.dialect_options.skip_rows.GetValue() << "\'. Consider unsetting it."
		      << '\n';
	}
	// ignore_errors
	if (!options.ignore_errors.GetValue()) {
		error << "* Enable ignore errors (ignore_errors=true) to ignore potential errors" << '\n';
	}
	// null_padding
	if (!options.null_padding) {
		error << "* Enable null padding (null_padding=true) to pad missing columns with NULL values" << '\n';
	}

	return CSVError(error.str(), SNIFFING, {});
}

CSVError CSVError::SniffingError(const CSVReaderOptions &options, const string &search_space, idx_t max_columns_found,
                                 SetColumns &set_columns, bool type_detection) {
	std::ostringstream error;
	// 1. Which file
	error << "Error when sniffing file \"" << options.file_path << "\"." << '\n';
	// 2. What's the error
	error << "It was not possible to automatically detect the CSV parsing ";
	if (type_detection) {
		error << "types" << '\n';
	} else {
		error << "dialect" << '\n';
	}

	// 2. What was the search space?
	error << "The search space used was:" << '\n';
	error << search_space;
	error << "Encoding: " << options.encoding << '\n';
	// 3. Suggest how to fix it!
	error << "Possible fixes:" << '\n';
	// 3.0 Inform the user about the strict_mode
	// 3.1 Inform the reader of the dialect
	if (options.dialect_options.state_machine_options.strict_mode.GetValue()) {
		error << "* Disable the parser's strict mode (strict_mode=false) to allow reading rows that do not comply with "
		         "the CSV standard."
		      << '\n';
	}
	if (options.columns_set) {
		// If columns are set, suggest to either unset it or validate that it matches the schema
		error << "* Columns are set as: \"" << set_columns.ToString() << "\", and they contain: " << set_columns.Size()
		      << " columns. It does not match the number of columns found by the sniffer: " << max_columns_found << "."
		      << " Verify the columns parameter is correctly set." << '\n';
	}
	// 3.0.1 Inform the user about encoding
	error << "* Make sure you are using the correct file encoding. If not, set it (e.g., encoding = 'utf-16')." << '\n';
	// 3.1 Inform the reader of the dialect
	// delimiter
	if (!options.dialect_options.state_machine_options.delimiter.IsSetByUser()) {
		error << "* Set delimiter (e.g., delim=\',\')" << '\n';
	} else {
		error << "* Delimiter is set to \'" << options.dialect_options.state_machine_options.delimiter.GetValue()
		      << "\'. Consider unsetting it." << '\n';
	}
	// quote
	if (!options.dialect_options.state_machine_options.quote.IsSetByUser()) {
		error << "* Set quote (e.g., quote=\'\"\')" << '\n';
	} else {
		error << "* Quote is set to \'" << options.dialect_options.state_machine_options.quote.GetValue()
		      << "\'. Consider unsetting it." << '\n';
	}
	// escape
	if (!options.dialect_options.state_machine_options.escape.IsSetByUser()) {
		error << "* Set escape (e.g., escape=\'\"\')" << '\n';
	} else {
		error << "* Escape is set to \'" << options.dialect_options.state_machine_options.escape.GetValue()
		      << "\'. Consider unsetting it." << '\n';
	}
	// comment
	if (!options.dialect_options.state_machine_options.comment.IsSetByUser()) {
		error << "* Set comment (e.g., comment=\'#\')" << '\n';
	} else {
		error << "* Comment is set to \'" << options.dialect_options.state_machine_options.comment.GetValue()
		      << "\'. Consider unsetting it." << '\n';
	}
	// 3.2 skip_rows
	if (!options.dialect_options.skip_rows.IsSetByUser()) {
		error << "* Set skip (skip=${n}) to skip ${n} lines at the top of the file" << '\n';
	}
	// 3.3 ignore_errors
	if (!options.ignore_errors.GetValue()) {
		error << "* Enable ignore errors (ignore_errors=true) to ignore potential errors" << '\n';
	}
	// 3.4 null_padding
	if (!options.null_padding) {
		error << "* Enable null padding (null_padding=true) to pad missing columns with NULL values" << '\n';
	}
	error << "* Check you are using the correct file compression, otherwise set it (e.g., compression = \'zstd\')"
	      << '\n';
	error << "* Be sure that the maximum line size is set to an appropriate value, otherwise set it (e.g., "
	         "max_line_size=10000000)"
	      << "\n";
	return CSVError(error.str(), SNIFFING, {});
}

CSVError CSVError::NullPaddingFail(const CSVReaderOptions &options, LinesPerBoundary error_info,
                                   const String &current_path) {
	std::ostringstream error;
	error << " The parallel scanner does not support null_padding in conjunction with quoted new lines. Please "
	         "disable the parallel csv reader with parallel=false"
	      << '\n';
	// What were the options
	error << options.ToString(current_path);
	return CSVError(error.str(), NULLPADDED_QUOTED_NEW_VALUE, error_info);
}

CSVError CSVError::UnterminatedQuotesError(const CSVReaderOptions &options, idx_t current_column,
                                           LinesPerBoundary error_info, string &csv_row, idx_t row_byte_position,
                                           optional_idx byte_position, const String &current_path) {
	std::ostringstream error;
	error << "Value with unterminated quote found." << '\n';
	std::ostringstream how_to_fix_it;
	how_to_fix_it << "Possible fixes:" << '\n';
	if (options.dialect_options.state_machine_options.strict_mode.GetValue()) {
		how_to_fix_it << "* Disable the parser's strict mode (strict_mode=false) to allow reading rows that do not "
		                 "comply with the CSV standard."
		              << '\n';
	}
	how_to_fix_it << "* Enable ignore errors (ignore_errors=true) to skip this row" << '\n';
	how_to_fix_it << "* Set quote to empty or to a different value (e.g., quote=\'\')" << '\n';
	return CSVError(error.str(), UNTERMINATED_QUOTES, current_column, csv_row, error_info, row_byte_position,
	                byte_position, options, how_to_fix_it.str(), current_path);
}

CSVError CSVError::IncorrectColumnAmountError(const CSVReaderOptions &options, idx_t actual_columns,
                                              LinesPerBoundary error_info, string &csv_row, idx_t row_byte_position,
                                              optional_idx byte_position, const String &current_path) {
	std::ostringstream error;
	// We don't have a fix for this
	std::ostringstream how_to_fix_it;
	how_to_fix_it << "Possible fixes:" << '\n';
	if (options.dialect_options.state_machine_options.strict_mode.GetValue()) {
		how_to_fix_it << "* Disable the parser's strict mode (strict_mode=false) to allow reading rows that do not "
		                 "comply with the CSV standard."
		              << '\n';
	}
	if (!options.null_padding) {
		how_to_fix_it << "* Enable null padding (null_padding=true) to replace missing values with NULL" << '\n';
	}
	if (!options.ignore_errors.GetValue()) {
		how_to_fix_it << "* Enable ignore errors (ignore_errors=true) to skip this row" << '\n';
	}
	// How many columns were expected and how many were found
	error << "Expected Number of Columns: " << options.dialect_options.num_cols << " Found: " << actual_columns + 1;
	idx_t byte_pos = byte_position.GetIndex() == 0 ? 0 : byte_position.GetIndex() - 1;
	if (actual_columns >= options.dialect_options.num_cols) {
		return CSVError(error.str(), TOO_MANY_COLUMNS, actual_columns, csv_row, error_info, row_byte_position, byte_pos,
		                options, how_to_fix_it.str(), current_path);
	} else {
		return CSVError(error.str(), TOO_FEW_COLUMNS, actual_columns, csv_row, error_info, row_byte_position, byte_pos,
		                options, how_to_fix_it.str(), current_path);
	}
}

CSVError CSVError::InvalidUTF8(const CSVReaderOptions &options, idx_t current_column, LinesPerBoundary error_info,
                               string &csv_row, idx_t row_byte_position, optional_idx byte_position,
                               const String &current_path) {
	std::ostringstream error;
	// How many columns were expected and how many were found
	error << "Invalid unicode (byte sequence mismatch) detected. This file is not " << options.encoding << " encoded."
	      << '\n';
	std::ostringstream how_to_fix_it;
	how_to_fix_it
	    << "Possible Solution: Set the correct encoding, if available, to read this CSV File (e.g., encoding='UTF-16')"
	    << '\n';
	how_to_fix_it << "Possible Solution: Enable ignore errors (ignore_errors=true) to skip this row" << '\n';
	return CSVError(error.str(), INVALID_ENCODING, current_column, csv_row, error_info, row_byte_position,
	                byte_position, options, how_to_fix_it.str(), current_path);
}

bool CSVErrorHandler::PrintLineNumber(const CSVError &error) const {
	if (!print_line) {
		return false;
	}
	switch (error.type) {
	case CAST_ERROR:
	case UNTERMINATED_QUOTES:
	case TOO_FEW_COLUMNS:
	case TOO_MANY_COLUMNS:
	case MAXIMUM_LINE_SIZE:
	case NULLPADDED_QUOTED_NEW_VALUE:
	case INVALID_ENCODING:
		return true;
	default:
		return false;
	}
}

bool CSVErrorHandler::CanGetLine(idx_t boundary_index) {
	for (idx_t i = 0; i < boundary_index; i++) {
		if (lines_per_batch_map.find(i) == lines_per_batch_map.end()) {
			return false;
		}
	}
	return true;
}

idx_t CSVErrorHandler::GetLine(const LinesPerBoundary &error_info) {
	lock_guard<mutex> parallel_lock(main_mutex);
	return GetLineInternal(error_info);
}
idx_t CSVErrorHandler::GetLineInternal(const LinesPerBoundary &error_info) {
	// We start from one, since the lines are 1-indexed
	idx_t current_line = 1 + error_info.lines_in_batch;
	for (idx_t boundary_idx = 0; boundary_idx < error_info.boundary_idx; boundary_idx++) {
		current_line += lines_per_batch_map[boundary_idx].lines_in_batch;
	}
	return current_line;
}

} // namespace duckdb
