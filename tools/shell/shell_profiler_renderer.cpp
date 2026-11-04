#include "shell_highlight.hpp"
#include "shell_state.hpp"

#include "duckdb/common/tree_renderer.hpp"
#include "duckdb/common/tree_renderer/text_tree_renderer.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/profiler_extension.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/tree_renderer/html_tree_renderer.hpp"
#include "duckdb/main/connection.hpp"

#include <cstdio>
#include <cstdlib>

namespace duckdb_shell {

namespace {

//! Maps the renderer-agnostic TreeRenderType categories onto the shell's highlight elements.
static HighlightElementType ToHighlightElement(duckdb::TreeRenderType type) {
	switch (type) {
	case duckdb::TreeRenderType::NODE_NAME:
		return HighlightElementType::EXPLAIN_OPERATOR;
	case duckdb::TreeRenderType::NODE_NAME_SCAN:
		return HighlightElementType::EXPLAIN_OPERATOR_SCAN;
	case duckdb::TreeRenderType::NODE_NAME_JOIN:
		return HighlightElementType::EXPLAIN_OPERATOR_JOIN;
	case duckdb::TreeRenderType::NODE_NAME_AGGREGATE:
		return HighlightElementType::EXPLAIN_OPERATOR_AGGREGATE;
	case duckdb::TreeRenderType::NODE_NAME_ORDER:
		return HighlightElementType::EXPLAIN_OPERATOR_ORDER;
	case duckdb::TreeRenderType::KEY:
		return HighlightElementType::EXPLAIN_DETAIL_KEY;
	case duckdb::TreeRenderType::VALUE:
		return HighlightElementType::EXPLAIN_DETAIL_VALUE;
	case duckdb::TreeRenderType::ROWS:
		return HighlightElementType::EXPLAIN_ROWS;
	case duckdb::TreeRenderType::TIMING_CRITICAL:
		return HighlightElementType::EXPLAIN_TIMING_CRITICAL;
	case duckdb::TreeRenderType::TIMING_HIGH:
		return HighlightElementType::EXPLAIN_TIMING_HIGH;
	case duckdb::TreeRenderType::TIMING_MODERATE:
		return HighlightElementType::EXPLAIN_TIMING_MODERATE;
	case duckdb::TreeRenderType::TIMING_LOW:
		return HighlightElementType::EXPLAIN_TIMING_LOW;
	case duckdb::TreeRenderType::HEADER:
		return HighlightElementType::EXPLAIN_HEADER;
	default:
		return HighlightElementType::EXPLAIN_LAYOUT;
	}
}

//! Base sink that maps each rendered segment onto a shell highlight element. Subclasses decide whether the
//! highlighted text is printed to the terminal or accumulated into a string.
class CLIHighlightRenderer : public duckdb::BaseTreeRenderer {
public:
	void Render(const string &text, duckdb::TreeRenderType type) override {
		Emit(text, ToHighlightElement(type));
	}

protected:
	virtual void Emit(const string &text, HighlightElementType type) = 0;
};

//! Prints the highlighted tree directly to an output stream (used by PRAGMA enable_profiling).
class CLIResultRenderer : public CLIHighlightRenderer {
public:
	explicit CLIResultRenderer(PrintOutput output = PrintOutput::STDERR)
	    : highlight(ShellState::Get()), output(output) {
	}

protected:
	void Emit(const string &text, HighlightElementType type) override {
		auto &state = highlight.state;
		bool is_console = output == PrintOutput::STDOUT ? state.stdout_is_console : state.stderr_is_console;
		if (is_console) {
			// PrintText additionally honors the ".highlight" toggle
			highlight.PrintText(text, output, type);
		} else {
			// not a console - emit plain text so redirected output has no escape codes
			state.Print(output, text);
		}
	}

private:
	ShellHighlight highlight;
	PrintOutput output;
};

//! Accumulates the highlighted tree into a string (used for EXPLAIN, whose plan flows back as a result value).
class HighlightStringRenderer : public CLIHighlightRenderer {
public:
	const string &str() {
		return result;
	}

protected:
	void Emit(const string &text, HighlightElementType type) override {
		auto &element = ShellHighlight::GetHighlightElement(type);
		if (!ShellHighlight::IsEnabled() ||
		    (element.color == PrintColor::STANDARD && element.intensity == PrintIntensity::STANDARD)) {
			result += text;
			return;
		}
		result += ShellHighlight::TerminalCode(element.color, element.intensity);
		result += text;
		result += ShellHighlight::ResetTerminalCode();
	}

private:
	string result;
};

//! A text tree renderer whose print sink highlights the profiler output for the CLI (PRAGMA enable_profiling).
class CLITreeRenderer : public duckdb::TextTreeRenderer {
public:
	duckdb::unique_ptr<duckdb::BaseTreeRenderer> GetPrintRenderer() override {
		return duckdb::make_uniq<CLIResultRenderer>(PrintOutput::STDERR);
	}
};

//! Renderer used for EXPLAIN / EXPLAIN ANALYZE in the CLI. The plan is rendered into a highlighted string that flows
//! back as the EXPLAIN result value and is printed by ModeExplainRenderer.
class ShellExplainPrinter : public duckdb::TextTreeRenderer {
public:
	// plain EXPLAIN: the logical/physical operator tree
	void ToStreamInternal(duckdb::RenderTree &root, duckdb::BaseTreeRenderer &ss) override {
		HighlightStringRenderer highlighted;
		duckdb::TextTreeRenderer::ToStreamInternal(root, highlighted);
		// remember the rendered width so the shell can page when the tree is too wide for the terminal
		ShellState::Get().last_explain_width = highlighted.max_render_width;
		ss << highlighted.str();
	}

