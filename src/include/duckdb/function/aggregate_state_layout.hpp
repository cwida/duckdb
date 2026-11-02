//===----------------------------------------------------------------------===//
//
// duckdb/function/aggregate_state_layout.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/order_type.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/map.hpp"
#include "duckdb/common/type_util.hpp"
#include "duckdb/common/types/list_segment.hpp"
#include "duckdb/common/types/value.hpp"

namespace duckdb {

//! Detection trait: true when STATE defines a nested STATE_TYPE (i.e. StructStateType<...> or OptionalStateType<T>)
template <class STATE, class = void>
struct HasStructStateType : std::false_type {};

template <class STATE>
struct HasStructStateType<STATE, std::void_t<typename STATE::STATE_TYPE>> : std::true_type {};

//! Phantom marker type for use in StructStateType only.
//! Represents two consecutive flat fields at base+0 and base+sizeof(T): T value, bool is_set.
//! memset(0) initializes to "not set" (is_set = false, value = 0).
template <class T>
struct OptionalStateType {
	using value_type = T;
};

//! Detection trait: true when T is OptionalStateType<U> for some U.
template <class T>
struct IsOptionalStateType : std::false_type {};
template <class T>
struct IsOptionalStateType<OptionalStateType<T>> : std::true_type {};

//! Phantom marker type: resolves to the return type of the bound aggregate function.
struct StateReturnType {};

//! Phantom marker type: resolves to the type of the bound aggregate function's INDEX'th argument.
template <idx_t INDEX>
struct StateInputType {
	static constexpr idx_t index = INDEX;
};

//! Detection trait: true when T is StateInputType<INDEX> for some INDEX.
template <class T>
struct IsStateInputType : std::false_type {};
template <idx_t I>
struct IsStateInputType<StateInputType<I>> : std::true_type {};

//! Detection trait: true when T is a type source marker (StateReturnType or StateInputType<INDEX>) - as opposed to a
//! field descriptor (StateSortKey, StateTypedValue, ...) that describes a physical field rather than just a type.
template <class T>
struct IsStateTypeSource
    : std::integral_constant<bool, std::is_same<T, StateReturnType>::value || IsStateInputType<T>::value> {};

//! The runtime types of a bound aggregate function - used to resolve the logical types of state fields that are
//! only known after binding (see StateReturnType / StateInputType).
struct StateLayoutTypeInfo {
	const LogicalType &return_type;
	const vector<LogicalType> &argument_types;
};

//! Resolves a type source marker (StateReturnType or StateInputType<INDEX>) to a LogicalType.
template <class SOURCE>
LogicalType ResolveStateSourceType(const StateLayoutTypeInfo &info) {
	if constexpr (IsStateInputType<SOURCE>::value) {
		D_ASSERT(SOURCE::index < info.argument_types.size());
		return info.argument_types[SOURCE::index];
	} else {
		static_assert(std::is_same<SOURCE, StateReturnType>::value,
		              "the type source must be StateReturnType or StateInputType<INDEX>");
		return info.return_type;
	}
}

//! Phantom marker type for use inside OptionalStateType.
//! Signals that the field stores a binary sort key (string_t) that must be decoded/encoded
//! via CreateSortKeyHelpers when exporting/importing aggregate state.
//! SOURCE describes where the decoded logical type comes from; ORDER is the ordering used when creating the sort key.
template <class SOURCE, OrderType ORDER = OrderType::ASCENDING>
struct StateSortKey {
	using SOURCE_TYPE = SOURCE;
	static constexpr OrderType order_type = ORDER;
};

//! Detection trait: true when T is StateSortKey<SOURCE, ORDER> for some SOURCE/ORDER.
template <class T>
struct IsStateSortKeyType : std::false_type {};
template <class S, OrderType O>
struct IsStateSortKeyType<StateSortKey<S, O>> : std::true_type {};

//! Phantom marker type for fields that are physically stored as T, while their exported logical type comes from
//! the bound aggregate function as described by SOURCE - e.g. exporting a string_t as VARCHAR, BLOB or BIT, or
//! an int64_t "by" value as TIMESTAMP.
template <class T, class SOURCE>
struct StateTypedValue {
	using PHYSICAL_TYPE = T;
	using SOURCE_TYPE = SOURCE;
};

//! Detection trait: true when T is StateTypedValue<V, SOURCE> for some V/SOURCE.
template <class T>
struct IsStateTypedValueType : std::false_type {};
template <class T, class S>
struct IsStateTypedValueType<StateTypedValue<T, S>> : std::true_type {};

//! Shorthand for values physically stored as a string_t.
template <class SOURCE>
using StateString = StateTypedValue<string_t, SOURCE>;

//! Phantom marker type for use as an aggregate STATE's STATE_TYPE.
//! Signals that the state is a LinkedList (see list_segment.hpp) holding the rows of a LIST value.
//! Export reads the linked list into a LIST vector; import appends the LIST value's rows back into a linked list.
//! SOURCE describes where the list's logical type comes from.
//! SOURCE may be a StateSortKey<ELEMENT_SOURCE, ORDER>, signalling that the linked list physically stores binary
//! sort keys (string_t): export decodes each element via CreateSortKeyHelpers, import re-encodes them.
template <class SOURCE>
struct StateListType {
	using SOURCE_TYPE = SOURCE;
};

//! Detection trait: true when T is StateListType<SOURCE> for some SOURCE.
template <class T>
struct IsStateListType : std::false_type {};
template <class S>
struct IsStateListType<StateListType<S>> : std::true_type {};

//! Detection trait: true when STATE is itself a C++ primitive type mappable to a LogicalType via PrimitiveToLogicalType
template <class T>
struct HasPrimitiveLogicalType : std::false_type {};

template <>
struct HasPrimitiveLogicalType<bool> : std::true_type {};
template <>
struct HasPrimitiveLogicalType<int8_t> : std::true_type {};
template <>
struct HasPrimitiveLogicalType<int16_t> : std::true_type {};
template <>
struct HasPrimitiveLogicalType<int32_t> : std::true_type {};
template <>
struct HasPrimitiveLogicalType<int64_t> : std::true_type {};
template <>
struct HasPrimitiveLogicalType<uint8_t> : std::true_type {};
template <>
struct HasPrimitiveLogicalType<uint16_t> : std::true_type {};
template <>
struct HasPrimitiveLogicalType<uint32_t> : std::true_type {};
template <>
struct HasPrimitiveLogicalType<uint64_t> : std::true_type {};
template <>
struct HasPrimitiveLogicalType<hugeint_t> : std::true_type {};
template <>
struct HasPrimitiveLogicalType<uhugeint_t> : std::true_type {};
template <>
struct HasPrimitiveLogicalType<float> : std::true_type {};
template <>
struct HasPrimitiveLogicalType<double> : std::true_type {};
template <>
struct HasPrimitiveLogicalType<date_t> : std::true_type {};
template <>
struct HasPrimitiveLogicalType<dtime_t> : std::true_type {};
template <>
struct HasPrimitiveLogicalType<dtime_tz_t> : std::true_type {};
template <>
struct HasPrimitiveLogicalType<dtime_ns_t> : std::true_type {};
template <>
struct HasPrimitiveLogicalType<timestamp_t> : std::true_type {};
template <>
struct HasPrimitiveLogicalType<timestamp_sec_t> : std::true_type {};
template <>
struct HasPrimitiveLogicalType<timestamp_ms_t> : std::true_type {};
template <>
struct HasPrimitiveLogicalType<timestamp_ns_t> : std::true_type {};
template <>
struct HasPrimitiveLogicalType<timestamp_tz_t> : std::true_type {};
template <>
struct HasPrimitiveLogicalType<timestamp_tz_ns_t> : std::true_type {};
template <>
struct HasPrimitiveLogicalType<interval_t> : std::true_type {};

//! Maps a single C++ field type to a LogicalType.
//! info holds the runtime types of the bound function - used to resolve fields whose logical type cannot be
//! expressed statically (sort keys and StateTypedValue fields).
//! OptionalStateType<T> → the type of T (the optional encoding is captured in AggregateStateField::kind)
//! StateSortKey<SOURCE, ORDER> / StateTypedValue<T, SOURCE> → the resolved SOURCE type
//! T with STATE_TYPE → nested struct type   otherwise → PrimitiveToLogicalType<T>()
template <class T>
LogicalType FieldToLogicalType(const StateLayoutTypeInfo &info) {
	if constexpr (IsOptionalStateType<T>::value) {
		return FieldToLogicalType<typename T::value_type>(info);
	} else if constexpr (IsStateListType<T>::value) {
		using SRC = typename T::SOURCE_TYPE;
		if constexpr (IsStateTypeSource<SRC>::value) {
			return ResolveStateSourceType<SRC>(info);
		} else {
			// the element is described by a nested field descriptor (e.g. a sort key)
			return LogicalType::LIST(FieldToLogicalType<SRC>(info));
		}
	} else if constexpr (IsStateSortKeyType<T>::value || IsStateTypedValueType<T>::value) {
		return ResolveStateSourceType<typename T::SOURCE_TYPE>(info);
	} else if constexpr (HasStructStateType<T>::value) {
		return T::STATE_TYPE::GetLogicalType(T::STATE_NAMES, info);
	} else {
		return PrimitiveToLogicalType<T>();
	}
}

//! Describes the kind of a single field within an aggregate state layout.
enum class AggregateFieldKind {
	//! Scalar value. field_offset = byte offset of the value.
	PRIMITIVE,
	//! Compound struct. field_offset = byte offset of the struct base.
	//! children = one entry per struct member (offsets relative to this field's base).
	STRUCT,
	//! Nullable wrapper. field_offset = byte offset of the bool is_set flag.
	//! children has exactly one entry: the value field (PRIMITIVE, STRUCT, SORT_KEY or LIST).
	//! The value field's field_offset is relative to the same parent base as this field.
	OPTIONAL_VALUE,
	//! Binary sort key (stored as string_t). field_offset = byte offset of the string_t.
	//! sort_key_order carries the ordering. Always appears as children[0] of an OPTIONAL_VALUE field.
	SORT_KEY,
	//! Linked list of values (stored as a LinkedList, see list_segment.hpp). field_offset = byte offset of the
	//! LinkedList. Exported as a LIST value; an empty linked list is exported as NULL.
	//! The segment functions used to read/write the linked list live in AggregateStateField::list_functions.
	LIST,
};

//! Per-field layout information within an aggregate state.
struct AggregateStateField {
	idx_t field_offset = 0;
	//! Physical byte size of the data described by this field (including nested struct members).
	//! For OPTIONAL_VALUE: includes is_set bool (field_offset + sizeof(bool)).
	idx_t field_size = 0;
	//! The C++ alignment (alignof) of the field's data - the offsets in the real state structs depend on it,
	//! and it is platform-dependent (e.g. string_t is 8-byte aligned on 64-bit platforms but 4-byte aligned on
	//! 32-bit Windows), so it must be recorded from the actual C++ types rather than derived from the field size
	idx_t field_alignment = 0;
	AggregateFieldKind kind = AggregateFieldKind::PRIMITIVE;
	OrderType sort_key_order = OrderType::ASCENDING; // only meaningful when kind == SORT_KEY
	//! For LIST: always holds a single element descriptor (children[0]). A SORT_KEY element means the linked list
	//! physically stores binary sort keys; any other kind means the elements are stored directly.
	vector<AggregateStateField> children;
	//! The segment functions used to read/write the linked list - only set when kind is LIST
	//! (populated by PopulateListFunctions, which requires the resolved logical type)
	ListSegmentFunctions list_functions;
	//! For LIST: whether the linked list cannot contain NULL elements - importing a NULL element is an error
	bool reject_null_elements = false;
	//! For LIST: whether an empty linked list is exported as a non-NULL empty list instead of NULL. Set for the
	//! sorted aggregate buffer so that an empty group exports a valid (non-NULL) state and finalize runs the wrapped
	//! aggregate's empty-group finalize (e.g. count -> 0) instead of short-circuiting to NULL on a NULL state.
	bool list_empty_as_empty = false;

