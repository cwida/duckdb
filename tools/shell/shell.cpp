#if (defined(_WIN32) || defined(WIN32)) && !defined(_CRT_SECURE_NO_WARNINGS)
/* This needs to come before any includes for MSVC compiler */
#define _CRT_SECURE_NO_WARNINGS
#endif

/*
** Determine if we are dealing with WinRT, which provides only a subset of
** the full Win32 API.
*/
#if !defined(SQLITE_OS_WINRT)
#define SQLITE_OS_WINRT 0
#endif

/*
** Warning pragmas copied from msvc.h in the core.
*/
#if defined(_MSC_VER)
#pragma warning(disable : 4054)
#pragma warning(disable : 4055)
#pragma warning(disable : 4100)
#pragma warning(disable : 4127)
#pragma warning(disable : 4130)
#pragma warning(disable : 4152)
#pragma warning(disable : 4189)
#pragma warning(disable : 4206)
#pragma warning(disable : 4210)
#pragma warning(disable : 4232)
#pragma warning(disable : 4244)
#pragma warning(disable : 4305)
#pragma warning(disable : 4306)
#pragma warning(disable : 4702)
#pragma warning(disable : 4706)
#endif /* defined(_MSC_VER) */

/*
** Enable large-file support for fopen() and friends on unix.
*/
#ifndef SQLITE_DISABLE_LFS
#define _LARGE_FILE 1
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#define _LARGEFILE_SOURCE 1
#endif

#include <stdlib.h>
#include "duckdb/logging/log_storage.hpp"
#include "duckdb/logging/log_manager.hpp"
#include "duckdb/main/statement_iterator.hpp"
#include <string.h>
#include <stdio.h>
#include <assert.h>

#include "duckdb/common/time_point.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/parser/qualified_name.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/explain_statement.hpp"
#include "duckdb/parser/statement/multi_statement.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "shell_progress_bar.hpp"
#include "shell_prompt.hpp"
#include "highlighting.hpp"
#ifdef SHELL_INLINE_AUTOCOMPLETE
#include "autocomplete_extension.hpp"
#endif
#include "shell_extension.hpp"
#include <ctype.h>
#include <algorithm>

#if !defined(_WIN32) && !defined(WIN32)
#include <signal.h>
#if !defined(__RTP__) && !defined(_WRS_KERNEL)
#include <pwd.h>
#endif
#endif
#if (!defined(_WIN32) && !defined(WIN32)) || defined(__MINGW32__)
#include <unistd.h>
#include <dirent.h>
#endif
#if defined(__MINGW32__)
#define DIRENT dirent
#ifndef S_ISLNK
#define S_ISLNK(mode) (0)
#endif
#endif
#include <sys/types.h>
#include <sys/stat.h>

#ifdef HAVE_LINENOISE
#include "linenoise.h"
#endif
#include "terminal.hpp"

#include "duckdb.hpp"
#include "shell_renderer.hpp"
#include "shell_highlight.hpp"
#include "shell_manual.hpp"
#include "shell_state.hpp"
#include "duckdb/main/error_manager.hpp"
#include "duckdb/main/client_config.hpp"

using namespace duckdb_shell;

#if defined(_WIN32) || defined(WIN32)
#if SQLITE_OS_WINRT
#define SQLITE_OMIT_POPEN 1
#else
#include <io.h>
#include <fcntl.h>
#define isatty(h) _isatty(h)
#ifndef access
#define access(f, m) _access((f), (m))
#endif
#ifndef unlink
#define unlink _unlink
#endif
#ifndef strdup
#define strdup _strdup
#endif
#undef popen
#define popen _popen
#undef pclose
#define pclose _pclose
#endif
#else
/* Make sure isatty() has a prototype. */
extern int isatty(int);

#if !defined(__RTP__) && !defined(_WRS_KERNEL)
/* popen and pclose are not C89 functions and so are
** sometimes omitted from the <stdio.h> header */
extern FILE *popen(const char *, const char *);
extern int pclose(FILE *);
#else
#define SQLITE_OMIT_POPEN 1
#endif
#endif

#if defined(_WIN32_WCE)
/* Windows CE (arm-wince-mingw32ce-gcc) does not provide isatty()
 * thus we always assume that we have a console. That can be
 * overridden with the -batch command line option.
 */
#define isatty(x) 1
#endif

#if defined(_WIN32) || defined(WIN32)
#if SQLITE_OS_WINRT
#include <intrin.h>
#endif
#include <windows.h>

#endif

/* On Windows, we normally run with output mode of TEXT so that \n characters
** are automatically translated into \r\n.  However, this behavior needs
** to be disabled in some cases (ex: when generating CSV output and when
** rendering quoted strings that contain \n characters).  The following
** routines take care of that.
*/
#if (defined(_WIN32) || defined(WIN32)) && !SQLITE_OS_WINRT
static void setBinaryMode(FILE *file, int isOutput) {
	if (isOutput)
		fflush(file);
	_setmode(_fileno(file), _O_BINARY);
}
static void setTextMode(FILE *file, int isOutput) {
	if (isOutput)
		fflush(file);
	_setmode(_fileno(file), _O_TEXT);
}
#else
#define setBinaryMode(X, Y)
#define setTextMode(X, Y)
#endif

/* True if the timer is enabled */
static bool enableTimer = false;
/* Number of decimals printed for the real time */
static int timerDigits = 3;

#if !defined(_WIN32) && !defined(WIN32) && !defined(__minux)
#include <sys/time.h>
#include <sys/resource.h>

/* VxWorks does not support getrusage() as far as we can determine */
#if defined(_WRS_KERNEL) || defined(__RTP__)
struct rusage {
	struct timeval ru_utime; /* user CPU time used */
	struct timeval ru_stime; /* system CPU time used */
};
#define getrusage(A, B) memset(B, 0, sizeof(*B))
#endif

/* Saved resource information for the beginning of an operation */
static struct rusage sBegin;     /* CPU time at start */
static duckdb::TimePoint tBegin; /* Monotonic time at start */

/*
** Begin timing an operation
*/
static void beginTimer(void) {
	if (enableTimer) {
		getrusage(RUSAGE_SELF, &sBegin);
		tBegin = duckdb::TimePoint::Tick();
	}
}

/* Return the difference of two time_structs in seconds */
static double timeDiff(struct timeval *pStart, struct timeval *pEnd) {
	return (pEnd->tv_usec - pStart->tv_usec) * 0.000001 + (double)(pEnd->tv_sec - pStart->tv_sec);
}

/*
** Print the timing results.
*/
static void endTimer(void) {
	if (enableTimer) {
		struct rusage sEnd;
		getrusage(RUSAGE_SELF, &sEnd);
		printf("Run Time (s): real %.*f user %f sys %f\n", timerDigits, tBegin.ElapsedSeconds(),
		       timeDiff(&sBegin.ru_utime, &sEnd.ru_utime), timeDiff(&sBegin.ru_stime, &sEnd.ru_stime));
	}
}

#define BEGIN_TIMER beginTimer()
#define END_TIMER   endTimer()
#define HAS_TIMER   1

#elif (defined(_WIN32) || defined(WIN32))

/* Saved resource information for the beginning of an operation */
static HANDLE hProcess;
static FILETIME ftKernelBegin;
static FILETIME ftUserBegin;
static duckdb::TimePoint tBegin;
typedef BOOL(WINAPI *GETPROCTIMES)(HANDLE, LPFILETIME, LPFILETIME, LPFILETIME, LPFILETIME);
static GETPROCTIMES getProcessTimesAddr = NULL;

/*
** Check to see if we have timer support.  Return 1 if necessary
** support found (or found previously).
*/
static int hasTimer(void) {
	if (getProcessTimesAddr) {
		return 1;
	} else {
#if !SQLITE_OS_WINRT
		/* GetProcessTimes() isn't supported in WIN95 and some other Windows
		** versions. See if the version we are running on has it, and if it
		** does, save off a pointer to it and the current process handle.
		*/
		hProcess = GetCurrentProcess();
		if (hProcess) {
			HINSTANCE hinstLib = LoadLibrary(TEXT("Kernel32.dll"));
			if (NULL != hinstLib) {
				getProcessTimesAddr = (GETPROCTIMES)GetProcAddress(hinstLib, "GetProcessTimes");
				if (NULL != getProcessTimesAddr) {
					return 1;
				}
				FreeLibrary(hinstLib);
			}
		}
#endif
	}
	return 0;
}

/*
** Begin timing an operation
*/
static void beginTimer(void) {
	if (enableTimer && getProcessTimesAddr) {
		FILETIME ftCreation, ftExit;
		getProcessTimesAddr(hProcess, &ftCreation, &ftExit, &ftKernelBegin, &ftUserBegin);
		tBegin = duckdb::TimePoint::Tick();
	}
}

/* Return the difference of two FILETIME structs in seconds */
static double timeDiff(FILETIME *pStart, FILETIME *pEnd) {
	int64_t i64Start = *((int64_t *)pStart);
	int64_t i64End = *((int64_t *)pEnd);
	return (double)((i64End - i64Start) / 10000000.0);
}

/*
** Print the timing results.
*/
static void endTimer(void) {
	if (enableTimer && getProcessTimesAddr) {
		FILETIME ftCreation, ftExit, ftKernelEnd, ftUserEnd;
		getProcessTimesAddr(hProcess, &ftCreation, &ftExit, &ftKernelEnd, &ftUserEnd);
		printf("Run Time (s): real %.*f user %f sys %f\n", timerDigits, tBegin.ElapsedSeconds(),
		       timeDiff(&ftUserBegin, &ftUserEnd), timeDiff(&ftKernelBegin, &ftKernelEnd));
	}
}

#define BEGIN_TIMER beginTimer()
#define END_TIMER   endTimer()
#define HAS_TIMER   hasTimer()

#else
#define BEGIN_TIMER
#define END_TIMER
#define HAS_TIMER 0
#endif

/*
** Used to prevent warnings about unused parameters
*/
#define UNUSED_PARAMETER(x) (void)(x)

/*
** Number of elements in an array
*/
#define ArraySize(X) (int)(sizeof(X) / sizeof(X[0]))

bool ShellState::HighlightErrors() const {
	if (highlight_errors == OptionType::DEFAULT) {
		return stderr_is_console;
	}
	return highlight_errors == OptionType::ON;
}

bool ShellState::HighlightResults() const {
	if (highlight_results == OptionType::DEFAULT) {
		return stdout_is_console;
	}
	return highlight_results == OptionType::ON;
}

/* Used with ShellState::EvaluateSQL to indicate special result states */
const string EVAL_SQL_ERROR = "#ERROR#:";
const string EVAL_SQL_NOT_A_QUERY = "#NOT A QUERY#";
const string EVAL_SQL_NO_RESULT = "#NO RESULT#";
const string EVAL_SQL_TOO_MANY_ROWS = "#TOO MANY ROWS#";
const string EVAL_SQL_TOO_MANY_COLUMNS = "#TOO MANY COLUMNS#";
const string EVAL_SQL_NULL = "#NULL#";
const string EVAL_SQL_EMPTY = "#EMPTY#";

