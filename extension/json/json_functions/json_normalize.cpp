#include "json_common.hpp"
#include "json_functions.hpp"
#include "json_stack.hpp"

namespace duckdb {

//! Sort all object keys alphabetically (byte-order) in a mutable JSON value
static void SortKeys(yyjson_mut_val *v) {
	struct stack_item {
		yyjson_mut_val *val;
	};

	Stack<stack_item> stack;
	stack.Push(stack_item {v});

	while (!stack.Empty()) {
		auto item = stack.Pop();

		// if null, do nothing
		if (!item.val) {
			continue;
		}
		// if object
		if (yyjson_mut_is_obj(item.val)) {
			idx_t size = yyjson_mut_obj_size(item.val);
			// if value is < 1
			if (size <= 1) {
				if (size == 1) {
					yyjson_mut_obj_iter iter;
					yyjson_mut_obj_iter_init(item.val, &iter);
					yyjson_mut_val *key = yyjson_mut_obj_iter_next(&iter);
					stack.Push(stack_item {yyjson_mut_obj_iter_get_val(key)});
				}
				continue;
			}

			// Extract key-value pairs
			vector<std::pair<yyjson_mut_val *, yyjson_mut_val *>> pairs;
			pairs.reserve(size);
			idx_t idx, max;
			yyjson_mut_val *key, *child_val;
			// NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast): yyjson iteration macro
			yyjson_mut_obj_foreach(item.val, idx, max, key, child_val) {
				pairs.emplace_back(key, child_val);
			}

			// Sort by key string
			std::sort(pairs.begin(), pairs.end(),
			          [](const std::pair<yyjson_mut_val *, yyjson_mut_val *> &a,
			             const std::pair<yyjson_mut_val *, yyjson_mut_val *> &b) {
				          const auto a_key = string_t(unsafe_yyjson_get_str(a.first), unsafe_yyjson_get_len(a.first));
				          const auto b_key = string_t(unsafe_yyjson_get_str(b.first), unsafe_yyjson_get_len(b.first));
				          return a_key < b_key;
			          });

			// Recursively sort nested values
			for (auto &pair : pairs) {
				stack.Push(stack_item {pair.second});
			}

			// Clear and rebuild in sorted order
			yyjson_mut_obj_clear(item.val);
			for (auto &pair : pairs) {
				yyjson_mut_obj_add(item.val, pair.first, pair.second);
			}
		} else if (yyjson_mut_is_arr(item.val)) {
			idx_t idx, max;
			yyjson_mut_val *elem;
			yyjson_mut_arr_foreach(item.val, idx, max, elem) {
				stack.Push(stack_item {elem});
			}
		}
	}

	// return ...
}

//! Normalize JSON: sort all object keys recursively, serialize compact
static void NormalizeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &lstate = JSONFunctionLocalState::ResetAndGet(state);
	auto alc = lstate.json_allocator->GetYYAlc();

	const auto count = args.size();
	UnifiedVectorFormat input_data;
	args.data[0].ToUnifiedFormat(input_data);
	auto inputs = UnifiedVectorFormat::GetData<string_t>(input_data);

	auto result_data = FlatVector::Writer<string_t>(result, count);
	for (idx_t i = 0; i < count; i++) {
		auto idx = input_data.sel->get_index(i);
		if (!input_data.validity.RowIsValid(idx)) {
			result_data.WriteNull();
			continue;
		}

		auto doc = JSONCommon::ReadDocument(inputs[idx], JSONCommon::READ_FLAG, alc);
		auto mut_doc = yyjson_doc_mut_copy(doc, alc);
		auto root = yyjson_mut_doc_get_root(mut_doc);

		SortKeys(root);

		result_data.WriteStringRef(JSONCommon::WriteVal<yyjson_mut_val>(root, alc));
	}
	JSONAllocator::AddBuffer(result, alc);
}

ScalarFunctionSet JSONFunctions::GetNormalizeFunction() {
	ScalarFunctionSet set("json_normalize");

	ScalarFunction func({}, LogicalType::VARCHAR, NormalizeFunction, nullptr, nullptr, JSONFunctionLocalState::Init);

	func.GetSignature().AddParameter("json", LogicalType::JSON());

	set.AddFunction(std::move(func));

	return set;
}

} // namespace duckdb
