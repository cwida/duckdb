#if defined(_WIN32) || defined(WIN32)
#include <io.h>
#include <process.h>
#define getpid _getpid
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <unistd.h>
#endif
#include "linenoise.h"
#include "linenoise.hpp"
#include "shortcuts.hpp"
#include "history.hpp"
#include "highlighting.hpp"
#include "terminal.hpp"
#include "utf8proc_wrapper.hpp"
#include <unordered_set>
#include <vector>
#include <string>
#include "duckdb/common/string_util.hpp"
#include "shell_state.hpp"
#ifdef __MVS__
#include <strings.h>
#include <sys/time.h>
#endif

#ifdef LINENOISE_EDITOR
#if defined(WIN32) || defined(__CYGWIN__)
#define DEFAULT_EDITOR "notepad.exe"
#else
#define DEFAULT_EDITOR "vi"
#endif
#endif

namespace duckdb {

static linenoiseCompletionCallback *completionCallback = NULL;
static linenoiseFormatCallback *formatCallback = NULL;

int linenoiseHistoryAdd(const char *line);

/* ============================== Completion ================================ */

/* Register a callback function to be called for tab-completion. */
void Linenoise::SetCompletionCallback(linenoiseCompletionCallback *fn) {
	completionCallback = fn;
}

/* Register a callback function to be called to format the input before returning it to the shell. */
void Linenoise::SetFormatCallback(linenoiseFormatCallback *fn) {
	formatCallback = fn;
}

void Linenoise::Format() {
	if (!formatCallback) {
		return;
	}
	buf[len] = '\0';
	char *formatted = formatCallback(buf);
	if (!formatted) {
		return;
	}
	// The formatter returns \n line endings, but linenoise multiline mode uses \r\n
	// internally. Convert \n -> \r\n so RefreshLine() renders correctly.
	// The \r\n -> \n rewrite in Edit() will convert them back before returning.
	string converted;
	for (const char *p = formatted; *p; p++) {
		if (*p == '\n' && (p == formatted || *(p - 1) != '\r')) {
			converted += '\r';
		}
		converted += *p;
	}
	free(formatted);
	if (converted.size() >= buflen) {
		return;
	}
	memcpy(buf, converted.c_str(), converted.size() + 1);
	len = converted.size();
	pos = len;
	RefreshLine();
}

CompletionType Linenoise::GetCompletionType(const char *type) {
	if (StringUtil::Equals(type, "keyword")) {
		return CompletionType::KEYWORD;
	}
	if (StringUtil::Equals(type, "catalog")) {
		return CompletionType::CATALOG_NAME;
	}
	if (StringUtil::Equals(type, "schema")) {
		return CompletionType::SCHEMA_NAME;
	}
	if (StringUtil::Equals(type, "table")) {
		return CompletionType::TABLE_NAME;
	}
	if (StringUtil::Equals(type, "column")) {
		return CompletionType::COLUMN_NAME;
	}
	if (StringUtil::Equals(type, "type")) {
		return CompletionType::TYPE_NAME;
	}
	if (StringUtil::Equals(type, "file_name")) {
		return CompletionType::FILE_NAME;
	}
	if (StringUtil::Equals(type, "directory")) {
		return CompletionType::DIRECTORY_NAME;
	}
	if (StringUtil::Equals(type, "scalar_function")) {
		return CompletionType::SCALAR_FUNCTION;
	}
	if (StringUtil::Equals(type, "table_function")) {
		return CompletionType::TABLE_FUNCTION;
	}
	if (StringUtil::Equals(type, "setting")) {
		return CompletionType::SETTING_NAME;
	}
	return CompletionType::UNKNOWN;
}

TabCompletion Linenoise::TabComplete() const {
	TabCompletion result;
	if (!completionCallback) {
		return result;
	}
	// complete based on the cursor position
	auto prev_char = buf[pos];
	buf[pos] = '\0';
	completionCallback(buf, reinterpret_cast<linenoiseCompletions *>(&result));
	buf[pos] = prev_char;
	for (auto &completion : result.completions) {
		idx_t copy_offset = pos;
		if (completion.extra_char != '\0') {
			if (buf[pos] == completion.extra_char) {
				// if the buffer already has the extra char at this position do not add it again
				// i.e. if we are completing 'file.parq[CURSOR]' - complete to 'file.parquet' and not 'file.parquet''
				copy_offset++;
			}
			completion.extra_char_pos = completion.completion.size() - 1;
		}
		completion.completion += buf + copy_offset;
	}
	return result;
}

/* This is an helper function for linenoiseEdit() and is called when the
 * user types the <tab> key in order to complete the string currently in the
 * input.
 *
 * The state of the editing is encapsulated into the pointed linenoiseState
 * structure as described in the structure definition. */
bool Linenoise::CompleteLine(KeyPress &next_key) {
	next_key.action = KEY_NULL;
	completion_list = TabComplete();
	auto &completions = completion_list.completions;
	// we only start rendering completion suggestions once we start tabbing through them
	render_completion_suggestion = false;
	if (completions.empty()) {
		Terminal::Beep();
	} else {
		bool stop = false;
		bool accept_completion = false;
		// check if there are ties between completions
		bool has_ties = false;
		if (completion_list.completions.size() > 1) {
			has_ties = completion_list.completions[0].score == completion_list.completions[1].score;
		}
		if (has_ties) {
			// if there are ties we don't auto-complete immediately
			// instead we display the list of suggestions
			// but first, complete up to the longest common prefix (like shell behavior)
			auto &first = completions[0].completion;
			// only consider the completed part - the text after the cursor is shared by all completions
			idx_t common_len = first.size();
			for (auto &completion : completions) {
				common_len = MinValue<idx_t>(common_len, completion.cursor_pos);
			}
			for (idx_t i = 1; i < completions.size(); i++) {
				auto &other = completions[i].completion;
				idx_t min_len = MinValue<idx_t>(common_len, other.size());
				idx_t j;
				for (j = 0; j < min_len; j++) {
					if (first[j] != other[j]) {
						break;
					}
				}
				common_len = j;
			}
			if (common_len > (idx_t)pos) {
				// there is a common prefix longer than the input before the cursor - apply it
				string suffix(buf + pos, len - pos);
				int nwritten = snprintf(buf, buflen, "%.*s%s", (int)common_len, first.c_str(), suffix.c_str());
				len = MinValue<idx_t>((idx_t)nwritten, buflen - 1);
				pos = MinValue<idx_t>(common_len, len);
			}
			completion_idx = optional_idx();
			render_completion_suggestion = true;
		} else {
			// if there are no ties we immediately accept the first completion suggestion
			completion_idx = 0;
		}

		idx_t action_count = 0;
		while (!stop) {
			action_count++;
			HandleTerminalResize();
			/* Show completion or original buffer */
			if (completion_idx.IsValid()) {
				Linenoise saved = *this;

				auto &completion = completions[completion_idx.GetIndex()];
				len = completion.completion.size();
				pos = completion.cursor_pos;
				buf = (char *)completion.completion.c_str();
				RefreshLine();
				len = saved.len;
				pos = saved.pos;
				buf = saved.buf;
			} else {
				RefreshLine();
			}

			KeyPress key_press;
			if (!TryGetKeyPress(ifd, key_press)) {
				// no longer completing - clear list of completions
				completion_list.completions.clear();
				return false;
			}

			Linenoise::Log("\nComplete Character %d\n", (int)key_press.action);
			switch (key_press.action) {
			case TAB: /* tab */
				if (action_count == 1 && !has_ties) {
					// if we had an "instant complete" as we had a clear winner - tab complete again from this position
					// instead of cycling through this series of completions
					next_key = key_press;
					accept_completion = true;
					stop = true;
					break;
				}
				if (!completion_idx.IsValid()) {
					completion_idx = 0;
				} else {
					completion_idx = (completion_idx.GetIndex() + 1) % completions.size();
				}
				render_completion_suggestion = true;
				break;
			case CTRL_C:
				// ctrl + c cancels auto-complete
				next_key.action = KEY_NULL;
				accept_completion = false;
				stop = true;
				break;
			case ESC: { /* escape */
				switch (key_press.sequence) {
				case EscapeSequence::SHIFT_TAB:
					// shift-tab: move backwards
					if (!completion_idx.IsValid()) {
						// pressing shift-tab when we don't have a selected completion means we abort searching
						RefreshLine();
						next_key.action = KEY_NULL;
						stop = true;
					} else if (completion_idx.GetIndex() == 0) {
						completion_idx = optional_idx();
					} else {
						completion_idx = completion_idx.GetIndex() - 1;
					}
					render_completion_suggestion = true;
					break;
				case EscapeSequence::ESCAPE:
					/* Re-show original buffer */
					RefreshLine();
					next_key = key_press;
					stop = true;
					break;
				default:
					next_key = key_press;
					accept_completion = true;
					stop = true;
					break;
				}
				break;
			}
			default:
				next_key = key_press;
				accept_completion = true;
				stop = true;
				break;
			}
		}
		if (accept_completion && completion_idx.IsValid()) {
			auto &accepted_completion = completions[completion_idx.GetIndex()];
			if (accepted_completion.extra_char != '\0' && next_key.action == accepted_completion.extra_char) {
				next_key.action = KEY_NULL;
			}
			/* Update buffer and return */
			int nwritten = snprintf(buf, buflen, "%s", accepted_completion.completion.c_str());
			pos = accepted_completion.cursor_pos;
			len = nwritten;
		}
	}
	// no longer completing - clear list of completions
	completion_list.completions.clear();
	completion_idx = optional_idx();
	return true; /* Return last read character */
}

bool Linenoise::HandleANSIEscape(const char *buf, size_t len, size_t &cpos) {
	// --- Handle ANSI escape sequences ---
	if (buf[cpos] != '\033') {
		// not an escape sequence
		return false;
	}
	cpos++;
	if (cpos < len && buf[cpos] == '[') { // CSI sequence
		cpos++;
		while (cpos < len && !(buf[cpos] >= '@' && buf[cpos] <= '~')) {
			cpos++;
		}
		if (cpos < len)
			cpos++; // skip final letter
	} else {
		// standalone ESC
		cpos++;
	}
	return true;
}

size_t Linenoise::ComputeRenderWidth(const char *buf, size_t len) {
	// utf8 in prompt, get render width
	size_t cpos = 0;
	size_t render_width = 0;
	int sz;
	while (cpos < len) {
		// --- 1. Handle newline ---
		if (buf[cpos] == '\n') {
			render_width = 0; // reset width for new line
			cpos++;
			continue;
		}

		// --- 2. Handle tab (optional, usually 8-space tab stops) ---
		if (buf[cpos] == '\t') {
			render_width += 8 - (render_width % 8);
			cpos++;
			continue;
		}

		// --- 3. Handle ANSI escape sequences ---
		if (HandleANSIEscape(buf, len, cpos)) {
			continue;
		}

		// --- 4. Handle UTF-8 grapheme clusters ---
		int codepoint = duckdb::Utf8Proc::UTF8ToCodepoint(buf + cpos, sz, len - cpos);
		if (codepoint < 0) {
			// invalid byte, treat as width 1
			cpos++;
			render_width++;
			continue;
		}

		size_t char_render_width = duckdb::Utf8Proc::RenderWidth(buf, len, cpos);
		cpos = duckdb::Utf8Proc::NextGraphemeCluster(buf, len, cpos);
		render_width += char_render_width;
	}
	return render_width;
}

int Linenoise::GetPromptWidth() const {
	return int(ComputeRenderWidth(prompt, strlen(prompt)));
}

int Linenoise::GetRenderPosition(const char *buf, size_t len, int max_width, int *n) {
	if (duckdb::Utf8Proc::IsValid(buf, len)) {
		// utf8 in prompt, get render width
		size_t cpos = 0;
		size_t render_width = 0;
		while (cpos < len) {
			size_t char_render_width = duckdb::Utf8Proc::RenderWidth(buf, len, cpos);
			if (int(render_width + char_render_width) > max_width) {
				*n = render_width;
				return cpos;
			}
			cpos = duckdb::Utf8Proc::NextGraphemeCluster(buf, len, cpos);
			render_width += char_render_width;
		}
		*n = render_width;
		return len;
	} else {
		// invalid utf8, return -1
		return -1;
	}
}

bool Linenoise::IsNewline(char c) {
	return c == '\r' || c == '\n';
}

void Linenoise::NextPosition(const char *buf, size_t len, size_t &cpos, int &rows, int &cols, int plen) const {
	if (IsNewline(buf[cpos])) {
		// explicit newline! move to next line and insert a prompt
		rows++;
		cols = plen;
		cpos++;
		if (buf[cpos - 1] == '\r' && cpos < len && buf[cpos] == '\n') {
			cpos++;
		}
		return;
	}
	if (HandleANSIEscape(buf, len, cpos)) {
		return;
	}
	int sz;
	int char_render_width;
	if (duckdb::Utf8Proc::UTF8ToCodepoint(buf + cpos, sz, len - cpos) < 0) {
		char_render_width = 1;
		cpos++;
	} else {
		char_render_width = (int)duckdb::Utf8Proc::RenderWidth(buf, len, cpos);
		cpos = duckdb::Utf8Proc::NextGraphemeCluster(buf, len, cpos);
	}
	if (cols + char_render_width > ws.ws_col) {
		// exceeded l->cols, move to next row
		rows++;
		cols = char_render_width;
	}
	cols += char_render_width;
}

void Linenoise::PositionToColAndRow(int prompt_len, const char *render_buf, idx_t render_len, size_t target_pos,
                                    int &out_row, int &out_col, int &rows, int &cols) const {
	out_row = -1;
	out_col = 0;
	rows = 1;
	cols = prompt_len;
	size_t cpos = 0;
	while (cpos < render_len) {
		if (cols >= ws.ws_col && !IsNewline(render_buf[cpos])) {
			// exceeded width - move to next line
			rows++;
			cols = 0;
		}
		if (out_row < 0 && cpos >= target_pos) {
			out_row = rows;
			out_col = cols;
		}
		NextPosition(render_buf, render_len, cpos, rows, cols, prompt_len);
	}
	if (target_pos == render_len) {
		out_row = rows;
		out_col = cols;
	}
}

void Linenoise::PositionToColAndRow(size_t target_pos, int &out_row, int &out_col, int &rows, int &cols) const {
	int plen = GetPromptWidth();
	PositionToColAndRow(plen, buf, len, target_pos, out_row, out_col, rows, cols);
}

size_t Linenoise::ColAndRowToPosition(int plen, const char *buf, idx_t len, int target_row, int target_col) const {
	int rows = 1;
	int cols = plen;
	size_t last_cpos = 0;
	size_t cpos = 0;
	while (cpos < len) {
		if (cols >= ws.ws_col) {
			// exceeded width - move to next line
			rows++;
			cols = 0;
		}
		if (rows > target_row) {
			// we have skipped our target row - that means "target_col" was out of range for this row
			// return the last position within the target row
			return last_cpos;
		}
		if (rows == target_row) {
			last_cpos = cpos;
		}
		if (rows == target_row && cols == target_col) {
			return cpos;
		}
		NextPosition(buf, len, cpos, rows, cols, plen);
	}
	return cpos;
}

size_t Linenoise::ColAndRowToPosition(int target_row, int target_col) const {
	int plen = GetPromptWidth();
	return ColAndRowToPosition(plen, buf, len, target_row, target_col);
}

/* Insert the character 'c' at cursor current position.
 *
 * On error writing to the terminal -1 is returned, otherwise 0. */
void Linenoise::InsertCharacter(char c) {
	if (len < buflen) {
		if (len == pos) {
			buf[pos] = c;
			pos++;
			len++;
			buf[len] = '\0';
		} else {
			memmove(buf + pos + 1, buf + pos, len - pos);
			buf[pos] = c;
			len++;
			pos++;
			buf[len] = '\0';
		}
	}
}

int Linenoise::EditInsert(char c) {
	if (has_more_data) {
		render = false;
	}
	insert = true;
	InsertCharacter(c);
	RefreshLine();
	return 0;
}

int Linenoise::EditInsertMulti(const char *c) {
	for (size_t pos = 0; c[pos]; pos++) {
		InsertCharacter(c[pos]);
	}
	RefreshLine();
	return 0;
}

size_t Linenoise::PrevChar() const {
	return duckdb::Utf8Proc::PreviousGraphemeCluster(buf, len, pos);
}

size_t Linenoise::NextChar() const {
	return duckdb::Utf8Proc::NextGraphemeCluster(buf, len, pos);
}

/* Move cursor on the left. */
void Linenoise::EditMoveLeft() {
	if (pos > 0) {
		pos = PrevChar();
		RefreshLine();
	}
}

/* Move cursor on the right. */
void Linenoise::EditMoveRight() {
	if (pos != len) {
		pos = NextChar();
		RefreshLine();
	}
}

bool Linenoise::IsWordBoundary(char c) {
	if (c >= 'a' && c <= 'z') {
		return false;
	}
	if (c >= 'A' && c <= 'Z') {
		return false;
	}
	if (c >= '0' && c <= '9') {
		return false;
	}
	return true;
}

/* Move cursor to the next left word. */
void Linenoise::EditMoveWordLeft() {
	if (pos == 0) {
		return;
	}
	do {
		pos = PrevChar();
	} while (pos > 0 && !IsWordBoundary(buf[pos]));
	RefreshLine();
}

/* Move cursor on the right. */
void Linenoise::EditMoveWordRight() {
	if (pos == len) {
		return;
	}
	do {
		pos = NextChar();
	} while (pos != len && !IsWordBoundary(buf[pos]));
	RefreshLine();
}

/* Move cursor one row up. */
bool Linenoise::EditMoveRowUp() {
	if (!Terminal::IsMultiline()) {
		return false;
	}
	int rows, cols;
	int cursor_row, cursor_col;
	PositionToColAndRow(pos, cursor_row, cursor_col, rows, cols);
	if (cursor_row <= 1) {
		return false;
	}
	// we can move the cursor a line up
	Linenoise::Log("source pos %d", pos);
	Linenoise::Log("move from row %d to row %d", cursor_row, cursor_row - 1);
	cursor_row--;
	pos = ColAndRowToPosition(cursor_row, cursor_col);
	Linenoise::Log("new pos %d", pos);
	RefreshLine();
	return true;
}

/* Move cursor one row down. */
bool Linenoise::EditMoveRowDown() {
	if (!Terminal::IsMultiline()) {
		return false;
	}
	int rows, cols;
	int cursor_row, cursor_col;
	PositionToColAndRow(pos, cursor_row, cursor_col, rows, cols);
	if (cursor_row >= rows) {
		return false;
	}
	// we can move the cursor a line down
	Linenoise::Log("source pos %d", pos);
	Linenoise::Log("move from row %d to row %d", cursor_row, cursor_row + 1);
	cursor_row++;
	pos = ColAndRowToPosition(cursor_row, cursor_col);
	Linenoise::Log("new pos %d", pos);
	RefreshLine();
	return true;
}

/* Move cursor to the start of the buffer. */
void Linenoise::EditMoveHome() {
	if (pos != 0) {
		pos = 0;
		RefreshLine();
	}
}

/* Move cursor to the end of the buffer. */
void Linenoise::EditMoveEnd() {
	if (pos != len) {
		pos = len;
		RefreshLine();
	}
}

/* Move cursor to the start of the line. */
void Linenoise::EditMoveStartOfLine() {
	while (pos > 0 && buf[pos - 1] != '\n') {
		pos--;
	}
	RefreshLine();
}

/* Move cursor to the end of the line. */
void Linenoise::EditMoveEndOfLine() {
	while (pos < len && buf[pos + 1] != '\n') {
		pos++;
	}
	RefreshLine();
}

/* Substitute the currently edited line with the next or previous history
 * entry as specified by 'dir'. */
void Linenoise::EditHistoryNext(HistoryScrollDirection dir) {
	auto history_len = History::GetLength();
	if (history_len > 1) {
		/* Update the current history entry before to
		 * overwrite it with the next one. */
		History::Overwrite(history_len - 1 - history_index, buf);
		/* Show the new entry */
		switch (dir) {
		case HistoryScrollDirection::LINENOISE_HISTORY_PREV:
			// scroll back
			history_index++;
			if (history_index >= history_len) {
				history_index = history_len - 1;
				return;
			}
			break;
		case HistoryScrollDirection::LINENOISE_HISTORY_NEXT:
			// scroll forwards
			if (history_index == 0) {
				return;
			}
			history_index--;
			break;
		case HistoryScrollDirection::LINENOISE_HISTORY_END:
			history_index = 0;
			break;
		case HistoryScrollDirection::LINENOISE_HISTORY_START:
			history_index = history_len - 1;
			break;
		}
		strncpy(buf, History::GetEntry(history_len - 1 - history_index), buflen);
		buf[buflen - 1] = '\0';
		len = pos = strlen(buf);
		if (Terminal::IsMultiline() && dir == HistoryScrollDirection::LINENOISE_HISTORY_NEXT) {
			pos = ColAndRowToPosition(1, len);
		}
		RefreshLine();
	}
}

/* Delete the character at the right of the cursor without altering the cursor
 * position. Basically this is what happens with the "Delete" keyboard key. */
void Linenoise::EditDelete() {
	if (len > 0 && pos < len) {
		size_t new_pos = NextChar();
		size_t char_sz = new_pos - pos;
		memmove(buf + pos, buf + new_pos, len - new_pos);
		len -= char_sz;
		buf[len] = '\0';
		RefreshLine();
	}
}

/* Backspace implementation. */
void Linenoise::EditBackspace() {
	if (pos > 0 && len > 0) {
		size_t new_pos = PrevChar();
		size_t char_sz = pos - new_pos;
		memmove(buf + new_pos, buf + pos, len - pos);
		len -= char_sz;
		pos = new_pos;
		buf[len] = '\0';
		RefreshLine();
	}
}

bool Linenoise::IsSpace(char c) {
	switch (c) {
	case ' ':
	case '\r':
	case '\n':
	case '\t':
		return true;
	default:
		return false;
	}
}

/* Delete the previous word, maintaining the cursor at the start of the
 * current word. */
void Linenoise::EditDeletePrevWord() {
	size_t old_pos = pos;
	size_t diff;

	while (pos > 0 && IsSpace(buf[pos - 1])) {
		pos--;
	}
	while (pos > 0 && !IsSpace(buf[pos - 1])) {
		pos--;
	}
	diff = old_pos - pos;
	memmove(buf + pos, buf + old_pos, len - old_pos + 1);
	len -= diff;
	RefreshLine();
}

/* Delete the next word */
void Linenoise::EditDeleteNextWord() {
	size_t next_pos = pos;
	size_t diff;

	while (next_pos < len && IsSpace(buf[next_pos])) {
		next_pos++;
	}
	while (next_pos < len && !IsSpace(buf[next_pos])) {
		next_pos++;
	}
	diff = next_pos - pos;
	memmove(buf + pos, buf + next_pos, len - next_pos + 1);
	len -= diff;
	RefreshLine();
}

void Linenoise::EditRemoveSpaces() {
	size_t start_pos = pos;
	size_t end_pos = pos;
	size_t diff;

	while (start_pos > 0 && buf[start_pos - 1] == ' ') {
		start_pos--;
	}
	while (end_pos < len && buf[end_pos] == ' ') {
		end_pos++;
	}
	diff = end_pos - start_pos;
	memmove(buf + start_pos, buf + end_pos, len - end_pos + 1);
	len -= diff;
	pos = start_pos;
	RefreshLine();
}

void Linenoise::EditSwapCharacter() {
	if (pos == 0 || len < 2) {
		return;
	}
	char temp_buffer[128];
	if (pos + 1 > len) {
		pos = PrevChar();
	}
	int prev_pos = PrevChar();
	int next_pos = NextChar();
	int prev_char_size = pos - prev_pos;
	int cur_char_size = next_pos - pos;
	memcpy(temp_buffer, buf + prev_pos, prev_char_size);
	memmove(buf + prev_pos, buf + pos, cur_char_size);
	memcpy(buf + prev_pos + cur_char_size, temp_buffer, prev_char_size);
	pos = next_pos;
	RefreshLine();
}

void Linenoise::EditSwapWord() {
	idx_t current_pos = pos;
	idx_t left_word_start, left_word_end;
	idx_t right_word_start, right_word_end;

	// move to the end of the right word
	idx_t end_pos = pos;
	while (end_pos < len && IsSpace(buf[end_pos])) {
		end_pos++;
	}
	while (end_pos < len && !IsSpace(buf[end_pos])) {
		end_pos++;
	}
	current_pos = end_pos;
	while (current_pos > 0 && IsSpace(buf[current_pos - 1])) {
		current_pos--;
	}
	right_word_end = current_pos;
	// move to the start of the right word
	while (current_pos > 0 && !IsSpace(buf[current_pos - 1])) {
		current_pos--;
	}
	right_word_start = current_pos;

	// move to the left of the left word
	while (current_pos > 0 && IsSpace(buf[current_pos - 1])) {
		current_pos--;
	}
	left_word_end = current_pos;
	while (current_pos > 0 && !IsSpace(buf[current_pos - 1])) {
		current_pos--;
	}
	left_word_start = current_pos;
	if (left_word_start == right_word_start) {
		// no words to swap
		return;
	}
	idx_t left_len = left_word_end - left_word_start;
	idx_t right_len = right_word_end - right_word_start;
	if (left_len == 0 || right_len == 0) {
		// one of the words has no length
		return;
	}
	// now we need to swap [LEFT][RIGHT] to [RIGHT][LEFT]
	// there's certainly some fancy way of doing this in-place
	// but this is way easier
	string left_word(buf + left_word_start, left_len);
	string right_word(buf + right_word_start, right_len);
	memset(buf + left_word_start, ' ', end_pos - left_word_start);
	memcpy(buf + left_word_start, right_word.c_str(), right_word.size());
	memcpy(buf + end_pos - left_len, left_word.c_str(), left_word.size());
	pos = end_pos;
	RefreshLine();
}

void Linenoise::EditDeleteAll() {
	buf[0] = '\0';
	pos = len = 0;
	RefreshLine();
}

void Linenoise::SetCursorPosition(int x, int y) {
	int current_row, current_col, cols, rows;
	// key presses are *relative to the current cursor*
	// first get the current cursor location
	PositionToColAndRow(pos, current_row, current_col, rows, cols);
	// adjust the location based on the provided x / y
	current_col += x;
	current_row += y;
	if (current_col < 0 || current_row < 0 || current_row > rows) {
		// out of bounds - ignore
		return;
	}
	pos = ColAndRowToPosition(current_row, current_col);
	RefreshLine();
}

void Linenoise::EditCapitalizeNextWord(Capitalization capitalization) {
	size_t next_pos = pos;

	// find the next word
	while (next_pos < len && IsSpace(buf[next_pos])) {
		next_pos++;
	}
	bool first_char = true;
	while (next_pos < len && !IsSpace(buf[next_pos])) {
		switch (capitalization) {
		case Capitalization::CAPITALIZE: {
			if (first_char) {
				first_char = false;
				if (buf[next_pos] >= 'a' && buf[next_pos] <= 'z') {
					buf[next_pos] -= 'a' - 'A';
				}
			}
			break;
		}
		case Capitalization::LOWERCASE: {
			if (buf[next_pos] >= 'A' && buf[next_pos] <= 'Z') {
				buf[next_pos] += 'a' - 'A';
			}
			break;
		}
		case Capitalization::UPPERCASE: {
			if (buf[next_pos] >= 'a' && buf[next_pos] <= 'z') {
				buf[next_pos] -= 'a' - 'A';
			}
			break;
		}
		default:
			break;
		}
		next_pos++;
	}
	pos = next_pos;
	RefreshLine();
}

void Linenoise::StartSearch() {
	// initiate reverse search
	search = true;
	search_buf = std::string(buf, len);
	search_matches.clear();
	search_index = 0;
	PerformSearch();
	RefreshSearch();
}

void Linenoise::CancelSearch() {
	search = false;
	search_buf = std::string();
	search_matches.clear();
	search_index = 0;
	RefreshLine();
}

void Linenoise::AcceptSearch() {
	if (search_index < search_matches.size()) {
		// if there is a match - copy it into the buffer
		auto match = search_matches[search_index];
		auto history_entry = History::GetEntry(match.history_index);
		auto history_len = strlen(history_entry);
		memcpy(buf, history_entry, history_len);
		buf[history_len] = '\0';
		pos = match.match_end;
		len = history_len;
	}
	CancelSearch();
}

void Linenoise::PerformSearch() {
	// we try to maintain the current match while searching
	size_t current_match = History::GetLength();
	if (search_index < search_matches.size()) {
		current_match = search_matches[search_index].history_index;
	}
	search_matches.clear();
	search_index = 0;
	if (search_buf.empty()) {
		return;
	}
	std::unordered_set<std::string> matches;
	auto lsearch = duckdb::StringUtil::Lower(search_buf);
	for (size_t i = History::GetLength(); i > 0; i--) {
		size_t history_index = i - 1;
		auto lhistory = duckdb::StringUtil::Lower(History::GetEntry(history_index));
		if (matches.find(lhistory) != matches.end()) {
			continue;
		}
		matches.insert(lhistory);
		auto entry = lhistory.find(lsearch);
		if (entry != duckdb::string::npos) {
			if (history_index == current_match) {
				search_index = search_matches.size();
			}
			searchMatch match;
			match.history_index = history_index;
			match.match_start = entry;
			match.match_end = entry + lsearch.size();
			search_matches.push_back(match);
		}
	}
}

void Linenoise::SearchPrev() {
	if (search_index > 0) {
		search_index--;
	} else if (search_matches.size() > 0) {
		search_index = search_matches.size() - 1;
	}
}

void Linenoise::SearchNext() {
	search_index += 1;
	if (search_index >= search_matches.size()) {
		search_index = 0;
	}
}

KeyPress Linenoise::Search(KeyPress key_press) {
	switch (key_press.action) {
	case CTRL_J:
	case ENTER: /* enter */
		// accept search and run
		AcceptSearch();
		return ENTER;
	case CTRL_R:
	case CTRL_S:
		// move to the next match index
		SearchNext();
		break;
	case ESC: /* escape sequence */ {
		switch (key_press.sequence) {
		case EscapeSequence::ESCAPE:
			// double escape accepts search without any additional command
			AcceptSearch();
			return KEY_NULL;
		case EscapeSequence::UP:
			SearchPrev();
			break;
		case EscapeSequence::DOWN:
			SearchNext();
			break;
		case EscapeSequence::HOME:
			AcceptSearch();
			return CTRL_A;
		case EscapeSequence::END:
			AcceptSearch();
			return CTRL_E;
		case EscapeSequence::LEFT:
			AcceptSearch();
			return CTRL_B;
		case EscapeSequence::RIGHT:
			AcceptSearch();
			return CTRL_F;
		default:
			break;
		}
		break;
	}
	case CTRL_A: // accept search, move to start of line
		AcceptSearch();
		return CTRL_A;
	case '\t':
	case CTRL_E: // accept search - move to end of line
		AcceptSearch();
		return CTRL_E;
	case CTRL_B: // accept search - move cursor left
		AcceptSearch();
		return CTRL_B;
	case CTRL_F: // accept search - move cursor right
		AcceptSearch();
		return CTRL_F;
	case CTRL_T: // accept search: swap character
		AcceptSearch();
		return CTRL_T;
	case CTRL_U: // accept search, clear buffer
		AcceptSearch();
		return CTRL_U;
	case CTRL_K: // accept search, clear after cursor
		AcceptSearch();
		return CTRL_K;
	case CTRL_D: // accept search, delete a character
		AcceptSearch();
		return CTRL_D;
	case CTRL_L:
		linenoiseClearScreen();
		break;
	case CTRL_P:
		SearchPrev();
		break;
	case CTRL_N:
		SearchNext();
		break;
	case CTRL_C:
	case CTRL_G:
		// abort search
		CancelSearch();
		return KEY_NULL;
	case BACKSPACE: /* backspace */
	case CTRL_H:    /* ctrl-h */
	case CTRL_W:    /* ctrl-w */
		// remove trailing UTF-8 bytes (if any)
		while (!search_buf.empty() && ((search_buf.back() & 0xc0) == 0x80)) {
			search_buf.pop_back();
		}
		// finally remove the first UTF-8 byte
		if (!search_buf.empty()) {
			search_buf.pop_back();
		}
		PerformSearch();
		break;
	default:
		// add input to search buffer
		search_buf += key_press.action;
		// perform the search
		PerformSearch();
		break;
	}
	RefreshSearch();
	return KEY_NULL;
}

bool Linenoise::AllWhitespace(const char *z) {
	for (; *z; z++) {
		if (StringUtil::CharacterIsSpace((unsigned char)z[0]))
			continue;
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
			if (*z == 0) {
				return true;
			}
			continue;
		}
		return false;
	}
	return true;
}

