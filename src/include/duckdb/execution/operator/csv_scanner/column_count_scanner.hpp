//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/csv_scanner/column_count_scanner.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/operator/csv_scanner/csv_buffer_manager.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_state_machine.hpp"
#include "duckdb/execution/operator/csv_scanner/scanner_boundary.hpp"
#include "duckdb/execution/operator/csv_scanner/string_value_scanner.hpp"
#include "duckdb/execution/operator/csv_scanner/base_scanner.hpp"
#include "duckdb/common/shared_ptr.hpp"

namespace duckdb {

//! Result of a sniffed tuples using the column count scanner
struct ColumnCount {
	//! Number of columns found in a row
	idx_t number_of_columns = 0;
	//! If all values from this row onwards are null
	bool last_value_always_empty = true;
	//! If this row is potentially a comment
	bool is_comment = false;
	//! If this row is potentially a mid-line comment
	bool is_mid_comment = false;
	//! Number of empty lines before this value
	idx_t empty_lines = 0;
};

class ColumnCountResult : public ScannerResult {
public:
	ColumnCountResult(CSVStates &states, CSVStateMachine &state_machine, idx_t result_size,
	                  CSVErrorHandler &error_handler);
	inline ColumnCount &operator[](size_t index) {
		return column_counts[index];
	}

	vector<ColumnCount> column_counts;
	idx_t current_column_count = 0;
	bool error = false;
	idx_t result_position = 0;
	//! Full-line comments excluded from the current chunk's sample budget
	idx_t comment_rows = 0;
	//! Automatic full-line comments accepted across all chunks
	idx_t automatic_comment_rows = 0;
	//! Confirmed full-line comments excluded from data sampling and column votes
	bool full_line_comment = false;
	//! The current row start follows a CR, possibly before an LF in the next buffer
	bool row_start_after_carriage_return = false;

	idx_t cur_buffer_idx = 0;
	idx_t current_buffer_size = 0;
	//! How many rows fit a given column count
	map<idx_t, idx_t> rows_per_column_count;
	CSVErrorHandler &error_handler;
	map<idx_t, shared_ptr<CSVBufferHandle>> buffer_handles;
	idx_t empty_lines = 0;
	//! Adds a Value to the result
	static inline void AddValue(ColumnCountResult &result, idx_t buffer_pos);
	//! Adds a Row to the result
	static inline bool AddRow(ColumnCountResult &result, idx_t buffer_pos);
	//! Behavior when hitting an invalid state
	static inline void InvalidState(ColumnCountResult &result);
	//! Handles QuotedNewline State
	static inline void QuotedNewLine(ColumnCountResult &result);
	//! Handles EmptyLine states
	static inline bool EmptyLine(ColumnCountResult &result, idx_t buffer_pos);
	//! Handles unset comment
	static inline bool UnsetComment(ColumnCountResult &result, idx_t buffer_pos);

	static inline void SetComment(ColumnCountResult &result, idx_t buffer_pos);

	//! Returns the column count
	idx_t GetMostFrequentColumnCount() const;
	//! Return whether the current chunk's data-row budget is exhausted
	bool SniffingLimitReached() const;

	inline void InternalAddRow();

private:
	bool IsCommentPrefixEmpty(idx_t buffer_pos) const;
};

//! Scanner that goes over the CSV and figures out how many columns each row has. Used for dialect sniffing
class ColumnCountScanner : public BaseScanner {
public:
	ColumnCountScanner(shared_ptr<CSVBufferManager> buffer_manager, const shared_ptr<CSVStateMachine> &state_machine,
	                   shared_ptr<CSVErrorHandler> error_handler, idx_t result_size = STANDARD_VECTOR_SIZE,
	                   CSVIterator iterator = {});

	ColumnCountResult &ParseChunk() override;

	ColumnCountResult &GetResult() override;

	unique_ptr<StringValueScanner> UpgradeToStringValueScanner();

	//! Limit the number of rows produced by the next parse without reallocating the result
	void SetResultLimit(idx_t result_size);

private:
	void Initialize() override;

	void FinalizeChunkProcess() override;

	ColumnCountResult result;

	idx_t column_count;
	idx_t result_size;
};

} // namespace duckdb