void ShellState::Print(PrintOutput output, const char *str, idx_t len) {
	if (seenInterrupt) {
		// no more printing after seeing an interrupt
		return;
	}

#if defined(_WIN32) || defined(WIN32)
	if ((stdout_is_console && (out == stdout || out == stderr)) && !pager_is_active) {
		// convert from utf8 to utf16
		string data_str = str ? string(str, len) : "";
		auto unicode_text = ShellState::Win32Utf8ToUnicode(data_str);
		auto out_handle = GetStdHandle(output == PrintOutput::STDOUT ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
		// use WriteConsoleW to write the unicode codepoints to the console
		WriteConsoleW(out_handle, unicode_text.c_str(), unicode_text.size(), NULL, NULL);
		return;
	}
#endif
	fwrite((const void *)str, len, 1, output == PrintOutput::STDOUT ? out : stderr);
}

void ShellState::Print(PrintOutput output, const char *str) {
	if (seenInterrupt) {
		// no more printing after seeing an interrupt
		return;
	}
#if defined(_WIN32) || defined(WIN32)
	Print(output, str, strlen(str));
#else
	fputs(str, output == PrintOutput::STDOUT ? out : stderr);
#endif
}

void ShellState::Print(PrintOutput output, const string &str) {
	Print(output, str.c_str(), str.size());
}

void ShellState::Print(PrintOutput output, duckdb::string_t str) {
	Print(output, str.GetData(), str.GetSize());
}

void ShellState::Print(const char *str, idx_t len) {
	Print(PrintOutput::STDOUT, str, len);
}

void ShellState::Print(const string &str) {
	Print(PrintOutput::STDOUT, str.c_str(), str.size());
}

void ShellState::Print(duckdb::string_t str) {
	Print(PrintOutput::STDOUT, str.GetData(), str.GetSize());
}

void ShellState::Print(const char *str) {
	Print(PrintOutput::STDOUT, str);
}

/* Indicate out-of-memory and exit. */
static void shell_out_of_memory(void) {
	fprintf(stderr, "Error: out of memory\n");
	ShellState::Exit(1);
}

ShellState::ShellState() : seenInterrupt(0), program_name("duckdb") {
	config.error_manager->AddCustomError(
	    duckdb::ErrorType::UNSIGNED_EXTENSION,
	    "Extension \"%s\" could not be loaded because its signature is either missing or invalid and unsigned "
	    "extensions are disabled by configuration.\nStart the shell with the -unsigned parameter to allow this "
	    "(e.g. duckdb -unsigned).");
	nullValue = "NULL";
	strcpy(continuePrompt, "  ");
	strcpy(continuePromptSelected, "  ");
	strcpy(scrollUpPrompt, "⇡ ");
	strcpy(scrollDownPrompt, "⇣ ");
}

ShellState::~ShellState() {
	Destroy();
	// Do not launch an external viewer while shutting down.
	doXdgOpen = 0;
	ResetOutput();
	ClearTempFile();
}

void ShellState::Destroy() {
	last_result.reset();
	conn.reset();
	db.reset();
}

bool ShellState::IsSpace(char c) {
	return duckdb::StringUtil::CharacterIsSpace(c);
}

bool ShellState::IsDigit(char c) {
	return isdigit(c);
}

PagerState::~PagerState() {
#if defined(_WIN32) || defined(WIN32)
	if (win_console_cp_before_pager > 0 && win_console_cp_before_pager != CP_UTF8) {
		SetConsoleCP(win_console_cp_before_pager);
	}
#endif
	if (state) {
		state->ResetOutput();
		ShellState::FinishPagerDisplay();
		state = nullptr;
	}
}

/*
** Compute a string length that is limited to what can be stored in
** lower 30 bits of a 32-bit signed integer.
*/
idx_t ShellState::StringLength(const char *z) {
	return strlen(z);
}

/*
** Return the length of a string in characters.
*/
bool ShellState::IsCharacter(char c) {
	return (c & 0xc0) != 0x80;
}

idx_t ShellState::RenderLength(const char *str, idx_t str_len) {
#ifdef HAVE_LINENOISE
	return linenoiseComputeRenderWidth(str, str_len);
#else
	idx_t n = 0;
	for (idx_t i = 0; i < str_len; i++) {
		if (IsCharacter(str[i])) {
			n++;
		}
	}
	return n;
#endif
}

idx_t ShellState::RenderLength(duckdb::string_t str) {
	return RenderLength(str.GetData(), str.GetSize());
}

idx_t ShellState::RenderLength(const string &str) {
	return RenderLength(str.c_str(), str.size());
}

int ShellState::RunInitialCommand(const char *sql, bool bail) {
	int rc = 0;
	if (sql[0] == '.') {
		rc = DoMetaCommand(sql);
		if (rc && bail) {
			return rc == 2 ? false : rc;
		}
	} else {
		string zErrMsg;
		BEGIN_TIMER;
		auto res = ExecuteSQL(sql);
		END_TIMER;
		if (res == SuccessState::FAILURE && bail) {
			return 1;
		}
	}
	return 0;
}

/*
** Return true if zFile does not exist or if it is not an ordinary file.
*/
#ifdef _WIN32
#define notNormalFile(X) 0
#else
static int notNormalFile(const char *zFile) {
	struct stat x;
	int rc;
	memset(&x, 0, sizeof(x));
	rc = stat(zFile, &x);
	return rc || !S_ISREG(x.st_mode);
}
#endif

/*
** This routine reads a line of text from FILE in, stores
** the text in memory obtained from malloc() and returns a pointer
** to the text.  NULL is returned at end of file, or if malloc()
** fails.
**
** If zLine is not NULL then it is a malloced buffer returned from
** a previous call to this routine that may be reused.
*/
static char *local_getline(char *&zLine, FILE *in) {
	idx_t nLine = zLine == 0 ? 0 : 100;
	idx_t n = 0;

	while (1) {
		if (n + 100 > nLine) {
			nLine = nLine * 2 + 100;
			auto new_line = (char *)realloc(zLine, nLine);
			if (!new_line) {
				shell_out_of_memory();
			}
			zLine = new_line;
		}
		if (fgets(&zLine[n], nLine - n, in) == 0) {
			if (n == 0) {
				free(zLine);
				zLine = nullptr;
				return 0;
			}
			zLine[n] = 0;
			break;
		}
		while (zLine[n])
			n++;
		if (n > 0 && zLine[n - 1] == '\n') {
			n--;
			if (n > 0 && zLine[n - 1] == '\r')
				n--;
			zLine[n] = 0;
			break;
		}
	}
	return zLine;
}

/*
** Retrieve a single line of input text.
**
** If in==0 then read from standard input and prompt before each line.
** If isContinuation is true, then a continuation prompt is appropriate.
** If isContinuation is zero, then the main prompt should be used.
**
** If zPrior is not NULL then it is a buffer from a prior call to this
** routine that can be reused.
**
** The result is stored in space obtained from malloc() and must either
** be freed by the caller or else passed back into this routine via the
** zPrior argument for reuse.
*/
char *ShellState::OneInputLine(FILE *in, char *&zPrior, int isContinuation) {
	if (in) {
		// use local_getline when reading from a file
		// don't print prompt in this scenario
		return local_getline(zPrior, in);
	}

#ifdef HAVE_LINENOISE
	if (rl_version == ReadLineVersion::LINENOISE) {
		// use linenoise
		string prompt_str;
		const char *prompt_text;
		if (!isContinuation) {
			prompt_str = main_prompt->GeneratePrompt(*this);
			prompt_text = prompt_str.c_str();
		} else {
			prompt_text = continuePrompt;
		}
		free(zPrior);
		zPrior = nullptr;
		return linenoise(prompt_text);
	}
#endif
	// using local_getline to read from stdin - print the prompt
	if (!isContinuation) {
		main_prompt->PrintPrompt(*this, PrintOutput::STDOUT);
	} else {
		Print(continuePrompt);
	}
	fflush(stdout);
	return local_getline(zPrior, stdin);
}

/*
** Return the value of a hexadecimal digit.  Return -1 if the input
** is not a hex digit.
*/
static int hexDigitValue(char c) {
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

/*
** Interpret zArg as an integer value, possibly with suffixes.
*/
int64_t ShellState::StringToInt(const string &arg) {
	int64_t v = 0;
	static const struct {
		const char *zSuffix;
		int iMult;
	} aMult[] = {
	    {"KiB", 1024}, {"MiB", 1024 * 1024}, {"GiB", 1024 * 1024 * 1024},
	    {"KB", 1000},  {"MB", 1000000},      {"GB", 1000000000},
	    {"K", 1000},   {"M", 1000000},       {"G", 1000000000},
	};
	int i;
	int isNeg = 0;
	auto zArg = arg.c_str();
	if (zArg[0] == '-') {
		isNeg = 1;
		zArg++;
	} else if (zArg[0] == '+') {
		zArg++;
	}
	if (zArg[0] == '0' && zArg[1] == 'x') {
		int x;
		zArg += 2;
		while ((x = hexDigitValue(zArg[0])) >= 0) {
			v = (v << 4) + x;
			zArg++;
		}
	} else {
		while (IsDigit(zArg[0])) {
			v = v * 10 + zArg[0] - '0';
			zArg++;
		}
	}
	for (i = 0; i < ArraySize(aMult); i++) {
		if (StringUtil::CIEquals(aMult[i].zSuffix, zArg)) {
			v *= aMult[i].iMult;
			break;
		}
	}
	return isNeg ? -v : v;
}

string ShellState::ModeToString(RenderMode mode) {
	switch (mode) {
	case RenderMode::LINE:
		return "line";
	case RenderMode::COLUMN:
		return "column";
	case RenderMode::LIST:
		return "list";
	case RenderMode::SEMI:
		return "semi";
	case RenderMode::HTML:
		return "html";
	case RenderMode::INSERT:
		return "insert";
	case RenderMode::QUOTE:
		return "quote";
	case RenderMode::TCL:
		return "tcl";
	case RenderMode::CSV:
		return "csv";
	case RenderMode::EXPLAIN:
		return "explain";
	case RenderMode::DESCRIBE:
		return "describe";
	case RenderMode::ASCII:
		return "ascii";
	case RenderMode::EQP:
		return "eqp";
	case RenderMode::JSON:
		return "json";
	case RenderMode::MARKDOWN:
		return "markdown";
	case RenderMode::TABLE:
		return "table";
	case RenderMode::BOX:
		return "box";
	case RenderMode::LATEX:
		return "latex";
	case RenderMode::TRASH:
		return "trash";
	case RenderMode::JSONLINES:
		return "jsonlines";
	case RenderMode::DUCKBOX:
		return "duckbox";
	}
	return "invalid";
}

/*
** These are the column/row/line separators used by the various
** import/export modes.
*/
#define SEP_Column "|"
#define SEP_Row    "\n"
#define SEP_Tab    "\t"
#define SEP_Space  " "
#define SEP_Comma  ","
#define SEP_CrLf   "\r\n"
#define SEP_Unit   "\x1F"
#define SEP_Record "\x1E"

/*
** Save or restore the current output mode
*/
void ShellState::PushOutputMode() {
	modePrior = mode;
	priorShFlgs = shellFlgs;
	colSepPrior = colSeparator;
	rowSepPrior = rowSeparator;
}

void ShellState::PopOutputMode() {
	mode = modePrior;
	shellFlgs = priorShFlgs;
	colSeparator = colSepPrior;
	rowSeparator = rowSepPrior;
}

/*
** Output the given string as a quoted according to C or TCL quoting rules.
*/
string ShellState::EscapeCString(const string &str) {
	string result = "\"";
	for (auto c : str) {
		if (c == '\\') {
			result += "\\\\";
		} else if (c == '"') {
			result += "\\\"";
		} else if (c == '\t') {
			result += "\\t";
		} else if (c == '\n') {
			result += "\\n";
		} else if (c == '\r') {
			result += "\\r";
		} else if (!isprint(c & 0xff)) {
			result += "\\";
			char buf[4];
			snprintf(buf, 4, "%03o", c & 0xFF);
			result += buf;
		} else {
			result += c;
		}
	}
	result += "\"";
	return result;
}

extern "C" {

/*
** This routine runs when the user presses Ctrl-C
*/
static void InterruptHandler(int NotUsed) {
	UNUSED_PARAMETER(NotUsed);
	auto &state = ShellState::Get();
	state.seenInterrupt++;
	if (state.seenInterrupt > 2) {
		exit(1);
	}
	if (state.conn) {
		state.conn->Interrupt();
	}
}
}

#if (defined(_WIN32) || defined(WIN32)) && !defined(_WIN32_WCE)
/*
** This routine runs for console events (e.g. Ctrl-C) on Win32
*/
static BOOL WINAPI ConsoleCtrlHandler(DWORD dwCtrlType /* One of the CTRL_*_EVENT constants */
) {
	if (dwCtrlType == CTRL_C_EVENT) {
		InterruptHandler(0);
		return TRUE;
	}
	return FALSE;
}
#endif

// Stop routing signals to ShellState before any of its owners are destroyed.
void ShellState::IgnoreInterrupts() {
#ifdef SIGINT
	signal(SIGINT, SIG_IGN);
#elif (defined(_WIN32) || defined(WIN32)) && !defined(_WIN32_WCE)
	SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
#endif
#if !defined(_WIN32) && !defined(WIN32)
	signal(SIGPIPE, SIG_IGN);
#endif
}

void ShellState::Exit(int exit_code) {
	throw ShellExitException(exit_code);
}

void ShellState::ClearInterrupt() {
	seenInterrupt = 0;
	if (conn) {
		conn->context->ClearInterrupt();
	}
}

string ShellState::GetSchemaLine(const string &str, const string &tail) {
	return str + tail;
}

string ShellState::GetSchemaLineN(const string &str, idx_t n, const string &tail) {
	if (str.size() > n) {
		return GetSchemaLine(str.substr(0, n), tail);
	}
	return GetSchemaLine(str, tail);
}

void ShellState::SetBinaryMode() {
	setBinaryMode(out, 1);
}

void ShellState::SetTextMode() {
	setTextMode(out, 1);
}

SuccessState ShellState::RenderQuery(ShellRenderer &renderer, const string &query, PagerMode pager_overwrite) {
	auto &con = *conn;
	auto result = con.Query(query);
	if (result->HasError()) {
		PrintDatabaseError(result->GetError());
		return SuccessState::FAILURE;
	}
	return RenderQueryResult(renderer, *result, pager_overwrite);
}

/*
** Set the destination table field of the ShellState structure to
** the name of the table given.  Escape any quote characters in the
** table name.
*/
void ShellState::SetTableName(const char *zName) {
	zDestTable = zName ? StringUtil::Format("%s", SQLIdentifier(zName)) : string();
}

/*
** Execute a query statement that will generate SQL output.  Print
** the result columns, comma-separated, on a line and then add a
** semicolon terminator to the end of that line.
**
** If the number of columns is 1 and that column contains text "--"
** then write the semicolon on a separate line.  That way, if a
** "--" comment occurs at the end of the statement, the comment
** won't consume the semicolon terminator.
*/
void ShellState::RunTableDumpQuery(const string &zSelect) {
	auto &con = *conn;
	auto result = con.Query(zSelect);
	if (result->HasError()) {
		PrintF("/**** ERROR: %s *****/\n", result->GetError().c_str());
		AddError();
		return;
	}
	for (auto &row : *result) {
		auto zStr = row.GetValue<string>(0);
		PrintSQL(zStr);
		auto z = zStr.c_str();
		if (!z) {
			z = "";
		}
		while (z[0] && (z[0] != '-' || z[1] != '-')) {
			z++;
		}
		if (z[0]) {
			PrintF("\n;\n");
		} else {
			PrintF(";\n");
		}
	}
}

bool ShellState::ColumnTypeIsInteger(const char *type) {
	if (!type) {
		return false;
	}
	if (strcmp(type, "TINYINT") == 0) {
		return true;
	}
	if (strcmp(type, "SMALLINT") == 0) {
		return true;
	}
	if (strcmp(type, "INTEGER") == 0) {
		return true;
	}
	if (strcmp(type, "BIGINT") == 0) {
		return true;
	}
	if (strcmp(type, "FLOAT") == 0) {
		return true;
	}
	if (strcmp(type, "DOUBLE") == 0) {
		return true;
	}
	if (strcmp(type, "DECIMAL") == 0) {
		return true;
	}
	return false;
}

ShellState *&ShellState::GetReference() {
	// NOTE: this is a raw pointer to avoid the ShellState from being automatically destroyed if the CLI exits
	// Destroying a DuckDB database during exit-time destruction can lead to odd behavior due to
	// the static-destructor order not being defined, in particular when interacting with extensions that have statics
	static ShellState *reference = new ShellState();
	return reference;
}

ShellState &ShellState::Get() {
	return *GetReference();
}

static bool ResultIsDescribeShaped(const duckdb::QueryResult &result) {
	// The describe renderer reads the fixed column layout produced by DESCRIBE / a table describe (column_name,
	// column_type, null, key, default, extra). Anything else - e.g. a setting value from a bareword "SHOW name" - is a
	// regular result that must not be rendered in describe mode.
	auto &names = result.GetNames();
	return names.size() == 6 && names[0] == "column_name" && names[1] == "column_type";
}

SuccessState ShellState::ExecuteStatement(unique_ptr<duckdb::SQLStatement> statement) {
	if (statement->has_anonymous_parameters) {
		PrintDatabaseError("Prepared statement parameters cannot be used directly\nTo use prepared "
		                   "statement parameters, use PREPARE to prepare a statement, followed by EXECUTE");
		return SuccessState::FAILURE;
	}
	auto &con = *conn;
	auto renderer = GetRenderer();
	unique_ptr<duckdb::QueryResult> result;
	unique_ptr<duckdb::QueryResultStream<>> stream;
	const bool render_materialized = renderer->RequireMaterializedResult();
	if (render_materialized) {
		// we need to materialize the result prior to rendering
		result = con.Query(std::move(statement), duckdb::ChunkFormat::BufferManaged());
	} else {
		result = con.Submit(std::move(statement));
	}
	auto &res = *result;
	if (res.HasError()) {
		PrintDatabaseError(res.GetError());
		return SuccessState::FAILURE;
	}
	// A renderer stops at its row cap and a closed pipe stops the shell outright, so a stream is never read to its end
	// The result's type, so that a PRAGMA the engine reparses into a SELECT streams as that SELECT
	const bool render_streaming = !render_materialized && duckdb::StreamsByDefault(res.GetStatementType(), res, false);
	if (!render_materialized && !render_streaming) {
		// the statement is not rendered row by row, but its side effects must still happen
		res.Complete();
		if (res.HasError()) {
			PrintDatabaseError(res.GetError());
			return SuccessState::FAILURE;
		}
	}
	// Known by now: a statement whose properties are known only once it runs is never streamed
	auto &properties = res.GetStatementProperties();
	if (properties.return_type == duckdb::StatementReturnType::CHANGED_ROWS) {
		auto result_chunk = res.Fetch();
		if (result_chunk && result_chunk->size() == 1) {
			// update total changes
			auto row_changes = result_chunk->GetValue(0, 0);
			if (!row_changes.IsNull()) {
				auto cast_row_changes = row_changes.DefaultTryCastAs(duckdb::LogicalType::BIGINT);
				if (cast_row_changes) {
					last_changes = cast_row_changes->GetValue<int64_t>();
					total_changes += last_changes;
				}
			}
		}
	}
	if (properties.return_type != duckdb::StatementReturnType::QUERY_RESULT) {
		// only SELECT statements return results that need to be rendered
		return SuccessState::SUCCESS;
	}
	if (render_streaming) {
		stream = duckdb::make_uniq<duckdb::QueryResultStream<>>(std::move(result));
	} else {
		last_result = std::move(result);
	}
	// A bareword "SHOW name" is optimistically routed to the describe renderer, but it may have resolved to a setting
	// value rather than a table describe. Only a describe-shaped result can be rendered in describe mode - fall back to
	// the default rendering otherwise.
	if (cMode == RenderMode::DESCRIBE && !ResultIsDescribeShaped(res)) {
		cMode = mode;
		renderer = GetRenderer();
	}
	// analyze the query result so we know how long/wide the result will be
	auto render_state = stream ? RenderQueryResult(*renderer, *stream) : RenderQueryResult(*renderer, res);
	if (stream && stream->HasError()) {
		// the query failed after it started streaming rows (e.g. a division by zero, or max_execution_time)
		PrintDatabaseError(stream->GetError());
		return SuccessState::FAILURE;
	}
	return render_state;
}

/*
** Execute a statement or set of statements.  Print
** any result rows/columns depending on the current mode
** set via the supplied callback.
*/
void ShellState::SetupPrettyExplain(duckdb::SQLStatement &statement) {
	auto &explain = statement.Cast<duckdb::ExplainStatement>();
	if (explain.explain_type == duckdb::ExplainType::EXPLAIN_SQL) {
		return;
	}
	if (explain.format != duckdb::ProfilerPrintFormat::Default()) {
		// the user explicitly requested an output format (e.g. EXPLAIN (FORMAT json))
		return;
	}
	if (explain.explain_type == duckdb::ExplainType::EXPLAIN_ANALYZE) {
		auto &profiler_format = duckdb::ClientConfig::GetConfig(*conn->context).profiler_print_format;
		if (profiler_format != "query_tree" && profiler_format != "no_output") {
			// a custom profiler output format is configured - respect it
			return;
		}
	}
	if (agent_mode_active) {
		// one operator per line - cheaper to read than the box-drawing tree, and easier to parse than JSON/YAML
		explain.format = duckdb::ProfilerPrintFormat("compact");
		return;
	}
	// default to the full plan; only fold low-impact operators in an interactive console session, where the user
	// can type ".last" to expand the tree again (batch/redirected output has no such affordance)
	if (stdin_is_interactive && stdout_is_console) {
		duckdb::ClientConfig::GetConfig(*conn->context).profiling_renderer_settings["expand_all"] =
		    duckdb::Value::BOOLEAN(false);
	}
	if (!stdout_is_console) {
		// only pretty-print to an interactive console - redirected output keeps the plain plan as a result value
		return;
	}
	// render the plan as a highlighted string (see RegisterProfilerHighlighting)
	explain.format = duckdb::ProfilerPrintFormat("shell_explain_printer");
}

//! The statement whose rows the shell renders. Of a statement the engine expands into several, only the last can
//! return rows
static duckdb::SQLStatement &RenderedStatement(duckdb::SQLStatement &statement) {
	if (statement.type == duckdb::StatementType::MULTI_STATEMENT) {
		auto &multi = statement.Cast<duckdb::MultiStatement>();
		if (!multi.statements.empty()) {
			return *multi.statements.back();
		}
	}
	return statement;
}

SuccessState ShellState::ExecuteSQL(const string &zSql) {
	auto &con = *conn;
	try {
		auto iterator = con.context->IterateStatements(zSql);
		while (iterator.Peek()) {
			auto statement = iterator.GetStatementForExecution();
			auto &statement_query = statement->query.empty() ? zSql : statement->query;
			idx_t start_pos = statement->stmt_location.offset;
			idx_t len = statement->stmt_location.length;
			while (len > 0 && IsSpace(statement_query[start_pos])) {
				start_pos++;
				len--;
			}
			auto zStmtSql = statement_query.substr(start_pos, len);

			/* echo the sql statement if echo on */
			if (ShellHasFlag(ShellFlags::SHFLG_Echo)) {
				PrintF("%s\n", !zStmtSql.empty() ? zStmtSql.c_str() : zSql.c_str());
			}

			cMode = mode;
			auto &rendered = RenderedStatement(*statement);
			if (rendered.type == duckdb::StatementType::EXPLAIN_STATEMENT) {
				cMode = RenderMode::EXPLAIN;
				SetupPrettyExplain(rendered);
			}
			if (mode == RenderMode::DUCKBOX && UseDescribeRenderMode(rendered, describe_table_name)) {
				cMode = RenderMode::DESCRIBE;
			}

			if (agent_mode_active) {
				PrintQueryEstimate(zStmtSql, *statement);
			}

			// Reset before bind; the `_` replacement scan sets it to true if it fires.
			last_result_referenced = false;
			auto rc = ExecuteStatement(std::move(statement));
			if (agent_mode_active) {
				// keep stdout in step with the stderr progress/estimate lines when both go to the same pipe
				fflush(out);
			}
			if (rc != SuccessState::SUCCESS) {
				return rc;
			}
		} /* end while */
	} catch (std::exception &ex) {
		PrintDatabaseError(duckdb::ErrorData(ex), zSql);
		return SuccessState::FAILURE;
	}
	return SuccessState::SUCCESS;
}

/*
** Return a list of pointers to strings which are the names of all
** columns in table zTab.   The memory to hold the names is dynamically
** allocated and must be released by the caller using a subsequent call
** to freeColumnList().
**
** The azCol[0] entry is usually NULL.  However, if zTab contains a rowid
** value that needs to be preserved, then azCol[0] is filled in with the
** name of the rowid column.
**
** The first regular column in the table is azCol[1].  The list is terminated
** by an entry with azCol[i]==0.
*/
vector<string> ShellState::TableColumnList(const char *zTab) {
	vector<string> result;

	auto zSql = StringUtil::Format("PRAGMA table_info=%s", SQLString(zTab));
	auto &con = *conn;
	auto query_result = con.Query(zSql);
	if (query_result->HasError()) {
		return result;
	}
	for (auto &row : *query_result) {
		result.push_back(row.GetValue<string>(1));
	}
	return result;
}

/*
** Lookup the schema for a table using the information schema.
*/
static string getTableSchema(duckdb::Connection &con, const char *zTable) {
	string zSchema;
	auto zSql = StringUtil::Format("SELECT table_schema FROM information_schema.tables "
	                               "WHERE table_name = %s AND table_type='BASE TABLE' "
	                               "ORDER BY (table_schema='main') DESC LIMIT 1",
	                               SQLString(zTable));

	auto query_result = con.Query(zSql);
	if (query_result->HasError()) {
		return zSchema;
	}
	for (auto &row : *query_result) {
		zSchema = row.GetValue<string>(0);
	}
	return zSchema;
}

/*
** Build a qualified name: schema.table
*/
static string buildQualifiedName(const char *zSchema, const char *zTable) {
	return StringUtil::Format("%s.%s", SQLIdentifier(zSchema), SQLIdentifier(zTable));
}

void ShellState::AddError() {
	nErr++;
}
/*
** Run zQuery.  Use dump_callback() as the callback routine so that
** the contents of the query are output as SQL statements.
**
** If we get a SQLITE_CORRUPT error, rerun the query after appending
** "ORDER BY rowid DESC" to the end.
*/
void ShellState::RunSchemaDumpQuery(const string &zQuery) {
	auto &con = *conn;
	auto result = con.Query(zQuery);
	for (auto &row : *result) {
		auto zTable = row.GetValue<string>(0);
		auto zType = row.GetValue<string>(1);
		auto zSql = row.GetValue<string>(2);

		// print sql
		PrintSQL(GetSchemaLine(zSql, ";\n"));
		if (zType == "table") {
			// dump table contents
			string sSelect;
			string sTable;

			auto zSchema = getTableSchema(con, zTable.c_str());
			auto zQualifiedName = buildQualifiedName(zSchema.c_str(), zTable.c_str());

			auto table_columns = TableColumnList(zQualifiedName.c_str());
			if (table_columns.empty()) {
				AddError();
				break;
			}

			if (!zSchema.empty()) {
				sTable += zQualifiedName;
			} else {
				sTable += StringUtil::Format("%s", SQLIdentifier(zTable));
			}

			/* Build an appropriate SELECT statement */
			sSelect += "SELECT ";
			for (idx_t i = 0; i < table_columns.size(); i++) {
				if (i > 0) {
					sSelect += ", ";
				}
				sSelect += StringUtil::Format("%s", SQLIdentifier(table_columns[i]));
			}
			sSelect += " FROM ";
			sSelect += zQualifiedName;

			auto savedDestTable = zDestTable;
			auto savedMode = mode;
			zDestTable = sTable;
			mode = cMode = RenderMode::INSERT;
			auto res = ExecuteSQL(sSelect);
			zDestTable = savedDestTable;
			mode = savedMode;
			if (res != SuccessState::SUCCESS) {
				AddError();
			}
		}
	}
}

SuccessState ShellState::ExecuteQuery(const string &query) {
	auto &con = *conn;
	auto res = con.Query(query);
	if (res->HasError()) {
		PrintF(PrintOutput::STDERR, "Failed to execute query \"%s\": %s\n", query.c_str(), res->GetError().c_str());
		return SuccessState::FAILURE;
	}
	return SuccessState::SUCCESS;
}

unique_ptr<duckdb::ProgressBarDisplay> CreateProgressBar() {
	return make_uniq<ShellProgressBarDisplay>();
}

unique_ptr<duckdb::ProgressBarDisplay> CreateAgentProgressBar() {
	return make_uniq<AgentProgressBarDisplay>();
}

static void RegisterShellLogger(duckdb::DuckDB &db, duckdb::shared_ptr<duckdb::LogStorage> storage_ptr) {
	auto *db_instance = db.instance.get();
	auto &log_manager = db_instance->GetLogManager();
	log_manager.RegisterLogStorage("shell_log_storage", storage_ptr);
	log_manager.SetLogStorage(*db_instance, "shell_log_storage");
	log_manager.SetEnableLogging(db_instance);
	log_manager.SetLogLevel(duckdb::LogLevel::LOG_WARNING);
}

void ShellState::OpenDB(ShellOpenFlags flags) {
	// log storage to stdout
	auto std_out_log_storage = duckdb::make_shared_ptr<ShellLogStorage>(*this);
	duckdb::shared_ptr<duckdb::LogStorage> storage_ptr = std_out_log_storage;

	if (!db) {
		try {
			db = make_uniq<duckdb::DuckDB>(zDbFilename.c_str(), &config);
			RegisterShellLogger(*db, storage_ptr);
			conn = make_uniq<duckdb::Connection>(*db);
		} catch (std::exception &ex) {
			PrintDatabaseError(duckdb::ErrorData(ex));
			if (flags == ShellOpenFlags::KEEP_ALIVE_ON_FAILURE) {
				db = make_uniq<duckdb::DuckDB>(":memory:", &config);
				RegisterShellLogger(*db, storage_ptr);
				conn = make_uniq<duckdb::Connection>(*db);
			} else {
				ShellState::Exit(1);
			}
		}
		auto &client_config = duckdb::ClientConfig::GetConfig(*conn->context);
		if (stdout_is_console) {
			client_config.display_create_func = CreateProgressBar;
		} else if (agent_mode_active) {
			// no terminal to draw a progress bar on - print periodic progress lines to stderr instead
			client_config.display_create_func = CreateAgentProgressBar;
		}
#ifdef SHELL_INLINE_AUTOCOMPLETE
		db->LoadStaticExtension<duckdb::AutocompleteExtension>();
#endif
		db->LoadStaticExtension<duckdb::ShellExtension>();
		// bare expressions / table names as statements - only in the CLI, so it is opt-in grammar
		// appended rather than SET, so grammar extensions that activated themselves on this connection stay active
		ExecuteQuery("CALL add_grammar_extension('shell')");
		if (stdout_is_console || agent_mode_active) {
			ExecuteQuery("PRAGMA enable_progress_bar");
			ExecuteQuery("PRAGMA enable_print_progress_bar");
		}
		if (agent_mode_active) {
			// structured errors: the engine renders them as JSON instead of a LINE/caret block
			ExecuteQuery("SET errors_as_json = true");
		}
		if (safe_mode) {
			ExecuteQuery("SET enable_external_access=false");
			ExecuteQuery("SET lock_configuration=true");
		}
	}
}

/*
** Do C-language style dequoting.
**
**    \a    -> alarm
**    \b    -> backspace
**    \t    -> tab
**    \n    -> newline
**    \v    -> vertical tab
**    \f    -> form feed
**    \r    -> carriage return
**    \s    -> space
**    \"    -> "
**    \'    -> '
**    \\    -> backslash
**    \NNN  -> ascii character NNN in octal
*/
static string resolve_backslashes(const string &z) {
	string result;
	for (idx_t pos = 0; pos < z.size(); pos++) {
		auto c = z[pos];
		if (c == '\\' && pos + 1 < z.size()) {
			c = z[++pos];
			if (c == 'a') {
				c = '\a';
			} else if (c == 'b') {
				c = '\b';
			} else if (c == 't') {
				c = '\t';
			} else if (c == 'n') {
				c = '\n';
			} else if (c == 'v') {
				c = '\v';
			} else if (c == 'f') {
				c = '\f';
			} else if (c == 'r') {
				c = '\r';
			} else if (c == '"') {
				c = '"';
			} else if (c == '\'') {
				c = '\'';
			} else if (c == '\\') {
				c = '\\';
			} else if (c >= '0' && c <= '7') {
				c -= '0';
				if (pos + 1 < z.size() && z[pos + 1] >= '0' && z[pos + 1] <= '7') {
					pos++;
					c = (c << 3) + z[pos] - '0';
					if (pos + 1 < z.size() && z[pos + 1] >= '0' && z[pos + 1] <= '7') {
						pos++;
						c = (c << 3) + z[pos] - '0';
					}
				}
			}
		}
		result += c;
	}
	return result;
}

/*
** Interpret zArg as either an integer or a boolean value.  Return 1 or 0
** for TRUE and FALSE.  Return the integer value if appropriate.
*/
bool ShellState::StringToBool(const string &zArg) {
	idx_t i;
	if (zArg[0] == '0' && zArg[1] == 'x') {
		for (i = 2; hexDigitValue(zArg[i]) >= 0; i++) {
		}
	} else {
		for (i = 0; zArg[i] >= '0' && zArg[i] <= '9'; i++) {
		}
	}
	if (i > 0 && zArg[i] == 0) {
		return bool(ShellState::StringToInt(zArg) & 0xffffffff);
	}
	if (StringUtil::CIEquals(zArg, "on") || StringUtil::CIEquals(zArg, "yes")) {
		return true;
	}
	if (StringUtil::CIEquals(zArg, "off") || StringUtil::CIEquals(zArg, "no")) {
		return false;
	}
	PrintF(PrintOutput::STDERR, "ERROR: Not a boolean value: \"%s\". Assuming \"no\".\n", zArg.c_str());
	return false;
}

/*
** Set or clear a shell flag according to a boolean value.
*/
void ShellState::SetOrClearFlag(ShellFlags mFlag, const string &zArg) {
	if (StringToBool(zArg)) {
		ShellSetFlag(mFlag);
	} else {
		ShellClearFlag(mFlag);
	}
}

/*
** Close an output file, assuming it is not stderr or stdout
*/
void ShellState::CloseOutputFile(FILE *f) {
	if (f && f != stdout && f != stderr) {
		fclose(f);
	}
}

/*
** Try to open an output file.   The names "stdout" and "stderr" are
** recognized and do the right thing.  NULL is returned if the output
** filename is "off".
*/
FILE *ShellState::OpenOutputFile(const char *zFile, int bTextMode) {
	FILE *f = nullptr;
	if (strcmp(zFile, "stdout") == 0) {
		f = stdout;
	} else if (strcmp(zFile, "stderr") == 0) {
		f = stderr;
	} else if (strcmp(zFile, "off") == 0) {
		f = 0;
	} else {
		const string expanded_path = duckdb::FileSystem::ExpandPath(zFile, /*opener=*/nullptr);
		f = fopen(expanded_path.c_str(), bTextMode ? "w" : "wb");
		if (f == 0) {
			PrintF(PrintOutput::STDERR, "Error: cannot open \"%s\"\n", zFile);
		}
	}
	return f;
}

string ShellState::GetSystemPager() {
	const char *duckdb_pager = getenv("DUCKDB_PAGER");

	// Try DUCKDB_PAGER first (highest priority for env vars)
	if (duckdb_pager && strlen(duckdb_pager) > 0) {
		return duckdb_pager;
	}

	// Try PAGER next
	const char *pager = getenv("PAGER");
	if (pager && strlen(pager) > 0) {
		return pager;
	}

	// No valid pager environment variable set, use platform default
#if defined(_WIN32) || defined(WIN32)
	// On Windows, use 'more' as default pager
	return "more";
#else
	// On other systems, use 'less' as default pager
	return "less -SRX";
#endif
}

bool ShellState::ShouldUsePager() {
	if (safe_mode) {
		// the pager runs an external command, which is not allowed in -safe mode
		return false;
	}
	if (out != stdout || !stdout_is_console || !outfile.empty() || !stdin_is_interactive) {
		// if we have an outfile specified, or we are in non-interactive/batch mode, don't use the pager
		return false;
	}
	// setup a pager for output
	if (pager_mode == PagerMode::PAGER_OFF) {
		return false;
	}
	if (pager_command.empty()) {
		pager_command = GetSystemPager();
		if (pager_command.empty()) {
			Print(PrintOutput::STDERR, "Warning: No pager configured. Set DUCKDB_PAGER or PAGER environment variable\n"
			                           "or supply a command like `.pager 'less -SR'` or `.pager 'pspg --csv'`.\n");
			return false;
		}
	}
	return true;
}

bool ShellState::ShouldUsePager(idx_t line_count) {
	if (!ShouldUsePager()) {
		return false;
	}
	if (pager_mode == PagerMode::PAGER_AUTOMATIC) {
		if (line_count < pager_min_rows) {
			return false;
		}
	}
	return true;
}

idx_t ShellState::GetScreenHeight() {
	auto size = duckdb::Terminal::GetTerminalSize();
	return size.ws_row > 0 ? idx_t(size.ws_row) : 0;
}

bool ShellState::ShouldUsePagerForSize(idx_t line_count, idx_t render_width) {
	if (!ShouldUsePager()) {
		return false;
	}
	if (pager_mode != PagerMode::PAGER_AUTOMATIC) {
		// PAGER_ON (PAGER_OFF was already rejected by ShouldUsePager())
		return true;
	}
	// in automatic mode we page when the output does not fit on the screen - either too tall or too wide
	idx_t screen_rows = GetScreenHeight();
	idx_t row_threshold = screen_rows > 0 ? screen_rows : pager_min_rows;
	if (line_count >= row_threshold) {
		return true;
	}
	if (render_width > GetMaxRenderWidth()) {
		return true;
	}
	return false;
}

bool ShellState::ShouldUsePager(ShellRenderer &renderer, RenderingQueryResult &result) {
	if (!ShouldUsePager()) {
		return false;
	}
	return renderer.ShouldUsePager(result, pager_mode);
}

void ShellState::StartPagerDisplay() {
#if !defined(_WIN32) && !defined(WIN32)
	// turn sigpipe trap into an interrupt while displaying the pager
	// this allows us to interrupt display after the pager is exited by the user
	signal(SIGPIPE, InterruptHandler);
#endif
}

void ShellState::FinishPagerDisplay() {
	ShellState::Get().pager_is_active = false;
#if !defined(_WIN32) && !defined(WIN32)
	// enable sigpipe trap again after finishing the display
	signal(SIGPIPE, SIG_DFL);
#endif
}

unique_ptr<PagerState> ShellState::SetupPager() {
	uint32_t win_console_cp_before_pager = 0;
#if defined(_WIN32) || defined(WIN32)
	if (pager_command == "more") { // UTF-8 mode must be used with "more" pager
		win_console_cp_before_pager = GetConsoleCP();
		if (win_console_cp_before_pager > 0 && win_console_cp_before_pager != CP_UTF8) {
			SetConsoleCP(CP_UTF8);
		}
	}
#endif
	StartPagerDisplay();
	auto pager_out = popen(pager_command.c_str(), "w");
	if (!pager_out) {
		FinishPagerDisplay();
		PrintF(PrintOutput::STDERR, "Error: Failed to start pager process: %s. Output will be sent to stdout.\n",
		       strerror(errno));
		return nullptr;
	}
	pager_is_active = true;
	out = pager_out;
	outfile = "|" + pager_command;
	return make_uniq<PagerState>(*this, win_console_cp_before_pager);
}
/*
** Change the output file back to stdout.
**
** If the p->doXdgOpen flag is set, that means the output was being
** redirected to a temporary file named by p->zTempFile.  In that case,
** launch start/open/xdg-open on that temporary file.
*/
void ShellState::ResetOutput() {
	if (outfile.size() > 1 && outfile[0] == '|') {
#ifndef SQLITE_OMIT_POPEN
		pclose(out);
#endif
	} else {
		CloseOutputFile(out);
#ifndef SQLITE_NOHAVE_SYSTEM
		if (doXdgOpen) {
			const char *zXdgOpenCmd =
#if defined(_WIN32)
			    "start";
#elif defined(__APPLE__)
			    "open";
#else
			    "xdg-open";
#endif
			auto zCmd = StringUtil::Format("%s %s", zXdgOpenCmd, zTempFile);
			if (system(zCmd.c_str())) {
				PrintF(PrintOutput::STDERR, "Failed: [%s]\n", zCmd.c_str());
			} else {
				/* Give the start/open/xdg-open command some time to get
				** going before we continue, and potentially delete the
				** zTempFile data file out from under it */
				Sleep(2000);
			}
			PopOutputMode();
			doXdgOpen = 0;
		}
#endif /* !defined(SQLITE_NOHAVE_SYSTEM) */
	}
	outfile = string();
	out = stdout;
	stdout_is_console = true;
}

//! Render an error message as the JSON object the engine produces under errors_as_json
static string ErrorToJSON(const string &message) {
	if (!message.empty() && message[0] == '{') {
		// already JSON - drop the candidates field, the message already lists them
		auto fields = StringUtil::ParseJSONMap(message, true);
		if (fields.find("candidates") == fields.end()) {
			return message;
		}
		fields.erase("candidates");
		auto type = duckdb::Exception::StringToExceptionType(fields["exception_type"]);
		auto text = fields["exception_message"];
		fields.erase("exception_type");
		fields.erase("exception_message");
		return StringUtil::ExceptionToJSONMap(type, text, fields);
	}
	// preserve the exception type when the message starts with e.g. "Binder Error: "
	auto type = duckdb::ExceptionType::INVALID_INPUT;
	auto text = message;
	auto prefix_pos = message.find(" Error: ");
	if (prefix_pos != string::npos) {
		auto prefix_type = duckdb::Exception::StringToExceptionType(message.substr(0, prefix_pos));
		if (prefix_type != duckdb::ExceptionType::INVALID) {
			type = prefix_type;
			text = message.substr(prefix_pos + 8);
		}
	}
	StringUtil::RTrim(text);
	ErrorData error(type, text);
	error.ConvertErrorToJSON();
	return error.Message();
}

void ShellState::PrintDatabaseError(ErrorData error, const string &query) {
	if (agent_mode_active) {
		error.ConvertErrorToJSON();
	} else {
		error.AddErrorLocation(query);
	}
	PrintDatabaseError(error.Message());
}

void ShellState::PrintDatabaseError(const string &zErr) {
	if (agent_mode_active) {
		PrintF(PrintOutput::STDERR, "%s\n", ErrorToJSON(zErr).c_str());
		return;
	}
	if (!HighlightErrors()) {
		PrintF(PrintOutput::STDERR, "%s\n", zErr.c_str());
		return;
	}
	// detect dark-light mode if we haven't yet
	DetectDarkLightMode();
	// print the error
	ShellHighlight shell_highlight(*this);
	shell_highlight.PrintError(zErr);
}

/*
** Compare the string as a command-line option with either one or two
** initial "-" characters.
*/
static bool optionMatch(const string &str, const string &zOpt) {
	auto zStr = str.c_str();
	if (zStr[0] != '-') {
		return false;
	}
	zStr++;
	if (zStr[0] == '-') {
		zStr++;
	}
	return StringUtil::Equals(zStr, zOpt);
}

/*
** Delete a file.
*/
int shellDeleteFile(const char *zFilename) {
	int rc;
#ifdef _WIN32
	string str(zFilename);
	auto z = ShellState::Win32Utf8ToUnicode(str);
	rc = _wunlink(z.c_str());
#else
	rc = unlink(zFilename);
#endif
	return rc;
}

/*
** Try to delete the temporary file (if there is one) and free the
** memory used to hold the name of the temp file.
*/
void ShellState::ClearTempFile() {
	if (zTempFile.empty()) {
		return;
	}
	if (doXdgOpen) {
		return;
	}
	if (shellDeleteFile(zTempFile.c_str())) {
		return;
	}
	zTempFile = string();
}

/*
** Create a new temp file name with the given suffix.
*/
void ShellState::NewTempFile(const char *zSuffix) {
	ClearTempFile();
	zTempFile = string();
	/* If db is an in-memory database then the TEMPFILENAME file-control
	** will not work and we will need to fallback to guessing */
	const char *zTemp;
	uint64_t r;
	GenerateRandomBytes(sizeof(r), &r);
	zTemp = getenv("TEMP");
	if (zTemp == 0)
		zTemp = getenv("TMP");
	if (zTemp == 0) {
#ifdef _WIN32
		zTemp = "\\tmp";
#else
		zTemp = "/tmp";
#endif
	}
	zTempFile = StringUtil::Format("%s/temp%llx.%s", zTemp, r, zSuffix);
	if (zTempFile.empty()) {
		PrintF(PrintOutput::STDERR, "out of memory\n");
		ShellState::Exit(1);
	}
}

MetadataResult ShellState::EnableSafeMode(ShellState &state, const vector<string> &args) {
	state.safe_mode = true;
	if (state.db) {
		// db has been opened - disable external access
		state.ExecuteQuery("SET enable_external_access=false");
		state.ExecuteQuery("SET lock_configuration=true");
	}
	return MetadataResult::SUCCESS;
}

bool ShellState::SetOutputMode(const string &mode_name, const char *tbl_name) {
	auto mode_str = mode_name.c_str();
	idx_t n2 = mode_name.size();
	char c2 = mode_str[0];
	if (tbl_name && !(c2 == 'i' && strncmp(mode_str, "insert", n2) == 0)) {
		PrintF(PrintOutput::STDERR, "TABLE argument can only be used with .mode insert");
		return false;
	}
	if (c2 == 'l' && n2 > 2 && strncmp(mode_str, "lines", n2) == 0) {
		mode = RenderMode::LINE;
		rowSeparator = SEP_Row;
	} else if (c2 == 'c' && strncmp(mode_str, "columns", n2) == 0) {
		mode = RenderMode::COLUMN;
		if (ShellHasFlag(ShellFlags::SHFLG_HeaderSet)) {
			showHeader = true;
		}
		rowSeparator = SEP_Row;
	} else if (c2 == 'l' && n2 > 2 && strncmp(mode_str, "list", n2) == 0) {
		mode = RenderMode::LIST;
		colSeparator = SEP_Column;
		rowSeparator = SEP_Row;
	} else if (c2 == 'h' && strncmp(mode_str, "html", n2) == 0) {
		mode = RenderMode::HTML;
	} else if (c2 == 't' && strncmp(mode_str, "tcl", n2) == 0) {
		mode = RenderMode::TCL;
		colSeparator = SEP_Space;
		rowSeparator = SEP_Row;
	} else if (c2 == 'c' && strncmp(mode_str, "csv", n2) == 0) {
		mode = RenderMode::CSV;
		colSeparator = SEP_Comma;
		rowSeparator = SEP_CrLf;
	} else if (c2 == 't' && strncmp(mode_str, "tabs", n2) == 0) {
		mode = RenderMode::LIST;
		colSeparator = SEP_Tab;
	} else if (c2 == 'i' && strncmp(mode_str, "insert", n2) == 0) {
		mode = RenderMode::INSERT;
		SetTableName(tbl_name ? tbl_name : "table");
	} else if (c2 == 'q' && strncmp(mode_str, "quote", n2) == 0) {
		mode = RenderMode::QUOTE;
		colSeparator = SEP_Comma;
		rowSeparator = SEP_Row;
	} else if (c2 == 'a' && strncmp(mode_str, "ascii", n2) == 0) {
		mode = RenderMode::ASCII;
		colSeparator = SEP_Unit;
		rowSeparator = SEP_Record;
	} else if (c2 == 'm' && strncmp(mode_str, "markdown", n2) == 0) {
		mode = RenderMode::MARKDOWN;
	} else if (c2 == 't' && strncmp(mode_str, "table", n2) == 0) {
		mode = RenderMode::TABLE;
	} else if (c2 == 'b' && strncmp(mode_str, "box", n2) == 0) {
		mode = RenderMode::BOX;
	} else if (c2 == 'd' && strncmp(mode_str, "duckbox", n2) == 0) {
		mode = RenderMode::DUCKBOX;
	} else if (c2 == 'j' && strncmp(mode_str, "json", n2) == 0) {
		mode = RenderMode::JSON;
	} else if (c2 == 'l' && strncmp(mode_str, "latex", n2) == 0) {
		mode = RenderMode::LATEX;
	} else if (c2 == 't' && strncmp(mode_str, "trash", n2) == 0) {
		mode = RenderMode::TRASH;
	} else if (c2 == 'j' && strncmp(mode_str, "jsonlines", n2) == 0) {
		mode = RenderMode::JSONLINES;
	} else {
		PrintF(PrintOutput::STDERR, "Error: mode should be one of: "
		                            "ascii box column csv duckbox html insert json jsonlines latex line "
		                            "list markdown quote table tabs tcl trash \n");
		return false;
	}
	cMode = mode;
	return true;
}

MetadataResult ShellState::SetNullValue(ShellState &state, const vector<string> &args) {
	state.nullValue = args[1];
	return MetadataResult::SUCCESS;
}

bool ShellState::ImportData(const vector<string> &args) {
	if (safe_mode) {
		PrintF(PrintOutput::STDERR, ".import cannot be used in -safe mode\n");
		return false;
	}
	string table_name;
	string file_name;
	unordered_map<string, string> generic_parameters;
	string function;

	for (idx_t i = 1; i < args.size(); i++) {
		auto z = args[i].c_str();
		if (z[0] == '-' && z[1] == '-') {
			z++;
		}
		if (z[0] != '-') {
			if (file_name.empty()) {
				file_name = z;
			} else if (table_name.empty()) {
				table_name = z;
			} else {
				PrintF("ERROR: extra argument: \"%s\".  Usage:\n", z);
				PrintHelp("import");
				return false;
			}
		} else if (strcmp(z, "-v") == 0) {
			// verbose - ignore
		} else if (strcmp(z, "-ascii") == 0) {
			PrintF(PrintOutput::STDERR, "-ascii mode is no longer supported for .import");
			return false;
		} else if (strcmp(z, "-csv") == 0) {
			function = "read_csv";
		} else if (strcmp(z, "-parquet") == 0) {
			function = "read_parquet";
		} else if (strcmp(z, "-json") == 0) {
			function = "read_json";
		} else {
			z++;
			if (i + 1 >= args.size()) {
				PrintF("ERROR: expected an argument for generic parameter: \"%s\".  Usage:\n", z);
				PrintHelp("import");
				return false;
			}
			generic_parameters[z] = args[++i];
		}
	}
	if (table_name.empty()) {
		PrintF("ERROR: missing %s argument. Usage:\n", file_name.empty() ? "FILE" : "TABLE");
		PrintHelp("import");
		return false;
	}
	if (function.empty()) {
		// derive function to use from file extension
		// FIXME: get this list from the system somehow
		unordered_map<string, string> function_map;
		function_map[".parquet"] = "read_parquet";
		function_map[".csv"] = "read_csv";
		function_map[".tsv"] = "read_csv";
		function_map[".tbl"] = "read_csv";
		function_map[".json"] = "read_json";
		function_map[".jsonl"] = "read_json";
		function_map[".ndjson"] = "read_json";
		function_map[".avro"] = "read_avro";
		function_map[".xlsx"] = "read_xlsx";

		vector<string> compression_suffixes {"", ".gz", ".zst"};

		for (auto &entry : function_map) {
			for (auto &compression_suffix : compression_suffixes) {
				auto suffix = entry.first + compression_suffix;
				if (StringUtil::EndsWith(file_name, suffix)) {
					function = entry.second;
					break;
				}
			}
			if (!function.empty()) {
				break;
			}
		}
		if (function.empty()) {
			// fallback to read_csv
			function = "read_csv";
		}
	}
	if (function == "read_csv" && generic_parameters.find("ignore_errors") == generic_parameters.end()) {
		generic_parameters["ignore_errors"] = "true";
	}
	ClearInterrupt();
	// check if the table exists
	auto &con = *conn;
	auto needCommit = con.context->transaction.IsAutoCommit();
	if (needCommit) {
		con.BeginTransaction();
	}
	auto table_info = con.TableInfo(duckdb::Identifier(table_name));

	string import_query;

	if (!table_info) {
		// table does not exist - create it
		import_query = StringUtil::Format("CREATE TABLE %s AS ", SQLIdentifier(table_name));
	} else {
		// table exists - insert into it
		import_query = StringUtil::Format("INSERT INTO %s ", SQLIdentifier(table_name));
	}
	import_query += StringUtil::Format("SELECT * FROM %s(%s", function, SQLString(file_name));
	// add the generic parameters
	for (auto &entry : generic_parameters) {
		import_query += StringUtil::Format(", %s=%s", SQLIdentifier(entry.first), SQLString(entry.second));
	}
	import_query += ")";
	auto result = con.Query(import_query);
	if (result->HasError()) {
		if (needCommit) {
			con.Rollback();
		}
		string error = StringUtil::Format("Failed To Import Error: Failed to import from file '%s'\n", file_name);
		PrintDatabaseError(error);
		PrintDatabaseError(result->GetError());
		return false;
	}
	if (needCommit) {
		con.Commit();
	}
	return true;
}

ExecuteSQLSingleValueResult ShellState::ExecuteSQLSingleValue(duckdb::Connection &con, const string &sql,
                                                              string &result_value) {
	auto result = con.Query(sql);
	if (result->HasError()) {
		// store error in the result
		result_value = result->GetError();
		return ExecuteSQLSingleValueResult::EXECUTION_ERROR;
	}
	auto is_query = result->GetStatementProperties().return_type == duckdb::StatementReturnType::QUERY_RESULT;
	if (!is_query) {
		return ExecuteSQLSingleValueResult::EMPTY_RESULT;
	}
	auto &collection = result->Collection();
	if (collection.Count() == 0) {
		return ExecuteSQLSingleValueResult::EMPTY_RESULT;
	}
	if (collection.Count() > 1) {
		return ExecuteSQLSingleValueResult::MULTIPLE_ROWS;
	}
	if (collection.ColumnCount() != 1) {
		return ExecuteSQLSingleValueResult::MULTIPLE_COLUMNS;
	}

	auto value = collection.GetRows().GetValue(0, 0);
	if (value.IsNull()) {
		return ExecuteSQLSingleValueResult::NULL_RESULT;
	}
	result_value = value.ToString();
	return ExecuteSQLSingleValueResult::SUCCESS;
}

ExecuteSQLSingleValueResult ShellState::ExecuteSQLSingleValue(const string &sql, string &result_value) {
	return ExecuteSQLSingleValue(*conn, sql, result_value);
}

bool ShellState::OpenDatabase(const vector<string> &args) {
	if (safe_mode) {
		PrintF(PrintOutput::STDERR, ".open cannot be used in -safe mode\n");
		return false;
	}
	string zNewFilename;  /* Name of the database file to open */
	idx_t iName = 1;      /* Index in azArg[] of the filename */
	bool newFlag = false; /* True to delete file before opening */
	bool has_sql = false; /* True to use a query to derive the file path or connection string */
	zDbFilename = string();
	szMax = 0;
	/* Check for command-line arguments */
	config.options.access_mode = duckdb::AccessMode::READ_WRITE;
	for (iName = 1; iName < args.size() && args[iName][0] == '-'; iName++) {
		const char *z = args[iName].c_str();
		if (optionMatch(z, "new")) {
			newFlag = true;
		} else if (optionMatch(z, "readonly")) {
			config.options.access_mode = duckdb::AccessMode::READ_ONLY;
		} else if (optionMatch(z, "nofollow")) {
		} else if (optionMatch(z, "sql")) {
			if (has_sql) {
				Print(PrintOutput::STDERR, "Error: --sql provided multiple times\n");
				return false;
			}
			if (iName + 1 >= args.size()) {
				Print(PrintOutput::STDERR, "Error: missing SQL query after --sql\n");
				return false;
			}
			auto &query = args[++iName];

			string val;
			auto exec_result = ExecuteSQLSingleValue(query, val);
			switch (exec_result) {
			case ExecuteSQLSingleValueResult::EXECUTION_ERROR:
				PrintF(PrintOutput::STDERR, "Error: failed to evaluate --sql query '%s': %s\n", query, val);
				return false;
			case ExecuteSQLSingleValueResult::EMPTY_RESULT:
				Print(PrintOutput::STDERR, "Error: --sql query returned no rows, expected single value\n");
				return false;
			case ExecuteSQLSingleValueResult::MULTIPLE_ROWS:
				Print(PrintOutput::STDERR, "Error: --sql query returned multiple rows, expected single value\n");
				return false;
			case ExecuteSQLSingleValueResult::MULTIPLE_COLUMNS:
				Print(PrintOutput::STDERR, "Error: --sql query returned multiple columns, expected single value\n");
				return false;
			case ExecuteSQLSingleValueResult::NULL_RESULT:
				Print(PrintOutput::STDERR, "Error: --sql query returned a null value\n");
				return false;
			default:
				break;
			}
			zNewFilename = val;
			has_sql = true;
		} else if (z[0] == '-') {
			PrintF(PrintOutput::STDERR, "unknown option: %s\n", z);
			return false;
		}
	}

	if (has_sql && args.size() > iName) {
		Print(PrintOutput::STDERR, "Error: cannot use both --sql and a FILE argument\n");
		return false;
	}

	/* If a filename is specified, try to open it first */
	if (!has_sql && args.size() > iName) {
		zNewFilename = args[iName];
	}

	/* Close the existing database */
	db.reset();
	conn.reset();

	if (!zNewFilename.empty()) {
		if (newFlag) {
			shellDeleteFile(zNewFilename.c_str());
		}
		zDbFilename = zNewFilename;
		OpenDB(ShellOpenFlags::KEEP_ALIVE_ON_FAILURE);
		if (!db) {
			PrintF(PrintOutput::STDERR, "Error: cannot open '%s'\n", zNewFilename.c_str());
		}
	}

	if (!db) {
		/* As a fall-back open a TEMP database */
		zDbFilename = string();
		OpenDB();
	}
	return true;
}

MetadataResult ShellState::SetSeparator(ShellState &state, const vector<string> &args) {
	if (args.size() < 2 || args.size() > 3) {
		return MetadataResult::PRINT_USAGE;
	}
	state.colSeparator = args[1];
	if (args.size() >= 3) {
		state.rowSeparator = args[2];
	}
	return MetadataResult::SUCCESS;
}

bool ShellState::SetOutputFile(const vector<string> &args, OutputCommand output_command) {
	if (safe_mode) {
		PrintF(PrintOutput::STDERR, ".output/.once/.excel cannot be used in -safe mode\n");
		return false;
	}
	string zFile;
	int bTxtMode = 0;
	enum class OutputFileMode { DEFAULT, TEXT_EDITOR, SPREADSHEET };
	auto file_mode = OutputFileMode::DEFAULT;
	bool bBOM = false;

	if (output_command == OutputCommand::EXCEL) {
		file_mode = OutputFileMode::SPREADSHEET;
	}
	for (idx_t i = 1; i < args.size(); i++) {
		const char *z = args[i].c_str();
		if (z[0] == '-') {
			if (z[1] == '-') {
				z++;
			}
			if (strcmp(z, "-bom") == 0) {
				bBOM = true;
			} else if (output_command != OutputCommand::EXCEL && strcmp(z, "-x") == 0) {
				file_mode = OutputFileMode::SPREADSHEET;
			} else if (output_command != OutputCommand::EXCEL && strcmp(z, "-e") == 0) {
				file_mode = OutputFileMode::TEXT_EDITOR;
			} else {
				PrintF("ERROR: unknown option: \"%s\".  Usage:\n", args[i].c_str());
				PrintHelp(args[0].c_str());
				return false;
			}
		} else if (zFile.empty()) {
			zFile = z;
		} else {
			PrintF("ERROR: extra parameter: \"%s\".  Usage:\n", args[i].c_str());
			PrintHelp(args[0].c_str());
			return false;
		}
	}
	if (zFile.empty()) {
		zFile = "stdout";
	}
	if (output_command != OutputCommand::OUTPUT) {
		outCount = 2;
	} else {
		outCount = 0;
	}
	ResetOutput();
#ifndef SQLITE_NOHAVE_SYSTEM
	if (file_mode != OutputFileMode::DEFAULT) {
		doXdgOpen = 1;
		PushOutputMode();
		if (file_mode == OutputFileMode::SPREADSHEET) {
			/* spreadsheet mode.  Output as CSV. */
			NewTempFile("csv");
			ShellClearFlag(ShellFlags::SHFLG_Echo);
			mode = RenderMode::CSV;
			colSeparator = SEP_Comma;
			rowSeparator = SEP_CrLf;
		} else {
			/* text editor mode */
			NewTempFile("txt");
			bTxtMode = 1;
		}
		zFile = zTempFile;
	}
#endif /* SQLITE_NOHAVE_SYSTEM */
	if (zFile[0] == '|') {
#ifdef SQLITE_OMIT_POPEN
		PrintF(PrintOutput::STDERR, "Error: pipes are not supported in this OS\n");
		out = stdout;
		return false;
#else
		out = popen(zFile.c_str() + 1, "w");
		if (out == nullptr) {
			PrintF(PrintOutput::STDERR, "Error: cannot open pipe \"%s\"\n", zFile.c_str() + 1);
			out = stdout;
			return false;
		} else {
			if (bBOM) {
				fprintf(out, "\357\273\277");
			}
			outfile = zFile;
		}
#endif
	} else {
		out = OpenOutputFile(zFile.c_str(), bTxtMode);
		if (!out) {
			if (zFile != "off") {
				PrintF(PrintOutput::STDERR, "Error: cannot write to \"%s\"\n", zFile.c_str());
			}
			out = stdout;
			return false;
		} else {
			if (bBOM) {
				fprintf(out, "\357\273\277");
			}
			outfile = zFile;
		}
	}
	stdout_is_console = false;
	return true;
}

struct ShellInputFile {
	ShellInputFile(ShellState &state, FILE *file, const string *active_file = nullptr)
	    : state(state), file(file), saved_in(state.in), saved_lineno(state.lineno), active_file(active_file) {
		state.in = file;
	}
	~ShellInputFile() {
		if (file) {
			fclose(file);
		}
		state.in = saved_in;
		state.lineno = saved_lineno;
		if (active_file) {
			state.active_read_files.erase(*active_file);
		}
	}
	ShellState &state;
	FILE *file;
	FILE *saved_in;
	int saved_lineno;
	const string *active_file;
};

bool ShellState::ReadFromFile(const string &file) {
	if (safe_mode) {
		PrintF(PrintOutput::STDERR, ".read cannot be used in -safe mode\n");
		return false;
	}
	duckdb::LocalFileSystem lfs;
	auto canonical_file = lfs.CanonicalizePath(file, nullptr);
	if (!active_read_files.insert(canonical_file).second) {
		PrintF(PrintOutput::STDERR, "Error: recursive .read of \"%s\"\n", file);
		return false;
	}
	ShellInputFile input(*this, notNormalFile(file.c_str()) ? nullptr : fopen(file.c_str(), "rb"), &canonical_file);
	if (!in) {
		PrintF(PrintOutput::STDERR, "Error: cannot open \"%s\"\n", file.c_str());
		return false;
	}
	return ProcessInput(InputMode::FILE) == 0;
}

bool ShellState::DisplaySchemas(const vector<string> &args) {
	const char *zName = nullptr;
	bool bDebug = 0;
	// statements are pretty-printed using the SQL formatter by default
	bool bFormat = true;
	SuccessState rc = SuccessState::SUCCESS;

	for (idx_t ii = 1; ii < args.size(); ii++) {
		// --indent (and its alias --format) pretty-prints the statements using the SQL formatter (the default)
		if (optionMatch(args[ii], "indent") || optionMatch(args[ii], "format")) {
			bFormat = true;
			// --no-indent (and its alias --no-format) prints the statements as they are stored
		} else if (optionMatch(args[ii], "no-indent") || optionMatch(args[ii], "no-format")) {
			bFormat = false;
		} else if (optionMatch(args[ii], "debug")) {
			bDebug = true;
		} else if (!args[ii].empty() && args[ii][0] == '-') {
			PrintF(PrintOutput::STDERR,
			       "Error: unknown option \"%s\"\nUsage: .schema ?--indent|--no-indent? ?LIKE-PATTERN?\n",
			       args[ii].c_str());
			return false;
		} else if (zName == 0) {
			zName = args[ii].c_str();
		} else {
			PrintF(PrintOutput::STDERR, "Usage: .schema ?--indent|--no-indent? ?LIKE-PATTERN?\n");
			return false;
		}
	}
	auto renderer = GetRenderer(RenderMode::SEMI);
	renderer->show_header = false;

	string sSelect;
	// by default the stored SQL is pretty-printed through the duckdb_format_sql function
	// (unless --no-indent/--no-format was passed)
	sSelect +=
	    bFormat ? "SELECT duckdb_format_sql(sql) FROM sqlite_master WHERE " : "SELECT sql FROM sqlite_master WHERE ";
	if (zName) {
		auto zQarg = StringUtil::Format("%s", SQLString(zName));
		int bGlob = strchr(zName, '*') != 0 || strchr(zName, '?') != 0 || strchr(zName, '[') != 0;
		if (strchr(zName, '.')) {
			sSelect += "lower(printf('%s.%s',sname,tbl_name))";
		} else {
			sSelect += "lower(tbl_name)";
		}
		sSelect += bGlob ? " GLOB " : " LIKE ";
		sSelect += zQarg.c_str();
		if (!bGlob) {
			sSelect += " ESCAPE '\\' ";
		}
		sSelect += " AND ";
	}
	sSelect += "type!='meta' AND sql IS NOT NULL"
	           " ORDER BY name";
	if (bDebug) {
		PrintF("SQL: %s;\n", sSelect.c_str());
	} else {
		rc = RenderQuery(*renderer, sSelect, PagerMode::PAGER_OFF);
	}
	if (rc == SuccessState::FAILURE) {
		PrintF(PrintOutput::STDERR, "Error: querying schema information\n");
		return false;
	} else {
		return true;
	}
}

void ShellState::ShowConfiguration() {
	PrintF("%12.12s: %s\n", "echo", ShellHasFlag(ShellFlags::SHFLG_Echo) ? "on" : "off");
	PrintF("%12.12s: %s\n", "headers", showHeader ? "on" : "off");
	PrintF("%12.12s: %s\n", "mode", ModeToString(mode));
	PrintF("%12.12s: ", "nullvalue");
	Print(EscapeCString(nullValue));
	PrintF("\n");
	PrintF("%12.12s: %s\n", "output", !outfile.empty() ? outfile.c_str() : "stdout");
	PrintF("%12.12s: ", "colseparator");
	Print(EscapeCString(colSeparator));
	PrintF("\n");
	PrintF("%12.12s: ", "rowseparator");
	Print(EscapeCString(rowSeparator));
	PrintF("\n");
	PrintF("%12.12s: ", "width");
	for (auto w : colWidth) {
		PrintF("%d ", w);
	}
	PrintF("\n");
	PrintF("%12.12s: %s\n", "filename", zDbFilename.c_str());
	PrintF("%12.12s: %s\n", "agent", agent_mode_active ? (agent_name.empty() ? "on" : agent_name.c_str()) : "off");
}

MetadataResult ShellState::DisplayTables(const vector<string> &args) {
	if (args.size() > 2) {
		return MetadataResult::PRINT_USAGE;
	}
	// FIXME: copy pasted from below
	// Parse the filter pattern to check for schema qualification
	string filter_pattern = args.size() > 1 ? args[1] : string();
	string schema_filter = "";
	string table_filter = "%" + filter_pattern + "%";

	// Parse the filter pattern to check for schema qualification
	try {
		auto components = duckdb::QualifiedName::ParseComponents(filter_pattern);
		if (components.size() >= 2) {
			// e.g : "schema.table" or "schema.%"
			schema_filter = "%" + components[0].GetIdentifierName() + "%";
			table_filter = "%" + components[1].GetIdentifierName() + "%";
		}
	} catch (const duckdb::ParserException &) {
		// If parsing fails, treat as a simple table pattern
	}
	string schema_filter_str;
	string name_filter;
	if (!table_filter.empty()) {
		name_filter = StringUtil::Format(" AND columns.table_name ILIKE %s", SQLString(table_filter));
	}
	if (!schema_filter.empty()) {
		schema_filter_str = StringUtil::Format(" AND columns.schema_name ILIKE %s", SQLString(schema_filter));
	}
	auto query = StringUtil::Format(R"(
SELECT columns.database_name, columns.schema_name, columns.table_name, list(
	struct_pack(column_name, data_type, is_primary_key := c.column_index IS NOT NULL) order by column_index),
	t.estimated_size AS estimated_size, t.table_oid AS table_oid
FROM duckdb_columns() columns
LEFT JOIN duckdb_tables() t USING (table_oid)
LEFT JOIN (
	SELECT table_oid, UNNEST(constraint_column_indexes)+1 column_index
	FROM duckdb_constraints()
	WHERE constraint_type='PRIMARY KEY') c
USING (table_oid, column_index)
WHERE NOT columns.internal%s%s
GROUP BY ALL;
)",
	                                schema_filter_str, name_filter);

	auto &con = *conn;
	auto query_result = con.Query(query);
	if (query_result->HasError()) {
		PrintDatabaseError(query_result->GetError());
		return MetadataResult::FAIL;
	}
	vector<ShellTableInfo> result;
	for (auto &row : *query_result) {
		ShellTableInfo table;
		table.database_name = row.GetValue<string>(0);
		table.schema_name = row.GetValue<string>(1);
		table.table_name = row.GetValue<string>(2);

		auto column_val = row.GetBaseValue(3);
		for (auto &column_entry : duckdb::ListValue::GetChildren(column_val)) {
			ShellColumnInfo column;
			auto &struct_children = duckdb::StructValue::GetChildren(column_entry);
			column.column_name = struct_children[0].GetValue<string>();
			column.column_type = struct_children[1].GetValue<string>();
			column.is_primary_key = struct_children[2].GetValue<bool>();
			table.columns.push_back(std::move(column));
		}

		if (!row.IsNull(4)) {
			table.estimated_size = row.GetValue<idx_t>(4);
		}
		if (row.IsNull(5)) {
			// view
			table.is_view = true;
		}

		result.push_back(std::move(table));
	}
	RenderTableMetadata(result);
	return MetadataResult::SUCCESS;
}

//! Convert a LIST(VARCHAR) Value into a vector of strings (NULL children become "").
static vector<string> ManualStringList(const duckdb::Value &value) {
	vector<string> result;
	if (value.IsNull()) {
		return result;
	}
	for (auto &child : duckdb::ListValue::GetChildren(value)) {
		result.push_back(child.IsNull() ? "" : child.ToString());
	}
	return result;
}

MetadataResult ShellState::DisplayManual(const vector<string> &args) {
	if (args.size() != 2) {
		return MetadataResult::PRINT_USAGE;
	}
	auto &con = *conn;
	// the argument is a function name, optionally qualified: NAME, SCHEMA.NAME or DATABASE.SCHEMA.NAME;
	// quoted components ("my.func") are handled by the standard qualified-name parsing
	duckdb::QualifiedName qname;
	try {
		qname = duckdb::QualifiedName::Parse(args[1]);
	} catch (const std::exception &ex) {
		PrintF(PrintOutput::STDERR, "'%s' is not a valid function name - %s\n", args[1],
		       ErrorData(ex).RawMessage().c_str());
		return MetadataResult::FAIL;
	}
	if (qname.Path().size() > 3) {
		// duckdb_functions() only reports the innermost schema, so a nested schema path cannot be matched here
		PrintF(PrintOutput::STDERR,
		       "'%s' is not a valid function name - expected NAME, SCHEMA.NAME or DATABASE.SCHEMA.NAME\n", args[1]);
		return MetadataResult::FAIL;
	}
	// missing qualifiers match everything
	string name_pattern = qname.Name().GetIdentifierName();
	string schema_pattern = qname.Schema().empty() ? "%" : qname.Schema().GetIdentifierName();
	string database_pattern = qname.Catalog().empty() ? "%" : qname.Catalog().GetIdentifierName();

	// each part is used directly as a case-insensitive LIKE pattern: a plain word matches exactly,
	// while wildcards ("%abs") are honored. A pattern may resolve to several distinct functions - each
	// is rendered as its own manual entry
	auto prepared = con.Prepare(R"(
SELECT function_name, function_type, return_type, parameters, parameter_types, varargs, description, examples,
       database_name, schema_name
FROM duckdb_functions()
WHERE database_name ILIKE ? AND schema_name ILIKE ? AND function_name ILIKE ?
ORDER BY function_name, function_type, database_name, schema_name, length(parameter_types), parameter_types)");
	if (prepared->HasError()) {
		PrintDatabaseError(prepared->GetError());
		return MetadataResult::FAIL;
	}
	// note: pass C strings - the variadic Execute maps std::string to BLOB
	auto query_result = prepared->Execute(database_pattern.c_str(), schema_pattern.c_str(), name_pattern.c_str());
	if (query_result->HasError()) {
		PrintDatabaseError(query_result->GetError());
		return MetadataResult::FAIL;
	}

	vector<ManualItem> items;
	for (auto &row : *query_result) {
		ManualItem item;
		item.function_name = row.GetValue<string>(0);
		item.function_type = row.GetValue<string>(1);
		item.schema_path = row.GetValue<string>(8) + "." + row.GetValue<string>(9);
		item.return_type = row.IsNull(2) ? string() : row.GetValue<string>(2);
		item.parameters = ManualStringList(row.GetBaseValue(3));
		item.parameter_types = ManualStringList(row.GetBaseValue(4));
		item.varargs = row.IsNull(5) ? string() : row.GetValue<string>(5);
		item.description = row.IsNull(6) ? string() : row.GetValue<string>(6);
		item.examples = ManualStringList(row.GetBaseValue(7));
		items.push_back(std::move(item));
	}

	if (items.empty()) {
		PrintF(PrintOutput::STDERR, "No function matches '%s'\n", args[1]);
		// suggest similarly-named functions within the same qualifiers (typo tolerance) via
		// Jaro-Winkler similarity
		auto suggest_prepared = con.Prepare(R"(
SELECT name FROM (
  SELECT min(function_name) AS name, max(jaro_winkler_similarity(lower(function_name), lower(?))) AS score
  FROM duckdb_functions()
  WHERE database_name ILIKE ? AND schema_name ILIKE ?
  GROUP BY lower(function_name)
)
WHERE score >= 0.7
ORDER BY score DESC, name
LIMIT 5)");
		if (!suggest_prepared->HasError()) {
			auto suggest_result =
			    suggest_prepared->Execute(name_pattern.c_str(), database_pattern.c_str(), schema_pattern.c_str());
			if (!suggest_result->HasError()) {
				vector<string> suggestions;
				for (auto &row : *suggest_result) {
					suggestions.push_back(row.GetValue<string>(0));
				}
				if (!suggestions.empty()) {
					Print(PrintOutput::STDERR, "\nDid you mean:\n");
					for (auto &suggestion : suggestions) {
						PrintF(PrintOutput::STDERR, "\t%s\n", suggestion.c_str());
					}
				}
			}
		}
		return MetadataResult::SUCCESS;
	}

	// page width: terminal width, clamped to a readable range
	idx_t content_width = GetMaxRenderWidth();
	content_width = duckdb::MaxValue<idx_t>(40, duckdb::MinValue<idx_t>(content_width, 98));

	// coloring is only applied on an interactive console (matching the shell's SQL highlighter);
	// structural elements: reference markers/rules like the EXPLAIN layout (gray), section headings
	// and the banner name in bold white, the header schema path / entry type in white, and signature
	// parameter names in gray italic with parameter/return types in bold
	ManualStyle style;
	if (ShellHighlight::IsEnabled() && stdin_is_interactive && stdout_is_console) {
		auto &layout = ShellHighlight::GetHighlightElement(HighlightElementType::EXPLAIN_LAYOUT);
		style.layout_on = ShellHighlight::TerminalCode(layout.color, layout.intensity);
		if (!style.layout_on.empty()) {
			style.layout_off = ShellHighlight::ResetTerminalCode();
		}
		if (linenoiseGetTerminalColorMode() == LINENOISE_LIGHT_MODE) {
			style.heading_on = ShellHighlight::TerminalCode(PrintColor::BLACK, PrintIntensity::BOLD);
			style.path_on = ShellHighlight::TerminalCode(PrintColor::BLACK, PrintIntensity::STANDARD);
		} else {
			style.heading_on = ShellHighlight::TerminalCode(PrintColor::WHITE, PrintIntensity::BOLD);
			style.path_on = ShellHighlight::TerminalCode(PrintColor::WHITE, PrintIntensity::STANDARD);
		}
		style.param_on = ShellHighlight::TerminalCode(PrintColor::GRAY, PrintIntensity::ITALIC);
		style.type_on = ShellHighlight::TerminalCode(PrintColor::STANDARD, PrintIntensity::BOLD);

		style.heading_off = ShellHighlight::ResetTerminalCode();
		style.path_off = ShellHighlight::ResetTerminalCode();
		style.param_off = ShellHighlight::ResetTerminalCode();
		style.type_off = ShellHighlight::ResetTerminalCode();
	}

	// syntax-highlight the examples using the shell's SQL highlighter (no-op off-console)
	ManualHighlighter highlighter = [this](const string &text) {
		string highlighted = text;
		HighlightSQL(highlighted);
		return highlighted;
	};
	string page = RenderManualPage(items, content_width, style, highlighter);

	// page the output when it is long (respecting the user's .pager mode); no-op off an interactive console
	idx_t line_count = duckdb::NumericCast<idx_t>(std::count(page.begin(), page.end(), '\n'));
	auto pager_setup = ShouldUsePager(line_count) ? SetupPager() : nullptr;
	Print(page);
	return MetadataResult::SUCCESS;
}

MetadataResult ShellState::DisplayEntries(const vector<string> &args, char type) {
	string s;

	if (args.size() > 2) {
		return MetadataResult::PRINT_USAGE;
	}

	// Parse the filter pattern to check for schema qualification
	string filter_pattern = args.size() > 1 ? args[1] : "%";
	string schema_filter = "";
	string table_filter = filter_pattern;

	// Parse the filter pattern to check for schema qualification
	try {
		auto components = duckdb::QualifiedName::ParseComponents(filter_pattern);
		if (components.size() >= 2) {
			// e.g : "schema.table" or "schema.%"
			schema_filter = components[0].GetIdentifierName();
			table_filter = components[1].GetIdentifierName();
			// e.g : "schema."
			if (table_filter.empty()) {
				table_filter = "%";
			}
		}
	} catch (const duckdb::ParserException &) {
		// If parsing fails, treat as a simple table pattern
		schema_filter = "";
		table_filter = filter_pattern;
	}

	// Use DuckDB's system tables instead of SQLite's sqlite_schema
	if (type == 't') {
		string schema_filter_str;
		string name_filter = "WHERE ao.name LIKE ?1";
		if (!schema_filter.empty()) {
			schema_filter_str = "\n  WHERE schema_name LIKE ?1";
			name_filter = "WHERE ao.name LIKE ?2";
		}
		s = StringUtil::Format(R"(
WITH all_objects AS (
  SELECT schema_name, table_name as name FROM duckdb_tables%s
  UNION ALL
  SELECT schema_name, view_name as name FROM duckdb_views%s
),
name_counts AS (
  SELECT name, COUNT(*) as count FROM all_objects
  GROUP BY name
),
disambiguated AS (
  SELECT
    CASE
      WHEN nc.count > 1 THEN ao.schema_name || '.' || ao.name
      ELSE ao.name
    END as display_name
  FROM all_objects ao
  JOIN name_counts nc ON ao.name = nc.name
  %s
)
SELECT DISTINCT display_name FROM disambiguated ORDER BY display_name
)",
		                       schema_filter_str, schema_filter_str, name_filter);
	} else {
		// For indexes, use the original SQLite approach
		s = R"(
SELECT name FROM
sqlite_schema
WHERE type='index' AND tbl_name LIKE ?1)";
	}

	auto &con = *conn;
	auto prepared = con.Prepare(s);
	if (prepared->HasError()) {
		PrintDatabaseError(prepared->GetError());
		return MetadataResult::FAIL;
	}

	duckdb::vector<duckdb::Value> bind_values;

	if (type == 't') {
		// Bind parameters for the new DuckDB query
		if (!schema_filter.empty()) {
			bind_values.emplace_back(schema_filter);
			bind_values.emplace_back(table_filter);
		} else {
			bind_values.emplace_back(filter_pattern);
		}
	} else {
		// Original binding for indexes
		if (args.size() > 1) {
			bind_values.emplace_back(args[1]);
		} else {
			bind_values.emplace_back("%");
		}
	}

	auto query_result = prepared->Execute(bind_values);
	if (query_result->HasError()) {
		PrintDatabaseError(query_result->GetError());
	}
	vector<string> result;
	for (auto &row : *query_result) {
		result.push_back(row.GetValue<string>(0));
	}

	/* Pretty-print the contents of array azResult[] to the output */
	if (!result.empty()) {
		idx_t maxlen = 0;
		for (auto &r : result) {
			idx_t len = r.size();
			if (len > maxlen) {
				maxlen = len;
			}
		}
		idx_t nPrintCol = 80 / (maxlen + 2);
		if (nPrintCol < 1) {
			nPrintCol = 1;
		}
		idx_t nPrintRow = (result.size() + nPrintCol - 1) / nPrintCol;
		for (idx_t i = 0; i < nPrintRow; i++) {
			for (idx_t j = i; j < result.size(); j += nPrintRow) {
				string prefix = (j < nPrintRow) ? "" : "  ";
				string padded = result[j] + string(maxlen - result[j].length(), ' ');
				Print(prefix + padded);
			}
			Print("\n");
		}
	}
	return MetadataResult::SUCCESS;
}