Linenoise::Linenoise(int stdin_fd, int stdout_fd, char *buf, size_t buflen, const char *prompt)
    : ifd(stdin_fd), ofd(stdout_fd), buf(buf), buflen(buflen), prompt(prompt), plen(strlen(prompt)) {
	pos = 0;
	old_cursor_rows = 1;
	len = 0;
	ws = Terminal::GetTerminalSize();
	maxrows = 0;
	history_index = 0;
	y_scroll = 0;
	clear_screen = false;
	search = false;
	has_more_data = false;
	render = true;
	continuation_markers = true;
	insert = false;
	search_index = 0;
	rendered_completion_lines = 0;
	completion_idx = optional_idx();
	render_completion_suggestion = false;

	/* Buffer starts empty. */
	buf[0] = '\0';
	buflen--; /* Make sure there is always space for the nulterm */
}

void Linenoise::HandleTerminalResize() {
	if (!Terminal::IsMultiline()) {
		return;
	}
	TerminalSize new_size = Terminal::GetTerminalSize();
	if (new_size.ws_col == ws.ws_col && new_size.ws_row == ws.ws_row) {
		return;
	}
	// terminal resize! re-compute max lines
	ws = new_size;
	int rows, cols;
	int cursor_row, cursor_col;
	PositionToColAndRow(pos, cursor_row, cursor_col, rows, cols);
	old_cursor_rows = cursor_row;
	maxrows = rows;

	if (rendered_completion_lines > 0) {
		// if we have rendered completions - figure out how many rows this takes up post-resize
		string completion_text;
		for (idx_t i = 0; i < completion_list.completions.size(); i++) {
			if (i > 0) {
				completion_text += " ";
			}
			auto &completion = completion_list.completions[i];
			auto &rendered_text = completion.original_completion;
			completion_text += rendered_text;
		}
		PositionToColAndRow(0, completion_text.c_str(), completion_text.size(), 0, cursor_row, cursor_col, rows, cols);
		rendered_completion_lines = rows;
		maxrows += rows;
	}
}

