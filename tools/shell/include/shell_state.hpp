//===----------------------------------------------------------------------===//
//                         DuckDB
//
// shell_state.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include <memory>
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/parser/sql_statement.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb.hpp"

namespace duckdb_shell {
using duckdb::make_uniq;
using duckdb::QueryResult;
using duckdb::string;
using duckdb::StringUtil;
using duckdb::unique_ptr;
using duckdb::vector;
struct ColumnarResult;
struct RowResult;
class ShellRenderer;
using duckdb::atomic;
using duckdb::ErrorData;
using duckdb::KeywordHelper;
using duckdb::optional_idx;
using duckdb::optional_ptr;
using duckdb::SQLIdentifier;
using duckdb::SQLString;
using duckdb::unordered_map;
using duckdb::unordered_set;
struct ShellState;
using duckdb::InternalException;
using duckdb::InvalidInputException;
using duckdb::to_string;
struct Prompt;
struct ShellProgressBar;
struct PagerState;
struct ShellTableInfo;
struct RenderingQueryResult;
enum class HighlightElementType : uint32_t;

using idx_t = uint64_t;

enum class RenderMode : uint32_t {
	LINE = 0,  /* One column per line.  Blank line between records */
	COLUMN,    /* One record per line in neat columns */
	LIST,      /* One record per line with a separator */
	SEMI,      /* Same as RenderMode::List but append ";" to each line */
	HTML,      /* Generate an XHTML table */
	INSERT,    /* Generate SQL "insert" statements */
	QUOTE,     /* Quote values as for SQL */
	TCL,       /* Generate ANSI-C or TCL quoted elements */
	CSV,       /* Quote strings, numbers are plain */
	EXPLAIN,   /* Like RenderMode::Column, but do not truncate data */
	DESCRIBE,  /* Special DESCRIBE Renderer */
	ASCII,     /* Use ASCII unit and record separators (0x1F/0x1E) */
	EQP,       /* Converts EXPLAIN QUERY PLAN output into a graph */
	JSON,      /* Output JSON */
	MARKDOWN,  /* Markdown formatting */
	TABLE,     /* MySQL-style table formatting */
	BOX,       /* Unicode box-drawing characters */
	LATEX,     /* Latex tabular formatting */
	TRASH,     /* Discard output */
	JSONLINES, /* Output JSON Lines */
	DUCKBOX    /* Unicode box drawing - using DuckDB's own renderer */
};

enum class PrintOutput { STDOUT, STDERR };

enum class InputMode { STANDARD, FILE, DUCKDB_RC };

enum class OutputCommand { OUTPUT, ONCE, EXCEL };

enum class LargeNumberRendering { NONE = 0, FOOTER = 1, ALL = 2, DEFAULT = 3 };

/*
** These are the allowed shellFlgs values
*/
enum class ShellFlags : uint32_t {
	SHFLG_Newlines = 0x00000010,     /* .dump --newline flag */
	SHFLG_CountChanges = 0x00000020, /* .changes setting */
	SHFLG_Echo = 0x00000040,         /* .echo or --echo setting */
	SHFLG_HeaderSet = 0x00000080     /* .header has been used */
};

enum class ShellOpenFlags { EXIT_ON_FAILURE, KEEP_ALIVE_ON_FAILURE };
enum class SuccessState { SUCCESS, FAILURE };
enum class OptionType { DEFAULT, ON, OFF };
enum class StartupText { ALL, VERSION, NONE };
enum class ReadLineVersion { LINENOISE, FALLBACK };
enum class PagerMode { PAGER_AUTOMATIC, PAGER_ON, PAGER_OFF };

enum class MetadataResult : uint8_t { SUCCESS = 0, FAIL = 1, EXIT = 2, PRINT_USAGE = 3 };
enum class HighlightMode : uint32_t { AUTOMATIC, MIXED_MODE, DARK_MODE, LIGHT_MODE };

// Deliberately not a std::exception: command error handlers must not consume an explicit exit.
struct ShellExitException {
	explicit ShellExitException(int exit_code) : exit_code(exit_code) {
	}
	int exit_code;
};

enum class ExecuteSQLSingleValueResult {
	SUCCESS,
	EXECUTION_ERROR,
	EMPTY_RESULT,
	MULTIPLE_ROWS,
	MULTIPLE_COLUMNS,
	NULL_RESULT
};

typedef MetadataResult (*metadata_command_t)(ShellState &state, const vector<string> &args);

struct CommandLineOption {
	const char *option;
	idx_t argument_count;
	const char *arguments;
	metadata_command_t pre_init_callback;
	metadata_command_t post_init_callback;
	const char *description;
	//! Whether one more argument is consumed when the next one is not an option itself
	bool optional_argument;
};

struct MetadataCommand {
	const char *command;
	idx_t argument_count;
	metadata_command_t callback;
	const char *usage;
	const char *description;
	idx_t match_size;
	const char *extra_description;
};

struct ShellColumnInfo {
	string column_name;
	string column_type;
	bool is_primary_key = false;
	bool is_not_null = false;
	bool is_unique = false;
	string default_value;
};

struct ShellTableInfo {
	string database_name;
	string schema_name;
	string table_name;
	optional_idx estimated_size;
	bool is_view = false;
	vector<ShellColumnInfo> columns;
};

enum class BailOnError { AUTOMATIC, BAIL_ON_ERROR, DONT_BAIL_ON_ERROR };
enum class AutoFormatMode { NO_AUTO_FORMAT, AUTO_FORMAT_COMPLETE_STATEMENTS };

/*
** State information about the database connection is contained in an
** instance of the following structure.
*/
struct ShellState {
public:
	unique_ptr<duckdb::DuckDB> db;            /* The database */
	unique_ptr<duckdb::Connection> conn;      /* The primary connection to the database */
	duckdb::DBConfig config;                  /* Config used for opening the database */
	uint8_t doXdgOpen = 0;                    /* Invoke start/open/xdg-open in output_reset() */
	int outCount = 0;                         /* Revert to stdout when reaching zero */
	int lineno = 0;                           /* Line number of last line read from in */
	FILE *in = nullptr;                       /* Read commands from this stream */
	FILE *out = nullptr;                      /* Write results here */
	int nErr = 0;                             /* Number of errors seen */
	RenderMode mode = RenderMode::LINE;       /* An output mode setting */
	RenderMode modePrior = RenderMode::LINE;  /* Saved mode */
	RenderMode cMode = RenderMode::LINE;      /* temporary output mode for the current query */
	RenderMode normalMode = RenderMode::LINE; /* Output mode before ".explain on" */
	bool showHeader = false;                  /* True to show column names in List or Column mode */
	uint32_t shellFlgs = 0;                   /* Various flags */
	uint32_t priorShFlgs = 0;                 /* Saved copy of flags */
	int64_t szMax = 0;                        /* --maxsize argument to .open */
	string zDestTable;                        /* Name of destination table when RenderMode::Insert */
	string zTempFile;                         /* Temporary file that might need deleting */
	string colSeparator;                      /* Column separator character for several modes */
	string rowSeparator;                      /* Row separator character for RenderMode::Ascii */
	string colSepPrior;                       /* Saved column separator */
	string rowSepPrior;                       /* Saved row separator */
	vector<int> colWidth;                     /* Requested width of each column in columnar modes */
	string nullValue;                         /* The text to print when a NULL comes back from the database */
	int columns = 0;                          /* Column-wise DuckBox rendering */
	string outfile;                           /* Filename for *out */
	string zDbFilename;                       /* name of the database file */
	FILE *pLog = nullptr;                     /* Write log output here */
	size_t max_rows = 0;                      /* The maximum number of rows to render in DuckBox mode */
	size_t max_width = 0;      /* The maximum number of characters to render horizontally in DuckBox mode */
	size_t max_bytes = 0;      /* The maximum number of bytes of rows to render in markdown mode (0 = all) */
	size_t max_cell_width = 0; /* The maximum number of characters to render per cell in markdown mode (0 = all) */
	//! The maximum number of rows to analyze in order to determine column widths in DuckBox mode
	idx_t max_analyze_rows = 0;
	//! Decimal separator (if any)
	char decimal_separator = '\0';
	//! Thousand separator (if any)
	char thousand_separator = '\0';
	//! When to use formatting of large numbers (in DuckBox mode)
	LargeNumberRendering large_number_rendering = LargeNumberRendering::DEFAULT;
	//! The command to execute when `-ui` is passed in
	string ui_command = "CALL start_ui()";
	//! The command to execute when `-serve` is passed in - `create_secret_if_not_exists` persists the
	//! token the server picks, so that `-connect` works without any configuration
	string serve_command = "CALL quack_serve(create_secret_if_not_exists=true{serve_secret|})";
	//! The command to execute when `-connect` is passed in - `{type}` is the database type given to
	//! `-connect`, and decides which extension the connection goes through
	string connect_command = "CONNECT '{type|quack}:'{connect_secret|}";
	//! Parameters used to expand placeholders in the serve/connect commands
	unordered_map<string, string> command_parameters;
	//! Whether the shell was launched as a client using `-connect` - Ctrl-D then exits instead of disconnecting
	bool started_as_client = false;
	idx_t last_changes = 0;
	idx_t total_changes = 0;
	bool readStdin = true;
	string initFile;
	bool run_init = true;
	unique_ptr<duckdb::QueryResult> last_result;
	bool last_result_referenced = false;
	//! Whether the last EXPLAIN ANALYZE tree folded any operators (so ".last" has a fuller tree to show)
	bool last_explain_hid_content = false;
	//! The widest rendered line of the last EXPLAIN tree, in display columns (used for the pager-width decision)
	idx_t last_explain_width = 0;
	//! If the following flag is set, then command execution stops at an error
	BailOnError bail = BailOnError::AUTOMATIC;
	//! Controls automatic SQL formatting before execution
	AutoFormatMode auto_format = AutoFormatMode::NO_AUTO_FORMAT;
	//! Table name when rendering a DESCRIBE statement
	string describe_table_name;

