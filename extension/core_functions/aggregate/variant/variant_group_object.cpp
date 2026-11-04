#include "core_functions/aggregate/variant_functions.hpp"
#include "duckdb/common/assert.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/primitive_dictionary.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/list_segment.hpp"
#include "duckdb/common/types/variant/variant_builder.hpp"
#include "duckdb/common/types/variant_iterator.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/function/aggregate/list_aggregate.hpp"
#include "duckdb/storage/arena_allocator.hpp"
#include "duckdb/function/aggregate_function.hpp"
namespace duckdb {

namespace {

// Pack the key and value of a row together so they are considered one entry in the list aggregate state.
static LogicalType GetInternalBufferType() {
	return LogicalType::STRUCT({{"key", LogicalType::VARCHAR}, {"value", LogicalType::VARIANT()}});
}

struct VariantObjAggState : ListAggState {};

struct VariantObjFun {
	static LogicalType GetElementType(AggregateInputData &aggr_input_data) {
		return GetInternalBufferType();
	}
};

void PackPair(Vector inputs[], idx_t count, Vector &packed) {
	auto &entries = StructVector::GetEntries(packed);
	auto &key = entries[0];
	auto &value = entries[1];

	key.Reference(inputs[0]);
	value.Reference(inputs[1]);
	FlatVector::SetSize(packed, count);

	const auto key_validity = key.Validity();
	const auto value_validity = value.Validity();
	if (key_validity.CannotHaveNull() && value_validity.CannotHaveNull()) {
		return;
	}

	for (idx_t i = 0; i < count; i++) {
		if (!key_validity.IsValid(i)) {
			// Similar to JSON, we do not accept NULL key values.
			throw InvalidInputException("variant_group_object key cannot be NULL");
		}
	}
}

void VariantObjUpdate(Vector inputs[], AggregateInputData &aggr_input_data, idx_t input_count, Vector &states,
                      idx_t count) {
	D_ASSERT(input_count == 2);
	if (count == 0) {
		return;
	}

	Vector packed(GetInternalBufferType(), count);
	PackPair(inputs, count, packed);
	ListUpdateFunction<false>(&packed, aggr_input_data, 1, states, count);
}

class VariantObjSource {
public:
	VariantObjSource(const VectorIterator<VariantObjAggState *> &states, const ListSegmentFunctions &functions,
	                 ArenaAllocator &allocator)
	    : allocator(allocator), states(states), functions(functions) {
	}

	// Invoked for each aggregate group in the result
	bool Emit(idx_t row, VariantBuilder &builder) {
		const auto &state = *states[row].GetValue();
		auto logical_count = state.linked_list.total_capacity;
		if (logical_count == 0) {
			return true;
		}

		// a group can be the whole input, so the flattened pairs are allocated through the (tracked) base allocator
		DataChunk packed_chunk;
		packed_chunk.Initialize(allocator.GetUnderlyingAllocator(), {GetInternalBufferType()}, logical_count);
		auto &packed = packed_chunk.data[0];
		functions.BuildListVector(state.linked_list, packed, 0);
		FlatVector::SetSize(packed, logical_count);

		auto &entries = StructVector::GetEntries(packed);
		const auto &keys = entries[0];
		const auto &values = entries[1];

		// keys are verified to be non-NULL on update, but the state can also be imported
		const auto key_validity = keys.Validity();
		for (idx_t i = 0; key_validity.CanHaveNull() && i < key_validity.size(); i++) {
			if (!key_validity.IsValid(i)) {
				throw InvalidInputException("variant_group_object key cannot be NULL");
			}
		}
		// The VARIANT type does not support duplicate keys
		if (const auto duplicate = HasDuplicateKeys(keys)) {
			throw InvalidInputException("variant_group_object contains duplicate key \"%s\"", *duplicate);
		}

		VariantIterator value_it(values);
		const auto key_it = keys.Values<string_t>();

		builder.EmitObject(
		    keys.size(), [&](idx_t child_idx) { return key_it.GetValueUnsafe(child_idx); },
		    [&](idx_t child_idx) {
			    const auto node = value_it.Root(child_idx);
			    if (node.IsNull()) {
				    builder.EmitNull();
				    return;
			    }
			    EmitIterator(node, builder);
		    });

		return false;
	}

private:
	optional<string> HasDuplicateKeys(const Vector &keys) {
		allocator.AlignNext();
		PrimitiveDictionary<string_t> seen_keys(allocator.GetAllocator(), MaxValue(keys.size(), idx_t(1)), 1);
		auto key_iterator = keys.Values<string_t>();

		for (const auto &entry : key_iterator) {
			auto key = entry.GetValueUnsafe();
			auto old_size = seen_keys.GetSize();

			seen_keys.Insert(key);

			if (seen_keys.GetSize() == old_size) {
				return key.GetString();
			}
		}

		return nullopt;
	}

private:
	ArenaAllocator &allocator;
	const VectorIterator<VariantObjAggState *> &states;
	const ListSegmentFunctions &functions;
};

void VariantObjFinalize(Vector &vec, AggregateFinalizeInputData &data, Vector &result, idx_t count, idx_t offset) {
	D_ASSERT(result.GetType().id() == LogicalTypeId::VARIANT);

	ListSegmentFunctions functions;
	GetSegmentDataFunctions(functions, GetInternalBufferType());

	const auto states = vec.Values<VariantObjAggState *>();
	Vector tmp(LogicalType::VARIANT(), count);

	VariantObjSource source(states, functions, data.allocator);
	BuildVariant(source, count, tmp);

	VectorOperations::Copy(tmp, result, count, 0, offset);
	FlatVector::SetSize(result, offset + count);
}

void VariantObjClusterUpdate(Vector inputs[], AggregateInputData &aggr_input_data, idx_t input_count,
                             const ClusteredAggr &clustered, idx_t count) {
	D_ASSERT(input_count == 2);
	if (count == 0) {
		return;
	}

	Vector packed(GetInternalBufferType(), count);
	PackPair(inputs, count, packed);
	ListClusterUpdate<false>(&packed, aggr_input_data, 1, clustered, count);
}

AggregateStateLayout VariantObjStateLayout(AggregateLayoutInput &input) {
	AggregateStateLayout layout;
	if (input.function.GetReturnType().IsAggregateState()) {
		layout.type = input.function.GetReturnType();
	} else {
		layout.type = LogicalType::LIST(GetInternalBufferType());
	}
	layout.total_state_size = AlignValue<idx_t>(sizeof(VariantObjAggState));
	layout.field = BuildStateField<StateListType<StateReturnType>>();
	AggregateStateField::PopulateListFunctions(layout.type, layout.field);
	return layout;
}

} // namespace

AggregateFunction VariantGroupObjectFun::GetFunction() {
	auto function =
	    AggregateFunction({LogicalType::VARCHAR, LogicalType::VARIANT()}, LogicalType::VARIANT(),
	                      AggregateFunction::StateSize<VariantObjAggState>,
	                      AggregateFunction::StateInitialize<VariantObjAggState, VariantObjFun>, VariantObjUpdate,
	                      ListCombineFunction<VariantObjFun>, VariantObjFinalize, VariantObjClusterUpdate);
	function.GetSignature().GetParameter(0).SetName("key");
	function.GetSignature().GetParameter(1).SetName("value");
	function.SetStructStateExport(VariantObjStateLayout);
	function.SetFallible(); // NULL keys are rejected at update time and duplicate keys at finalize time
	return function;
}

} // namespace duckdb