BufferedKeyPresses::BufferedKeyPresses() {
}

BufferedKeyPresses &BufferedKeyPresses::Get() {
	static BufferedKeyPresses buffered_key_presses;
	return buffered_key_presses;
}

void BufferedKeyPresses::BufferKeyPress(KeyPress key_press) {
	auto &instance = Get();
	instance.remaining_presses.push(key_press);
}

bool BufferedKeyPresses::TryGetKeyPress(KeyPress &result) {
	auto &instance = Get();
	if (instance.remaining_presses.empty()) {
		return false;
	}
	result = instance.remaining_presses.front();
	instance.remaining_presses.pop();
	Linenoise::Log("Consumed 1 press, leaving %d presses to be processed", int(instance.remaining_presses.size()));
	return true;
}

bool BufferedKeyPresses::HasMoreData() {
	auto &instance = Get();
	return !instance.remaining_presses.empty();
}

#if defined(_WIN32) || defined(WIN32)
struct KeyPressEntry {
	KeyPressEntry(KeyPress key_press) : is_unicode(false), key_press(key_press) {
	}
	KeyPressEntry(WCHAR w, bool control_pressed, bool alt_pressed)
	    : is_unicode(true), control_pressed(control_pressed), alt_pressed(alt_pressed) {
		unicode += w;
	}

