#include "shell_state.hpp"
#include "shell_highlight.hpp"
#include "shell_prompt.hpp"
#include "shell_progress_bar.hpp"
#include "shell_renderer.hpp"

#ifdef HAVE_LINENOISE
#include "linenoise.h"
#include "shortcuts.hpp"
#endif

namespace duckdb_shell {

MetadataResult ToggleAbout(ShellState &state, const vector<string> &args) {
	string about_text = "DuckDB is an in-process analytical database management system designed for fast "
	                    "execution of complex SQL queries. It runs embedded within its host process with "
	                    "no external dependencies, and is optimized for OLAP workloads using a columnar, "
	                    "vectorized execution engine.\n\n"
	                    "Developed and maintained by the DuckDB Foundation, "
	                    "available under the MIT License.\n"
	                    "For more information, visit https://duckdb.org or "
	                    "https://github.com/duckdb/duckdb.\n\n";

	state.PrintF(PrintOutput::STDOUT, "DuckDB %s (%s)\n\n", duckdb::DuckDB::LibraryVersion(),
	             duckdb::DuckDB::ReleaseCodename());
	state.Print(PrintOutput::STDOUT, about_text);
	return MetadataResult::SUCCESS;
}

MetadataResult ToggleBail(ShellState &state, const vector<string> &args) {
	if (args[1] == "auto") {
		state.bail = BailOnError::AUTOMATIC;
	} else if (state.StringToBool(args[1])) {
		state.bail = BailOnError::BAIL_ON_ERROR;
	} else {
		state.bail = BailOnError::DONT_BAIL_ON_ERROR;
	}
	return MetadataResult::SUCCESS;
}

MetadataResult ToggleBinary(ShellState &state, const vector<string> &args) {
	if (state.StringToBool(args[1])) {
		state.SetBinaryMode();
	} else {
		state.SetTextMode();
	}
	return MetadataResult::SUCCESS;
}

MetadataResult ChangeDirectory(ShellState &state, const vector<string> &args) {
	if (state.safe_mode) {
		state.PrintF(PrintOutput::STDERR, ".cd cannot be used in -safe mode\n");
		return MetadataResult::FAIL;
	}
	auto result = state.ChangeDirectory(args[1]);
	return result == SuccessState::SUCCESS ? MetadataResult::SUCCESS : MetadataResult::FAIL;
}

MetadataResult ToggleChanges(ShellState &state, const vector<string> &args) {
	state.SetOrClearFlag(ShellFlags::SHFLG_CountChanges, args[1]);
	return MetadataResult::SUCCESS;
}

MetadataResult ShowDatabases(ShellState &state, const vector<string> &args) {
	auto result = state.ShowDatabases();
	return result == SuccessState::SUCCESS ? MetadataResult::SUCCESS : MetadataResult::FAIL;
}

MetadataResult SetSeparator(ShellState &state, const vector<string> &args, const char *separator_name,
                            char &separator) {
	if (args.size() == 1) {
		state.PrintF("current %s separator: %c\n", separator_name, separator);
	} else if (args.size() != 2) {
		return MetadataResult::PRINT_USAGE;
	} else if (StringUtil::Equals(args[1], "space")) {
		separator = ' ';
	} else if (StringUtil::Equals(args[1], "none")) {
		separator = '\0';
	} else if (args[1].size() != 1) {
		state.PrintF(PrintOutput::STDERR, ".%s_sep SEP must be one byte, \"space\" or \"none\"\n", separator_name);
		return MetadataResult::FAIL;
	} else {
		separator = args[1][0];
	}
	return MetadataResult::SUCCESS;
}

MetadataResult SetDecimalSep(ShellState &state, const vector<string> &args) {
	return SetSeparator(state, args, "decimal", state.decimal_separator);
}

MetadataResult SetThousandSep(ShellState &state, const vector<string> &args) {
	return SetSeparator(state, args, "thousand", state.thousand_separator);
}

MetadataResult SetLargeNumberRendering(ShellState &state, const vector<string> &args) {
	if (StringUtil::Equals(args[1], "all")) {
		state.large_number_rendering = LargeNumberRendering::ALL;
	} else if (StringUtil::Equals(args[1], "footer")) {
		state.large_number_rendering = LargeNumberRendering::FOOTER;
	} else {
		if (state.StringToBool(args[1])) {
			state.large_number_rendering = LargeNumberRendering::DEFAULT;
		} else {
			state.large_number_rendering = LargeNumberRendering::NONE;
		}
	}
	return MetadataResult::SUCCESS;
}

MetadataResult DumpTable(ShellState &state, const vector<string> &args) {
	string zLike;
	bool savedShowHeader = state.showHeader;
	int savedShellFlags = state.shellFlgs;
	state.ShellClearFlag(ShellFlags::SHFLG_Newlines);
	state.ShellClearFlag(ShellFlags::SHFLG_Echo);
	for (idx_t i = 1; i < args.size(); i++) {
		if (args[i][0] == '-') {
			const char *z = args[i].c_str() + 1;
			if (z[0] == '-') {
				z++;
			}
			if (StringUtil::Equals(z, "newlines")) {
				state.ShellSetFlag(ShellFlags::SHFLG_Newlines);
			} else {
				state.PrintF(PrintOutput::STDERR, "Unknown option \"%s\" on \".dump\"\n", args[i].c_str());
				return MetadataResult::FAIL;
			}
		} else if (!zLike.empty()) {
			zLike = StringUtil::Format("%s OR name LIKE %s ESCAPE '\\'", zLike, SQLString(args[i]));
		} else {
			zLike = StringUtil::Format("name LIKE %s ESCAPE '\\'", SQLString(args[i]));
		}
	}

	state.PrintSQL("BEGIN TRANSACTION;\n");
	state.showHeader = 0;
	state.nErr = 0;
	if (zLike.empty()) {
		zLike = "true";
	}

	// Emit CREATE SCHEMA for non-main schemas first
	auto zSql = StringUtil::Format("SELECT DISTINCT table_schema FROM information_schema.tables "
	                               "WHERE table_schema != 'main' AND table_schema NOT LIKE 'pg_%%' "
	                               "AND table_schema != 'information_schema' "
	                               "AND table_name IN (SELECT name FROM sqlite_schema WHERE (%s) AND type=='table') "
	                               "ORDER BY table_schema",
	                               zLike);
	auto result = state.conn->Query(zSql);
	for (auto &row : *result) {
		auto schema = row.GetValue<string>(0);
		auto create_schema = StringUtil::Format("CREATE SCHEMA IF NOT EXISTS %s;", SQLIdentifier(schema));
		state.PrintSQL(create_schema + ";\n");
	}

	zSql = StringUtil::Format("SELECT name, type, sql FROM sqlite_schema "
	                          "WHERE (%s) AND type=='table'"
	                          "  AND sql NOT NULL"
	                          " ORDER BY tbl_name='sqlite_sequence'",
	                          zLike);
	state.RunSchemaDumpQuery(zSql);
	zSql = StringUtil::Format("SELECT sql FROM sqlite_schema "
	                          "WHERE (%s) AND sql NOT NULL"
	                          "  AND type IN ('index','trigger','view')",
	                          zLike);
	state.RunTableDumpQuery(zSql);
	state.PrintSQL(state.nErr ? "ROLLBACK; -- due to errors\n" : "COMMIT;\n");
	state.showHeader = savedShowHeader;
	state.shellFlgs = savedShellFlags;
	return MetadataResult::SUCCESS;
}

MetadataResult ToggleEcho(ShellState &state, const vector<string> &args) {
	state.SetOrClearFlag(ShellFlags::SHFLG_Echo, args[1]);
	return MetadataResult::SUCCESS;
}

MetadataResult ToggleAutoFormat(ShellState &state, const vector<string> &args) {
	if (state.StringToBool(args[1])) {
		state.auto_format = AutoFormatMode::AUTO_FORMAT_COMPLETE_STATEMENTS;
	} else {
		state.auto_format = AutoFormatMode::NO_AUTO_FORMAT;
	}
	return MetadataResult::SUCCESS;
}

MetadataResult ExitProcess(ShellState &state, const vector<string> &args) {
	if (args.size() > 2) {
		return MetadataResult::PRINT_USAGE;
	}
	int rc = 0;
	if (args.size() > 1 && (rc = (int)ShellState::StringToInt(args[1])) != 0) {
		// Stop all input processing and return through main's cleanup.
		ShellState::Exit(rc);
	}
	return MetadataResult::EXIT;
}

MetadataResult ToggleHeaders(ShellState &state, const vector<string> &args) {
	state.showHeader = state.StringToBool(args[1]);
	state.ShellSetFlag(ShellFlags::SHFLG_HeaderSet);
	return MetadataResult::SUCCESS;
}

MetadataResult SetHighlightColors(ShellState &state, const vector<string> &args) {
	if (args.size() < 3 || args.size() > 4) {
		return MetadataResult::PRINT_USAGE;
	}
	ShellHighlight highlighter(state);
	if (!highlighter.SetColor(args[1].c_str(), args[2].c_str(), args.size() == 3 ? nullptr : args[3].c_str())) {
		return MetadataResult::FAIL;
	}
	return MetadataResult::SUCCESS;
}

MetadataResult ToggleHighlightErrors(ShellState &state, const vector<string> &args) {
	state.highlight_errors = state.StringToBool(args[1]) ? OptionType::ON : OptionType::OFF;
	return MetadataResult::SUCCESS;
}

MetadataResult ToggleHighlightMode(ShellState &state, const vector<string> &args) {
	if (args[1] == "mixed") {
		state.highlight_mode = HighlightMode::MIXED_MODE;
	} else if (args[1] == "dark") {
		state.highlight_mode = HighlightMode::DARK_MODE;
	} else if (args[1] == "light") {
		state.highlight_mode = HighlightMode::LIGHT_MODE;
	} else if (args[1] == "auto") {
		state.highlight_mode = HighlightMode::AUTOMATIC;
	} else {
		return MetadataResult::PRINT_USAGE;
	}
	ShellHighlight highlight(state);
	highlight.ToggleMode(state.highlight_mode);
	return MetadataResult::SUCCESS;
}

MetadataResult ToggleHighlightResult(ShellState &state, const vector<string> &args) {
	state.highlight_results = state.StringToBool(args[1]) ? OptionType::ON : OptionType::OFF;
	return MetadataResult::SUCCESS;
}

MetadataResult ShowHelp(ShellState &state, const vector<string> &args) {
	if (args.size() >= 2) {
		if (duckdb::StringUtil::CIEquals(args[1], "agent")) {
			state.PrintAgentHelp(PrintOutput::STDOUT, false);
			return MetadataResult::SUCCESS;
		}
#ifdef HAVE_LINENOISE
		if (duckdb::StringUtil::CIEquals(args[1], "shortcuts")) {
			auto shortcuts = duckdb::GetShellShortcuts();
			const char *current_category = nullptr;
			for (auto &entry : shortcuts) {
				if (!current_category || strcmp(current_category, entry.category) != 0) {
					if (current_category) {
						state.PrintF("\n");
					}
					current_category = entry.category;
					duckdb_shell::ShellHighlight highlighter(state);
					highlighter.PrintText(entry.category, PrintOutput::STDOUT, HighlightElementType::KEYWORD);
					state.PrintF("\n");
				}
				state.PrintF("  %-24s %s\n", entry.key_name, entry.description);
			}
			return MetadataResult::SUCCESS;
		}
#endif
		idx_t n = state.PrintHelp(args[1].c_str());
		if (n == 0) {
			state.PrintF("Nothing matches '%s'\n", args[1].c_str());
		}
	} else {
		state.PrintHelp(0);
	}
	return MetadataResult::SUCCESS;
}

MetadataResult OpenProfileWeb(ShellState &state, const vector<string> &args) {
	if (state.safe_mode) {
		state.Print(PrintOutput::STDERR, ".web cannot be used in -safe mode\n");
		return MetadataResult::FAIL;
	}
	return OpenProfileInBrowser(state) ? MetadataResult::SUCCESS : MetadataResult::FAIL;
}

MetadataResult RenderLastResult(ShellState &state, const vector<string> &args) {
	// if the last query produced a profiling tree (e.g. EXPLAIN ANALYZE), show the full expanded query tree
	if (RenderExpandedQueryTree(state)) {
		return MetadataResult::SUCCESS;
	}
	if (state.last_result) {
		auto renderer = state.GetRenderer();
		renderer->RemoveRenderLimits();
		auto res = state.RenderQueryResult(*renderer, *state.last_result);
		if (res == SuccessState::FAILURE) {
			return MetadataResult::FAIL;
		}
	}
	return MetadataResult::SUCCESS;
}

MetadataResult PrintHistory(ShellState &state, const vector<string> &args) {
	if (args.size() > 2) {
		return MetadataResult::PRINT_USAGE;
	}
	// query the history through the shell_history() table function
	auto result = state.conn->Query("SELECT id, sql FROM shell_history()");
	if (result->HasError()) {
		state.PrintF(PrintOutput::STDERR, "%s: %s\n", state.program_name, result->GetError().c_str());
		return MetadataResult::FAIL;
	}
	idx_t row_count = result->RowCount();
	idx_t start = 0;
	if (args.size() == 2) {
		// .history N - only show the last N entries
		auto limit = static_cast<idx_t>(ShellState::StringToInt(args[1]));
		if (limit < row_count) {
			start = row_count - limit;
		}
	}
	auto rows = result->Collection().GetRows();
	for (idx_t row = start; row < row_count; row++) {
		auto id = rows.GetValue(0, row).GetValue<int64_t>();
		auto sql = rows.GetValue(1, row).GetValue<string>();
		state.HighlightSQL(sql);
		// prefix each entry with its index - align any continuation lines (from multi-line
		// statements) underneath the SQL by padding them with spaces to the prefix width
		string prefix = StringUtil::Format("%5lld  ", static_cast<long long>(id));
		sql = StringUtil::Replace(sql, "\n", "\n" + string(prefix.size(), ' '));
		state.PrintF("%s%s\n", prefix.c_str(), sql.c_str());
	}
	return MetadataResult::SUCCESS;
}

MetadataResult ToggleLog(ShellState &state, const vector<string> &args) {
	if (state.safe_mode) {
		state.PrintF(PrintOutput::STDERR, ".log cannot be used in -safe mode\n");
		return MetadataResult::FAIL;
	}
	const char *zFile = args[1].c_str();
	state.CloseOutputFile(state.pLog);
	state.pLog = state.OpenOutputFile(zFile, 0);
	return MetadataResult::SUCCESS;
}

MetadataResult SetMaxBytes(ShellState &state, const vector<string> &args) {
	if (args.size() > 2) {
		return MetadataResult::PRINT_USAGE;
	}
	if (args.size() == 1) {
		state.PrintF("current max bytes: %zu\n", state.max_bytes);
		return MetadataResult::SUCCESS;
	}
	state.max_bytes = (size_t)ShellState::StringToInt(args[1]);
	return MetadataResult::SUCCESS;
}

MetadataResult SetMaxCellWidth(ShellState &state, const vector<string> &args) {
	if (args.size() > 2) {
		return MetadataResult::PRINT_USAGE;
	}
	if (args.size() == 1) {
		state.PrintF("current max cell width: %zu\n", state.max_cell_width);
		return MetadataResult::SUCCESS;
	}
	state.max_cell_width = (size_t)ShellState::StringToInt(args[1]);
	return MetadataResult::SUCCESS;
}

MetadataResult SetMaxRows(ShellState &state, const vector<string> &args) {
	if (args.size() > 3) {
		return MetadataResult::PRINT_USAGE;
	}
	if (args.size() == 1) {
		state.PrintF("current max rows: %zu\n", state.max_rows);
		return MetadataResult::SUCCESS;
	}
	state.max_rows = (size_t)ShellState::StringToInt(args[1]);
	if (args.size() > 2) {
		state.max_analyze_rows = (size_t)ShellState::StringToInt(args[2]);
	}
	return MetadataResult::SUCCESS;
}

MetadataResult SetMaxWidth(ShellState &state, const vector<string> &args) {
	if (args.size() > 2) {
		return MetadataResult::PRINT_USAGE;
	}
	if (args.size() == 1) {
		state.PrintF("current max rows: %zu\n", state.max_width);
	} else {
		state.max_width = (size_t)ShellState::StringToInt(args[1]);
	}
	return MetadataResult::SUCCESS;
}

MetadataResult SetColumnRendering(ShellState &state, const vector<string> &args) {
	state.columns = 1;
	return MetadataResult::SUCCESS;
}

MetadataResult SetRowRendering(ShellState &state, const vector<string> &args) {
	state.columns = 0;
	return MetadataResult::SUCCESS;
}

MetadataResult ImportData(ShellState &state, const vector<string> &args) {
	if (!state.ImportData(args)) {
		return MetadataResult::FAIL;
	}
	return MetadataResult::SUCCESS;
}

MetadataResult OpenDatabase(ShellState &state, const vector<string> &args) {
	if (!state.OpenDatabase(args)) {
		return MetadataResult::FAIL;
	}
	return MetadataResult::SUCCESS;
}

MetadataResult PrintArguments(ShellState &state, const vector<string> &args) {
	for (idx_t i = 1; i < args.size(); i++) {
		if (i > 1) {
			state.PrintF(" ");
		}
		state.PrintF("%s", args[i].c_str());
	}
	state.PrintF("\n");
	return MetadataResult::SUCCESS;
}

void ShellState::SetPrompt(char *prompt, const string &new_value) {
	strncpy(prompt, new_value.c_str(), MAX_PROMPT_SIZE - 1);
	prompt[MAX_PROMPT_SIZE - 1] = '\0';
}

MetadataResult SetPrompt(ShellState &state, const vector<string> &args) {
	if (args.size() >= 2) {
		auto new_prompt = make_uniq<Prompt>();
		new_prompt->ParsePrompt(args[1]);
		state.main_prompt = std::move(new_prompt);
	}
	if (args.size() >= 3) {
		ShellState::SetPrompt(state.continuePrompt, args[2]);
	}
	if (args.size() >= 4) {
		ShellState::SetPrompt(state.continuePromptSelected, args[3]);
	}
	if (args.size() >= 5) {
		ShellState::SetPrompt(state.scrollUpPrompt, args[4]);
	}
	if (args.size() >= 6) {
		ShellState::SetPrompt(state.scrollDownPrompt, args[5]);
	}
	return MetadataResult::SUCCESS;
}

MetadataResult ConfigureProgressBar(ShellState &state, const vector<string> &args) {
	if (args.size() < 2 || args.size() > 3) {
		return MetadataResult::PRINT_USAGE;
	}
	if (args[1] == "--clear") {
		if (args.size() != 2) {
			return MetadataResult::PRINT_USAGE;
		}
		state.progress_bar->ClearComponents();
	} else if (args[1] == "--add") {
		if (args.size() != 3) {
			return MetadataResult::PRINT_USAGE;
		}
		state.progress_bar->AddComponent(args[2]);
	} else {
		return MetadataResult::PRINT_USAGE;
	}
	return MetadataResult::SUCCESS;
}

MetadataResult SetOutputMode(ShellState &state, const vector<string> &args) {
	if (args.size() > 3) {
		return MetadataResult::PRINT_USAGE;
	}
	if (args.size() == 1) {
		state.PrintF("current output mode: %s\n", ShellState::ModeToString(state.mode));
	} else {
		if (!state.SetOutputMode(args[1], args.size() > 2 ? args[2].c_str() : nullptr)) {
			return MetadataResult::FAIL;
		}
	}
	return MetadataResult::SUCCESS;
}

MetadataResult QuitProcess(ShellState &, const vector<string> &args) {
	return MetadataResult::EXIT;
}

MetadataResult SetOutput(ShellState &state, const vector<string> &args) {
	if (!state.SetOutputFile(args, OutputCommand::OUTPUT)) {
		return MetadataResult::FAIL;
	}
	return MetadataResult::SUCCESS;
}

MetadataResult SetOutputOnce(ShellState &state, const vector<string> &args) {
	if (!state.SetOutputFile(args, OutputCommand::ONCE)) {
		return MetadataResult::FAIL;
	}
	return MetadataResult::SUCCESS;
}

MetadataResult SetOutputExcel(ShellState &state, const vector<string> &args) {
	if (!state.SetOutputFile(args, OutputCommand::EXCEL)) {
		return MetadataResult::FAIL;
	}
	return MetadataResult::SUCCESS;
}

MetadataResult ReadFromFile(ShellState &state, const vector<string> &args) {
	if (!state.ReadFromFile(args[1])) {
		return MetadataResult::FAIL;
	}
	return MetadataResult::SUCCESS;
}

MetadataResult DisplaySchemas(ShellState &state, const vector<string> &args) {
	if (!state.DisplaySchemas(args)) {
		return MetadataResult::FAIL;
	}
	return MetadataResult::SUCCESS;
}

MetadataResult RunShellCommand(ShellState &state, const vector<string> &args) {
	if (state.safe_mode) {
		state.Print(PrintOutput::STDERR, ".sh/.system cannot be used in -safe mode\n");
		return MetadataResult::FAIL;
	}
	int x;
	if (args.size() < 2) {
		return MetadataResult::PRINT_USAGE;
	}
	auto zCmd = StringUtil::Format(StringUtil::Contains(args[1], ' ') ? "%s" : "\"%s\"", args[1]);
	for (idx_t i = 2; i < args.size(); i++) {
		zCmd += StringUtil::Format(StringUtil::Contains(args[i], ' ') ? " %s" : " \"%s\"", args[i]);
	}
	x = system(zCmd.c_str());
	if (x) {
		state.PrintF(PrintOutput::STDERR, "System command returns %d\n", x);
	}
	return MetadataResult::SUCCESS;
}

MetadataResult ShowConfiguration(ShellState &state, const vector<string> &args) {
	state.ShowConfiguration();
	return MetadataResult::SUCCESS;
}

MetadataResult SetStartupText(ShellState &state, const vector<string> &args) {
	auto prev_display = state.startup_text;
	if (args[1] == "all") {
		state.startup_text = StartupText::ALL;
	} else if (args[1] == "version") {
		state.startup_text = StartupText::VERSION;
	} else if (args[1] == "none") {
		state.startup_text = StartupText::NONE;
	} else {
		return MetadataResult::PRINT_USAGE;
	}
	if (state.displayed_loading_resources_message && prev_display == StartupText::ALL &&
	    state.startup_text != StartupText::ALL) {
		ShellHighlight highlight(state);
		string warning = "WARNING: .startup_text should be on top of your ~/.duckdbrc in order to "
		                 "prevent the \"Loading resources\" message from being displayed\n";
		highlight.PrintText(warning, PrintOutput::STDERR, HighlightElementType::STARTUP_TEXT);
	}
	return MetadataResult::SUCCESS;
}

MetadataResult ShowVersion(ShellState &state, const vector<string> &args) {
	state.PrintF("DuckDB %s (%s) %s\n" /*extra-version-info*/, duckdb::DuckDB::LibraryVersion(),
	             duckdb::DuckDB::ReleaseCodename(), duckdb::DuckDB::SourceID());
#define CTIMEOPT_VAL_(opt) #opt
#define CTIMEOPT_VAL(opt)  CTIMEOPT_VAL_(opt)
#if defined(__clang__) && defined(__clang_major__)
	state.PrintF("clang-" CTIMEOPT_VAL(__clang_major__) "." CTIMEOPT_VAL(__clang_minor__) "." CTIMEOPT_VAL(
	    __clang_patchlevel__) "\n");
#elif defined(_MSC_VER)
	state.PrintF("msvc-" CTIMEOPT_VAL(_MSC_VER) "\n");
#elif defined(__GNUC__) && defined(__VERSION__)
	state.PrintF("gcc-" __VERSION__ "\n");
#endif
	return MetadataResult::SUCCESS;
}

MetadataResult SetWidths(ShellState &state, const vector<string> &args) {
	state.colWidth.clear();
	for (idx_t j = 1; j < args.size(); j++) {
		state.colWidth.push_back((int)ShellState::StringToInt(args[j]));
	}
	return MetadataResult::SUCCESS;
}

MetadataResult ShowIndexes(ShellState &state, const vector<string> &args) {
	return state.DisplayEntries(args, 'i');
}

MetadataResult ShowManual(ShellState &state, const vector<string> &args) {
	return state.DisplayManual(args);
}

MetadataResult ShowTables(ShellState &state, const vector<string> &args) {
	return state.DisplayTables(args);
}

static string JoinArguments(const vector<string> &args) {
	string command;
	for (idx_t i = 1; i < args.size(); i++) {
		if (i > 1) {
			command += " ";
		}
		command += args[i];
	}
	return command;
}

MetadataResult SetUICommand(ShellState &state, const vector<string> &args) {
	if (args.size() < 1) {
		return MetadataResult::PRINT_USAGE;
	}
	state.ui_command = "CALL " + JoinArguments(args);
	return MetadataResult::SUCCESS;
}

MetadataResult SetServeCommand(ShellState &state, const vector<string> &args) {
	if (args.size() < 2) {
		return MetadataResult::PRINT_USAGE;
	}
	state.serve_command = JoinArguments(args);
	return MetadataResult::SUCCESS;
}

MetadataResult SetConnectCommand(ShellState &state, const vector<string> &args) {
	if (args.size() < 2) {
		return MetadataResult::PRINT_USAGE;
	}
	state.connect_command = JoinArguments(args);
	return MetadataResult::SUCCESS;
}

MetadataResult ToggleHighlighting(ShellState &state, const vector<string> &args) {
	ShellHighlight::SetHighlighting(state.StringToBool(args[1]));
	return MetadataResult::SUCCESS;
}

#ifdef HAVE_LINENOISE
MetadataResult ToggleErrorRendering(ShellState &state, const vector<string> &args) {
	linenoiseSetErrorRendering(state.StringToBool(args[1]));
	return MetadataResult::SUCCESS;
}

MetadataResult ToggleCompletionRendering(ShellState &state, const vector<string> &args) {
	linenoiseSetCompletionRendering(state.StringToBool(args[1]));
	return MetadataResult::SUCCESS;
}

MetadataResult ToggleMultiLine(ShellState &state, const vector<string> &args) {
	if (args.size() != 1) {
		return MetadataResult::PRINT_USAGE;
	}
	linenoiseSetMultiLine(true);
	return MetadataResult::SUCCESS;
}

MetadataResult ToggleSingleLine(ShellState &state, const vector<string> &args) {
	if (args.size() != 1) {
		return MetadataResult::PRINT_USAGE;
	}
	linenoiseSetMultiLine(false);
	return MetadataResult::SUCCESS;
}

MetadataResult TrySetHighlightColor(ShellState &state, const string &component, const string &code) {
	vector<string> args;
	args.push_back("highlight_colors");
	args.push_back(component);
	args.push_back(code);
	return SetHighlightColors(state, args);
}

enum class DeprecatedHighlightColors {
	COMMENT,
	CONSTANT,
	KEYWORD,
	ERROR,
	CONT,
	CONT_SEL,
};

template <DeprecatedHighlightColors T>
MetadataResult SetHighlightingColor(ShellState &state, const vector<string> &args) {
	string literal;
	string hint;
	switch (T) {
	case DeprecatedHighlightColors::COMMENT:
		literal = "comment";
		hint = literal;
		break;
	case DeprecatedHighlightColors::CONSTANT:
		state.PrintF(PrintOutput::STDERR,
		             ".constant has been split into numeric_constant and string_constant, use .highlight_colors "
		             "numeric_constant %s and .highlight_colors string_constant %s instead\n",
		             args[1].c_str(), args[1].c_str());
		return MetadataResult::FAIL;
	case DeprecatedHighlightColors::KEYWORD:
		literal = "keyword";
		hint = literal;
		break;
	case DeprecatedHighlightColors::ERROR:
		literal = "error";
		hint = literal;
		break;
	case DeprecatedHighlightColors::CONT:
		literal = "cont";
		hint = "continuation";
		break;
	case DeprecatedHighlightColors::CONT_SEL:
		literal = "cont_sel";
		hint = "continuation_selected";
		break;
	default:
		throw std::runtime_error("eek");
	}
	state.PrintF(PrintOutput::STDERR,
	             "WARNING: .%s [COLOR] will be removed in a future release, use .highlight_colors %s %s instead\n",
	             literal.c_str(), hint.c_str(), args[1].c_str());
	return TrySetHighlightColor(state, hint, args[1]);
}

#endif

double GetHue(const HighlightColorInfo &input) {
	double r = input.r / 255.0;
	double g = input.g / 255.0;
	double b = input.b / 255.0;

	double max = std::max({r, g, b});
	double min = std::min({r, g, b});
	double delta = max - min;

	if (delta == 0) {
		return 0;
	}
	double hue;
	if (max == r) {
		hue = (g - b) / delta;
	} else if (max == g) {
		hue = 2.0 + (b - r) / delta;
	} else {
		hue = 4.0 + (r - g) / delta;
	}
	hue *= 60.0;
	if (hue < 0) {
		hue += 360;
	}
	return hue;
}

double GetLum(const HighlightColorInfo &input) {
	return sqrt(0.241 * input.r + 0.691 * input.g + 0.068 * input.b);
}

idx_t FindColorGroup(const vector<vector<string>> &groups, const string &name) {
	optional_idx group_idx;
	for (idx_t grp_idx = 0; grp_idx < groups.size(); grp_idx++) {
		for (auto &group_name : groups[grp_idx]) {
			if (StringUtil::Contains(name, group_name)) {
				group_idx = grp_idx;
				break;
			}
		}
	}
	if (!group_idx.IsValid()) {
		throw InternalException("Color %s does not have a group", name);
	}
	return group_idx.GetIndex();
}

MetadataResult DisplayColors(ShellState &state, const vector<string> &args) {
	bool bold = false;
	bool underline = false;
	for (idx_t i = 1; i < args.size(); i++) {
		if (args[i] == "bold") {
			bold = true;
		} else if (args[i] == "underline") {
			underline = true;
		} else {
			return MetadataResult::PRINT_USAGE;
		}
	}
	PrintIntensity intensity = PrintIntensity::STANDARD;
	if (bold && underline) {
		intensity = PrintIntensity::BOLD_UNDERLINE;
	} else if (bold) {
		intensity = PrintIntensity::BOLD;
	} else if (underline) {
		intensity = PrintIntensity::UNDERLINE;
	}
	ShellHighlight highlighter(state);
	vector<duckdb::const_reference<HighlightColorInfo>> color_list;
	for (idx_t i = 0; i < static_cast<idx_t>(PrintColor::EXTENDED_COLOR_COUNT); i++) {
		auto color = static_cast<PrintColor>(i);
		color_list.push_back(*ShellHighlight::GetColorInfo(color));
	}
	// group and sort the colors
	// groups color by whether or not their name contains certain words
	// this is the order in which colors are displayed
	vector<vector<string>> color_groups {{"red", "maroon", "coral"},
	                                     {"orange"},
	                                     {"yellow", "gold", "khaki", "wheat"},
	                                     {"green", "chartreuse", "lime", "honeydew", "olive"},
	                                     {"cyan", "aqua", "turquoise"},
	                                     {"blue", "navy"},
	                                     {"pink", "orchid", "rose", "thistle", "salmon", "tan"},
	                                     {"purple", "magenta", "plum", "fuchsia", "violet"},
	                                     {"brown"},
	                                     {"grey", "gray", "black"},
	                                     {"white", "silver", "cornsilk"}};
	std::sort(color_list.begin(), color_list.end(), [&](const HighlightColorInfo &a, const HighlightColorInfo &b) {
		// find the group index of this color
		auto a_group = FindColorGroup(color_groups, a.color_name);
		auto b_group = FindColorGroup(color_groups, b.color_name);
		if (a_group != b_group) {
			return a_group < b_group;
		}
		// for colors in the same group - sort on hue, followed by luminosity
		auto a_hue = GetHue(a);
		auto b_hue = GetHue(b);

		if (a_hue != b_hue) {
			return a_hue < b_hue;
		}
		return GetLum(a) < GetLum(b);
	});

	// now print the colors
	for (auto &color_info_ref : color_list) {
		auto &color_info = color_info_ref.get();
		auto color = static_cast<PrintColor>(color_info.code);
		highlighter.PrintText(color_info.color_name, PrintOutput::STDOUT, color, intensity);
		state.Print(" ");
	}
	state.Print("\n");
	return MetadataResult::SUCCESS;
}

MetadataResult SetReadLineVersion(ShellState &state, const vector<string> &args) {
	if (args[1] == "linenoise") {
#ifdef HAVE_LINENOISE
		state.rl_version = ReadLineVersion::LINENOISE;
		return MetadataResult::SUCCESS;
#else
		state.Print("linenoise is not available in this build");
		return MetadataResult::FAIL;
#endif
	} else if (args[1] == "fallback") {
		state.rl_version = ReadLineVersion::FALLBACK;
		return MetadataResult::SUCCESS;
	}
	return MetadataResult::PRINT_USAGE;
}

MetadataResult SetPager(ShellState &state, const vector<string> &args) {
	if (args.size() == 1) {
		// Show current pager status
		string mode_str;
		switch (state.pager_mode) {
		case PagerMode::PAGER_OFF:
			mode_str = "off";
			break;
		case PagerMode::PAGER_ON:
			mode_str = "on";
			break;
		case PagerMode::PAGER_AUTOMATIC:
			mode_str = "automatic";
			break;
		}
		state.PrintF("Pager mode: %s\n", mode_str);
		if (state.pager_mode == PagerMode::PAGER_AUTOMATIC) {
			string page_mode = StringUtil::Format("Trigger pager when rows exceed %d", state.pager_min_rows);
			if (state.max_width == 0) {
				page_mode += " or result set is wider than terminal";
			} else {
				page_mode += StringUtil::Format(" or result set is wider than %d characters", state.max_width);
			}
			state.Print(page_mode);
		}
		if (state.pager_mode != PagerMode::PAGER_OFF || !state.pager_command.empty()) {
			state.PrintF("Pager command: %s\n", state.pager_command);
		}
		return MetadataResult::SUCCESS;
	}
	if (args[1] == "set_row_threshold") {
		if (args.size() != 3) {
			return MetadataResult::PRINT_USAGE;
		}
		idx_t limit = (idx_t)state.StringToInt(args[2]);
		state.pager_min_rows = limit;
		return MetadataResult::SUCCESS;
	}
	if (args.size() != 2) {
		return MetadataResult::PRINT_USAGE;
	}
	if (args[1] == "on") {
		state.pager_mode = PagerMode::PAGER_ON;
		if (state.pager_command.empty()) {
			state.pager_command = state.GetSystemPager();
		}
	} else if (args[1] == "off") {
		state.pager_mode = PagerMode::PAGER_OFF;
	} else if (args[1] == "automatic") {
		state.pager_mode = PagerMode::PAGER_AUTOMATIC;
	} else {
		if (state.safe_mode) {
			state.Print(PrintOutput::STDERR, ".pager cannot be used in -safe mode\n");
			return MetadataResult::FAIL;
		}
		state.pager_command = args[1];
	}
	return MetadataResult::SUCCESS;
}

static const MetadataCommand metadata_commands[] = {
    {"about", 0, ToggleAbout, "", "Show information about DuckDB", 0, ""},
#ifdef HAVE_LINENOISE
    {"auto_format", 2, ToggleAutoFormat, "on|off", "Automatically format SQL before execution.  Default OFF", 3, ""},
#endif
    {"bail", 2, ToggleBail, "on|off", "Stop after hitting an error.  Default OFF", 3, ""},
    {"binary", 2, ToggleBinary, "on|off", "Turn binary output on or off.  Default OFF", 3, ""},
    {"cd", 2, ChangeDirectory, "DIRECTORY", "Change the working directory to DIRECTORY", 0, ""},
    {"changes", 2, ToggleChanges, "on|off", "Show number of rows changed by SQL", 3, ""},
    {"columns", 1, SetColumnRendering, "", "Column-wise rendering of query results", 0, ""},
    {"connect_command", 0, SetConnectCommand, "COMMAND", "Set the command executed by -connect", 0,
     "Placeholders of the form {parameter|default} are replaced by the -host/-port/-token arguments"},
#ifdef HAVE_LINENOISE
    {"comment", 2, SetHighlightingColor<DeprecatedHighlightColors::COMMENT>, "?COLOR?",
     "DEPRECATED: Sets the syntax highlighting color used for comment values", 0, nullptr},
    {"constant", 2, SetHighlightingColor<DeprecatedHighlightColors::CONSTANT>, "?COLOR?",
     "DEPRECATED: Sets the syntax highlighting color used for constant values", 0, nullptr},
    {"cont", 2, SetHighlightingColor<DeprecatedHighlightColors::CONT>, "?COLOR?",
     "DEPRECATED: Sets the syntax highlighting color used for continuation markers", 0, nullptr},
    {"cont_sel", 2, SetHighlightingColor<DeprecatedHighlightColors::CONT_SEL>, "?COLOR?",
     "DEPRECATED: Sets the syntax highlighting color used for continuation markers", 0, nullptr},
#endif
    {"decimal_sep", 0, SetDecimalSep, "SEP",
     "Sets the decimal separator used when rendering numbers. Only for duckbox mode.", 3, ""},
    {"databases", 1, ShowDatabases, "", "List names and files of attached databases", 2, ""},
    {
        "dump",
        0,
        DumpTable,
        "?TABLE?",
        "Render database content as SQL",
        0,
        "Options:\n\t--newlines\tAllow unescaped newline characters in output\nTABLE is a LIKE pattern for the tables "
        "to dump\nAdditional LIKE patterns can be given in subsequent arguments",
    },
    {"display_colors", 0, DisplayColors, "[bold|underline]", "Display all terminal colors and their names", 0, ""},
    {"echo", 2, ToggleEcho, "on|off", "Turn command echo on or off", 3, ""},
    {"edit", 0, nullptr, "", "Opens an external text editor to edit a query.", 0,
     "Notes:\n\t* The editor is read from the environment variables\n\t  DUCKDB_EDITOR, EDITOR, VISUAL in-order\n\t* "
     "If "
     "none of these are set, the default editor is vi\n\t* \\e can be used as an alias for .edit"},
#ifdef HAVE_LINENOISE
    {"error", 2, SetHighlightingColor<DeprecatedHighlightColors::ERROR>, "?COLOR?",
     "DEPRECATED: Sets the syntax highlighting color used for errors", 0, nullptr},
#endif
    {"excel", 0, SetOutputExcel, "", "Display the output of next command in spreadsheet", 0,
     "--bom\tPut a UTF8 byte-order mark on intermediate file"},
    {"exit", 0, ExitProcess, "?CODE?", "Exit this program with return-code CODE", 0, ""},
    {"headers", 2, ToggleHeaders, "on|off", "Turn display of headers on or off", 0, ""},
    {"help", 0, ShowHelp, "?-all? ?PATTERN?", "Show help text for PATTERN (.help agent: the agent mode)", 0, ""},
    {"highlight", 2, ToggleHighlighting, "on|off", "Toggle syntax highlighting in the shell on/off", 0, ""},
    {"highlight_colors", 0, SetHighlightColors, "OPTIONS", "Configure highlighting colors", 0, ""},
    {"highlight_errors", 2, ToggleHighlightErrors, "on|off", "Turn highlighting of errors on or off", 0, ""},
    {"highlight_mode", 2, ToggleHighlightMode, "mixed|dark|light", "Toggle the highlight mode to dark or light mode", 0,
     ""},
    {"highlight_results", 2, ToggleHighlightResult, "on|off", "Turn highlighting of results on or off", 0, ""},
    {"history", 0, PrintHistory, "?N?", "Show the command history with syntax highlighting", 0,
     "If N is given, only the last N entries are shown.\nUse the shell_history() table function to query the history."},
    {"import", 0, ImportData, "FILE TABLE", "Import data from FILE into TABLE", 0,
     "Options:\n\t--csv\tImport data from CSV (read_csv)\n\t--json\tImport data from JSON "
     "(read_json)\n\t--parquet\tImport data from Parquet (read_parquet)\n\t--[parameter] [value]\tProvides a parameter "
     "to the reader function\n\tNotes:\n\t* If TABLE does not exist, it is created.\n\t* If file type is not selected, "
     "the input mode is derived from the file extension\n\t* Generic parameters are passed to the reader functions"},
    {"indexes", 0, ShowIndexes, "?TABLE?", "Show names of indexes", 0,
     "Notes:\n\t* If TABLE is specified, only show indexes for\n\t  tables matching TABLE using the LIKE operator."},
    {"indices", 0, ShowIndexes, "?TABLE?", "Show names of indexes", 0},
#ifdef HAVE_LINENOISE
    {"keyword", 2, SetHighlightingColor<DeprecatedHighlightColors::KEYWORD>, "?COLOR?",
     "DEPRECATED: Sets the syntax highlighting color used for keywords", 0, nullptr},
#endif
    {"last", 1, RenderLastResult, "",
     "Render the last result in full (after EXPLAIN ANALYZE: the full, expanded query tree)", 0, ""},
    {"large_number_rendering", 2, SetLargeNumberRendering, "MODE",
     "Toggle readable rendering of large numbers (duckbox only)", 0, "Mode: all|footer|off"},
    {"log", 2, ToggleLog, "FILE|off", "Turn logging on or off.  FILE can be stderr/stdout", 0, ""},
    {"manual", 2, ShowManual, "FUNCTION", "Show the manual page for a SQL function", 0,
     "Displays the signatures, descriptions and examples of all overloads of FUNCTION.\n"
     "FUNCTION may be qualified: [database.][schema.]function."},
    {"maxbytes", 0, SetMaxBytes, "COUNT",
     "Sets the maximum number of bytes of rows for display (0 = all). Only for markdown mode in -agent mode.", 0, ""},
    {"maxcellwidth", 0, SetMaxCellWidth, "COUNT",
     "Sets the maximum number of characters per cell (0 = all). Only for markdown mode in -agent mode.", 0, ""},
    {"maxrows", 0, SetMaxRows, "COUNT",
     "Sets the maximum number of rows for display (default: 40, -1 = all). Only for duckbox mode and for markdown "
     "mode in -agent mode.",
     0, ""},
    {"maxwidth", 0, SetMaxWidth, "COUNT",
     "Sets the maximum width in characters. 0 defaults to terminal width. Only for duckbox mode.", 0, ""},
    {"mode", 0, SetOutputMode, "MODE ?TABLE?", "Set output mode", 0,
     "MODE is one of:\n\tascii\tColumns/rows delimited by 0x1F and 0x1E\n\tbox\tTables using unicode box-drawing "
     "characters\n\tcsv\tComma-separated values\n\tcolumn\tOutput in columns. (See .width)\n\tduckbox\tTables "
     "with extensive features\n\thtml\tHTML <table> code\n\tinsert\tSQL insert statements for TABLE\n\t"
     "json\tResults in a JSON array\n\tjsonlines\tResults in a NDJSON\n\tlatex\tLaTeX tabular environment code\n\t"
     "line\tOne value per line\n\tlist\tValues delimited by \"|\"\n\tmarkdown\tMarkdown table format\n\t"
     "quote\tEscape answers as for SQL\n\ttable\tASCII-art table\n\ttabs\tTab-separated values\n\ttcl\tTCL list "
     "elements\n\ttrash\tNo output"},
#ifdef HAVE_LINENOISE
    {"multiline", 1, ToggleMultiLine, "", "Sets the render mode to multi-line", 0, ""},
#endif
    {"nullvalue", 2, ShellState::SetNullValue, "STRING", "Use STRING in place of NULL values", 0, ""},

    {"open", 0, OpenDatabase, "?OPTIONS? ?FILE?", "Close existing database and reopen FILE", 2,
     "Options:\n\t--new\tInitialize FILE to an empty database\n\t--nofollow\tDo not follow symbolic "
     "links\n\t--readonly\tOpen FILE in read-only mode\n\t--sql\tSet FILE using SQL"},
    {"once", 0, SetOutputOnce, "?FILE?", "Output for the next SQL command only to FILE", 0,
     "If FILE begins with '|' then open as a pipe\n\t--bom\tPut a UTF8 byte-order mark at the beginning\n\t-e\tSend "
     "output to the system text editor\n\t-x\tSend output as CSV to a spreadsheet (same as \".excel\")"},
    {"output", 0, SetOutput, "?FILE?", "Send output to FILE or stdout if FILE is omitted", 0,
     "If FILE begins with '|' then open as a pipe\n\t--bom\tPut a UTF8 byte-order mark at the beginning\n\t-e\tSend "
     "output to the system text editor\n\t-x\tSend output as CSV to a spreadsheet (same as \".excel\")"},
    {"pager", 0, SetPager, "OPTIONS", "Control pager usage for output", 0,
     "Options:\n\t[on|off|automatic]\tToggle pager mode (default: automatic)\n\tset_[row|column]_threshold "
     "THRESHOLD\tIn automatic mode, trigger the pager when the result has more rows/columns than "
     "this\n\t[pager_command]\tSet the pager command to invoke\nNote: Set DUCKDB_PAGER or PAGER environment variable "
     "or <cmd> to configure default pager"},
    {"print", 0, PrintArguments, "STRING...", "Print literal STRING", 3, ""},
    {"progress_bar", 0, ConfigureProgressBar, "OPTIONS", "Configure the progress bar display", 0,
     "OPTIONS:\n\t--add [COMPONENT]\tAdd a component to the progress bar\n\t--clear\tClear all components"},
    {"prompt", 0, SetPrompt, "MAIN CONTINUE", "Replace the standard prompts", 0, ""},

    {"quit", 0, QuitProcess, "", "Exit this program", 0, ""},
    {"read", 2, ReadFromFile, "FILE", "Read input from FILE", 3, ""},
    {"read_line_version", 2, SetReadLineVersion, "linenoise|fallback",
     "Sets the library used for processing interactive input", 0, ""},
#ifdef HAVE_LINENOISE
    {"render_completion", 2, ToggleCompletionRendering, "on|off",
     "Toggle displaying of completion prompts in the shell on/off", 0, ""},
    {"render_errors", 2, ToggleErrorRendering, "on|off", "Toggle rendering of errors in the shell on/off", 0, ""},
#endif
    {"rows", 1, SetRowRendering, "", "Row-wise rendering of query results (default)", 0, ""},
    {"safe_mode", 0, ShellState::EnableSafeMode, "", "Enable safe-mode", 0, ""},
    {"separator", 0, ShellState::SetSeparator, "COL ?ROW?", "Change the column and row separators", 0, ""},
    {"serve_command", 0, SetServeCommand, "COMMAND", "Set the command executed by -serve", 0,
     "Placeholders of the form {parameter|default} are replaced by the -host/-port/-token arguments"},
    {"schema", 0, DisplaySchemas, "?PATTERN?", "Show the CREATE statements matching PATTERN", 0,
     "By default the schema is pretty-printed using the SQL formatter.\nOptions:\n\t--no-indent\tPrint the schema as "
     "it is stored, without formatting\n\t--no-format\tAlias for --no-indent\n\t--indent\tForce pretty-printing (the "
     "default)\n\t--format\tAlias for --indent"},
    {"shell", 0, RunShellCommand, "CMD ARGS...", "Run CMD ARGS... in a system shell", 0, ""},
    {"show", 1, ShowConfiguration, "", "Show the current values for various settings", 0, ""},
#ifdef HAVE_LINENOISE
    {"singleline", 1, ToggleSingleLine, "", "Sets the render mode to single-line", 0, ""},
#endif
    {"startup_text", 2, SetStartupText, "none|version|all",
     "Start-up text to display. Set this as the first line in .duckdbrc", 0, ""},
    {"system", 0, RunShellCommand, "CMD ARGS...", "Run CMD ARGS... in a system shell", 0, ""},
    {"tables", 0, ShowTables, "?TABLE?", "List names of tables matching LIKE pattern TABLE", 2, ""},
    {"thousand_sep", 0, SetThousandSep, "SEP",
     "Sets the thousand separator used when rendering numbers. Only for duckbox mode.", 4, ""},
    {"timer", 0, ShellState::ToggleTimer, "on|off [DIGITS]",
     "Turn SQL timer on or off, DIGITS decimals for the real time (default 3)", 0, ""},
    {"ui_command", 0, SetUICommand, "[command]", "Set the UI command", 0, ""},
    {"version", 1, ShowVersion, "", "Show the version", 0, ""},
    {"web", 1, OpenProfileWeb, "", "Open the last query profile (EXPLAIN ANALYZE) in a web browser", 0, ""},
    {"width", 0, SetWidths, "NUM1 NUM2 ...", "Set minimum column widths for columnar output", 0,
     "Negative values right-justify"},
#if defined(_WIN32) || defined(WIN32)
    {"utf8", 1, [](ShellState &, const vector<string> &) -> MetadataResult { return MetadataResult::SUCCESS; }, "",
     "Deprecated. This option is accepted for compatibility but has no effect.", 0, ""},
#endif
    {nullptr, 0, nullptr, 0, nullptr}};

bool ShouldPrintCommand(const MetadataCommand &command, const string &glob_pattern) {
	if (!command.extra_description) {
		return false;
	}
	if (StringUtil::Contains(command.description, "DEPRECATED")) {
		return false;
	}
	// check if the command matches the pattern
	if (glob_pattern.empty()) {
		// no pattern - always matches
		return true;
	}
	// explicit pattern provided - glob
	return ShellState::StringGlob(glob_pattern.c_str(), command.command);
}

struct PrintCommandInfo {
	string command_name;
	string first_part;
	string second_part;
	HighlightElementType first_part_highlight = HighlightElementType::NONE;
};

idx_t ShellState::PrintHelp(const char *pattern) {
	bool print_extended = false;
	string glob_pattern;
	if (pattern) {
		// if a pattern is provided we always print extended info
		print_extended = true;
		if (StringUtil::Equals(pattern, "-a") || StringUtil::Equals(pattern, "-all") ||
		    StringUtil::Equals(pattern, "--all")) {
			// --all matches all commands
			glob_pattern = string();
		} else {
			glob_pattern = StringUtil::Format("%s*", pattern);
		}
	}

	constexpr idx_t MIN_SPACING = 4;
	constexpr idx_t SPACING_PER_LAYER = 2;
	vector<PrintCommandInfo> print_info_list;
	// gather a list of all print statements
	for (idx_t i = 0; metadata_commands[i].command; i++) {
		auto &command = metadata_commands[i];
		if (!ShouldPrintCommand(command, glob_pattern)) {
			continue;
		}
		PrintCommandInfo print_info;
		print_info.command_name = StringUtil::Format(".%s", command.command);
		print_info.first_part += StringUtil::Format(" %s", command.usage);
		print_info.second_part = command.description;
		print_info.first_part_highlight = HighlightElementType::STRING_CONSTANT;
		print_info_list.push_back(std::move(print_info));

		if (print_extended) {
			// process extended info
			PrintCommandInfo current_command;
			bool first_part = true;
			bool after_newline = true;
			for (auto c = command.extra_description; *c; c++) {
				if (*c == '\n') {
					// newline - flush the current command and reset
					print_info_list.push_back(std::move(current_command));
					current_command = PrintCommandInfo();
					first_part = true;
					after_newline = true;
				} else if (*c == '\t') {
					// tab
					if (after_newline) {
						// tab right after newline - add spaces
						current_command.first_part += string(SPACING_PER_LAYER, ' ');
					} else {
						// tab not right after newline move to second part
						if (!first_part) {
							throw duckdb::InternalException(
							    "Failed to parse extra description for command \"%s\" - only one tab (switch from "
							    "first -> second part) was expected",
							    command.command);
						}
						first_part = false;
					}
				} else {
					if (after_newline) {
						// add one more "layer" to commands
						current_command.first_part += string(SPACING_PER_LAYER, ' ');
						if (*c == '-') {
							current_command.first_part_highlight = HighlightElementType::STRING_CONSTANT;
						}
					}
					after_newline = false;
					if (first_part) {
						current_command.first_part += *c;
					} else {
						current_command.second_part += *c;
					}
				}
			}
			if (!current_command.first_part.empty()) {
				// push final command
				print_info_list.push_back(std::move(current_command));
			}
		}
	}
	// figure out alignment based on the total first part print size
	idx_t max_lhs_size = 0;
	for (auto &print_info : print_info_list) {
		if (print_info.second_part.empty()) {
			// only print info with two parts needs to influence padding
			continue;
		}
		idx_t lhs_size = print_info.command_name.size() + print_info.first_part.size() + MIN_SPACING;
		if (lhs_size > max_lhs_size) {
			max_lhs_size = lhs_size;
		}
	}

	// print
	for (auto &print_info : print_info_list) {
		idx_t lhs_size = print_info.command_name.size() + print_info.first_part.size();
		string spaces;
		if (!print_info.second_part.empty()) {
			// only add padding for lines that have extra info
			spaces = string(max_lhs_size - lhs_size, ' ');
		}
		ShellHighlight highlighter(*this);
		if (!print_info.command_name.empty()) {
			highlighter.PrintText(print_info.command_name, PrintOutput::STDOUT, HighlightElementType::KEYWORD);
		}
		highlighter.PrintText(print_info.first_part, PrintOutput::STDOUT, print_info.first_part_highlight);
		PrintF("%s%s\n", spaces.c_str(), print_info.second_part.c_str());
	}
	if (!print_extended) {
		PrintF("\nRun .help --all for extended information\n");
#ifdef HAVE_LINENOISE
		PrintF("Run .help shortcuts for keyboard shortcuts\n");
#endif
	}
	return print_info_list.size();
}

vector<string> ShellState::GetMetadataCompletions(const char *zLine, idx_t nLine) {
	char zBuf[1000];
	vector<string> result;
	for (idx_t c = 0; metadata_commands[c].command; c++) {
		auto &command = metadata_commands[c];
		auto &line = command.command;
		bool found_match = true;
		idx_t line_pos;
		zBuf[0] = '.';
		for (line_pos = 0; !IsSpace(line[line_pos]) && line[line_pos] && line_pos + 2 < sizeof(zBuf); line_pos++) {
			zBuf[line_pos + 1] = line[line_pos];
			if (line_pos + 1 < nLine && line[line_pos] != zLine[line_pos + 1]) {
				// only match prefixes for auto-completion, i.e. ".sh" matches ".shell"
				found_match = false;
				break;
			}
		}
		zBuf[line_pos + 1] = '\0';
		if (found_match && line_pos + 1 >= nLine) {
			result.push_back(zBuf);
		}
	}
	return result;
}

optional_ptr<const MetadataCommand> ShellState::FindMetadataCommand(const string &option, string &error_msg) const {
	idx_t n = option.size();
	for (idx_t command_idx = 0; metadata_commands[command_idx].command; command_idx++) {
		auto &command = metadata_commands[command_idx];
		idx_t match_size = command.match_size ? command.match_size : n;
		if (n < match_size || strncmp(option.c_str(), command.command, n) != 0) {
			continue;
		}
		return command;
	}
	// no command found
	error_msg = StringUtil::Format("Unknown Command Error: Unrecognized command '%s'\n", option);

	vector<string> command_names;
	for (idx_t command_idx = 0; metadata_commands[command_idx].command; command_idx++) {
		auto &command = metadata_commands[command_idx];
		command_names.push_back(string(".") + command.command);
	}
	auto candidates_msg = StringUtil::CandidatesErrorMessage(command_names, option, "Did you mean");
	error_msg += candidates_msg + "\n";
	error_msg += "Run '.help' for more information.";
	return nullptr;
}

} // namespace duckdb_shell
