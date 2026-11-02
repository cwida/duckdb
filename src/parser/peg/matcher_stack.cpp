#include "duckdb/parser/peg/matcher_stack.hpp"
#include "duckdb/parser/peg/matcher/list_matcher.hpp"
#include "duckdb/parser/peg/matcher/choice_matcher.hpp"
#include "duckdb/parser/peg/matcher/optional_matcher.hpp"
#include "duckdb/parser/peg/matcher/repeat_matcher.hpp"

namespace duckdb {

struct PackratMatchState {
	static bool IsEnabled(const Matcher &matcher, const MatchState &state) {
		return state.context.packrat_cache && matcher.IsPackratMemoized() && matcher.GetPackratId().IsValid();
	}

	optional<MatcherResult> TryLoadCachedResult(const Matcher &matcher, MatchState &state);
	void StoreResult(const Matcher &matcher, MatchState &state, const MatcherResult &result) const;

private:
	optional_idx token_index_before;
	idx_t max_token_index_before = 0;
};

struct MatchStackFrame : public MatchProcess {
	MatchStackFrame(MatcherType kind_p, MatchInput input, ArenaAllocator::Position mark_p,
	                const PackratMatchState &packrat_p)
	    : kind(kind_p), input_matcher(input.matcher), state(input.state), mark(mark_p), packrat(packrat_p) {
	}
	MatchStep Resume(optional<MatcherResult> child_result) final;
	MatcherType kind;
	const Matcher &input_matcher;
	MatchState &state;
	ArenaAllocator::Position mark;
	PackratMatchState packrat;
};
namespace {

//! What the driver does after stepping a list frame
enum class ListStep : uint8_t {
	//! a child to match is in `next`
	DISPATCH_CHILD,
	//! the frame switched to another list (a choice region entered or resumed): step it again
	RESUME,
	//! the leading choice produced the first child result: continue with the children
	CHILDREN,
	//! every child of the current member matched
	MEMBER_MATCHED,
	//! `completed` holds the list's result
	COMPLETE
};

static idx_t FindChoiceChild(const ChoiceMatcher &matcher, MatchState &state, idx_t index, bool single_child) {
	if (!single_child && state.BuildParseResult()) {
		while (index < matcher.matchers.size()) {
			auto &child = matcher.matchers[index].get();
			if (!child.IsStructural() || child.first_set.MightMatch(state)) {
				break;
			}
			index++;
		}
	}
	return index;
}

static MatcherResult MakeChoiceResult(MatchState &state, const MatcherResult &child, idx_t index,
                                      optional_idx start_offset) {
	if (!child.HasParseResult()) {
		return MatcherResult::Success();
	}
	return state.AllocateParseResult<ChoiceParseResult>(*child.GetParseResult(), index, start_offset);
}

class ListMatchFrame final : public MatchStackFrame {
public:
	struct PendingList {
		PendingList(const ListMatcher &matcher, optional_ptr<PendingList> parent) : matcher(matcher), parent(parent) {
		}

		const ListMatcher &matcher;
		optional_ptr<PendingList> parent;
	};

	struct PendingChoice {
		PendingChoice(const ListMatcher &matcher, optional_ptr<PendingList> lists, optional_ptr<PendingChoice> parent,
		              idx_t index, bool single, ArenaAllocator::Position mark)
		    : matcher(matcher), lists(lists), parent(parent), index(index), single(single), mark(mark) {
		}