	bool is_unicode = false;
	KeyPress key_press;
	std::wstring unicode;
	bool control_pressed = false;
	bool alt_pressed = false;
};
#endif

bool Linenoise::TryGetKeyPress(int fd, KeyPress &key_press) {
	if (BufferedKeyPresses::TryGetKeyPress(key_press)) {
		// there are still characters left to consume
		has_more_data = BufferedKeyPresses::HasMoreData() || Terminal::HasMoreData(ifd);
		return true;
	}
#if defined(_WIN32) || defined(WIN32)
	INPUT_RECORD rec;
	DWORD count;
	has_more_data = false;
	vector<KeyPressEntry> key_presses;
	do {
		key_press.action = KEY_NULL;
		key_press.sequence = EscapeSequence::INVALID;
		if (!key_presses.empty()) {
			// we already have output that we can emit
			// check if there is more input to process
			// if there are no events anymore, then just break and return our current set of input
			DWORD event_count = 0;
			if (!GetNumberOfConsoleInputEvents(Terminal::GetConsoleInput(), &event_count)) {
				break;
			}
			if (event_count == 0) {
				break;
			}
		}
		if (!ReadConsoleInputW(Terminal::GetConsoleInput(), &rec, 1, &count)) {
			return false;
		}
		if (rec.EventType == WINDOW_BUFFER_SIZE_EVENT) {
			// resizing buffer - handle resize and continue processing
			HandleTerminalResize();
			continue;
		}
		if (rec.EventType != KEY_EVENT) {
			// not a key press - continue
			continue;
		}
		auto &key_event = rec.Event.KeyEvent;
		if (!key_event.bKeyDown) {
			// releasing a key - ignore
			continue;
		}
		bool control_pressed = false;
		bool alt_pressed = false;
		Linenoise::Log("Unicode character %d, ascii character %d, control state %d", key_event.uChar.UnicodeChar,
		               key_event.uChar.AsciiChar, key_event.dwControlKeyState);

		if (key_event.dwControlKeyState & (RIGHT_CTRL_PRESSED | LEFT_CTRL_PRESSED)) {
			control_pressed = true;
		}
		if (key_event.dwControlKeyState & (RIGHT_ALT_PRESSED | LEFT_ALT_PRESSED)) {
			alt_pressed = true;
		}
		if (key_event.uChar.UnicodeChar == 0) {
			switch (key_event.wVirtualKeyCode) {
			case VK_LEFT:
				key_press.action = ESC;
				if (control_pressed) {
					key_press.sequence = EscapeSequence::CTRL_MOVE_BACKWARDS;
				} else if (alt_pressed) {
					key_press.sequence = EscapeSequence::ALT_LEFT_ARROW;
				} else {
					key_press.sequence = EscapeSequence::LEFT;
				}
				break;
			case VK_RIGHT:
				key_press.action = ESC;
				if (control_pressed) {
					key_press.sequence = EscapeSequence::CTRL_MOVE_FORWARDS;
				} else if (alt_pressed) {
					key_press.sequence = EscapeSequence::ALT_RIGHT_ARROW;
				} else {
					key_press.sequence = EscapeSequence::RIGHT;
				}
				break;
			case VK_UP:
				key_press.action = ESC;
				key_press.sequence = EscapeSequence::UP;
				break;
			case VK_DOWN:
				key_press.action = ESC;
				key_press.sequence = EscapeSequence::DOWN;
				break;
			case VK_DELETE:
				key_press.action = ESC;
				key_press.sequence = EscapeSequence::DELETE_KEY;
				break;
			case VK_HOME:
				key_press.action = ESC;
				key_press.sequence = EscapeSequence::HOME;
				break;
			case VK_END:
				key_press.action = ESC;
				key_press.sequence = EscapeSequence::END;
				break;
			case VK_PRIOR:
				key_press.action = CTRL_A;
				break;
			case VK_NEXT:
				key_press.action = CTRL_E;
				break;
			default:
				continue; // in raw mode, ReadConsoleInput shows shift, ctrl ...
			}             //  ... ignore them
			if (key_press.action != KEY_NULL) {
				// add the key press to the list of key presses
				key_presses.emplace_back(key_press);
			}
		} else {
			// we got a character - push it to the stack
			// in this phase we gather all unicode characters together as much as possible
			// because of surrogate pairs we might need to convert characters together in groups, rather than
			// individually
			auto wc = key_event.uChar.UnicodeChar;
			if (key_presses.empty() || !key_presses.back().is_unicode ||
			    key_presses.back().control_pressed != control_pressed ||
			    key_presses.back().alt_pressed != alt_pressed) {
				key_presses.emplace_back(wc, control_pressed, alt_pressed);
			} else {
				key_presses.back().unicode += wc;
			}
		}
	} while (true);
	if (key_presses.empty()) {
		return false;
	}
	// we have key actions - turn them into KeyPress objects
	// now process the key presses
	for (auto &key_action : key_presses) {
		if (!key_action.is_unicode) {
			// standard key press - just add it
			BufferedKeyPresses::BufferKeyPress(key_action.key_press);
			continue;
		}
		auto allocate_size = key_action.unicode.size() * 10;
		auto data = unique_ptr<char[]>(new char[allocate_size]);
		// unicode - need to convert
		int len = WideCharToMultiByte(CP_UTF8,                    // Target code page (UTF-8)
		                              0,                          // Flags
		                              key_action.unicode.c_str(), // Input UTF-16 string
		                              key_action.unicode.size(),  // One wchar_t
		                              data.get(),                 // Output buffer
		                              allocate_size,              // Output buffer size
		                              NULL, NULL);
		// process the characters in ORDER and add the key presses
		for (int i = 0; i < len; i++) {
			char c = data[i];
			key_press.sequence = EscapeSequence::INVALID;
			if (c > 0 && c <= BACKSPACE) {
				// ascii character
				if (key_action.alt_pressed && !key_action.control_pressed) {
					// support ALT codes
					if (c >= 'a' && c <= 'z') {
						key_press.sequence =
						    static_cast<EscapeSequence>(static_cast<int>(EscapeSequence::ALT_A) + (c - 'a'));
					} else if (c >= 'A' && c <= 'Z') {
						key_press.sequence =
						    static_cast<EscapeSequence>(static_cast<int>(EscapeSequence::ALT_A) + (c - 'A'));
					} else if (c == BACKSPACE) {
						key_press.sequence = EscapeSequence::ALT_BACKSPACE;
					} else if (c == '\\') {
						key_press.sequence = EscapeSequence::ALT_BACKSLASH;
					}
				}
				if (key_press.sequence != EscapeSequence::INVALID) {
					key_press.action = ESC;
				} else {
					key_press.action = c;
				}
				// add the key press to the list of key presses
			} else {
				key_press.action = c;
			}
			BufferedKeyPresses::BufferKeyPress(key_press);
		}
	}

	// emit the first key press on the stack
	if (BufferedKeyPresses::TryGetKeyPress(key_press)) {
		// there are still characters left to consume
		has_more_data = BufferedKeyPresses::HasMoreData();
		return true;
	}
	return false;

#else
	char c;
	int nread;

	nread = read(ifd, &c, 1);
	if (nread <= 0) {
		return false;
	}
	has_more_data = Terminal::HasMoreData(ifd);
	if (!has_more_data) {
		HandleTerminalResize();
	}
	key_press.action = c;
	if (key_press.action == ESC) {
		// for ESC we need to read an escape sequence
		key_press.sequence = Terminal::ReadEscapeSequence(ifd, key_press);
	}
	return true;
#endif
}