	/*
	** Treat stdin as an interactive input if the following variable
	** is true.  Otherwise, assume stdin is connected to a file or pipe.
	*/
	bool stdin_is_interactive = true;

	/*
	** On Windows systems we have to know if standard output is a console
	** in order to translate UTF-8 into MBCS.  The following variable is
	** true if translation is required.
	*/
	bool stdout_is_console = true;
	bool stderr_is_console = true;

	//! Whether to render for an AI coding agent: forced on/off from the command line, or auto-detected (DEFAULT)
	OptionType agent_mode = OptionType::DEFAULT;
	//! Whether agent mode is active (see DetectAgentMode)
	bool agent_mode_active = false;
	//! The name of the detected agent (e.g. "claude-code"), if any
	string agent_name;
	//! The environment variable that switched agent mode on (e.g. "CLAUDECODE"), empty when forced with -agent
	string agent_marker;
	//! Whether DUCKDB_AGENT_MODE forced agent mode on or off (no -agent / -no-agent was given)
	bool agent_mode_from_environment = false;
	//! Whether an output mode was given on the command line (-csv, -json, ...): detected agent mode then stays off
	bool output_mode_flag = false;
	//! Whether the exit hint (see PrintExitHint) went out already
	bool exit_hint_printed = false;

	//! True if an interrupt (Control-C) has been received.
	atomic<idx_t> seenInterrupt;
	//! Name of our program
	const char *program_name;

