#include "json_common.hpp"
#include "json_functions.hpp"
#include "json_stack.hpp"

namespace duckdb {

//! Internal diff. Returns nullptr to signal "no changes" to the caller,
//! which is used to skip unchanged keys in the parent object's diff.
//! lists still handled by recursive yyjson function
static yyjson_mut_val *ComputeDiff(yyjson_mut_doc *doc, yyjson_val *old_val, yyjson_val *new_val) {
	struct stack_item {
		yyjson_mut_val *key;
		yyjson_val *old_node;
		yyjson_val *new_node;
		yyjson_mut_val *parent_builder;
		yyjson_mut_val *builder;
		bool finalize;
	};

	yyjson_mut_val *result = nullptr;
	Stack<stack_item> stack;
	stack.Push(stack_item {nullptr, old_val, new_val, nullptr, nullptr, false});

	while (!stack.Empty()) {
		auto item = stack.Pop();

		// finalize phase: update result
		if (item.finalize) {
			bool has_diff = yyjson_mut_obj_size(item.builder) > 0;

			if (item.parent_builder && has_diff) {
				yyjson_mut_obj_add(item.parent_builder, item.key, item.builder);
			} else {
				result = has_diff ? item.builder : nullptr;
			}
			continue;
		}

		// Both objects: compute structural diff
		if (yyjson_is_obj(item.old_node) && yyjson_is_obj(item.new_node)) {
			auto builder = yyjson_mut_obj(doc);

			// Keys in old but not in new: removed (emit null)
			{
				idx_t idx, max;
				yyjson_val *key, *old_child;
				yyjson_obj_foreach(item.old_node, idx, max, key, old_child) {
					if (!yyjson_obj_getn(item.new_node, unsafe_yyjson_get_str(key), unsafe_yyjson_get_len(key))) {
						yyjson_mut_obj_add(builder, yyjson_val_mut_copy(doc, key), yyjson_mut_null(doc));
					}
				}
			}

			stack.Push({item.key, nullptr, nullptr, item.parent_builder, builder, true});

			// Keys in new: collect in order
			{
				idx_t idx, max;
				yyjson_val *key, *new_child;
				vector<stack_item> children;
				yyjson_obj_foreach(item.new_node, idx, max, key, new_child) {
					auto old_child =
					    yyjson_obj_getn(item.old_node, unsafe_yyjson_get_str(key), unsafe_yyjson_get_len(key));
					auto mut_key = yyjson_val_mut_copy(doc, key);

					children.push_back({mut_key, old_child, new_child, builder, nullptr, false});
				}
				// push to stack in reverse to preserve order in output
				for (auto it = children.rbegin(); it != children.rend(); ++it) {
					stack.Push(*it);
				}
			}
		} else if (!item.old_node || !yyjson_equals(item.old_node, item.new_node)) {
			auto diff_val = yyjson_val_mut_copy(doc, item.new_node);
			if (item.parent_builder) {
				yyjson_mut_obj_add(item.parent_builder, item.key, diff_val);
			} else {
				result = diff_val;
			}
		}
	}

	return result;
}

//! Compute the minimal RFC 7396 merge patch that transforms old_val into new_val.
//! Both objects: returns the structural diff (or empty object {} if equal).
//! Otherwise: returns a copy of new_val.
static yyjson_mut_val *MergePatchDiff(yyjson_mut_doc *doc, yyjson_val *old_val, yyjson_val *new_val) {
	if (yyjson_is_obj(old_val) && yyjson_is_obj(new_val)) {
		auto diff = ComputeDiff(doc, old_val, new_val);
		return diff ? diff : yyjson_mut_obj(doc);
	}
	return yyjson_val_mut_copy(doc, new_val);
}

//! Compute the RFC 7396 merge patch that transforms old into new
static void MergePatchDiffFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &lstate = JSONFunctionLocalState::ResetAndGet(state);
	auto alc = lstate.json_allocator->GetYYAlc();

	auto doc = JSONCommon::CreateDocument(alc);
	const auto count = args.size();

	UnifiedVectorFormat old_data, new_data;
	args.data[0].ToUnifiedFormat(old_data);
	args.data[1].ToUnifiedFormat(new_data);
	auto old_inputs = UnifiedVectorFormat::GetData<string_t>(old_data);
	auto new_inputs = UnifiedVectorFormat::GetData<string_t>(new_data);

	auto result_data = FlatVector::Writer<string_t>(result, count);

	for (idx_t i = 0; i < count; i++) {
		auto old_idx = old_data.sel->get_index(i);
		auto new_idx = new_data.sel->get_index(i);

		if (!new_data.validity.RowIsValid(new_idx)) {
			result_data.WriteNull();
			continue;
		}

		auto new_doc = JSONCommon::ReadDocument(new_inputs[new_idx], JSONCommon::READ_FLAG, alc);

		if (!old_data.validity.RowIsValid(old_idx)) {
			result_data.WriteStringRef(JSONCommon::WriteVal<yyjson_val>(new_doc->root, alc));
			continue;
		}

		auto old_doc = JSONCommon::ReadDocument(old_inputs[old_idx], JSONCommon::READ_FLAG, alc);
		auto diff = MergePatchDiff(doc, old_doc->root, new_doc->root);
		result_data.WriteStringRef(JSONCommon::WriteVal<yyjson_mut_val>(diff, alc));
	}
	JSONAllocator::AddBuffer(result, alc);
}

ScalarFunctionSet JSONFunctions::GetMergePatchDiffFunction() {
	ScalarFunction fun("json_merge_patch_diff", {}, LogicalType::JSON(), MergePatchDiffFunction, nullptr, nullptr,
	                   JSONFunctionLocalState::Init);
	fun.GetSignature().AddParameter("old", LogicalType::JSON()).AddParameter("new", LogicalType::JSON());
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	fun.SetFallible();

	return ScalarFunctionSet(fun);
}

} // namespace duckdb