		const ListMatcher &matcher;
		optional_ptr<PendingList> lists;
		optional_ptr<PendingChoice> parent;
		idx_t index;
		bool single;
		ArenaAllocator::Position mark;
	};

public:
	ListMatchFrame(const ListMatcher &matcher_p, MatchState &state_p, ArenaAllocator::Position mark = {},
	               const PackratMatchState &packrat = {}, bool allow_empty_prefix = false)
	    : MatchStackFrame(MatcherType::LIST, {matcher_p, state_p}, mark, packrat), matcher(matcher_p),
	      list_state(state_p) {
		saved_suggestion_size = SuppressSuggestions(matcher_p) ? list_state.context.suggestions.size() : 0;
		auto current = list_state.token_iterator.Current();
		if (current) {
			start_offset = optional_idx(current->offset);
		}
		InitializeListChain(allow_empty_prefix);
	}

public:
	void InitializeListChain(bool allow_empty_prefix) {
		auto current = list_state.token_iterator.Current();
		if (state.BuildParseResult() && (!current || current->type != TokenType::END_OF_INPUT_AUTOCOMPLETE)) {
			while (true) {
				auto chain = matcher.get().GetPreparedChain();
				if (chain && matcher.get().GetPreparedChainIndex() + 1 < chain->lists.size() &&
				    (!chain->has_optional_prefix ||
				     (allow_empty_prefix && !chain->prefix_first_set.MightMatch(list_state)))) {
					auto &last = chain->lists.back().get();
					if (!last.first_set.MightMatch(list_state)) {
						break;
					}
					pending = state.context.process_allocator.Make<PendingList>(matcher.get(), pending);
					matcher = last;
					continue;
				}

				auto child = matcher.get().GetChainChild();
				if (child && matcher.get().GetChainChildIndex() != 0 &&
				    (!allow_empty_prefix ||
				     matcher.get().matchers[0].get().Cast<OptionalMatcher>().GetChildMatcher().first_set.MightMatch(
				         list_state))) {
					break;
				}
				if (!child || !child->first_set.MightMatch(list_state)) {
					break;
				}
				pending = state.context.process_allocator.Make<PendingList>(matcher.get(), pending);
				matcher = *child;
			}
		}
	}

	bool HasPendingParent() const {
		return pending || choice_regions;
	}

	void EnterChoiceList(const ListMatcher &child) {
		D_ASSERT(first_choice && !child.IsPackratMemoized() && !child.suppress_suggestions);
		D_ASSERT(child_index == 0 && result_count == 0 && !results);
		D_ASSERT(list_state.token_iterator.Position() == state.token_iterator.Position());
		auto &arena = state.context.process_allocator;
		auto mark = arena.GetPosition();
		choice_regions =
		    arena.Make<PendingChoice>(matcher.get(), pending, choice_regions, choice_index, single_choice, mark);
		matcher = child;
		pending = nullptr;
		first_choice = nullptr;
		awaiting_child = false;
		list_state.added_suggestions.reset();
		list_state.rule = child.GetRule();
		InitializeListChain(true);
		InitializeFirstChoice();
	}

	void ResumeChoiceList() {
		D_ASSERT(choice_regions);
		// All ancestors entered before consuming their first child, so they share the frame's entry cursor.
		auto region = *choice_regions;
		matcher = region.matcher;
		pending = region.lists;
		choice_regions = region.parent;
		first_choice = matcher.get().GetFirstChoice();
		choice_index = region.index;
		single_choice = region.single;
		results = nullptr;
		result_count = 0;
		child_index = 0;
		awaiting_child = true;
		list_state.added_suggestions.reset();
		state.context.process_allocator.Rewind(region.mark);
	}

	void InitializeFirstChoice() {
		auto choice = matcher.get().GetFirstChoice();
		auto current = list_state.token_iterator.Current();
		if (!choice || !state.BuildParseResult() ||
		    (current && current->type == TokenType::END_OF_INPUT_AUTOCOMPLETE) ||
		    !choice->first_set.MightMatch(list_state)) {
			return;
		}
		first_choice = choice;
		list_state.rule = choice->GetRule();
		auto selected = choice->SelectChild(list_state);
		single_choice = selected.IsValid();
		choice_index = single_choice ? selected.GetIndex() : 0;
	}

	static bool IsEmptyOptional(ParseResult &result) {
		return result.type == ParseResultType::OPTIONAL && !result.Cast<OptionalParseResult>().HasResult();
	}

