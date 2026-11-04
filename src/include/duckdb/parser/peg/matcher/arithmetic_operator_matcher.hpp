#pragma once

#include "duckdb/parser/peg/tokenizer/tokenizer.hpp"
#include "duckdb/parser/peg/keyword_helper.hpp"
#include "duckdb/parser/peg/matcher.hpp"

namespace duckdb {

class ArithmeticOperatorMatcher : public AtomicMatcher {
public:
	static constexpr MatcherType TYPE = MatcherType::OPERATOR;

public:
	explicit ArithmeticOperatorMatcher() : AtomicMatcher(TYPE) {
	}

	MatcherResult MatchAtomic(MatchState &state) const override {
		auto token = state.token_iterator.Current();
		if (!token) {
			return MatcherResult::Failure();
		}
		auto &token_text = token->text;
		auto start_offset = optional_idx(token->offset);
		auto token_length = optional_idx(token->length);
		if (!MatchArithmeticOperator(state)) {
			return MatcherResult::Failure();
		}
		return state.AllocateParseResult<OperatorParseResult>(token_text, start_offset, token_length);
	}

	SuggestionType AddSuggestionInternal(MatchState &state) const override {
		return SuggestionType::MANDATORY;
	}

	string ToString() const override {
		return "ARITHMETICOPERATOR";
	}

	void InitializeFirstSet(MatcherFirstSet &first_set, const GrammarLiteralTable &table) const override {
		first_set.class_mask = MatcherTokenClass::OPERATOR;
	}

private:
	bool MatchArithmeticOperator(MatchState &state) const {
		auto token = state.token_iterator.Current();
		if (!token) {
			return false;
		}
		auto &token_text = token->text;
		for (auto &c : token_text) {
			if (!IsArithmeticOperatorChar(c)) {
				return false;
			}
		}
		state.token_iterator.Advance();
		state.UpdateMaxTokenIndex();
		return true;
	}

protected:
	bool IsArithmeticOperatorChar(char c) const {
		switch (c) {
		case '+':
		case '-':
		case '*':
		case '/':
		case '%':
			return true;
		default:
			return false;
		}
	}
};

} // namespace duckdb
