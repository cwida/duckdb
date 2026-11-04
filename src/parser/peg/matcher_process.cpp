#include "duckdb/parser/peg/matcher.hpp"
#include "duckdb/parser/peg/matcher/list_matcher.hpp"
#include "duckdb/parser/peg/matcher/choice_matcher.hpp"
#include "duckdb/parser/peg/matcher/optional_matcher.hpp"

namespace duckdb {

MatchStep MatchStep::Child(MatchInput input) {
	return MatchStep(input, nullopt);
}

MatchStep MatchStep::Complete(MatcherResult result) {
	return MatchStep(nullopt, result);
}

optional<MatchInput> MatchStep::GetChild() {
	return child;
}

MatcherResult MatchStep::GetResult() const {
	D_ASSERT(!child);
	if (!result) {
		throw InternalException("Completed match step has no result");
	}
	return *result;
}

class AtomicMatchProcess : public MatchProcess {
public:
	AtomicMatchProcess(const AtomicMatcher &matcher_p, MatchState &state_p) : matcher(matcher_p), state(state_p) {
	}

	MatchStep Resume(optional<MatcherResult> child_result) override {
		D_ASSERT(!child_result);
		D_ASSERT(!completed);
		completed = true;
		return MatchStep::Complete(matcher.MatchAtomic(state));
	}

private:
	const AtomicMatcher &matcher;
	MatchState &state;
	bool completed = false;
};

arena_ptr<MatchProcess> AtomicMatcher::StartMatch(MatchState &state) const {
	return state.Make<AtomicMatchProcess>(*this, state);
}

void ListMatcher::InitializeListChain() {
	chain_child_index = optional_idx();
	first_choice = nullptr;
	prepared_chain = nullptr;
	prepared_chain_index = 0;
	if (!IsStructural() || suppress_suggestions || matchers.empty()) {
		return;
	}

	auto &first = matchers[0].get();
	if (first.IsStructural() && first.Type() == MatcherType::CHOICE && !first.IsPackratMemoized() &&
	    first.Cast<ChoiceMatcher>().PreservesSelectionState()) {
		first_choice = first.Cast<ChoiceMatcher>();
	}

	idx_t child_index = 0;
	if (IsCollapsible() && matchers.size() == 2 && first.IsStructural() && first.Type() == MatcherType::OPTIONAL) {
		// The prefix can be omitted only when its FIRST predicate proves it absent at execution time.
		// A named final list always supplies the nonempty result required by the collapse contract.
		if (!matchers[1].get().GetRule()) {
			return;
		}
		child_index = 1;
	}
	auto &child = matchers[child_index].get();
	if (!child.IsStructural() || child.Type() != MatcherType::LIST || child.IsPackratMemoized() ||
	    child.Cast<ListMatcher>().suppress_suggestions) {
		return;
	}
	chain_child_index = optional_idx(child_index);
}

} // namespace duckdb
