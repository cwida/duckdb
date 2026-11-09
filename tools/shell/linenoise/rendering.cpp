#include "linenoise.hpp"
#include "highlighting.hpp"
#include "history.hpp"
#include "utf8proc_wrapper.hpp"
#include "shell_highlight.hpp"
#include "shell_state.hpp"
#if defined(_WIN32) || defined(WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace duckdb {
static const char *continuationPrompt = "  ";
static const char *continuationSelectedPrompt = "  ";
static const char *scrollUpPrompt = "^ ";
static const char *scrollDownPrompt = "v ";
static bool enableCompletionRendering = false;
static bool enableErrorRendering = true;

void Linenoise::EnableCompletionRendering() {
	enableCompletionRendering = true;
}

void Linenoise::DisableCompletionRendering() {
	enableCompletionRendering = false;
}

void Linenoise::EnableErrorRendering() {
	enableErrorRendering = true;
}

void Linenoise::DisableErrorRendering() {
	enableErrorRendering = false;
}

/* =========================== Line editing ================================= */

/* We define a very simple "append buffer" structure, that is an heap
 * allocated string where we can append to. This is useful in order to
 * write all the escape sequences in a buffer and flush them to the standard
 * output in a single call, to avoid flickering effects. */
struct AppendBuffer {
	void Append(const char *s, idx_t len) {
		buffer.append(s, len);
	}
	void Append(const char *s) {
		buffer.append(s);
	}

	void Write(int fd) {
		if (!Linenoise::Write(fd, buffer.c_str(), buffer.size())) {
			/* Can't recover from write error. */
			Linenoise::Log("%s", "Failed to write buffer\n");
		}
	}

private:
	std::string buffer;
};

void Linenoise::SetPrompt(const char *continuation, const char *continuationSelected, const char *scrollUp,
                          const char *scrollDown) {
	continuationPrompt = continuation;
	continuationSelectedPrompt = continuationSelected;
	scrollUpPrompt = scrollUp;
	scrollDownPrompt = scrollDown;
}

static void renderText(size_t &render_pos, char *&buf, size_t &len, size_t pos, size_t cols, size_t plen,
                       std::string &highlight_buffer, bool highlight) {
	if (duckdb::Utf8Proc::IsValid(buf, len)) {
		// utf8 in prompt, handle rendering
		size_t remaining_render_width = cols - plen - 1;
		size_t start_pos = 0;
		size_t cpos = 0;
		size_t prev_pos = 0;
		size_t total_render_width = 0;
		while (cpos < len) {
			size_t char_render_width = duckdb::Utf8Proc::RenderWidth(buf, len, cpos);
			prev_pos = cpos;
			cpos = duckdb::Utf8Proc::NextGraphemeCluster(buf, len, cpos);
			total_render_width += cpos - prev_pos;
			if (total_render_width >= remaining_render_width) {
				// character does not fit anymore! we need to figure something out
				if (prev_pos >= pos) {
					// we passed the cursor: break
					cpos = prev_pos;
					break;
				} else {
					// we did not pass the cursor yet! remove characters from the start until it fits again
					while (total_render_width >= remaining_render_width) {
						size_t start_char_width = duckdb::Utf8Proc::RenderWidth(buf, len, start_pos);
						size_t new_start = duckdb::Utf8Proc::NextGraphemeCluster(buf, len, start_pos);
						total_render_width -= new_start - start_pos;
						start_pos = new_start;
						render_pos -= start_char_width;
					}
				}
			}
			if (prev_pos < pos) {
				render_pos += char_render_width;
			}
		}
		if (highlight) {
			bool is_dot_command = buf[0] == '.' || buf[0] == '!';
			auto tokens = Highlighting::Tokenize(buf, len, is_dot_command);
			highlight_buffer = Highlighting::HighlightText(buf, len, start_pos, cpos, tokens);
			buf = (char *)highlight_buffer.c_str();
			len = highlight_buffer.size();
		} else {
			buf = buf + start_pos;
			len = cpos - start_pos;
		}
	} else {
		// invalid UTF8: fallback
		while ((plen + pos) >= cols) {
			buf++;
			len--;
			pos--;
		}
		while (plen + len > cols) {
			len--;
		}
		render_pos = pos;
	}
}

/* Single line low level line refresh.
 *
 * Rewrite the currently edited line accordingly to the buffer content,
 * cursor position, and number of columns of the terminal. */
void Linenoise::RefreshSingleLine() const {
	char seq[64];
	size_t plen = GetPromptWidth();
	int fd = ofd;
	char *render_buf = buf;
	size_t render_len = len;
	size_t render_pos = 0;
	std::string highlight_buffer;

	renderText(render_pos, render_buf, render_len, pos, ws.ws_col, plen, highlight_buffer, Highlighting::IsEnabled());

	AppendBuffer append_buffer;
	/* Cursor to left edge */
	append_buffer.Append("\r");
	/* Write the prompt and the current buffer content */
	append_buffer.Append(prompt);
	append_buffer.Append(render_buf, render_len);
	/* Erase to right */
	append_buffer.Append("\x1b[0K");
	/* Move cursor to original position. */
	snprintf(seq, 64, "\r\x1b[%dC", (int)(render_pos + plen));
	append_buffer.Append(seq);
	append_buffer.Write(fd);
}

