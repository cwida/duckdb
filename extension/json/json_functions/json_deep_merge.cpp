#include "json_common.hpp"
#include "json_functions.hpp"
#include "json_stack.hpp"

namespace duckdb {

//! Coalescing deep merge: null in patch means "absent/unknown", keeps the original value.
//! Non-null patch values overwrite.
static yyjson_mut_val *DeepMerge(yyjson_mut_doc *doc, yyjson_mut_val *orig_root, yyjson_mut_val *patch_root) {
	if (!yyjson_mut_is_obj(orig_root) || !yyjson_mut_is_obj(patch_root)) {
		if (unsafe_yyjson_is_null(patch_root)) {
			return JSONCommon::MutValMutCopy(doc, orig_root);
		}
		return JSONCommon::MutValMutCopy(doc, patch_root);
	}

	auto root_builder = yyjson_mut_obj(doc);

	// Initialize stack
	struct stack_item {
		yyjson_mut_val *key;
		yyjson_mut_val *orig_node;
		yyjson_mut_val *patch_node;
		yyjson_mut_val *builder;
	};
	Stack<stack_item> stack;
	stack.Push(stack_item {nullptr, orig_root, patch_root, root_builder});

	// loop over each level of nesting
	while (!stack.Empty()) {
		auto nodes = stack.Pop();

		auto builder = nodes.builder;

		// Copy orig keys not in patch or where patch value is null
		{
			idx_t idx, max;
			yyjson_mut_val *key, *orig_val;
			// NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast): yyjson iteration macro
			yyjson_mut_obj_foreach(nodes.orig_node, idx, max, key, orig_val) {
				auto patch_val =
				    yyjson_mut_obj_getn(nodes.patch_node, unsafe_yyjson_get_str(key), unsafe_yyjson_get_len(key));
				if (!patch_val || unsafe_yyjson_is_null(patch_val)) {
					auto mut_key = JSONCommon::MutValMutCopy(doc, key);
					auto mut_val = JSONCommon::MutValMutCopy(doc, orig_val);
					yyjson_mut_obj_add(builder, mut_key, mut_val);
				}
			}
		}

		// Merge non-null items from patch
		{
			idx_t idx, max;
			yyjson_mut_val *key, *patch_val;
			// NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast): yyjson iteration macro
			yyjson_mut_obj_foreach(nodes.patch_node, idx, max, key, patch_val) {
				if (unsafe_yyjson_is_null(patch_val)) {
					continue; // null entries handled in the first pass
				}

				auto orig_val =
				    yyjson_mut_obj_getn(nodes.orig_node, unsafe_yyjson_get_str(key), unsafe_yyjson_get_len(key));
				auto mut_key = JSONCommon::MutValMutCopy(doc, key);

				// at least one of (patch_val, orig_val) is not an object, then we copy the patch if it's not null, and
				// the original otherwise
				if (!yyjson_mut_is_obj(patch_val) || !yyjson_mut_is_obj(orig_val)) {
					if (unsafe_yyjson_is_null(patch_val) && !orig_val) {
						continue;
					}

					yyjson_mut_val *mut_val;
					if (unsafe_yyjson_is_null(patch_val) && orig_val) {
						mut_val = JSONCommon::MutValMutCopy(doc, orig_val);
					} else {
						mut_val = JSONCommon::MutValMutCopy(doc, patch_val);
					}
					yyjson_mut_obj_add(builder, mut_key, mut_val);
				} else {
					auto child_builder = yyjson_mut_obj(doc);
					// now we know that both are objects and we need to check them, so we add them to the stack
					stack.Push(stack_item {mut_key, orig_val, patch_val, child_builder});
					yyjson_mut_obj_add(builder, mut_key, child_builder);
				}
			}
		}
	}

	return root_builder;
}

static inline void DeepMergeReadObjects(yyjson_mut_doc *doc, const Vector &input, yyjson_mut_val *objs[]) {
	const idx_t count = input.size();
	UnifiedVectorFormat input_data;
	input.ToUnifiedFormat(input_data);
	auto inputs = UnifiedVectorFormat::GetData<string_t>(input_data);

	for (idx_t i = 0; i < count; i++) {
		auto idx = input_data.sel->get_index(i);
		if (!input_data.validity.RowIsValid(idx)) {
			objs[i] = nullptr;
		} else {
			objs[i] =
			    yyjson_val_mut_copy(doc, JSONCommon::ReadDocument(inputs[idx], JSONCommon::READ_FLAG, &doc->alc)->root);
		}
	}
}

static void DeepMergeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &lstate = JSONFunctionLocalState::ResetAndGet(state);
	auto alc = lstate.json_allocator->GetYYAlc();

	auto doc = JSONCommon::CreateDocument(alc);
	const auto count = args.size();

	auto origs = JSONCommon::AllocateArray<yyjson_mut_val *>(alc, count);
	DeepMergeReadObjects(doc, args.data[0], origs);

	auto patches = JSONCommon::AllocateArray<yyjson_mut_val *>(alc, count);
	for (idx_t arg_idx = 1; arg_idx < args.data.size(); arg_idx++) {
		DeepMergeReadObjects(doc, args.data[arg_idx], patches);
		for (idx_t i = 0; i < count; i++) {
			if (patches[i] == nullptr) {
				origs[i] = nullptr;
			} else if (origs[i] == nullptr) {
				origs[i] = patches[i];
			} else {
				origs[i] = DeepMerge(doc, origs[i], patches[i]);
			}
		}
	}

	auto result_data = FlatVector::Writer<string_t>(result, count);
	for (idx_t i = 0; i < count; i++) {
		if (origs[i] == nullptr) {
			result_data.WriteNull();
		} else {
			result_data.WriteStringRef(JSONCommon::WriteVal<yyjson_mut_val>(origs[i], alc));
		}
	}

	JSONAllocator::AddBuffer(result, alc);
}

ScalarFunctionSet JSONFunctions::GetDeepMergeFunction() {
	ScalarFunction fun("json_deep_merge", {}, LogicalType::JSON(), DeepMergeFunction, nullptr, nullptr,
	                   JSONFunctionLocalState::Init);
	fun.GetSignature().AddParameter("json1", LogicalType::JSON()).AddParameter("json2", LogicalType::JSON());
	fun.GetSignature().AddArgs("args", LogicalType::JSON());
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	fun.SetFallible();

	return ScalarFunctionSet(fun);
}

} // namespace duckdb
