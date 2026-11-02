#include "duckdb/parser/peg/matcher.hpp"
#include "duckdb/parser/peg/matcher_stack.hpp"
#include "duckdb/parser/peg/compiled_grammar.hpp"
#include "duckdb/parser/peg/matcher_factory.hpp"
#include "duckdb/parser/peg/matcher/list_matcher.hpp"
#include "duckdb/parser/peg/matcher/optional_matcher.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"

#include "duckdb/common/printer.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/common/string_map_set.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/parser/peg/keyword_helper.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/exception/parser_exception.hpp"
#include "duckdb/parser/peg/tokenizer/tokenizer.hpp"
#include "duckdb/parser/peg/peg_parser.hpp"
#include "duckdb/parser/peg/transformer/parse_result.hpp"

namespace duckdb {

MatcherResult Matcher::MatchParseResult(MatchState &state) const {
	MatchInput input {*this, state};
	MatchStack stack;
	return stack.Execute(input);
}

SuggestionType Matcher::AddSuggestion(MatchState &state) const {
	auto &context = state.context;
	if (context.memoizable_suggestions) {
		// the walk does not depend on the position, so each matcher needs to be walked only once
		auto key = std::make_pair(this, context.active_label.get());
		auto entry = context.walked_suggestions.find(key);
		if (entry != context.walked_suggestions.end()) {
			return entry->second;
		}
		auto result = AddLabelledSuggestion(state);
		context.walked_suggestions.emplace(key, result);
		return result;
	}
	if (!state.added_suggestions) {
		state.added_suggestions = make_uniq<reference_set_t<const Matcher>>();
	}
	auto &added_suggestions = *state.added_suggestions;
	auto entry = added_suggestions.find(*this);
	if (entry != added_suggestions.end()) {
		return SuggestionType::MANDATORY;
	}
	added_suggestions.insert(*this);
	return AddLabelledSuggestion(state);
}

SuggestionType Matcher::AddLabelledSuggestion(MatchState &state) const {
	auto &context = state.context;
	if (label.empty() || context.active_label) {
		return AddSuggestionInternal(state);
	}
	context.active_label = label;
	if (context.memoizable_suggestions) {
		// only the label is reported, so the rule is not walked; labelled rules never match empty input
		state.AddSuggestion(MatcherSuggestion(SuggestionState::SUGGEST_VARIABLE));
		context.active_label = nullptr;
		return SuggestionType::MANDATORY;
	}
	// suggestions added inside a labelled rule are summarised by its label
	auto result = AddSuggestionInternal(state);
	context.active_label = nullptr;
	return result;
}

string Matcher::GetPrintName() const {
	if (name.empty()) {
		return ToString();
	}
	return name;
}

void Matcher::Print() const {
	Printer::Print(ToString());
}

void MatchState::AddSuggestion(MatcherSuggestion suggestion) {
	suggestion.label = context.active_label;
	context.suggestions.push_back(std::move(suggestion));
}

MatcherAllocator::MatcherAllocator() = default;
MatcherAllocator::MatcherAllocator(MatcherAllocator &&) noexcept = default;
MatcherAllocator &MatcherAllocator::operator=(MatcherAllocator &&) noexcept = default;
MatcherAllocator::~MatcherAllocator() = default;

Matcher &MatcherAllocator::Allocate(unique_ptr<Matcher> matcher) {
	auto &result = *matcher;
	result.packrat_id = optional_idx(matchers.size());
	matchers.push_back(std::move(matcher));
	return result;
}

static bool CanGroupList(const ListMatcher &list) {
	if (!list.IsCollapsible() || list.suppress_suggestions || list.matchers.empty() || !list.first_set.computed ||
	    list.first_set.nullable || !list.first_set.table) {
		return false;
	}

	const auto chain_child = list.GetChainChild();
	const idx_t child_index = chain_child ? list.GetChainChildIndex() : 0;
	for (idx_t i = 0; i < list.matchers.size(); i++) {
		if (i == child_index) {
			continue;
		}
		auto &child = list.matchers[i].get();
		if (!child.IsStructural() || child.Type() != MatcherType::OPTIONAL) {
			return false;
		}
		auto &first = child.Cast<OptionalMatcher>().GetChildMatcher().first_set;
		// Class predicates can make a group too broad. Keep this first version literal-only.
		if (!first.computed || first.any || first.nullable || first.class_mask || first.table != list.first_set.table) {
			return false;
		}
	}
	return true;
}

static bool SameFirstPredicate(const MatcherFirstSet &left, const MatcherFirstSet &right) {
	return left.computed && right.computed && left.any == right.any && left.nullable == right.nullable &&
	       left.class_mask == right.class_mask && left.table == right.table && left.literals == right.literals;
}

void MatcherAllocator::InitializeListChains() {
	// Clear borrowed links before releasing their owners or making any allocation that can throw.
	for (auto &matcher : matchers) {
		if (matcher->IsStructural() && matcher->Type() == MatcherType::LIST) {
			matcher->Cast<ListMatcher>().InitializeListChain();
		}
	}
	list_chains.clear();
	vector<bool> eligible(matchers.size(), false);
	for (idx_t i = 0; i < matchers.size(); i++) {
		auto &matcher = *matchers[i];
		if (matcher.IsStructural() && matcher.Type() == MatcherType::LIST) {
			auto &list = matcher.Cast<ListMatcher>();
			eligible[i] = CanGroupList(list);
		}
	}
	vector<optional_idx> next(matchers.size());
	vector<idx_t> parents(matchers.size(), 0);
	for (idx_t i = 0; i < matchers.size(); i++) {
		if (!eligible[i]) {
			continue;
		}
		auto &list = matchers[i]->Cast<ListMatcher>();
		auto child = list.GetChainChild();
		if (!child || !child->GetPackratId().IsValid()) {
			continue;
		}
		auto child_id = child->GetPackratId().GetIndex();
		if (child_id >= matchers.size() || matchers[child_id].get() != child.get() || !eligible[child_id] ||
		    (list.GetChainChildIndex() == 0 && !SameFirstPredicate(list.first_set, child->first_set))) {
			continue;
		}
		next[i] = optional_idx(child_id);
		parents[child_id]++;
	}
	for (idx_t i = 0; i < matchers.size(); i++) {
		if (!eligible[i] || parents[i] == 1) {
			continue;
		}
		vector<idx_t> members {i};
		auto current = i;
		while (next[current].IsValid() && parents[next[current].GetIndex()] == 1) {
			current = next[current].GetIndex();
			members.push_back(current);
		}
		if (members.size() < 2) {
			continue;
		}
		auto chain = make_uniq<ListMatchChain>();
		chain->tail_first_set.computed = true;
		chain->tail_first_set.table = matchers[i]->first_set.table;
		chain->prefix_first_set.computed = true;
		chain->prefix_first_set.table = matchers[i]->first_set.table;
		for (auto member : members) {
			auto &list = matchers[member]->Cast<ListMatcher>();
			chain->lists.push_back(list);

			const auto chain_child = list.GetChainChild();
			const idx_t child_index = chain_child ? list.GetChainChildIndex() : 0;
			for (idx_t child = 0; child < list.matchers.size(); child++) {
				if (child == child_index) {
					continue;
				}
				auto &first = list.matchers[child].get().Cast<OptionalMatcher>().GetChildMatcher().first_set;
				if (child < child_index) {
					chain->has_optional_prefix = true;
					chain->prefix_first_set.Merge(first);
				} else {
					chain->tail_first_set.Merge(first);
				}
			}
		}
		// Publish the owner before installing borrowed pointers: vector growth can throw.
		auto chain_ptr = chain.get();
		list_chains.push_back(std::move(chain));
		for (idx_t index = 0; index < members.size(); index++) {
			auto &list = matchers[members[index]]->Cast<ListMatcher>();
			list.prepared_chain = chain_ptr;
			list.prepared_chain_index = index;
		}
	}
}

ParseResultAllocator::ParseResultAllocator() : arena(Allocator::DefaultAllocator()), pending_destructors(arena) {
}

ParseResultAllocator::~ParseResultAllocator() = default;

} // namespace duckdb