SuccessState ShellState::ChangeDirectory(const string &path) {
	int rc;
#if defined(_WIN32) || defined(WIN32)
	auto z = ShellState::Win32Utf8ToUnicode(path);
	rc = !SetCurrentDirectoryW(z.c_str());
#else
	rc = chdir(path.c_str());
#endif
	if (rc) {
		PrintF(PrintOutput::STDERR, "Cannot change to directory \"%s\"\n", path);
		return SuccessState::FAILURE;
	}
	return SuccessState::SUCCESS;
}

SuccessState ShellState::ShowDatabases() {
	OpenDB();

	auto &con = *conn;
	auto query_result = con.Query("SELECT name, file FROM pragma_database_list");
	if (query_result->HasError()) {
		PrintDatabaseError(query_result->GetError());
		return SuccessState::FAILURE;
	}
	ShellTableInfo result;
	result.table_name = "databases";
	for (auto &row : *query_result) {
		ShellColumnInfo column;
		// database name
		column.column_name = row.GetValue<string>(0);
		// database file
		column.column_type = row.IsNull(1) ? "(memory)" : row.GetValue<string>(1);
		result.columns.push_back(std::move(column));
	}
	vector<ShellTableInfo> result_list;
	result_list.push_back(std::move(result));
	RenderTableMetadata(result_list);
	return SuccessState::SUCCESS;
}