	//! The alignment of this field when placed as a struct member, mirroring the C++ struct layout rules.
	//! For OPTIONAL_VALUE the alignment is that of the wrapped value - the trailing is_set bool does not affect it.
	idx_t GetAlignment() const {
		if (kind == AggregateFieldKind::OPTIONAL_VALUE) {
			D_ASSERT(children.size() == 1);
			return children[0].GetAlignment();
		}
		if (field_alignment != 0) {
			return field_alignment;
		}
		return MinValue<idx_t>(field_size, 8);
	}

	//! Shift this field's offsets to place it at `offset` within its parent.
	//! The value child of an OPTIONAL_VALUE is relative to the same parent base as the optional itself, so it shifts
	//! along; STRUCT children are relative to the struct's own base and stay untouched.
	void ShiftBase(idx_t offset) {
		field_offset += offset;
		if (kind == AggregateFieldKind::OPTIONAL_VALUE) {
			D_ASSERT(children.size() == 1);
			children[0].ShiftBase(offset);
		}
	}

	//! Populate the segment functions of all LIST fields in the field tree, walking the (resolved) logical type
	//! alongside the fields. Called once when the layout is created.
	static void PopulateListFunctions(const LogicalType &type, AggregateStateField &field) {
		switch (field.kind) {
		case AggregateFieldKind::LIST: {
			D_ASSERT(type.id() == LogicalTypeId::LIST);
			D_ASSERT(field.children.size() == 1);
			// sort-key elements are physically stored as BLOB, all other elements as their logical child type
			const auto child_type = ListType::GetChildType(type);
			const auto stored_type =
			    field.children[0].kind == AggregateFieldKind::SORT_KEY ? LogicalType::BLOB : child_type;
			GetSegmentDataFunctions(field.list_functions, stored_type);
			PopulateListFunctions(child_type, field.children[0]);
			break;
		}
		case AggregateFieldKind::OPTIONAL_VALUE:
			D_ASSERT(field.children.size() == 1);
			PopulateListFunctions(type, field.children[0]);
			break;
		case AggregateFieldKind::STRUCT: {
			const auto &child_types = StructType::GetChildTypes(type);
			D_ASSERT(child_types.size() == field.children.size());
			for (idx_t child_idx = 0; child_idx < field.children.size(); child_idx++) {
				PopulateListFunctions(child_types[child_idx].second, field.children[child_idx]);
			}
			break;
		}
		default:
			break;
		}
	}