bool Linenoise::Write(int fd, const char *data, idx_t size) {
#if defined(_WIN32) || defined(WIN32)
	// convert to character encoding in Windows shell
	string data_str = data ? string(data, size) : "";
	auto unicode_text = duckdb_shell::ShellState::Win32Utf8ToUnicode(data_str);
	auto out_handle = GetStdHandle(STD_OUTPUT_HANDLE);
	if (!WriteConsoleW(out_handle, unicode_text.c_str(), unicode_text.size(), NULL, NULL)) {
		return false;
	}
#else
	if (write(fd, data, size) == -1) {
		return false;
	}
#endif
	return true;
}
/* This function is the core of the line editing capability of linenoise.
 * It expects 'fd' to be already in "raw mode" so that every key pressed
 * will be returned ASAP to read().
 *
 * The resulting string is put into 'buf' when the user type enter, or
 * when ctrl+d is typed.
 *
 * The function returns the length of the current buffer. */

// Shortcut table — kept near the key handler switch so they stay in sync.
// If you add or change a keybinding in Edit(), update this table too.
vector<ShortcutEntry> GetShellShortcuts() {
	const vector<ShortcutEntry> shortcuts = {
	    // Control
	    {"Enter / Ctrl+J", "Submit input", "Control"},
	    {"Ctrl+C", "Cancel current input or interrupt in-flight query", "Control"},
	    {"Ctrl+D", "Exit shell (when line is empty)", "Control"},
	    {"Ctrl+G", "Submit input (in multiline mode)", "Control"},
	    {"Ctrl+L", "Clear screen", "Control"},
	    {"Ctrl+Z", "Suspend shell", "Control"},
	    {"Tab", "Auto-complete", "Control"},
	    {"Ctrl+Q, then click", "Move cursor to mouse click position", "Control"},
	    // Editing
	    {"Ctrl+D / Delete", "Delete character under cursor (or exit if line is empty)", "Editing"},
	    {"Ctrl+H / Backspace", "Delete character before cursor", "Editing"},
	    {"Ctrl+K", "Delete from cursor to end of line", "Editing"},
	    {"Ctrl+U", "Delete entire line", "Editing"},
	    {"Ctrl+W", "Delete previous word", "Editing"},
	    {"Alt+D", "Delete next word", "Editing"},
	    {"Alt+Backspace", "Delete previous word", "Editing"},
	    {"Ctrl+T", "Swap character under cursor with previous", "Editing"},
	    {"Alt+T", "Swap current word with previous", "Editing"},
	    {"Alt+C", "Capitalize next word", "Editing"},
	    {"Alt+L", "Lowercase next word", "Editing"},
	    {"Alt+U", "Uppercase next word", "Editing"},
	    {"Alt+R", "Delete entire line", "Editing"},
	    {"Alt+\\", "Remove spaces around cursor", "Editing"},
	    {"Ctrl+X", "Insert newline (multiline input)", "Editing"},
	    // Navigation
	    {"Ctrl+A / Home", "Go to beginning of line", "Navigation"},
	    {"Ctrl+E / End", "Go to end of line", "Navigation"},
	    {"Ctrl+B / Left", "Move cursor left", "Navigation"},
	    {"Ctrl+F / Right", "Move cursor right", "Navigation"},
	    {"Alt+B / Alt+Left", "Move cursor one word left", "Navigation"},
	    {"Alt+F / Alt+Right", "Move cursor one word right", "Navigation"},
	    // History
	    {"Ctrl+P / Up", "Previous history entry", "History"},
	    {"Ctrl+N / Down", "Next history entry", "History"},
	    {"Ctrl+R", "Reverse search history", "History"},
	    {"Ctrl+S", "Forward search history", "History"},
	    {"Ctrl+Up", "Jump to first history entry", "History"},
	    {"Ctrl+Down", "Jump to last history entry", "History"},
	};
	return shortcuts;
}