void Linenoise::RefreshSearch() {
	std::string search_prompt;
	static const size_t SEARCH_PROMPT_RENDER_SIZE = 28;
	std::string no_matches_text = "(no matches)";
	bool no_matches = search_index >= search_matches.size();
	if (search_buf.empty()) {
		search_prompt = "search" + std::string(SEARCH_PROMPT_RENDER_SIZE - 8, ' ') + "> ";
		no_matches_text = "(type to search)";
	} else {
		std::string search_text;
		std::string matches_text;
		search_text += search_buf;
		if (!no_matches) {
			matches_text += std::to_string(search_index + 1);
			matches_text += "/" + std::to_string(search_matches.size());
		}
		size_t search_text_length = ComputeRenderWidth(search_text.c_str(), search_text.size());
		size_t matches_text_length = ComputeRenderWidth(matches_text.c_str(), matches_text.size());
		size_t total_text_length = search_text_length + matches_text_length;
		if (total_text_length < SEARCH_PROMPT_RENDER_SIZE - 2) {
			// search text is short: we can render the entire search text
			search_prompt = search_text;
			search_prompt += std::string(SEARCH_PROMPT_RENDER_SIZE - 2 - total_text_length, ' ');
			search_prompt += matches_text;
			search_prompt += "> ";
		} else {
			// search text length is too long to fit: truncate
			bool render_matches = matches_text_length < SEARCH_PROMPT_RENDER_SIZE - 8;
			char *search_buf = (char *)search_text.c_str();
			size_t search_len = search_text.size();
			size_t search_render_pos = 0;
			size_t max_render_size = SEARCH_PROMPT_RENDER_SIZE - 3;
			if (render_matches) {
				max_render_size -= matches_text_length;
			}
			std::string highlight_buffer;
			renderText(search_render_pos, search_buf, search_len, search_len, max_render_size, 0, highlight_buffer,
			           false);
			search_prompt = std::string(search_buf, search_len);
			for (size_t i = search_render_pos; i < max_render_size; i++) {
				search_prompt += " ";
			}
			if (render_matches) {
				search_prompt += matches_text;
			}
			search_prompt += "> ";
		}
	}
	auto old_highlighting = duckdb_shell::ShellHighlight::IsEnabled();
	Linenoise clone = *this;
	prompt = search_prompt.c_str();
	plen = search_prompt.size();
	if (no_matches || search_buf.empty()) {
		// if there are no matches render the no_matches_text
		buf = (char *)no_matches_text.c_str();
		len = no_matches_text.size();
		pos = 0;
		// don't highlight the "no_matches" text
		duckdb_shell::ShellHighlight::SetHighlighting(false);
	} else {
		// if there are matches render the current history item
		auto search_match = search_matches[search_index];
		auto history_index = search_match.history_index;
		auto cursor_position = search_match.match_end;
		buf = (char *)History::GetEntry(history_index);
		len = strlen(buf);
		pos = cursor_position;
	}
	RefreshLine();

	if (old_highlighting) {
		duckdb_shell::ShellHighlight::SetHighlighting(true);
	}
	buf = clone.buf;
	len = clone.len;
	pos = clone.pos;
	prompt = clone.prompt;
	plen = clone.plen;
}

string Linenoise::AddContinuationMarkers(const char *buf, size_t len, int plen, int cursor_row, idx_t rows_to_render,
                                         vector<highlightToken> &tokens, RenderTruncation truncation) const {
	std::string result;
	int rows = 1;
	int cols = plen;
	size_t cpos = 0;
	size_t prev_pos = 0;
	size_t extra_bytes = 0;    // extra bytes introduced
	size_t token_position = 0; // token position
	vector<highlightToken> new_tokens;
	new_tokens.reserve(tokens.size());
	bool truncate_top = truncation == RenderTruncation::TRUNCATE_TOP || truncation == RenderTruncation::TRUNCATE_BOTH;
	bool truncate_bottom =
	    truncation == RenderTruncation::TRUNCATE_BOTTOM || truncation == RenderTruncation::TRUNCATE_BOTH;
	while (cpos < len) {
		bool is_newline = IsNewline(buf[cpos]);
		NextPosition(buf, len, cpos, rows, cols, plen);
		for (; prev_pos < cpos; prev_pos++) {
			result += buf[prev_pos];
		}
		if (is_newline) {
			bool is_cursor_row = rows == cursor_row;
			const char *prompt;
			if (is_cursor_row) {
				prompt = continuationSelectedPrompt;
			} else if (truncate_top && rows == 2) {
				prompt = scrollUpPrompt;
			} else if (truncate_bottom && NumericCast<idx_t>(rows) == rows_to_render + 1) {
				prompt = scrollDownPrompt;
			} else {
				prompt = continuationPrompt;
			}
			if (!continuation_markers) {
				prompt = "";
			}
			size_t continuationLen = strlen(prompt);
			size_t continuationRender = ComputeRenderWidth(prompt, continuationLen);
			// pad with spaces prior to prompt
			for (int i = int(continuationRender); i < plen; i++) {
				result += " ";
			}
			result += prompt;
			size_t continuationBytes = plen - continuationRender + continuationLen;
			if (token_position < tokens.size()) {
				for (; token_position < tokens.size(); token_position++) {
					if (tokens[token_position].start >= cpos) {
						// not there yet
						break;
					}
					tokens[token_position].start += extra_bytes;
					new_tokens.push_back(tokens[token_position]);
				}
				tokenType prev_type = tokenType::TOKEN_IDENTIFIER;
				if (token_position > 0 && token_position < tokens.size() + 1) {
					prev_type = tokens[token_position - 1].type;
				}
				highlightToken token;
				token.start = cpos + extra_bytes;
				token.type = is_cursor_row ? tokenType::TOKEN_CONTINUATION_SELECTED : tokenType::TOKEN_CONTINUATION;
				new_tokens.push_back(token);

				token.start = cpos + extra_bytes + continuationBytes;
				token.type = prev_type;
				new_tokens.push_back(token);
			}
			extra_bytes += continuationBytes;
		}
	}
	for (; prev_pos < cpos; prev_pos++) {
		result += buf[prev_pos];
	}
	for (; token_position < tokens.size(); token_position++) {
		tokens[token_position].start += extra_bytes;
		new_tokens.push_back(tokens[token_position]);
	}
	tokens = std::move(new_tokens);
	return result;
}

