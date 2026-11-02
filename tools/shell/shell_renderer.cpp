#include "shell_renderer.hpp"

#include "shell_state.hpp"
#include "duckdb/common/box_renderer.hpp"
#include "shell_highlight.hpp"
#include "duckdb/logging/log_storage.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/hash.hpp"
#include <stdexcept>
#include <cstring>

namespace duckdb_shell {

//===--------------------------------------------------------------------===//
// Print Stream
//===--------------------------------------------------------------------===//
PrintStream::PrintStream(ShellState &state) : state(state) {
}

void PrintStream::RenderAlignedValue(const char *str, idx_t str_len, idx_t width, TextAlignment alignment) {
	idx_t w = width;
	idx_t n = state.RenderLength(str, str_len);
	idx_t space_count = w < n ? 0 : w - n;
	idx_t lspace;
	idx_t rspace;
	if (alignment == TextAlignment::LEFT) {
		lspace = 0;
		rspace = space_count;
	} else if (alignment == TextAlignment::RIGHT) {
		lspace = space_count;
		rspace = 0;
	} else {
		lspace = space_count / 2;
		rspace = (space_count + 1) / 2;
	}
	if (lspace > 0) {
		Print(string(lspace, ' '));
	}
	Print(duckdb::string_t(str, str_len));
	if (rspace > 0) {
		Print(string(rspace, ' '));
	}
}

void PrintStream::RenderAlignedValue(const string &str, idx_t width, TextAlignment alignment) {
	RenderAlignedValue(str.c_str(), str.size(), width, alignment);
}
void PrintStream::RenderAlignedValue(duckdb::string_t str, idx_t width, TextAlignment alignment) {
	RenderAlignedValue(str.GetData(), str.GetSize(), width, alignment);
}
void PrintStream::PrintDashes(idx_t N) {
	Print(string(N, '-'));
}

void PrintStream::PrintSQL(const string &sql) {
	if (!SupportsHighlight()) {
		// the output stream does not support highlighting (e.g. width measuring / redirected output)
		Print(sql);
		return;
	}
	string highlighted = sql;
	// HighlightSQL is a no-op when highlighting is disabled or output is not a console
	state.HighlightSQL(highlighted);
	Print(highlighted);
}

// A PrintStream that captures all output into a string instead of writing it out.
// Used to build a complete SQL statement so it can be highlighted as a whole before printing.
struct StringPrintStream : public PrintStream {
	explicit StringPrintStream(ShellState &state) : PrintStream(state) {
	}

	void Print(const string &str) override {
		result += str;
	}
	void Print(duckdb::string_t str) override {
		result.append(str.GetData(), str.GetSize());
	}
	void Print(const char *str) override {
		result += str;
	}
	void SetBinaryMode() override {
	}
	void SetTextMode() override {
	}
	bool SupportsHighlight() override {
		return false;
	}

	string result;
};

void PrintStream::OutputQuotedIdentifier(const string &str) {
	Print(StringUtil::Format("%s", SQLIdentifier(str)));
}

void PrintStream::OutputQuotedString(const string &str) {
	string result = "'";
	for (auto c : str) {
		if (c == '\'') {
			result += c;
		}
		result += c;
	}
	result += "'";
	Print(result);
}

//===--------------------------------------------------------------------===//
// ShellRenderer
//===--------------------------------------------------------------------===//
ShellRenderer::ShellRenderer(ShellState &state)
    : state(state), show_header(state.showHeader), col_sep(state.colSeparator), row_sep(state.rowSeparator) {
}

void ShellRenderer::RenderHeader(PrintStream &out, ResultMetadata &result) {
}

void ShellRenderer::RenderRow(PrintStream &out, ResultMetadata &result, RowData &row) {
}

void ShellRenderer::RenderFooter(PrintStream &out, ResultMetadata &result) {
}

const char *ShellRenderer::NullValue() {
	return state.nullValue.c_str();
}

string ShellRenderer::ConvertValue(const char *value, idx_t str_len) {
	return string(value, str_len);
}

void ShellRenderer::Analyze(RenderingQueryResult &result) {
}

// RenderingResultIterator is an iterator that EITHER
// (1) iterates over a query result
// (2) iterates over a materialized result
struct RenderingResultIterator {
public:
	explicit RenderingResultIterator(optional_ptr<RenderingQueryResult> result_p) : result(result_p) {
		if (!result) {
			return;
		}
		auto nCol = result->ColumnCount();
		row_data.data.resize(nCol, string());
		row_data.is_null.resize(nCol, false);
		row_data.row_index = 0;
		chunk_idx = 0;
		row_in_chunk = 0;

		if (!result->exhausted_result && Finished()) {
			result->TryConvertChunk();
		}
		AssignData();
	}

	optional_ptr<RenderingQueryResult> result;
	RowData row_data;
	idx_t chunk_idx;
	idx_t row_in_chunk;

public:
	bool Finished() {
		return row_data.row_index >= result->loaded_row_count;
	}

	void AssignData() {
		if (Finished()) {
			result = nullptr;
			return;
		}
		// read from the materialized rows
		auto &chunk = *result->chunks[chunk_idx];
		idx_t column_count = chunk.ColumnCount();
		if (row_data.data.size() != column_count) {
			row_data.data.resize(column_count);
		}
		for (idx_t c = 0; c < column_count; c++) {
			auto &vector = chunk.data[c];
			if (duckdb::FlatVector::IsNull(vector, row_in_chunk)) {
				row_data.data[c] = duckdb::string_t(result->renderer.NullValue());
				row_data.is_null[c] = true;
			} else {
				row_data.data[c] = duckdb::FlatVector::GetData<duckdb::string_t>(chunk.data[c])[row_in_chunk];
				row_data.is_null[c] = false;
			}
		}
	}

	void Next() {
		if (!result) {
			return;
		}
		// iterate to next position
		row_data.row_index++;
		row_in_chunk++;
		auto &chunk = *result->chunks[chunk_idx];
		if (row_in_chunk >= chunk.size()) {
			row_in_chunk = 0;
			chunk_idx++;
		}
		if (!result->exhausted_result && Finished()) {
			// convert the next chunk (if we have any)
			result->TryConvertChunk();
		}
		// read data from this position
		AssignData();
	}

