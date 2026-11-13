#include "duckdb/common/radix.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/types/variant_iterator.hpp"
#include "duckdb/common/types/variant_comparison.hpp"
#include "duckdb/function/scalar/variant_functions.hpp"

namespace duckdb {

namespace {

//! State for one haystack-needle node pair.
enum class ContainsState {
	//! Classify the current pair; if both are primitive compare their comparison categories, otherwise delegate
	//! to either object or array matcher states.
	ENTER,
	//! Find a match for the current needle element in the array. If needle_idx is outside the list of needles,
	//! all needles were matched to a haystack element. If candidate_idx is outside the list of candidates,
	//! there are no more haystack elements to satisfy the current needle.
	ARRAY_NEXT,
	//! Consume the result of the child; if we found a match we can go to the next needle element of the array,
	//! otherwise we request a check on the next haystack element.
	ARRAY_RESUME,
	//! Find a matching haystack key for the current needle entry-value pair. If no such key exists the needle object
	//! can never be a subset of the haystack, otherwise compare the child values.
	OBJECT_NEXT,
	//! Consume the result of the child; if no match is found the needle object can never be a subset of the haystack.
	//! Otherwise we continue to the next key-value pair in the needle.
	OBJECT_RESUME,
	//! Send the result of the current frame to the parent frame on the stack. If the stack is empty we return the
	//! result and we are done.
	RETURN
};

struct ContainsFrame {
	ContainsFrame(const VariantNode &haystack, const VariantNode &needle) : haystack(haystack), needle(needle) {
	}

	//! Nodes that reference (sub)trees which should be compared with each other.
	VariantNode haystack;
	VariantNode needle;
	//! The state of the current frame.
	ContainsState state = ContainsState::ENTER;
	//! Positional indexes used by either object or array matching.
	idx_t needle_idx = 0;
	idx_t candidate_idx = 0;
	//! If the comparison of the child frame was equivalent.
	bool child_result = false;
	//! If the comparison of the current frame is equivalent.
	bool result = false;