MetadataResult ShellState::ToggleTimer(ShellState &state, const vector<string> &args) {
	if (args.size() < 2 || args.size() > 3) {
		return MetadataResult::PRINT_USAGE;
	}
	if (args.size() == 3) {
		auto digits = ShellState::StringToInt(args[2]);
		if (digits < 0 || digits > 9) {
			state.PrintF(PrintOutput::STDERR, ".timer DIGITS must be between 0 and 9\n");
			return MetadataResult::FAIL;
		}
		timerDigits = static_cast<int>(digits);
	}
	enableTimer = state.StringToBool(args[1]);
	if (enableTimer && !HAS_TIMER) {
		state.PrintF(PrintOutput::STDERR, "Error: timer not available on this system.\n");
		enableTimer = false;
	}
	return MetadataResult::SUCCESS;
}

/*
** If an input line begins with "." then invoke this routine to
** process that line.
**
** Return 1 on error, 2 to exit, and 0 otherwise.
*/
int ShellState::DoMetaCommand(const string &zLine) {
	int rc = 0;
	vector<string> args;
	// skip initial dot
	idx_t pos = 1;
	while (pos < zLine.size()) {
		// skip initial spaces
		while (pos < zLine.size() && IsSpace(zLine[pos])) {
			pos++;
		}
		if (pos >= zLine.size()) {
			break;
		}
		string arg;
		if (zLine[pos] == '\'' || zLine[pos] == '"') {
			// quoted argument - scan until next quote
			auto quote = zLine[pos];
			// skip over the initial quote
			pos++;

			while (pos < zLine.size() && zLine[pos] != quote) {
				if (zLine[pos] == '\\' && quote == '"' && pos + 1 < zLine.size()) {
					// skip over any escaped characters
					arg += zLine[pos++];
				}
				arg += zLine[pos++];
			}
			if (pos < zLine.size()) {
				// skip over the final quote
				pos++;
			}
			if (quote == '"') {
				arg = resolve_backslashes(arg);
			}
		} else {
			// unquoted argument - scan until the next space
			while (pos < zLine.size() && !IsSpace(zLine[pos])) {
				arg += zLine[pos];
				pos++;
			}
			arg = resolve_backslashes(arg);
		}
		args.push_back(std::move(arg));
	}

	/* Process the input line.
	 */
	if (args.empty()) {
		return 0; /* no tokens, no error */
	}
	ClearTempFile();

	string error_msg;
	auto metadata_command = FindMetadataCommand(args[0], error_msg);
	if (!metadata_command) {
		// command not found
		PrintDatabaseError(error_msg);
		rc = 1;
	} else {
		auto &command = *metadata_command;
		MetadataResult result = MetadataResult::PRINT_USAGE;
		try {
			if (!command.callback) {
				PrintF(PrintOutput::STDERR, "Command \"%s\" is unsupported in the current version of the CLI\n",
				       command.command);
				result = MetadataResult::FAIL;
			} else if (command.argument_count == 0 || command.argument_count == args.size()) {
				result = command.callback(*this, args);
			}
			if (result == MetadataResult::PRINT_USAGE) {
				string error = StringUtil::Format("Invalid Command Error: Invalid usage of command '.%s'\n\n", args[0]);
				error += StringUtil::Format("Usage: '.%s %s'", command.command, command.usage);
				PrintDatabaseError(error);
				rc = 1;
				result = MetadataResult::FAIL;
			}
		} catch (std::exception &ex) {
			PrintDatabaseError(ErrorData(ex));
			result = MetadataResult::FAIL;
		}
		rc = int(result);
	}

	if (outCount) {
		outCount--;
		if (outCount == 0) {
			ResetOutput();
		}
	}
	return rc;
}