	RenderingResultIterator &operator++() {
		Next();
		return *this;
	}
	bool operator!=(const RenderingResultIterator &other) const {
		return result != other.result;
	}
	RowData &operator*() {
		return row_data;
	}
};

RenderingQueryResult::RenderingQueryResult(duckdb::QueryResult &result, ShellRenderer &renderer)
    : result(result), renderer(renderer), metadata(result) {
}

RenderingQueryResult::RenderingQueryResult(duckdb::QueryResultStream<> &stream, ShellRenderer &renderer)
    : stream(stream), renderer(renderer), metadata(stream) {
}

RenderingResultIterator RenderingQueryResult::begin() {
	return RenderingResultIterator(*this);
}

RenderingResultIterator RenderingQueryResult::end() {
	return RenderingResultIterator(nullptr);
}

SuccessState ShellState::RenderQueryResult(ShellRenderer &renderer, duckdb::QueryResultStream<> &stream,
                                           PagerMode pager_overwrite) {
	RenderingQueryResult render_result(stream, renderer);
	return RenderPreparedResult(renderer, render_result, pager_overwrite);
}

SuccessState ShellState::RenderQueryResult(ShellRenderer &renderer, duckdb::QueryResult &query_result,
                                           PagerMode pager_overwrite) {
	RenderingQueryResult render_result(query_result, renderer);
	return RenderPreparedResult(renderer, render_result, pager_overwrite);
}

SuccessState ShellState::RenderPreparedResult(ShellRenderer &renderer, RenderingQueryResult &render_result,
                                              PagerMode pager_overwrite) {
	renderer.Analyze(render_result);
	if (seenInterrupt) {
		return SuccessState::FAILURE;
	}

	// check if we need to use the pager for the rendering
	unique_ptr<PagerState> pager_setup;
	if (pager_overwrite == PagerMode::PAGER_AUTOMATIC) {
		if (ShouldUsePager(renderer, render_result)) {
			pager_overwrite = PagerMode::PAGER_ON;
		}
	}
	if (pager_overwrite == PagerMode::PAGER_ON) {
		pager_setup = SetupPager();
	}
	// render the query result
	PrintStream print_stream(*this);
	return renderer.RenderQueryResult(print_stream, *this, render_result);
}

SuccessState ShellRenderer::RenderQueryResult(PrintStream &out, ShellState &state, RenderingQueryResult &result) {
	RenderHeader(out, result.metadata);
	for (auto &row_data : result) {
		if (state.seenInterrupt) {
			state.PrintF("Interrupt\n");
			return SuccessState::FAILURE;
		}
		RenderRow(out, result.metadata, row_data);
	}
	if (result.HasError()) {
		// the query failed while streaming - the caller reports it, no footer
		return SuccessState::FAILURE;
	}
	RenderFooter(out, result.metadata);
	return SuccessState::SUCCESS;
}

bool RenderingQueryResult::HasError() const {
	return stream && stream->HasError();
}

//===--------------------------------------------------------------------===//
// Result Metadata
//===--------------------------------------------------------------------===//
string GetTypeName(duckdb::LogicalType &type) {
	switch (type.id()) {
	case duckdb::LogicalTypeId::BOOLEAN:
		return "BOOLEAN";
	case duckdb::LogicalTypeId::TINYINT:
		return "TINYINT";
	case duckdb::LogicalTypeId::SMALLINT:
		return "SMALLINT";
	case duckdb::LogicalTypeId::INTEGER:
		return "INTEGER";
	case duckdb::LogicalTypeId::BIGINT:
		return "BIGINT";
	case duckdb::LogicalTypeId::FLOAT:
		return "FLOAT";
	case duckdb::LogicalTypeId::DOUBLE:
		return "DOUBLE";
	case duckdb::LogicalTypeId::DECIMAL:
		return "DECIMAL";
	case duckdb::LogicalTypeId::DATE:
		return "DATE";
	case duckdb::LogicalTypeId::TIME:
	case duckdb::LogicalTypeId::TIME_NS:
		return "TIME";
	case duckdb::LogicalTypeId::TIMESTAMP:
	case duckdb::LogicalTypeId::TIMESTAMP_NS:
	case duckdb::LogicalTypeId::TIMESTAMP_MS:
	case duckdb::LogicalTypeId::TIMESTAMP_SEC:
		return "TIMESTAMP";
	case duckdb::LogicalTypeId::VARCHAR:
		return "VARCHAR";
	case duckdb::LogicalTypeId::LIST:
		return "LIST";
	case duckdb::LogicalTypeId::MAP:
		return "MAP";
	case duckdb::LogicalTypeId::STRUCT:
		return "STRUCT";
	case duckdb::LogicalTypeId::BLOB:
		return "BLOB";
	default:
		return "NULL";
	}
}

ResultMetadata::ResultMetadata(const vector<duckdb::Identifier> &names,
                               const vector<duckdb::LogicalType> &result_types) {
	// initialize the result and the column names
	idx_t nCol = names.size();
	column_names.reserve(nCol);
	types.reserve(nCol);
	for (idx_t c = 0; c < nCol; c++) {
		column_names.push_back(names[c].GetIdentifierName());
		types.push_back(result_types[c]);
		type_names.push_back(GetTypeName(types.back()));
	}
}

ResultMetadata::ResultMetadata(duckdb::QueryResult &result) : ResultMetadata(result.GetNames(), result.GetTypes()) {
}

ResultMetadata::ResultMetadata(duckdb::QueryResultStream<> &stream)
    : ResultMetadata(stream.GetNames(), stream.GetTypes()) {
}

//===--------------------------------------------------------------------===//
// Column Renderers
//===--------------------------------------------------------------------===//
ColumnRenderer::ColumnRenderer(ShellState &state) : ShellRenderer(state) {
}

unique_ptr<duckdb::DataChunk> ShellRenderer::ConvertChunk(duckdb::DataChunk &chunk) {
	// cast to the varchar chunk
	auto varchar_chunk = make_uniq<duckdb::DataChunk>();
	vector<duckdb::LogicalType> all_varchar;
	for (idx_t c = 0; c < chunk.ColumnCount(); c++) {
		all_varchar.emplace_back(duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
	}
	varchar_chunk->Initialize(duckdb::Allocator::DefaultAllocator(), all_varchar);

	for (idx_t c = 0; c < chunk.ColumnCount(); c++) {
		duckdb::VectorOperations::Cast(*state.conn->context, chunk.data[c], varchar_chunk->data[c], chunk.size());
	}
	varchar_chunk->SetChildCardinality(chunk.size());
	varchar_chunk->Flatten();
	return varchar_chunk;
}

void RenderingQueryResult::Drain(ShellState &state) {
	while (!exhausted_result && !state.seenInterrupt) {
		auto chunk = stream ? stream->Fetch() : result->Fetch();
		if (!chunk) {
			exhausted_result = true;
		}
	}
}

bool RenderingQueryResult::TryConvertChunk() {
	if (exhausted_result) {
		return false;
	}
	auto chunk = stream ? stream->Fetch() : result->Fetch();
	if (!chunk) {
		exhausted_result = true;
		return false;
	}
	auto varchar_chunk = renderer.ConvertChunk(*chunk);
	if (renderer.HasConvertValue()) {
		for (idx_t c = 0; c < ColumnCount(); c++) {
			auto &str_vec = varchar_chunk->data[c];
			auto strings = duckdb::FlatVector::GetDataMutable<duckdb::string_t>(str_vec);
			for (idx_t r = 0; r < varchar_chunk->size(); r++) {
				if (duckdb::FlatVector::IsNull(str_vec, r)) {
					continue;
				}
				auto val = strings[r].GetData();
				auto str_len = strings[r].GetSize();
				if (!renderer.ShouldConvertValue(val, str_len)) {
					continue;
				}
				strings[r] = duckdb::StringVector::AddStringOrBlob(str_vec, renderer.ConvertValue(val, str_len));
			}
		}
	}
	loaded_row_count += chunk->size();
	chunks.push_back(std::move(varchar_chunk));
	return true;
}

void ColumnRenderer::Analyze(RenderingQueryResult &result) {
	auto &state = ShellState::Get();
	for (auto &column_name : result.metadata.column_names) {
		column_name = ConvertValue(column_name.c_str(), column_name.size());
	}
	// materialize the query result
	while (result.TryConvertChunk()) {
		if (state.seenInterrupt) {
			state.PrintF("Interrupt\n");
			return;
		}
	}

	// compute the column widths
	for (idx_t c = 0; c < result.ColumnCount(); c++) {
		int w = c < state.colWidth.size() ? state.colWidth[c] : 0;
		if (w < 0) {
			right_align.push_back(true);
			w = -w;
		} else {
			right_align.push_back(false);
		}
		idx_t render_width = static_cast<idx_t>(w);
		idx_t column_name_width = state.RenderLength(result.metadata.column_names[c]);
		if (column_name_width > render_width) {
			render_width = column_name_width;
		}
		column_width.push_back(render_width);
	}
	for (idx_t column_idx = 0; column_idx < result.ColumnCount(); column_idx++) {
		for (auto &chunk : result.chunks) {
			auto &vector = chunk->data[column_idx];
			auto string_data = duckdb::FlatVector::GetData<duckdb::string_t>(vector);
			for (idx_t r = 0; r < chunk->size(); r++) {
				idx_t width;
				if (duckdb::FlatVector::IsNull(vector, r)) {
					width = state.RenderLength(state.nullValue);
				} else {
					width = state.RenderLength(string_data[r]);
				}
				if (width > column_width[column_idx]) {
					column_width[column_idx] = width;
				}
			}
		}
	}
}

bool ColumnRenderer::ShouldUsePager(RenderingQueryResult &result, PagerMode global_mode) {
	if (global_mode == PagerMode::PAGER_ON) {
		return true;
	}
	if (result.loaded_row_count >= state.pager_min_rows) {
		// rows exceed min rows
		return true;
	}
	idx_t max_render_width = state.GetMaxRenderWidth();
	idx_t total_width = 0;
	for (auto w : column_width) {
		total_width += w + 2;
	}
	if (total_width > max_render_width) {
		return true;
	}
	return false;
}

void ColumnRenderer::RenderRow(PrintStream &out, ResultMetadata &result, RowData &row) {
	auto colSep = GetColumnSeparator();
	auto rowSep = GetRowSeparator();
	auto row_start = GetRowStart();
	if (row_start) {
		out.Print(row_start);
	}
	for (idx_t c = 0; c < row.data.size(); c++) {
		if (c > 0) {
			out.Print(colSep);
		}
		TextAlignment alignment = right_align[c] ? TextAlignment::RIGHT : TextAlignment::LEFT;
		out.RenderAlignedValue(row.data[c], column_width[c], alignment);
	}
	out.Print(rowSep);
}

class ModeColumnRenderer : public ColumnRenderer {
public:
	explicit ModeColumnRenderer(ShellState &state) : ColumnRenderer(state) {
	}

	void RenderHeader(PrintStream &out, ResultMetadata &result) override {
		if (!show_header) {
			return;
		}
		auto column_count = result.ColumnCount();
		for (idx_t c = 0; c < column_count; c++) {
			TextAlignment alignment = right_align[c] ? TextAlignment::RIGHT : TextAlignment::LEFT;
			out.RenderAlignedValue(result.column_names[c], column_width[c], alignment);
			out.Print(c == column_count - 1 ? "\n" : "  ");
		}
		for (idx_t i = 0; i < column_count; i++) {
			out.PrintDashes(column_width[i]);
			out.Print(i == column_count - 1 ? "\n" : "  ");
		}
	}

	const char *GetColumnSeparator() override {
		return "  ";
	}
	const char *GetRowSeparator() override {
		return "\n";
	}
};

class ModeTableRenderer : public ColumnRenderer {
public:
	explicit ModeTableRenderer(ShellState &state) : ColumnRenderer(state) {
	}

	void RenderHeader(PrintStream &out, ResultMetadata &result) override {
		auto column_count = result.ColumnCount();
		PrintRowSeparator(out, column_count, "+", column_width);
		out.Print("| ");
		for (idx_t c = 0; c < column_count; c++) {
			out.RenderAlignedValue(result.column_names[c], column_width[c]);
			out.Print(c == column_count - 1 ? " |\n" : " | ");
		}
		PrintRowSeparator(out, column_count, "+", column_width);
	}

	void RenderFooter(PrintStream &out, ResultMetadata &result) override {
		auto column_count = result.ColumnCount();
		PrintRowSeparator(out, column_count, "+", column_width);
	}

	void PrintRowSeparator(PrintStream &out, idx_t nArg, const char *zSep, const vector<idx_t> &actualWidth) {
		if (nArg > 0) {
			out.Print(zSep);
			out.PrintDashes(actualWidth[0] + 2);
			for (idx_t i = 1; i < nArg; i++) {
				out.Print(zSep);
				out.PrintDashes(actualWidth[i] + 2);
			}
			out.Print(zSep);
		}
		out.Print("\n");
	}

	const char *GetColumnSeparator() override {
		return " | ";
	}
	const char *GetRowSeparator() override {
		return " |\n";
	}
	const char *GetRowStart() override {
		return "| ";
	}
};

class ModeMarkdownRenderer : public ColumnRenderer {
public:
	//! In agent mode the table is rendered for a machine reader: no alignment padding, the type in the header cell,
	//! only the first max_rows rows / max_bytes bytes and at most max_cell_width characters per cell, and a footer
	//! with the row count and a hash of the whole result when they are not obvious from the rows (see
	//! DetectAgentMode). The result is streamed: after the cap the rows are only counted and hashed, and a query that
	//! keeps producing rows beyond LOOKAHEAD_ROWS is stopped, with the count reported as a lower bound.
	explicit ModeMarkdownRenderer(ShellState &state)
	    : ColumnRenderer(state), compact(state.agent_mode_active), max_rows(state.max_rows), max_bytes(state.max_bytes),
	      max_cell_width(state.max_cell_width) {
	}

	bool RequireMaterializedResult() const override {
		// the compact table needs no column widths, so it can stream (and stop a query it will not render)
		return !compact;
	}

	void Analyze(RenderingQueryResult &result) override {
		if (!compact) {
			ColumnRenderer::Analyze(result);
			row_count = result.loaded_row_count;
			return;
		}
		for (auto &column_name : result.metadata.column_names) {
			column_name = ConvertValue(column_name.c_str(), column_name.size());
		}
		column_width.assign(result.ColumnCount(), 0);
		right_align.assign(result.ColumnCount(), false);
	}

	void RemoveRenderLimits() override {
		max_rows = (size_t)-1;
		max_bytes = 0;
		max_cell_width = 0;
	}

	SuccessState RenderQueryResult(PrintStream &out, ShellState &state, RenderingQueryResult &result) override {
		if (!compact) {
			return ColumnRenderer::RenderQueryResult(out, state, result);
		}
		RenderHeader(out, result.metadata);
		// the rows are buffered up to the cap (at most max_rows / max_bytes, which is small) so that a result that
		// fits is rendered whole, and one that does not is rendered as a sample of its first and last rows
		vector<string> head_lines;
		idx_t head_bytes = 0;
		vector<string> tail_ring;
		idx_t tail_next = 0;
		const idx_t tail_max = duckdb::MinValue<idx_t>(SAMPLE_ROWS, max_rows / 2);
		auto push_tail = [&](string line) {
			if (tail_max == 0) {
				return;
			}
			if (tail_ring.size() < tail_max) {
				tail_ring.push_back(std::move(line));
			} else {
				tail_ring[tail_next] = std::move(line);
			}
			tail_next = (tail_next + 1) % tail_max;
		};
		bool capped = false;
		for (auto &row_data : result) {
			if (state.seenInterrupt) {
				state.PrintF("Interrupt\n");
				return SuccessState::FAILURE;
			}
			auto line = FormatRow(row_data);
			if (!capped) {
				// the first row always counts: a budget below one row is no reason to show nothing
				bool within_rows = head_lines.size() < max_rows;
				bool within_bytes = max_bytes == 0 || head_bytes < max_bytes || head_lines.empty();
				if (within_rows && within_bytes) {
					head_bytes += line.size();
					head_lines.push_back(line);
					push_tail(std::move(line));
					continue;
				}
				capped = true;
				cap_hint = within_rows ? ".maxbytes 0 for all" : ".maxrows -1 for all";
			}
			push_tail(std::move(line));
			// beyond the cap the rows are only counted and hashed - up to a point: the count is then a lower bound
			if (row_data.row_index >= head_lines.size() + LOOKAHEAD_ROWS) {
				break;
			}
		}
		if (result.HasError()) {
			return SuccessState::FAILURE;
		}
		row_count = result.loaded_row_count;
		complete = result.exhausted_result;
		if (!capped) {
			for (auto &line : head_lines) {
				out.Print(line);
			}
			head_rows = head_lines.size();
			RenderFooter(out, result.metadata);
			return SuccessState::SUCCESS;
		}
		// a sample: the first rows within half the byte budget, then the last rows within the other half
		const idx_t head_max = duckdb::MinValue<idx_t>(SAMPLE_ROWS, max_rows - tail_max);
		const idx_t half_bytes = max_bytes / 2;
		idx_t bytes = 0;
		for (auto &line : head_lines) {
			if (head_rows >= head_max || (max_bytes > 0 && head_rows > 0 && bytes + line.size() > half_bytes)) {
				break;
			}
			out.Print(line);
			bytes += line.size();
			head_rows++;
		}
		if (complete && !tail_ring.empty()) {
			// the ring holds the last rows in arrival order starting at tail_next; take from the end, but never a row
			// that is part of the head
			vector<string> tail_lines;
			bytes = 0;
			for (idx_t i = 0; i < tail_ring.size() && head_rows + tail_lines.size() < row_count; i++) {
				auto &line = tail_ring[(tail_next + tail_ring.size() - 1 - i) % tail_ring.size()];
				if (max_bytes > 0 && bytes + line.size() > half_bytes) {
					break;
				}
				bytes += line.size();
				tail_lines.push_back(line);
			}
			tail_rows = tail_lines.size();
			if (tail_rows > 0) {
				auto omitted = row_count - head_rows - tail_rows;
				if (omitted > 0) {
					// a one-cell row is a valid table row (the missing cells read as empty) and stands out
					out.Print(
					    StringUtil::Format("%s… %llu rows omitted …%s", GetRowStart(), omitted, GetRowSeparator()));
				}
				for (idx_t i = tail_rows; i > 0; i--) {
					out.Print(tail_lines[i - 1]);
				}
			}
		}
		RenderFooter(out, result.metadata);
		return SuccessState::SUCCESS;
	}

	void RenderFooter(PrintStream &out, ResultMetadata &result) override {
		if (!compact) {
			return;
		}
		if (!complete) {
			out.Print(StringUtil::Format("first %llu of at least %llu rows (query stopped early; %s)\n", head_rows,
			                             row_count, cap_hint));
		} else if (head_rows + tail_rows < row_count) {
			if (tail_rows > 0) {
				out.Print(StringUtil::Format("first %llu and last %llu of %llu rows (%s), hash %016llx\n", head_rows,
				                             tail_rows, row_count, cap_hint, result_hash));
			} else {
				out.Print(StringUtil::Format("first %llu of %llu rows (%s), hash %016llx\n", head_rows, row_count,
				                             cap_hint, result_hash));
			}
		} else if (row_count == 0 || row_count >= 10) {
			out.Print(StringUtil::Format("%llu rows, hash %016llx\n", row_count, result_hash));
		}
	}

	//! Fingerprint the whole result, not only the rendered rows: two results with the same footer are the same
	//! result. The sum over the row hashes is independent of the row order (which is arbitrary without ORDER BY)
	//! while it keeps counting duplicate rows.
	void HashChunk(duckdb::DataChunk &chunk) {
		for (idx_t r = 0; r < chunk.size(); r++) {
			duckdb::hash_t row_hash = 0;
			for (idx_t c = 0; c < chunk.ColumnCount(); c++) {
				auto &vector = chunk.data[c];
				duckdb::hash_t value_hash;
				if (duckdb::FlatVector::IsNull(vector, r)) {
					value_hash = NULL_HASH;
				} else {
					auto &str = duckdb::FlatVector::GetData<duckdb::string_t>(vector)[r];
					value_hash = duckdb::Hash(str.GetData(), str.GetSize());
				}
				// positional mix, so that swapping two columns changes the hash
				row_hash = duckdb::Hash<uint64_t>(row_hash ^ value_hash);
			}
			result_hash += row_hash;
		}
	}

	string FormatRow(RowData &row) {
		string line = GetRowStart();
		for (idx_t c = 0; c < row.data.size(); c++) {
			if (c > 0) {
				line += GetColumnSeparator();
			}
			line += row.data[c].GetString();
		}
		line += GetRowSeparator();
		return line;
	}

	bool HasConvertValue() override {
		return true;
	}

	//! Whether a cell exceeds max_cell_width; sets `cut` to the byte offset of the first character past it
	bool ExceedsCellWidth(const char *value, idx_t str_len, idx_t &cut, idx_t &char_count) {
		if (!compact || max_cell_width == 0) {
			return false;
		}
		char_count = 0;
		cut = str_len;
		for (idx_t i = 0; i < str_len; i++) {
			if ((static_cast<unsigned char>(value[i]) & 0xC0) == 0x80) {
				continue; // UTF-8 continuation byte
			}
			if (char_count == max_cell_width) {
				cut = i;
			}
			char_count++;
		}
		return char_count > max_cell_width;
	}

	bool ShouldConvertValue(const char *value, idx_t str_len) override {
		idx_t cut, char_count;
		if (ExceedsCellWidth(value, str_len, cut, char_count)) {
			return true;
		}
		for (idx_t i = 0; i < str_len; i++) {
			if (value[i] == '|') {
				return true;
			}
		}
		return false;
	}

	string ConvertValue(const char *value, idx_t str_len) override {
		// cut an over-long cell, but say so: a reader cannot tell a cut value from a short one otherwise
		string marker;
		idx_t cut, char_count;
		if (ExceedsCellWidth(value, str_len, cut, char_count)) {
			marker = StringUtil::Format("…(+%llu chars)", char_count - max_cell_width);
			str_len = cut;
		}
		// when rendering for markdown we need to escape pipes
		string result;
		for (idx_t idx = 0; idx < str_len; idx++) {
			auto c = value[idx];
			if (c == '|') {
				result += '\\';
			}
			result += c;
		}
		return result + marker;
	}

	unique_ptr<duckdb::DataChunk> ConvertChunk(duckdb::DataChunk &chunk) override {
		auto varchar_chunk = ColumnRenderer::ConvertChunk(chunk);
		if (compact) {
			// fingerprint the values before they are cut or escaped for rendering
			HashChunk(*varchar_chunk);
		}
		return varchar_chunk;
	}

	void RenderHeader(PrintStream &out, ResultMetadata &result) override {
		auto column_count = result.ColumnCount();
		out.Print(GetRowStart());
		for (idx_t c = 0; c < column_count; c++) {
			if (c > 0) {
				out.Print(GetColumnSeparator());
			}
			if (compact) {
				auto &type = result.types[c];
				// the NULL type renders quoted (to not read as the keyword) - the header is not SQL
				auto type_name = type.id() == duckdb::LogicalTypeId::SQLNULL ? "NULL" : type.ToString();
				out.Print(result.column_names[c] + ":" + type_name);
			} else {
				out.RenderAlignedValue(result.column_names[c], column_width[c]);
			}
		}
		out.Print(GetRowSeparator());
		PrintMarkdownSeparator(out, column_count, "|", result.types, column_width);
	}

	void PrintMarkdownSeparator(PrintStream &out, idx_t nArg, const char *zSep,
	                            const vector<duckdb::LogicalType> &colTypes, const vector<idx_t> &actualWidth) {
		if (nArg > 0) {
			for (idx_t i = 0; i < nArg; i++) {
				out.Print(zSep);
				if (colTypes[i].IsNumeric() && !compact) {
					// right-align numerics in tables
					out.PrintDashes(actualWidth[i] + 1);
					out.Print(":");
				} else {
					out.PrintDashes(duckdb::MaxValue<idx_t>(actualWidth[i] + 2, 3));
				}
			}
			out.Print(zSep);
		}
		out.Print("\n");
	}

	const char *GetColumnSeparator() override {
		return " | ";
	}
	const char *GetRowSeparator() override {
		return " |\n";
	}
	const char *GetRowStart() override {
		return "| ";
	}

	bool ShouldUsePager(RenderingQueryResult &result, PagerMode global_mode) override {
		// this mode never uses the pager in automatic mode
		return global_mode == PagerMode::PAGER_ON;
	}

private:
	//! Hash of a NULL cell, distinct from any string's hash
	static constexpr duckdb::hash_t NULL_HASH = 0x9E3779B97F4A7C15ULL;
	//! How many rows past the cap are still read (counted and hashed) before the query is stopped
	static constexpr idx_t LOOKAHEAD_ROWS = 100000;
	//! How many first and last rows a result that exceeds the cap is sampled to (each side)
	static constexpr idx_t SAMPLE_ROWS = 20;

	bool compact;
	idx_t max_rows;
	idx_t max_bytes;
	idx_t max_cell_width;
	idx_t row_count = 0;
	idx_t head_rows = 0;
	idx_t tail_rows = 0;
	bool complete = true;
	const char *cap_hint = "";
	duckdb::hash_t result_hash = 0;
};

/*
** UTF8 box-drawing characters.  Imagine box lines like this:
**
**           1
**           |
**       4 --+-- 2
**           |
**           3
**
** Each box characters has between 2 and 4 of the lines leading from
** the center.  The characters are here identified by the numbers of
** their corresponding lines.
*/
#define BOX_24   "\342\224\200" /* U+2500 --- */
#define BOX_13   "\342\224\202" /* U+2502  |  */
#define BOX_23   "\342\224\214" /* U+250c  ,- */
#define BOX_34   "\342\224\220" /* U+2510 -,  */
#define BOX_12   "\342\224\224" /* U+2514  '- */
#define BOX_14   "\342\224\230" /* U+2518 -'  */
#define BOX_123  "\342\224\234" /* U+251c  |- */
#define BOX_134  "\342\224\244" /* U+2524 -|  */
#define BOX_234  "\342\224\254" /* U+252c -,- */
#define BOX_124  "\342\224\264" /* U+2534 -'- */
#define BOX_1234 "\342\224\274" /* U+253c -|- */

class ModeBoxRenderer : public ColumnRenderer {
public:
	explicit ModeBoxRenderer(ShellState &state) : ColumnRenderer(state) {
	}

	bool HasConvertValue() override {
		return true;
	}

	bool ShouldConvertValue(const char *value, idx_t str_len) override {
		for (idx_t i = 0; i < str_len; i++) {
			if (value[i] == '\n') {
				return true;
			}
		}
		return false;
	}

	string ConvertValue(const char *value, idx_t str_len) override {
		string result;
		for (idx_t i = 0; i < str_len; i++) {
			auto c = value[i];
			if (c == '\n') {
				result += "\\n";
			} else {
				result += c;
			}
		}
		return result;
	}

	void RenderHeader(PrintStream &out, ResultMetadata &result) override {
		auto column_count = result.ColumnCount();
		PrintBoxRowSeparator(out, column_count, BOX_23, BOX_234, BOX_34, column_width);
		out.Print(BOX_13 " ");
		for (idx_t c = 0; c < column_count; c++) {
			out.RenderAlignedValue(result.column_names[c], column_width[c]);
			out.Print(c == column_count - 1 ? " " BOX_13 "\n" : " " BOX_13 " ");
		}
		PrintBoxRowSeparator(out, column_count, BOX_123, BOX_1234, BOX_134, column_width);
	}

	void RenderFooter(PrintStream &out, ResultMetadata &result) override {
		auto column_count = result.ColumnCount();
		PrintBoxRowSeparator(out, column_count, BOX_12, BOX_124, BOX_14, column_width);
	}

	const char *GetColumnSeparator() override {
		return " " BOX_13 " ";
	}
	const char *GetRowSeparator() override {
		return " " BOX_13 "\n";
	}
	const char *GetRowStart() override {
		return BOX_13 " ";
	}

private:
	/* Draw horizontal line N characters long using unicode box
	** characters
	*/
	void PrintBoxLine(PrintStream &out, idx_t N) {
		string box_line;
		for (idx_t i = 0; i < N; i++) {
			box_line += BOX_24;
		}
		out.Print(box_line);
	}

	/*
	** Draw a horizontal separator for a RenderMode::Box table.
	*/
	void PrintBoxRowSeparator(PrintStream &out, idx_t nArg, const char *zSep1, const char *zSep2, const char *zSep3,
	                          const vector<idx_t> &actualWidth) {
		idx_t i;
		if (nArg > 0) {
			out.Print(zSep1);
			PrintBoxLine(out, actualWidth[0] + 2);
			for (i = 1; i < nArg; i++) {
				out.Print(zSep2);
				PrintBoxLine(out, actualWidth[i] + 2);
			}
			out.Print(zSep3);
		}
		out.Print("\n");
	}
};

class ModeLatexRenderer : public ColumnRenderer {
public:
	explicit ModeLatexRenderer(ShellState &state) : ColumnRenderer(state) {
	}

	void RenderHeader(PrintStream &out, ResultMetadata &result) override {
		auto column_count = result.ColumnCount();

		out.Print("\\begin{tabular}{|");
		for (idx_t i = 0; i < column_count; i++) {
			if (state.ColumnTypeIsInteger(result.type_names[i].c_str())) {
				out.Print("r");
			} else {
				out.Print("l");
			}
		}
		out.Print("|}\n");
		out.Print("\\hline\n");
		for (idx_t c = 0; c < column_count; c++) {
			out.RenderAlignedValue(result.column_names[c], column_width[c]);
			out.Print(c == column_count - 1 ? GetRowSeparator() : GetColumnSeparator());
		}
		out.Print("\\hline\n");
	}

	void RenderFooter(PrintStream &out, ResultMetadata &) override {
		out.Print("\\hline\n");
		out.Print("\\end{tabular}\n");
	}

	const char *GetColumnSeparator() override {
		return " & ";
	}
	const char *GetRowSeparator() override {
		return " \\\\\n";
	}

	bool ShouldUsePager(RenderingQueryResult &result, PagerMode global_mode) override {
		// this mode never uses the pager in automatic mode
		return global_mode == PagerMode::PAGER_ON;
	}
};

//===--------------------------------------------------------------------===//
// Row Renderers
//===--------------------------------------------------------------------===//
RowRenderer::RowRenderer(ShellState &state) : ShellRenderer(state) {
}

void RowRenderer::RenderHeader(PrintStream &out, ResultMetadata &result) {
}

struct WidthMeasuringStream : public PrintStream {
public:
	explicit WidthMeasuringStream(ShellState &state) : PrintStream(state) {
	}

	void ProcessCharacter(char c) {
		if (c == '\r') {
			return;
		}
		if (c == '\n') {
			// newline - compute the render width of the current line and reset
			auto render_width = state.RenderLength(output);
			if (render_width > max_width) {
				max_width = render_width;
			}
			output = string();
			return;
		}
		output += c;
	}

	void MeasureText(const char *str, idx_t str_len) {
		for (idx_t i = 0; i < str_len; i++) {
			auto c = str[i];
			ProcessCharacter(c);
		}
	}
	void Print(const string &str) override {
		MeasureText(str.c_str(), str.size());
	}
	void Print(duckdb::string_t str) override {
		MeasureText(str.GetData(), str.GetSize());
	}
	void Print(const char *str) override {
		for (idx_t i = 0; str[i]; i++) {
			auto c = str[i];
			ProcessCharacter(c);
		}
	}
	void SetBinaryMode() override {
	}
	void SetTextMode() override {
	}
	bool SupportsHighlight() override {
		return false;
	}

	string output;
	idx_t max_width = 0;
};

bool RowRenderer::ShouldUsePager(RenderingQueryResult &result, PagerMode global_mode) {
	if (global_mode == PagerMode::PAGER_ON) {
		return true;
	}
	// fetch data until we have either fetched more than the pager min, or we have exhausted the data
	while (result.loaded_row_count < state.pager_min_rows && result.TryConvertChunk()) {
	}
	if (result.loaded_row_count >= state.pager_min_rows) {
		// rows exceed min rows
		return true;
	}
	D_ASSERT(result.exhausted_result);
	// figure out how wide the result would be when rendered
	WidthMeasuringStream stream(state);
	// make a copy of the result to avoid consuming it
	RenderQueryResult(stream, state, result);

	idx_t max_render_width = state.GetMaxRenderWidth();
	if (stream.max_width > max_render_width) {
		return true;
	}
	return false;
}

class ModeLineRenderer : public RowRenderer {
public:
	explicit ModeLineRenderer(ShellState &state) : RowRenderer(state) {
	}

	void RenderHeader(PrintStream &out, ResultMetadata &result) override {
		auto &col_names = result.column_names;
		// determine the render width by going over the column names
		header_width = 5;
		for (idx_t i = 0; i < col_names.size(); i++) {
			auto len = col_names[i].size();
			if (len > header_width) {
				header_width = len;
			}
		}
	}

	void RenderRow(PrintStream &out, ResultMetadata &result, RowData &row) override {
		if (row.row_index > 0) {
			out.Print(state.rowSeparator);
		}
		auto &data = row.data;
		auto &col_names = result.column_names;
		for (idx_t i = 0; i < data.size(); i++) {
			idx_t space_count = header_width - col_names[i].size();
			if (space_count > 0) {
				out.Print(string(space_count, ' '));
			}
			out.Print(col_names[i]);
			out.Print(" = ");
			out.Print(data[i]);
			out.Print(state.rowSeparator);
		}
	}

	idx_t header_width = 0;
};

class ModeExplainRenderer : public RowRenderer {
public:
	explicit ModeExplainRenderer(ShellState &state) : RowRenderer(state) {
	}

	void RenderRow(PrintStream &out, ResultMetadata &result, RowData &row) override {
		auto &data = row.data;
		if (data.size() != 2) {
			return;
		}
		auto plan = data[1].GetString();
		if (data[0].GetString() == "sql") {
			out.PrintSQL(plan);
		} else {
			out.Print(data[1]);
		}
		if (!plan.empty() && plan.back() != '\n') {
			out.Print("\n");
		}
		// after EXPLAIN ANALYZE (interactive), point users at the full (expanded) tree when the pretty tree folded
		// low-impact operators, and always at the ".web" command which opens the profile in a browser.
		// (skip it for EXPLAIN ANALYZE (FORMAT WEB), whose result value is empty and which already opened the profile)
		if (out.SupportsHighlight() && state.stdin_is_interactive && state.stdout_is_console &&
		    !data[1].GetString().empty() && data[0].GetString() == "analyzed_plan") {
			string hint = state.last_explain_hid_content
			                  ? "\ntype .last to show the full tree, .web to open it in a browser\n"
			                  : "\ntype .web to open the query profile in a browser\n";
			ShellHighlight highlight(state);
			highlight.PrintText(hint, PrintOutput::STDOUT, HighlightElementType::FOOTER);
		}
	}

	bool RequireMaterializedResult() const override {
		return true;
	}
	bool ShouldUsePager(RenderingQueryResult &result, PagerMode global_mode) override {
		if (global_mode == PagerMode::PAGER_ON) {
			return true;
		}
		// load the (materialized) result so we can measure the rendered tree height
		while (result.TryConvertChunk()) {
		}
		bool sql_result = false;
		WidthMeasuringStream sql_width(state);
		idx_t row_count = 0;
		for (auto &row : result) {
			if (row.is_null[1]) {
				continue;
			}
			auto plan = row.data[1].GetString();
			for (auto c : plan) {
				if (c == '\n') {
					row_count++;
				}
			}
			if (row.data[0].GetString() == "sql") {
				sql_result = true;
				RenderRow(sql_width, result.metadata, row);
				if (!plan.empty() && plan.back() != '\n') {
					row_count++;
				}
			}
		}
		auto render_width = sql_result ? sql_width.max_width : state.last_explain_width;
		// page the tree when it does not fit on the screen (too tall, or too wide for the terminal)
		return state.ShouldUsePagerForSize(row_count, render_width);
	}
};

class ModeListRenderer : public RowRenderer {
public:
	explicit ModeListRenderer(ShellState &state) : RowRenderer(state) {
	}

	void RenderHeader(PrintStream &out, ResultMetadata &result) override {
		if (!show_header) {
			return;
		}
		auto &col_names = result.column_names;
		for (idx_t i = 0; i < col_names.size(); i++) {
			if (i > 0) {
				out.Print(col_sep);
			}
			out.Print(col_names[i]);
		}
		out.Print(row_sep);
	}

	void RenderRow(PrintStream &out, ResultMetadata &result, RowData &row) override {
		auto &data = row.data;
		for (idx_t i = 0; i < data.size(); i++) {
			if (i > 0) {
				out.Print(col_sep);
			}
			out.Print(data[i]);
		}
		out.Print(row_sep);
	}
};

class ModeHtmlRenderer : public RowRenderer {
public:
	explicit ModeHtmlRenderer(ShellState &state) : RowRenderer(state) {
	}

	void RenderHeader(PrintStream &out, ResultMetadata &result) override {
		if (!show_header) {
			return;
		}
		auto &col_names = result.column_names;
		out.Print("<tr>");
		for (idx_t i = 0; i < col_names.size(); i++) {
			out.Print("<th>");
			OutputHTMLString(out, col_names[i]);
			out.Print("</th>\n");
		}
		out.Print("</tr>\n");
	}

	void RenderRow(PrintStream &out, ResultMetadata &result, RowData &row) override {
		auto &data = row.data;
		out.Print("<tr>");
		for (idx_t i = 0; i < data.size(); i++) {
			out.Print("<td>");
			OutputHTMLString(out, data[i].GetString());
			out.Print("</td>\n");
		}
		out.Print("</tr>\n");
	}

	/*
	** Output the given string with characters that are special to
	** HTML escaped.
	*/
	void OutputHTMLString(PrintStream &out, const string &z) {
		string escaped;
		for (auto c : z) {
			switch (c) {
			case '<':
				escaped += "&lt;";
				break;
			case '&':
				escaped += "&amp;";
				break;
			case '>':
				escaped += "&gt;";
				break;
			case '\"':
				escaped += "&quot;";
				break;
			case '\'':
				escaped += "&#39;";
				break;
			default:
				escaped += c;
			}
		}
		out.Print(escaped);
	}

	bool ShouldUsePager(RenderingQueryResult &result, PagerMode global_mode) override {
		// this mode never uses the pager in automatic mode
		return global_mode == PagerMode::PAGER_ON;
	}
};

class ModeTclRenderer : public RowRenderer {
public:
	explicit ModeTclRenderer(ShellState &state) : RowRenderer(state) {
	}

	void RenderHeader(PrintStream &out, ResultMetadata &result) override {
		if (!show_header) {
			return;
		}
		auto &col_names = result.column_names;
		for (idx_t i = 0; i < col_names.size(); i++) {
			if (i > 0) {
				out.Print(col_sep);
			}
			out.Print(state.EscapeCString(col_names[i]));
		}
		out.Print(row_sep);
	}

	void RenderRow(PrintStream &out, ResultMetadata &result, RowData &row) override {
		auto &data = row.data;
		for (idx_t i = 0; i < data.size(); i++) {
			if (i > 0) {
				out.Print(col_sep);
			}
			out.Print(state.EscapeCString(data[i].GetString()));
		}
		out.Print(row_sep);
	}
};

class ModeCsvRenderer : public RowRenderer {
public:
	explicit ModeCsvRenderer(ShellState &state) : RowRenderer(state) {
	}

	void RenderHeader(PrintStream &out, ResultMetadata &result) override {
		if (!show_header) {
			return;
		}
		out.SetBinaryMode();
		auto &col_names = result.column_names;
		for (idx_t i = 0; i < col_names.size(); i++) {
			if (i > 0) {
				out.Print(state.colSeparator);
			}
			PrintEscapedCSV(out, col_names[i].c_str(), col_names[i].size());
		}
		out.Print(row_sep);
		out.SetTextMode();
	}

	void RenderRow(PrintStream &out, ResultMetadata &result, RowData &row) override {
		out.SetBinaryMode();
		auto &data = row.data;
		for (idx_t i = 0; i < data.size(); i++) {
			if (i > 0) {
				out.Print(state.colSeparator);
			}
			PrintEscapedCSV(out, data[i].GetData(), data[i].GetSize());
		}
		out.Print(row_sep);
		out.SetTextMode();
	}

	bool CharacterNeedsQuote(unsigned char c) {
		if (c <= 31) {
			// non-printable - needs quote
			return true;
		}
		if (c == '"' || c == '\'') {
			return true;
		}
		if (c >= 127) {
			return true;
		}
		return false;
	}

	void PrintEscapedCSV(PrintStream &out, const char *str, idx_t str_len) {
		bool needs_quote = false;
		idx_t col_sep_size = state.colSeparator.size();
		for (idx_t idx = 0; idx < str_len; idx++) {
			if (CharacterNeedsQuote((unsigned char)str[idx])) {
				needs_quote = true;
				break;
			}
			if (idx + col_sep_size <= str_len) {
				if (memcmp((const void *)(str + idx), (const void *)state.colSeparator.c_str(), col_sep_size) == 0) {
					needs_quote = true;
					break;
				}
			}
		}
		if (!needs_quote) {
			out.Print(duckdb::string_t(str, str_len));
			return;
		}
		auto result = StringUtil::Format("%s", SQLIdentifier(string(str, str_len)));
		out.Print(result);
	}

	bool ShouldUsePager(RenderingQueryResult &result, PagerMode global_mode) override {
		// this mode never uses the pager in automatic mode
		return global_mode == PagerMode::PAGER_ON;
	}
};

class ModeAsciiRenderer : public RowRenderer {
public:
	explicit ModeAsciiRenderer(ShellState &state) : RowRenderer(state) {
		col_sep = "\n";
		row_sep = "\n";
	}

	void RenderHeader(PrintStream &out, ResultMetadata &result) override {
		if (!show_header) {
			return;
		}
		auto &col_names = result.column_names;
		for (idx_t i = 0; i < col_names.size(); i++) {
			if (i > 0) {
				out.Print(col_sep);
			}
			out.Print(col_names[i]);
		}
		out.Print(row_sep);
	}

	void RenderRow(PrintStream &out, ResultMetadata &result, RowData &row) override {
		auto &data = row.data;
		for (idx_t i = 0; i < data.size(); i++) {
			if (i > 0) {
				out.Print(col_sep);
			}
			out.Print(data[i]);
		}
		out.Print(row_sep);
	}

	bool ShouldUsePager(RenderingQueryResult &result, PagerMode global_mode) override {
		// this mode never uses the pager in automatic mode
		return global_mode == PagerMode::PAGER_ON;
	}
};

class ModeQuoteRenderer : public RowRenderer {
public:
	explicit ModeQuoteRenderer(ShellState &state) : RowRenderer(state) {
	}

	void RenderHeader(PrintStream &out, ResultMetadata &result) override {
		if (!show_header) {
			return;
		}
		auto &col_names = result.column_names;
		for (idx_t i = 0; i < col_names.size(); i++) {
			if (i > 0) {
				out.Print(col_sep);
			}
			out.OutputQuotedString(col_names[i]);
		}
		out.Print(row_sep);
	}

	void RenderRow(PrintStream &out, ResultMetadata &result, RowData &row) override {
		auto &data = row.data;
		auto &types = result.types;
		auto &is_null = row.is_null;
		for (idx_t i = 0; i < data.size(); i++) {
			if (i > 0) {
				out.Print(col_sep);
			}
			if (types[i].IsNumeric() || is_null[i] || types[i].id() == duckdb::LogicalTypeId::BOOLEAN) {
				out.Print(data[i]);
			} else {
				out.OutputQuotedString(data[i].GetString());
			}
		}
		out.Print(row_sep);
	}

	const char *NullValue() override {
		return "NULL";
	}
};

class ModeJsonRenderer : public RowRenderer {
public:
	explicit ModeJsonRenderer(ShellState &state, bool json_array) : RowRenderer(state), json_array(json_array) {
	}

	void RenderHeader(PrintStream &out, ResultMetadata &result) override {
		if (json_array) {
			// wrap all JSON objects in an array
			out.Print("[");
		}
	}

	bool RequiresQuotes(const duckdb::LogicalType &type) {
		// Booleans are cast to VARCHAR ("true"/"false") in ConvertChunk; emit them as JSON booleans, not strings.
		if (type.id() == duckdb::LogicalTypeId::BOOLEAN) {
			return false;
		}
		if (!type.IsNumeric()) {
			return true;
		}
		switch (type.id()) {
		case duckdb::LogicalTypeId::UBIGINT:
		case duckdb::LogicalTypeId::HUGEINT:
		case duckdb::LogicalTypeId::UHUGEINT:
		case duckdb::LogicalTypeId::DECIMAL:
			return true;
		default:
			return false;
		}
	}

	void RenderRow(PrintStream &out, ResultMetadata &result, RowData &row) override {
		if (row.row_index > 0) {
			if (json_array) {
				// wrap all JSON objects in an array
				out.Print(",");
			}
			out.Print("\n");
		}
		out.Print("{");
		auto &data = row.data;
		auto &is_null = row.is_null;
		auto &types = result.types;
		auto &col_names = result.column_names;
		for (idx_t i = 0; i < col_names.size(); i++) {
			if (i > 0) {
				out.Print(",");
			}
			out.Print(EscapeJSONString(col_names[i]));
			out.Print(":");
			if (is_null[i]) {
				out.Print("null");
			} else if (types[i].IsNested() || types[i].IsJSONType()) {
				// nested types have already been converted to the correct JSON format
				out.Print(data[i]);
			} else if (!RequiresQuotes(types[i])) {
				// for numeric types emit the data directly without quoting
				out.Print(data[i]);
			} else {
				// not a nested type - escape here
				out.Print(EscapeJSONString(data[i].GetString()));
			}
		}
		out.Print("}");
	}

	string EscapeJSONString(const string &str) {
		string result = "\"";
		for (auto c_ : str) {
			auto c = (unsigned char)c_;
			if (c == '\\' || c == '"') {
				// escape \ and "
				result += "\\";
				result += c_;
			} else if (c <= 0x1f) {
				result += "\\";
				if (c == '\b') {
					result += "b";
				} else if (c == '\f') {
					result += "f";
				} else if (c == '\n') {
					result += "n";
				} else if (c == '\r') {
					result += "r";
				} else if (c == '\t') {
					result += "t";
				} else {
					char buf[10];
					snprintf(buf, 10, "u%04x", c);
					result += buf;
				}
			} else {
				result += c_;
			}
		}
		result += "\"";
		return result;
	}

	static bool RequiresJSONCast(const duckdb::LogicalType &type) {
		if (type.IsNested()) {
			// cast nested types to JSON to preserve structure
			return true;
		}
		if (type.IsFloating()) {
			// cast floating point numbers to JSON to correctly deal with inf / nan
			return true;
		}
		return false;
	}

	unique_ptr<duckdb::DataChunk> ConvertChunk(duckdb::DataChunk &chunk) override {
		// convert all nested types to JSON directly
		duckdb::DataChunk json_chunk;
		vector<duckdb::LogicalType> all_json;
		for (idx_t c = 0; c < chunk.ColumnCount(); c++) {
			if (!RequiresJSONCast(chunk.data[c].GetType())) {
				all_json.emplace_back(duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
			} else {
				all_json.emplace_back(duckdb::LogicalType::JSON());
			}
		}
		json_chunk.Initialize(duckdb::Allocator::DefaultAllocator(), all_json);

		for (idx_t c = 0; c < chunk.ColumnCount(); c++) {
			duckdb::VectorOperations::Cast(*state.conn->context, chunk.data[c], json_chunk.data[c], chunk.size());
		}
		json_chunk.SetChildCardinality(chunk.size());
		json_chunk.Flatten();
		// now convert the JSON chunk to VARCHAR
		return ShellRenderer::ConvertChunk(json_chunk);
	}

	void RenderFooter(PrintStream &out, ResultMetadata &result) override {
		if (json_array) {
			out.Print("]\n");
		} else {
			out.Print("\n");
		}
	}

	const char *NullValue() override {
		return "null";
	}

	bool ShouldUsePager(RenderingQueryResult &result, PagerMode global_mode) override {
		// this mode never uses the pager in automatic mode
		return global_mode == PagerMode::PAGER_ON;
	}

	bool json_array;
};

class ModeInsertRenderer : public RowRenderer {
public:
	explicit ModeInsertRenderer(ShellState &state) : RowRenderer(state) {
	}

	void RenderRow(PrintStream &out, ResultMetadata &result, RowData &row) override {
		auto &data = row.data;
		auto &types = result.types;
		auto &col_names = result.column_names;
		auto &is_null = row.is_null;

		// build the full INSERT statement into a buffer so it can be highlighted as a whole
		StringPrintStream buffer(state);
		buffer.Print("INSERT INTO ");
		buffer.Print(state.zDestTable);
		if (show_header) {
			buffer.Print("(");
			for (idx_t i = 0; i < col_names.size(); i++) {
				if (i > 0) {
					buffer.Print(",");
				}
				buffer.OutputQuotedIdentifier(col_names[i]);
			}
			buffer.Print(")");
		}
		for (idx_t i = 0; i < data.size(); i++) {
			buffer.Print(i > 0 ? "," : " VALUES(");
			if (is_null[i]) {
				buffer.Print("NULL");
			} else if (types[i].IsNumeric()) {
				buffer.Print(data[i]);
			} else if (state.ShellHasFlag(ShellFlags::SHFLG_Newlines)) {
				buffer.OutputQuotedString(data[i].GetString());
			} else {
				buffer.Print(EscapeNewlines(data[i].GetString()));
			}
		}
		buffer.Print(");\n");
		out.PrintSQL(buffer.result);
	}

	string EscapeNewlines(const string &str) {
		bool needs_concat = false;
		for (auto c : str) {
			if (c == '\n' || c == '\r' || c == '\0') {
				needs_concat = true;
				break;
			}
		}
		string res;
		if (needs_concat) {
			res = "concat('";
		} else {
			res = "'";
		}
		for (auto c : str) {
			switch (c) {
			case '\n':
			case '\r':
			case '\0':
				// newline - finish the current string literal and write the newline with a chr function
				res += "', chr(";
				switch (c) {
				case '\n':
					res += "10";
					break;
				case '\r':
					res += "13";
					break;
				case '\0':
					res += "0";
					break;
				}
				res += "), '";
				break;
			case '\'':
				// escape the quote
				res += "''";
				break;
			default:
				res += c;
				break;
			}
		}
		res += "'";
		if (needs_concat) {
			res += ")";
		}
		return res;
	}

	bool ShouldUsePager(RenderingQueryResult &result, PagerMode global_mode) override {
		// this mode never uses the pager in automatic mode
		return global_mode == PagerMode::PAGER_ON;
	}
};

class ModeSemiRenderer : public RowRenderer {
public:
	explicit ModeSemiRenderer(ShellState &state) : RowRenderer(state) {
	}

	void RenderRow(PrintStream &out, ResultMetadata &result, RowData &row) override {
		/* .schema and .fullschema output */
		out.PrintSQL(state.GetSchemaLine(row.data[0].GetString(), "\n"));
	}
};

//===--------------------------------------------------------------------===//
// DuckBox Renderer
//===--------------------------------------------------------------------===//
class DuckBoxRenderer : public duckdb::BaseResultRenderer {
public:
	DuckBoxRenderer(PrintStream &out, ShellState &state, bool highlight)
	    : out(out), shell_highlight(state), output(PrintOutput::STDOUT), highlight(highlight) {
	}

	void RenderLayout(const string &text) override {
		PrintText(text, HighlightElementType::LAYOUT);
	}

	void RenderColumnName(const string &text) override {
		PrintText(text, HighlightElementType::COLUMN_NAME);
	}

	void RenderType(const string &text) override {
		PrintText(text, HighlightElementType::COLUMN_TYPE);
	}

	void RenderValue(const string &text, const duckdb::LogicalType &type) override {
		if (type.IsNumeric()) {
			PrintText(text, HighlightElementType::NUMERIC_VALUE);
		} else if (type.IsTemporal()) {
			PrintText(text, HighlightElementType::TEMPORAL_VALUE);
		} else {
			PrintText(text, HighlightElementType::STRING_VALUE);
		}
	}

	void RenderStringLiteral(const string &text, const duckdb::LogicalType &type) override {
		PrintText(text, HighlightElementType::STRING_CONSTANT);
	}

	void RenderNull(const string &text, const duckdb::LogicalType &type) override {
		PrintText(text, HighlightElementType::NULL_VALUE);
	}

	void RenderFooter(const string &text) override {
		PrintText(text, HighlightElementType::FOOTER);
	}

	void PrintText(const string &text, HighlightElementType element_type) {
		if (highlight) {
			shell_highlight.PrintText(text, output, element_type);
		} else {
			out.Print(text);
		}
	}

private:
	PrintStream &out;
	ShellHighlight shell_highlight;
	PrintOutput output;
	bool highlight = true;
};

class ModeDuckBoxRenderer : public ShellRenderer {
public:
	explicit ModeDuckBoxRenderer(ShellState &state);

	void RemoveRenderLimits() override;
	void Analyze(RenderingQueryResult &result) override;
	SuccessState RenderQueryResult(PrintStream &out, ShellState &state, RenderingQueryResult &result) override;
	bool RequireMaterializedResult() const override {
		return true;
	}
	bool ShouldUsePager(RenderingQueryResult &result, PagerMode global_mode) override;

private:
	duckdb::BoxRendererConfig config;
	unique_ptr<duckdb::ClientBoxRendererContext> render_context;
	unique_ptr<duckdb::BoxRendererState> render_state;
	unique_ptr<duckdb::ColumnDataCollectionWrapper> wrapper;
	string error_str;
};

ModeDuckBoxRenderer::ModeDuckBoxRenderer(ShellState &state) : ShellRenderer(state) {
	config.max_rows = state.max_rows;
	config.max_width = state.max_width;
	if (state.max_analyze_rows == 0) {
		// if max_analyze_rows is set to 0 (auto) - set max_analyze_rows to 100K if we are rendering to the console
		if (state.stdout_is_console) {
			// if we are
			config.max_analyze_rows = 100000;
		}
	} else {
		config.max_analyze_rows = state.max_analyze_rows;
	}
	if (config.max_width == 0) {
		// if max_width is set to 0 (auto) - set it to infinite if we are writing to a file
		if (!state.outfile.empty() && state.outfile[0] != '|') {
			config.max_rows = (size_t)-1;
			config.max_width = (size_t)-1;
		}
		if (!state.stdout_is_console) {
			config.max_width = (size_t)-1;
		}
	}
	LargeNumberRendering large_rendering = state.large_number_rendering;
	if (large_rendering == LargeNumberRendering::DEFAULT) {
		large_rendering = state.stdout_is_console ? LargeNumberRendering::FOOTER : LargeNumberRendering::NONE;
	}
	config.null_value = state.nullValue;
	if (state.columns) {
		config.render_mode = duckdb::RenderMode::COLUMNS;
	}
	config.decimal_separator = state.decimal_separator;
	config.thousand_separator = state.thousand_separator;
	config.large_number_rendering = static_cast<duckdb::LargeNumberRendering>(static_cast<int>(large_rendering));
	if (state.pager_mode != PagerMode::PAGER_OFF) {
		config.hidden_rows_hint = "use .last to show entire result";
	}
}

void ModeDuckBoxRenderer::RemoveRenderLimits() {
	config.max_rows = (size_t)-1;
	config.max_width = (size_t)-1;
}

void ModeDuckBoxRenderer::Analyze(RenderingQueryResult &result) {
	duckdb::BoxRenderer renderer(config);
	auto &con = *state.conn;
	try {
		wrapper = make_uniq<duckdb::ColumnDataCollectionWrapper>(result.result->Collection());
		render_context = make_uniq<duckdb::ClientBoxRendererContext>(*con.context);
		render_state = renderer.Prepare(*render_context, result.metadata.column_names, *wrapper);
	} catch (std::exception &ex) {
		// store the error - throw on render
		error_str = ex.what();
	}
}

bool ModeDuckBoxRenderer::ShouldUsePager(RenderingQueryResult &result, PagerMode global_mode) {
	if (global_mode == PagerMode::PAGER_ON) {
		return true;
	}
	// in duckbox mode the output is automatically truncated to the terminal width if max_width = 0
	// if max_width is set - check the render width - we use the pager if it exceeds the max render width
	if (config.max_width != 0) {
		idx_t max_render_width = state.GetMaxRenderWidth();
		if (render_state->TotalRenderWidth() > max_render_width) {
			return true;
		}
	}
	// in duckbox mode the output is truncated to max_rows
	// if this is larger than pager_min_rows - we actually check the row count of the result
	if (config.max_rows >= state.pager_min_rows) {
		// show the pager if the row count exceeds the min rows
		if (result.result->RowCount() >= state.pager_min_rows) {
			return true;
		}
	}
	return false;
}

SuccessState ModeDuckBoxRenderer::RenderQueryResult(PrintStream &out, ShellState &state, RenderingQueryResult &result) {
	DuckBoxRenderer result_renderer(out, state, state.HighlightResults() && out.SupportsHighlight());
	try {
		if (!error_str.empty()) {
			throw std::runtime_error(error_str);
		}
		render_state->Render(result_renderer);
		return SuccessState::SUCCESS;
	} catch (std::exception &ex) {
		string error_str = duckdb::ErrorData(ex).Message() + "\n";
		result_renderer.RenderLayout(error_str);
		return SuccessState::FAILURE;
	}
}

//===--------------------------------------------------------------------===//
// Describe Renderer
//===--------------------------------------------------------------------===//
class ModeDescribeRenderer : public ShellRenderer {
public:
	explicit ModeDescribeRenderer(ShellState &state) : ShellRenderer(state) {
	}

	SuccessState RenderQueryResult(PrintStream &out, ShellState &state, RenderingQueryResult &result) override;
	bool RequireMaterializedResult() const override {
		return true;
	}
	bool ShouldUsePager(RenderingQueryResult &result, PagerMode global_mode) override {
		// pager gets handled separately (inside RenderTableMetadata)
		return false;
	}
};

SuccessState ModeDescribeRenderer::RenderQueryResult(PrintStream &out, ShellState &state, RenderingQueryResult &res) {
	vector<ShellTableInfo> result;
	ShellTableInfo table;
	table.table_name = state.describe_table_name;
	for (auto &row : *res.result) {
		ShellColumnInfo column;
		column.column_name = row.GetValue<string>(0);
		column.column_type = row.GetValue<string>(1);
		if (!row.IsNull(2)) {
			column.is_not_null = row.GetValue<string>(2) == "NO";
		}
		if (!row.IsNull(3)) {
			column.is_primary_key = row.GetValue<string>(3) == "PRI";
			column.is_unique = row.GetValue<string>(3) == "UNI";
		}
		if (!row.IsNull(4)) {
			column.default_value = row.GetValue<string>(4);
		}
		table.columns.push_back(std::move(column));
	}
	result.push_back(std::move(table));
	state.RenderTableMetadata(result);
	return SuccessState::SUCCESS;
}

//===--------------------------------------------------------------------===//
// Trash Renderer
//===--------------------------------------------------------------------===//
class ModeTrashRenderer : public ShellRenderer {
public:
	explicit ModeTrashRenderer(ShellState &state) : ShellRenderer(state) {
	}

	SuccessState RenderQueryResult(PrintStream &out, ShellState &state, RenderingQueryResult &result) override {
		// the rows are discarded, but the query still runs to completion
		result.Drain(state);
		if (state.seenInterrupt) {
			state.PrintF("Interrupt\n");
			return SuccessState::FAILURE;
		}
		return SuccessState::SUCCESS;
	}
	bool RequireMaterializedResult() const override {
		// mode trash discards the result
		return false;
	}
	bool ShouldUsePager(RenderingQueryResult &result, PagerMode global_mode) override {
		// mode trash never uses the pager
		return false;
	}
};

//===--------------------------------------------------------------------===//
// Get Renderer
//===--------------------------------------------------------------------===//
unique_ptr<ShellRenderer> ShellState::GetRenderer() {
	return GetRenderer(cMode);
}

unique_ptr<ShellRenderer> ShellState::GetRenderer(RenderMode mode) {
	switch (mode) {
	case RenderMode::LINE:
		return make_uniq<ModeLineRenderer>(*this);
	case RenderMode::EXPLAIN:
		return make_uniq<ModeExplainRenderer>(*this);
	case RenderMode::LIST:
		return make_uniq<ModeListRenderer>(*this);
	case RenderMode::HTML:
		return make_uniq<ModeHtmlRenderer>(*this);
	case RenderMode::TCL:
		return make_uniq<ModeTclRenderer>(*this);
	case RenderMode::CSV:
		return make_uniq<ModeCsvRenderer>(*this);
	case RenderMode::ASCII:
		return make_uniq<ModeAsciiRenderer>(*this);
	case RenderMode::QUOTE:
		return make_uniq<ModeQuoteRenderer>(*this);
	case RenderMode::JSON:
		return make_uniq<ModeJsonRenderer>(*this, true);
	case RenderMode::JSONLINES:
		return make_uniq<ModeJsonRenderer>(*this, false);
	case RenderMode::INSERT:
		return make_uniq<ModeInsertRenderer>(*this);
	case RenderMode::SEMI:
		return make_uniq<ModeSemiRenderer>(*this);
	case RenderMode::COLUMN:
		return make_uniq<ModeColumnRenderer>(*this);
	case RenderMode::TABLE:
		return make_uniq<ModeTableRenderer>(*this);
	case RenderMode::MARKDOWN:
		return make_uniq<ModeMarkdownRenderer>(*this);
	case RenderMode::BOX:
		return make_uniq<ModeBoxRenderer>(*this);
	case RenderMode::LATEX:
		return make_uniq<ModeLatexRenderer>(*this);
	case RenderMode::DUCKBOX:
		return make_uniq<ModeDuckBoxRenderer>(*this);
	case RenderMode::DESCRIBE:
		return make_uniq<ModeDescribeRenderer>(*this);
	case RenderMode::TRASH:
		return make_uniq<ModeTrashRenderer>(*this);
	default:
		throw std::runtime_error("Unsupported mode for GetRenderer");
	}
}

//===--------------------------------------------------------------------===//
// Shell Logging Storage
//===--------------------------------------------------------------------===//

namespace {

//! Column the compact log line pads the left-aligned "LEVEL:type" prefix to, so the elapsed aligns
constexpr duckdb::idx_t PRETTY_PREFIX_WIDTH = 20;

//! Width of the right-aligned elapsed field (widest form is "59'59.99" / "01h02'12")
constexpr duckdb::idx_t ELAPSED_WIDTH = 8;

//! Formats an elapsed duration (micros since CLI launch) with scale-adaptive precision, right-aligned
//! to ELAPSED_WIDTH so the message column lines up:
//!   < 1 min:  "12.12"     (seconds, 2 decimals)
//!   < 1 hour: "2'12.12"   (minutes'seconds, 2 decimals)
//!   >= 1 hour:"01h02'12"  (hours h minutes' seconds)
//! Rounds at the microsecond level BEFORE decomposing, so a carry rolls the unit over and we never
//! render an impossible "60.00" / "1'60.00" at a unit boundary.
string FormatElapsed(int64_t micros) {
	if (micros < 0) {
		micros = 0; // clock skew guard: never render a negative elapsed
	}
	static constexpr int64_t MICROS_PER_SEC = 1000000;
	static constexpr int64_t MICROS_PER_MIN = 60 * MICROS_PER_SEC;
	static constexpr int64_t MICROS_PER_HOUR = 60 * MICROS_PER_MIN;

	// Round to the precision we will display (picked from the raw magnitude), then choose the branch
	// from the ROUNDED value so a boundary carry promotes seconds->minutes / minutes->hours.
	int64_t rounded;
	if (micros < MICROS_PER_HOUR) {
		rounded = (micros + 5000) / 10000 * 10000; // nearest 10 ms (2 decimals of a second)
	} else {
		rounded = (micros + MICROS_PER_SEC / 2) / MICROS_PER_SEC * MICROS_PER_SEC; // nearest second
	}

	string str;
	if (rounded < MICROS_PER_MIN) {
		str = duckdb::StringUtil::Format("%.2f", double(rounded) / double(MICROS_PER_SEC));
	} else if (rounded < MICROS_PER_HOUR) {
		auto minutes = static_cast<int32_t>(rounded / MICROS_PER_MIN);
		auto seconds = double(rounded % MICROS_PER_MIN) / double(MICROS_PER_SEC);
		str = duckdb::StringUtil::Format("%d'%05.2f", minutes, seconds);
	} else {
		auto total_seconds = rounded / MICROS_PER_SEC;
		auto hours = static_cast<int32_t>(total_seconds / 3600);
		auto minutes = static_cast<int32_t>((total_seconds % 3600) / 60);
		auto seconds = static_cast<int32_t>(total_seconds % 60);
		str = duckdb::StringUtil::Format("%02dh%02d'%02d", hours, minutes, seconds);
	}
	// Right-align within the fixed field so successive lines' elapsed values align on the right
	if (str.size() < ELAPSED_WIDTH) {
		str.insert(str.begin(), ELAPSED_WIDTH - str.size(), ' ');
	}
	return str;
}

//! Renders a log message on a single line: collapses every run of whitespace (newlines, tabs,
//! spaces) to one space and drops other control chars. Keeps one entry on one line (QueryLog
//! messages are raw, often multi-line SQL) and prevents a payload from injecting escape sequences.
string SanitizeLogMessage(const string &message) {
	string result;
	result.reserve(message.size());
	bool prev_space = false;
	for (char c : message) {
		auto uc = static_cast<unsigned char>(c);
		const bool is_whitespace = uc < 0x20 || uc == 0x7f || c == ' ';
		if (is_whitespace) {
			if (!prev_space && !result.empty()) {
				result += ' ';
			}
			prev_space = true;
			continue;
		}
		result += c;
		prev_space = false;
	}
	if (!result.empty() && result.back() == ' ') {
		result.pop_back();
	}
	return result;
}

} // namespace

ShellLogStorage::ShellLogStorage(ShellState &state) : shell_highlight(state), start_time(duckdb::TimePoint::Tick()) {
	// Elapsed time on compact log lines is measured from here (CLI launch / db open)
}

void ShellLogStorage::WriteLogEntry(duckdb::timestamp_t, duckdb::LogLevel level, const string &log_type,
                                    const string &log_message, const duckdb::RegisteredLoggingContext &context) {
	duckdb::lock_guard<duckdb::mutex> l(lock);

	// Warnings/errors keep the original loud, multi-line representation; lower-severity logs
	// (INFO/DEBUG/TRACE) get a compact single-line form.
	const bool loud = level == duckdb::LogLevel::LOG_WARNING || level == duckdb::LogLevel::LOG_ERROR ||
	                  level == duckdb::LogLevel::LOG_FATAL;

	HighlightElementType element_type;
	switch (level) {
	case duckdb::LogLevel::LOG_TRACE:
		element_type = HighlightElementType::LOG_TRACE;
		break;
	case duckdb::LogLevel::LOG_DEBUG:
		element_type = HighlightElementType::LOG_DEBUG;
		break;
	case duckdb::LogLevel::LOG_INFO:
		element_type = HighlightElementType::LOG_INFO;
		break;
	case duckdb::LogLevel::LOG_WARNING:
		element_type = HighlightElementType::LOG_WARNING;
		break;
	case duckdb::LogLevel::LOG_ERROR:
	case duckdb::LogLevel::LOG_FATAL:
		element_type = HighlightElementType::ERROR_TOKEN;
		break;
	default:
		throw std::runtime_error("Unsupported log level for WriteLogEntry");
	}

	const auto log_level = duckdb::EnumUtil::ToString(level);

	if (loud) {
		// De-duplicate repeated WARNINGS so they don't spam the shell; errors/fatals are always shown
		if (level == duckdb::LogLevel::LOG_WARNING) {
			auto log_id = duckdb::StringUtil::CIHash(log_message);
			if (printed_logs.find(log_id) != printed_logs.end()) {
				return;
			}
			printed_logs.emplace(log_id);
		}

		shell_highlight.PrintText(log_level + ":\n", PrintOutput::STDOUT, element_type);
		shell_highlight.PrintText(log_message + "\n\n", PrintOutput::STDOUT, element_type);
		return;
	}

	// Compact single-line form: "LEVEL:type   <elapsed>  <message>". Elapsed is measured from CLI
	// launch. No de-duplication - every logged statement is shown.
	// Colored prefix (the configurable LOG_* palette), then the rest plain
	const string prefix = log_level + ":" + log_type;
	shell_highlight.PrintText(prefix, PrintOutput::STDOUT, element_type);

	string rest;
	if (prefix.size() < PRETTY_PREFIX_WIDTH) {
		rest.append(PRETTY_PREFIX_WIDTH - prefix.size(), ' ');
	} else {
		rest += "  ";
	}
	rest += FormatElapsed(start_time.ElapsedMicros());
	rest += "  ";
	rest += SanitizeLogMessage(log_message);
	rest += "\n";
	shell_highlight.PrintText(rest, PrintOutput::STDOUT, PrintColor::STANDARD, PrintIntensity::STANDARD);
}

} // namespace duckdb_shell
