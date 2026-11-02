//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/csv_scanner/csv_error.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_reader_options.hpp"
#include "duckdb/execution/operator/csv_scanner/header_value.hpp"
#include "duckdb/execution/operator/csv_scanner/set_columns.hpp"

namespace duckdb {
class InternalAppender;
class CSVFileScan;
class CSVRejectsTable;
struct ReadCSVData;
struct MultiFileBindData;

//! Object that holds information on how many lines each csv batch read.
class LinesPerBoundary {
public:
	LinesPerBoundary();
	LinesPerBoundary(idx_t boundary_idx, idx_t lines_in_batch);

	idx_t boundary_idx = 0;
	idx_t lines_in_batch = 0;
};

enum CSVErrorType : uint8_t {
	CAST_ERROR = 0,                //! If when casting a value from string to the column type fails
	COLUMN_NAME_TYPE_MISMATCH = 1, //! If there is a mismatch between Column Names and Types
	TOO_FEW_COLUMNS = 2,           //! If the CSV has too few columns
	TOO_MANY_COLUMNS = 3,          //! If the CSV has too many  column
	UNTERMINATED_QUOTES = 4,       //! If a quote is not terminated
	SNIFFING = 5,          //! If something went wrong during sniffing and was not possible to find suitable candidates
	MAXIMUM_LINE_SIZE = 6, //! Maximum line size was exceeded by a line in the CSV File
	NULLPADDED_QUOTED_NEW_VALUE = 7, //! If the null_padding option is set, and we have quoted new values in parallel
	INVALID_ENCODING = 8,            //! If we have invalid UTF-8 encoded values
	INVALID_STATE = 9                //! If our CSV Scanner ended up in an invalid state
};

class CSVError {
public:
	CSVError() {};
	CSVError(string error_message, CSVErrorType type, idx_t column_idx, string csv_row, LinesPerBoundary error_info,
	         idx_t row_byte_position, optional_idx byte_position, const CSVReaderOptions &reader_options,
	         const string &fixes, const String &current_path);
	CSVError(string error_message, CSVErrorType type, LinesPerBoundary error_info);
	//! Produces error messages for column name -> type mismatch.
	static CSVError ColumnTypesError(identifier_map_t<idx_t> sql_types_per_column, const vector<Identifier> &names);
	//! Produces error messages for casting errors
	static CSVError CastError(const CSVReaderOptions &options, const string &column_name, string &cast_error,
	                          idx_t column_idx, string &csv_row, LinesPerBoundary error_info, idx_t row_byte_position,
	                          optional_idx byte_position, LogicalTypeId type, const String &current_path);
	//! Produces error for when the line size exceeds the maximum line size option
	static CSVError LineSizeError(const CSVReaderOptions &options, LinesPerBoundary error_info, string &csv_row,
	                              idx_t byte_position, const String &current_path);
	//! Produces error for when the state machine reaches an invalid state
	static CSVError InvalidState(const CSVReaderOptions &options, idx_t current_column, LinesPerBoundary error_info,
	                             string &csv_row, idx_t row_byte_position, optional_idx byte_position,
	                             const String &current_path);
	//! Produces an error message for a dialect sniffing error.
	static CSVError SniffingError(const CSVReaderOptions &options, const string &search_space, idx_t max_columns_found,
	                              SetColumns &set_columns, bool type_detection);
	//! Produces an error message for a header sniffing error.
	static CSVError HeaderSniffingError(const CSVReaderOptions &options, const vector<HeaderValue> &best_header_row,
	                                    idx_t column_count, const string &delimiter);
	//! Produces error messages for unterminated quoted values
	static CSVError UnterminatedQuotesError(const CSVReaderOptions &options, idx_t current_column,
	                                        LinesPerBoundary error_info, string &csv_row, idx_t row_byte_position,
	                                        optional_idx byte_position, const String &current_path);
	//! Produces error messages for null_padding option is set, and we have quoted new values in parallel
	static CSVError NullPaddingFail(const CSVReaderOptions &options, LinesPerBoundary error_info,
	                                const String &current_path);
	//! Produces error for incorrect (e.g., smaller and lower than the predefined) number of columns in a CSV Line
	static CSVError IncorrectColumnAmountError(const CSVReaderOptions &state_machine, idx_t actual_columns,
	                                           LinesPerBoundary error_info, string &csv_row, idx_t row_byte_position,
	                                           optional_idx byte_position, const String &current_path);
	static CSVError InvalidUTF8(const CSVReaderOptions &options, idx_t current_column, LinesPerBoundary error_info,
	                            string &csv_row, idx_t row_byte_position, optional_idx byte_position,
	                            const String &current_path);

	idx_t GetBoundaryIndex() const {
		return error_info.boundary_idx;
	}

	//! We might want to remove newline in errors if we are doing them for the rejects tables
	static void RemoveNewLine(string &error);

	//! Actual error message
	string error_message;
	//! Full error message used in throws
	//! 1. The Actual error
	//! 2. How to fix it
	//! 3. Options that generated the error
	string full_error_message;
	//! Error Type
	CSVErrorType type {};
	//! Column Index where error happened
	idx_t column_idx {};
	//! Original CSV row where error happened
	string csv_row;
	//! Line information regarding this error
	LinesPerBoundary error_info;
	//! Byte position of where the row starts
	idx_t row_byte_position {};
	//! Byte Position where error occurred.
	optional_idx byte_position;
};

class CSVErrorHandler {
public:
	//! If store_ignored_errors is false, ignored errors are dropped instead of kept for the rejects table
	explicit CSVErrorHandler(bool ignore_errors = false, bool store_ignored_errors = true);
	//! Throws the error
	void Error(const CSVError &csv_error, bool force_error = false);
	//! If we have a cached error, and we can now error, we error.
	void ErrorIfNeeded();
	//! Throw an error if there are any
	void ErrorIfAny();
	//! Throws an error if a given type exists
	void ErrorIfTypeExists(CSVErrorType error_type);
	//! Inserts a finished error info
	void Insert(idx_t boundary_idx, idx_t rows);
	idx_t GetLine(const LinesPerBoundary &error_info);
	void NewMaxLineSize(idx_t scan_line_size);
	//! Returns true if there are any errors
	bool AnyErrors();
	bool HasError(CSVErrorType error_type);
	CSVError GetFirstError(CSVErrorType error_type);
	idx_t GetMaxLineLength();

	void DontPrintErrorLine();

	void SetIgnoreErrors(bool ignore_errors);
	idx_t GetSize();

	void FillRejectsTable(InternalAppender &errors_appender, idx_t file_idx, idx_t scan_idx, const CSVFileScan &file,
	                      CSVRejectsTable &rejects, const vector<Identifier> &column_names, idx_t limit);

private:
	//! Private methods should always be locked by parent method.
	//! If we should print the line of an error
	bool PrintLineNumber(const CSVError &error) const;
	//! Method that actually throws the error
	void ThrowError(const CSVError &csv_error);
	//! If we processed all boundaries before the one that error-ed
	bool CanGetLine(idx_t boundary_index);
	//! Return the 1-indexed line number
	idx_t GetLineInternal(const LinesPerBoundary &error_info);
	//! CSV Error Handler Mutex
	mutex main_mutex;
	//! Map of <boundary indexes> -> lines read
	unordered_map<idx_t, LinesPerBoundary> lines_per_batch_map;
	idx_t max_line_length = 0;
	bool ignore_errors = false;
	//! Whether ignored errors have to be kept, e.g. to fill the rejects table
	bool store_ignored_errors = true;
	bool print_line = true;
	//! Set of errors
	vector<CSVError> errors;
};

} // namespace duckdb