	static idx_t GetPhysicalSize(const LogicalType &type) {
		if (type.id() != LogicalTypeId::STRUCT) {
			return GetTypeIdSize(type.InternalType());
		}
		idx_t size = 0;
		for (const auto &child : StructType::GetChildTypes(type)) {
			size = AlignValue(size, GetPhysicalAlignment(child.second));
			size += GetPhysicalSize(child.second);
		}
		return size;
	}

	//! The C++ alignment of a state member described by a logical type, mirroring alignof of the corresponding
	//! member in the actual state struct. The alignment is platform-dependent (e.g. string_t is 8-byte aligned on
	//! 64-bit platforms but only 4-byte aligned on 32-bit Windows), so it must come from the actual C++ types.
	static idx_t GetPhysicalAlignment(const LogicalType &type) {
		if (type.id() == LogicalTypeId::STRUCT) {
			idx_t alignment = 1;
			for (const auto &child : StructType::GetChildTypes(type)) {
				alignment = MaxValue<idx_t>(alignment, GetPhysicalAlignment(child.second));
			}
			return alignment;
		}
		switch (type.InternalType()) {
		case PhysicalType::VARCHAR:
			return alignof(string_t);
		case PhysicalType::INT64:
		case PhysicalType::UINT64:
			return alignof(int64_t);
		case PhysicalType::DOUBLE:
			return alignof(double);
		case PhysicalType::INT128:
			return alignof(hugeint_t);
		case PhysicalType::UINT128:
			return alignof(uhugeint_t);
		case PhysicalType::INTERVAL:
			return alignof(interval_t);
		case PhysicalType::LIST:
			return alignof(list_entry_t);
		default:
			// the remaining primitive types are at most 4 bytes - their alignment equals their size
			return MinValue<idx_t>(GetTypeIdSize(type.InternalType()), 8);
		}
	}

