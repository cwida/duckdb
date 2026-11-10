#pragma once

#include "duckdb/parser/peg/tokenizer/tokenizer.hpp"
#include "duckdb/parser/peg/keyword_helper.hpp"
#include "duckdb/parser/peg/matcher.hpp"

namespace duckdb {

class NumberLiteralMatcher : public AtomicMatcher {
public:
	static constexpr MatcherType TYPE = MatcherType::NUMBER_LITERAL;

public:
	explicit NumberLiteralMatcher() : AtomicMatcher(TYPE) {
		name = "NumberLiteral";
	}

	MatcherResult MatchAtomic(MatchState &state) const override {
		auto token = state.token_iterator.Current();
		if (!token) {
			return MatcherResult::Failure();
		}
		auto &token_text = token->text;
		auto start_offset = optional_idx(token->offset);
		auto token_length = optional_idx(token->length);
		if (!MatchNumberLiteral(state)) {
			return MatcherResult::Failure();
		}
		state.token_iterator.SetPreviousTokenType(TokenType::NUMBER_LITERAL);
		auto result = state.AllocateParseResult<NumberParseResult>(token_text, start_offset, token_length);
		if (result.HasParseResult()) {
			result.GetParseResult()->SetNameFrom(*this);
		}
		return result;
	}

	SuggestionType AddSuggestionInternal(MatchState &state) const override {
		return SuggestionType::MANDATORY;
	}

	string ToString() const override {
		return "NUMBER_LITERAL";
	}

	void InitializeFirstSet(MatcherFirstSet &first_set, const GrammarLiteralTable &table) const override {
		first_set.class_mask = MatcherTokenClass::NUMBER;
	}

private:
	static bool MatchNumberLiteral(MatchState &state) {
		auto token = state.token_iterator.Current();
		if (!token) {
			return false;
		}
		auto &token_text = token->text;
		if (token_text.empty() || !Tokenizer::CharacterIsInitialNumber(token_text[0])) {
			return false;
		}
		// A lone '.' is a dot operator, not a number literal (e.g., '?.method()' should not consume '.')
		if (token_text.size() == 1 && token_text[0] == '.') {
			return false;
		}
		bool scientific_notation = false;
		bool has_dot = token_text[0] == '.';
		for (idx_t i = 1; i < token_text.size(); i++) {
			if (token_text[i] == '.') {
				// a number has at most one dot, and none in its exponent
				if (has_dot || scientific_notation) {
					return false;
				}
				has_dot = true;
			}
			if (Tokenizer::CharacterIsScientific(token_text[i])) {
				if (scientific_notation) {
					throw ParserException("Already found scientific notation");
				}
				scientific_notation = true;
			}
			if (scientific_notation && (token_text[i] == '+' || token_text[i] == '-')) {
				continue;
			}
			if (!Tokenizer::CharacterIsNumber(token_text[i])) {
				return false;
			}
		}
		state.token_iterator.Advance();
		state.UpdateMaxTokenIndex();
		return true;
	}
};

} // namespace duckdb