	// EXPLAIN ANALYZE: the framed query profiling tree
	void RenderProfiler(const duckdb::QueryProfiler &profiler, duckdb::BaseTreeRenderer &ss) override {
		HighlightStringRenderer highlighted;
		profiler.RenderQueryTree(highlighted);
		// remember whether the tree folded anything (so ".last" is only offered when there is more to show) and its
		// rendered width (so the shell can page when it is too wide for the terminal)
		ShellState::Get().last_explain_hid_content = highlighted.hidden_content;
		ShellState::Get().last_explain_width = highlighted.max_render_width;
		ss << highlighted.str();
	}
};

} // namespace

bool RenderExpandedQueryTree(ShellState &state) {
	if (!state.stdout_is_console || !state.conn) {
		return false;
	}
	auto &context = *state.conn->context;
	auto &profiler = duckdb::QueryProfiler::Get(context);
	if (!profiler.HasRoot()) {
		return false;
	}
	// force the renderer to expand every operator for this render (disable timing-based folding)
	auto &settings = duckdb::ClientConfig::GetConfig(context).profiling_renderer_settings;
	auto saved_settings = settings;
	settings["expand_all"] = duckdb::Value::BOOLEAN(true);

	HighlightStringRenderer sink;
	try {
		profiler.RenderQueryTree(sink);
	} catch (...) {
		settings = std::move(saved_settings);
		throw;
	}
	settings = std::move(saved_settings);

	const string &rendered = sink.str();
	idx_t line_count = 0;
	for (auto c : rendered) {
		if (c == '\n') {
			line_count++;
		}
	}
	// materialized first, so we know the total size - page it when it does not fit on screen (too tall or too wide)
	duckdb::unique_ptr<PagerState> pager;
	if (state.ShouldUsePagerForSize(line_count, sink.max_render_width)) {
		pager = state.SetupPager();
	}
	state.Print(PrintOutput::STDOUT, rendered);
	return true;
}

//! Write a self-contained HTML page to a temp file (kept around, not auto-deleted) and launch the default browser on
//! it. Returns the file path, or an empty string on failure.
static string WriteProfileAndOpen(ShellState &state, const string &html) {
	if (html.empty()) {
		return string();
	}
	state.NewTempFile("html");
	auto path = state.zTempFile;
	// stop tracking it: the rendered profile should persist for the browser to load (and for the user to revisit)
	state.zTempFile = string();
	auto out = fopen(path.c_str(), "wb");
	if (!out) {
		state.PrintF(PrintOutput::STDERR, "Could not write profile to %s\n", path.c_str());
		return string();
	}
	fwrite(html.c_str(), 1, html.size(), out);
	fclose(out);

	const char *opener =
#if defined(_WIN32)
	    "start";
#elif defined(__APPLE__)
	    "open";
#else
	    "xdg-open";
#endif
	auto cmd = duckdb::StringUtil::Format("%s \"%s\"", opener, path);
	if (system(cmd.c_str()) != 0) {
		state.PrintF(PrintOutput::STDERR, "Failed to launch browser (%s)\n", cmd.c_str());
		return string();
	}
	return path;
}

//! HTML renderer that pretty-prints the query SQL via duckdb_format_sql. This requires the autocomplete extension (and
//! a catalog query), so it lives in the CLI rather than in core's HTMLTreeRenderer.
class ShellHTMLTreeRenderer : public duckdb::HTMLTreeRenderer {
protected:
	duckdb::string FormatSQL(const duckdb::string &sql) override {
		auto &state = ShellState::Get();
		if (sql.empty() || !state.conn) {
			return sql;
		}
		auto &context = *state.conn->context;
		// only format if the autocomplete extension is already loaded (never autoload)
		try {
			if (!context.db || !context.db->ExtensionIsLoaded("autocomplete")) {
				return sql;
			}
			duckdb::Connection con(*context.db);
			auto prepared = con.Prepare("SELECT duckdb_format_sql($1)");
			if (!prepared || prepared->HasError()) {
				return sql;
			}
			duckdb::vector<duckdb::Value> params;
			params.emplace_back(sql);
			auto result = prepared->Execute(params);
			if (!result || result->HasError()) {
				return sql;
			}
			auto chunk = result->Fetch();
			if (!chunk || chunk->size() == 0 || chunk->GetValue(0, 0).IsNull()) {
				return sql;
			}
			return chunk->GetValue(0, 0).ToString();
		} catch (const std::exception &) {
			return sql;
		}
	}
};

//! Renderer for "EXPLAIN [ANALYZE] (FORMAT WEB)" and the ".web" command: render the plan/profile to HTML and open it
//! in a browser when rendering finishes (see Finish). Emits nothing as the result, so the EXPLAIN output stays empty.
class WebTreeRenderer : public duckdb::TreeRenderer {
public:
	// plain EXPLAIN: render the operator tree to HTML (via a fresh HTML renderer to avoid virtual re-entrancy)
	void ToStreamInternal(duckdb::RenderTree &root, duckdb::BaseTreeRenderer &ss) override {
		ShellHTMLTreeRenderer html_renderer;
		duckdb::StringTreeRenderer sink;
		html_renderer.ToStreamInternal(root, sink);
		html = sink.str();
	}