	//! Only used when comparing objects.
	vector<VariantObjectEntry> haystack_fields;
	vector<VariantObjectEntry> needle_fields;
};

struct WalkFrame {
	explicit WalkFrame(const VariantNode node) : node(node) {
	}
	//! A by-value cursor for a haystack node.
	const VariantNode node;
};

bool IsPrimitiveEqual(const VariantNode &haystack, const VariantNode &needle, VariantLogicalType haystack_type,
                      VariantLogicalType needle_type, VariantComparisonType haystack_category) {
	switch (haystack_category) {
	case VariantComparisonType::NULL_VALUE:
		return true;
	case VariantComparisonType::BOOLEAN:
		return haystack_type == needle_type;
	case VariantComparisonType::UUID:
		return haystack.GetData<hugeint_t>() == needle.GetData<hugeint_t>();
	case VariantComparisonType::REAL:
		// we need to encode so NaN == NaN, -0.0 == 0.0, and infinity == infinity
		return Radix::EncodeDouble(VariantGetRealValue(haystack_type, haystack)) ==
		       Radix::EncodeDouble(VariantGetRealValue(needle_type, needle));
	case VariantComparisonType::NUMBER:
		return VariantGetNumberKey(haystack_type, haystack) == VariantGetNumberKey(needle_type, needle);
	case VariantComparisonType::TIME:
		return VariantGetTimeValue(haystack_type, haystack) == VariantGetTimeValue(needle_type, needle);
	case VariantComparisonType::TIME_TZ:
		return haystack.GetData<dtime_tz_t>() == needle.GetData<dtime_tz_t>();
	case VariantComparisonType::TIMESTAMP:
		return VariantGetTimestampValue(haystack_type, haystack) == VariantGetTimestampValue(needle_type, needle);
	case VariantComparisonType::TIMESTAMP_TZ:
		return VariantGetTimestampTZValue(haystack_type, haystack) == VariantGetTimestampTZValue(needle_type, needle);
	case VariantComparisonType::INTERVAL:
		return haystack.GetData<interval_t>() == needle.GetData<interval_t>();
	case VariantComparisonType::BLOB:
	case VariantComparisonType::BITSTRING:
	case VariantComparisonType::GEOMETRY:
	case VariantComparisonType::VARCHAR:
		return haystack.GetString() == needle.GetString();
	case VariantComparisonType::ARRAY:
	case VariantComparisonType::OBJECT:
		return true;
	}

	return false;
}

//! Determines if the needle is an equivalent subset of the subtree of the haystack at the given node (inner walk).
bool IsEquivalentSubset(vector<ContainsFrame> &stack) {
	D_ASSERT(stack.size() == 1);

	while (!stack.empty()) {
		auto &frame = stack.back();

		switch (frame.state) {
		case ContainsState::ENTER: {
			const auto haystack_type = frame.haystack.GetTypeId();
			const auto needle_type = frame.needle.GetTypeId();
			const auto haystack_category = GetVariantComparisonType(haystack_type);
			const auto needle_category = GetVariantComparisonType(needle_type);

			if (haystack_category != needle_category) {
				frame.result = false;
				frame.state = ContainsState::RETURN;
			} else if (haystack_category == VariantComparisonType::ARRAY) {
				frame.state = ContainsState::ARRAY_NEXT;
			} else if (haystack_category == VariantComparisonType::OBJECT) {
				for (const auto &field : frame.haystack.GetObjectChildren()) {
					frame.haystack_fields.push_back(field);
				}
				for (const auto &field : frame.needle.GetObjectChildren()) {
					frame.needle_fields.push_back(field);
				}
				frame.state = ContainsState::OBJECT_NEXT;
			} else {
				frame.result =
				    IsPrimitiveEqual(frame.haystack, frame.needle, haystack_type, needle_type, haystack_category);
				frame.state = ContainsState::RETURN;
			}
			break;
		}

		case ContainsState::ARRAY_NEXT: {
			const auto haystack_children = frame.haystack.GetArrayChildren();
			const auto needle_children = frame.needle.GetArrayChildren();

			if (frame.needle_idx == needle_children.size()) {
				frame.result = true;
				frame.state = ContainsState::RETURN;
			} else if (frame.candidate_idx == haystack_children.size()) {
				frame.result = false;
				frame.state = ContainsState::RETURN;
			} else {
				const auto haystack_child = haystack_children[frame.candidate_idx];
				const auto needle_child = needle_children[frame.needle_idx];

				frame.state = ContainsState::ARRAY_RESUME;
				stack.emplace_back(haystack_child, needle_child);
			}
			break;
		}

		case ContainsState::ARRAY_RESUME:
			if (frame.child_result) {
				++frame.needle_idx;
				frame.candidate_idx = 0;
			} else {
				++frame.candidate_idx;
			}
			frame.state = ContainsState::ARRAY_NEXT;
			break;

		case ContainsState::OBJECT_NEXT: {
			if (frame.needle_idx == frame.needle_fields.size()) {
				frame.result = true;
				frame.state = ContainsState::RETURN;
				break;
			}

			const auto &needle_field = frame.needle_fields[frame.needle_idx];
			while (frame.candidate_idx < frame.haystack_fields.size() &&
			       needle_field.key != frame.haystack_fields[frame.candidate_idx].key) {
				++frame.candidate_idx;
			}
			if (frame.candidate_idx == frame.haystack_fields.size()) {
				frame.result = false;
				frame.state = ContainsState::RETURN;
			} else {
				const auto haystack_value = frame.haystack_fields[frame.candidate_idx].value;
				const auto needle_value = needle_field.value;

				frame.state = ContainsState::OBJECT_RESUME;
				stack.emplace_back(haystack_value, needle_value);
			}
			break;
		}

		case ContainsState::OBJECT_RESUME:
			if (!frame.child_result) {
				frame.result = false;
				frame.state = ContainsState::RETURN;
			} else {
				++frame.needle_idx;
				frame.candidate_idx = 0;
				frame.state = ContainsState::OBJECT_NEXT;
			}
			break;

		case ContainsState::RETURN: {
			const auto result = frame.result;
			stack.pop_back();

			if (stack.empty()) {
				return result;
			}

			auto &parent = stack.back();
			D_ASSERT(parent.state == ContainsState::ARRAY_RESUME || parent.state == ContainsState::OBJECT_RESUME);
			parent.child_result = result;
			break;
		}
		}
	}

	return false;
}

//! Determine if the needle is contained in the haystack (outer walk).
bool IterativeHaystackWalk(const VariantNode &needle, vector<WalkFrame> &stack, vector<ContainsFrame> &inner_stack) {
	D_ASSERT(stack.size() == 1);
	D_ASSERT(inner_stack.empty());

	while (!stack.empty()) {
		const auto frame = stack.back();
		stack.pop_back();

		const auto &haystack = frame.node;

		inner_stack.emplace_back(haystack, needle);
		if (IsEquivalentSubset(inner_stack)) {
			return true;
		}

		// DFS walk; load all children onto the stack.
		const auto &node_type = haystack.GetTypeId();
		if (node_type == VariantLogicalType::ARRAY) {
			for (const auto element : haystack.GetArrayChildren()) {
				stack.emplace_back(element);
			}
		}
		if (node_type == VariantLogicalType::OBJECT) {
			for (const auto &entry : haystack.GetObjectChildren()) {
				stack.emplace_back(entry.value);
			}
		}
	}

	// no nodes left to discover; no equivalent subset of the needle found in the haystack.
	return false;
}

void VariantContainsFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	D_ASSERT(input.ColumnCount() == 2);
	(void)state;

	const auto count = input.size();
	const VectorIterator<VectorVariantType> haystacks(input.data[0]);
	const VectorIterator<VectorVariantType> needles(input.data[1]);

	result.Initialize(VectorDataInitialization::UNINITIALIZED, count);
	auto result_writer = FlatVector::Writer<bool>(result, count);

	vector<WalkFrame> stack;
	vector<ContainsFrame> inner_stack;

	for (idx_t row_idx = 0; row_idx < count; row_idx++) {
		if (!haystacks.RowIsValid(row_idx) || !needles.RowIsValid(row_idx)) {
			result_writer.WriteNull();
			continue;
		}
		// seed the stack with the root node of the haystack.
		stack.clear();
		stack.emplace_back(haystacks[row_idx]);
		result_writer.WriteValue(IterativeHaystackWalk(needles[row_idx], stack, inner_stack));
	}
}

} // namespace

ScalarFunctionSet VariantContainsFun::GetFunctions() {
	ScalarFunction function("variant_contains", {}, LogicalType::BOOLEAN, VariantContainsFunction);
	function.GetSignature()
	    .AddParameter("variant_haystack", LogicalType::VARIANT())
	    .AddParameter("variant_needle", LogicalType::VARIANT());
	return ScalarFunctionSet(function);
}

} // namespace duckdb
