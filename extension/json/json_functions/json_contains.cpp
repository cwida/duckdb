#include "json_executors.hpp"

namespace duckdb {

static inline bool JSONContains(yyjson_val *haystack, yyjson_val *needle);
static inline bool JSONFuzzyEquals(yyjson_val *haystack, yyjson_val *needle);

static inline bool JSONArrayFuzzyEquals(yyjson_val *haystack, yyjson_val *needle) {
	D_ASSERT(yyjson_get_tag(haystack) == (YYJSON_TYPE_ARR | YYJSON_SUBTYPE_NONE) &&
	         yyjson_get_tag(needle) == (YYJSON_TYPE_ARR | YYJSON_SUBTYPE_NONE));

	size_t needle_idx, needle_max, haystack_idx, haystack_max;
	yyjson_val *needle_child, *haystack_child;
	yyjson_arr_foreach(needle, needle_idx, needle_max, needle_child) {
		bool found = false;
		yyjson_arr_foreach(haystack, haystack_idx, haystack_max, haystack_child) {
			if (JSONFuzzyEquals(haystack_child, needle_child)) {
				found = true;
				break;
			}
		}
		if (!found) {
			return false;
		}
	}
	return true;
}

static inline bool JSONObjectFuzzyEquals(yyjson_val *haystack, yyjson_val *needle) {
	D_ASSERT(yyjson_get_tag(haystack) == (YYJSON_TYPE_OBJ | YYJSON_SUBTYPE_NONE) &&
	         yyjson_get_tag(needle) == (YYJSON_TYPE_OBJ | YYJSON_SUBTYPE_NONE));

	size_t idx, max;
	yyjson_val *key, *needle_child;
	yyjson_obj_foreach(needle, idx, max, key, needle_child) {
		auto haystack_child = yyjson_obj_getn(haystack, unsafe_yyjson_get_str(key), unsafe_yyjson_get_len(key));
		if (!haystack_child || !JSONFuzzyEquals(haystack_child, needle_child)) {
			return false;
		}
	}
	return true;
}

static inline bool JSONFuzzyEquals(yyjson_val *haystack, yyjson_val *needle) {
	D_ASSERT(haystack && needle);

	// Strict equality
	if (unsafe_yyjson_equals(haystack, needle)) {
		return true;
	}

	auto haystack_tag = yyjson_get_tag(needle);
	if (haystack_tag != yyjson_get_tag(haystack)) {
		return false;
	}

	// Fuzzy equality (contained in)
	switch (haystack_tag) {
	case YYJSON_TYPE_ARR | YYJSON_SUBTYPE_NONE:
		return JSONArrayFuzzyEquals(haystack, needle);
	case YYJSON_TYPE_OBJ | YYJSON_SUBTYPE_NONE:
		return JSONObjectFuzzyEquals(haystack, needle);
	default:
		return false;
	}
}

static inline bool JSONArrayContains(yyjson_val *haystack_array, yyjson_val *needle) {
	D_ASSERT(yyjson_get_tag(haystack_array) == (YYJSON_TYPE_ARR | YYJSON_SUBTYPE_NONE));

	size_t idx, max;
	yyjson_val *child_haystack;
	yyjson_arr_foreach(haystack_array, idx, max, child_haystack) {
		if (JSONContains(child_haystack, needle)) {
			return true;
		}
	}
	return false;
}

static inline bool JSONObjectContains(yyjson_val *haystack_object, yyjson_val *needle) {
	D_ASSERT(yyjson_get_tag(haystack_object) == (YYJSON_TYPE_OBJ | YYJSON_SUBTYPE_NONE));

	size_t idx, max;
	yyjson_val *key, *child_haystack;
	yyjson_obj_foreach(haystack_object, idx, max, key, child_haystack) {
		if (JSONContains(child_haystack, needle)) {
			return true;
		}
	}
	return false;
}

static inline bool JSONContains(yyjson_val *haystack, yyjson_val *needle) {
	if (JSONFuzzyEquals(haystack, needle)) {
		return true;
	}

	switch (yyjson_get_tag(haystack)) {
	case YYJSON_TYPE_ARR | YYJSON_SUBTYPE_NONE:
		return JSONArrayContains(haystack, needle);
	case YYJSON_TYPE_OBJ | YYJSON_SUBTYPE_NONE:
		return JSONObjectContains(haystack, needle);
	default:
		return false;
	}
}

static void JSONContainsFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	D_ASSERT(args.data.size() == 2);
	auto &lstate = JSONFunctionLocalState::ResetAndGet(state);
	auto alc = lstate.json_allocator->GetYYAlc();

	const auto &haystacks = args.data[0];
	const auto &needles = args.data[1];

	if (needles.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		if (ConstantVector::IsNull(needles)) {
			throw InternalException("JSON Contains called with constant NULL needles");
		}
		auto &needle_str = *ConstantVector::GetData<string_t>(needles);
		auto needle_doc = JSONCommon::ReadDocument(needle_str, JSONCommon::READ_FLAG, alc);
		UnaryExecutor::Execute<string_t, bool>(haystacks, result, [&](string_t haystack_str) {
			auto haystack_doc = JSONCommon::ReadDocument(haystack_str, JSONCommon::READ_FLAG, alc);
			return JSONContains(haystack_doc->root, needle_doc->root);
		});
	} else {
		BinaryExecutor::Execute<string_t, string_t, bool>(
		    haystacks, needles, result, [&](string_t haystack_str, string_t needle_str) {
			    auto needle_doc = JSONCommon::ReadDocument(needle_str, JSONCommon::READ_FLAG, alc);
			    auto haystack_doc = JSONCommon::ReadDocument(haystack_str, JSONCommon::READ_FLAG, alc);
			    return JSONContains(haystack_doc->root, needle_doc->root);
		    });
	}
}

static void GetContainsFunctionInternal(ScalarFunctionSet &set, const LogicalType &lhs, const LogicalType &rhs) {
	ScalarFunction fun({}, LogicalType::BOOLEAN, JSONContainsFunction, nullptr, nullptr, JSONFunctionLocalState::Init);
	fun.GetSignature().AddParameter("haystack", lhs).AddParameter("needle", rhs);
	set.AddFunction(fun);
}

ScalarFunctionSet JSONFunctions::GetContainsFunction() {
	ScalarFunctionSet set("json_contains");
	GetContainsFunctionInternal(set, LogicalType::VARCHAR, LogicalType::VARCHAR);
	GetContainsFunctionInternal(set, LogicalType::VARCHAR, LogicalType::JSON());
	GetContainsFunctionInternal(set, LogicalType::JSON(), LogicalType::VARCHAR);
	set.SetFallible();
	GetContainsFunctionInternal(set, LogicalType::JSON(), LogicalType::JSON());
	// TODO: implement json_contains that accepts path argument as well

	return set;
}

} // namespace duckdb