int Linenoise::Edit() {
	/* The latest history entry is always our current buffer, that
	 * initially is just an empty string. */
	History::Add("");

	if (!Write(ofd, prompt, plen)) {
		return -1;
	}
	while (true) {
		KeyPress key_press;
		if (!TryGetKeyPress(ifd, key_press)) {
			return len;
		}
		render = true;
		insert = false;

		if (search) {
			auto next_action = Search(key_press);
			if (search || next_action.action == KEY_NULL) {
				// still searching - continue searching
				continue;
			}
			// run subsequent command
			key_press = next_action;
		}

		/* Only autocomplete when the callback is set. It returns < 0 when
		 * there was an error reading from fd. Otherwise it will return the
		 * character that should be handled next. */
		if (key_press.action == TAB && completionCallback != NULL) {
			if (has_more_data) {
				// if there is more data, this tab character was added as part of copy-pasting data
				// instead insert some spaces
				if (EditInsertMulti("    ")) {
					return -1;
				}
				continue;
			}
			while (key_press.action == TAB) {
				if (!CompleteLine(key_press)) {
					/* Return on errors */
					return len;
				}
			}
			/* Read next character when 0 */
			if (key_press.action == KEY_NULL) {
				RefreshLine();
				continue;
			}
		}

		Linenoise::Log("%d\n", (int)key_press.action);
		switch (key_press.action) {
		case CTRL_G:
		case CTRL_J:
		case ENTER: { /* enter */
#ifdef LINENOISE_EDITOR
			if (len > 0) {
				// check if this contains ".edit"

				// scroll back to last newline
				idx_t begin_pos;
				for (begin_pos = len; begin_pos > 0 && buf[begin_pos - 1] != '\n'; begin_pos--) {
				}
				// check if line is ".edit"
				bool open_editor = false;
				if (begin_pos + 5 == len && memcmp(buf + begin_pos, ".edit", 5) == 0) {
					open_editor = true;
				}
				// check if line is "\\e"
				if (begin_pos + 2 == len && memcmp(buf + begin_pos, "\\e", 2) == 0) {
					open_editor = true;
				}
				if (open_editor) {
					// .edit
					// clear the buffer and open the editor
					pos = len = begin_pos;
					if (!EditBufferWithEditor(nullptr)) {
						// failed to edit - refresh the removal of ".edit" / "\e"
						RefreshLine();
						break;
					}
				}
			}
#endif
			if (Terminal::IsMultiline() && len > 0) {
				// check if this forms a complete SQL statement or not
				buf[len] = '\0';
				if (buf[0] != '.' && !AllWhitespace(buf) && !duckdb_shell::ShellState::SQLIsComplete(buf)) {
					// not a complete SQL statement yet! continuation
					pos = len;
					if (key_press.action != CTRL_G) {
						// insert "\r\n" at the end if this is enter/ctrl+j
						if (EditInsertMulti("\r\n")) {
							return -1;
						}
						break;
					} else {
						// if this is Ctrl+G, terminate the statement
						if (EditInsertMulti(";")) {
							return -1;
						}
					}
				}
			}
			// final refresh before returning control to the shell
			continuation_markers = false;
			History::RemoveLastEntry();
			Format();
			if (Terminal::IsMultiline()) {
				if (pos == len) {
					// already at the end - only refresh
					RefreshLine();
				} else {
					EditMoveEnd();
				}
			}
			// rewrite \r\n to \n
			idx_t new_len = 0;
			for (idx_t i = 0; i < len; i++) {
				if (buf[i] == '\r' && buf[i + 1] == '\n') {
					continue;
				}
				buf[new_len++] = buf[i];
			}
			buf[new_len] = '\0';
			return (int)new_len;
		}
		case CTRL_O:
		case CTRL_C: /* ctrl-c */ {
			if (Terminal::IsMultiline()) {
				continuation_markers = false;
				// force a refresh by setting pos to 0
				pos = 0;
				EditMoveEnd();
			}
			buf[0] = '\3';
			// we keep track of whether or not the line was empty by writing \3 to the second position of the line
			// this is because at a higher level we might want to know if we pressed ctrl c to clear the line
			// or to exit the process
			if (len > 0) {
				buf[1] = '\3';
				buf[2] = '\0';
				pos = 2;
				len = 2;
			} else {
				buf[1] = '\0';
				pos = 1;
				len = 1;
			}
			return (int)len;
		}
		case BACKSPACE: /* backspace */
		case CTRL_H:    /* ctrl-h */
			EditBackspace();
			break;
		case CTRL_D: /* ctrl-d, remove char at right of cursor, or if the
		        line is empty, act as end-of-file. */
			if (len > 0) {
				EditDelete();
			} else {
				History::RemoveLastEntry();
				return -1;
			}
			break;
		case CTRL_Z: /* ctrl-z, suspends shell */
#if defined(_WIN32) || defined(WIN32)
#else
			Terminal::DisableRawMode();
			raise(SIGTSTP);
			Terminal::EnableRawMode();
#endif
			RefreshLine();
			break;
		case CTRL_T: /* ctrl-t, swaps current character with previous. */
			EditSwapCharacter();
			break;
		case CTRL_B: /* ctrl-b */
			EditMoveLeft();
			break;
		case CTRL_F: /* ctrl-f */
			EditMoveRight();
			break;
		case CTRL_P: /* ctrl-p */
			EditHistoryNext(HistoryScrollDirection::LINENOISE_HISTORY_PREV);
			break;
		case CTRL_N: /* ctrl-n */
			EditHistoryNext(HistoryScrollDirection::LINENOISE_HISTORY_NEXT);
			break;
		case CTRL_S:
		case CTRL_R: /* ctrl-r */ {
			StartSearch();
			break;
		}
		case ESC: /* escape sequence */ {
			EscapeSequence escape = key_press.sequence;
			switch (escape) {
			case EscapeSequence::CTRL_UP:
				EditHistoryNext(HistoryScrollDirection::LINENOISE_HISTORY_START);
				break;
			case EscapeSequence::CTRL_DOWN:
				EditHistoryNext(HistoryScrollDirection::LINENOISE_HISTORY_END);
				break;
			case EscapeSequence::CTRL_MOVE_BACKWARDS:
			case EscapeSequence::ALT_LEFT_ARROW:
			case EscapeSequence::ALT_B:
				EditMoveWordLeft();
				break;
			case EscapeSequence::CTRL_MOVE_FORWARDS:
			case EscapeSequence::ALT_RIGHT_ARROW:
			case EscapeSequence::ALT_F:
				EditMoveWordRight();
				break;
			case EscapeSequence::ALT_D:
				EditDeleteNextWord();
				break;
			case EscapeSequence::ALT_C:
				EditCapitalizeNextWord(Capitalization::CAPITALIZE);
				break;
			case EscapeSequence::ALT_L:
				EditCapitalizeNextWord(Capitalization::LOWERCASE);
				break;
			case EscapeSequence::ALT_N:
			case EscapeSequence::ALT_P:
				StartSearch();
				break;
			case EscapeSequence::ALT_T:
				EditSwapWord();
				break;
			case EscapeSequence::ALT_U:
				EditCapitalizeNextWord(Capitalization::UPPERCASE);
				break;
			case EscapeSequence::ALT_R:
				EditDeleteAll();
				break;
			case EscapeSequence::ALT_BACKSPACE:
				EditDeletePrevWord();
				break;
			case EscapeSequence::ALT_BACKSLASH:
				EditRemoveSpaces();
				break;
			case EscapeSequence::HOME:
				EditMoveHome();
				break;
			case EscapeSequence::END:
				EditMoveEnd();
				break;
			case EscapeSequence::UP:
				if (EditMoveRowUp()) {
					break;
				}
				EditHistoryNext(HistoryScrollDirection::LINENOISE_HISTORY_PREV);
				break;
			case EscapeSequence::DOWN:
				if (EditMoveRowDown()) {
					break;
				}
				EditHistoryNext(HistoryScrollDirection::LINENOISE_HISTORY_NEXT);
				break;
			case EscapeSequence::RIGHT:
				EditMoveRight();
				break;
			case EscapeSequence::LEFT:
				EditMoveLeft();
				break;
			case EscapeSequence::DELETE_KEY:
				EditDelete();
				break;
			case EscapeSequence::MOUSE_CLICK:
				// mouse click
				SetCursorPosition(key_press.position.ws_col, key_press.position.ws_row);
				break;
			default:
				Linenoise::Log("Unrecognized escape\n");
				break;
			}
			break;
		}
		case CTRL_U: /* Ctrl+u, delete the whole line. */
			EditDeleteAll();
			break;
		case CTRL_K: /* Ctrl+k, delete from current to end of line. */
			buf[pos] = '\0';
			len = pos;
			RefreshLine();
			break;
		case CTRL_A: /* Ctrl+a, go to the start of the line */
			EditMoveStartOfLine();
			break;
		case CTRL_E: /* ctrl+e, go to the end of the line */
			EditMoveEndOfLine();
			break;
		case CTRL_L: /* ctrl+l, clear screen */
			linenoiseClearScreen();
			clear_screen = true;
			RefreshLine();
			break;
		case CTRL_W: /* ctrl+w, delete previous word */
			EditDeletePrevWord();
			break;
		case CTRL_X: /* ctrl+x, insert newline */
			EditInsertMulti("\r\n");
			break;
		case CTRL_Q: /* ctrl+q enables mouse tracking for a single click */
			Terminal::EnableMouseTracking();
			break;
		case CTRL_Y:
			// unsupported
			break;
		default: {
			if (EditInsert(key_press.action)) {
				return -1;
			}
			break;
		}
		}
	}
	return len;
}

