#pragma once

#include "duckdb/parser/peg/matcher.hpp"
#include "duckdb/common/string.hpp"

namespace duckdb {

class ChoiceMatcher : public Matcher {
public:
	static constexpr MatcherType TYPE = MatcherType::CHOICE;

public:
	ChoiceMatcher() : Matcher(TYPE) {
	}
	explicit ChoiceMatcher(vector<reference<Matcher>> &&matchers_p) : Matcher(TYPE), matchers(std::move(matchers_p)) {
	}

	DUCKDB_API arena_ptr<MatchProcess> StartMatch(MatchState &state) const override;
	//! Restrict matching to one child; matchers.size() means none, invalid means ordered choice.
	virtual optional_idx SelectChild(MatchState &) const {
		return optional_idx();
	}

	//! SelectChild preserves the cursor, rule and private suggestion set.
	void SetSelectionStatePreserving() {
		selection_state_preserving = true;
	}
	bool PreservesSelectionState() const {
		return selection_state_preserving;
	}

	SuggestionType AddSuggestionInternal(MatchState &state) const override {
		for (auto &child_matcher : matchers) {
			child_matcher.get().AddSuggestion(state);
		}
		return SuggestionType::MANDATORY;
	}

	string ToString() const override {
		string result = "";
		for (auto &matcher : matchers) {
			if (!result.empty()) {
				result += " / ";
			}
			result += matcher.get().GetPrintName();
		}
		return result;
	}

public:
	vector<reference<Matcher>> matchers;

private:
	bool selection_state_preserving = false;
};

} // namespace duckdb
