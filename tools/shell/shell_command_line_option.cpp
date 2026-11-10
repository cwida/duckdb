#include "shell_state.hpp"
#include "shell_highlight.hpp"
#include "terminal.hpp"

namespace duckdb_shell {

// FIXME: should be moved out of a define
#define SEP_Unit   "\x1F"
#define SEP_Record "\x1E"

//! An output mode on the command line is a deliberate choice of format: it switches detected agent mode off (see
//! DetectAgentMode). Runs in the first pass, before the mode itself is applied
MetadataResult MarkOutputModeFlag(ShellState &state, const vector<string> &args) {
	state.output_mode_flag = true;
	return MetadataResult::SUCCESS;
}

template <RenderMode output_mode>
MetadataResult ToggleOutputMode(ShellState &state, const vector<string> &args) {
	state.cMode = state.mode = output_mode;
	return MetadataResult::SUCCESS;
}

MetadataResult ToggleASCIIMode(ShellState &state, const vector<string> &args) {
	state.cMode = state.mode = RenderMode::ASCII;
	state.colSeparator = SEP_Unit;
	state.rowSeparator = SEP_Record;
	return MetadataResult::SUCCESS;
}

MetadataResult ToggleCSVMode(ShellState &state, const vector<string> &args) {
	state.cMode = state.mode = RenderMode::CSV;
	state.colSeparator = ",";
	return MetadataResult::SUCCESS;
}

MetadataResult EnableBail(ShellState &state, const vector<string> &args) {
	state.bail = BailOnError::BAIL_ON_ERROR;
	return MetadataResult::SUCCESS;
}

MetadataResult EnableBatch(ShellState &state, const vector<string> &args) {
	state.stdin_is_interactive = false;
	return MetadataResult::SUCCESS;
}

MetadataResult DisableBatch(ShellState &state, const vector<string> &args) {
	state.stdin_is_interactive = true;
	return MetadataResult::SUCCESS;
}

template <OptionType AGENT_MODE>
MetadataResult SetAgentMode(ShellState &state, const vector<string> &args) {
	state.agent_mode = AGENT_MODE;
	return MetadataResult::SUCCESS;
}

MetadataResult SetReadOnlyMode(ShellState &state, const vector<string> &args) {
	state.config.options.access_mode = duckdb::AccessMode::READ_ONLY;
	return MetadataResult::SUCCESS;
}

template <bool HEADER>
MetadataResult ToggleHeader(ShellState &state, const vector<string> &args) {
	state.showHeader = HEADER;
	return MetadataResult::SUCCESS;
}

MetadataResult DisableStdin(ShellState &state, const vector<string> &args) {
	state.readStdin = false;
	return MetadataResult::SUCCESS;
}

MetadataResult EnableEcho(ShellState &state, const vector<string> &args) {
	state.ShellSetFlag(ShellFlags::SHFLG_Echo);
	return MetadataResult::SUCCESS;
}

MetadataResult AllowUnredacted(ShellState &state, const vector<string> &args) {
	state.config.SetOptionByName("allow_unredacted_secrets", true);
	return MetadataResult::SUCCESS;
}

MetadataResult AllowUnsigned(ShellState &state, const vector<string> &args) {
	state.config.SetOptionByName("allow_unsigned_extensions", true);
	return MetadataResult::SUCCESS;
}

MetadataResult ShowVersionAndExit(ShellState &state, const vector<string> &args) {
	printf("%s (%s) %s\n", duckdb::DuckDB::LibraryVersion(), duckdb::DuckDB::ReleaseCodename(),
	       duckdb::DuckDB::SourceID());
	return MetadataResult::EXIT;
}

MetadataResult PrintHelpAndExit(ShellState &state, const vector<string> &args) {
	state.PrintUsage();
	return MetadataResult::EXIT;
}

MetadataResult LaunchUI(ShellState &state, const vector<string> &args) {
	// run the UI command
	auto rc = state.RunInitialCommand((char *)state.ui_command.c_str(), true);
	if (rc != 0) {
		return MetadataResult::FAIL;
	}
	return MetadataResult::SUCCESS;
}

bool ShellState::ExpandCommandParameters(const string &command, string &result) {
	result.clear();
	bool in_string_literal = false;
	for (idx_t pos = 0; pos < command.size(); pos++) {
		if (command[pos] != '{') {
			if (command[pos] == '\'') {
				in_string_literal = !in_string_literal;
			}
			result += command[pos];
			continue;
		}
		auto close_pos = command.find('}', pos);
		if (close_pos == string::npos) {
			// no closing bracket - emit the remainder of the command verbatim
			result += command.substr(pos);
			break;
		}
		auto placeholder = command.substr(pos + 1, close_pos - pos - 1);
		pos = close_pos;

		// placeholders have the form {parameter} or {parameter|default_value}
		auto separator = placeholder.find('|');
		auto parameter = placeholder.substr(0, separator);
		auto entry = command_parameters.find(parameter);
		if (entry != command_parameters.end()) {
			result += in_string_literal ? StringUtil::Replace(entry->second, "'", "''") : entry->second;
			continue;
		}
		if (separator == string::npos) {
			// no value provided and no default value specified
			PrintDatabaseError(
			    StringUtil::Format("Invalid Command Error: no value provided for parameter '%s' in command \"%s\"\n"
			                       "Provide a value using '-%s VALUE', or specify a default value using '{%s|default}'",
			                       parameter, command, parameter, parameter));
			return false;
		}
		result += placeholder.substr(separator + 1);
	}
	return true;
}

//! Expand and run one of the configurable commands (e.g. the serve/connect command)
static MetadataResult RunConfiguredCommand(ShellState &state, const string &command) {
	string expanded_command;
	if (!state.ExpandCommandParameters(command, expanded_command)) {
		return MetadataResult::FAIL;
	}
	auto rc = state.RunInitialCommand(expanded_command.c_str(), true);
	if (rc != 0) {
		return MetadataResult::FAIL;
	}
	return MetadataResult::SUCCESS;
}

MetadataResult LaunchServer(ShellState &state, const vector<string> &args) {
	return RunConfiguredCommand(state, state.serve_command);
}

//! The only database type `-serve` can serve
static constexpr const char *SERVE_TYPE = "quack";

//! Split the `TYPE[:SECRET]` argument shared by `-serve` and `-connect`
static bool ParseConnectionTarget(ShellState &state, const char *option, const string &argument, string &type,
                                  string &secret) {
	auto separator = argument.find(':');
	type = argument.substr(0, separator);
	secret = separator == string::npos ? string() : argument.substr(separator + 1);
	if (type.empty() || (separator != string::npos && secret.empty())) {
		state.PrintF(PrintOutput::STDERR, "%s: Error: invalid argument (%s) for '-%s': expected TYPE[:SECRET]\n",
		             state.program_name, argument.c_str(), option);
		return false;
	}
	return true;
}

static string QuoteSecretName(const string &secret) {
	return StringUtil::Replace(secret, "'", "''");
}

//! Runs while the command line is still being parsed, so that the database argument can be rejected
//! before the database is opened
MetadataResult MarkClientMode(ShellState &state, const vector<string> &args) {
	// the shell is a client for the remainder of the session - Ctrl-D exits instead of disconnecting
	state.started_as_client = true;
	if (args.size() <= 1) {
		return MetadataResult::SUCCESS;
	}
	string type, secret;
	if (!ParseConnectionTarget(state, "connect", args[1], type, secret)) {
		ShellState::Exit(1);
	}
	state.command_parameters["type"] = type;
	if (!secret.empty()) {
		state.command_parameters["connect_secret"] = StringUtil::Format(" (SECRET '%s')", QuoteSecretName(secret));
	}
	return MetadataResult::SUCCESS;
}

MetadataResult SetServeTarget(ShellState &state, const vector<string> &args) {
	if (args.size() <= 1) {
		return MetadataResult::SUCCESS;
	}
	string type, secret;
	if (!ParseConnectionTarget(state, "serve", args[1], type, secret)) {
		ShellState::Exit(1);
	}
	if (!StringUtil::CIEquals(type, SERVE_TYPE)) {
		state.PrintF(PrintOutput::STDERR,
		             "%s: Error: cannot serve a database of type '%s' - only '%s' is supported by '-serve'\n",
		             state.program_name, type.c_str(), SERVE_TYPE);
		ShellState::Exit(1);
	}
	if (!secret.empty()) {
		state.command_parameters["serve_secret"] = StringUtil::Format(", secret='%s'", QuoteSecretName(secret));
	}
	return MetadataResult::SUCCESS;
}

MetadataResult ConnectToServer(ShellState &state, const vector<string> &args) {
	return RunConfiguredCommand(state, state.connect_command);
}

MetadataResult SetNewlineSeparator(ShellState &state, const vector<string> &args) {
	// run the UI command
	state.rowSeparator = args[1];
	return MetadataResult::SUCCESS;
}

MetadataResult SetStorageVersion(ShellState &state, const vector<string> &args) {
	auto &storage_version = args[1];
	try {
		state.config.options.storage_compatibility = duckdb::StorageCompatibility::FromString(storage_version);
	} catch (std::exception &ex) {
		duckdb::ErrorData error(ex);
		state.PrintF(PrintOutput::STDERR, "%s: Error: unknown argument (%s) for '-storage-version': %s\n",
		             state.program_name, storage_version.c_str(), error.Message().c_str());
		return MetadataResult::EXIT;
	}
	return MetadataResult::SUCCESS;
}

template <HighlightMode mode>
MetadataResult SetColorScheme(ShellState &state, const vector<string> &args) {
	state.highlight_mode = mode;
	ShellHighlight highlight(state);
	highlight.ToggleMode(mode);
	return MetadataResult::SUCCESS;
}

MetadataResult ProcessFile(ShellState &state, const vector<string> &args) {
	state.readStdin = false;
	auto &file = args[1];
	if (!state.ProcessFile(file)) {
		return MetadataResult::FAIL;
	}
	return MetadataResult::SUCCESS;
}

MetadataResult SetInitFile(ShellState &state, const vector<string> &args) {
	state.initFile = args[1];
	return MetadataResult::SUCCESS;
}

MetadataResult SkipInit(ShellState &state, const vector<string> &args) {
	state.run_init = false;
	return MetadataResult::SUCCESS;
}

template <bool EXIT>
MetadataResult RunCommand(ShellState &state, const vector<string> &args) {
	if (EXIT) {
		state.readStdin = false;
	}
	// Always bail if -c or -s fail
	bool bail = true;
	if (state.bail != BailOnError::AUTOMATIC) {
		bail = state.bail == BailOnError::BAIL_ON_ERROR;
	}
	auto &cmd = args[1];
	auto rc = state.RunInitialCommand(cmd.c_str(), bail);
	if (rc != 0) {
		return MetadataResult::FAIL;
	}
	return MetadataResult::SUCCESS;
}

MetadataResult RunManual(ShellState &state, const vector<string> &args) {
	// show the manual page and exit; unlike -c we keep interactive detection so the output stays colored
	state.readStdin = false;
	if (state.DisplayManual(args) == MetadataResult::FAIL) {
		return MetadataResult::FAIL;
	}
	return MetadataResult::SUCCESS;
}

MetadataResult FormatStdin(ShellState &state, const vector<string> &args) {
	state.readStdin = false;

	if (duckdb::Terminal::IsAtty()) {
		state.PrintF(PrintOutput::STDERR,
		             "%s: Error: -format requires SQL input on stdin (e.g. echo 'SELECT 1' | duckdb -format)\n",
		             state.program_name);
		return MetadataResult::FAIL;
	}

	// Read all of stdin into a string.
	string sql = state.ReadFileContents(stdin);

	auto result = state.FormatSQL(sql);
	if (result != MetadataResult::SUCCESS) {
		return result;
	}

	// Write formatted SQL to stdout, with syntax highlighting if stdout is a terminal.
	state.HighlightSQL(sql);
	state.Print(PrintOutput::STDOUT, sql);
	return MetadataResult::SUCCESS;
}

MetadataResult FormatFile(ShellState &state, const vector<string> &args) {
	state.readStdin = false;
	const string &filename = args[1];

	string sql = state.ReadFileContents(filename);

	auto result = state.FormatSQL(sql);
	if (result != MetadataResult::SUCCESS) {
		return result;
	}
	state.HighlightSQL(sql);
	state.Print(PrintOutput::STDOUT, sql);
	return MetadataResult::SUCCESS;
}

static const CommandLineOption command_line_options[] = {
    {"agent", 0, "", SetAgentMode<OptionType::ON>, nullptr,
     "render output for an AI coding agent (compact markdown tables with a loud row/byte cap and a result hash, "
     "JSON errors, compact plans, cost estimates and progress on stderr). Default: on when an agent's environment "
     "variable is set, stdout is not a terminal and no output mode is given; DUCKDB_AGENT_MODE=1 / 0 forces it on / "
     "off"},
    {"ascii", 0, "", MarkOutputModeFlag, ToggleASCIIMode, "set output mode to 'ascii'"},
    {"bail", 0, "", nullptr, EnableBail, "stop after hitting an error"},
    {"batch", 0, "", EnableBatch, EnableBatch, "force batch I/O'"},
    {"box", 0, "", MarkOutputModeFlag, ToggleOutputMode<RenderMode::BOX>, "set output mode to 'box'"},
    {"column", 0, "", MarkOutputModeFlag, ToggleOutputMode<RenderMode::COLUMN>, "set output mode to 'column'"},
    {"cmd", 1, "COMMAND", nullptr, RunCommand<false>, "run \"COMMAND\" before reading stdin"},
    {"connect", 0, "[TYPE[:SECRET]]", MarkClientMode, ConnectToServer,
     "connect to a database of the given type, optionally using a named secret. Default: 'quack' "
     "(configurable with .connect_command)",
     true},
    {"csv", 0, "", MarkOutputModeFlag, ToggleCSVMode, "set output mode to 'csv'"},
    {"c", 1, "COMMAND", EnableBatch, RunCommand<true>, "run \"COMMAND\" and exit"},
    {"dark-mode", 0, "", SetColorScheme<HighlightMode::DARK_MODE>, SetColorScheme<HighlightMode::DARK_MODE>,
     "use dark mode colors"},
    {"echo", 0, "", nullptr, EnableEcho, "print commands before execution"},
    {"f", 1, "FILENAME", EnableBatch, ProcessFile, "read/process named file and exit"},
    {"format", 0, "", EnableBatch, FormatStdin, "format SQL from stdin, writing result to stdout"},
    {"format-file", 1, "FILENAME", EnableBatch, FormatFile, "format SQL in file, writing result to stdout"},
    {"init", 1, "FILENAME", SetInitFile, nullptr, "read/process named file"},
    {"header", 0, "", nullptr, ToggleHeader<true>, "turn headers on"},
    {"h", 0, "", EnableBatch, PrintHelpAndExit, "show help message"},
    {"help", 0, "", EnableBatch, PrintHelpAndExit, "show help message"},
    {"html", 0, "", MarkOutputModeFlag, ToggleOutputMode<RenderMode::HTML>, "set output mode to HTML"},
    {"interactive", 0, "", nullptr, DisableBatch, "force interactive I/O"},
    {"json", 0, "", MarkOutputModeFlag, ToggleOutputMode<RenderMode::JSON>, "set output mode to 'json'"},
    {"jsonlines", 0, "", MarkOutputModeFlag, ToggleOutputMode<RenderMode::JSONLINES>, "set output mode to 'jsonlines'"},
    {"light-mode", 0, "", SetColorScheme<HighlightMode::LIGHT_MODE>, SetColorScheme<HighlightMode::LIGHT_MODE>,
     "use light mode colors"},
    {"line", 0, "", MarkOutputModeFlag, ToggleOutputMode<RenderMode::LINE>, "set output mode to 'line'"},
    {"list", 0, "", MarkOutputModeFlag, ToggleOutputMode<RenderMode::LIST>, "set output mode to 'list'"},
    {"manual", 1, "FUNCTION", nullptr, RunManual, "show the manual page for a SQL function and exit"},
    {"markdown", 0, "", MarkOutputModeFlag, ToggleOutputMode<RenderMode::MARKDOWN>, "set output mode to 'markdown'"},
    {"newline", 1, "SEP", nullptr, SetNewlineSeparator, "set output row separator. Default: '\\n'"},
    {"no-agent", 0, "", SetAgentMode<OptionType::OFF>, nullptr, "never render output for an AI coding agent"},
    {"no-init", 0, "", SkipInit, nullptr, "skip processing the init file"},
    {"no-stdin", 0, "", nullptr, DisableStdin, "exit after processing options instead of reading stdin"},
    {"noheader", 0, "", nullptr, ToggleHeader<false>, "turn headers off"},
    {"nullvalue", 1, "TEXT", nullptr, ShellState::SetNullValue, "set text string for NULL values. Default 'NULL'"},
    {"quote", 0, "", MarkOutputModeFlag, ToggleOutputMode<RenderMode::QUOTE>, "set output mode to 'quote'"},
    {"readonly", 0, "", SetReadOnlyMode, nullptr, "open the database read-only"},
    {"s", 1, "COMMAND", EnableBatch, RunCommand<true>, "run \"COMMAND\" and exit"},
    {"safe", 0, "", ShellState::EnableSafeMode, nullptr, "enable safe-mode"},
    {"separator", 1, "SEP", nullptr, ShellState::SetSeparator, "set output column separator. Default: '|'"},
    {"serve", 0, "[quack[:SECRET]]", SetServeTarget, LaunchServer,
     "serve this database, optionally using a named secret (configurable with .serve_command)", true},
    {"storage-version", 1, "VER", SetStorageVersion, nullptr,
     "database storage compatibility version to use. Default: 'v0.10.0'"},
    {"table", 0, "", MarkOutputModeFlag, ToggleOutputMode<RenderMode::TABLE>, "set output mode to 'table'"},
    {"ui", 0, "", nullptr, LaunchUI, "launches a web interface using the ui extension (configurable with .ui_command)"},
    {"unredacted", 0, "", AllowUnredacted, nullptr, "allow printing unredacted secrets"},
    {"unsigned", 0, "", AllowUnsigned, nullptr, "allow loading of unsigned extensions"},
    {"version", 0, "", nullptr, ShowVersionAndExit, "show DuckDB version"},
    {nullptr, 0, nullptr, nullptr, nullptr, nullptr}};

optional_idx FindOption(const char *name) {
	for (idx_t c = 0; command_line_options[c].option; c++) {
		auto &option = command_line_options[c];
		if (!StringUtil::Equals(name, option.option)) {
			// not this one
			continue;
		}
		// found it!
		return c;
	}
	return optional_idx();
}

optional_ptr<const CommandLineOption> ShellState::FindCommandLineOption(const string &option, string &error_msg) const {
	auto c = FindOption(option.c_str());
	if (!c.IsValid()) {
		// we haven't found it yet - try substituting all underscores with dashes
		// this is legacy behavior - we allow e.g. "-storage_version" to be used instead of "-storage-version"
		auto option_name = StringUtil::Replace(option, "_", "-");
		c = FindOption(option_name.c_str());
	}
	if (!c.IsValid()) {
		// not found
		error_msg = StringUtil::Format("Unknown Option Error: Unrecognized option '-%s'\n", option);
		vector<string> option_names;
		for (idx_t c = 0; command_line_options[c].option; c++) {
			auto &option = command_line_options[c];
			option_names.push_back(string("-") + option.option);
		}
		auto candidates_msg = StringUtil::CandidatesErrorMessage(option_names, "-" + option, "Did you mean");
		error_msg += candidates_msg + "\n";
		error_msg += StringUtil::Format("Run '%s -help' for a list of options.\n", program_name);
		return nullptr;
	}
	return command_line_options[c.GetIndex()];
}

struct PrintOptionInfo {
	string command_name;
	string arguments;
	string description;
};

void ShellState::PrintUsage() {
	ShellHighlight highlighter(*this);
	highlighter.PrintText("Usage: ", PrintOutput::STDOUT, PrintColor::STANDARD, PrintIntensity::BOLD);
	highlighter.PrintText(program_name, PrintOutput::STDOUT, HighlightElementType::KEYWORD);
	highlighter.PrintText(" [OPTIONS] FILENAME [SQL]\n\n", PrintOutput::STDOUT, HighlightElementType::STRING_CONSTANT);
	highlighter.PrintText("FILENAME", PrintOutput::STDOUT, PrintColor::STANDARD, PrintIntensity::BOLD);
	PrintF(" is the name of a DuckDB database. A new database is created\n"
	       "if the file does not previously exist.\n\n");
	highlighter.PrintText("OPTIONS:\n", PrintOutput::STDOUT, PrintColor::STANDARD, PrintIntensity::BOLD);
	constexpr idx_t INITIAL_SPACING = 2;
	constexpr idx_t MIN_SPACING = 4;
	vector<PrintOptionInfo> print_options;
	for (idx_t c = 0; command_line_options[c].option; c++) {
		auto &option = command_line_options[c];
		PrintOptionInfo print_option;
		print_option.command_name = string(INITIAL_SPACING, ' ') + "-" + option.option;
		print_option.arguments = option.arguments;
		print_option.description = option.description;
		print_options.push_back(std::move(print_option));
	}
	idx_t max_lhs_length = 0;
	for (auto &option : print_options) {
		auto lhs_length = option.command_name.size() + option.arguments.size();
		if (!option.arguments.empty()) {
			lhs_length++;
		}
		if (lhs_length > max_lhs_length) {
			max_lhs_length = lhs_length;
		}
	}
	// print the options
	for (auto &option : print_options) {
		auto lhs_length = option.command_name.size() + option.arguments.size();
		if (!option.arguments.empty()) {
			lhs_length++;
		}
		idx_t padding = max_lhs_length - lhs_length + MIN_SPACING;
		string spaces(padding, ' ');
		highlighter.PrintText(option.command_name, PrintOutput::STDOUT, HighlightElementType::KEYWORD);
		if (!option.arguments.empty()) {
			highlighter.PrintText(" " + option.arguments, PrintOutput::STDOUT, HighlightElementType::STRING_CONSTANT);
		}
		PrintF("%s%s\n", spaces.c_str(), option.description.c_str());
	}
	ShellState::Exit(0);
}

} // namespace duckdb_shell