	//! Whether or not syntax highlighting is enabled
	bool highlighting_enabled = true;
	//! Whether or not we are running in safe mode
	bool safe_mode = false;
	//! Whether or not we are highlighting errors
	OptionType highlight_errors = OptionType::DEFAULT;
	//! Whether or not we are highlighting results
	OptionType highlight_results = OptionType::DEFAULT;
	//! Path to .duckdbrc file
	string duckdb_rc_path;
	//! Canonical paths of files currently being processed by .read
	unordered_set<string> active_read_files;
	//! Startup text to display
	StartupText startup_text = StartupText::ALL;
	//! Whether or not the loading resources message was displayed
	bool displayed_loading_resources_message = false;

	/*
	** Prompt strings. Initialized in main. Settable with
	**   .prompt main continue
	*/
	static constexpr idx_t MAX_PROMPT_SIZE = 20;
	unique_ptr<Prompt> main_prompt;
	//! Continuation prompt
	char continuePrompt[MAX_PROMPT_SIZE];
	//! Selected continuation prompt
	char continuePromptSelected[MAX_PROMPT_SIZE];
	//! Prompt showing there is more text available up
	char scrollUpPrompt[MAX_PROMPT_SIZE];
	//! Prompt showing there is more text available down
	char scrollDownPrompt[MAX_PROMPT_SIZE];
	//! Progress bar used to render the components that are displayed when query status / progress is rendered
	unique_ptr<ShellProgressBar> progress_bar;
	//! User-configured highlight elements
	duckdb::unordered_set<HighlightElementType> user_configured_elements;

#ifdef HAVE_LINENOISE
	ReadLineVersion rl_version = ReadLineVersion::LINENOISE;
#else
	ReadLineVersion rl_version = ReadLineVersion::FALLBACK;
#endif

