#include "linenoise.hpp"
#include "linenoise.h"
#include "highlighting.hpp"

#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/peg/keyword_helper/duckdb_keyword_helper.hpp"
#include "duckdb/parser/peg/compiled_grammar.hpp"
#include "duckdb/common/string.hpp"
#include "shell_highlight.hpp"
#include "duckdb/parser/peg/tokenizer/highlight_tokenizer.hpp"

namespace duckdb {

bool Highlighting::IsEnabled() {
	return duckdb_shell::ShellHighlight::IsEnabled();
}

static tokenType convertToken(TokenType token_type) {
	switch (token_type) {
	case TokenType::IDENTIFIER:
		return tokenType::TOKEN_IDENTIFIER;
	case TokenType::NUMBER_LITERAL:
		return tokenType::TOKEN_NUMERIC_CONSTANT;
	case TokenType::STRING_LITERAL:
		return tokenType::TOKEN_STRING_CONSTANT;
	case TokenType::OPERATOR:
	case TokenType::TERMINATOR: // FIXME(Dtenwolde): Should become a special token for highlighting
		return tokenType::TOKEN_OPERATOR;
	case TokenType::KEYWORD:
		return tokenType::TOKEN_KEYWORD;
	case TokenType::COMMENT:
		return tokenType::TOKEN_COMMENT;
	case TokenType::TOKEN_ERROR:
		return tokenType::TOKEN_ERROR;
	case TokenType::END_OF_INPUT:
	case TokenType::END_OF_INPUT_AUTOCOMPLETE:
		return tokenType::TOKEN_OPERATOR;
	default:
		throw duckdb::InternalException("Unrecognized token type");
	}
}

static vector<highlightToken> GetParseTokens(char *buf, size_t len) {
	string sql(buf, len);
	vector<highlightToken> tokens;
	vector<MatcherToken> matcher_tokens;
	HighlightTokenizerBehavior behavior(sql, matcher_tokens);
	auto &keyword_helper = DuckDBKeywordHelper::Instance();
	Tokenizer tokenizer(keyword_helper);
	tokenizer.TokenizeInput(behavior);
	vector<SimplifiedToken> result;
	result.reserve(matcher_tokens.size());
	for (auto &token : matcher_tokens) {
		highlightToken new_token;
		if (token.unterminated) {
			new_token.type = tokenType::TOKEN_ERROR;
		} else if (token.type == TokenType::IDENTIFIER && keyword_helper.IsGrammarWord(token.text)) {
			// a word the grammar matches literally that is in none of the keyword lists, e.g. REPOSITORY
			new_token.type = tokenType::TOKEN_KEYWORD;
		} else {
			new_token.type = convertToken(token.type);
		}
		new_token.start = token.offset;
		tokens.push_back(new_token);
	}

	if (!tokens.empty() && tokens[0].start > 0) {
		highlightToken new_token;
		new_token.type = tokenType::TOKEN_IDENTIFIER;
		new_token.start = 0;
		tokens.insert(tokens.begin(), new_token);
	}
	if (tokens.empty() && sql.size() > 0) {
		highlightToken new_token;
		new_token.type = tokenType::TOKEN_IDENTIFIER;
		new_token.start = 0;
		tokens.push_back(new_token);
	}
	return tokens;
}

static vector<highlightToken> GetDotCommandTokens(char *buf, size_t len) {
	vector<highlightToken> tokens;

	// identifier token for the dot command itself
	highlightToken dot_token;
	dot_token.type = tokenType::TOKEN_KEYWORD;
	dot_token.start = 0;
	tokens.push_back(dot_token);

	for (idx_t i = 0; i + 1 < len; i++) {
		if (Linenoise::IsSpace(buf[i])) {
			highlightToken argument_token;
			argument_token.type = tokenType::TOKEN_STRING_CONSTANT;
			argument_token.start = i + 1;
			tokens.push_back(argument_token);
		}
	}
	return tokens;
}

void Highlighting::AddExtraHighlighting(size_t len, vector<highlightToken> &tokens,
                                        ExtraHighlighting extra_highlighting) {
	if (tokens.empty() || extra_highlighting.type == ExtraHighlightingType::NONE) {
		return;
	}
	// we have a extra highlighting - insert it into the token list
	// we want to insert an extra token with start = match_start, end = match_end
	// first figure out which token type we would have at match_end (if any)
	for (size_t i = 0; i + 1 < tokens.size(); i++) {
		if (tokens[i].start <= extra_highlighting.start && tokens[i + 1].start >= extra_highlighting.start) {
			// this token begins after the search position, insert the token here
			size_t token_position = i + 1;
			auto end_type = tokens[i].type;
			if (tokens[i].start == extra_highlighting.start) {
				// exact start: only set the search match
				tokens[i].extra_highlighting = extra_highlighting.type;
			} else {
				// non-exact start: add a new token
				highlightToken search_token;
				search_token.type = tokens[i].type;
				search_token.start = extra_highlighting.start;
				search_token.extra_highlighting = extra_highlighting.type;
				tokens.insert(tokens.begin() + static_cast<int64_t>(token_position), search_token);
				token_position++;
			}

			// move forwards
			while (token_position < tokens.size() && tokens[token_position].start < extra_highlighting.end) {
				// this token is
				// mark this token as a search token
				end_type = tokens[token_position].type;
				tokens[token_position].extra_highlighting = extra_highlighting.type;
				token_position++;
			}
			if (token_position >= tokens.size() || tokens[token_position].start > extra_highlighting.end) {
				// insert the token that marks the end of the search
				highlightToken end_token;
				end_token.type = end_type;
				end_token.start = extra_highlighting.end;
				tokens.insert(tokens.begin() + static_cast<int64_t>(token_position), end_token);
				token_position++;
			}
			return;
		}
	}
	// we didn't manage to add the extra highlighting - this means the token is in the end of the buffer
	// insert a new token at the end with this extra highlighting
	if (extra_highlighting.start >= tokens.back().start) {
		highlightToken start_token;
		start_token.type = tokens.back().type;
		start_token.start = extra_highlighting.start;
		start_token.extra_highlighting = extra_highlighting.type;
		tokens.push_back(start_token);

		if (extra_highlighting.end < len) {
			// resume original rendering
			highlightToken end_token;
			end_token.type = tokens.back().type;
			end_token.start = extra_highlighting.end;
			tokens.push_back(end_token);
		}
	}
}

vector<highlightToken> Highlighting::Tokenize(char *buf, size_t len, bool is_dot_command) {
	vector<highlightToken> tokens;
	if (!is_dot_command) {
		// SQL query - use parser to obtain tokens
		tokens = GetParseTokens(buf, len);
	} else {
		// . command
		tokens = GetDotCommandTokens(buf, len);
	}
	return tokens;
}

string Highlighting::HighlightText(char *buf, size_t len, size_t start_pos, size_t end_pos,
                                   const vector<highlightToken> &tokens) {
	duckdb::stringstream ss;
	size_t prev_pos = 0;
	for (size_t i = 0; i < tokens.size(); i++) {
		size_t next = i + 1 < tokens.size() ? tokens[i + 1].start : len;
		if (next < start_pos) {
			// this token is not rendered at all
			continue;
		}

		auto &token = tokens[i];
		size_t start = token.start > start_pos ? token.start : start_pos;
		size_t end = next > end_pos ? end_pos : next;
		if (end <= start) {
			continue;
		}
		if (prev_pos > start) {
#ifdef DEBUG
			throw InternalException("ERROR - Rendering at position %llu after rendering at position %llu\n", start,
			                        prev_pos);
#endif
			Linenoise::Log("ERROR - Rendering at position %llu after rendering at position %llu\n", start, prev_pos);
			continue;
		}
		prev_pos = start;
		std::string text = std::string(buf + start, end - start);

		duckdb_shell::HighlightElementType element_type;
		switch (token.type) {
		case tokenType::TOKEN_KEYWORD:
			element_type = duckdb_shell::HighlightElementType::KEYWORD;
			break;
		case tokenType::TOKEN_NUMERIC_CONSTANT:
			element_type = duckdb_shell::HighlightElementType::NUMERIC_CONSTANT;
			break;
		case tokenType::TOKEN_STRING_CONSTANT:
			element_type = duckdb_shell::HighlightElementType::STRING_CONSTANT;
			break;
		case tokenType::TOKEN_CONTINUATION:
			element_type = duckdb_shell::HighlightElementType::CONTINUATION;
			break;
		case tokenType::TOKEN_CONTINUATION_SELECTED:
			element_type = duckdb_shell::HighlightElementType::CONTINUATION_SELECTED;
			break;
		case tokenType::TOKEN_BRACKET:
			element_type = duckdb_shell::HighlightElementType::BRACKET;
			break;
		case tokenType::TOKEN_ERROR:
			element_type = duckdb_shell::HighlightElementType::ERROR_TOKEN;
			break;
		case tokenType::TOKEN_COMMENT:
			element_type = duckdb_shell::HighlightElementType::COMMENT;
			break;
		default:
			element_type = duckdb_shell::HighlightElementType::NONE;
			break;
		}
		auto &element = duckdb_shell::ShellHighlight::GetHighlightElement(element_type);
		auto color = element.color;
		auto intensity = element.intensity;
		if (token.extra_highlighting == ExtraHighlightingType::UNDERLINE) {
			// add underline
			if (intensity == duckdb_shell::PrintIntensity::BOLD) {
				intensity = duckdb_shell::PrintIntensity::BOLD_UNDERLINE;
			} else {
				intensity = duckdb_shell::PrintIntensity::UNDERLINE;
			}
		} else if (token.extra_highlighting == ExtraHighlightingType::BOLD) {
			// add bold
			if (intensity == duckdb_shell::PrintIntensity::UNDERLINE) {
				intensity = duckdb_shell::PrintIntensity::BOLD_UNDERLINE;
			} else if (intensity == duckdb_shell::PrintIntensity::BOLD) {
				// might be a weird choice - but "flip" bold fonts if we encounter them
				intensity = duckdb_shell::PrintIntensity::STANDARD;
			} else {
				intensity = duckdb_shell::PrintIntensity::BOLD;
			}
		}
		string terminal_code = duckdb_shell::ShellHighlight::TerminalCode(color, intensity);
		if (terminal_code.empty()) {
			ss << text;
		} else {
			ss << terminal_code;
			ss << text;
			ss << duckdb_shell::ShellHighlight::ResetTerminalCode();
		}
	}
	return ss.str();
}

} // namespace duckdb