	//! Return the first child directly when either a whole prepared group or this list has no remaining value.
	bool TryCollapseEmptyTail(ParseResult &first_result) {
		if (!matcher.get().IsCollapsible() || !state.BuildParseResult() || !first_result.GetRule() ||
		    IsEmptyOptional(first_result)) {
			return false;
		}
		auto current = list_state.token_iterator.Current();
		if (current && current->type == TokenType::END_OF_INPUT_AUTOCOMPLETE) {
			return false;
		}

		// only the chain child's result can stand in for the rule: a present prefix's result never does
		if (matcher.get().GetChainChild() && child_index != matcher.get().GetChainChildIndex()) {
			return false;
		}
		if (child_index != 0) {
			// Resuming a final child after its known-empty optional prefix. No prefix result was materialized.
			D_ASSERT(result_count == 0);
			return child_index + 1 == matcher.get().matchers.size();
		}
		auto chain = matcher.get().GetPreparedChain();
		if (chain && pending && pending->matcher.GetPreparedChain() == chain &&
		    (chain->tail_first_set.literals.empty() || !chain->tail_first_set.MightMatch(list_state))) {
			D_ASSERT(matcher.get().GetPreparedChainIndex() > pending->matcher.GetPreparedChainIndex());
			matcher = pending->matcher;
			pending = pending->parent;
			list_state.added_suggestions.reset();
			return true;
		}
		if (matcher.get().matchers.size() <= 1) {
			return false;
		}
		for (idx_t i = 1; i < matcher.get().matchers.size(); i++) {
			auto &child = matcher.get().matchers[i].get();
			if (child.Type() != MatcherType::OPTIONAL || !child.IsStructural() ||
			    child.Cast<OptionalMatcher>().GetChildMatcher().first_set.MightMatch(list_state)) {
				return false;
			}
		}
		return true;
	}

	//! Steps the leading choice: its alternative's result becomes the first child result
	ListStep StepFirstChoice(bool single_step, optional<MatcherResult> &child_result, optional<MatchInput> &next,
	                         MatcherResult &completed) {
		if (child_result) {
			awaiting_child = false;
			// Restore the choice's entry state after destroying the alternative's private state.
			list_state.added_suggestions.reset();
			list_state.rule = first_choice->GetRule();
			if (child_result->IsSuccess()) {
				child_result = MakeChoiceResult(list_state, *child_result, choice_index, start_offset);
				first_choice = nullptr;
				return ListStep::CHILDREN;
			}
			if (single_choice) {
				DiscardSuggestions();
				completed = MatcherResult::Failure();
				return ListStep::COMPLETE;
			}
			choice_index++;
			list_state.token_iterator.SetPosition(state.token_iterator);
			child_result.reset();
		}
		choice_index = FindChoiceChild(*first_choice, list_state, choice_index, single_choice);
		if (choice_index >= first_choice->matchers.size()) {
			DiscardSuggestions();
			completed = MatcherResult::Failure();
			return ListStep::COMPLETE;
		}
		auto &alternative = first_choice->matchers[choice_index].get();
		if (!single_step && alternative.IsStructural() && alternative.Type() == MatcherType::LIST &&
		    !alternative.IsPackratMemoized() && !alternative.Cast<ListMatcher>().suppress_suggestions &&
		    (!single_choice || alternative.first_set.MightMatch(list_state))) {
			EnterChoiceList(alternative.Cast<ListMatcher>());
			child_result.reset();
			return ListStep::RESUME;
		}
		awaiting_child = true;
		next.emplace(MatchInput {alternative, list_state});
		return ListStep::DISPATCH_CHILD;
	}

	//! Dispatches the next child of the current member; at the autocomplete cursor the member ends instead
	ListStep StepNextChild(optional<MatchInput> &next, MatcherResult &completed) {
		auto &current_matcher = matcher.get();
		while (child_index < current_matcher.matchers.size()) {
			auto &child_matcher = current_matcher.matchers[child_index].get();
			auto current = list_state.token_iterator.Current();
			bool at_autocomplete_cursor = current && current->type == TokenType::END_OF_INPUT_AUTOCOMPLETE;
			if (!at_autocomplete_cursor) {
				awaiting_child = true;
				next.emplace(MatchInput {child_matcher, list_state});
				return ListStep::DISPATCH_CHILD;
			}
			if (SuppressSuggestions(current_matcher)) {
				DiscardSuggestions();
				completed = MatcherResult::Failure();
				return ListStep::COMPLETE;
			}
			if (child_matcher.AddSuggestion(list_state) == SuggestionType::OPTIONAL) {
				child_index++;
				continue;
			}
			if (!HasPendingParent()) {
				state.token_iterator.SetPosition(list_state.token_iterator);
			}
			completed = MatcherResult::Failure();
			return ListStep::COMPLETE;
		}
		return ListStep::MEMBER_MATCHED;
	}