/*
** Return TRUE if a semicolon occurs anywhere in the first N characters
** of string z[].
*/
static bool line_contains_semicolon(const char *z, idx_t N) {
	for (idx_t i = 0; i < N; i++) {
		if (z[i] == ';') {
			return true;
		}
	}
	return false;
}

/*
** Test to see if a line consists entirely of whitespace.
*/
static bool _all_whitespace(const char *z) {
	for (; *z; z++) {
		if (ShellState::IsSpace(z[0])) {
			continue;
		}
		if (*z == '/' && z[1] == '*') {
			z += 2;
			while (*z && (*z != '*' || z[1] != '/')) {
				z++;
			}
			if (*z == 0) {
				return false;
			}
			z++;
			continue;
		}
		if (*z == '-' && z[1] == '-') {
			z += 2;
			while (*z && *z != '\n') {
				z++;
			}
			if (*z == 0)
				return true;
			continue;
		}
		return false;
	}
	return true;
}

enum class SQLParseState { SEMICOLON, WHITESPACE, NORMAL };

static const char *skipDollarQuotedString(const char *zSql, const char *delimiterStart, idx_t delimiterLength) {
	for (; *zSql; zSql++) {
		if (*zSql == '$') {
			// found a dollar
			// move forward and find the next dollar
			zSql++;
			auto start = zSql;
			while (*zSql && *zSql != '$') {
				zSql++;
			}
			if (!zSql[0]) {
				// reached end of string while looking for the dollar
				return nullptr;
			}
			// check if the dollar quoted string name matches
			if (delimiterLength == idx_t(zSql - start)) {
				if (memcmp(start, delimiterStart, delimiterLength) == 0) {
					return zSql;
				}
			}
			// dollar does not match - reset position to start and keep looking
			zSql = start - 1;
		}
	}
	// unterminated
	return nullptr;
}