	//! Whether or not to run the pager
	PagerMode pager_mode = PagerMode::PAGER_AUTOMATIC;
	//! The command to run when running the pager
	string pager_command;
	// In automatic mode, only show a pager when this row count is exceeded
	idx_t pager_min_rows = 50;
	//! Whether or not the pager is currently active
	bool pager_is_active = false;
	//! Shell highlighting mode
	HighlightMode highlight_mode = HighlightMode::AUTOMATIC;

public:
	ShellState();
	~ShellState();

	static ShellState &Get();
	static ShellState *&GetReference();

	void Initialize();
	void Destroy();
	void PushOutputMode();
	void PopOutputMode();
	void OutputCSV(const char *z, int bSep);
	string EscapeCString(const string &z);
	string GetSchemaLine(const string &str, const string &tail);
	string GetSchemaLineN(const string &str, idx_t n, const string &tail);
	bool SetOutputMode(const string &mode, const char *tbl_name);
	bool ImportData(const vector<string> &args);
	bool OpenDatabase(const vector<string> &args);
	bool SetOutputFile(const vector<string> &args, OutputCommand output_command);
	bool ReadFromFile(const string &file);
	bool DisplaySchemas(const vector<string> &args);
	MetadataResult DisplayEntries(const vector<string> &args, char type);
	MetadataResult DisplayTables(const vector<string> &args);
	MetadataResult DisplayManual(const vector<string> &args);
	void ShowConfiguration();
	void ClearInterrupt();

	[[noreturn]] static void Exit(int exit_code);
	static void IgnoreInterrupts();
	static idx_t RenderLength(const char *str, idx_t str_len);
	static idx_t RenderLength(duckdb::string_t str);
	static idx_t RenderLength(const string &str);
	static bool IsCharacter(char c);
	void SetBinaryMode();
	void SetTextMode();
	static idx_t StringLength(const char *z);
	void SetTableName(const char *zName);
	static void StaticPrint(PrintOutput output, const char *str, idx_t len);
	void Print(PrintOutput output, const char *str, idx_t len);
	void Print(PrintOutput output, const char *str);
	void Print(PrintOutput output, duckdb::string_t str);
	void Print(PrintOutput output, const string &str);
	void Print(const char *str, idx_t len);
	void Print(duckdb::string_t str);
	void Print(const char *str);
	void Print(const string &str);
	template <typename... ARGS>
	void PrintF(PrintOutput stream, const string &str, ARGS... params) {
		Print(stream, StringUtil::Format(str, params...));
	}
	template <typename... ARGS>
	void PrintF(const string &str, ARGS... params) {
		PrintF(PrintOutput::STDOUT, str, std::forward<ARGS>(params)...);
	}
	bool ColumnTypeIsInteger(const char *type);
	unique_ptr<ShellRenderer> GetRenderer();
	unique_ptr<ShellRenderer> GetRenderer(RenderMode mode);
	vector<string> TableColumnList(const char *zTab);
	SuccessState ExecuteStatement(unique_ptr<duckdb::SQLStatement> statement);
	static bool UseDescribeRenderMode(const duckdb::SQLStatement &stmt, string &describe_table_name);
	//! Route EXPLAIN ANALYZE output through the shell's direct-printing renderer when on an interactive console
	void SetupPrettyExplain(duckdb::SQLStatement &statement);
	void RenderTableMetadata(vector<ShellTableInfo> &result);