	// EXPLAIN ANALYZE: render the full query profile to HTML (via ShellHTMLTreeRenderer so the SQL is pretty-printed)
	void RenderProfiler(const duckdb::QueryProfiler &profiler, duckdb::BaseTreeRenderer &ss) override {
		ShellHTMLTreeRenderer html_renderer;
		duckdb::StringTreeRenderer sink;
		html_renderer.RenderProfiler(profiler, sink);
		html = sink.str();
	}

	// rendering finished - write the HTML to a temp file and open it in a browser
	void Finish() override {
		if (html.empty()) {
			return;
		}
		auto &state = ShellState::Get();
		if (state.safe_mode) {
			state.Print(PrintOutput::STDERR, "(FORMAT WEB) cannot be used in -safe mode\n");
			return;
		}
		auto path = WriteProfileAndOpen(state, html);
		if (!path.empty()) {
			state.PrintF(PrintOutput::STDOUT, "Opening query profile in browser: %s\n", path.c_str());
		}
	}

	// keep the internal metric keys raw, matching the HTML renderer this delegates to
	bool UsesRawKeyNames() override {
		return true;
	}

	// one HTML page is opened per statement, so EXPLAIN renders only the final plan (not every stage)
	bool RendersSinglePlan() override {
		return true;
	}

	duckdb::string RenderProfilerDisabled() override {
		return "Query profiling is disabled. Use 'PRAGMA enable_profiling;' to enable profiling.";
	}

private:
	duckdb::string html;
};

bool OpenProfileInBrowser(ShellState &state) {
	if (!state.conn) {
		return false;
	}
	auto &context = *state.conn->context;
	auto &profiler = duckdb::QueryProfiler::Get(context);
	if (!profiler.HasRoot()) {
		state.Print(PrintOutput::STDERR, "No query profile available - run EXPLAIN ANALYZE first.\n");
		return false;
	}
	try {
		// render the current profile through the "web" format, which opens it in a browser (WebTreeRenderer::Finish)
		profiler.RenderProfile("web");
	} catch (const std::exception &e) {
		state.PrintF(PrintOutput::STDERR, "Failed to render profile: %s\n", e.what());
		return false;
	}
	return true;
}

void RegisterProfilerHighlighting(duckdb::DBConfig &config) {
	// PRAGMA enable_profiling output (printed directly to stderr)
	auto profiler_extension = duckdb::make_shared_ptr<duckdb::ProfilerExtension>();
	profiler_extension->create_renderer =
	    [](duckdb::ClientContext &context) -> duckdb::unique_ptr<duckdb::TreeRenderer> {
		auto renderer = duckdb::make_uniq<CLITreeRenderer>();
		renderer->Configure(duckdb::ClientConfig::GetConfig(context).profiling_renderer_settings);
		return std::move(renderer);
	};
	duckdb::ProfilerExtension::Register(config, "query_tree", std::move(profiler_extension));

	// EXPLAIN / EXPLAIN ANALYZE output (highlighted string, see ShellState::SetupPrettyExplain)
	auto explain_extension = duckdb::make_shared_ptr<duckdb::ProfilerExtension>();
	explain_extension->create_renderer =
	    [](duckdb::ClientContext &context) -> duckdb::unique_ptr<duckdb::TreeRenderer> {
		auto renderer = duckdb::make_uniq<ShellExplainPrinter>();
		renderer->Configure(duckdb::ClientConfig::GetConfig(context).profiling_renderer_settings);
		return std::move(renderer);
	};
	duckdb::ProfilerExtension::Register(config, "shell_explain_printer", std::move(explain_extension));

	// EXPLAIN [ANALYZE] (FORMAT WEB): render to HTML, open it in a browser, and report the path
	auto web_extension = duckdb::make_shared_ptr<duckdb::ProfilerExtension>();
	web_extension->create_renderer = [](duckdb::ClientContext &context) -> duckdb::unique_ptr<duckdb::TreeRenderer> {
		return duckdb::make_uniq<WebTreeRenderer>();
	};
	duckdb::ProfilerExtension::Register(config, "web", std::move(web_extension));
}

} // namespace duckdb_shell