	//! Steps the children of the current member once: dispatches a child or completes the member
	ListStep StepMember(optional<MatcherResult> &child_result, optional<MatchInput> &next, MatcherResult &completed) {
		// a collapsed group tail moves the frame to the group's root first
		auto collapsed_tail = child_result && child_result->IsSuccess() && child_result->HasParseResult() &&
		                      result_count == 0 && TryCollapseEmptyTail(*child_result->GetParseResult());
		if (collapsed_tail) {
			D_ASSERT(result_count == 0);
			awaiting_child = false;
			if (!HasPendingParent()) {
				state.token_iterator.SetPosition(list_state.token_iterator);
			}
			DiscardSuggestions();
			child_result->GetParseResult()->collapsed = true;
			completed = *child_result;
			return ListStep::MEMBER_MATCHED;
		}
		if (child_result) {
			awaiting_child = false;
			if (!child_result->IsSuccess()) {
				DiscardSuggestions();
				completed = MatcherResult::Failure();
				return ListStep::COMPLETE;
			}
			if (child_result->HasParseResult()) {
				AppendResult(*child_result->GetParseResult());
			}
			child_index++;
		}
		auto step = StepNextChild(next, completed);
		if (step != ListStep::MEMBER_MATCHED) {
			return step;
		}
		if (!HasPendingParent()) {
			state.token_iterator.SetPosition(list_state.token_iterator);
		}
		completed = CompleteList();
		return ListStep::MEMBER_MATCHED;
	}

	//! A member completed: continue with the next member up, or with the group's root
	void ContinueWithPendingMember() {
		auto &current_matcher = matcher.get();
		list_state.rule = current_matcher.GetRule();
		list_state.added_suggestions.reset();
		auto chain = current_matcher.GetPreparedChain();
		if (chain && pending->matcher.GetPreparedChain() == chain) {
			D_ASSERT(current_matcher.GetPreparedChainIndex() > pending->matcher.GetPreparedChainIndex());
			matcher = chain->lists[current_matcher.GetPreparedChainIndex() - 1];
		} else {
			matcher = pending->matcher;
		}
		if (&matcher.get() == &pending->matcher) {
			pending = pending->parent;
		}
		// Completed inner lists retain their spans; ancestors need independent storage.
		results = nullptr;
		result_count = 0;
		child_index = matcher.get().GetChainChildIndex();
	}

	//! Steps the children of the current member and feeds completed members to their pending ancestors
	ListStep StepChildren(optional<MatcherResult> &child_result, optional<MatchInput> &next, MatcherResult &completed) {
		while (true) {
			auto step = StepMember(child_result, next, completed);
			if (step != ListStep::MEMBER_MATCHED) {
				return step;
			}
			if (!pending) {
				return ListStep::COMPLETE;
			}
			ContinueWithPendingMember();
			child_result = completed;
		}
	}

	void AppendResult(ParseResult &result) {
		if (!results) {
			results = state.context.allocator.AllocateChildren(matcher.get().matchers.size()).data();
		}
		D_ASSERT(result_count < matcher.get().matchers.size());
		new (results.get() + result_count) reference<ParseResult>(result);
		result_count++;
	}

	MatcherResult CompleteList() {
		auto &current_matcher = matcher.get();
		DiscardSuggestions();
		if (current_matcher.IsCollapsible()) {
			auto collapsible = FindCollapsibleResult();
			if (collapsible) {
				collapsible->collapsed = true;
				return MatcherResult::Success(collapsible);
			}
		}
		auto &result_state = HasPendingParent() ? list_state : state;
		if (HasPendingParent()) {
			result_state.rule = current_matcher.GetRule();
		}
		auto named_matcher = current_matcher.HasName() ? &current_matcher : nullptr;
		if (!results) {
			results = state.context.allocator.AllocateChildren(0).data();
		}
		return result_state.AllocateParseResult<ListParseResult>(
		    unsafe_array_ptr<reference<ParseResult>>(results.get(), result_count), named_matcher, start_offset);
	}

