//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/csv_scanner/string_value_scanner.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/operator/csv_scanner/csv_buffer_manager.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_state_machine.hpp"
#include "duckdb/execution/operator/csv_scanner/scanner_boundary.hpp"
#include "duckdb/execution/operator/csv_scanner/base_scanner.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_validator.hpp"
namespace duckdb {

struct CSVBufferUsage {
	CSVBufferUsage(CSVBufferManager &buffer_manager_p, idx_t buffer_idx_p)
	    : buffer_manager(buffer_manager_p), buffer_idx(buffer_idx_p) {

	                                        };
	~CSVBufferUsage() {
		buffer_manager.ResetBuffer(buffer_idx);
	}
	CSVBufferManager &buffer_manager;
	idx_t buffer_idx;
};

//! Keeps track of start and end of line positions in regard to the CSV file
class FullLinePosition {
public:
	FullLinePosition() {};
	LinePosition begin;
	LinePosition end;
	static void SanitizeError(string &value);
	//! Reconstructs the current line to be used in error messages
	template <class T>
	string ReconstructCurrentLine(bool &first_char_nl, T &buffer_handles, bool reconstruct_line) const {
		if (!reconstruct_line || begin == end) {
			return {};
		}
		string result;
		if (end.buffer_idx == begin.buffer_idx || begin.buffer_pos == begin.buffer_size) {
			idx_t buffer_idx = end.buffer_idx;
			if (buffer_handles.find(buffer_idx) == buffer_handles.end()) {
				return {};
			}
			idx_t start_pos = begin.buffer_pos == begin.buffer_size ? 0 : begin.buffer_pos;
			auto buffer = buffer_handles[buffer_idx]->Ptr();
			first_char_nl = buffer[start_pos] == '\n' || buffer[start_pos] == '\r';
			for (idx_t i = start_pos + first_char_nl; i < end.buffer_pos; i++) {
				result += buffer[i];
			}
		} else {
			if (buffer_handles.find(begin.buffer_idx) == buffer_handles.end() ||
			    buffer_handles.find(end.buffer_idx) == buffer_handles.end()) {
				return {};
			}
			if (begin.buffer_pos >= begin.buffer_size) {
				throw InternalException("CSV reader: buffer pos out of range for buffer");
			}
			auto first_buffer = buffer_handles[begin.buffer_idx]->Ptr();
			auto first_buffer_size = buffer_handles[begin.buffer_idx]->actual_size;
			auto second_buffer = buffer_handles[end.buffer_idx]->Ptr();
			first_char_nl = first_buffer[begin.buffer_pos] == '\n' || first_buffer[begin.buffer_pos] == '\r';
			for (idx_t i = begin.buffer_pos + first_char_nl; i < first_buffer_size; i++) {
				result += first_buffer[i];
			}
			for (idx_t i = 0; i < end.buffer_pos; i++) {
				result += second_buffer[i];
			}
		}
		// sanitize borked line
		SanitizeError(result);
		return result;
	}
};

class StringValueResult;

class CurrentError {
public:
	CurrentError(CSVErrorType type, idx_t col_idx_p, idx_t chunk_idx_p, const LinePosition &error_position_p,
	             idx_t current_line_size_p)
	    : type(type), col_idx(col_idx_p), chunk_idx(chunk_idx_p), current_line_size(current_line_size_p),
	      error_position(error_position_p) {};
	//! Error Type (e.g., Cast, Wrong # of columns, ...)
	CSVErrorType type;
	//! Column index related to the CSV File columns
	idx_t col_idx;
	//! Column index related to the produced chunk (i.e., with projection applied)
	idx_t chunk_idx;
	//! Current CSV Line size in Bytes
	idx_t current_line_size;
	//! Error Message produced
	string error_message;
	//! Exact Position where the error happened
	LinePosition error_position;

