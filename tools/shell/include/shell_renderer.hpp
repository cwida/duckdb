//===----------------------------------------------------------------------===//
//                         DuckDB
//
// shell_renderer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/time_point.hpp"
#include "duckdb/logging/log_storage.hpp"
#include "shell_state.hpp"
#include "shell_highlight.hpp"

namespace duckdb_shell {
struct ShellState;
struct RenderingResultIterator;

struct ResultMetadata {
	explicit ResultMetadata(duckdb::QueryResult &result);
	explicit ResultMetadata(duckdb::QueryResultStream<> &stream);
	ResultMetadata(const vector<duckdb::Identifier> &names, const vector<duckdb::LogicalType> &result_types);

	vector<string> column_names;
	vector<duckdb::LogicalType> types;
	vector<string> type_names;

	idx_t ColumnCount() const {
		return column_names.size();
	}
};

struct RowData {
	vector<duckdb::string_t> data;
	vector<bool> is_null;
	idx_t row_index = 0;
};

struct RenderingQueryResult {
	RenderingQueryResult(duckdb::QueryResult &result, ShellRenderer &renderer);
	RenderingQueryResult(duckdb::QueryResultStream<> &stream, ShellRenderer &renderer);

	//! The retained result, or null when the rows come from a stream
	duckdb::optional_ptr<duckdb::QueryResult> result;
	//! The stream the rows come from, or null when they come from a retained result
	duckdb::optional_ptr<duckdb::QueryResultStream<>> stream;
	ShellRenderer &renderer;
	ResultMetadata metadata;
	vector<unique_ptr<duckdb::DataChunk>> chunks;
	bool exhausted_result = false;
	idx_t loaded_row_count = 0;

	idx_t ColumnCount() const {
		return metadata.ColumnCount();
	}
	bool TryConvertChunk();
	//! Runs the result to completion without rendering it. Stops early on an interrupt
	void Drain(ShellState &state);
	//! Whether the query failed while its rows were streamed: Fetch then returns null, like a clean end
	bool HasError() const;

public:
	RenderingResultIterator begin(); // NOLINT: match stl API
	RenderingResultIterator end();   // NOLINT: match stl API
};

enum class TextAlignment { CENTER, LEFT, RIGHT };

struct PrintStream {
public:
	explicit PrintStream(ShellState &state);
	virtual ~PrintStream() = default;

	virtual void Print(const string &str) {
		state.Print(str);
	}
	virtual void Print(duckdb::string_t str) {
		state.Print(str);
	}
	virtual void Print(const char *str) {
		state.Print(str);
	}
	virtual void SetBinaryMode() {
		state.SetBinaryMode();
	}
	virtual void SetTextMode() {
		state.SetTextMode();
	}
	virtual bool SupportsHighlight() {
		return true;
	}

	// Print a complete SQL statement, applying syntax highlighting when the output stream supports it
	void PrintSQL(const string &sql);

	void RenderAlignedValue(const string &str, idx_t width, TextAlignment alignment = TextAlignment::CENTER);
	void RenderAlignedValue(const char *str, idx_t str_len, idx_t width,
	                        TextAlignment alignment = TextAlignment::CENTER);
	void RenderAlignedValue(duckdb::string_t str, idx_t width, TextAlignment alignment = TextAlignment::CENTER);
	void PrintDashes(idx_t N);
	void OutputQuotedIdentifier(const string &str);
	void OutputQuotedString(const string &str);

public:
	ShellState &state;
};

class ShellRenderer {
public:
	explicit ShellRenderer(ShellState &state);
	virtual ~ShellRenderer() = default;

	ShellState &state;
	bool show_header;
	string col_sep;
	string row_sep;

public:
	virtual SuccessState RenderQueryResult(PrintStream &out, ShellState &state, RenderingQueryResult &result);
	virtual void RemoveRenderLimits() {
	}
	virtual void Analyze(RenderingQueryResult &result);
	virtual void RenderHeader(PrintStream &out, ResultMetadata &result);
	virtual void RenderRow(PrintStream &out, ResultMetadata &result, RowData &row);
	virtual void RenderFooter(PrintStream &out, ResultMetadata &result);
	virtual const char *NullValue();
	virtual bool RequireMaterializedResult() const = 0;
	virtual bool ShouldUsePager(RenderingQueryResult &result, PagerMode global_mode) = 0;
	virtual unique_ptr<duckdb::DataChunk> ConvertChunk(duckdb::DataChunk &chunk);
	virtual bool HasConvertValue() {
		return false;
	}
	virtual bool ShouldConvertValue(const char *value, idx_t str_len) {
		return false;
	}
	virtual string ConvertValue(const char *value, idx_t str_len);
};

class ColumnRenderer : public ShellRenderer {
public:
	explicit ColumnRenderer(ShellState &state);

	void Analyze(RenderingQueryResult &result) override;
	void RenderRow(PrintStream &out, ResultMetadata &result, RowData &row) override;

	virtual const char *GetColumnSeparator() = 0;
	virtual const char *GetRowSeparator() = 0;
	virtual const char *GetRowStart() {
		return nullptr;
	}
	bool RequireMaterializedResult() const override {
		return true;
	}
	bool ShouldUsePager(RenderingQueryResult &result, PagerMode global_mode) override;

protected:
	vector<idx_t> column_width;
	vector<bool> right_align;
};

class RowRenderer : public ShellRenderer {
public:
	explicit RowRenderer(ShellState &state);

public:
	void RenderHeader(PrintStream &out, ResultMetadata &result) override;
	bool RequireMaterializedResult() const override {
		return false;
	}
	bool ShouldUsePager(RenderingQueryResult &result, PagerMode global_mode) override;
};

class ShellLogStorage : public duckdb::LogStorage {
public:
	explicit ShellLogStorage(ShellState &state);

	~ShellLogStorage() override = default;

	const string GetStorageName() override {
		return "ShellLogStorage";
	}

protected:
	void WriteLogEntry(duckdb::timestamp_t timestamp, duckdb::LogLevel level, const string &log_type,
	                   const string &log_message, const duckdb::RegisteredLoggingContext &context) override;
	void WriteLogEntries(duckdb::DataChunk &chunk, const duckdb::RegisteredLoggingContext &context) override {};
	void FlushAll() override {};
	void Flush(duckdb::LoggingTargetTable table) override {};
	bool IsEnabled(duckdb::LoggingTargetTable table) override {
		return true;
	};

private:
	ShellHighlight shell_highlight;

	// Warnings/errors that have already been printed, to avoid spamming the shell (loud logs only)
	duckdb::unordered_set<uint64_t> printed_logs;

	// Monotonic zero point for elapsed time in compact log lines.
	duckdb::TimePoint start_time;

	// lock to ensure thread safety of the printed_logs set and the elapsed-time state
	mutable duckdb::mutex lock;
};

} // namespace duckdb_shell