	void PrintDatabaseError(const string &zErr);
	//! Print an error, either with its location in the query (default) or as JSON (agent mode)
	void PrintDatabaseError(ErrorData error, const string &query = string());
	int RunInitialCommand(const char *sql, bool bail);
	//! Expand `{parameter|default}` placeholders in a command using the parameters set on the command line
	bool ExpandCommandParameters(const string &command, string &result);
	void AddError();

	SuccessState ExecuteSQL(const string &zSql);
	void RunSchemaDumpQuery(const string &zQuery);
	void RunTableDumpQuery(const string &zSelect);
	void OpenDB(ShellOpenFlags open_flags = ShellOpenFlags::EXIT_ON_FAILURE);

	void SetOrClearFlag(ShellFlags mFlag, const string &zArg);
	bool ShellHasFlag(ShellFlags flag) {
		return (shellFlgs & static_cast<uint32_t>(flag)) != 0;
	}

	void ShellSetFlag(ShellFlags flag) {
		shellFlgs |= static_cast<uint32_t>(flag);
	}

	void ShellClearFlag(ShellFlags flag) {
		shellFlgs &= ~static_cast<uint32_t>(flag);
	}
	void ResetOutput();
	bool ShouldUsePager(ShellRenderer &renderer, RenderingQueryResult &result);
	bool ShouldUsePager();
	bool ShouldUsePager(idx_t line_count);
	//! Whether to page a block of output based on whether it fits on the screen - either too tall (line_count vs the
	//! terminal height) or too wide (render_width vs the max render width). Used for tree output.
	bool ShouldUsePagerForSize(idx_t line_count, idx_t render_width);
	//! The terminal height in rows, or 0 when it cannot be determined
	idx_t GetScreenHeight();
	idx_t GetMaxRenderWidth() const;
	string GetSystemPager();
	unique_ptr<PagerState> SetupPager();
	static void StartPagerDisplay();
	static void FinishPagerDisplay();
	void ClearTempFile();
	void NewTempFile(const char *zSuffix);
	int DoMetaCommand(const string &zLine);
	idx_t PrintHelp(const char *zPattern);

	void ShellAddHistory(const char *line);
	int ShellLoadHistory(const char *path);
	int ShellSaveHistory(const char *path);
	int ShellSetHistoryMaxLength(idx_t max_length);
	char *OneInputLine(FILE *in, char *&zPrior, int isContinuation);

