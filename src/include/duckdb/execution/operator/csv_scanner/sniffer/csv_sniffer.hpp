//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/csv_scanner/sniffer/csv_sniffer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/operator/csv_scanner/csv_state_machine.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/execution/operator/csv_scanner/column_count_scanner.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_schema.hpp"
#include "duckdb/execution/operator/csv_scanner/header_value.hpp"
#include "duckdb/execution/operator/csv_scanner/sniffer/sniff_result.hpp"

namespace duckdb {
struct DateTimestampSniffing {
	bool initialized = false;
	bool had_match = false;
	vector<string> format;
	idx_t initial_size;
};

struct QuoteEscapeCombination {
	QuoteEscapeCombination(char quote, char escape) : quote(quote), escape(escape) {
	}
	char quote;
	char escape;
};

//! Current stats of candidate analysis
struct CandidateStats {
	//! Number of rows read
	idx_t rows_read = 0;
	//! Best Number of consistent rows (i.e., presenting all columns)
	idx_t best_consistent_rows = 0;
	//! If padding was necessary (i.e., rows are missing some columns, how many)
	idx_t prev_padding_count = 0;
	//! Min number of ignored rows
	idx_t min_ignored_rows = 0;
};

//! All the options that will be used to sniff the dialect of the CSV file
struct DialectCandidates {
	//! The constructor populates all of our the options that will be used in our sniffer search space
	explicit DialectCandidates(const CSVStateMachineOptions &options);

	//! Static functions to get defaults of the search space
	static vector<string> GetDefaultDelimiter();
	//! Default Quote/Escape combinations in priority order
	static vector<QuoteEscapeCombination> GetDefaultQuoteEscapeCombination();

	static vector<char> GetDefaultComment();

	string Print();

	//! Candidates for the delimiter
	vector<string> delim_candidates;
	//! Candidates for the comment
	vector<char> comment_candidates;
	//! Candidates for combinations of quotes and escapes
	vector<QuoteEscapeCombination> quote_escape_candidates;
};

//! Struct used to know if we have a date or timestamp type already identified in this CSV File
struct HasType {
	bool date = false;
	bool timestamp = false;
};

//! Sniffer that detects Header, Dialect and Types of CSV Files
class CSVSniffer {
public:
	explicit CSVSniffer(CSVReaderOptions &options_p, const MultiFileOptions &file_options,
	                    shared_ptr<CSVBufferManager> buffer_manager_p, CSVStateMachineCache &state_machine_cache,
	                    bool default_null_to_varchar = true);

	//! Main method that sniffs the CSV file, returns the types, names and options as a result
	//! CSV Sniffing consists of five steps:
	//! 1. Dialect Detection: Generate the CSV Options (delimiter, quote, escape, etc.)
	//! 2. Type Detection: Figures out the types of the columns (For one chunk)
	//! 3. Type Refinement: Refines the types of the columns for the remaining chunks
	//! 4. Header Detection: Figures out if  the CSV file has a header and produces the names of the columns
	//! 5. Type Replacement: Replaces the types of the columns if the user specified them
	SnifferResult SniffCSV(bool force_match = false);

	//! I call it adaptive, since that's a sexier term.
	//! In practice this function sniffs up to STANDARD_VECTOR_SIZE rows to verify if a header exists and determine the
	//! data types. It does this considering a priorly set CSV schema. If there is a mismatch of the schema it runs the
	//! full on blazing all guns sniffer, if that still fails it tells the user to union_by_name.
	//! It returns the projection order.
	SnifferResult AdaptiveSniff(const CSVSchema &file_schema);

	//! Function that sniffs up to STANDARD_VECTOR_SIZE rows to detect a header and data types
	AdaptiveSnifferResult MinimalSniff();

	static NewLineIdentifier DetectNewLineDelimiter(CSVBufferManager &buffer_manager);

	//! If a string_t value can be cast to a type
	static bool CanYouCastIt(ClientContext &context, const string_t value, const LogicalType &type,
	                         const DialectOptions &dialect_options, const bool is_null, const char decimal_separator,
	                         const char thousands_separator);
	static bool CanYouCastBignum(const char *value_ptr, idx_t value_size);

	idx_t LinesSniffed() const;

	bool EmptyOrOnlyHeader() const;

private:
	//! If all our candidates failed due to lines being bigger than the max line size.
	bool all_fail_max_line_size = true;
	CSVError line_error;
	//! CSV State Machine Cache
	CSVStateMachineCache &state_machine_cache;
	//! Highest number of columns found
	idx_t max_columns_found = 0;
	idx_t max_columns_found_error = 0;
	bool best_candidate_is_strict = false;
	//! Current Candidates being considered
	vector<unique_ptr<ColumnCountScanner>> candidates;
	//! Reference to original CSV Options, it will be modified as a result of the sniffer.
	CSVReaderOptions &options;
	//! The multi-file reader options
	const MultiFileOptions &file_options;
	//! Buffer being used on sniffer
	shared_ptr<CSVBufferManager> buffer_manager;
	//! Information regarding columns that were set by user/query
	SetColumns set_columns;
	shared_ptr<CSVErrorHandler> error_handler;
	shared_ptr<CSVErrorHandler> detection_error_handler;
	//! Number of lines sniffed in this sniffer
	idx_t lines_sniffed;
	//! Sets the result options
	void SetResultOptions() const;