	friend bool operator==(const CurrentError &error, CSVErrorType other) {
		return error.type == other;
	}
};

class LineError {
public:
	explicit LineError(const idx_t scan_id_p, const bool ignore_errors_p)
	    : is_error_in_line(false), ignore_errors(ignore_errors_p), scan_id(scan_id_p) {};
	//! We clear up our CurrentError Vector
	void Reset() {
		current_errors.clear();
		is_error_in_line = false;
	}
	void Insert(const CSVErrorType &type, const idx_t &col_idx, const idx_t &chunk_idx,
	            const LinePosition &error_position, const idx_t current_line_size = 0) {
		is_error_in_line = true;
		if (!ignore_errors) {
			// We store it for later
			current_errors.push_back({type, col_idx, chunk_idx, error_position, current_line_size});
			current_errors.back().current_line_size = current_line_size;
		}
	}
	//! Set that we currently have an error, but don't really store them
	void SetError() {
		is_error_in_line = true;
	}
	//! Dirty hack for adding cast message
	void ModifyErrorMessageOfLastError(string error_message) {
		D_ASSERT(!current_errors.empty() && current_errors.back().type == CSVErrorType::CAST_ERROR);
		current_errors.back().error_message = std::move(error_message);
	}

	bool HasErrorType(CSVErrorType type) const {
		for (auto &error : current_errors) {
			if (type == error.type) {
				return true;
			}
		}
		return false;
	}

	bool HandleErrors(StringValueResult &result);

	bool HasError() const {
		return !current_errors.empty();
	}

	idx_t Size() const {
		return current_errors.size();
	}

private:
	vector<CurrentError> current_errors;
	bool is_error_in_line;
	bool ignore_errors;
	idx_t scan_id;
};

struct ParseTypeInfo {
	ParseTypeInfo()
	    : validate_utf8(false), type_id(LogicalTypeId::INVALID), internal_type(PhysicalType::INVALID), scale(0),
	      width(0) {};
	ParseTypeInfo(const LogicalType &type, const bool validate_utf_8_p) : validate_utf8(validate_utf_8_p) {
		type_id = type.id();
		internal_type = type.InternalType();
		if (type.id() == LogicalTypeId::DECIMAL) {
			// We only care about these if we have a decimal value
			type.GetDecimalProperties(width, scale);
		}
	}

	bool validate_utf8;
	LogicalTypeId type_id;
	PhysicalType internal_type;
	uint8_t scale;
	uint8_t width;
};

class StringValueResult : public ScannerResult {
public:
	StringValueResult(CSVStates &states, CSVStateMachine &state_machine,
	                  const shared_ptr<CSVBufferHandle> &buffer_handle, Allocator &buffer_allocator,
	                  idx_t result_size_p, idx_t buffer_position, CSVErrorHandler &error_handler, CSVIterator &iterator,
	                  shared_ptr<CSVFileScan> csv_file_scan, idx_t &lines_read, bool sniffing, const string &path,
	                  idx_t scan_id, bool &used_unstrictness);

	~StringValueResult();

	//! Information on the vector
	unsafe_vector<void *> vector_ptr;
	unsafe_vector<ValidityMask *> validity_mask;

	//! Variables to iterate over the CSV buffers

	const char *buffer_ptr;
	idx_t buffer_size;
	idx_t position_before_comment;

	//! CSV Options that impact the parsing
	const uint32_t number_of_columns;
	const bool null_padding;
	const bool ignore_errors;

	const idx_t extra_delimiter_bytes = 0;

	unsafe_unique_array<const char *> null_str_ptr;
	unsafe_unique_array<idx_t> null_str_size;
	idx_t null_str_count;

	//! Internal Data Chunk used for flushing
	DataChunk parse_chunk;
	int64_t number_of_rows = 0;
	idx_t cur_col_id = 0;
	bool figure_out_new_line = false;
	//! Information to properly handle errors
	CSVErrorHandler &error_handler;
	CSVIterator &iterator;
	//! Line position of the current line
	FullLinePosition current_line_position;
	//! Used for CSV line reconstruction on flushed errors
	unordered_map<idx_t, FullLinePosition> line_positions_per_row;
	bool added_last_line = false;
	bool quoted_new_line = false;

	//! If we are trying a row or not when figuring out the next row to start from.
	bool try_row = false;

	unsafe_unique_array<ParseTypeInfo> parse_types;
	vector<string> names;

	shared_ptr<CSVFileScan> csv_file_scan;
	idx_t &lines_read;
	bool &used_unstrictness;
	//! Information regarding projected columns
	unsafe_unique_array<bool> projected_columns;
	bool projecting_columns = false;
	idx_t chunk_col_id = 0;

	bool icu_loaded = false;

	//! We must ensure that we keep the buffers alive until processing the query result
	unordered_map<idx_t, shared_ptr<CSVBufferHandle>> buffer_handles;

