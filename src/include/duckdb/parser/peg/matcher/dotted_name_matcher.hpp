#pragma once

#include "duckdb/parser/peg/tokenizer/tokenizer.hpp"
#include "duckdb/parser/peg/matcher.hpp"

namespace duckdb {

//! Matches a dotted name spelled without quotes: 'carlopi.github.io', 'my-website.com'. The tokenizer cuts such a name
//! into words, '.' and '-' tokens; this matcher glues them back together as long as they touch (no whitespace or
//! comment in between) and the result is a dotted name: two or more words of letters, digits, '_' and '-' (so a word
//! may be hyphenated), separated by single dots, with no word starting or ending with '-'.
class DottedNameMatcher : public AtomicMatcher {
public:
	static constexpr MatcherType TYPE = MatcherType::VARIABLE;

public:
	DottedNameMatcher() : AtomicMatcher(TYPE) {
		name = "DottedName";
	}

	MatcherResult MatchAtomic(MatchState &state) const override {
		auto token = state.token_iterator.Current();
		if (!token) {
			return MatcherResult::Failure();
		}
		auto start_offset = token->offset;
		string text;
		idx_t end_offset = start_offset;
		idx_t token_count = 0;
		while (true) {
			auto next = state.token_iterator.Current();
			if (!next || next->offset != end_offset || !IsNamePart(*next)) {
				break;
			}
			text += next->text;
			end_offset = next->offset + next->length;
			state.token_iterator.Advance();
			token_count++;
		}
		if (token_count == 0 || !IsDottedName(text)) {
			return MatcherResult::Failure();
		}
		state.UpdateMaxTokenIndex();
		state.token_iterator.SetPreviousTokenType(TokenType::IDENTIFIER);
		if (!state.BuildParseResult()) {
			return MatcherResult::Success();
		}
		state.FoldIdentifier(text);
		return state.AllocateParseResult<IdentifierParseResult>(text, optional_idx(start_offset),
		                                                        optional_idx(end_offset - start_offset));
	}

	SuggestionType AddSuggestionInternal(MatchState &state) const override {
		return SuggestionType::MANDATORY;
	}

	string ToString() const override {
		return "DOTTED_NAME";
	}

	void InitializeFirstSet(MatcherFirstSet &first_set, const GrammarLiteralTable &table) const override {
		first_set.class_mask = MatcherTokenClass::WORD | MatcherTokenClass::NUMBER;
	}

	//! A dotted name: labels of letters, digits, '_' and '-' separated by single dots, two or more of them
	static bool IsDottedName(const string &text) {
		idx_t label_count = 0;
		idx_t label_start = 0;
		for (idx_t i = 0; i <= text.size(); i++) {
			if (i < text.size() && text[i] != '.') {
				auto c = text[i];
				if (!StringUtil::CharacterIsAlpha(c) && !StringUtil::CharacterIsDigit(c) && c != '_' && c != '-') {
					return false;
				}
				continue;
			}
			// end of a label
			if (i == label_start || text[label_start] == '-' || text[i - 1] == '-') {
				return false;
			}
			label_count++;
			label_start = i + 1;
		}
		return label_count >= 2;
	}

private:
	//! A token that can be part of an unquoted dotted name: a plain word or number, or a lone '.' or '-'
	static bool IsNamePart(const MatcherToken &token) {
		if (token.text.empty()) {
			return false;
		}
		switch (token.type) {
		case TokenType::KEYWORD:
		case TokenType::IDENTIFIER:
		case TokenType::NUMBER_LITERAL:
			return true;
		case TokenType::OPERATOR:
			return token.text == "." || token.text == "-";
		default:
			return false;
		}
	}
};

} // namespace duckdb