bool ShellState::SQLIsComplete(const char *zSql) {
	auto state = SQLParseState::NORMAL;

	for (; *zSql; zSql++) {
		SQLParseState next_state;
		switch (*zSql) {
		case ';':
			next_state = SQLParseState::SEMICOLON;
			break;
		case ' ':
		case '\r':
		case '\t':
		case '\n':
		case '\f': { /* White space is ignored */
			next_state = SQLParseState::WHITESPACE;
			break;
		}
		case '/': { /* C-style comments */
			if (zSql[1] != '*') {
				next_state = SQLParseState::NORMAL;
				break;
			}
			zSql += 2;
			while (zSql[0] && (zSql[0] != '*' || zSql[1] != '/')) {
				zSql++;
			}
			if (zSql[0] == 0) {
				// unterminated c-style string
				return false;
			}
			zSql++;
			next_state = SQLParseState::WHITESPACE;
			break;
		}
		case '-': { /* SQL-style comments from "--" to end of line */
			if (zSql[1] != '-') {
				next_state = SQLParseState::NORMAL;
				break;
			}
			while (*zSql && *zSql != '\n') {
				zSql++;
			}
			if (*zSql == 0) {
				// unterminated SQL-style comment - return whether or not we had a semicolon right before it
				return state == SQLParseState::SEMICOLON;
			}
			next_state = SQLParseState::WHITESPACE;
			break;
		}
		case '$': { /* Dollar-quoted strings */
			// check if this is a dollar-quoted string
			idx_t next_dollar = 0;
			for (idx_t idx = 1; zSql[idx]; idx++) {
				if (zSql[idx] == '$') {
					// found the next dollar
					next_dollar = idx;
					break;
				}
				// all characters can be between A-Z, a-z, underscore, or \200 - \377
				if (zSql[idx] >= 'A' && zSql[idx] <= 'Z') {
					continue;
				}
				if (zSql[idx] >= 'a' && zSql[idx] <= 'z') {
					continue;
				}
				if (zSql[idx] == '_') {
					continue;
				}
				if (zSql[idx] >= '\200' && zSql[idx] <= '\377') {
					continue;
				}
				// the first character CANNOT be a numeric, only subsequent characters
				if (idx > 1 && zSql[idx] >= '0' && zSql[idx] <= '9') {
					continue;
				}
				// not a dollar quoted string
				break;
			}
			if (next_dollar == 0) {
				// not a dollar quoted string
				next_state = SQLParseState::NORMAL;
				break;
			}
			auto start = zSql + 1;
			zSql += next_dollar;
			const char *delimiterStart = start;
			idx_t delimiterLength = zSql - start;
			zSql++;
			// skip the dollar quoted string
			zSql = skipDollarQuotedString(zSql, delimiterStart, delimiterLength);
			if (!zSql) {
				// unterminated dollar string
				return false;
			}
			next_state = SQLParseState::WHITESPACE;
			break;
		}
		case '`': /* backtick-quoted identifiers */
		case '"': /* single- and double-quoted strings */
		case '\'': {
			int c = *zSql;
			zSql++;
			while (*zSql && *zSql != c) {
				zSql++;
			}
			if (*zSql == 0) {
				// unterminated single or double quoted string
				return 0;
			}
			next_state = SQLParseState::WHITESPACE;
			break;
		}
		default:
			next_state = SQLParseState::NORMAL;
		}
		// white space is ignored (no change in state)
		if (next_state != SQLParseState::WHITESPACE) {
			state = next_state;
		}
	}
	// the statement is complete only if we end in a semicolon
	return state == SQLParseState::SEMICOLON;
}

void ShellState::ShellAddHistory(const char *history) {
#ifdef HAVE_LINENOISE
	if (rl_version == ReadLineVersion::LINENOISE) {
		linenoiseHistoryAdd(history);
	}
#endif
}

int ShellState::ShellLoadHistory(const char *path) {
#ifdef HAVE_LINENOISE
	if (rl_version == ReadLineVersion::LINENOISE) {
		return linenoiseHistoryLoad(path);
	}
#endif
	return 0;
}

int ShellState::ShellSaveHistory(const char *path) {
#ifdef HAVE_LINENOISE
	if (rl_version == ReadLineVersion::LINENOISE) {
		return linenoiseHistorySave(path);
	}
#endif
	return 0;
}

