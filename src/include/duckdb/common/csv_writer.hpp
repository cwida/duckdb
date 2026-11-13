//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/csv_writer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_reader_options.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/common/query_context.hpp"

namespace duckdb {
class AsyncFileWriter;
class MemoryStream;

enum class CSVNewLineMode {
	//! Newlines are written before writing out a new csv line. Ensures we don't write an empty line at the end of
	//! a csv file. Uses CSVWriterState::written_anything to keep track of when to insert newlines
	WRITE_BEFORE = 0,
	//! Newlines are written after every line. This is cleanest in stdout, where lines are expected to end with a
	//! newline
	WRITE_AFTER = 1
};

struct CSVWriterOptions {
	CSVWriterOptions(const string &delim, const char &quote, const string &write_newline, char comment_char = '#');
	explicit CSVWriterOptions(CSVReaderOptions &options);

	//! The newline string to write
	string newline = "\n";
	//! The size of the CSV file (in bytes) that we buffer before we flush it to disk
	idx_t flush_size = 4096ULL * 8ULL;
	//! For each byte whether the CSV file requires quotes when containing the byte
	vector<bool> requires_quotes;
	//! How to write newlines
	CSVNewLineMode newline_writing_mode = CSVNewLineMode::WRITE_BEFORE;
};

struct CSVWriterState {
	CSVWriterState(ClientContext &context, idx_t flush_size);
	CSVWriterState(DatabaseInstance &db, idx_t flush_size);
	CSVWriterState();
	~CSVWriterState();

	void Reset() {
		stream->Rewind();
		written_anything = false;
	}

	idx_t flush_size;
	unique_ptr<MemoryStream> stream;
	bool written_anything = false;

	bool require_manual_flush = false;
};

class CSVWriter {
public:
	//! Create a CSVWriter that writes to a (non-owned) WriteStream
	CSVWriter(WriteStream &stream, vector<Identifier> name_list, bool shared = true);
	//! Create a CSVWriter with the full set of options that writes to a (non-owned) AsyncFileWriter
	CSVWriter(CSVReaderOptions &options, AsyncFileWriter &file_writer, bool shared = true);

	//! Create a CSVWriter that writes to a file
	CSVWriter(CSVReaderOptions &options, FileSystem &fs, const string &file_path, FileCompressionType compression,
	          QueryContext context = QueryContext(), bool shared = true);
	~CSVWriter();

	//! Writes header and prefix if necessary
	void Initialize(bool force = false);

	//! Writes the raw string directly into the output stream
	void WriteRawString(const string &data);
	//! Writes the header directly into the output stream
	void WriteHeader();
	//! Write the Raw String, using the local_state
	void WriteRawString(const string &prefix, CSVWriterState &local_state);
	//! Write a chunk of VARCHAR vectors to the CSV file (any casts are the responsibility of caller)
	void WriteChunk(DataChunk &input, CSVWriterState &local_state);
	//! (Non-shared only) variant of WriteChunk
	void WriteChunk(DataChunk &input);

	//! Flushes all data in the local write state
	void Flush(CSVWriterState &local_state);
	//! (Non-shared only) variant of Flush
	void Flush();

	//! Resets the state of the writer. Warning: the file_writer is not reset
	void Reset(optional_ptr<CSVWriterState> local_state);

	//! Closes the writer, optionally writes a postfix
	void Close();

	vector<unique_ptr<Expression>> string_casts;

	idx_t BytesWritten();

	//! BytesWritten + OriginalSize;
	idx_t FileSize();

	bool WrittenAnything() {
		return written_anything;
	}
	void SetWrittenAnything(bool val) {
		if (shared) {
			lock_guard<mutex> guard(lock);
			written_anything = val;
		} else {
			written_anything = val;
		}
	}

	CSVReaderOptions options;
	CSVWriterOptions writer_options;

protected:
	void FlushInternal(CSVWriterState &local_state);
	void ResetInternal(optional_ptr<CSVWriterState> local_state);

	//! If we've written any rows yet, allows us to prevent a trailing comma when writing JSON ARRAY
	bool written_anything = false;

	//! (optional) The owned file writer of this CSVWriter
	unique_ptr<BufferedFileWriter> file_writer;
	//! (optional) The non-owned async file writer of this CSVWriter
	optional_ptr<AsyncFileWriter> async_file_writer;

	//! The WriteStream to write the CSV data to
	WriteStream &write_stream;

	idx_t bytes_written = 0;

	bool should_initialize;

	mutex lock;
	bool shared;

	unique_ptr<CSVWriterState> global_write_state;

	static void WriteQuoteOrEscape(WriteStream &writer, char quote_or_escape);
	static bool RequiresQuotes(const char *str, idx_t len, const string &null_str, const vector<bool> &requires_quotes);
	static void WriteQuotedString(WriteStream &writer, const char *str, idx_t len, bool force_quote,
	                              const string &null_str, const vector<bool> &requires_quotes, char quote, char escape);
	static void WriteQuotedString(WriteStream &writer, const char *str, idx_t len, idx_t col_idx,
	                              CSVReaderOptions &options, CSVWriterOptions &writer_options);

	static void WriteChunk(DataChunk &input, MemoryStream &writer, CSVReaderOptions &options, bool &written_anything,
	                       CSVWriterOptions &writer_options);
	static void WriteHeader(MemoryStream &stream, CSVReaderOptions &options, CSVWriterOptions &writer_options);
};

} // namespace duckdb