#ifdef LINENOISE_LOGGING
void Linenoise::LogMessageRecursive(const string &msg, std::vector<ExceptionFormatValue> &values) {
	static FILE *lndebug_fp = NULL;
	if (!lndebug_fp) {
		string path;
#if defined(_WIN32) || defined(WIN32)
		path = GetTemporaryDirectory() + "\\lndebug.txt";
#else
		path = "/tmp/lndebug.txt";
#endif
		lndebug_fp = fopen(path.c_str(), "a");
	}
	auto log_message = Exception::ConstructMessageRecursive(msg, values);
	fprintf(lndebug_fp, "%s", log_message.c_str());
	fflush(lndebug_fp);
}
#endif

void Linenoise::LogTokens(const vector<highlightToken> &tokens) {
#ifdef LINENOISE_LOGGING
	for (auto &token : tokens) {
		const char *token_type_name = "";
		switch (token.type) {
		case tokenType::TOKEN_IDENTIFIER:
			token_type_name = "IDENTIFIER";
			break;
		case tokenType::TOKEN_NUMERIC_CONSTANT:
			token_type_name = "NUMERIC_CONSTANT";
			break;
		case tokenType::TOKEN_STRING_CONSTANT:
			token_type_name = "STRING_CONSTANT";
			break;
		case tokenType::TOKEN_OPERATOR:
			token_type_name = "OPERATOR";
			break;
		case tokenType::TOKEN_KEYWORD:
			token_type_name = "KEYWORD";
			break;
		case tokenType::TOKEN_COMMENT:
			token_type_name = "COMMENT";
			break;
		case tokenType::TOKEN_CONTINUATION:
			token_type_name = "CONTINUATION";
			break;
		case tokenType::TOKEN_CONTINUATION_SELECTED:
			token_type_name = "CONTINUATION_SELECTED";
			break;
		case tokenType::TOKEN_BRACKET:
			token_type_name = "BRACKET";
			break;
		case tokenType::TOKEN_ERROR:
			token_type_name = "ERROR";
			break;
		default:
			break;
		}
		Linenoise::Log("Token at position %d with type %s\n", token.start, token_type_name);
	}
#endif
}