int ShellState::ShellSetHistoryMaxLength(idx_t max_length) {
#ifdef HAVE_LINENOISE
	if (rl_version == ReadLineVersion::LINENOISE) {
		return linenoiseHistorySetMaxLen(static_cast<int>(max_length));
	}
#endif
	return 0;
}
/*
** Run a single line of SQL.  Return the number of errors.
*/
int ShellState::RunOneSqlLine(InputMode mode, char *zSql) {
	string zErrMsg;

	if (mode == InputMode::STANDARD && zSql && *zSql && *zSql != '\3') {
		ShellAddHistory(zSql);
	}
	BEGIN_TIMER;
	auto success = ExecuteSQL(zSql);
	END_TIMER;
	if (success != SuccessState::SUCCESS) {
		return 1;
	} else if (ShellHasFlag(ShellFlags::SHFLG_CountChanges)) {
		PrintF("changes: %3llu   total_changes: %llu\n", (unsigned long long)last_changes,
		       (unsigned long long)total_changes);
	}
	return 0;
}

bool ShellState::GetBailOnError(InputMode mode) {
	if (bail != BailOnError::AUTOMATIC) {
		return bail == BailOnError::BAIL_ON_ERROR;
	}
	// by default bail on error in file and duckdb_rc modes
	return mode == InputMode::FILE || mode == InputMode::DUCKDB_RC;
}

/*
** Read input from *in and process it.  If *in==0 then input
** is interactive - the user is typing it in.  Otherwise, input
** is coming from a file or device.  A prompt is issued and history
** is saved only if input is interactive.  An interrupt signal will
** cause this routine to exit immediately, unless input is interactive.
**
** Return the number of errors.
*/
struct ShellInputBuffers {
	~ShellInputBuffers() {
		free(line);
		free(sql);
	}
	char *line = nullptr;
	char *sql = nullptr;
};

int ShellState::ProcessInput(InputMode mode) {
	ShellInputBuffers buffers;
	auto &zLine = buffers.line;
	auto &zSql = buffers.sql;
	idx_t nLine;         /* Length of current line */
	idx_t nSql = 0;      /* Bytes of zSql[] used */
	idx_t nAlloc = 0;    /* Allocated zSql[] space */
	idx_t nSqlPrior = 0; /* Bytes of zSql[] used by prior line */
	int rc;              /* Error code */
	idx_t errCnt = 0;    /* Number of errors seen */
	idx_t numCtrlC = 0;
	lineno = 0;
	while (errCnt == 0 || !GetBailOnError(mode) || (!in && stdin_is_interactive)) {
		fflush(out);
		zLine = OneInputLine(in, zLine, nSql > 0);
		if (!zLine) {
			/* End of input */
			if (!in && stdin_is_interactive && !started_as_client && conn && conn->context &&
			    conn->context->IsConnected()) {
				// First Ctrl-D while CONNECT-ed: implicit DISCONNECT instead of exiting. A second
				// Ctrl-D (now unbound) will exit normally. Shells launched with `-connect` skip this
				// and exit right away - disconnecting would leave a local shell that was never asked for.
				printf("\n");
				conn->Query("DISCONNECT");
				nSql = 0;
				continue;
			}
			if (!in && stdin_is_interactive) {
				printf("\n");
			}
			break;
		}
		// if we are receiving input after a query was interrupted
		// we need to clear the interrupt flag to be able to
		// print messages again
		if (seenInterrupt) {
			if (in) {
				break;
			}
			ClearInterrupt();
		}
		if (*zLine == '\3') {
			// ctrl c: reset sql statement
			if (nSql == 0 && zLine[1] == '\0' && stdin_is_interactive) {
				// if in interactive mode and we press ctrl c twice
				// on an empty line, we print the ctrl d hint message
				numCtrlC++;
				if (numCtrlC >= 2) {
					Print("Interrupted, use Ctrl+D to exit\n");
				}
			}
			nSql = 0;
			continue;
		} else {
			numCtrlC = 0;
		}
		if (mode == InputMode::DUCKDB_RC && !StringUtil::StartsWith(zLine, ".startup_text")) {
			if (startup_text == StartupText::ALL) {
				ShellHighlight highlight(*this);
				highlight.PrintText(StringUtil::Format("-- Loading resources from %s\n", duckdb_rc_path),
				                    PrintOutput::STDERR, HighlightElementType::STARTUP_TEXT);
				displayed_loading_resources_message = true;
			}
			mode = InputMode::FILE;
		}
		lineno++;
		if (nSql == 0 && _all_whitespace(zLine)) {
			if (ShellHasFlag(ShellFlags::SHFLG_Echo)) {
				printf("%s\n", zLine);
			}
			continue;
		}
		if ((zLine[0] == '.' || zLine[0] == '#') && nSql == 0) {
			if (ShellHasFlag(ShellFlags::SHFLG_Echo)) {
				printf("%s\n", zLine);
			}
			if (zLine[0] == '.') {
				if (mode == InputMode::STANDARD && *zLine && *zLine != '\3') {
					ShellAddHistory(zLine);
				}
				rc = DoMetaCommand(zLine);
				if (rc == 2) { /* exit requested */
					break;
				} else if (rc) {
					errCnt++;
				}
			}
			continue;
		}
		nLine = StringLength(zLine);
		if (nSql + nLine + 2 >= nAlloc) {
			nAlloc = nSql + nLine + 100;
			auto new_sql = (char *)realloc(zSql, nAlloc);
			if (!new_sql) {
				shell_out_of_memory();
			}
			zSql = new_sql;
		}
		nSqlPrior = nSql;
		if (nSql == 0) {
			int i;
			for (i = 0; zLine[i] && IsSpace(zLine[i]); i++) {
			}
			assert(nAlloc > 0 && zSql);
			memcpy(zSql, zLine + i, nLine + 1 - i);
			nSql = nLine - i;
		} else {
			zSql[nSql++] = '\n';
			memcpy(zSql + nSql, zLine, nLine + 1);
			nSql += nLine;
		}
		if (nSql && line_contains_semicolon(&zSql[nSqlPrior], nSql - nSqlPrior) && SQLIsComplete(zSql)) {
			errCnt += RunOneSqlLine(mode, zSql);
			nSql = 0;
			if (outCount) {
				ResetOutput();
				outCount = 0;
			} else {
				ClearTempFile();
			}
		} else if (nSql && _all_whitespace(zSql)) {
			if (ShellHasFlag(ShellFlags::SHFLG_Echo)) {
				printf("%s\n", zSql);
			}
			nSql = 0;
		}
	}
	if (nSql && !_all_whitespace(zSql)) {
		errCnt += RunOneSqlLine(mode, zSql);
	}
	return errCnt > 0;
}

static string GetHomeDirectory() {
	duckdb::LocalFileSystem lfs;
	return lfs.GetHomeDirectory();
}

string ShellState::GetDefaultDuckDBRC() {
	duckdb::LocalFileSystem lfs;
	return lfs.JoinPath(GetHomeDirectory(), ".duckdbrc");
}

MetadataResult ShellState::FormatSQL(string &sql) {
	if (sql.empty()) {
		// no input
		return MetadataResult::SUCCESS;
	}
	// Format through the duckdb_format_sql SQL function using a prepared statement.
	auto result = conn->Query("SELECT duckdb_format_sql($1)", duckdb::Value(sql));
	if (result->HasError()) {
		PrintF(PrintOutput::STDERR, "%s: %s\n", program_name, result->GetError().c_str());
		return MetadataResult::FAIL;
	}
	sql = string();
	for (auto &row : *result) {
		sql = row.GetValue<string>(0) + "\n";
	}
	return MetadataResult::SUCCESS;
}

void ShellState::HighlightSQL(string &sql) {
	if (!stdout_is_console || !duckdb::Highlighting::IsEnabled()) {
		// highlighting is not enabled
		return;
	}
	auto tokens = duckdb::Highlighting::Tokenize(const_cast<char *>(sql.c_str()), sql.size(), false);
	auto highlighted =
	    duckdb::Highlighting::HighlightText(const_cast<char *>(sql.c_str()), sql.size(), 0, sql.size(), tokens);
	sql = std::move(highlighted);
}

void ShellState::PrintSQL(const string &sql) {
	string highlighted = sql;
	// HighlightSQL is a no-op when highlighting is disabled or output is not a console
	HighlightSQL(highlighted);
	Print(highlighted);
}

string ShellState::ReadFileContents(FILE *f) {
	char buf[4096];
	size_t n;
	string result;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
		result.append(buf, n);
	}
	return result;
}

string ShellState::ReadFileContents(const string &filename) {
	unique_ptr<FILE, decltype(&fclose)> f(fopen(filename.c_str(), "rb"), fclose);
	if (!f) {
		throw duckdb::IOException("cannot open '%s' for reading: %s\n", filename.c_str(), strerror(errno));
	}
	return ReadFileContents(f.get());
}

/*
** Read input from the file given by sqliterc_override.  Or if that
** parameter is NULL, take input from ~/.duckdbrc
**
** Returns true if successful, false otherwise.
*/

bool ShellState::ProcessFile(const string &file, InputMode input_mode, bool default_duckdb_rc) {
	ShellInputFile input(*this, fopen(file.c_str(), "rb"));
	if (in) {
		return ProcessInput(input_mode) == 0;
	}
	if (input_mode != InputMode::DUCKDB_RC || !default_duckdb_rc) {
		PrintDatabaseError("IO Error: Failed to open file \"" + file + "\"");
		return false;
	}
	return true;
}

bool ShellState::ProcessDuckDBRC(const char *file) {
	string path;
	bool is_default = false;
	if (!file) {
		// use default .duckdbrc path
		path = ShellState::GetDefaultDuckDBRC();
		if (path.empty()) {
			// could not find home directory - return
			PrintF(PrintOutput::STDERR, "-- warning: cannot find home directory;"
			                            " cannot read ~/.duckdbrc\n");
			return true;
		}
		file = path.c_str();
		is_default = true;
	}
	duckdb_rc_path = file;
	return ProcessFile(file, InputMode::DUCKDB_RC, is_default);
}

#ifdef HAVE_LINENOISE
/*
** Linenoise completion callback
*/
static char *linenoise_format(const char *zLine) {
	auto &state = ShellState::Get();
	if (state.auto_format == AutoFormatMode::NO_AUTO_FORMAT) {
		return nullptr;
	}
	if (!state.conn) {
		return nullptr;
	}
	if (zLine[0] == '.' || zLine[0] == '#' || zLine[0] == '\3') {
		return nullptr;
	}
	try {
		auto prepared = state.conn->Prepare("SELECT duckdb_format_sql($1)");
		if (prepared->HasError()) {
			return nullptr;
		}
		vector<duckdb::Value> params = {duckdb::Value(string(zLine))};
		auto result = prepared->Execute(params);
		if (result->HasError()) {
			return nullptr;
		}
		auto row = result->begin();
		if (row == result->end() || (*row).IsNull(0)) {
			return nullptr;
		}
		return strdup((*row).GetValue<string>(0).c_str());
	} catch (std::exception &) {
		return nullptr;
	}
}

static void linenoise_completion(const char *zLine, linenoiseCompletions *lc) {
	auto &state = ShellState::Get();
	try {
		idx_t nLine = ShellState::StringLength(zLine);
		if (zLine[0] == '.') {
			// auto-complete dot command
			auto dot_completions = ShellState::GetMetadataCompletions(zLine, nLine);
			for (auto &completion : dot_completions) {
				linenoiseAddCompletion(lc, zLine, completion.c_str(), completion.size(), 0, "keyword", 0, '\0');
			}
			return;
		}
		if (zLine[0] == '#') {
			return;
		}
		auto zSql = StringUtil::Format("CALL sql_auto_complete(%s)", SQLString(zLine));
		unique_ptr<duckdb::DuckDB> localDB;
		unique_ptr<duckdb::Connection> localCon;

		auto &con = *state.conn;
		auto result = con.Query(zSql);
		if (result->HasError()) {
			return;
		}
		for (auto &row : *result) {
			auto zCompletion = row.GetValue<string>(0);
			idx_t iStart = row.GetValue<idx_t>(1);
			auto completion_type = row.GetValue<string>(2);
			auto score = row.GetValue<uint64_t>(3);
			char extra_char = '\0';
			if (!row.IsNull(4)) {
				auto extra_char_str = row.GetValue<string>(4);
				if (extra_char_str.size() == 1) {
					extra_char = extra_char_str[0];
				}
			}
			linenoiseAddCompletion(lc, zLine, zCompletion.c_str(), zCompletion.size(), iStart, completion_type.c_str(),
			                       score, extra_char);
		}
	} catch (std::exception &ex) {
		return;
	}
}
#endif

struct CommandLineCall {
	CommandLineCall(const CommandLineOption &option, vector<string> arguments_p)
	    : option(option), arguments(std::move(arguments_p)) {
	}

	const CommandLineOption &option;
	vector<string> arguments;
};