// insert a token of length 1 of the specified type
static void InsertToken(tokenType insert_type, idx_t insert_pos, vector<highlightToken> &tokens) {
	vector<highlightToken> new_tokens;
	new_tokens.reserve(tokens.size() + 1);
	idx_t i;
	bool found = false;
	for (i = 0; i < tokens.size(); i++) {
		// find the exact position where we need to insert the token
		if (tokens[i].start == insert_pos) {
			// this token is exactly at this render position

			// insert highlighting for the bracket
			highlightToken token;
			token.start = insert_pos;
			token.type = insert_type;
			new_tokens.push_back(token);

			// now we need to insert the other token ONLY if the other token is not immediately following this one
			if (i + 1 >= tokens.size() || tokens[i + 1].start > insert_pos + 1) {
				token.start = insert_pos + 1;
				token.type = tokens[i].type;
				new_tokens.push_back(token);
			}
			i++;
			found = true;
			break;
		} else if (tokens[i].start > insert_pos) {
			// the next token is AFTER the render position
			// insert highlighting for the bracket
			highlightToken token;
			token.start = insert_pos;
			token.type = insert_type;
			new_tokens.push_back(token);

			// now just insert the next token
			new_tokens.push_back(tokens[i]);
			i++;
			found = true;
			break;
		} else {
			// insert the token
			new_tokens.push_back(tokens[i]);
		}
	}
	// copy over the remaining tokens
	for (; i < tokens.size(); i++) {
		new_tokens.push_back(tokens[i]);
	}
	if (!found) {
		// token was not added - add it to the end
		highlightToken token;
		token.start = insert_pos;
		token.type = insert_type;
		new_tokens.push_back(token);
	}
	tokens = std::move(new_tokens);
}

enum class ScanState { STANDARD, IN_SINGLE_QUOTE, IN_DOUBLE_QUOTE, IN_COMMENT, DOLLAR_QUOTED_STRING };

static void OpenBracket(vector<idx_t> &brackets, vector<idx_t> &cursor_brackets, idx_t pos, idx_t i) {
	// check if the cursor is at this position
	if (pos == i) {
		// cursor is exactly on this position - always highlight this bracket
		if (!cursor_brackets.empty()) {
			cursor_brackets.clear();
		}
		cursor_brackets.push_back(i);
	}
	if (cursor_brackets.empty() && ((i + 1) == pos || (pos + 1) == i)) {
		// cursor is either BEFORE or AFTER this bracket and we don't have any highlighted bracket yet
		// highlight this bracket
		cursor_brackets.push_back(i);
	}
	brackets.push_back(i);
}

static void CloseBracket(vector<idx_t> &brackets, vector<idx_t> &cursor_brackets, idx_t pos, idx_t i,
                         vector<idx_t> &errors) {
	if (pos == i) {
		// cursor is on this closing bracket
		// clear any selected brackets - we always select this one
		cursor_brackets.clear();
	}
	if (brackets.empty()) {
		// closing bracket without matching opening bracket
		errors.push_back(i);
	} else {
		if (cursor_brackets.size() == 1) {
			if (cursor_brackets.back() == brackets.back()) {
				// this closing bracket matches the highlighted opening cursor bracket - highlight both
				cursor_brackets.push_back(i);
			}
		} else if (cursor_brackets.empty() && (pos == i || (i + 1) == pos || (pos + 1) == i)) {
			// no cursor bracket selected yet and cursor is BEFORE or AFTER this bracket
			// add this bracket
			cursor_brackets.push_back(i);
			cursor_brackets.push_back(brackets.back());
		}
		brackets.pop_back();
	}
}

static void HandleBracketErrors(const vector<idx_t> &brackets, vector<idx_t> &errors) {
	if (brackets.empty()) {
		return;
	}
	// if there are unclosed brackets remaining not all brackets were closed
	for (auto &bracket : brackets) {
		errors.push_back(bracket);
	}
}

