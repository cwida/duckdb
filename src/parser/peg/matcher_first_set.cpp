#include "duckdb/parser/peg/matcher.hpp"
#include "duckdb/parser/peg/matcher/list.hpp"
#include "duckdb/common/reference_map.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// FIRST sets
//===--------------------------------------------------------------------===//
bool MatcherFirstSet::Merge(const MatcherFirstSet &other) {
	bool changed = false;
	if (other.any && !any) {
		any = changed = true;
	}
	if ((class_mask | other.class_mask) != class_mask) {
		class_mask |= other.class_mask;
		changed = true;
	}
	if (other.literals.size() > literals.size()) {
		literals.resize(other.literals.size(), 0);
	}
	for (idx_t i = 0; i < other.literals.size(); i++) {
		auto merged = literals[i] | other.literals[i];
		if (merged != literals[i]) {
			literals[i] = merged;
			changed = true;
		}
	}
	return changed;
}

void MatcherFirstSet::AddLiteral(idx_t literal_id) {
	auto word = literal_id / 64;
	if (word >= literals.size()) {
		literals.resize(word + 1, 0);
	}
	literals[word] |= uint64_t(1) << (literal_id % 64);
}

//! Calls fun(child) for every child of a built-in composite matcher
template <class FUN>
static void ForEachChild(Matcher &matcher, FUN &&fun) {
	switch (matcher.Type()) {
	case MatcherType::LIST:
		for (auto &child : matcher.Cast<ListMatcher>().matchers) {
			fun(child.get());
		}
		break;
	case MatcherType::CHOICE:
		for (auto &child : matcher.Cast<ChoiceMatcher>().matchers) {
			fun(child.get());
		}
		break;
	case MatcherType::OPTIONAL:
		fun(matcher.Cast<OptionalMatcher>().GetChildMatcher());
		break;
	case MatcherType::REPEAT:
		fun(matcher.Cast<RepeatMatcher>().GetChildMatcher());
		break;
	default:
		break;
	}
}

//! Derived matchers can override StartMatch, so only matchers marked as structural follow the semantics of their type
static bool IsComposite(const Matcher &matcher) {
	if (!matcher.IsStructural()) {
		return false;
	}
	auto type = matcher.Type();
	return type == MatcherType::LIST || type == MatcherType::CHOICE || type == MatcherType::OPTIONAL ||
	       type == MatcherType::REPEAT;
}

//! Recomputes the FIRST set of a composite matcher from its children, returns whether anything changed
static bool UpdateFirstSet(Matcher &matcher) {
	auto &first = matcher.first_set;
	bool changed = false;
	bool nullable;
	switch (matcher.Type()) {
	case MatcherType::LIST:
		// the first non-nullable element ends the FIRST set of a sequence
		nullable = true;
		for (auto &child : matcher.Cast<ListMatcher>().matchers) {
			auto &child_first = child.get().first_set;
			changed |= first.Merge(child_first);
			if (!child_first.nullable) {
				nullable = false;
				break;
			}
		}
		break;
	case MatcherType::CHOICE:
		nullable = false;
		for (auto &child : matcher.Cast<ChoiceMatcher>().matchers) {
			auto &child_first = child.get().first_set;
			changed |= first.Merge(child_first);
			nullable = nullable || child_first.nullable;
		}
		break;
	case MatcherType::OPTIONAL:
		changed |= first.Merge(matcher.Cast<OptionalMatcher>().GetChildMatcher().first_set);
		nullable = true;
		break;
	case MatcherType::REPEAT: {
		auto &child_first = matcher.Cast<RepeatMatcher>().GetChildMatcher().first_set;
		changed |= first.Merge(child_first);
		nullable = child_first.nullable;
		break;
	}
	default:
		throw InternalException("UpdateFirstSet called on a matcher that is not composite");
	}
	if (nullable && !first.nullable) {
		first.nullable = true;
		changed = true;
	}
	return changed;
}

struct FirstSetTraversalEntry {
	explicit FirstSetTraversalEntry(Matcher &matcher) : matcher(matcher) {
	}

	reference<Matcher> matcher;
	bool expanded = false;
};

void ComputeFirstSets(Matcher &root, const GrammarLiteralTable &table) {
	// collect the reachable matchers, children before their parents (except for recursive references)
	vector<reference<Matcher>> matchers;
	vector<reference<Matcher>> composites;
	reference_set_t<Matcher> seen;
	vector<FirstSetTraversalEntry> pending;
	pending.emplace_back(root);
	seen.insert(root);
	while (!pending.empty()) {
		auto &entry = pending.back();
		auto &matcher = entry.matcher.get();
		if (entry.expanded) {
			pending.pop_back();
			matchers.push_back(matcher);
			continue;
		}
		entry.expanded = true;
		// entry is invalidated by the pushes below
		ForEachChild(matcher, [&](Matcher &child) {
			if (seen.insert(child).second) {
				pending.emplace_back(child);
			}
		});
	}
	for (auto &entry : matchers) {
		auto &matcher = entry.get();
		auto &first = matcher.first_set;
		first = MatcherFirstSet();
		first.table = &table;
		if (IsComposite(matcher)) {
			composites.push_back(matcher);
		} else if (matcher.IsAtomic()) {
			static_cast<const AtomicMatcher &>(matcher).InitializeFirstSet(first, table);
		} else {
			// custom and derived matchers: no information
			first.any = true;
		}
	}
	// composite matchers: least fixed point, the grammar is recursive
	// children are visited before their parents, so only recursive references need another pass
	bool changed = true;
	while (changed) {
		changed = false;
		for (auto &matcher : composites) {
			changed |= UpdateFirstSet(matcher.get());
		}
	}
	for (auto &entry : matchers) {
		entry.get().first_set.computed = true;
	}
}

bool MatcherFirstSet::MightMatch(MatchState &state) const {
	if (!computed || any || nullable) {
		return true;
	}
	auto token = state.token_iterator.Current();
	if (!token) {
		return false;
	}
	if (token->token_class & class_mask) {
		return true;
	}
	auto literal_id = state.token_iterator.CurrentLiteralInfo(*table).LiteralId();
	return literal_id && HasLiteral(literal_id);
}

} // namespace duckdb