	//! Requested size of buffers (i.e., either 32Mb or set by buffer_size parameter)
	idx_t requested_size;

	//! Errors happening in the current line (if any)
	LineError current_errors;
	StrpTimeFormat date_format, timestamp_format;
	bool sniffing;

	char decimal_separator;

	//! We store borked rows so we can generate multiple errors during flushing
	unordered_set<idx_t> borked_rows;

	String path;

	//! Variable used when trying to figure out where a new segment starts, we must always start from a Valid
	//! (i.e., non-comment) line.
	bool first_line_is_comment = false;

	bool ignore_empty_values = true;

	//! Specialized code for quoted values, makes sure to remove quotes and escapes
	static inline void AddQuotedValue(StringValueResult &result, const idx_t buffer_pos);
	//! Specialized code for possibly escaped values, makes sure to remove escapes
	static inline void AddPossiblyEscapedValue(StringValueResult &result, const idx_t buffer_pos, const char *value_ptr,
	                                           const idx_t length, const bool empty);
	//! Adds a Value to the result
	static inline void AddValue(StringValueResult &result, const idx_t buffer_pos);
	//! Adds a Row to the result
	static inline bool AddRow(StringValueResult &result, const idx_t buffer_pos);
	//! Behavior when hitting an invalid state
	static inline void InvalidState(StringValueResult &result);
	//! Handles QuotedNewline State
	static inline void QuotedNewLine(StringValueResult &result);
	void NullPaddingQuotedNewlineCheck() const;
	//! Handles EmptyLine states
	static inline bool EmptyLine(StringValueResult &result, const idx_t buffer_pos);
	inline bool AddRowInternal();
	//! Marks the columns of a borked row that were never written as NULL
	void InvalidateUnwrittenColumns(idx_t first_unwritten_col);
	//! Force the throw of a Unicode error
	void HandleUnicodeError(idx_t col_idx, LinePosition &error_position);
	bool HandleTooManyColumnsError(const char *value_ptr, const idx_t size);
	inline void AddValueToVector(const char *value_ptr, idx_t size, bool allocate = false);
	static inline void SetComment(StringValueResult &result, idx_t buffer_pos);
	static inline bool UnsetComment(StringValueResult &result, idx_t buffer_pos);

	inline idx_t HandleMultiDelimiter(const idx_t buffer_pos) const;

	DataChunk &ToChunk();
	//! Resets the state of the result
	void Reset();

	//! BOM skipping (https://en.wikipedia.org/wiki/Byte_order_mark)
	void SkipBOM() const;
	//! If we should Print Error Lines
	//! We only really care about error lines if we are going to error or store them in a rejects table
	bool PrintErrorLine() const;
	//! Removes last added line, usually because we figured out later on that it's an ill-formed line
	//! or that it does not fit our schema
	void RemoveLastLine();
};

struct ValidRowInfo {
	ValidRowInfo(bool is_valid_p, idx_t start_pos_p, idx_t end_buffer_idx_p, idx_t end_pos_p, bool last_state_quote_p)
	    : is_valid(is_valid_p), start_pos(start_pos_p), end_buffer_idx(end_buffer_idx_p), end_pos(end_pos_p),
	      last_state_quote(last_state_quote_p) {};
	ValidRowInfo() : is_valid(false), start_pos(0), end_buffer_idx(0), end_pos(0) {};

	bool is_valid;
	idx_t start_pos;
	idx_t end_buffer_idx;
	idx_t end_pos;
	bool last_state_quote = false;
};
//! Our dialect scanner basically goes over the CSV and actually parses the values to a DuckDB vector of string_t
class StringValueScanner : public BaseScanner {
public:
	StringValueScanner(idx_t scanner_idx, const shared_ptr<CSVBufferManager> &buffer_manager,
	                   const shared_ptr<CSVStateMachine> &state_machine,
	                   const shared_ptr<CSVErrorHandler> &error_handler, const shared_ptr<CSVFileScan> &csv_file_scan,
	                   bool sniffing = false, const CSVIterator &boundary = {},
	                   idx_t result_size = STANDARD_VECTOR_SIZE, bool can_suspend = false);