void Linenoise::AddErrorHighlighting(idx_t render_start, idx_t render_end, vector<highlightToken> &tokens) const {
	static constexpr const idx_t MAX_ERROR_LENGTH = 2000;
	if (!enableErrorRendering) {
		return;
	}
	if (len >= MAX_ERROR_LENGTH) {
		return;
	}
	// do a pass over the buffer to collect errors:
	// * brackets without matching closing/opening bracket
	// * single quotes without matching closing single quote
	// * double quote without matching double quote
	ScanState state = ScanState::STANDARD;
	vector<idx_t> brackets;        // ()
	vector<idx_t> square_brackets; // []
	vector<idx_t> curly_brackets;  // {}
	vector<idx_t> errors;
	vector<idx_t> cursor_brackets;
	vector<idx_t> comment_start;
	vector<idx_t> comment_end;
	string dollar_quote_marker;
	idx_t quote_pos = 0;
	for (idx_t i = 0; i < len; i++) {
		auto c = buf[i];
		switch (state) {
		case ScanState::STANDARD:
			switch (c) {
			case '-':
				if (i + 1 < len && buf[i + 1] == '-') {
					// -- puts us in a comment
					comment_start.push_back(i);
					i++;
					state = ScanState::IN_COMMENT;
					break;
				}
				break;
			case '\'':
				state = ScanState::IN_SINGLE_QUOTE;
				quote_pos = i;
				break;
			case '\"':
				state = ScanState::IN_DOUBLE_QUOTE;
				quote_pos = i;
				break;
			case '(':
				OpenBracket(brackets, cursor_brackets, pos, i);
				break;
			case '[':
				OpenBracket(square_brackets, cursor_brackets, pos, i);
				break;
			case '{':
				OpenBracket(curly_brackets, cursor_brackets, pos, i);
				break;
			case ')':
				CloseBracket(brackets, cursor_brackets, pos, i, errors);
				break;
			case ']':
				CloseBracket(square_brackets, cursor_brackets, pos, i, errors);
				break;
			case '}':
				CloseBracket(curly_brackets, cursor_brackets, pos, i, errors);
				break;
			case '$': { // dollar symbol
				if (i + 1 >= len) {
					// we need more than just a dollar
					break;
				}
				// check if this is a dollar-quoted string
				idx_t next_dollar = 0;
				for (idx_t idx = i + 1; idx < len; idx++) {
					if (buf[idx] == '$') {
						// found the next dollar
						next_dollar = idx;
						break;
					}
					// all characters can be between A-Z, a-z, underscore, or \200 - \377
					if (buf[idx] >= 'A' && buf[idx] <= 'Z') {
						continue;
					}
					if (buf[idx] >= 'a' && buf[idx] <= 'z') {
						continue;
					}
					if (buf[idx] == '_') {
						continue;
					}
					if (buf[idx] >= '\200' && buf[idx] <= '\377') {
						continue;
					}
					// the first character CANNOT be a numeric, only subsequent characters
					if (idx > i + 1 && buf[idx] >= '0' && buf[idx] <= '9') {
						continue;
					}
					// not a dollar quoted string
					break;
				}
				if (next_dollar == 0) {
					// not a dollar quoted string
					break;
				}
				// dollar quoted string
				state = ScanState::DOLLAR_QUOTED_STRING;
				quote_pos = i;
				i = next_dollar;
				if (i < len) {
					// found a complete marker - store it
					idx_t marker_start = quote_pos + 1;
					dollar_quote_marker = string(buf + marker_start, i - marker_start);
				}
				break;
			}
			default:
				break;
			}
			break;
		case ScanState::IN_COMMENT:
			// comment state - the only thing that will get us out is a newline
			switch (c) {
			case '\r':
			case '\n':
				// newline - left comment state
				state = ScanState::STANDARD;
				comment_end.push_back(i);
				break;
			default:
				break;
			}
			break;
		case ScanState::IN_SINGLE_QUOTE:
			// single quote - all that will get us out is an unescaped single-quote
			if (c == '\'') {
				if (i + 1 < len && buf[i + 1] == '\'') {
					// double single-quote means the quote is escaped - continue
					i++;
					break;
				} else {
					// otherwise revert to standard scan state
					state = ScanState::STANDARD;
					break;
				}
			}
			break;
		case ScanState::IN_DOUBLE_QUOTE:
			// double quote - all that will get us out is an unescaped quote
			if (c == '"') {
				if (i + 1 < len && buf[i + 1] == '"') {
					// double quote means the quote is escaped - continue
					i++;
					break;
				} else {
					// otherwise revert to standard scan state
					state = ScanState::STANDARD;
					break;
				}
			}
			break;
		case ScanState::DOLLAR_QUOTED_STRING: {
			// dollar-quoted string - all that will get us out is a $[marker]$
			if (c != '$') {
				break;
			}
			if (i + 1 >= len) {
				// no room for the final dollar
				break;
			}
			// skip to the next dollar symbol
			idx_t start = i + 1;
			idx_t end = start;
			while (end < len && buf[end] != '$') {
				end++;
			}
			if (end >= len) {
				// no final dollar found - continue as normal
				break;
			}
			if (end - start != dollar_quote_marker.size()) {
				// length mismatch - cannot match
				break;
			}
			if (memcmp(buf + start, dollar_quote_marker.c_str(), dollar_quote_marker.size()) != 0) {
				// marker mismatch
				break;
			}
			// marker found! revert to standard state
			dollar_quote_marker = string();
			state = ScanState::STANDARD;
			i = end;
			break;
		}
		default:
			break;
		}
	}
	if (state == ScanState::IN_DOUBLE_QUOTE || state == ScanState::IN_SINGLE_QUOTE ||
	    state == ScanState::DOLLAR_QUOTED_STRING) {
		// quote is never closed
		errors.push_back(quote_pos);
	}
	HandleBracketErrors(brackets, errors);
	HandleBracketErrors(square_brackets, errors);
	HandleBracketErrors(curly_brackets, errors);

	// insert all the errors for highlighting
	for (auto &error : errors) {
		Linenoise::Log("Error found at position %llu\n", error);
		if (error < render_start || error > render_end) {
			continue;
		}
		auto render_error = error - render_start;
		InsertToken(tokenType::TOKEN_ERROR, render_error, tokens);
	}
	if (cursor_brackets.size() != 2) {
		// no matching cursor brackets found
		cursor_brackets.clear();
	}
	// insert bracket for highlighting
	for (auto &bracket_position : cursor_brackets) {
		Linenoise::Log("Highlight bracket at position %d\n", bracket_position);
		if (bracket_position < render_start || bracket_position > render_end) {
			continue;
		}

		idx_t render_position = bracket_position - render_start;
		InsertToken(tokenType::TOKEN_BRACKET, render_position, tokens);
	}
	// insert comments
	if (!comment_start.empty()) {
		vector<highlightToken> new_tokens;
		new_tokens.reserve(tokens.size());
		idx_t token_idx = 0;
		for (idx_t c = 0; c < comment_start.size(); c++) {
			auto c_start = comment_start[c];
			auto c_end = c < comment_end.size() ? comment_end[c] : len;
			if (c_start < render_start || c_end > render_end) {
				continue;
			}
			Linenoise::Log("Comment at position %d to %d\n", c_start, c_end);
			c_start -= render_start;
			c_end -= render_start;
			bool inserted_comment = false;

			highlightToken comment_token;
			comment_token.start = c_start;
			comment_token.type = tokenType::TOKEN_COMMENT;

			for (; token_idx < tokens.size(); token_idx++) {
				if (tokens[token_idx].start >= c_start) {
					// insert the comment here
					new_tokens.push_back(comment_token);
					inserted_comment = true;
					break;
				}
				new_tokens.push_back(tokens[token_idx]);
			}
			if (!inserted_comment) {
				new_tokens.push_back(comment_token);
			} else {
				// skip all tokens until we exit the comment again
				for (; token_idx < tokens.size(); token_idx++) {
					if (tokens[token_idx].start > c_end) {
						break;
					}
				}
			}
		}
		for (; token_idx < tokens.size(); token_idx++) {
			new_tokens.push_back(tokens[token_idx]);
		}
		tokens = std::move(new_tokens);
	}
}