	static void PopulateChildren(const LogicalType &type, AggregateStateField &field) {
		if (type.id() != LogicalTypeId::STRUCT) {
			return;
		}
		D_ASSERT(field.children.empty());
		idx_t offset = 0;
		for (auto &[name, child_type] : StructType::GetChildTypes(type)) {
			idx_t child_size = GetPhysicalSize(child_type);
			offset = AlignValue(offset, GetPhysicalAlignment(child_type));
			AggregateStateField child_field;
			child_field.field_offset = offset;
			child_field.field_size = child_size;
			child_field.field_alignment = GetPhysicalAlignment(child_type);
			child_field.kind =
			    (child_type.id() == LogicalTypeId::STRUCT) ? AggregateFieldKind::STRUCT : AggregateFieldKind::PRIMITIVE;
			PopulateChildren(child_type, child_field);
			field.children.push_back(std::move(child_field));
			offset += child_size;
		}
	}
};

// Forward-declared so StructStateType::AppendChildren can call it before the full definition below.
template <class T>
AggregateStateField BuildStateField();

//! Describes a struct-typed aggregate state layout. Intended for use as a nested type inside an aggregate STATE struct:
//!   static constexpr const char *STATE_NAMES[] = {"field_a", "field_b"};
//!   using STATE_TYPE = StructStateType<uint64_t, double>;
//! UnaryAggregate/BinaryAggregate/NullaryAggregate detect STATE_TYPE and wire up SetStructStateExport automatically.
//! Fields that themselves have STATE_TYPE are recursively expanded into nested struct types.
//! OptionalStateType<T> fields are exported as nullable T (T value at base+0, bool is_set at base+sizeof(T)).
//! Names are passed at call time (STATE::STATE_NAMES) rather than as template arguments.
template <typename... Ts>
struct StructStateType {
	static LogicalType GetLogicalType(const char *const *names, const StateLayoutTypeInfo &info) {
		child_list_t<LogicalType> children;
		idx_t i = 0;
		(children.emplace_back(names[i++], FieldToLogicalType<Ts>(info)), ...);
		return LogicalType::STRUCT(std::move(children));
	}