	StringValueScanner(const shared_ptr<CSVBufferManager> &buffer_manager,
	                   const shared_ptr<CSVStateMachine> &state_machine,
	                   const shared_ptr<CSVErrorHandler> &error_handler, idx_t result_size = STANDARD_VECTOR_SIZE,
	                   const CSVIterator &boundary = {});

	StringValueResult &ParseChunk() override;

	//! Flushes the result to the insert_chunk
	void Flush(DataChunk &insert_chunk);

	//! Function that creates and returns a non-boundary CSV Scanner, can be used for internal csv reading.
	static unique_ptr<StringValueScanner> GetCSVScanner(ClientContext &context, CSVReaderOptions &options,
	                                                    const MultiFileOptions &file_options);

	bool FinishedIterator() const;

	//! Limit the number of rows produced by the next parse without reallocating the result
	void SetResultLimit(idx_t result_size);

	//! Creates a new string with all escaped values removed
	static string_t RemoveEscape(const char *str_ptr, idx_t end, char escape, char quote, bool strict_mode,
	                             Vector &vector);

	//! If we can directly cast the type when consuming the CSV file, or we have to do it later
	static bool CanDirectlyCast(const LogicalType &type, bool icu_loaded);

	//! Gets validation line information
	ValidatorLine GetValidationLine();

	//! Whether the scanner is suspended waiting for a buffer
	bool IsSuspended() const {
		return suspended;
	}
	//! The buffer a suspended scanner is waiting for
	idx_t PendingBufferIdx() const {
		return pending_buffer_idx;
	}

	const idx_t scanner_idx;
	//! We use the max of idx_t to signify this is a line finder scanner.
	static constexpr idx_t LINE_FINDER_ID = NumericLimits<idx_t>::Maximum();

	//! Variable that manages buffer tracking
	shared_ptr<CSVBufferUsage> buffer_tracker;

private:
	//! Result of trying to move to the next buffer
	enum class MoveBufferResult : uint8_t {
		MOVED,        //! Advanced to the next buffer, stitching any value that straddled the edge
		NOT_MOVED,    //! The buffer still has bytes left, or the file has ended
		NOT_IN_MEMORY //! The next buffer is not in memory, the scanner suspended on its load
	};

	void Initialize() override;

	void FinalizeChunkProcess() override;

	//! Continues a chunk that suspended on a buffer load
	void ResumeParse();

	//! Function used to process values that go over the first buffer, extra allocation might be necessary
	void ProcessOverBufferValue();

	void ProcessExtraRow();
	//! Function used to move from one buffer to the other, if necessary
	MoveBufferResult TryMoveToNextBuffer();
	//! Move to the next buffer and processes the value straddling the buffer edge
	void FinishMoveToNextBuffer(shared_ptr<CSVBufferHandle> next_buffer);
	//! Tail of the boundary chunk finalization, runs after the move past the boundary end
	void FinishBoundaryScan(bool moved);
	//! Processes buffers until the chunk is full or the file ends
	void ProcessRemainingBuffers();

	//! -------- Functions used to figure out where lines start ---------!//
	//! Main function, sets the correct start
	void SetStart();
	//! From a given initial state, it skips until we reach the until_state
	bool SkipUntilState(CSVState initial_state, CSVState until_state, CSVIterator &current_iterator,
	                    bool &quoted) const;
	//! If the current row we found is valid
	bool IsRowValid(CSVIterator &current_iterator) const;
	ValidRowInfo TryRow(CSVState state, idx_t start_pos, idx_t end_pos) const;
	bool FirstValueEndsOnQuote(CSVIterator iterator) const;

	StringValueResult result;
	vector<LogicalType> types;
	//! True Position where this scanner started scanning(i.e., after figuring out where the first line starts)
	idx_t start_pos;
	//! Pointer to the previous buffer handle, necessary for over-buffer values
	shared_ptr<CSVBufferHandle> previous_buffer_handle;
	//! Strict state machine is basically a state machine with rfc 4180 set to true, used to figure out a new line.
	shared_ptr<CSVStateMachine> state_machine_strict;
	//! Whether this scanner is allowed to pause for I/O. sniffing/line-finding/skipping can't pause.
	const bool can_suspend;
	//! Set when the scanner suspends on a buffer not in memory
	bool suspended = false;
	//! The buffer a suspension waits on, a scanner suspends at most once per buffer
	idx_t pending_buffer_idx = 0;
};

} // namespace duckdb