	//! ------------------------------------------------------//
	//! ----------------- Dialect Detection ----------------- //
	//! ------------------------------------------------------//
	//! First phase of auto detection: detect CSV dialect (i.e. delimiter, quote rules, etc)
	void DetectDialect();
	//! Functions called in the main DetectDialect(); function
	//! 1. Generates the search space candidates for the state machines
	void GenerateStateMachineSearchSpace(vector<unique_ptr<ColumnCountScanner>> &column_count_scanners,
	                                     const DialectCandidates &dialect_candidates);

	//! 2. Analyzes if a dialect candidate is a good candidate to be considered, if so, it adds it to the candidates
	void AnalyzeDialectCandidate(unique_ptr<ColumnCountScanner>, CandidateStats &stats,
	                             vector<unique_ptr<ColumnCountScanner>> &successful_candidates);
	//! 3. Refine Candidates over remaining chunks
	void RefineCandidates();

	//! Checks if candidate still produces good values for the next chunk
	bool RefineCandidateNextChunk(ColumnCountScanner &candidate) const;

	//! ------------------------------------------------------//
	//! ------------------- Type Detection ------------------ //
	//! ------------------------------------------------------//
	//! Second phase of auto-detection: detect types, format template candidates
	//! ordered by descending specificity (~ from high to low)
	void DetectTypes();
	//! Change the date format for the type to the string
	//! Try to cast a string value to the specified sql type
	static void SetDateFormat(CSVStateMachine &candidate, const string &format_specifier,
	                          const LogicalTypeId &sql_type);

	//! Function that initialized the necessary variables used for date and timestamp detection
	void InitializeDateAndTimeStampDetection(CSVStateMachine &candidate, const string &separator,
	                                         const LogicalType &sql_type);
	//! Sets user defined date and time formats (if any)
	void SetUserDefinedDateTimeFormat(CSVStateMachine &candidate) const;
	//! Functions that performs detection for date and timestamp formats
	void DetectDateAndTimeStampFormats(CSVStateMachine &candidate, const LogicalType &sql_type, const string &separator,
	                                   const string_t &dummy_val);
	//! Sniffs the types from a data chunk
	void SniffTypes(DataChunk &data_chunk, CSVStateMachine &state_machine,
	                unordered_map<idx_t, vector<LogicalType>> &info_sql_types_candidates, idx_t start_idx_detection);

	//! Variables for Type Detection
	//! Format Candidates for Date and Timestamp Types
	const map<LogicalTypeId, vector<const char *>> format_template_candidates = {
	    {LogicalTypeId::DATE, {"%m-%d-%Y", "%m-%d-%y", "%d-%m-%Y", "%d-%m-%y", "%Y-%m-%d", "%y-%m-%d"}},
	    {LogicalTypeId::TIMESTAMP,
	     {"%Y-%m-%d %H:%M:%S.%f", "%m-%d-%Y %I:%M:%S %p", "%m-%d-%y %I:%M:%S %p", "%d-%m-%Y %H:%M:%S",
	      "%d-%m-%y %H:%M:%S", "%Y-%m-%d %H:%M:%S", "%y-%m-%d %H:%M:%S"}},
	};
	unordered_map<idx_t, vector<LogicalType>> best_sql_types_candidates_per_column_idx;
	map<LogicalTypeId, vector<string>> best_format_candidates;
	unique_ptr<StringValueScanner> best_candidate;
	vector<HeaderValue> best_header_row;
	//! Variable used for sniffing date and timestamp
	map<LogicalTypeId, DateTimestampSniffing> format_candidates;
	map<LogicalTypeId, DateTimestampSniffing> original_format_candidates;

	//! ------------------------------------------------------//
	//! ------------------ Type Refinement ------------------ //
	//! ------------------------------------------------------//
	void RefineTypes();
	bool TryCastVector(Vector &parse_chunk_col, idx_t size, const LogicalType &sql_type) const;
	vector<LogicalType> detected_types;
	//! If when finding a SQLNULL type in type detection we default it to varchar
	const bool default_null_to_varchar;
	//! ------------------------------------------------------//
	//! ------------------ Header Detection ----------------- //
	//! ------------------------------------------------------//
	void DetectHeader();
	static bool DetectHeaderWithSetColumn(ClientContext &context, vector<HeaderValue> &best_header_row,
	                                      const SetColumns &set_columns, CSVReaderOptions &options);
	static vector<Identifier>
	DetectHeaderInternal(ClientContext &context, vector<HeaderValue> &best_header_row, CSVStateMachine &state_machine,
	                     const SetColumns &set_columns,
	                     unordered_map<idx_t, vector<LogicalType>> &best_sql_types_candidates_per_column_idx,
	                     CSVReaderOptions &options, const MultiFileOptions &file_options,
	                     CSVErrorHandler &error_handler);
	vector<Identifier> names;
	//! If the file only has a header
	bool single_row_file = false;

	//! ------------------------------------------------------//
	//! ------------------ Type Replacement ----------------- //
	//! ------------------------------------------------------//
	void ReplaceTypes();
	vector<bool> manually_set;
};

} // namespace duckdb
