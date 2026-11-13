#pragma once

#include "duckdb/parser/peg/tokenizer/tokenizer.hpp"
#include "duckdb/parser/peg/keyword_helper.hpp"
#include "duckdb/parser/peg/matcher.hpp"

namespace duckdb {

class IdentifierMatcher : public AtomicMatcher {
public:
	static constexpr MatcherType TYPE = MatcherType::VARIABLE;

public:
	IdentifierMatcher(SuggestionState suggestion_type, const PEGKeywordHelper &keyword_helper_p)
	    : AtomicMatcher(TYPE), suggestion_type(suggestion_type), literal_table(keyword_helper_p.GetLiteralTable()),
	      identifier_mask(keyword_helper_p.GetIdentifierMask(suggestion_type)) {
	}

	static bool IsQuoteDelimiter(char c) {
		return c == '"' || c == '`';
	}

	bool IsQuoted(const string &text) const {
		if (text.size() < 2) {
			return false;
		}
		return IsQuoteDelimiter(text.front()) && text.front() == text.back();
	}

	//! Strip the surrounding delimiters of a quoted identifier and un-double any escaped delimiters
	static void UnquoteIdentifier(string &text) {
		const string delimiter(1, text.front());
		text = text.substr(1, text.size() - 2);
		text = StringUtil::Replace(text, delimiter + delimiter, delimiter);
	}

	bool IsSingleQuoted(const string &text) const {
		if (text.front() == '\'' && text.back() == '\'') {
			return true;
		}
		return false;
	}

	bool IsIdentifier(const string &text) const {
		if (text.empty()) {
			return false;
		}
		if (IsSingleQuoted(text) && SupportsStringLiteral()) {
			return true;
		}
		if (IsQuoted(text)) {
			return true;
		}
		if (Tokenizer::CharacterIsInitialNumber(text[0])) {
			return false;
		}
		if (text.back() == '\'') {
			// a prefixed string literal such as x'41' or e'ab'
			return false;
		}
		return Tokenizer::CharacterIsKeyword(text[0]);
	}

	MatcherResult MatchAtomic(MatchState &state) const override {
		auto token = state.token_iterator.Current();
		if (!token) {
			return MatcherResult::Failure();
		}
		const auto &token_text = token->text;
		auto start_offset = optional_idx(token->offset);
		auto token_length = optional_idx(token->length);
		if (!MatchIdentifier(state)) {
			return MatcherResult::Failure();
		}
		state.token_iterator.SetPreviousTokenType(GetTokenType());
		if (!state.BuildParseResult()) {
			return MatcherResult::Success();
		}

		string result_text = token_text;
		if (IsQuoted(result_text)) {
			UnquoteIdentifier(result_text);
		} else if (IsSingleQuoted(result_text) && SupportsStringLiteral()) {
			// a single-quoted token in a table or file-name position is a path, so it is unwrapped but never folded
			result_text = result_text.substr(1, result_text.size() - 2);
			result_text = StringUtil::Replace(result_text, "''", "'");
		} else {
			state.FoldIdentifier(result_text);
		}
		return state.AllocateParseResult<IdentifierParseResult>(result_text, start_offset, token_length);
	}

	TokenType GetTokenType() const {
		switch (suggestion_type) {
		case SuggestionState::SUGGEST_CATALOG_NAME:
			return TokenType::CATALOG_NAME;
		case SuggestionState::SUGGEST_SCHEMA_NAME:
			return TokenType::SCHEMA_NAME;
		case SuggestionState::SUGGEST_TABLE_NAME:
			return TokenType::TABLE_NAME;
		case SuggestionState::SUGGEST_TYPE_NAME:
			return TokenType::TYPE_NAME;
		case SuggestionState::SUGGEST_COLUMN_NAME:
			return TokenType::COLUMN_NAME;
		case SuggestionState::SUGGEST_SCALAR_FUNCTION_NAME:
			return TokenType::SCALAR_FUNCTION;
		case SuggestionState::SUGGEST_TABLE_FUNCTION_NAME:
			return TokenType::TABLE_FUNCTION;
		case SuggestionState::SUGGEST_PRAGMA_NAME:
			return TokenType::PRAGMA_FUNCTION;
		case SuggestionState::SUGGEST_SETTING_NAME:
			return TokenType::SETTING_NAME;
		default:
			return TokenType::IDENTIFIER;
		}
	}