	int RunOneSqlLine(InputMode mode, char *zSql);
	string GetDefaultDuckDBRC();
	bool ProcessDuckDBRC(const char *file);
	bool ProcessFile(const string &file, InputMode input_mode = InputMode::FILE, bool default_duckdb_rc = false);
	int ProcessInput(InputMode mode);
	bool GetBailOnError(InputMode mode);
	static bool SQLIsComplete(const char *zSql);
	static bool IsSpace(char c);
	static bool IsDigit(char c);
	static int64_t StringToInt(const string &arg);
	bool StringToBool(const string &zArg);
	static void GenerateRandomBytes(int N, void *pBuf);
	static bool StringGlob(const char *zGlobPattern, const char *zString);
	static bool StringLike(const char *zPattern, const char *zStr, unsigned int esc);
	static void Sleep(idx_t ms);
	void PrintUsage();
	void DetectDarkLightMode();
	//! Decide whether agent mode is active, from the -agent/-no-agent flags or the environment (see agent_mode)
	void DetectAgentMode();
	//! Whether the environment marks the shell as being run by an AI coding agent, which one, and by which variable
	static bool DetectAgentEnvironment(string &agent_name, string &marker);
	//! Print how agent mode renders and the engine features an agent should know about; `startup` is the one-line
	//! form printed before anything runs
	void PrintAgentHelp(PrintOutput output, bool startup);
	//! On a failed run through a pipe with no agent detected and no -agent/-no-agent given: point at -agent
	void PrintExitHint(int rc);
	//! Print the planner's estimate of what a statement will read and return to stderr (agent mode)
	void PrintQueryEstimate(const string &sql, const duckdb::SQLStatement &statement);
#if defined(_WIN32) || defined(WIN32)
	static std::wstring Win32Utf8ToUnicode(const string &zText);
	static std::wstring Win32Utf8ToUnicode(const char *) = delete;
	static std::wstring Win32Utf8ToUnicode(char *) = delete;
	static string Win32UnicodeToUtf8(const std::wstring &zWideText);
	static string Win32MbcsToUtf8(const string &zText, bool useAnsi);
	static string Win32Utf8ToMbcs(const string &zText, bool useAnsi);
#endif
	optional_ptr<const CommandLineOption> FindCommandLineOption(const string &option, string &error_msg) const;
	optional_ptr<const MetadataCommand> FindMetadataCommand(const string &option, string &error_msg) const;
	static vector<string> GetMetadataCompletions(const char *zLine, idx_t nLine);

	//! Execute a SQL query
	// On fail - print the error and returns FAILURE
	SuccessState ExecuteQuery(const string &query);
	//! Execute a SQL query and extracts a single string value
	ExecuteSQLSingleValueResult ExecuteSQLSingleValue(const string &sql, string &result);
	ExecuteSQLSingleValueResult ExecuteSQLSingleValue(duckdb::Connection &con, const string &sql, string &result_value);
	//! Execute a SQL query and renders the result using the given renderer.
	//! On fail - prints the error and returns FAILURE
	SuccessState RenderQuery(ShellRenderer &renderer, const string &query, PagerMode pager_overwrite);
	SuccessState RenderQueryResult(ShellRenderer &renderer, duckdb::QueryResult &result,
	                               PagerMode pager_overwrite = PagerMode::PAGER_AUTOMATIC);
	SuccessState RenderQueryResult(ShellRenderer &renderer, duckdb::QueryResultStream<> &stream,
	                               PagerMode pager_overwrite = PagerMode::PAGER_AUTOMATIC);
	SuccessState RenderPreparedResult(ShellRenderer &renderer, RenderingQueryResult &render_result,
	                                  PagerMode pager_overwrite);
	bool HighlightErrors() const;
	bool HighlightResults() const;

	static MetadataResult SetNullValue(ShellState &state, const vector<string> &args);
	static MetadataResult SetSeparator(ShellState &state, const vector<string> &args);
	static MetadataResult EnableSafeMode(ShellState &state, const vector<string> &args);
	static MetadataResult ToggleTimer(ShellState &state, const vector<string> &args);
	SuccessState ChangeDirectory(const string &path);
	SuccessState ShowDatabases();
	void CloseOutputFile(FILE *file);
	FILE *OpenOutputFile(const char *zFile, int bTextMode);
	static void SetPrompt(char prompt[], const string &new_value);
	static string ModeToString(RenderMode mode);
	MetadataResult FormatSQL(string &sql);
	void HighlightSQL(string &sql);
	// Print a complete SQL statement directly to the output, applying syntax highlighting when appropriate
	void PrintSQL(const string &sql);
	string ReadFileContents(FILE *f);
	string ReadFileContents(const string &filename);
};

struct PagerState {
	explicit PagerState(ShellState &state, uint32_t win_console_cp_before_pager_p = 0)
	    : state(state), win_console_cp_before_pager(win_console_cp_before_pager_p) {
	}
	~PagerState();

	optional_ptr<ShellState> state;
	uint32_t win_console_cp_before_pager = 0;
};

} // namespace duckdb_shell