static bool IsCompletionCharacter(char c) {
	if (c >= 'A' && c <= 'Z') {
		return true;
	}
	if (c >= 'a' && c <= 'z') {
		return true;
	}
	if (c == '_') {
		return true;
	}
	return false;
}

bool Linenoise::AddCompletionMarker(const char *buf, idx_t len, string &result_buffer,
                                    vector<highlightToken> &tokens) const {
	if (!enableCompletionRendering) {
		return false;
	}
	if (!continuation_markers) {
		// don't render when pressing ctrl+c, only when editing
		return false;
	}
	static constexpr const idx_t MAX_COMPLETION_LENGTH = 1000;
	if (len >= MAX_COMPLETION_LENGTH) {
		return false;
	}
	if (!insert || pos != len) {
		// only show when inserting a character at the end
		return false;
	}
	if (pos < 3) {
		// we need at least 3 bytes
		return false;
	}
	if (!tokens.empty() && tokens.back().type == tokenType::TOKEN_ERROR) {
		// don't show auto-completion when we have errors
		return false;
	}
	// we ONLY show completion if we have typed at least three characters that are supported for completion
	// for now this is ONLY the characters a-z, A-Z and underscore (_)
	for (idx_t i = pos - 3; i < pos; i++) {
		if (!IsCompletionCharacter(buf[i])) {
			return false;
		}
	}
	auto completion = TabComplete();
	if (completion.completions.empty()) {
		// no completions found
		return false;
	}
	if (completion.completions[0].completion.size() <= len) {
		// completion is not long enough
		return false;
	}
	// we have stricter requirements for rendering completions - the completion must match exactly
	for (idx_t i = pos; i > 0; i--) {
		auto cpos = i - 1;
		if (!IsCompletionCharacter(buf[cpos])) {
			break;
		}
		if (tolower(completion.completions[0].completion[cpos]) != tolower(buf[cpos])) {
			return false;
		}
	}
	// add the first completion found for rendering purposes
	result_buffer = string(buf, len);
	result_buffer += completion.completions[0].completion.substr(len);

	highlightToken completion_token;
	completion_token.start = len;
	completion_token.type = tokenType::TOKEN_COMMENT;
	completion_token.extra_highlighting = ExtraHighlightingType::UNDERLINE;
	tokens.push_back(completion_token);
	return true;
}

