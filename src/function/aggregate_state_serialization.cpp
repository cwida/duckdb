#include "duckdb/function/aggregate_state_serialization.hpp"

#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/common/types/list_segment.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/function/create_sort_key.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/storage/arena_allocator.hpp"

namespace duckdb {

namespace {

template <class OP, class... ARGS>
void TemplateDispatch(PhysicalType type, ARGS &&... args) {
	switch (type) {
	case PhysicalType::BOOL:
		OP::template Operation<bool>(std::forward<ARGS>(args)...);
		break;
	case PhysicalType::UINT8:
		OP::template Operation<uint8_t>(std::forward<ARGS>(args)...);
		break;
	case PhysicalType::UINT16:
		OP::template Operation<uint16_t>(std::forward<ARGS>(args)...);
		break;
	case PhysicalType::UINT32:
		OP::template Operation<uint32_t>(std::forward<ARGS>(args)...);
		break;
	case PhysicalType::UINT64:
		OP::template Operation<uint64_t>(std::forward<ARGS>(args)...);
		break;
	case PhysicalType::UINT128:
		OP::template Operation<uhugeint_t>(std::forward<ARGS>(args)...);
		break;
	case PhysicalType::INT8:
		OP::template Operation<int8_t>(std::forward<ARGS>(args)...);
		break;
	case PhysicalType::INT16:
		OP::template Operation<int16_t>(std::forward<ARGS>(args)...);
		break;
	case PhysicalType::INT32:
		OP::template Operation<int32_t>(std::forward<ARGS>(args)...);
		break;
	case PhysicalType::INT64:
		OP::template Operation<int64_t>(std::forward<ARGS>(args)...);
		break;
	case PhysicalType::INT128:
		OP::template Operation<hugeint_t>(std::forward<ARGS>(args)...);
		break;
	case PhysicalType::FLOAT:
		OP::template Operation<float>(std::forward<ARGS>(args)...);
		break;
	case PhysicalType::DOUBLE:
		OP::template Operation<double>(std::forward<ARGS>(args)...);
		break;
	case PhysicalType::VARCHAR:
		OP::template Operation<string_t>(std::forward<ARGS>(args)...);
		break;
	case PhysicalType::INTERVAL:
		OP::template Operation<interval_t>(std::forward<ARGS>(args)...);
		break;
	default:
		throw NotImplementedException("Unsupported physical type for default aggregate state export: %s",
		                              TypeIdToString(type));
	}
}

// Load rows from input_vec into the packed binary state buffer. Skips null rows.
struct LoadOp {
	template <class T>
	static void Operation(idx_t root_stride, const Vector &input_vec, idx_t count, data_ptr_t base_ptr,
	                      idx_t field_offset) {
		auto values = input_vec.Values<T>();
		for (idx_t i = 0; i < count; i++) {
			const auto entry = values[i];
			if (entry.IsValid()) {
				Store(entry.GetValue(), base_ptr + i * root_stride + field_offset);
			}
		}
	}
};

// Store rows from the packed binary state buffer into a result vector at [offset, offset + count).
struct StoreOp {
	template <class T>
	static void Operation(Vector &result, idx_t count, const data_ptr_t *sources, idx_t field_offset, idx_t offset) {
		auto dst = FlatVector::Writer<T>(result, count, offset);
		for (idx_t i = 0; i < count; i++) {
			dst.WriteValue(Load<T>(sources[i] + field_offset));
		}
	}
};

// Recursively serialize a state field to a result vector, writing the `count` rows at [offset, offset + count).
// base: accumulated byte offset from the state slot start to this field's parent base.
// Each child's field_offset is relative to that parent base.
static void SerializeField(const LogicalType &type, const AggregateStateField &field, Vector &result, idx_t count,
                           const data_ptr_t *addresses, idx_t base, idx_t offset) {
	switch (field.kind) {
	case AggregateFieldKind::OPTIONAL_VALUE:
		D_ASSERT(field.children.size() == 1);
		for (idx_t i = 0; i < count; i++) {
			if (!Load<bool>(addresses[i] + base + field.field_offset)) {
				FlatVector::SetNull(result, offset + i, true);
			}
		}
		SerializeField(type, field.children[0], result, count, addresses, base, offset);
		break;
	case AggregateFieldKind::SORT_KEY:
		for (idx_t i = 0; i < count; i++) {
			if (!FlatVector::Validity(result).RowIsValid(offset + i)) {
				continue;
			}
			const string_t sort_key = Load<string_t>(addresses[i] + base + field.field_offset);
			CreateSortKeyHelpers::DecodeSortKey(sort_key, result, offset + i,
			                                    OrderModifiers(field.sort_key_order, OrderByNullType::NULLS_LAST));
		}
		break;
	case AggregateFieldKind::STRUCT: {
		const auto &child_types = StructType::GetChildTypes(type);
		auto &struct_entries = StructVector::GetEntries(result);
		const idx_t new_base = base + field.field_offset;
		for (idx_t field_idx = 0; field_idx < field.children.size(); field_idx++) {
			SerializeField(child_types[field_idx].second, field.children[field_idx], struct_entries[field_idx], count,
			               addresses, new_base, offset);
		}
		break;
	}
	case AggregateFieldKind::PRIMITIVE:
		TemplateDispatch<StoreOp>(type.InternalType(), result, count, addresses, base + field.field_offset, offset);
		break;
	case AggregateFieldKind::LIST: {
		// linked list field: build the result LIST vector from each state's linked list
		// an empty linked list is exported as NULL, matching the finalize semantics of list aggregates
		D_ASSERT(type.id() == LogicalTypeId::LIST);
		D_ASSERT(field.children.size() == 1);
		vector<LinkedList> linked_lists;
		linked_lists.reserve(count);
		for (idx_t i = 0; i < count; i++) {
			linked_lists.push_back(Load<LinkedList>(addresses[i] + base + field.field_offset));
		}
		const auto &element = field.children[0];
		if (element.kind != AggregateFieldKind::SORT_KEY) {
			// elements are stored directly - build the result LIST vector from each state's linked list
			// (BuildLists appends to the result's child, writing the list entries at [offset, offset + count))
			field.list_functions.BuildLists(linked_lists, result, offset);
			if (field.list_empty_as_empty) {
				// an empty linked list is an empty group: export it as a non-NULL empty list rather than NULL
				auto result_entries = FlatVector::GetDataMutable<list_entry_t>(result);
				for (idx_t i = 0; i < count; i++) {
					if (FlatVector::IsNull(result, offset + i)) {
						FlatVector::SetNull(result, offset + i, false);
						result_entries[offset + i] = {0, 0};
					}
				}
			}
			break;
		}
		// the elements are sort keys: build the physically stored (BLOB) elements into a temporary LIST vector, then
		// decode each sort key into the result child while rebuilding the result's list entries
		Vector physical_list(LogicalType::LIST(LogicalType::BLOB), count);
		field.list_functions.BuildLists(linked_lists, physical_list, 0);

		// append to the result child, starting after any rows already written at a lower offset
		idx_t child_offset = ListVector::GetListSize(result);
		ListVector::Reserve(result, child_offset + ListVector::GetListSize(physical_list));
		auto &result_child = ListVector::GetChildMutable(result);
		auto result_entries = FlatVector::GetDataMutable<list_entry_t>(result);
		const OrderModifiers modifiers(element.sort_key_order, OrderByNullType::NULLS_LAST);

		for (const auto list_entry : physical_list.Values<VectorListType<string_t>>()) {
			const auto row = offset + list_entry.GetIndex();
			if (!list_entry.IsValid()) {
				// an empty linked list is exported as NULL, matching the finalize semantics of list aggregates;
				// the sorted aggregate buffer instead exports an empty group as a non-NULL empty list
				if (!field.list_empty_as_empty) {
					FlatVector::SetNull(result, row, true);
				}
				result_entries[row] = {child_offset, 0};
				continue;
			}
			result_entries[row] = {child_offset, list_entry.GetListLength()};
			for (const auto sort_key : list_entry.GetChildValues()) {
				CreateSortKeyHelpers::DecodeSortKey(sort_key.GetValueUnsafe(), result_child, child_offset++, modifiers);
			}
		}
		ListVector::SetListSize(result, child_offset);
		break;
	}
	}
}

// Recursively deserialize an input vector into a packed state buffer.
// base: accumulated byte offset within each state slot for this field's parent base.
static void DeserializeField(const LogicalType &type, const AggregateStateField &field, const Vector &input_vec,
                             idx_t count, data_ptr_t dest_buffer, idx_t stride, idx_t base, ArenaAllocator &allocator,
                             StateMemoryOwnership ownership) {
	switch (field.kind) {
	case AggregateFieldKind::OPTIONAL_VALUE: {
		D_ASSERT(field.children.size() == 1);
		const auto validity = input_vec.Validity();
		for (idx_t i = 0; i < count; i++) {
			Store<bool>(validity.IsValid(i), dest_buffer + i * stride + base + field.field_offset);
		}
		DeserializeField(type, field.children[0], input_vec, count, dest_buffer, stride, base, allocator, ownership);
		break;
	}
	case AggregateFieldKind::SORT_KEY: {
		Vector sort_keys(LogicalType::BLOB);
		CreateSortKeyHelpers::CreateSortKey(
		    input_vec, count, OrderModifiers(field.sort_key_order, OrderByNullType::NULLS_LAST), sort_keys);
		auto *key_data = FlatVector::GetData<string_t>(sort_keys);
		const auto validity = input_vec.Validity();
		for (idx_t i = 0; i < count; i++) {
			if (!validity.IsValid(i)) {
				continue;
			}
			auto sort_key = key_data[i];
			if (!sort_key.IsInlined()) {
				const auto len = sort_key.GetSize();
				auto *buf = char_ptr_cast(allocator.Allocate(len));
				memcpy(buf, sort_key.GetData(), len);
				sort_key = string_t(buf, UnsafeNumericCast<uint32_t>(len));
			}
			Store<string_t>(sort_key, dest_buffer + i * stride + base + field.field_offset);
		}
		break;
	}
	case AggregateFieldKind::STRUCT: {
		const auto &child_types = StructType::GetChildTypes(type);
		const auto &struct_entries = StructVector::GetEntries(input_vec);
		const idx_t new_base = base + field.field_offset;
		for (idx_t field_idx = 0; field_idx < field.children.size(); field_idx++) {
			DeserializeField(child_types[field_idx].second, field.children[field_idx], struct_entries[field_idx], count,
			                 dest_buffer, stride, new_base, allocator, ownership);
		}
		break;
	}
	case AggregateFieldKind::PRIMITIVE:
		if (type.InternalType() == PhysicalType::VARCHAR && ownership == StateMemoryOwnership::OWNED) {
			// the states may outlive the input vector, so string values are copied to the allocator
			const auto values = input_vec.Values<string_t>();
			for (idx_t i = 0; i < count; i++) {
				const auto entry = values[i];
				if (!entry.IsValid()) {
					continue;
				}
				auto value = entry.GetValue();
				if (!value.IsInlined()) {
					const auto len = value.GetSize();
					auto *buf = char_ptr_cast(allocator.Allocate(len));
					memcpy(buf, value.GetData(), len);
					value = string_t(buf, UnsafeNumericCast<uint32_t>(len));
				}
				Store<string_t>(value, dest_buffer + i * stride + base + field.field_offset);
			}
			break;
		}
		TemplateDispatch<LoadOp>(type.InternalType(), stride, input_vec, count, dest_buffer, base + field.field_offset);
		break;
	case AggregateFieldKind::LIST: {
		// linked list field: append each row of the input LIST vector into the state's linked list
		D_ASSERT(type.id() == LogicalTypeId::LIST);
		D_ASSERT(field.children.size() == 1);
		const auto values = input_vec.Values<list_entry_t>();
		const auto &element = field.children[0];
		const auto &logical_child = ListVector::GetChild(input_vec);

		// the child is appended through the ListSegmentFunctions API, which physically stores the element type -
		// sort-key elements are first re-encoded from the logical child into a temporary BLOB child vector
		optional_ptr<const Vector> physical_child = logical_child;
		unique_ptr<Vector> encoded_child;
		if (element.kind == AggregateFieldKind::SORT_KEY) {
			const auto child_count = ListVector::GetListSize(input_vec);
			const OrderModifiers modifiers(element.sort_key_order, OrderByNullType::NULLS_LAST);
			// the result must be sized for the full (possibly larger than standard) child up front
			encoded_child = make_uniq<Vector>(LogicalType::BLOB, MaxValue<idx_t>(child_count, 1));
			CreateSortKeyHelpers::CreateSortKey(logical_child, child_count, modifiers, *encoded_child);
			physical_child = *encoded_child;
		}

		RecursiveUnifiedVectorFormat child_data;
		Vector::RecursiveToUnifiedFormat(*physical_child, child_data);
		const auto child_validity = logical_child.Validity();
		for (idx_t i = 0; i < count; i++) {
			LinkedList linked_list;
			const auto entry = values[i];
			if (entry.IsValid()) {
				if (field.reject_null_elements) {
					const auto &list_entry = entry.GetValue();
					for (idx_t child_idx = 0; child_idx < list_entry.length; child_idx++) {
						if (!child_validity.IsValid(list_entry.offset + child_idx)) {
							throw InvalidInputException(
							    "Invalid aggregate state - the list cannot contain NULL values");
						}
					}
				}
				// NULL inputs keep an empty linked list
				field.list_functions.AppendListEntry(allocator, linked_list, child_data, entry.GetValue());
			}
			Store<LinkedList>(linked_list, dest_buffer + i * stride + base + field.field_offset);
		}
		break;
	}
	}
}

} // namespace

void AggregateStateSerialization::DeserializeStates(const BoundAggregateFunction &aggr,
                                                    const AggregateStateLayout &layout, const Vector &input_vec,
                                                    idx_t count, data_ptr_t dest_buffer, ArenaAllocator &allocator,
                                                    StateMemoryOwnership ownership) {
	if (aggr.HasImportAggregateStateCallback()) {
		// the aggregate explicitly deserializes its own states, copying all variable size data
		AggregateImportInputData import_input(layout, input_vec, dest_buffer, allocator);
		aggr.GetImportAggregateStateCallback()(import_input);
		return;
	}
	DeserializeField(layout.type, layout.field, input_vec, count, dest_buffer, layout.total_state_size, 0, allocator,
	                 ownership);
	if (layout.validate_state) {
		layout.validate_state(dest_buffer, count, layout.total_state_size);
	}
}

void AggregateStateSerialization::SerializeStates(const AggregateStateLayout &layout, Vector &result, idx_t count,
                                                  const data_ptr_t *addresses, idx_t offset) {
	SerializeField(layout.type, layout.field, result, count, addresses, 0, offset);
}

void AggregateStateSerialization::SerializeStates(const BoundAggregateFunction &aggr,
                                                  optional_ptr<FunctionData> bind_data,
                                                  const AggregateStateLayout &layout, Vector &states, idx_t count,
                                                  Vector &result, ArenaAllocator &allocator, idx_t offset) {
	if (aggr.HasExportAggregateStateCallback()) {
		// the aggregate explicitly serializes its own states, writing the count rows at [offset, offset + count)
		AggregateFinalizeInputData aggr_input_data(aggr, bind_data, allocator);
		aggr.GetExportAggregateStateCallback()(states, aggr_input_data, result, count, offset);
		return;
	}
	const data_ptr_t *addresses;
	if (states.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		addresses = ConstantVector::GetData<data_ptr_t>(states);
	} else {
		addresses = FlatVector::GetData<data_ptr_t>(states);
	}
	SerializeStates(layout, result, count, addresses, offset);
}
} // namespace duckdb
