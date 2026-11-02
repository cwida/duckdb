#include "linenoise.hpp"
#include "linenoise.h"
#include "history.hpp"
#include "terminal.hpp"
#include "highlighting.hpp"
#include "duckdb/common/string_util.hpp"
#include "shell_highlight.hpp"

#if defined(_WIN32) || defined(WIN32)
#define strdup _strdup
#endif

using duckdb::Highlighting;
using duckdb::History;
using duckdb::idx_t;
using duckdb::Linenoise;
using duckdb::Terminal;

/* The high level function that is the main API of the linenoise library.
 * This function checks if the terminal has basic capabilities, just checking
 * for a blacklist of stupid terminals, and later either calls the line
 * editing function or uses dummy fgets() so that you will be able to type
 * something even in the most desperate of the conditions. */
char *linenoise(const char *prompt) {
	char buf[LINENOISE_MAX_LINE];
	int count;

	if (!Terminal::IsAtty()) {
		/* Not a tty: read from file / pipe. In this mode we don't want any
		 * limit to the line size, so we call a function to handle that. */
		return Terminal::EditNoTTY();
	} else if (Terminal::IsUnsupportedTerm()) {
		size_t len;

		printf("%s", prompt);
		fflush(stdout);
		if (fgets(buf, LINENOISE_MAX_LINE, stdin) == NULL) {
			return NULL;
		}
		len = strlen(buf);
		while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
			len--;
			buf[len] = '\0';
		}
		return strdup(buf);
	} else {
		count = Terminal::EditRaw(buf, LINENOISE_MAX_LINE, prompt);
		if (count == -1) {
			return NULL;
		}
		return strdup(buf);
	}
}

/* This is just a wrapper the user may want to call in order to make sure
 * the linenoise returned buffer is freed with the same allocator it was
 * created with. Useful when the main program is using an alternative
 * allocator. */
void linenoiseFree(void *ptr) {
	free(ptr);
}

/* ================================ History ================================= */

/* This is the API call to add a new entry in the linenoise history.
 * It uses a fixed array of char pointers that are shifted (memmoved)
 * when the history max length is reached in order to remove the older
 * entry and make room for the new one, so it is not exactly suitable for huge
 * histories, but will work well for a few hundred of entries.
 *
 * Using a circular buffer is smarter, but a bit more complex to handle. */
int linenoiseHistoryAdd(const char *line) {
	return History::Add(line);
}

/* Set the maximum length for the history. This function can be called even
 * if there is already some history, the function will make sure to retain
 * just the latest 'len' elements if the new history length value is smaller
 * than the amount of items already inside the history. */
int linenoiseHistorySetMaxLen(int len) {
	if (len < 0) {
		return 0;
	}
	return History::SetMaxLength(idx_t(len));
}

/* Save the history in the specified file. On success 0 is returned
 * otherwise -1 is returned. */
int linenoiseHistorySave(const char *filename) {
	return History::Save(filename);
}

/* Load the history from the specified file. If the file does not exist
 * zero is returned and no operation is performed.
 *
 * If the file exists and the operation succeeded 0 is returned, otherwise
 * on error -1 is returned. */
int linenoiseHistoryLoad(const char *filename) {
	return History::Load(filename);
}

/* Register a callback function to be called for tab-completion. */
void linenoiseSetCompletionCallback(linenoiseCompletionCallback *fn) {
	Linenoise::SetCompletionCallback(fn);
}

/* Register a callback function to be called to format the input before returning it to the shell. */
void linenoiseSetFormatCallback(linenoiseFormatCallback *fn) {
	Linenoise::SetFormatCallback(fn);
}

void linenoiseSetMultiLine(int ml) {
	Terminal::SetMultiLine(ml);
}

void linenoiseSetErrorRendering(int enabled) {
	if (enabled) {
		Linenoise::EnableErrorRendering();
	} else {
		Linenoise::DisableErrorRendering();
	}
}

void linenoiseSetCompletionRendering(int enabled) {
	if (enabled) {
		Linenoise::EnableCompletionRendering();
	} else {
		Linenoise::DisableCompletionRendering();
	}
}

void linenoiseSetPrompt(const char *continuation, const char *continuationSelected, const char *scrollUp,
                        const char *scrollDown) {
	Linenoise::SetPrompt(continuation, continuationSelected, scrollUp, scrollDown);
}

/* This function is used by the callback function registered by the user
 * in order to add completion options given the input string when the
 * user typed <tab>. See the example.c source code for a very easy to
 * understand example. */
void linenoiseAddCompletion(linenoiseCompletions *lc, const char *zLine, const char *completion, size_t nCompletion,
                            size_t completion_start, const char *completion_type, size_t score, char extra_char) {
	auto &completions = *reinterpret_cast<duckdb::TabCompletion *>(lc);
	duckdb::Completion c;
	c.original_completion = duckdb::string(completion, nCompletion);
	c.completion.reserve(completion_start + nCompletion + 1);
	c.completion += duckdb::string(zLine, completion_start);
	c.completion += c.original_completion;
	c.completion_type = Linenoise::GetCompletionType(completion_type);
	c.cursor_pos = c.completion.size();
	c.score = score;
	c.extra_char = extra_char;
	completions.completions.push_back(std::move(c));
}

size_t linenoiseComputeRenderWidth(const char *buf, size_t len) {
	return Linenoise::ComputeRenderWidth(buf, len);
}

int linenoiseGetRenderPosition(const char *buf, size_t len, int max_width, int *n) {
	return Linenoise::GetRenderPosition(buf, len, max_width, n);
}

void linenoiseClearScreen(void) {
	Terminal::ClearScreen();
}

int linenoiseGetTerminalColorMode() {
	// buffer any available input - we need to interact with stdin
	Terminal::BufferAvailableInput();

	duckdb::TerminalColor background_color;
	if (!duckdb::Terminal::TryGetBackgroundColor(background_color)) {
		return LINENOISE_UNKNOWN_MODE;
	}
	// calculate the brightness
	double brightness = 0.2126 * background_color.r + 0.7152 * background_color.g + 0.0722 * background_color.b;
	// determine light or dark mode based on the brightness
	// if the value is too much in the middle we leave it as mixed
	if (brightness <= 96) {
		return LINENOISE_DARK_MODE;
	}
	if (brightness >= 160) {
		return LINENOISE_LIGHT_MODE;
	}
	return LINENOISE_MIXED_MODE;
}
