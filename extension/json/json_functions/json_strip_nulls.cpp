#include "json_common.hpp"
#include "json_functions.hpp"
#include "json_stack.hpp"

namespace duckdb {

//! Remove all object keys with null values
static void StripNulls(yyjson_mut_val *val) {
	struct stack_item {
		yyjson_mut_val *val;
	};

	Stack<stack_item> stack;
	stack.Push(stack_item {val});

	while (!stack.Empty()) {
		auto curr_val = stack.Pop().val;

		if (!curr_val) { // TODO: at any point would we push something onto the stack that is a nullptr val?
			return;      // TODO: maybe put this as continue
		}
		if (yyjson_mut_is_obj(curr_val)) {
			yyjson_mut_obj_iter iter;
			yyjson_mut_obj_iter_init(curr_val, &iter);
			yyjson_mut_val *key;
			while ((key = yyjson_mut_obj_iter_next(&iter)) != nullptr) {
				auto child = yyjson_mut_obj_iter_get_val(key);
				if (unsafe_yyjson_is_null(child)) {
					yyjson_mut_obj_iter_remove(&iter);
				} else {
					stack.Push(stack_item {child});
				}
			}
		} else if (yyjson_mut_is_arr(curr_val)) { // TODO: this is exceptionally stupid bc you never check if array
			                                      // items are  null ?????????
			// TODO: plus this logic could probably be combined with obj logic, its just the iterator ...
			idx_t idx, max;
			yyjson_mut_val *elem;
			yyjson_mut_arr_foreach(curr_val, idx, max, elem) {
				stack.Push(stack_item {elem});
			}
		}
	}
}

//! Strip all null-valued keys from a JSON document recursively
static void StripNullsFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &lstate = JSONFunctionLocalState::ResetAndGet(state);
	auto alc = lstate.json_allocator->GetYYAlc();

	const auto &inputs = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(inputs, result, [&](string_t input) {
		auto doc = JSONCommon::ReadDocument(input, JSONCommon::READ_FLAG, alc);
		auto mut_doc = yyjson_doc_mut_copy(doc, alc);
		auto root = yyjson_mut_doc_get_root(mut_doc);
		StripNulls(root);
		return JSONCommon::WriteVal<yyjson_mut_val>(root, alc);
	});

	JSONAllocator::AddBuffer(result, alc);
}

static void GetStripNullsFunctionInternal(ScalarFunctionSet &set, const LogicalType &json) {
	ScalarFunction fun("json_strip_nulls", {}, LogicalType::JSON(), StripNullsFunction, nullptr, nullptr,
	                   JSONFunctionLocalState::Init);
	fun.GetSignature().AddParameter("json", json);
	set.AddFunction(fun);
}

ScalarFunctionSet JSONFunctions::GetStripNullsFunction() {
	ScalarFunctionSet set("json_strip_nulls");
	GetStripNullsFunctionInternal(set, LogicalType::JSON());
	return set;
}

} // namespace duckdb