/* Multi line low level line refresh.
 *
 * Rewrite the currently edited line accordingly to the buffer content,
 * cursor position, and number of columns of the terminal. */
void Linenoise::RefreshMultiLine() {
	if (!render) {
		return;
	}
	char seq[64];
	int plen = GetPromptWidth();
	// utf8 in prompt, get render width
	RenderTruncation truncation = RenderTruncation::NO_TRUNCATE;
	int rows, cols;
	int new_cursor_row, new_cursor_x;
	PositionToColAndRow(pos, new_cursor_row, new_cursor_x, rows, cols);
	int col; /* column position, zero-based. */
	idx_t old_rows = maxrows ? maxrows : 1;
	int fd = ofd;
	std::string highlight_buffer;
	auto render_buf = this->buf;
	auto render_len = this->len;
	idx_t render_start = 0;
	idx_t render_end = render_len;
	if (clear_screen) {
		old_cursor_rows = 0;
		old_rows = 0;
		clear_screen = false;
	}
	idx_t max_rows_to_render = ws.ws_row;
	if (render_completion_suggestion && !completion_list.completions.empty()) {
		// if we are rendering completions keep at least one line clear for rendering them
		max_rows_to_render--;
	}
	if (NumericCast<idx_t>(rows) > max_rows_to_render) {
		if (max_rows_to_render == NumericCast<idx_t>(ws.ws_row) && rendered_completion_lines > 0) {
			y_scroll--;
		}
		// the text does not fit in the terminal (too many rows)
		// enable scrolling mode
		// check if, given the current y_scroll, the cursor is visible
		// display range is [y_scroll, y_scroll + ws.ws_row]
		if (new_cursor_row < int(y_scroll) + 1) {
			y_scroll = new_cursor_row - 1;
		} else if (new_cursor_row > int(y_scroll) + int(max_rows_to_render)) {
			y_scroll = new_cursor_row - max_rows_to_render;
		}
		// display only characters up to the current scroll position
		bool truncate_top = false, truncate_bottom = false;
		if (y_scroll == 0) {
			render_start = 0;
		} else {
			render_start = ColAndRowToPosition(y_scroll, 0);
			truncate_top = true;
		}
		if (int(y_scroll) + int(max_rows_to_render) >= rows) {
			render_end = len;
		} else {
			render_end = ColAndRowToPosition(y_scroll + max_rows_to_render, 99999);
			truncate_bottom = true;
		}
		if (truncate_top && truncate_bottom) {
			truncation = RenderTruncation::TRUNCATE_BOTH;
		} else if (truncate_top) {
			truncation = RenderTruncation::TRUNCATE_TOP;
		} else if (truncate_bottom) {
			truncation = RenderTruncation::TRUNCATE_BOTTOM;
		}
		new_cursor_row -= y_scroll;
		render_buf += render_start;
		render_len = render_end - render_start;
		Linenoise::Log("truncate to rows %d - %d (render bytes %d to %d)", y_scroll, y_scroll + max_rows_to_render,
		               render_start, render_end);
		rows = max_rows_to_render;
	} else {
		y_scroll = 0;
	}

	/* Update maxrows if needed. */
	if (rows > (int)maxrows) {
		maxrows = rows;
	}

	vector<highlightToken> tokens;
	if (Highlighting::IsEnabled()) {
		bool is_dot_command = buf[0] == '.' || buf[0] == '!';
		tokens = Highlighting::Tokenize(render_buf, render_len, is_dot_command);

		// add error highlighting
		AddErrorHighlighting(render_start, render_end, tokens);

		// add any extra highlighting (search match, autocomplete)
		auto match = search_index < search_matches.size() ? &search_matches[search_index] : nullptr;
		ExtraHighlighting extra_highlighting;
		if (match) {
			// search match - add underline under the match
			extra_highlighting.start = match->match_start;
			extra_highlighting.end = match->match_end;
			extra_highlighting.type = ExtraHighlightingType::UNDERLINE;
		} else if (completion_idx.IsValid()) {
			// auto-completing - bold-face the extra character (if any)
			auto &completion = completion_list.completions[completion_idx.GetIndex()];
			if (completion.extra_char != '\0') {
				extra_highlighting.start = completion.extra_char_pos;
				extra_highlighting.end = completion.extra_char_pos + 1;
				extra_highlighting.type = ExtraHighlightingType::BOLD;
			}
		}
		Highlighting::AddExtraHighlighting(render_len, tokens, extra_highlighting);

		// add completion hint
		if (AddCompletionMarker(render_buf, render_len, highlight_buffer, tokens)) {
			render_buf = (char *)highlight_buffer.c_str();
			render_len = highlight_buffer.size();
		}
	}
	if (rows > 1) {
		// add continuation markers
		highlight_buffer =
		    AddContinuationMarkers(render_buf, render_len, plen, y_scroll > 0 ? new_cursor_row + 1 : new_cursor_row,
		                           y_scroll > 0 ? rows : rows - 1, tokens, truncation);
		render_buf = (char *)highlight_buffer.c_str();
		render_len = highlight_buffer.size();
	}
	if (duckdb::Utf8Proc::IsValid(render_buf, render_len)) {
		if (Highlighting::IsEnabled()) {
			highlight_buffer = Highlighting::HighlightText(render_buf, render_len, 0, render_len, tokens);
			render_buf = (char *)highlight_buffer.c_str();
			render_len = highlight_buffer.size();
		}
	}

	/* First step: clear all the lines used before. To do so start by
	 * going to the last row. */
	AppendBuffer append_buffer;
	if (old_rows > old_cursor_rows) {
		int extra_rows = old_rows - old_cursor_rows;
		Linenoise::Log("go down %d\n", extra_rows);
		snprintf(seq, 64, "\x1b[%dB", extra_rows);
		append_buffer.Append(seq);
	}

	/* Now for every row clear it, go up. */
	for (idx_t j = 0; j + 1 < old_rows; j++) {
		Linenoise::Log("clear+up\n");
		append_buffer.Append("\r\x1b[0K\x1b[1A");
	}

	/* Clean the top line. */
	Linenoise::Log("clear");
	append_buffer.Append("\r\x1b[0K");

	/* Write the prompt and the current buffer content */
	if (y_scroll == 0) {
		append_buffer.Append(prompt);
	}
	append_buffer.Append(render_buf, render_len);

	/* If we are at the very end of the screen with our prompt, we need to
	 * emit a newline and move the prompt to the first column. */
	Linenoise::Log("pos > 0 %d\n", pos > 0 ? 1 : 0);
	Linenoise::Log("pos == len %d\n", pos == len ? 1 : 0);
	Linenoise::Log("new_cursor_x == cols %d\n", new_cursor_x == ws.ws_col ? 1 : 0);
	if (pos > 0 && pos == len && new_cursor_x == ws.ws_col) {
		Linenoise::Log("<newline>\n");
		append_buffer.Append("\n");
		append_buffer.Append("\r");
		rows++;
		new_cursor_row++;
		new_cursor_x = 0;
		if (rows > (int)maxrows) {
			maxrows = rows;
		}
	}

	if (render_completion_suggestion && !completion_list.completions.empty()) {
		// if we are tab-completing - write the list of completions one line below
		append_buffer.Append("\r\n");

		// figure out how to align the completions
		// we need to figure out how many "columns" we render
		idx_t max_length = 0;
		idx_t max_split_length = (ws.ws_col / 2) - 2;
		for (auto &completion : completion_list.completions) {
			auto &completion_text = completion.original_completion;
			if (!completion.original_completion_length.IsValid()) {
				completion.original_completion_length =
				    linenoiseComputeRenderWidth(completion_text.c_str(), completion_text.size());
			}
			idx_t completion_length = completion.original_completion_length.GetIndex();
			if (completion_length > max_split_length) {
				// oversized value - we treat these differently / separately - ignore here
			} else if (completion_length > max_length) {
				max_length = completion_length;
			}
		}

		// now based on the max width determine the column count
		// we need at least one space between each entry
		max_length++;
		string completion_text;
		idx_t column_count = ws.ws_col / max_length;
		idx_t column_index = 0;
		idx_t rendered_rows = 1;
		for (idx_t i = 0; i < completion_list.completions.size(); i++) {
			auto &completion = completion_list.completions[i];
			auto &rendered_text = completion.original_completion;
			auto element_type = duckdb_shell::HighlightElementType::NONE;
			switch (completion.completion_type) {
			case CompletionType::KEYWORD:
			case CompletionType::TYPE_NAME:
				element_type = duckdb_shell::HighlightElementType::KEYWORD;
				break;
			case CompletionType::CATALOG_NAME:
				element_type = duckdb_shell::HighlightElementType::SUGGESTION_CATALOG_NAME;
				break;
			case CompletionType::SCHEMA_NAME:
				element_type = duckdb_shell::HighlightElementType::SUGGESTION_SCHEMA_NAME;
				break;
			case CompletionType::TABLE_NAME:
				element_type = duckdb_shell::HighlightElementType::SUGGESTION_TABLE_NAME;
				break;
			case CompletionType::COLUMN_NAME:
				element_type = duckdb_shell::HighlightElementType::SUGGESTION_COLUMN_NAME;
				break;
			case CompletionType::FILE_NAME:
				element_type = duckdb_shell::HighlightElementType::SUGGESTION_FILE_NAME;
				break;
			case CompletionType::DIRECTORY_NAME:
				element_type = duckdb_shell::HighlightElementType::SUGGESTION_DIRECTORY_NAME;
				break;
			case CompletionType::SCALAR_FUNCTION:
			case CompletionType::TABLE_FUNCTION:
			case CompletionType::PRAGMA_FUNCTION:
				element_type = duckdb_shell::HighlightElementType::SUGGESTION_FUNCTION_NAME;
				break;
			case CompletionType::SETTING_NAME:
				element_type = duckdb_shell::HighlightElementType::SUGGESTION_SETTING_NAME;
				break;
			default:
				break;
			}
			auto &element = duckdb_shell::ShellHighlight::GetHighlightElement(element_type);
			auto color = element.color;
			auto intensity = element.intensity;
			if (completion_idx.IsValid() && i == completion_idx.GetIndex()) {
				// underline selected completion
				if (intensity == duckdb_shell::PrintIntensity::BOLD) {
					intensity = duckdb_shell::PrintIntensity::BOLD_UNDERLINE;
				} else {
					intensity = duckdb_shell::PrintIntensity::UNDERLINE;
				}
			}
			auto completion_length = completion.original_completion_length.GetIndex();
			bool is_oversized_value = completion_length > max_split_length;
			if (is_oversized_value) {
				// oversized column - start a new line if this is not the beginning of a line
				if (column_index > 0) {
					// have to wrap around - add a newline
					if (rendered_rows + 1 + NumericCast<idx_t>(rows) > NumericCast<idx_t>(ws.ws_row)) {
						// too many rows - stop rendering completion results
						break;
					}
					completion_text += "\r\n";
					column_index = 0;
					rendered_rows++;
				}
				// oversized values might need multiple lines - check if there is space
				idx_t total_lines = completion_length / ws.ws_row;
				if (rendered_rows + total_lines + NumericCast<idx_t>(rows) > NumericCast<idx_t>(ws.ws_row)) {
					// no space - truncate to a single line
					idx_t max_render_pos =
					    ColAndRowToPosition(0, rendered_text.c_str(), rendered_text.size(), 0, ws.ws_col);
					rendered_text = rendered_text.substr(0, max_render_pos);
				} else {
					// if there is space increment the total rendered rows
					rendered_rows += total_lines;
				}
			}
			auto terminal_text = duckdb_shell::ShellHighlight::TerminalCode(color, intensity);
			completion_text += terminal_text;
			completion_text += rendered_text;
			if (!terminal_text.empty()) {
				completion_text += duckdb_shell::ShellHighlight::ResetTerminalCode();
			}
			if (i + 1 == completion_list.completions.size()) {
				continue;
			}
			// add spaces to pad so we get nicely aligned suggestions
			// unless this is an oversized value
			column_index++;
			if (column_index >= column_count || is_oversized_value) {
				// have to wrap around - add a newline
				if (rendered_rows + 1 + NumericCast<idx_t>(rows) > NumericCast<idx_t>(ws.ws_row)) {
					// too many rows - stop rendering completion results
					break;
				}
				completion_text += "\r\n";
				column_index = 0;
				rendered_rows++;
			} else {
				idx_t space_count = max_length - completion_length;
				completion_text += string(space_count, ' ');
			}
		}

		// write the set of tab completions
		int completion_rows, completion_cols;
		int unused_row, unused_x;
		PositionToColAndRow(0, completion_text.c_str(), completion_text.size(), 0, unused_row, unused_x,
		                    completion_rows, completion_cols);
		append_buffer.Append(completion_text.c_str(), completion_text.size());

		rendered_completion_lines = completion_rows;
		rows += static_cast<int>(rendered_completion_lines);
		if (rows > (int)maxrows) {
			maxrows = rows;
		}
		Linenoise::Log("auto-complete lines %d\n", int(rendered_completion_lines));
	} else {
		rendered_completion_lines = 0;
	}

	Linenoise::Log("render %d rows (old rows %d)\n", rows, old_rows);

	/* Move cursor to right position. */
	Linenoise::Log("new_cursor_row %d\n", new_cursor_row);
	Linenoise::Log("new_cursor_x %d\n", new_cursor_x);
	Linenoise::Log("len %d\n", len);
	Linenoise::Log("old_cursor_rows %d\n", old_cursor_rows);
	Linenoise::Log("pos %d\n", pos);
	Linenoise::Log("max cols %d\n", ws.ws_col);
	Linenoise::Log("rows %d\n", int(rows));
	Linenoise::Log("max rows %d\n", int(maxrows));

	/* Go up till we reach the expected position. */
	if (rows - new_cursor_row > 0) {
		Linenoise::Log("go-up %d\n", rows - new_cursor_row);
		snprintf(seq, 64, "\x1b[%dA", rows - new_cursor_row);
		append_buffer.Append(seq);
	}

	/* Set column. */
	col = new_cursor_x;
	Linenoise::Log("set col %d\n", 1 + col);
	if (col) {
		snprintf(seq, 64, "\r\x1b[%dC", col);
	} else {
		snprintf(seq, 64, "\r");
	}
	append_buffer.Append(seq);

	Linenoise::Log("\n");
	old_cursor_rows = new_cursor_row;
	append_buffer.Write(fd);
}

/* Calls the two low level functions refreshSingleLine() or
 * refreshMultiLine() according to the selected mode. */
void Linenoise::RefreshLine() {
	if (Terminal::IsMultiline()) {
		RefreshMultiLine();
	} else {
		RefreshSingleLine();
	}
}

} // namespace duckdb