/*
** Initialize the state information in data
*/
void ShellState::Initialize() {
	normalMode = cMode = mode = RenderMode::DUCKBOX;
	max_rows = 40;
	colSeparator = SEP_Column;
	rowSeparator = SEP_Row;
	showHeader = true;
	main_prompt = make_uniq<Prompt>();
	string default_prompt;
	default_prompt = "{max_length:50}{highlight_element:prompt_connect}{setting:connect_name_prefix}{highlight_element:"
	                 "prompt}{setting:current_database_and_schema}"
	                 "{color:reset} D ";
	main_prompt->ParsePrompt(default_prompt);
	vector<string> default_components;
	default_components.push_back("{setting:progress_bar_percentage} {setting:progress_bar}{setting:eta}");
	default_components.push_back(
	    "{align:right}{min_size:18}{hide_if_contains:0 bytes}Written: {setting:bytes_written}");
	default_components.push_back("{align:right}{min_size:15}{hide_if_contains:0 bytes}Read: {setting:bytes_read}");
	default_components.push_back("{align:right}{min_size:17}Memory: {setting:memory_usage}");
	default_components.push_back("{align:right}{min_size:15}{hide_if_contains:0 bytes}Swap: {setting:swap_usage}");
	progress_bar = make_uniq<ShellProgressBar>();
	for (auto &component : default_components) {
		progress_bar->AddComponent(component);
	}
#ifdef HAVE_LINENOISE
	if (rl_version == ReadLineVersion::LINENOISE) {
		linenoiseSetPrompt(continuePrompt, continuePromptSelected, scrollUpPrompt, scrollDownPrompt);
	}
#endif
#if defined(_WIN32) || defined(WIN32)
	if (stdout_is_console) {
		// On Windows virtual terminal processing may be disabled by default,
		// we need it enabled (even when highlighting is off) to process
		// ANSI escape sequences, for example the position of the cursor.
		HANDLE out_handle = GetStdHandle(STD_OUTPUT_HANDLE);
		if (out_handle != INVALID_HANDLE_VALUE) {
			DWORD mode = 0;
			if (GetConsoleMode(out_handle, &mode)) {
				if (!(mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
					mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
					SetConsoleMode(out_handle, mode);
				}
			}
		}
	}
#endif
}

//! Environment variables that AI coding agents set for the commands they run. The generic AGENT/AI_AGENT convention
//! carries the agent's name as its value, the others are tool-specific markers.
//! Agent-mode defaults (see DetectAgentMode)
static constexpr size_t AGENT_MAX_ROWS = 1000;
static constexpr size_t AGENT_MAX_BYTES = 10000;
static constexpr size_t AGENT_MAX_CELL_WIDTH = 500;
//! The planner's read estimate from which the estimate line is printed
static constexpr idx_t ESTIMATE_MIN_ROWS = 1000000;

struct AgentEnvironmentMarker {
	const char *variable;
	//! The agent name when the variable does not carry one itself
	const char *agent_name;
};

static const AgentEnvironmentMarker AGENT_ENVIRONMENT_MARKERS[] = {{"AI_AGENT", nullptr},
                                                                   {"AGENT", nullptr},
                                                                   {"CLAUDECODE", "claude-code"},
                                                                   {"CODEX_CI", "codex"},
                                                                   {"CODEX_SANDBOX", "codex"},
                                                                   {"CODEX_THREAD_ID", "codex"},
                                                                   {"CURSOR_AGENT", "cursor"},
                                                                   {"GEMINI_CLI", "gemini-cli"},
                                                                   {"COPILOT_AGENT", "github-copilot"},
                                                                   {"COPILOT_CLI", "github-copilot"},
                                                                   {"COPILOT_AGENT_SESSION_ID", "github-copilot"},
                                                                   {nullptr, nullptr}};

bool ShellState::DetectAgentEnvironment(string &agent_name, string &marker) {
	for (idx_t i = 0; AGENT_ENVIRONMENT_MARKERS[i].variable; i++) {
		auto &entry = AGENT_ENVIRONMENT_MARKERS[i];
		auto value = getenv(entry.variable);
		if (!value || !value[0]) {
			continue;
		}
		agent_name = entry.agent_name ? entry.agent_name : value;
		marker = entry.variable;
		return true;
	}
	return false;
}

//! DUCKDB_AGENT_MODE=1/0 (on/off, true/false, yes/no) forces agent mode on or off - like -agent / -no-agent, which
//! take precedence. Unset, empty or "auto" leaves it to the auto-detection
static OptionType AgentModeFromEnvironment(ShellState &state) {
	auto value = getenv("DUCKDB_AGENT_MODE");
	if (!value || !value[0] || StringUtil::CIEquals(value, "auto")) {
		return OptionType::DEFAULT;
	}
	for (auto on : {"1", "on", "true", "yes"}) {
		if (StringUtil::CIEquals(value, on)) {
			return OptionType::ON;
		}
	}
	for (auto off : {"0", "off", "false", "no"}) {
		if (StringUtil::CIEquals(value, off)) {
			return OptionType::OFF;
		}
	}
	state.PrintF(PrintOutput::STDERR, "warning: ignoring DUCKDB_AGENT_MODE=%s (expected 1, 0 or auto)\n", value);
	return OptionType::DEFAULT;
}

void ShellState::DetectAgentMode() {
	if (agent_mode == OptionType::DEFAULT) {
		// no -agent / -no-agent on the command line - the environment may decide
		agent_mode = AgentModeFromEnvironment(*this);
		agent_mode_from_environment = agent_mode != OptionType::DEFAULT;
	}
	switch (agent_mode) {
	case OptionType::ON:
		agent_mode_active = true;
		break;
	case OptionType::OFF:
		agent_mode_active = false;
		break;
	default:
		// auto-detect: an agent reads our output through a pipe, never from a terminal. An output mode on the command
		// line (-csv, -json, ...) is a deliberate choice of format that the agent rendering would fight with, so it
		// leaves the mode off; -agent still forces it
		agent_mode_active = !output_mode_flag && !stdout_is_console && DetectAgentEnvironment(agent_name, agent_marker);
		break;
	}
	if (!agent_mode_active) {
		return;
	}
	if (agent_name.empty()) {
		string marker;
		DetectAgentEnvironment(agent_name, marker);
	}
	// a compact markdown table (see ModeMarkdownRenderer). The result is capped, but loudly: the first rows are
	// rendered and the footer says how many there are in total and how to get the rest. A silent cut (the duckbox's
	// dotted middle) is what a reader that cannot ask for more acts on as if it were the whole result.
	normalMode = cMode = mode = RenderMode::MARKDOWN;
	max_rows = AGENT_MAX_ROWS;
	max_bytes = AGENT_MAX_BYTES;
	max_cell_width = AGENT_MAX_CELL_WIDTH;
}

void ShellState::PrintAgentHelp(PrintOutput output, bool startup) {
	if (startup) {
		// one line, on every run: that the shell switched modes on its own, why, how to undo it, where the rest is.
		// A model keeps its context between runs, so the explanation is paid for once through .help agent
		if (agent_marker.empty()) {
			PrintF(output, "duckdb agent mode on (%s); .help agent explains the output\n",
			       agent_mode_from_environment ? "DUCKDB_AGENT_MODE is set" : "-agent");
		} else {
			PrintF(output,
			       "duckdb agent mode on: %s is set and stdout is not a terminal; -no-agent turns it off, .help agent "
			       "explains the output\n",
			       agent_marker);
		}
		return;
	}
	auto rows = agent_mode_active ? max_rows : AGENT_MAX_ROWS;
	auto bytes = agent_mode_active ? max_bytes : AGENT_MAX_BYTES;
	auto cell = agent_mode_active ? max_cell_width : AGENT_MAX_CELL_WIDTH;
	PrintF(output, "agent mode: %s\n", agent_mode_active ? (agent_name.empty() ? "on" : agent_name.c_str()) : "off");
	PrintF(output,
	       "output: markdown tables; a result of up to %zu rows and %zu bytes is rendered whole, a larger one as its "
	       "first and last rows with the count (.maxrows N, .maxbytes N; -1 / 0 = all); cells are cut at %zu chars "
	       "(.maxcellwidth N); the footer has the row count and, when the whole result was read, an "
	       "order-independent hash of it; errors are JSON on stderr; a query that reads over %zu rows gets an "
	       "estimate line and progress lines on stderr\n",
	       rows, bytes, cell, (size_t)ESTIMATE_MIN_ROWS);
	PrintF(output,
	       "tips: SET max_execution_time=<ms> bounds a query; DESCRIBE <query> gives the result columns "
	       "without running it; SUMMARIZE <table>; .tables; duckdb_functions() has descriptions and examples\n");
	PrintF(output,
	       "switches: -agent / -no-agent force the mode, and so does DUCKDB_AGENT_MODE=1 / 0 in the environment "
	       "(the flags win); an output mode flag (-csv, -json, ...) leaves it off; .startup_text none in "
	       "~/.duckdbrc hides the startup line\n");
}

void ShellState::PrintExitHint(int rc) {
	// a failed run through a pipe may well be a coding agent that does not know the mode exists - but only when
	// nothing identified one (the environment, -agent) and nothing declined (-no-agent)
	if (rc == 0 || stdout_is_console || agent_mode_active || agent_mode != OptionType::DEFAULT || exit_hint_printed) {
		return;
	}
	exit_hint_printed = true;
	string name, marker;
	if (DetectAgentEnvironment(name, marker)) {
		// an agent that chose its output format itself (-csv, ...) needs no hint
		return;
	}
	PrintF(PrintOutput::STDERR,
	       "hint: -agent renders errors as JSON and results compactly for AI coding agents (duckdb -help lists all "
	       "options)\n");
}

struct ScanEstimate {
	string name;
	idx_t rows;
};

static void CollectScanEstimates(duckdb::ClientContext &context, duckdb::LogicalOperator &op,
                                 vector<ScanEstimate> &scans) {
	if (op.type == duckdb::LogicalOperatorType::LOGICAL_GET) {
		auto &get = op.Cast<duckdb::LogicalGet>();
		ScanEstimate scan;
		auto table = get.GetTable();
		scan.name =
		    table ? table->name.GetIdentifierName() : StringUtil::Lower(get.function.GetName().GetIdentifierName());
		scan.rows = get.EstimateCardinality(context);
		scans.push_back(std::move(scan));
	}
	for (auto &child : op.children) {
		CollectScanEstimates(context, *child, scans);
	}
}

void ShellState::PrintQueryEstimate(const string &sql, const duckdb::SQLStatement &statement) {
	switch (statement.type) {
	case duckdb::StatementType::SELECT_STATEMENT:
	case duckdb::StatementType::INSERT_STATEMENT:
	case duckdb::StatementType::UPDATE_STATEMENT:
	case duckdb::StatementType::DELETE_STATEMENT:
	case duckdb::StatementType::CREATE_STATEMENT:
	case duckdb::StatementType::COPY_STATEMENT:
		break;
	default:
		// nothing to estimate
		return;
	}
	try {
		auto &context = *conn->context;
		auto plan = context.ExtractPlan(sql);
		vector<ScanEstimate> scans;
		CollectScanEstimates(context, *plan, scans);
		if (scans.empty()) {
			// e.g. a plain CREATE TABLE - not worth a line
			return;
		}
		idx_t rows_read = 0;
		for (auto &scan : scans) {
			rows_read += scan.rows;
		}
		if (rows_read < ESTIMATE_MIN_ROWS) {
			// the line exists so that a reader can decide whether to wait; a small read is not worth it
			return;
		}
		std::sort(scans.begin(), scans.end(),
		          [](const ScanEstimate &a, const ScanEstimate &b) { return a.rows > b.rows; });
		idx_t total_rows = 0;
		vector<string> scan_texts;
		for (idx_t i = 0; i < scans.size(); i++) {
			total_rows += scans[i].rows;
			if (i < 5) {
				scan_texts.push_back(StringUtil::Format("%s ~%llu", scans[i].name, scans[i].rows));
			}
		}
		if (scans.size() > 5) {
			scan_texts.push_back(StringUtil::Format("%llu more", scans.size() - 5));
		}
		string line =
		    StringUtil::Format("estimate: ~%llu rows read (%s)", total_rows, StringUtil::Join(scan_texts, ", "));
		if (statement.type == duckdb::StatementType::SELECT_STATEMENT) {
			line += StringUtil::Format(", ~%llu rows returned", plan->EstimateCardinality(context));
		}
		PrintF(PrintOutput::STDERR, "%s\n", line);
	} catch (std::exception &) {
		// planning failed - the real execution reports the error
	}
}

void ShellState::DetectDarkLightMode() {
#ifdef HAVE_LINENOISE
	ShellHighlight highlight(*this);
	if (highlight_mode != HighlightMode::AUTOMATIC) {
		// highlight mode is specified by the user - avoid setting manually
		return;
	}
	if (!stdout_is_console) {
		// not printing to console - don't auto-detect
		return;
	}
	// detect terminal colors
	auto terminal_color = linenoiseGetTerminalColorMode();
	if (terminal_color == LINENOISE_DARK_MODE) {
		highlight_mode = HighlightMode::DARK_MODE;
		highlight.ToggleMode(HighlightMode::DARK_MODE);
	} else if (terminal_color == LINENOISE_LIGHT_MODE) {
		highlight_mode = HighlightMode::LIGHT_MODE;
		highlight.ToggleMode(HighlightMode::LIGHT_MODE);
	} else {
		highlight_mode = HighlightMode::MIXED_MODE;
	}
#endif
}

struct ShellHistory {
	ShellHistory(ShellState &state, const char *path) : state(state), path(path) {
	}
	~ShellHistory() {
		if (path) {
			state.ShellSetHistoryMaxLength(2000);
			state.ShellSaveHistory(path);
		}
	}
	ShellState &state;
	const char *path;
};
static bool IsStdStreamAConsole(int std_fd) {
	if (!isatty(std_fd)) {
		return false;
	}
#if defined(_WIN32) || defined(WIN32)
	DWORD std_handle_type = -1;
	switch (std_fd) {
	case 0:
		std_handle_type = STD_INPUT_HANDLE;
		break;
	case 1:
		std_handle_type = STD_OUTPUT_HANDLE;
		break;
	case 2:
		std_handle_type = STD_ERROR_HANDLE;
		break;
	default:
		return true;
	}
	HANDLE handle = GetStdHandle(std_handle_type);
	if (handle == INVALID_HANDLE_VALUE) {
		return true;
	}
	DWORD mode;
	// false if it's a character device but NOT a console window, like NUL
	return GetConsoleMode(handle, &mode);
#else
	return true;
#endif
}

int RunShell(int argc, const char **argv) {
	int rc = 0;
	vector<string> extra_commands;

	auto &data = ShellState::Get();
	data.out = stdout;

	setBinaryMode(stdin, 0);
	setvbuf(stderr, 0, _IONBF, 0); /* Make sure stderr is unbuffered */
	data.stdin_is_interactive = IsStdStreamAConsole(0);
	data.stdout_is_console = IsStdStreamAConsole(1);
	data.stderr_is_console = IsStdStreamAConsole(2);

	data.Initialize();

	/* On Windows, we must translate command-line arguments into UTF-8.
	 */

	assert(argc >= 1 && argv && argv[0]);
	data.program_name = argv[0];

	/* Make sure we have a valid signal handler early, before anything
	** else is done.
	*/
#ifdef SIGINT
	signal(SIGINT, InterruptHandler);
#elif (defined(_WIN32) || defined(WIN32)) && !defined(_WIN32_WCE)
	SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
#endif

	/* Do an initial pass through the command-line argument to locate
	** the name of the database file, the name of the initialization file,
	** the size of the alternative malloc heap,
	** and the first command to execute.
	*/
	vector<CommandLineCall> command_line_calls;
	for (int i = 1; i < argc; i++) {
		auto z = argv[i];
		if (z[0] != '-') {
			if (data.zDbFilename.empty()) {
				data.zDbFilename = z;
			} else {
				/* Excess arguments are interpreted as SQL (or dot-commands) and
				** mean that nothing is read from stdin */
				data.readStdin = false;
				data.stdin_is_interactive = false;
				extra_commands.emplace_back(z);
			}
			continue;
		}
		z++;
		if (z[0] == '-') {
			// allow for double dashes, i.e. --init and -init are both valid
			z++;
		}

		string error_msg;
		auto command_line_option = data.FindCommandLineOption(z, error_msg);
		if (!command_line_option) {
			data.PrintDatabaseError(error_msg);
			return 1;
		}
		auto &option = *command_line_option;
		// parse arguments
		vector<string> arguments;
		arguments.push_back(option.option);
		for (idx_t arg_idx = 0; arg_idx < option.argument_count; arg_idx++) {
			if (i + 1 >= argc) {
				string error =
				    StringUtil::Format("Missing Argument Error: Argument '-%s' needs %llu arguments, but got %llu\n",
				                       option.option, option.argument_count, arg_idx);
				error += StringUtil::Format("OPTION:\n  -%s %s    %s\n\n", option.option, option.arguments,
				                            option.description);
				error += StringUtil::Format("Run '%s -help' for a list of options.\n", data.program_name);
				data.PrintDatabaseError(error);
				return 1;
			}
			arguments.emplace_back(argv[++i]);
		}
		if (option.optional_argument && i + 1 < argc && argv[i + 1][0] != '-') {
			// an optional argument is only consumed when it is not an option itself
			arguments.emplace_back(argv[++i]);
		}
		if (option.pre_init_callback) {
			// invoke the pre-init callback (if any)
			auto result = option.pre_init_callback(data, arguments);
			if (result == MetadataResult::EXIT) {
				return 0;
			}
		}
		// add the call to the list of options to handle
		command_line_calls.emplace_back(option, std::move(arguments));
	}

	if (data.started_as_client && !data.zDbFilename.empty()) {
		// checked before OpenDB, so that a database that does not exist yet is not created either
		data.PrintDatabaseError(
		    StringUtil::Format("Invalid Input Error: cannot open a database (%s) together with -connect\n"
		                       "-connect uses the database of the server it connects to",
		                       data.zDbFilename));
		return 1;
	}
	if (data.zDbFilename.empty()) {
		data.zDbFilename = ":memory:";
	}
	data.out = stdout;
	data.DetectAgentMode();

	// Open the database file
	data.OpenDB();

	/* Process the initialization file if there is one.  If no -init option
	** is given on the command line, look for a file named ~/.duckdbrc and
	** try to process it.
	*/
	if (data.run_init && !data.ProcessDuckDBRC(data.initFile.empty() ? nullptr : data.initFile.c_str())) {
		// failed to process init file - check if we should bail
		bool bail_on_init_fail = data.bail != BailOnError::DONT_BAIL_ON_ERROR;
		if (bail_on_init_fail) {
			if (!data.duckdb_rc_path.empty()) {
				data.PrintDatabaseError("Encountered errors while executing init file \"" + data.duckdb_rc_path +
				                        "\". Exiting.");
			}
			return 1;
		}
	}

	if (data.agent_mode_active && data.startup_text != StartupText::NONE) {
		// before anything runs (after the init file, so that .startup_text none in ~/.duckdbrc applies)
		data.PrintAgentHelp(PrintOutput::STDERR, true);
	}

	data.DetectDarkLightMode();

	/* Make a second pass through the command-line argument and set
	** options.  This second pass is delayed until after the initialization
	** file is processed so that the command-line arguments will override
	** settings in the initialization file.
	*/
	for (auto &call : command_line_calls) {
		auto &option = call.option;
		if (!option.post_init_callback) {
			continue;
		}
		auto result = option.post_init_callback(data, call.arguments);
		if (result == MetadataResult::FAIL) {
			// Return through main so database owners are destroyed before process-wide cleanup.
			return 1;
		}
		if (result == MetadataResult::EXIT) {
			return 0;
		}
	}

	if (!data.readStdin) {
		/* Run all arguments that do not begin with '-' as if they were separate
		** command-line inputs, except for the argToSkip argument which contains
		** the database filename.
		*/
		for (auto &cmd : extra_commands) {
			if (cmd[0] == '.') {
				rc = data.DoMetaCommand(cmd);
				if (rc) {
					return rc == 2 ? 0 : rc;
				}
			} else {
				auto success = data.ExecuteSQL(cmd.c_str());
				if (success != SuccessState::SUCCESS) {
					return rc != 0 ? rc : 1;
				}
			}
		}
	} else {
		/* Run commands received from standard input
		 */
		if (data.stdin_is_interactive) {
			string zHome;
			const char *zHistory;
			ShellHighlight highlight(data);

			auto startup_version = StringUtil::Format("DuckDB %s (%s", duckdb::DuckDB::LibraryVersion(),
			                                          duckdb::DuckDB::ReleaseCodename());
			if (StringUtil::Contains(duckdb::DuckDB::ReleaseCodename(), "Development")) {
				startup_version += ", ";
				startup_version += duckdb::DuckDB::SourceID();
			}
			startup_version += ")\n";
			if (data.startup_text != StartupText::NONE) {
				highlight.PrintText(startup_version, PrintOutput::STDOUT, HighlightElementType::STARTUP_VERSION);
			}
			if (data.startup_text == StartupText::ALL) {
				highlight.PrintText("Enter \".help\" for usage hints.\n", PrintOutput::STDOUT,
				                    HighlightElementType::STARTUP_TEXT);
			}
			zHistory = getenv("DUCKDB_HISTORY");
			if (!zHistory) {
				zHome = GetHomeDirectory() + "/.duckdb_history";
				zHistory = zHome.c_str();
			}
			if (zHistory) {
				data.ShellLoadHistory(zHistory);
			}
#ifdef HAVE_LINENOISE
			if (data.rl_version == ReadLineVersion::LINENOISE) {
				linenoiseSetCompletionCallback(linenoise_completion);
				linenoiseSetFormatCallback(linenoise_format);
			}
#endif
			ShellHistory history(data, zHistory);
			data.in = 0;
			rc = data.ProcessInput(InputMode::STANDARD);
		} else {
			data.in = stdin;
			rc = data.ProcessInput(InputMode::STANDARD);
		}
	}
	return rc;
}
