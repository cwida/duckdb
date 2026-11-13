#pragma once

#include "duckdb/parser/peg/tokenizer/tokenizer.hpp"
#include "duckdb/parser/peg/keyword_helper.hpp"
#include "duckdb/parser/peg/matcher.hpp"

namespace duckdb {

enum class OperatorMatcherMode : uint8_t { GENERIC_PRECEDENCE, ALL_OPERATORS };

class OperatorMatcher : public AtomicMatcher {
public:
	static constexpr MatcherType TYPE = MatcherType::OPERATOR;

public:
	explicit OperatorMatcher(OperatorMatcherMode mode_p = OperatorMatcherMode::GENERIC_PRECEDENCE)
	    : AtomicMatcher(TYPE), mode(mode_p) {
	}

	MatcherResult MatchAtomic(MatchState &state) const override {
		auto token = state.token_iterator.Current();
		if (!token) {
			return MatcherResult::Failure();
		}
		auto &token_text = token->text;
		auto start_offset = optional_idx(token->offset);
		auto token_length = optional_idx(token->length);
		if (!MatchOperator(state)) {
			return MatcherResult::Failure();
		}
		return state.AllocateParseResult<OperatorParseResult>(token_text, start_offset, token_length);
	}

	SuggestionType AddSuggestionInternal(MatchState &state) const override {
		return SuggestionType::MANDATORY;
	}

	string ToString() const override {
		return "OPERATOR";
	}

	void InitializeFirstSet(MatcherFirstSet &first_set, const GrammarLiteralTable &table) const override {
		first_set.class_mask = mode == OperatorMatcherMode::GENERIC_PRECEDENCE ? MatcherTokenClass::GENERIC_OPERATOR
		                                                                       : MatcherTokenClass::OPERATOR;
	}

private:
	bool MatchOperator(MatchState &state) const {
		auto token = state.token_iterator.Current();
		if (!token) {
			return false;
		}
		auto &token_text = token->text;
		if (mode == OperatorMatcherMode::GENERIC_PRECEDENCE && HasSpecialPrecedence(token_text)) {
			return false;
		}
		for (auto &c : token_text) {
			if (!Tokenizer::CharacterIsOperator(c)) {
				return false;
			}
		}
		state.token_iterator.Advance();
		state.UpdateMaxTokenIndex();
		return true;
	}

public:
	DUCKDB_API static bool HasSpecialPrecedence(const string &operator_name);

private:
	OperatorMatcherMode mode;
};

} // namespace duckdb
