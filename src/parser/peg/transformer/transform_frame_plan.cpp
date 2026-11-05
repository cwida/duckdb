#include "duckdb/parser/peg/transformer/peg_transformer.hpp"

namespace duckdb {

static ParseResult &UnwrapParens(ParseResult &parse_result, idx_t parens) {
	reference<ParseResult> result(parse_result);
	for (idx_t i = 0; i < parens; i++) {
		result = PEGTransformerFactory::ExtractResultFromParens(result.get());
	}
	return result.get();
}

static ParseResult &FollowPath(ParseResult &start, const TransformChildPath &path) {
	auto &unwrapped = UnwrapParens(start, path.parens);
	if (path.nested_child == TRANSFORM_PATH_END) {
		return unwrapped;
	}
	return UnwrapParens(unwrapped.Cast<ListParseResult>().GetChild(path.nested_child), path.nested_parens);
}

static ParseResult &PlannedChild(ListParseResult &list_pr, const TransformChildPlan &child) {
	return FollowPath(list_pr.GetChild(child.parse_child), child.path);
}

//! The parse result of an optional child, if present
static optional_ptr<ParseResult> PlannedOptionalChild(ListParseResult &list_pr, const TransformChildPlan &child) {
	auto &opt = PlannedChild(list_pr, child).Cast<OptionalParseResult>();
	if (!opt.HasResult()) {
		return nullptr;
	}
	return FollowPath(opt.GetResult(), child.content_path);
}

static bool IsDynamicChild(const TransformChildPlan &child) {
	return child.kind != TransformChildKind::REQUIRED && child.kind != TransformChildKind::OPTIONAL;
}

//! The items of a dynamic (list or repeat) child, in grammar order
static vector<reference<ParseResult>> PlannedDynamicItems(ListParseResult &list_pr, const TransformChildPlan &child) {
	vector<reference<ParseResult>> items;
	switch (child.kind) {
	case TransformChildKind::LIST:
		return PEGTransformerFactory::ExtractParseResultsFromList(PlannedChild(list_pr, child));
	case TransformChildKind::OPTIONAL_LIST: {
		auto content = PlannedOptionalChild(list_pr, child);
		if (content) {
			return PEGTransformerFactory::ExtractParseResultsFromList(*content);
		}
		return items;
	}
	case TransformChildKind::REPEAT: {
		auto repeat_children = PlannedChild(list_pr, child).Cast<RepeatParseResult>().GetChildren();
		items.reserve(repeat_children.size());
		for (idx_t i = 0; i < repeat_children.size(); i++) {
			items.push_back(repeat_children[i]);
		}
		return items;
	}
	case TransformChildKind::OPTIONAL_REPEAT: {
		auto content = PlannedOptionalChild(list_pr, child);
		if (!content) {
			return items;
		}
		auto repeat_children = content->Cast<RepeatParseResult>().GetChildren();
		items.reserve(repeat_children.size());
		for (idx_t i = 0; i < repeat_children.size(); i++) {
			items.push_back(repeat_children[i]);
		}
		return items;
	}
	default:
		throw InternalException("Unexpected dynamic transformer child kind");
	}
}

void PEGTransformerFactory::InitializePlannedTrampoline(PEGTransformer &transformer,
                                                        GeneratedTransformProcess &process) {
	auto &plan = *process.info.plan;
	if (plan.child_count == 0) {
		process.ReserveChildSlots(plan.fixed_slots);
		return;
	}
	auto &list_pr = process.parse_result.Cast<ListParseResult>();
	// the dynamic child decides how many slots the frame needs: the plan counts it as one slot, and its items take
	// one each. Kept in idx_t, added before the one is taken off: with no items, size() - 1 wraps in size_t, which
	// is 32 bits wide on some platforms.
	vector<reference<ParseResult>> dynamic_items;
	idx_t dynamic_item_count = 0;
	idx_t slot_count = plan.fixed_slots;
	if (plan.has_dynamic) {
		for (idx_t i = 0; i < plan.child_count; i++) {
			if (IsDynamicChild(plan.children[i])) {
				dynamic_items = PlannedDynamicItems(list_pr, plan.children[i]);
				break;
			}
		}
		dynamic_item_count = dynamic_items.size();
		slot_count = slot_count + dynamic_item_count - 1;
	}
	process.ReserveChildSlots(slot_count);
	// children are pushed last-first, so the first child in grammar order is transformed first
	for (idx_t i = 0; i < plan.child_count; i++) {
		auto &child = plan.children[i];
		idx_t slot = child.slot;
		if (child.after_dynamic) {
			slot = slot + dynamic_item_count - 1;
		}
		switch (child.kind) {
		case TransformChildKind::REQUIRED:
			process.PushChild({transformer.GetRule(child.rule), PlannedChild(list_pr, child)}, slot);
			break;
		case TransformChildKind::OPTIONAL: {
			auto content = PlannedOptionalChild(list_pr, child);
			if (content) {
				process.PushChild({transformer.GetRule(child.rule), *content}, slot);
			}
			break;
		}
		default:
			for (idx_t item_idx = dynamic_item_count; item_idx > 0; item_idx--) {
				process.PushChild({transformer.GetRule(child.rule), dynamic_items[item_idx - 1].get()},
				                  slot + item_idx - 1);
			}
			break;
		}
	}
}

void PEGTransformerFactory::InitializePlannedChoiceTrampoline(PEGTransformer &transformer,
                                                              GeneratedTransformProcess &process) {
	auto &dispatch = process.info.plan->choice;
	auto &list_pr = process.parse_result.Cast<ListParseResult>();
	auto &choice_pr = list_pr.Child<ChoiceParseResult>(0);
	auto &choice_result = choice_pr.GetResult();
	process.ReserveChildSlots(1);
	auto child_rule = choice_result.GetRule();
	auto has_transform_process = child_rule && child_rule->transform_process;
	auto type_bit = ParseResultTypeBit(choice_result.type);
	// the finalize step reads these alternatives straight from the parse result
	if ((dispatch.early_type_mask & type_bit) || (dispatch.early_empty_name && choice_result.Name().empty())) {
		return;
	}
	if (!has_transform_process) {
		if (dispatch.fallback_type_mask & type_bit) {
			return;
		}
		for (idx_t i = 0; i < dispatch.fallback_name_count; i++) {
			if (choice_result.Name() == dispatch.fallback_names[i]) {
				return;
			}
		}
		if (!dispatch.throw_without_process) {
			return;
		}
		throw InternalException("No transform process registered for rule '%s'", choice_result.Name());
	}
	process.PushChild({*child_rule, choice_result}, 0);
}

arena_ptr<TransformResultValue>
PEGTransformerFactory::FinalizePassthroughTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process) {
	idx_t slot = process.info.plan->result_slot;
	if (slot >= process.child_results.size() || !process.child_results[slot]) {
		throw InternalException("Missing transformer result for slot %llu in rule '%s'", slot, process.info.name);
	}
	// the rule returns its child's result as its own, so the child's result object is reused
	return std::move(process.child_results[slot]);
}

} // namespace duckdb
