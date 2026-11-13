#pragma once

#include "duckdb/parser/peg/matcher.hpp"

namespace duckdb {
class ListMatcher;
class ChoiceMatcher;

//! A prepared, uniquely linked group. Members and FIRST data belong to the same matcher allocator.
struct ListMatchChain {
	vector<reference<const ListMatcher>> lists;
	MatcherFirstSet tail_first_set;
	MatcherFirstSet prefix_first_set;
	bool has_optional_prefix = false;
};

class ListMatcher : public Matcher {
public:
	static constexpr MatcherType TYPE = MatcherType::LIST;

public:
	ListMatcher() : Matcher(TYPE) {
	}
	explicit ListMatcher(vector<reference<Matcher>> matchers_p) : Matcher(TYPE), matchers(std::move(matchers_p)) {
	}

	DUCKDB_API arena_ptr<MatchProcess> StartMatch(MatchState &state) const override;
	//! Prepare after matcher links, flags and FIRST sets are final; unprepared lists use ordinary execution.
	DUCKDB_API void InitializeListChain();

	optional_ptr<const ListMatcher> GetChainChild() const {
		if (!chain_child_index.IsValid()) {
			return nullptr;
		}
		D_ASSERT(IsStructural() && !suppress_suggestions && !matchers.empty());
		auto &child = matchers[chain_child_index.GetIndex()].get();
		D_ASSERT(child.IsStructural() && child.Type() == MatcherType::LIST && !child.IsPackratMemoized());
		auto &list = static_cast<const ListMatcher &>(child);
		D_ASSERT(!list.suppress_suggestions);
		return list;
	}

	idx_t GetChainChildIndex() const {
		return chain_child_index.GetIndex();
	}

	optional_ptr<const ChoiceMatcher> GetFirstChoice() const {
		return first_choice;
	}

	optional_ptr<const ListMatchChain> GetPreparedChain() const {
		return prepared_chain;
	}
	idx_t GetPreparedChainIndex() const {
		return prepared_chain_index;
	}

	SuggestionType AddSuggestionInternal(MatchState &state) const override {
		if (suppress_suggestions) {
			return SuggestionType::OPTIONAL;
		}
		for (auto &matcher : matchers) {
			auto suggestion_result = matcher.get().AddSuggestion(state);
			if (suggestion_result == SuggestionType::MANDATORY) {
				// we must match this suggestion before continuing
				return SuggestionType::MANDATORY;
			}
		}
		// all child suggestions were optional - the entire list is optional
		return SuggestionType::OPTIONAL;
	}

	string ToString() const override {
		string result = "";
		for (auto &matcher : matchers) {
			if (!result.empty()) {
				result += " ";
			}
			result += matcher.get().GetPrintName();
		}
		return "(" + result + ")";
	}

public:
	vector<reference<Matcher>> matchers;
	//! If true, this matcher will not contribute autocomplete suggestions (used for rules like ExpressionStatement)
	bool suppress_suggestions = false;

private:
	friend class MatcherAllocator;
	optional_idx chain_child_index;
	optional_ptr<const ChoiceMatcher> first_choice;
	optional_ptr<const ListMatchChain> prepared_chain;
	idx_t prepared_chain_index = 0;
};

} // namespace duckdb
