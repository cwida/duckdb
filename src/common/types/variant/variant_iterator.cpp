#include "duckdb/common/types/variant_iterator.hpp"
#include "duckdb/common/vector/shredded_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/array_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/serializer/varint.hpp"
#include "duckdb/function/scalar/variant_utils.hpp"

#include <algorithm>

namespace duckdb {

//! Indices into the shredded "STRUCT(typed_value, untyped_value_index)" wrapper
static constexpr idx_t TYPED_VALUE_INDEX = 0;
static constexpr idx_t UNTYPED_VALUE_INDEX = 1;

//! Child indices into the unshredded layout (see UnshreddedVariantLayout)
static constexpr idx_t KEYS_INDEX = 0;
static constexpr idx_t CHILDREN_INDEX = 1;
static constexpr idx_t VALUES_INDEX = 2;
static constexpr idx_t DATA_INDEX = 3;
//! Child indices within the children / values structs
static constexpr idx_t KEY_ID_INDEX = 0;
static constexpr idx_t VALUE_ID_INDEX = 1;
static constexpr idx_t TYPE_ID_INDEX = 0;
static constexpr idx_t BYTE_OFFSET_INDEX = 1;

namespace {

//! Decode the (length-prefixed) string payload of a value at the given byte offset in the blob
string_t DecodeStringData(const string_t &blob, uint32_t byte_offset) {
	auto blob_size = blob.GetSize();
	VariantUtils::Available(blob_size, byte_offset, 1);
	auto data = const_data_ptr_cast(blob.GetData());
	auto ptr = data + byte_offset;
	auto length = VarintDecode<uint32_t>(ptr);
	VariantUtils::Available(blob_size, NumericCast<idx_t>(ptr - data), length);
	return string_t(reinterpret_cast<const char *>(ptr), length);
}

//! Decode the (width, scale, value pointer) of a DECIMAL value at the given byte offset
VariantDecimalData DecodeDecimalData(const string_t &blob, uint32_t byte_offset) {
	auto blob_size = blob.GetSize();
	VariantUtils::Available(blob_size, byte_offset, 1);
	auto data = const_data_ptr_cast(blob.GetData());
	auto ptr = data + byte_offset;
	auto width = VarintDecode<uint32_t>(ptr);
	auto scale = VarintDecode<uint32_t>(ptr);
	VariantUtils::Available(blob_size, NumericCast<idx_t>(ptr - data), 0);
	return VariantDecimalData(width, scale, ptr);
}

//! Decode the (child_count, children_idx) of an OBJECT/ARRAY value at the given byte offset
VariantNestedData DecodeNestedData(const string_t &blob, uint32_t byte_offset) {
	auto blob_size = blob.GetSize();
	VariantUtils::Available(blob_size, byte_offset, 1);
	auto data = const_data_ptr_cast(blob.GetData());
	auto ptr = data + byte_offset;
	VariantNestedData result;
	result.child_count = VarintDecode<uint32_t>(ptr);
	if (result.child_count) {
		VariantUtils::Available(blob_size, NumericCast<idx_t>(ptr - data), 1);
		result.children_idx = VarintDecode<uint32_t>(ptr);
	} else {
		result.children_idx = 0;
	}
	return result;
}

//! Whether the value at logical position 'index' of a shredded layer is valid (non-NULL)
bool ShreddedIsValid(const ShreddedVariantIterator &node, idx_t index) {
	return node.unified.validity.RowIsValid(node.unified.sel->get_index(index));
}

} // namespace

//===--------------------------------------------------------------------===//
// VariantIterator
//===--------------------------------------------------------------------===//
VariantIterator::VariantIterator(const Vector &variant)
    //! The unshredded ("core") source is the variant itself, or the unshredded component of a shredded vector
    : unshredded(variant.GetVectorType() == VectorType::SHREDDED_VECTOR ? ShreddedVector::GetUnshreddedVector(variant)
                                                                        : variant) {
	if (variant.GetVectorType() != VectorType::SHREDDED_VECTOR) {
		return;
	}
	is_shredded = true;

	//! Build the (recursive) view of the shredded component. This does not reconstruct the unshredded
	//! representation (unlike flattening the SHREDDED_VECTOR itself) - it only normalizes the regular,
	//! typed vectors of the shredded tree so each layer can be navigated by index.
	auto &shredded_vec = ShreddedVector::GetShreddedVector(variant);
	shredded_format.Build(shredded_vec);
}

VariantIterator::VariantIterator(const Vector &unshredded_vec, const Vector &shredded) : unshredded(unshredded_vec) {
	is_shredded = true;
	shredded_format.Build(shredded);
}

//===--------------------------------------------------------------------===//
// ShreddedVariantIterator
//===--------------------------------------------------------------------===//
void ShreddedVariantIterator::Build(const Vector &vec) {
	vec.ToUnifiedFormat(unified);
	logical_type = vec.GetType();

	switch (vec.GetType().InternalType()) {
	case PhysicalType::LIST:
		children.resize(1);
		children[0].Build(ListVector::GetChild(vec));
		break;
	case PhysicalType::ARRAY:
		children.resize(1);
		children[0].Build(ArrayVector::GetChild(vec));
		break;
	case PhysicalType::STRUCT: {
		auto &entries = StructVector::GetEntries(vec);
		children.resize(entries.size());
		for (idx_t i = 0; i < entries.size(); i++) {
			children[i].Build(entries[i]);
		}
		break;
	}
	default:
		break;
	}
}

//===--------------------------------------------------------------------===//
// UnshreddedVariantIterator
//===--------------------------------------------------------------------===//
UnshreddedVariantIterator::UnshreddedVariantIterator(const Vector &unshredded) : data(unshredded) {
}

bool UnshreddedVariantIterator::RowIsValid(idx_t row) const {
	return data[row].IsValid();
}

VariantLogicalType UnshreddedVariantIterator::GetTypeId(idx_t row, idx_t value_index) const {
	auto raw = data[row].GetChildValue<VALUES_INDEX>().GetChildValue(value_index).GetChildValue<TYPE_ID_INDEX>();
	return static_cast<VariantLogicalType>(raw.GetValueUnsafe());
}

uint32_t UnshreddedVariantIterator::GetByteOffset(idx_t row, idx_t value_index) const {
	return data[row]
	    .GetChildValue<VALUES_INDEX>()
	    .GetChildValue(value_index)
	    .GetChildValue<BYTE_OFFSET_INDEX>()
	    .GetValueUnsafe();
}

const string_t &UnshreddedVariantIterator::GetBlob(idx_t row) const {
	return data[row].GetChildValue<DATA_INDEX>().GetValueUnsafe();
}

string_t UnshreddedVariantIterator::GetKey(idx_t row, idx_t key_index) const {
	return data[row].GetChildValue<KEYS_INDEX>().GetChildValue(key_index).GetValueUnsafe();
}

uint32_t UnshreddedVariantIterator::GetKeysIndex(idx_t row, idx_t child_index) const {
	return data[row]
	    .GetChildValue<CHILDREN_INDEX>()
	    .GetChildValue(child_index)
	    .GetChildValue<KEY_ID_INDEX>()
	    .GetValueUnsafe();
}

uint32_t UnshreddedVariantIterator::GetValuesIndex(idx_t row, idx_t child_index) const {
	return data[row]
	    .GetChildValue<CHILDREN_INDEX>()
	    .GetChildValue(child_index)
	    .GetChildValue<VALUE_ID_INDEX>()
	    .GetValueUnsafe();
}

//===--------------------------------------------------------------------===//
// Root / row validity
//===--------------------------------------------------------------------===//
VariantNode VariantIterator::Root(idx_t row) const {
	if (is_shredded) {
		//! The shredded component's top-level validity is the authoritative row validity (a SQL-NULL row
		//! has the whole shredded struct set to NULL). This must be checked separately because
		//! ResolveShredded only inspects the typed_value / untyped_value_index of a wrapper, never the
		//! wrapper's own struct validity.
		if (!ShreddedIsValid(shredded_format, row)) {
			return VariantNode::MakeNull(*this);
		}
		auto root = VariantNode::ResolveShredded(*this, shredded_format, row, row);
		//! a root value is never "missing" - treat any such case as a SQL NULL
		return root.IsMissing() ? VariantNode::MakeNull(*this) : root;
	}
	if (!unshredded.RowIsValid(row)) {
		return VariantNode::MakeNull(*this);
	}
	//! The unshredded root value lives at values[0]
	return VariantNode::MakeUnshredded(*this, row, 0);
}

bool VariantIterator::RowIsValid(idx_t row) const {
	//! A VARIANT is never NULL at the root via a VARIANT_NULL value (that is reserved for nested values) -
	//! a root that resolves to NULL is a genuine SQL NULL. This matches the semantics of unshredding,
	//! where a shredded value whose typed leaf is NULL (with no unshredded leftover) becomes a SQL NULL.
	return !Root(row).IsNull();
}

//===--------------------------------------------------------------------===//
// VariantNode - factory helpers
//===--------------------------------------------------------------------===//
VariantNode VariantNode::MakeNull(const VariantIterator &state) {
	VariantNode result;
	result.state = &state;
	result.kind = Kind::NULL_VALUE;
	return result;
}

VariantNode VariantNode::MakeMissing(const VariantIterator &state) {
	VariantNode result;
	result.state = &state;
	result.kind = Kind::MISSING;
	return result;
}

VariantNode VariantNode::MakeUnshredded(const VariantIterator &state, idx_t row, uint32_t value_index) {
	VariantNode result;
	result.state = &state;
	result.kind = Kind::UNSHREDDED;
	result.row = row;
	result.value_index = value_index;
	return result;
}

VariantNode VariantNode::MakeShredded(const VariantIterator &state, const ShreddedVariantIterator &content, idx_t index,
                                      idx_t row, uint32_t overlay_value_index) {
	VariantNode result;
	result.state = &state;
	result.kind = Kind::SHREDDED;
	result.row = row;
	result.shredded_format = content;
	result.shredded_index = index;
	result.overlay_value_index = overlay_value_index;
	return result;
}

//===--------------------------------------------------------------------===//
// Shredded resolution
//===--------------------------------------------------------------------===//
VariantNode VariantNode::ResolveShredded(const VariantIterator &state, const ShreddedVariantIterator &node, idx_t index,
                                         idx_t row) {
	if (node.logical_type.id() != LogicalTypeId::STRUCT) {
		//! A flattened (fully-consistent) primitive - a NULL here represents a VARIANT_NULL value
		if (!ShreddedIsValid(node, index)) {
			return MakeNull(state);
		}
		return MakeShredded(state, node, index, row, 0);
	}

	//! A "STRUCT(typed_value, [untyped_value_index])" wrapper
	auto &typed_value = node.children[TYPED_VALUE_INDEX];
	const bool has_overlay = node.children.size() > 1;

	if (ShreddedIsValid(typed_value, index)) {
		//! The value is (at least partially) shredded. The untyped_value_index (overlay) is only relevant
		//! for OBJECTs (to merge leftover fields) - for primitives and ARRAYs we avoid reading it entirely,
		//! which is the hot path for fully-shredded values.
		if (!has_overlay || typed_value.logical_type.id() != LogicalTypeId::STRUCT) {
			return MakeShredded(state, typed_value, index, row, 0);
		}
		//! OBJECT: read the overlay - either NULL (fully shredded) or a 1-based index to the leftover object
		auto &untyped = node.children[UNTYPED_VALUE_INDEX];
		auto untyped_sel = untyped.unified.sel->get_index(index);
		uint32_t overlay = 0;
		if (untyped.unified.validity.RowIsValid(untyped_sel)) {
			overlay = untyped.unified.GetData<uint32_t>()[untyped_sel];
		}
		return MakeShredded(state, typed_value, index, row, overlay);
	}

	//! The typed value is absent - the value (if any) lives entirely in the unshredded component
	if (!has_overlay) {
		//! No unshredded value -> VARIANT_NULL
		return MakeNull(state);
	}
	auto &untyped = node.children[UNTYPED_VALUE_INDEX];
	auto untyped_sel = untyped.unified.sel->get_index(index);
	if (!untyped.unified.validity.RowIsValid(untyped_sel)) {
		//! NULL untyped value -> VARIANT_NULL
		return MakeNull(state);
	}
	auto overlay_value_index = untyped.unified.GetData<uint32_t>()[untyped_sel];
	if (overlay_value_index == 0) {
		//! 0 is reserved to indicate a missing value
		return MakeMissing(state);
	}
	//! The unshredded component stores 1-based value indices (0 == missing)
	return MakeUnshredded(state, row, overlay_value_index - 1);
}

//===--------------------------------------------------------------------===//
// Type resolution
//===--------------------------------------------------------------------===//
static VariantLogicalType ShreddedTypeId(const ShreddedVariantIterator &content, idx_t index) {
	auto &type = content.logical_type;
	switch (type.id()) {
	case LogicalTypeId::STRUCT:
		return VariantLogicalType::OBJECT;
	case LogicalTypeId::LIST:
		return VariantLogicalType::ARRAY;
	case LogicalTypeId::BOOLEAN:
		return content.unified.GetData<bool>()[content.unified.sel->get_index(index)] ? VariantLogicalType::BOOL_TRUE
		                                                                              : VariantLogicalType::BOOL_FALSE;
	case LogicalTypeId::TINYINT:
		return VariantLogicalType::INT8;
	case LogicalTypeId::SMALLINT:
		return VariantLogicalType::INT16;
	case LogicalTypeId::INTEGER:
		return VariantLogicalType::INT32;
	case LogicalTypeId::BIGINT:
		return VariantLogicalType::INT64;
	case LogicalTypeId::HUGEINT:
		return VariantLogicalType::INT128;
	case LogicalTypeId::UTINYINT:
		return VariantLogicalType::UINT8;
	case LogicalTypeId::USMALLINT:
		return VariantLogicalType::UINT16;
	case LogicalTypeId::UINTEGER:
		return VariantLogicalType::UINT32;
	case LogicalTypeId::UBIGINT:
		return VariantLogicalType::UINT64;
	case LogicalTypeId::UHUGEINT:
		return VariantLogicalType::UINT128;
	case LogicalTypeId::FLOAT:
		return VariantLogicalType::FLOAT;
	case LogicalTypeId::DOUBLE:
		return VariantLogicalType::DOUBLE;
	case LogicalTypeId::DECIMAL:
		return VariantLogicalType::DECIMAL;
	case LogicalTypeId::VARCHAR:
		return VariantLogicalType::VARCHAR;
	case LogicalTypeId::BLOB:
		return VariantLogicalType::BLOB;
	case LogicalTypeId::UUID:
		return VariantLogicalType::UUID;
	case LogicalTypeId::DATE:
		return VariantLogicalType::DATE;
	case LogicalTypeId::TIME:
		return VariantLogicalType::TIME_MICROS;
	case LogicalTypeId::TIME_NS:
		return VariantLogicalType::TIME_NANOS;
	case LogicalTypeId::TIME_TZ:
		return VariantLogicalType::TIME_MICROS_TZ;
	case LogicalTypeId::TIMESTAMP_SEC:
		return VariantLogicalType::TIMESTAMP_SEC;
	case LogicalTypeId::TIMESTAMP_MS:
		return VariantLogicalType::TIMESTAMP_MILIS;
	case LogicalTypeId::TIMESTAMP:
		return VariantLogicalType::TIMESTAMP_MICROS;
	case LogicalTypeId::TIMESTAMP_NS:
		return VariantLogicalType::TIMESTAMP_NANOS;
	case LogicalTypeId::TIMESTAMP_TZ:
		return VariantLogicalType::TIMESTAMP_MICROS_TZ;
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		return VariantLogicalType::TIMESTAMP_NANOS_TZ;
	case LogicalTypeId::INTERVAL:
		return VariantLogicalType::INTERVAL;
	case LogicalTypeId::BIGNUM:
		return VariantLogicalType::BIGNUM;
	case LogicalTypeId::BIT:
		return VariantLogicalType::BITSTRING;
	case LogicalTypeId::GEOMETRY:
		return VariantLogicalType::GEOMETRY;
	default:
		throw NotImplementedException("Shredded VARIANT type '%s' is not supported by VariantNode", type.ToString());
	}
}

VariantLogicalType VariantNode::GetTypeId() const {
	switch (kind) {
	case Kind::NULL_VALUE:
		return VariantLogicalType::VARIANT_NULL;
	case Kind::UNSHREDDED:
		return state->unshredded.GetTypeId(row, value_index);
	case Kind::SHREDDED:
		return ShreddedTypeId(*shredded_format, shredded_index);
	default:
		throw InternalException("VariantNode::GetTypeId called on a MISSING value");
	}
}

//===--------------------------------------------------------------------===//
// Primitive accessors
//===--------------------------------------------------------------------===//
const_data_ptr_t VariantNode::GetDataPointer() const {
	if (kind == Kind::UNSHREDDED) {
		auto &blob = state->unshredded.GetBlob(row);
		return const_data_ptr_cast(blob.GetData()) + state->unshredded.GetByteOffset(row, value_index);
	}
	D_ASSERT(kind == Kind::SHREDDED);
	auto &content = *shredded_format;
	auto type_size = GetTypeIdSize(content.unified.physical_type);
	return content.unified.data + content.unified.sel->get_index(shredded_index) * type_size;
}

string_t VariantNode::GetString() const {
	if (kind == Kind::UNSHREDDED) {
		return DecodeStringData(state->unshredded.GetBlob(row), state->unshredded.GetByteOffset(row, value_index));
	}
	D_ASSERT(kind == Kind::SHREDDED);
	auto &content = *shredded_format;
	return content.unified.GetData<string_t>()[content.unified.sel->get_index(shredded_index)];
}

VariantDecimalData VariantNode::GetDecimal() const {
	if (kind == Kind::UNSHREDDED) {
		return DecodeDecimalData(state->unshredded.GetBlob(row), state->unshredded.GetByteOffset(row, value_index));
	}
	D_ASSERT(kind == Kind::SHREDDED);
	auto &content = *shredded_format;
	auto &type = content.logical_type;
	auto width = DecimalType::GetWidth(type);
	auto scale = DecimalType::GetScale(type);
	auto type_size = GetTypeIdSize(content.unified.physical_type);
	auto value_ptr = content.unified.data + content.unified.sel->get_index(shredded_index) * type_size;
	return VariantDecimalData(width, scale, value_ptr);
}

//===--------------------------------------------------------------------===//
// Nested accessors
//===--------------------------------------------------------------------===//
VariantObjectIterator VariantNode::GetObjectChildren(VariantIterationOrder order) const {
	return VariantObjectIterator(*this, order);
}

VariantArrayIterator VariantNode::GetArrayChildren() const {
	return VariantArrayIterator(*this);
}

//===--------------------------------------------------------------------===//
// VariantArrayIterator
//===--------------------------------------------------------------------===//
VariantArrayIterator::VariantArrayIterator(const VariantNode &array)
    : state(*array.state), row(array.row), shredded(array.kind == VariantNode::Kind::SHREDDED) {
	if (!shredded) {
		auto &unshredded = state.get().unshredded;
		auto nested = DecodeNestedData(unshredded.GetBlob(row), unshredded.GetByteOffset(row, array.value_index));
		base = nested.children_idx;
		length = nested.child_count;
		return;
	}
	//! The array's elements live at flat positions (offset + i) in the list's child layer
	auto &content = *array.shredded_format;
	auto list_data = content.unified.GetData<list_entry_t>();
	auto &entry = list_data[content.unified.sel->get_index(array.shredded_index)];
	base = entry.offset;
	length = entry.length;
	element_node = content.children[0];
}

VariantNode VariantArrayIterator::operator[](idx_t i) const {
	auto &state_ref = state.get();
	if (shredded) {
		return VariantNode::ResolveShredded(state_ref, *element_node, base + i, row);
	}
	return VariantNode::MakeUnshredded(state_ref, row, state_ref.unshredded.GetValuesIndex(row, base + i));
}

//===--------------------------------------------------------------------===//
// VariantObjectIterator
//===--------------------------------------------------------------------===//
VariantObjectIterator::VariantObjectIterator(const VariantNode &object, VariantIterationOrder order)
    : state(*object.state), row(object.row), order(order), shredded(object.kind == VariantNode::Kind::SHREDDED) {
	auto &unshredded = state.get().unshredded;
	if (!shredded) {
		auto nested = DecodeNestedData(unshredded.GetBlob(row), unshredded.GetByteOffset(row, object.value_index));
		base = nested.children_idx;
		raw_count = nested.child_count;
	} else {
		//! Shredded object: the typed struct fields, followed by the leftover (overlay) object's fields
		content = object.shredded_format;
		shredded_index = object.shredded_index;
		typed_field_count = content->children.size();
		raw_count = typed_field_count;
		if (object.overlay_value_index != 0) {
			auto overlay_value_index = object.overlay_value_index - 1;
			auto nested = DecodeNestedData(unshredded.GetBlob(row), unshredded.GetByteOffset(row, overlay_value_index));
			overlay_base = nested.children_idx;
			raw_count += nested.child_count;
		}
	}

	if (order != VariantIterationOrder::LEXICOGRAPHIC) {
		return;
	}

	//! Materialize all (non-missing) entries and sort them by key up-front
	ordered_entries.reserve(raw_count);
	for (idx_t raw_pos = 0; raw_pos < raw_count; raw_pos++) {
		auto entry = RawEntry(raw_pos);
		if (entry.value.IsMissing()) {
			continue;
		}
		ordered_entries.push_back(entry);
	}
	std::sort(ordered_entries.begin(), ordered_entries.end(),
	          [](const VariantObjectEntry &a, const VariantObjectEntry &b) { return a.key < b.key; });
}

VariantObjectEntry VariantObjectIterator::RawEntry(idx_t raw_pos) const {
	auto &state_ref = state.get();
	auto &unshredded = state_ref.unshredded;
	if (!shredded) {
		auto child_idx = base + raw_pos;
		auto key_idx = unshredded.GetKeysIndex(row, child_idx);
		auto value_idx = unshredded.GetValuesIndex(row, child_idx);
		return VariantObjectEntry {unshredded.GetKey(row, key_idx),
		                           VariantNode::MakeUnshredded(state_ref, row, value_idx)};
	}
	if (raw_pos < typed_field_count) {
		//! A shredded (typed) field - struct fields preserve the (logical) index of the parent
		auto &name = StructType::GetChildTypes(content->logical_type)[raw_pos].first;
		return VariantObjectEntry {
		    string_t(name.c_str(), NumericCast<uint32_t>(name.size())),
		    VariantNode::ResolveShredded(state_ref, content->children[raw_pos], shredded_index, row)};
	}
	//! A leftover field from the overlay (unshredded) object
	auto child_idx = overlay_base + (raw_pos - typed_field_count);
	auto key_idx = unshredded.GetKeysIndex(row, child_idx);
	auto value_idx = unshredded.GetValuesIndex(row, child_idx);
	return VariantObjectEntry {unshredded.GetKey(row, key_idx), VariantNode::MakeUnshredded(state_ref, row, value_idx)};
}

void VariantObjectIterator::Iterator::Load() {
	if (parent.order == VariantIterationOrder::LEXICOGRAPHIC) {
		//! Iterate the pre-sorted entries (missing fields were already filtered out)
		if (pos < parent.ordered_entries.size()) {
			current = parent.ordered_entries[pos];
		}
		return;
	}
	//! INTERNAL order: lazily materialize, skipping missing entries (only shredded typed fields can be
	//! "missing", i.e. absent for this row)
	while (pos < parent.raw_count) {
		current = parent.RawEntry(pos);
		if (!current.value.IsMissing()) {
			return;
		}
		++pos;
	}
}

} // namespace duckdb
