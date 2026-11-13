#pragma once

#include "duckdb/parser/peg/matcher.hpp"

namespace duckdb {

class RepeatMatcher : public Matcher {
public:
	static constexpr MatcherType TYPE = MatcherType::REPEAT;

public:
	explicit RepeatMatcher(Matcher &element_p) : Matcher(TYPE), element(element_p) {
	}

	DUCKDB_API arena_ptr<MatchProcess> StartMatch(MatchState &state) const override;

	SuggestionType AddSuggestionInternal(MatchState &state) const override {
		element.AddSuggestion(state);
		return SuggestionType::MANDATORY;
	}

	string ToString() const override {
		return element.GetPrintName() + "*";
	}
	Matcher &GetChildMatcher() {
		return element;
	}
	const Matcher &GetChildMatcher() const {
		return element;
	}

private:
	Matcher &element;
};

} // namespace duckdb