	bool SupportsStringLiteral() const {
		switch (suggestion_type) {
		case SuggestionState::SUGGEST_TABLE_NAME:
		case SuggestionState::SUGGEST_FILE_NAME:
			return true;
		default:
			return false;
		}
	}

	SuggestionType AddSuggestionInternal(MatchState &state) const override {
		state.AddSuggestion(MatcherSuggestion(suggestion_type));
		return SuggestionType::MANDATORY;
	}

	string ToString() const override {
		switch (suggestion_type) {
		case SuggestionState::SUGGEST_KEYWORD:
			return "KEYWORD";
		case SuggestionState::SUGGEST_CATALOG_NAME:
			return "CATALOG_NAME";
		case SuggestionState::SUGGEST_SCHEMA_NAME:
			return "SCHEMA_NAME";
		case SuggestionState::SUGGEST_TABLE_NAME:
			return "TABLE_NAME";
		case SuggestionState::SUGGEST_TYPE_NAME:
			return "TYPE_NAME";
		case SuggestionState::SUGGEST_COLUMN_NAME:
			return "COLUMN_NAME";
		case SuggestionState::SUGGEST_FILE_NAME:
			return "FILE_NAME";
		case SuggestionState::SUGGEST_SCALAR_FUNCTION_NAME:
			return "SCALAR_FUNCTION_NAME";
		case SuggestionState::SUGGEST_TABLE_FUNCTION_NAME:
			return "TABLE_FUNCTION_NAME";
		case SuggestionState::SUGGEST_PRAGMA_NAME:
			return "PRAGMA_NAME";
		case SuggestionState::SUGGEST_SETTING_NAME:
			return "SETTING_NAME";
		case SuggestionState::SUGGEST_VARIABLE:
			return "VARIABLE";
		default:
			return "?VARIABLE?";
		}
	}

	void InitializeFirstSet(MatcherFirstSet &first_set, const GrammarLiteralTable &table) const override {
		first_set.class_mask = MatcherTokenClass::WORD;
	}

private:
	bool IsAllowedKeyword(TokenIterator &tokens) const {
		auto info = tokens.CurrentLiteralInfo(literal_table);
		return !info.IsKeyword() || info.HasAnyFlags(identifier_mask);
	}

	bool MatchIdentifier(MatchState &state) const {
		auto token = state.token_iterator.Current();
		if (!token) {
			return false;
		}
		auto &token_text = token->text;
		if (!IsAllowedKeyword(state.token_iterator) || !IsIdentifier(token_text)) {
			return false;
		}
		state.token_iterator.Advance();
		state.UpdateMaxTokenIndex();
		return true;
	}

	SuggestionState suggestion_type;
	const GrammarLiteralTable &literal_table;
	const keyword_categories_t identifier_mask;
};

class ReservedIdentifierMatcher : public IdentifierMatcher {
public:
	static constexpr MatcherType TYPE = MatcherType::VARIABLE;

public:
	ReservedIdentifierMatcher(SuggestionState suggestion_type, const PEGKeywordHelper &keyword_helper)
	    : IdentifierMatcher(suggestion_type, keyword_helper) {
	}

	MatcherResult MatchAtomic(MatchState &state) const override {
		auto token = state.token_iterator.Current();
		if (!token) {
			return MatcherResult::Failure();
		}
		auto &token_text = token->text;
		auto start_offset = optional_idx(token->offset);
		auto token_length = optional_idx(token->length);
		if (!MatchReservedIdentifier(state)) {
			return MatcherResult::Failure();
		}
		state.token_iterator.SetPreviousTokenType(GetTokenType());
		if (!state.BuildParseResult()) {
			return MatcherResult::Success();
		}
		string result_text = token_text;
		// unlike IdentifierMatcher this rule does not unwrap path literals, it only has to avoid folding them
		const bool is_path_literal = IsSingleQuoted(result_text) && SupportsStringLiteral();
		if (IsQuoted(result_text)) {
			UnquoteIdentifier(result_text);
		} else if (!is_path_literal) {
			state.FoldIdentifier(result_text);
		}
		return state.AllocateParseResult<IdentifierParseResult>(result_text, start_offset, token_length);
	}

private:
	bool MatchReservedIdentifier(MatchState &state) const {
		auto token = state.token_iterator.Current();
		if (!token) {
			return false;
		}
		auto &token_text = token->text;
		if (!IsIdentifier(token_text)) {
			return false;
		}
		state.token_iterator.Advance();
		state.UpdateMaxTokenIndex();
		return true;
	}
};

} // namespace duckdb