#ifdef LINENOISE_EDITOR
// .edit functionality - code adopted from psql

bool Linenoise::EditFileWithEditor(const string &file_name, const char *editor) {
	/* Find an editor to use */
	if (!editor) {
		editor = getenv("DUCKDB_EDITOR");
	}
	if (!editor) {
		editor = getenv("EDITOR");
	}
	if (!editor) {
		editor = getenv("VISUAL");
	}
	if (!editor) {
		editor = DEFAULT_EDITOR;
	}

	/*
	 * On Unix the EDITOR value should *not* be quoted, since it might include
	 * switches, eg, EDITOR="pico -t"; it's up to the user to put quotes in it
	 * if necessary.  But this policy is not very workable on Windows, due to
	 * severe brain damage in their command shell plus the fact that standard
	 * program paths include spaces.
	 */
	string command;
#ifndef WIN32
	command = "exec " + string(editor) + " '" + file_name + "'";
#else
	command = "\"" + string(editor) + "\" \"" + file_name + "\"";
#endif
	int result = system(command.c_str());
	if (result == -1) {
		Log("could not start editor \"%s\"\n", editor);
	} else if (result == 127) {
		Log("could not start /bin/sh\n");
	}
	return result == 0;
}

string Linenoise::GetTemporaryDirectory() {
#ifndef WIN32
	/* make a temp file to edit */
	const char *tmpdir = getenv("TMPDIR");
	if (!tmpdir) {
		tmpdir = "/tmp";
	}
	return tmpdir;
#else
	static constexpr const idx_t MAX_PATH_LENGTH = 261;
	char tmpdir[MAX_PATH_LENGTH];
	int ret;

	// FIXME: use GetTempPathW
	ret = GetTempPath(MAX_PATH_LENGTH, tmpdir);
	if (ret == 0 || ret > MAX_PATH_LENGTH) {
		Log("cannot locate temporary directory: %s", !ret ? strerror(errno) : "");
		return string();
	}
	return tmpdir;
#endif
}

bool Linenoise::EditBufferWithEditor(const char *editor) {
	auto tmpdir = GetTemporaryDirectory();
	string temporary_file_name;
#ifndef WIN32
	temporary_file_name = tmpdir + "/duckdb.edit." + std::to_string(getpid()) + ".sql";
#else
	temporary_file_name = tmpdir + "duckdb.edit." + std::to_string(getpid()) + ".sql";
#endif

	FILE *f = fopen(temporary_file_name.c_str(), "w+");
	if (!f) {
		Log("could not open temporary file \"%s\": %s\n", temporary_file_name, strerror(errno));
		Terminal::Beep();
		return false;
	}

	// edit the current buffer by default
	const char *write_buffer = buf;
	idx_t write_len = len;
	if (write_len == 0) {
		// if the current buffer is empty we are typing ".edit" as the first command
		// edit the previous history entry
		auto edit_index = History::GetLength();
		if (edit_index >= 2) {
			auto history_entry = History::GetEntry(edit_index - 2);
			if (history_entry) {
				write_buffer = history_entry;
				write_len = strlen(history_entry);
			}
		}
	}

	// write existing buffer to file
	if (fwrite(write_buffer, 1, write_len, f) != write_len) {
		Log("Failed to write data %s: %s\n", temporary_file_name, strerror(errno));
		fclose(f);
		remove(temporary_file_name.c_str());
		Terminal::Beep();
		return false;
	}
	fclose(f);

	/* call editor */
	if (!EditFileWithEditor(temporary_file_name, editor)) {
		Terminal::Beep();
		return false;
	}

	// read the file contents again
	f = fopen(temporary_file_name.c_str(), "rb");
	if (!f) {
		Log("Failed to open file%s: %s\n", temporary_file_name, strerror(errno));
		remove(temporary_file_name.c_str());
		Terminal::Beep();
		return false;
	}

	/* read file back into buffer */
	string new_buffer;
	char line[1024];
	while (fgets(line, sizeof(line), f)) {
		// strip the existing newline from the line obtained from fgets
		// the reason for that is that we need the line endings to be "\r\n" for rendering purposes
		idx_t line_len = strlen(line);
		idx_t orig_len = line_len;
		while (line_len > 0 && (line[line_len - 1] == '\r' || line[line_len - 1] == '\n')) {
			line_len--;
		}
		new_buffer.append(line, line_len);
		if (orig_len != line_len) {
			// we stripped a newline - add a new newline (but this time always \r\n)
			new_buffer += "\r\n";
		}
	}
	if (ferror(f)) {
		Log("Failed while reading back buffer %s: %s\n", temporary_file_name, strerror(errno));
		Terminal::Beep();
	}
	fclose(f);

	/* remove temp file */
	if (remove(temporary_file_name.c_str()) == -1) {
		Log("Failed to remove file \"%s\": %s\n", temporary_file_name, strerror(errno));
		Terminal::Beep();
		return false;
	}

	// copy back into buffer
	memcpy(buf, new_buffer.c_str(), new_buffer.size());
	len = new_buffer.size();
	pos = len;
	RefreshLine();

	return true;
}
#endif

} // namespace duckdb