	//! The child that can stand in for this rule's own result, or nullptr when the rule has to build one
	optional_ptr<ParseResult> FindCollapsibleResult() const {
		optional_ptr<ParseResult> collapsible;
		for (idx_t i = 0; i < result_count; i++) {
			auto &child_result = results.get()[i].get();
			// an optional that matched nothing carries no value, so it does not stop the rule from collapsing
			if (IsEmptyOptional(child_result)) {
				continue;
			}

			// a second child with a result means the rule combines them rather than forwarding one of them
			if (collapsible) {
				return nullptr;
			}

			collapsible = child_result;
		}

		// only results that carry a rule of their own are collapsible into this one, since the result is
		// transformed by that rule
		if (collapsible && !collapsible->GetRule()) {
			return nullptr;
		}

		// null when no child produced a result, so a rule that matched empty still gets a result of its own
		return collapsible;
	}

	//! Rule-level suppression is ignored when suggestions are collected for a memoizable walk
	bool SuppressSuggestions(const ListMatcher &list_matcher) const {
		return list_matcher.suppress_suggestions && !list_state.context.memoizable_suggestions;
	}

	void DiscardSuggestions() {
		if (!SuppressSuggestions(matcher.get())) {
			return;
		}
		list_state.context.suggestions.erase(list_state.context.suggestions.begin() +
		                                         NumericCast<int64_t>(saved_suggestion_size),
		                                     list_state.context.suggestions.end());
	}

public:
	reference<const ListMatcher> matcher;
	MatchState list_state;
	//! Borrowed storage in the result arena, with at most one slot per grammar element.
	optional_ptr<reference<ParseResult>> results;
	idx_t result_count = 0;
	//! First-child ancestors share this cursor until their tails are reached.
	optional_ptr<PendingList> pending;
	//! Ordered-choice ancestors retain alternatives, but need no second cursor or result buffer.
	optional_ptr<PendingChoice> choice_regions;
	idx_t child_index = 0;
	idx_t saved_suggestion_size = 0;
	//! Before the first child commits, a prepared choice uses this list's speculative cursor.
	optional_ptr<const ChoiceMatcher> first_choice;
	idx_t choice_index = 0;
	bool single_choice = false;
	optional_idx start_offset;
	bool awaiting_child = false;
};
class ChoiceMatchFrame final : public MatchStackFrame {
public:
	ChoiceMatchFrame(const ChoiceMatcher &matcher_p, MatchState &state_p, ArenaAllocator::Position mark = {},
	                 const PackratMatchState &packrat = {})
	    : MatchStackFrame(MatcherType::CHOICE, {matcher_p, state_p}, mark, packrat), matcher(matcher_p) {
		auto selected = matcher.SelectChild(state);
		single_child = selected.IsValid();
		child_index = single_child ? selected.GetIndex() : 0;

		if (auto current = state.token_iterator.Current()) {
			start_offset = optional_idx(current->offset);
		}
	}

public:
	const ChoiceMatcher &matcher;
	optional<MatchState> child_state;
	idx_t child_index = 0;
	bool single_child = false;
	optional_idx start_offset;
	bool awaiting_child = false;
};
class OptionalMatchFrame final : public MatchStackFrame {
public:
	OptionalMatchFrame(const OptionalMatcher &matcher_p, MatchState &state_p, ArenaAllocator::Position mark = {},
	                   const PackratMatchState &packrat = {})
	    : MatchStackFrame(MatcherType::OPTIONAL, {matcher_p, state_p}, mark, packrat), matcher(matcher_p),
	      child_state(state_p) {
		if (auto current = child_state.token_iterator.Current()) {
			start_offset = optional_idx(current->offset);
		}
	}

public:
	const OptionalMatcher &matcher;
	MatchState child_state;
	optional_idx start_offset;
	bool awaiting_child = false;
};
class RepeatMatchFrame final : public MatchStackFrame {
public:
	RepeatMatchFrame(const RepeatMatcher &matcher_p, MatchState &state_p, ArenaAllocator::Position mark = {},
	                 const PackratMatchState &packrat = {})
	    : MatchStackFrame(MatcherType::REPEAT, {matcher_p, state_p}, mark, packrat), matcher(matcher_p),
	      repeat_state(state_p), results(state_p.context.process_allocator) {
		if (auto current = repeat_state.token_iterator.Current()) {
			start_offset = optional_idx(current->offset);
		}
	}

public:
	MatcherResult CreateResult() {
		return state.AllocateParseResult<RepeatParseResult>(state.context.allocator.MakeChildren(results),
		                                                    start_offset);
	}

public:
	const RepeatMatcher &matcher;
	MatchState repeat_state;
	arena_vector<reference<ParseResult>> results;
	bool matched_once = false;
	optional_idx start_offset;
	bool awaiting_child = false;
};
struct OpaqueMatchFrame final : MatchStackFrame {
	OpaqueMatchFrame(MatchInput input, ArenaAllocator::Position mark, const PackratMatchState &packrat)
	    : MatchStackFrame(MatcherType::CUSTOM, input, mark, packrat), process(input.matcher.StartMatch(input.state)) {
		D_ASSERT(process);
	}
	arena_ptr<MatchProcess> process;
};

} // namespace

optional<MatcherResult> PackratMatchState::TryLoadCachedResult(const Matcher &matcher, MatchState &state) {
	D_ASSERT(IsEnabled(matcher, state));
	auto token_index = state.token_iterator.Position();
	auto cached_result = state.context.packrat_cache->Lookup(matcher, token_index);
	if (!cached_result) {
		token_index_before = token_index;
		max_token_index_before = state.GetMaxTokenIndex();
		return nullopt;
	}

	state.token_iterator.SetPosition(cached_result->token_index_after);
	state.context.max_token_index = MaxValue(state.context.max_token_index, cached_result->max_token_index_seen);
	if (cached_result->success) {
		return MatcherResult::Success(cached_result->result);
	}
	return MatcherResult::Failure();
}

void PackratMatchState::StoreResult(const Matcher &matcher, MatchState &state, const MatcherResult &result) const {
	if (!token_index_before.IsValid()) {
		return;
	}
	ParserPackratEntry cache_entry;
	cache_entry.success = result.IsSuccess();
	cache_entry.token_index_after = state.token_iterator.Position();
	cache_entry.max_token_index_seen = MaxValue(max_token_index_before, state.GetMaxTokenIndex());
	cache_entry.result = result.GetParseResult();
	state.context.packrat_cache->Store(matcher, token_index_before.GetIndex(), cache_entry);
}

MatcherResult MatchStack::ExecuteAtomicMatcher(MatchInput input) {
	auto &matcher = input.matcher;
	auto &state = input.state;
	D_ASSERT(matcher.IsAtomic());
	state.rule = matcher.GetRule();

	PackratMatchState packrat_state;
	if (PackratMatchState::IsEnabled(matcher, state)) {
		auto cached_result = packrat_state.TryLoadCachedResult(matcher, state);
		if (cached_result) {
			return *cached_result;
		}
	}

	auto result = static_cast<const AtomicMatcher &>(matcher).MatchAtomic(state);
	packrat_state.StoreResult(matcher, state, result);
	return result;
}

optional<MatcherResult> MatchStack::PushFrame(MatchInput input) {
	input.state.rule = input.matcher.GetRule();
	PackratMatchState packrat;
	if (PackratMatchState::IsEnabled(input.matcher, input.state)) {
		auto cached = packrat.TryLoadCachedResult(input.matcher, input.state);
		if (cached) {
			return cached;
		}
	}
	auto &arena = input.state.context.process_allocator;
	auto mark = arena.GetPosition();
	arena_ptr<MatchStackFrame> owned;
	try {
		auto kind = input.matcher.IsStructural() ? input.matcher.Type() : MatcherType::CUSTOM;
		switch (kind) {
		case MatcherType::LIST:
			owned = arena_ptr<MatchStackFrame>(
			    arena.Make<ListMatchFrame>(input.matcher.Cast<ListMatcher>(), input.state, mark, packrat, true));
			static_cast<ListMatchFrame &>(*owned).InitializeFirstChoice();
			break;

		case MatcherType::CHOICE:
			owned = arena_ptr<MatchStackFrame>(
			    arena.Make<ChoiceMatchFrame>(input.matcher.Cast<ChoiceMatcher>(), input.state, mark, packrat));
			break;

		case MatcherType::OPTIONAL:
			owned = arena_ptr<MatchStackFrame>(
			    arena.Make<OptionalMatchFrame>(input.matcher.Cast<OptionalMatcher>(), input.state, mark, packrat));
			break;

		case MatcherType::REPEAT:
			owned = arena_ptr<MatchStackFrame>(
			    arena.Make<RepeatMatchFrame>(input.matcher.Cast<RepeatMatcher>(), input.state, mark, packrat));
			break;
		default:
			owned = arena_ptr<MatchStackFrame>(arena.Make<OpaqueMatchFrame>(input, mark, packrat));
			break;
		}
		frames.push_back(std::move(owned));
	} catch (...) {
		owned.reset();
		arena.Rewind(mark);
		throw;
	}
	return nullopt;
}

void MatchStack::DestroyTopFrame() {
	D_ASSERT(!frames.empty());
	auto &frame = *frames.back();
	auto &arena = frame.state.context.process_allocator;
	auto mark = frame.mark;
	frames.pop_back();
	arena.Rewind(mark);
}

// Full execution and the public single-step process interface share every state transition.
// Full execution and the public single-step process interface share every state transition.
// Each frame kind sets `has_child` and either `next` (a child to match) or `completed` (its result).
template <bool SINGLE_STEP>
MatchStep MatchStack::ExecuteInternal(optional_ptr<MatchStack> stack, MatchStackFrame &initial_frame,
                                      optional<MatcherResult> child_result) {
	reference<MatchStackFrame> current_frame(initial_frame);
	while (true) {
		auto &frame = current_frame.get();
		optional<MatchInput> next;
		auto completed = MatcherResult::Failure();
		bool has_child = false;
		switch (frame.kind) {
		case MatcherType::LIST: {
			auto &f = static_cast<ListMatchFrame &>(frame);
			D_ASSERT(f.awaiting_child == child_result.has_value());
			auto step = ListStep::RESUME;
			while (step == ListStep::RESUME) {
				step =
				    f.first_choice ? f.StepFirstChoice(SINGLE_STEP, child_result, next, completed) : ListStep::CHILDREN;
				if (step == ListStep::CHILDREN) {
					step = f.StepChildren(child_result, next, completed);
				}
				if (step == ListStep::COMPLETE && f.choice_regions) {
					// the alternative of a leading choice completed: the list continues with the choice's result
					f.ResumeChoiceList();
					child_result = completed;
					step = ListStep::RESUME;
				}
			}
			has_child = step == ListStep::DISPATCH_CHILD;
			break;
		}
		case MatcherType::CHOICE: {
			auto &f = static_cast<ChoiceMatchFrame &>(frame);
			D_ASSERT(f.awaiting_child == child_result.has_value());
			if (child_result) {
				f.awaiting_child = false;
				if (!f.child_state) {
					throw InternalException("Choice matcher child state is missing");
				}
				if (child_result->IsSuccess()) {
					f.state.token_iterator.SetPosition(f.child_state->token_iterator);
					completed = MakeChoiceResult(f.state, *child_result, f.child_index, f.start_offset);
					break;
				}
				if (f.single_child) {
					break;
				}
				f.child_index++;
				f.child_state.reset();
			}
			f.child_index = FindChoiceChild(f.matcher, f.state, f.child_index, f.single_child);
			if (f.child_index >= f.matcher.matchers.size()) {
				break;
			}
			f.child_state.emplace(f.state);
			f.awaiting_child = true;
			next.emplace(MatchInput {f.matcher.matchers[f.child_index].get(), *f.child_state});
			has_child = true;
			break;
		}
		case MatcherType::OPTIONAL: {
			auto &f = static_cast<OptionalMatchFrame &>(frame);
			D_ASSERT(f.awaiting_child == child_result.has_value());
			if (!child_result) {
				f.awaiting_child = true;
				next.emplace(MatchInput {f.matcher.GetChildMatcher(), f.child_state});
				has_child = true;
				break;
			}
			f.awaiting_child = false;
			if (!child_result->IsSuccess()) {
				completed = f.state.AllocateParseResult<OptionalParseResult>();
				break;
			}
			f.state.token_iterator.SetPosition(f.child_state.token_iterator);
			if (child_result->HasParseResult()) {
				completed =
				    f.state.AllocateParseResult<OptionalParseResult>(child_result->GetParseResult(), f.start_offset);
			} else {
				completed = MatcherResult::Success();
			}
			break;
		}
		case MatcherType::REPEAT: {
			auto &f = static_cast<RepeatMatchFrame &>(frame);
			D_ASSERT(f.awaiting_child == child_result.has_value());
			if (child_result) {
				f.awaiting_child = false;
				if (!child_result->IsSuccess()) {
					if (f.matched_once) {
						completed = f.CreateResult();
					}
					break;
				}
				f.matched_once = true;
				if (child_result->HasParseResult()) {
					f.results.emplace_back(*child_result->GetParseResult());
				}
				f.state.token_iterator.SetPosition(f.repeat_state.token_iterator);
				auto current = f.repeat_state.token_iterator.Current();
				if (current && current->type == TokenType::END_OF_INPUT_AUTOCOMPLETE) {
					f.matcher.GetChildMatcher().AddSuggestion(f.state);
					completed = f.CreateResult();
					break;
				}
			}
			f.awaiting_child = true;
			next.emplace(MatchInput {f.matcher.GetChildMatcher(), f.repeat_state});
			has_child = true;
			break;
		}
		default: {
			auto step = static_cast<OpaqueMatchFrame &>(frame).process->Resume(child_result);
			auto child = step.GetChild();
			if (child) {
				next.emplace(*child);
				has_child = true;
			} else {
				completed = step.GetResult();
			}
			break;
		}
		}
		if (has_child) {
			if (SINGLE_STEP) {
				return MatchStep::Child(*next);
			}
			auto &child = *next;
			if (!child.matcher.IsStructural() && child.matcher.IsAtomic()) {
				child_result = stack->ExecuteAtomicMatcher(child);
				continue;
			}
			if (child.state.BuildParseResult()) {
				if (child.matcher.Type() == MatcherType::OPTIONAL && child.matcher.IsStructural() &&
				    !child.matcher.Cast<OptionalMatcher>().GetChildMatcher().first_set.MightMatch(child.state)) {
					child.state.rule = child.matcher.GetRule();
					child_result = child.state.AllocateParseResult<OptionalParseResult>();
					continue;
				}
				if (!child.matcher.first_set.MightMatch(child.state)) {
					child_result = MatcherResult::Failure();
					continue;
				}
			}
			// PushFrame returns the cached result when the packrat cache already holds it
			child_result = stack->PushFrame(child);
			current_frame = *stack->frames.back();
			continue;
		}
		if (SINGLE_STEP) {
			return MatchStep::Complete(completed);
		}
		frame.packrat.StoreResult(frame.input_matcher, frame.state, completed);
		stack->DestroyTopFrame();
		if (stack->frames.empty()) {
			return MatchStep::Complete(completed);
		}
		child_result = completed;
		current_frame = *stack->frames.back();
	}
}

MatchStep MatchStackFrame::Resume(optional<MatcherResult> child_result) {
	// Extensions can drive a built-in process one step at a time through the same implementation.
	return MatchStack::ExecuteInternal<true>(nullptr, *this, child_result);
}

MatchStack::MatchStack() {
}

MatchStack::~MatchStack() {
	Clear();
}

void MatchStack::Clear() {
	// Child processes can reference state owned by their parents.
	while (!frames.empty()) {
		DestroyTopFrame();
	}
}

MatcherResult MatchStack::Execute(MatchInput input) {
	D_ASSERT(frames.empty());
	if (input.matcher.IsAtomic()) {
		return ExecuteAtomicMatcher(input);
	}
	frames.reserve(INITIAL_FRAME_CAPACITY);
	auto immediate = PushFrame(input);
	if (immediate) {
		return *immediate;
	}
	try {
		return ExecuteInternal<false>(this, *frames.back(), nullopt).GetResult();
	} catch (...) {
		Clear();
		throw;
	}
}

arena_ptr<MatchProcess> ListMatcher::StartMatch(MatchState &state) const {
	return state.Make<ListMatchFrame>(*this, state);
}

arena_ptr<MatchProcess> ChoiceMatcher::StartMatch(MatchState &state) const {
	return state.Make<ChoiceMatchFrame>(*this, state);
}

arena_ptr<MatchProcess> OptionalMatcher::StartMatch(MatchState &state) const {
	return state.Make<OptionalMatchFrame>(*this, state);
}

arena_ptr<MatchProcess> RepeatMatcher::StartMatch(MatchState &state) const {
	return state.Make<RepeatMatchFrame>(*this, state);
}

} // namespace duckdb