	//! Appends one child field for type T into field.children, advancing offset by the field's physical size.
	template <class T>
	static void AppendChildField(AggregateStateField &field, idx_t &offset) {
		auto child = BuildStateField<T>();
		offset = AlignValue(offset, child.GetAlignment());
		child.ShiftBase(offset);
		offset += child.field_size;
		field.children.push_back(std::move(child));
	}

	//! Populate field.children for all member types, computing offsets from sizes.
	static void AppendChildren(AggregateStateField &field, idx_t &offset) {
		(AppendChildField<Ts>(field, offset), ...);
	}
};

//! Detection trait: true when T is StructStateType<Us...> for some Us.
template <class T>
struct IsStructStateType : std::false_type {};
template <class... Ts>
struct IsStructStateType<StructStateType<Ts...>> : std::true_type {};

//! Build an AggregateStateField for a compile-time state type T, with field_offset=0.
//! field_size is always set to the physical byte size of T's data (see kind docs for details).
//!
//! Composable rules:
//!   - OptionalStateType<V>  → OPTIONAL_VALUE: is_set at field_offset=V.field_size, wraps BuildStateField<V>()
//!   - StateSortKey<ORDER>   → SORT_KEY: field_size=sizeof(string_t)
//!   - StructStateType<Ts…>  → STRUCT: children built via AppendChildren, field_size=total child data size
//!   - anything else         → PRIMITIVE: field_size=sizeof(T)
template <class T>
AggregateStateField BuildStateField() {
	AggregateStateField field;
	if constexpr (IsOptionalStateType<T>::value) {
		using V = typename T::value_type;
		field.kind = AggregateFieldKind::OPTIONAL_VALUE;
		auto value_child = BuildStateField<V>();
		value_child.field_offset = 0;
		field.field_offset = value_child.field_size; // is_set follows the value data
		field.field_size = value_child.field_size + sizeof(bool);
		field.children.push_back(std::move(value_child));
	} else if constexpr (IsStateSortKeyType<T>::value) {
		field.kind = AggregateFieldKind::SORT_KEY;
		field.sort_key_order = T::order_type;
		field.field_size = sizeof(string_t);
		field.field_alignment = alignof(string_t);
	} else if constexpr (IsStateTypedValueType<T>::value) {
		// stored as a plain value - only the logical type is resolved at bind time
		field.field_size = sizeof(typename T::PHYSICAL_TYPE);
		field.field_alignment = alignof(typename T::PHYSICAL_TYPE);
	} else if constexpr (IsStateListType<T>::value) {
		field.kind = AggregateFieldKind::LIST;
		field.field_size = sizeof(LinkedList);
		field.field_alignment = alignof(LinkedList);
		using SRC = typename T::SOURCE_TYPE;
		if constexpr (IsStateTypeSource<SRC>::value) {
			// the elements are stored directly - the (PRIMITIVE) element field resolves its type from the logical child
			field.children.emplace_back();
		} else {
			// the source is a field descriptor (e.g. a sort key) describing a per-element transform
			field.children.push_back(BuildStateField<SRC>());
		}
	} else if constexpr (IsStructStateType<T>::value) {
		// T is StructStateType<Ts...> — the phantom descriptor type itself
		field.kind = AggregateFieldKind::STRUCT;
		idx_t offset = 0;
		T::AppendChildren(field, offset);
		field.field_size = offset; // total physical size of all members
		for (auto &child : field.children) {
			field.field_alignment = MaxValue<idx_t>(field.field_alignment, child.GetAlignment());
		}
	} else if constexpr (HasStructStateType<T>::value) {
		// T is a concrete C++ struct that declares STATE_TYPE = StructStateType<...>
		field.kind = AggregateFieldKind::STRUCT;
		idx_t offset = 0;
		T::STATE_TYPE::AppendChildren(field, offset);
		field.field_size = sizeof(T);
		field.field_alignment = alignof(T);
	} else {
		// PRIMITIVE
		field.field_size = sizeof(T);
		field.field_alignment = alignof(T);
	}
	return field;
}

//! Top-level description of an aggregate state for export/import purposes.
//! Returned by the aggregate_get_state_type_t callback registered via SetStructStateExport.
//!
//! - Primitive state (e.g. int64_t for count): field.kind=PRIMITIVE, field.field_offset=0, field.children empty.
//! - Optional primitive (e.g. OptionalStateType<double>): field.kind=OPTIONAL_VALUE,
//!   field.field_offset=sizeof(double) (is_set offset), field.children=[{kind=PRIMITIVE, field_offset=0}].
//! - Optional struct: field.kind=OPTIONAL_VALUE, field.field_offset=struct_size (is_set offset),
//!   field.children=[{kind=STRUCT, field_offset=0, children=[struct fields]}].
//! - Non-optional struct: field.kind=STRUCT, field.field_offset=0, field.children=[struct fields].
//! total_state_size is the aligned stride between consecutive states in a packed buffer.
//! Checks `count` imported states, `stride` bytes apart - throws if a state is invalid
typedef void (*aggregate_validate_state_t)(const_data_ptr_t states, idx_t count, idx_t stride);

struct AggregateStateLayout {
	AggregateStateLayout() = default;
	AggregateStateLayout(LogicalType type_p, idx_t total_state_size_p, bool is_optional = false)
	    : type(std::move(type_p)), total_state_size(total_state_size_p) {
		if (is_optional) {
			field.kind = AggregateFieldKind::OPTIONAL_VALUE;
			field.field_offset = AggregateStateField::GetPhysicalSize(type); // is_set after the value
			field.field_size = field.field_offset + sizeof(bool);
			AggregateStateField value_child;
			value_child.field_offset = 0;
			value_child.field_size = field.field_offset;
			value_child.kind =
			    (type.id() == LogicalTypeId::STRUCT) ? AggregateFieldKind::STRUCT : AggregateFieldKind::PRIMITIVE;
			AggregateStateField::PopulateChildren(type, value_child);
			field.children.push_back(std::move(value_child));
		} else if (type.id() == LogicalTypeId::STRUCT) {
			field.kind = AggregateFieldKind::STRUCT;
			AggregateStateField::PopulateChildren(type, field);
			field.field_size = AggregateStateField::GetPhysicalSize(type);
		} else {
			field.field_size = AggregateStateField::GetPhysicalSize(type);
		}
		// else: field.kind = PRIMITIVE (default), field.children empty
	}

	LogicalType type;
	AggregateStateField field;
	idx_t total_state_size = 0;
	//! Constant values for arguments that must be re-bound with a specific constant rather than only the type
	unordered_map<idx_t, Value> constant_parameters;
	//! Checks the imported states for invariants the fields cannot express (e.g. between fields)
	aggregate_validate_state_t validate_state = nullptr;
};

} // namespace duckdb
