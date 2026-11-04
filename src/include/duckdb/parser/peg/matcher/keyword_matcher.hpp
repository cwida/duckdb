#pragma once

#include "duckdb/parser/peg/matcher.hpp"
#include "duckdb/common/string.hpp"

namespace duckdb {

class KeywordMatcher : public AtomicMatcher {
public:
	static constexpr MatcherType TYPE = MatcherType::KEYWORD;

public:
	explicit KeywordMatcher(string keyword_p, const KeywordInfo &info)
	    : AtomicMatcher(TYPE), keyword(std::move(keyword_p)), info(info) {
	}
	KeywordMatcher(string keyword_p, const KeywordInfo &info, const PEGKeywordHelper &keyword_helper)
	    : KeywordMatcher(std::move(keyword_p), info) {
		literal_table = keyword_helper.GetLiteralTable();
		literal_info = literal_table->Lookup(keyword);
		if (!literal_info.LiteralId()) {
			literal_table = nullptr;
		}
	}

	MatcherResult MatchAtomic(MatchState &state) const override {
		auto token = state.token_iterator.Current();
		if (!token) {
			return MatcherResult::Failure();
		}
		auto &token_text = token->text;
		auto start_offset = optional_idx(token->offset);
		auto token_length = optional_idx(token->length);
		if (!MatchKeyword(state)) {
			return MatcherResult::Failure();
		}
		auto result = state.AllocateParseResult<KeywordParseResult>(token_text, start_offset, token_length);
		if (result.HasParseResult()) {
			result.GetParseResult()->SetNameFrom(*this);
		}
		return result;
	}

	SuggestionType AddSuggestionInternal(MatchState &state) const override {
		AutoCompleteCandidate candidate(keyword, SuggestionState::SUGGEST_KEYWORD, info.score_bonus,
		                                CandidateType::KEYWORD);
		candidate.extra_char = info.extra_char;
		state.AddSuggestion(MatcherSuggestion(std::move(candidate)));
		return SuggestionType::MANDATORY;
	}

	string ToString() const override {
		return "'" + keyword + "'";
	}

	//! Custom matchers must not be filtered using ordinary literal semantics by default.
	virtual optional_idx GetDispatchLiteral(const GrammarLiteralTable &) const {
		return optional_idx();
	}

	void InitializeFirstSet(MatcherFirstSet &first_set, const GrammarLiteralTable &table) const override {
		auto literal = GetDispatchLiteral(table);
		if (literal.IsValid()) {
			first_set.AddLiteral(literal.GetIndex());
		} else {
			first_set.any = true;
		}
	}

private:
	bool MatchKeyword(MatchState &state) const {
		auto token = state.token_iterator.Current();
		if (!token) {
			return false;
		}
		const auto matches = literal_table ? state.token_iterator.CurrentLiteralInfo(*literal_table) == literal_info
		                                   : StringUtil::CIEquals(keyword, token->text);
		if (matches) {
			// move to the next token
			state.token_iterator.Advance();
			state.UpdateMaxTokenIndex();
			return true;
		}
		return false;
	}

private:
	const string keyword;
	const KeywordInfo info;

protected:
	optional_ptr<const GrammarLiteralTable> literal_table;
	LiteralInfo literal_info;
};

} // namespace duckdb
