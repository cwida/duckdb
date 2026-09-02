#include "duckdb/common/types/variant/parquet_variant_iterator.hpp"

#include "duckdb/common/types/variant/variant_builder.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/hugeint.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/common/types/interval.hpp"
#include "utf8proc_wrapper.hpp"
#include "duckdb/common/optional_idx.hpp"

#include <algorithm>
#include <cstring>
#include <cmath>
#include <set>
#include <type_traits>

namespace duckdb {

namespace {

//! Throw if reading 'size' bytes starting at 'ptr' would read past the end of the value buffer
void CheckBinaryRead(const_data_ptr_t ptr, idx_t size, const_data_ptr_t end) {
	if (ptr + size > end) {
		throw IOException("Data corruption detected, read of length_in_bytes (%d) would exceed buffer capacity", size);
	}
}

//! Read a little-endian unsigned integer of 'size' bytes (without advancing), bounds-checked against 'end'
idx_t ReadVarLE(idx_t size, const_data_ptr_t ptr, const_data_ptr_t end) {
	D_ASSERT(size <= sizeof(idx_t));
	CheckBinaryRead(ptr, size, end);
	idx_t result = 0;
	memcpy(&result, ptr, size);
	return result;
}

//! Read a fixed-width little-endian value, bounds-checked against 'end'
template <class T>
T LoadChecked(const_data_ptr_t ptr, const_data_ptr_t end) {
	CheckBinaryRead(ptr, sizeof(T), end);
	return Load<T>(ptr);
}

//! Lazy reader over a Spark variant-encoded OBJECT (the value starts at its header byte). All structural
//! reads are bounds-checked against 'end' (one past the end of the 'value' blob).
struct BinaryObjectReader {
	BinaryObjectReader(const VariantMetadata &metadata, const_data_ptr_t value_start, const_data_ptr_t end)
	    : metadata(metadata), end(end) {
		auto value_metadata = VariantValueMetadata::FromHeaderByte(value_start[0]);
		D_ASSERT(value_metadata.basic_type == VariantBasicType::OBJECT);
		field_id_size = value_metadata.field_id_size;
		field_offset_size = value_metadata.field_offset_size;
		auto data = value_start + 1;
		if (value_metadata.is_large) {
			count = LoadChecked<uint32_t>(data, end);
			data += sizeof(uint32_t);
		} else {
			count = LoadChecked<uint8_t>(data, end);
			data += sizeof(uint8_t);
		}
		field_ids = data;
		field_offsets = data + (count * field_id_size);
		values = field_offsets + (NumericCast<idx_t>(count + 1) * field_offset_size);
	}

	string_t Key(idx_t i) const {
		auto field_id = ReadVarLE(field_id_size, field_ids + (i * field_id_size), end);
		if (field_id >= metadata.strings.size()) {
			throw IOException("Corrupted VARIANT 'value' buffer");
		}
		auto &key = metadata.strings[field_id];
		return string_t(key.c_str(), NumericCast<uint32_t>(key.size()));
	}
	const_data_ptr_t Child(idx_t i) const {
		auto offset = ReadVarLE(field_offset_size, field_offsets + (i * field_offset_size), end);
		auto child = values + offset;
		//! The child's header byte must be readable
		CheckBinaryRead(child, 1, end);
		return child;
	}
	//! Compare keys by string so a wrong metadata sorted_strings flag cannot hide a field.
	optional_idx FindChild(const string &key) const {
		for (idx_t i = 0; i < count; i++) {
			auto child_key = Key(i);
			if (child_key.GetSize() == key.size() && memcmp(child_key.GetData(), key.data(), key.size()) == 0) {
				return optional_idx(i);
			}
		}
		return optional_idx();
	}

	const VariantMetadata &metadata;
	const_data_ptr_t end;
	const_data_ptr_t field_ids;
	const_data_ptr_t field_offsets;
	const_data_ptr_t values;
	uint32_t field_id_size;
	uint32_t field_offset_size;
	idx_t count;
};

//! Lazy reader over a Spark variant-encoded ARRAY (the value starts at its header byte). All structural
//! reads are bounds-checked against 'end' (one past the end of the 'value' blob).
struct BinaryArrayReader {
	BinaryArrayReader(const_data_ptr_t value_start, const_data_ptr_t end) : end(end) {
		auto value_metadata = VariantValueMetadata::FromHeaderByte(value_start[0]);
		D_ASSERT(value_metadata.basic_type == VariantBasicType::ARRAY);
		field_offset_size = value_metadata.field_offset_size;
		auto data = value_start + 1;
		if (value_metadata.is_large) {
			count = LoadChecked<uint32_t>(data, end);
			data += sizeof(uint32_t);
		} else {
			count = LoadChecked<uint8_t>(data, end);
			data += sizeof(uint8_t);
		}
		field_offsets = data;
		values = field_offsets + (NumericCast<idx_t>(count + 1) * field_offset_size);
	}

	const_data_ptr_t end;
	const_data_ptr_t field_offsets;
	const_data_ptr_t values;
	uint32_t field_offset_size;
	idx_t count;
};

//! The VariantLogicalType of a (valid) shredded leaf at logical position 'index'. Mirrors the writer's
//! type mapping; note BINARY is kept as a BLOB so the type is preserved (the base64 conversion happens
//! only when serializing the VARIANT to JSON).
VariantLogicalType ShreddedLeafTypeId(const ShreddedGroupView &view, idx_t index) {
	switch (view.typed_type.id()) {
	case LogicalTypeId::BOOLEAN: {
		auto leaf_index = view.leaf_format.sel->get_index(index);
		return UnifiedVectorFormat::GetData<bool>(view.leaf_format)[leaf_index] ? VariantLogicalType::BOOL_TRUE
		                                                                        : VariantLogicalType::BOOL_FALSE;
	}
	case LogicalTypeId::TINYINT:
		return VariantLogicalType::INT8;
	case LogicalTypeId::SMALLINT:
		return VariantLogicalType::INT16;
	case LogicalTypeId::INTEGER:
		return VariantLogicalType::INT32;
	case LogicalTypeId::BIGINT:
		return VariantLogicalType::INT64;
	case LogicalTypeId::FLOAT:
		return VariantLogicalType::FLOAT;
	case LogicalTypeId::DOUBLE:
		return VariantLogicalType::DOUBLE;
	case LogicalTypeId::DECIMAL:
		return VariantLogicalType::DECIMAL;
	case LogicalTypeId::DATE:
		return VariantLogicalType::DATE;
	case LogicalTypeId::TIME:
		return VariantLogicalType::TIME_MICROS;
	case LogicalTypeId::TIMESTAMP_TZ:
		return VariantLogicalType::TIMESTAMP_MICROS_TZ;
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		return VariantLogicalType::TIMESTAMP_NANOS_TZ;
	case LogicalTypeId::TIMESTAMP:
		return VariantLogicalType::TIMESTAMP_MICROS;
	case LogicalTypeId::TIMESTAMP_NS:
		return VariantLogicalType::TIMESTAMP_NANOS;
	case LogicalTypeId::BLOB:
		return VariantLogicalType::BLOB;
	case LogicalTypeId::VARCHAR:
		return VariantLogicalType::VARCHAR;
	case LogicalTypeId::UUID:
		return VariantLogicalType::UUID;
	default:
		throw NotImplementedException("Variant shredding on type: '%s' is not implemented", view.typed_type.ToString());
	}
}

//! The VariantLogicalType of a Spark variant-encoded value (the value starts at its header byte)
VariantLogicalType BinaryTypeId(const_data_ptr_t data) {
	auto value_metadata = VariantValueMetadata::FromHeaderByte(data[0]);
	switch (value_metadata.basic_type) {
	case VariantBasicType::SHORT_STRING:
		return VariantLogicalType::VARCHAR;
	case VariantBasicType::OBJECT:
		return VariantLogicalType::OBJECT;
	case VariantBasicType::ARRAY:
		return VariantLogicalType::ARRAY;
	case VariantBasicType::PRIMITIVE:
		break;
	default:
		throw InternalException("Unexpected VariantBasicType");
	}
	switch (value_metadata.primitive_type) {
	case VariantPrimitiveType::NULL_TYPE:
		return VariantLogicalType::VARIANT_NULL;
	case VariantPrimitiveType::BOOLEAN_TRUE:
		return VariantLogicalType::BOOL_TRUE;
	case VariantPrimitiveType::BOOLEAN_FALSE:
		return VariantLogicalType::BOOL_FALSE;
	case VariantPrimitiveType::INT8:
		return VariantLogicalType::INT8;
	case VariantPrimitiveType::INT16:
		return VariantLogicalType::INT16;
	case VariantPrimitiveType::INT32:
		return VariantLogicalType::INT32;
	case VariantPrimitiveType::INT64:
		return VariantLogicalType::INT64;
	case VariantPrimitiveType::DOUBLE:
		return VariantLogicalType::DOUBLE;
	case VariantPrimitiveType::FLOAT:
		return VariantLogicalType::FLOAT;
	case VariantPrimitiveType::DECIMAL4:
	case VariantPrimitiveType::DECIMAL8:
	case VariantPrimitiveType::DECIMAL16:
		return VariantLogicalType::DECIMAL;
	case VariantPrimitiveType::DATE:
		return VariantLogicalType::DATE;
	case VariantPrimitiveType::TIMESTAMP_MICROS:
		return VariantLogicalType::TIMESTAMP_MICROS_TZ;
	case VariantPrimitiveType::TIMESTAMP_NTZ_MICROS:
		return VariantLogicalType::TIMESTAMP_MICROS;
	case VariantPrimitiveType::BINARY:
		//! Keep the raw bytes as a BLOB so the type is preserved (base64 conversion happens at JSON time)
		return VariantLogicalType::BLOB;
	case VariantPrimitiveType::STRING:
		return VariantLogicalType::VARCHAR;
	case VariantPrimitiveType::TIME_NTZ_MICROS:
		return VariantLogicalType::TIME_MICROS;
	case VariantPrimitiveType::TIMESTAMP_NANOS:
		return VariantLogicalType::TIMESTAMP_NANOS_TZ;
	case VariantPrimitiveType::TIMESTAMP_NTZ_NANOS:
		return VariantLogicalType::TIMESTAMP_NANOS;
	case VariantPrimitiveType::UUID:
		return VariantLogicalType::UUID;
	default:
		throw NotImplementedException("Variant PrimitiveType (%d) is not supported",
		                              static_cast<uint8_t>(value_metadata.primitive_type));
	}
}

//! The implied precision of a decimal value: floor(log10(val)) + 1
template <class T>
uint32_t ComputeDecimalWidth(T value) {
	if (value == 0) {
		return 1;
	}
	// |min| might not fit in signed T; use unsigned wrap.
	using unsigned_t = std::make_unsigned_t<T>;
	unsigned_t abs_val = 0;
	if (value < 0) {
		abs_val = static_cast<unsigned_t>(0) - static_cast<unsigned_t>(value);
	} else {
		abs_val = static_cast<unsigned_t>(value);
	}
	return static_cast<uint32_t>(floor(log10(static_cast<double>(abs_val))) + 1);
}

//! Whether T is a physical storage type a DECIMAL value can be re-encoded into
template <class T>
struct IsDecimalStorage : std::false_type {};
template <>
struct IsDecimalStorage<int16_t> : std::true_type {};
template <>
struct IsDecimalStorage<int32_t> : std::true_type {};
template <>
struct IsDecimalStorage<int64_t> : std::true_type {};
template <>
struct IsDecimalStorage<hugeint_t> : std::true_type {};

template <class T, class SRC>
T CastDecimalValue(SRC value) {
	if constexpr (std::is_same<T, hugeint_t>::value) {
		return hugeint_t(value);
	} else if constexpr (std::is_same<SRC, hugeint_t>::value) {
		//! Only reachable for DECIMAL16, which always re-encodes to hugeint (the branch above)
		return Hugeint::Cast<T>(value);
	} else {
		return NumericCast<T>(value);
	}
}

//! Read the (Parquet UUID-ordered) value as T. UUID is always read as hugeint_t.
template <class T>
T ReadBinaryUUID(const_data_ptr_t payload) {
	if constexpr (std::is_same<T, hugeint_t>::value) {
		return BaseUUID::FromBlob(payload);
	} else {
		throw InternalException("Variant UUID must be read as hugeint_t");
	}
}

//! Read the decimal value (re-encoded as T = the physical type implied by the width). 'payload' points at
//! the scale byte. T must be one of int16/int32/int64/hugeint (see VariantDecimalPhysicalType).
template <class T>
T ReadBinaryDecimalValue(VariantPrimitiveType primitive_type, const_data_ptr_t payload) {
	if constexpr (IsDecimalStorage<T>::value) {
		auto value_data = payload + sizeof(uint8_t); // skip the scale byte
		switch (primitive_type) {
		case VariantPrimitiveType::DECIMAL4:
			return CastDecimalValue<T>(Load<int32_t>(value_data));
		case VariantPrimitiveType::DECIMAL8:
			return CastDecimalValue<T>(Load<int64_t>(value_data));
		default: {
			D_ASSERT(primitive_type == VariantPrimitiveType::DECIMAL16);
			hugeint_t value;
			value.lower = Load<uint64_t>(value_data);
			value.upper = Load<int64_t>(value_data + sizeof(uint64_t));
			return CastDecimalValue<T>(value);
		}
		}
	} else {
		throw InternalException("Variant DECIMAL must be read as int16/int32/int64/hugeint");
	}
}

} // namespace

//===--------------------------------------------------------------------===//
// ShreddedGroupView
//===--------------------------------------------------------------------===//
void ShreddedGroupView::Build(Vector &group) {
	D_ASSERT(group.GetType().id() == LogicalTypeId::STRUCT);
	auto &entries = StructVector::GetEntries(group);
	auto &child_types = StructType::GetChildTypes(group.GetType());
	D_ASSERT(entries.size() == child_types.size());

	//! From the spec: the Parquet columns storing variant metadata and values must be accessed by name
	optional_ptr<Vector> value_vec;
	optional_ptr<Vector> typed_vec;
	for (idx_t i = 0; i < entries.size(); i++) {
		auto &name = child_types[i].first;
		if (name == "value") {
			value_vec = entries[i];
		} else if (name == "typed_value") {
			typed_vec = entries[i];
		} else {
			throw InvalidInputException("Variant group can only contain 'value'/'typed_value', not: %s", name);
		}
	}
	if (!value_vec) {
		throw InvalidInputException("Required column 'value' not found in Variant group");
	}

	value = make_uniq<VectorIterator<string_t>>(*value_vec);

	if (!typed_vec) {
		has_typed_value = false;
		return;
	}
	has_typed_value = true;
	typed_type = typed_vec->GetType();
	typed_value_vec = typed_vec;

	switch (typed_type.id()) {
	case LogicalTypeId::STRUCT: {
		kind = ParquetGroupKind::OBJECT;
		typed_validity = make_uniq<VectorValidityIterator>(*typed_vec);
		auto &fields_meta = StructType::GetChildTypes(typed_type);
		auto &field_entries = StructVector::GetEntries(*typed_vec);
		for (idx_t i = 0; i < field_entries.size(); i++) {
			field_names.push_back(fields_meta[i].first.GetIdentifierName());
			auto field_view = make_uniq<ShreddedGroupView>();
			field_view->Build(field_entries[i]);
			fields.push_back(std::move(field_view));
		}
		break;
	}
	case LogicalTypeId::LIST: {
		kind = ParquetGroupKind::ARRAY;
		list = make_uniq<VectorIterator<list_entry_t>>(*typed_vec);
		element = make_uniq<ShreddedGroupView>();
		element->Build(ListVector::GetChildMutable(*typed_vec));
		break;
	}
	default:
		kind = ParquetGroupKind::LEAF;
		typed_vec->ToUnifiedFormat(leaf_format);
		break;
	}
}

//===--------------------------------------------------------------------===//
// ParquetVariantIterator
//===--------------------------------------------------------------------===//
ParquetVariantIterator::ParquetVariantIterator(Vector &metadata_vec, Vector &group) : metadata(metadata_vec) {
	root_view.Build(group);
}

ParquetVariantIterator::ParquetVariantIterator(Vector &metadata_vec) : metadata(metadata_vec) {
}

void ParquetVariantIterator::BeginRow(idx_t row) {
	current_row = row;
	current_metadata.reset();
}

const VariantMetadata &ParquetVariantIterator::GetMetadata() const {
	if (!current_metadata) {
		current_metadata = make_uniq<VariantMetadata>(metadata[current_row].GetValueUnsafe());
	}
	return *current_metadata;
}

ParquetVariantNode ParquetVariantIterator::ResolveGroup(const ShreddedGroupView &view, idx_t index) const {
	if (view.has_typed_value) {
		bool typed_valid = false;
		switch (view.kind) {
		case ParquetGroupKind::LEAF:
			typed_valid = view.leaf_format.validity.RowIsValid(view.leaf_format.sel->get_index(index));
			break;
		case ParquetGroupKind::ARRAY:
			typed_valid = (*view.list)[index].IsValid();
			break;
		case ParquetGroupKind::OBJECT:
			typed_valid = view.typed_validity->IsValid(index);
			break;
		}
		if (typed_valid) {
			if (view.kind == ParquetGroupKind::OBJECT) {
				//! (Partially) shredded object - the binary 'value', if present, holds the leftover fields
				const_data_ptr_t overlay = nullptr;
				const_data_ptr_t overlay_end = nullptr;
				auto value_entry = (*view.value)[index];
				if (value_entry.IsValid()) {
					auto &overlay_blob = value_entry.GetValueUnsafe();
					auto overlay_data = const_data_ptr_cast(overlay_blob.GetData());
					overlay_end = overlay_data + overlay_blob.GetSize();
					CheckBinaryRead(overlay_data, 1, overlay_end);
					if (VariantValueMetadata::FromHeaderByte(overlay_data[0]).basic_type != VariantBasicType::OBJECT) {
						throw InvalidInputException(
						    "Partially shredded objects have to encode Object Variants in the 'value'");
					}
					overlay = overlay_data;
				}
				return ParquetVariantNode::MakeShredded(*this, view, index, overlay, overlay_end);
			}
			//! LEAF or ARRAY - the binary 'value' is irrelevant (a leaf is never partially shredded)
			return ParquetVariantNode::MakeShredded(*this, view, index);
		}
	}

	//! No (valid) shredded value - fall back to the binary 'value'
	auto value_entry = (*view.value)[index];
	if (value_entry.IsValid()) {
		auto &value_blob = value_entry.GetValueUnsafe();
		auto data = const_data_ptr_cast(value_blob.GetData());
		auto end = data + value_blob.GetSize();
		CheckBinaryRead(data, 1, end);
		if (view.has_typed_value && view.kind == ParquetGroupKind::OBJECT &&
		    VariantValueMetadata::FromHeaderByte(data[0]).basic_type == VariantBasicType::OBJECT) {
			throw InvalidInputException(
			    "When 'typed_value' for a shredded Object is NULL, 'value' can not contain an Object value");
		}
		return ParquetVariantNode::MakeBinary(*this, data, end);
	}
	return ParquetVariantNode::MakeMissing();
}

ParquetVariantNode ParquetVariantIterator::Root(idx_t row) const {
	auto root = ResolveGroup(root_view, row);
	//! A root value is never "missing" - treat any such case as a SQL NULL
	return root.IsMissing() ? ParquetVariantNode::MakeNull() : root;
}

ParquetVariantNode ParquetVariantNode::FindObjectChild(const string &key) const {
	if (IsNull() || IsMissing()) {
		return MakeMissing();
	}
	if (GetTypeId() != VariantLogicalType::OBJECT) {
		return MakeMissing();
	}
	if (kind == Kind::BINARY) {
		BinaryObjectReader reader(state->GetMetadata(), binary, binary_end);
		auto child_idx = reader.FindChild(key);
		if (!child_idx.IsValid()) {
			return MakeMissing();
		}
		return MakeBinary(*state, reader.Child(child_idx.GetIndex()), binary_end);
	}
	for (auto &entry : GetObjectChildren(VariantIterationOrder::INTERNAL)) {
		auto &entry_key = entry.key;
		if (entry_key.GetSize() == key.size() && memcmp(entry_key.GetData(), key.data(), key.size()) == 0) {
			return entry.value;
		}
	}
	return MakeMissing();
}

ParquetVariantNode ParquetVariantIterator::BinaryRoot() const {
	//! The metadata and the value share the same blob: the value bytes start right after the metadata
	auto &variant_metadata = GetMetadata();
	auto blob_start = const_data_ptr_cast(variant_metadata.metadata.GetData());
	auto blob_end = blob_start + variant_metadata.metadata.GetSize();
	auto value_start = blob_start + variant_metadata.total_size;
	//! The value's header byte must be readable
	CheckBinaryRead(value_start, 1, blob_end);
	return ParquetVariantNode::MakeBinary(*this, value_start, blob_end);
}

//===--------------------------------------------------------------------===//
// ParquetVariantNode
//===--------------------------------------------------------------------===//
VariantLogicalType ParquetVariantNode::GetTypeId() const {
	switch (kind) {
	case Kind::NULL_VALUE:
		return VariantLogicalType::VARIANT_NULL;
	case Kind::SHREDDED:
		switch (view->kind) {
		case ParquetGroupKind::OBJECT:
			return VariantLogicalType::OBJECT;
		case ParquetGroupKind::ARRAY:
			return VariantLogicalType::ARRAY;
		default:
			return ShreddedLeafTypeId(*view, index);
		}
	case Kind::BINARY:
		return BinaryTypeId(binary);
	default:
		throw InternalException("ParquetVariantNode::GetTypeId on a MISSING value");
	}
}

template <class T>
T ParquetVariantNode::GetData() const {
	if (kind == Kind::SHREDDED) {
		return UnifiedVectorFormat::GetData<T>(view->leaf_format)[view->leaf_format.sel->get_index(index)];
	}
	D_ASSERT(kind == Kind::BINARY);
	auto value_metadata = VariantValueMetadata::FromHeaderByte(binary[0]);
	auto payload = binary + 1;
	switch (value_metadata.primitive_type) {
	case VariantPrimitiveType::UUID:
		CheckBinaryRead(payload, sizeof(hugeint_t), binary_end);
		return ReadBinaryUUID<T>(payload);
	case VariantPrimitiveType::DECIMAL4:
		CheckBinaryRead(payload, sizeof(uint8_t) + sizeof(int32_t), binary_end);
		return ReadBinaryDecimalValue<T>(value_metadata.primitive_type, payload);
	case VariantPrimitiveType::DECIMAL8:
		CheckBinaryRead(payload, sizeof(uint8_t) + sizeof(int64_t), binary_end);
		return ReadBinaryDecimalValue<T>(value_metadata.primitive_type, payload);
	case VariantPrimitiveType::DECIMAL16:
		CheckBinaryRead(payload, sizeof(uint8_t) + sizeof(hugeint_t), binary_end);
		return ReadBinaryDecimalValue<T>(value_metadata.primitive_type, payload);
	case VariantPrimitiveType::TIME_NTZ_MICROS: {
		auto micros = LoadChecked<int64_t>(payload, binary_end);
		if (micros < 0 || micros > Interval::MICROS_PER_DAY) {
			throw IOException("Corrupted VARIANT 'value' buffer, TIME value (%d) is out of range", micros);
		}
		return LoadChecked<T>(payload, binary_end);
	}
	default:
		//! Fixed-width primitives are stored in the canonical little-endian layout
		return LoadChecked<T>(payload, binary_end);
	}
}

string_t ParquetVariantNode::GetString() const {
	if (kind == Kind::SHREDDED) {
		auto str = UnifiedVectorFormat::GetData<string_t>(view->leaf_format)[view->leaf_format.sel->get_index(index)];
		if (view->typed_type.id() == LogicalTypeId::BLOB) {
			//! Keep the raw bytes - the value is emitted as a BLOB (base64 conversion happens at JSON time)
			return str;
		}
		if (!Utf8Proc::IsValid(str.GetData(), str.GetSize())) {
			throw IOException("Can't decode Variant string, it isn't valid UTF8");
		}
		return str;
	}
	D_ASSERT(kind == Kind::BINARY);
	auto value_metadata = VariantValueMetadata::FromHeaderByte(binary[0]);
	auto payload = binary + 1;
	if (value_metadata.basic_type == VariantBasicType::SHORT_STRING) {
		auto string_data = const_char_ptr_cast(payload);
		CheckBinaryRead(payload, value_metadata.string_size, binary_end);
		if (!Utf8Proc::IsValid(string_data, value_metadata.string_size)) {
			throw IOException("Can't decode Variant short-string, string isn't valid UTF8");
		}
		return string_t(string_data, value_metadata.string_size);
	}
	auto size = LoadChecked<uint32_t>(payload, binary_end);
	auto string_data = const_char_ptr_cast(payload + sizeof(uint32_t));
	CheckBinaryRead(payload + sizeof(uint32_t), size, binary_end);
	if (value_metadata.primitive_type == VariantPrimitiveType::BINARY) {
		//! Keep the raw bytes - the value is emitted as a BLOB (base64 conversion happens at JSON time)
		return string_t(string_data, size);
	}
	if (!Utf8Proc::IsValid(string_data, size)) {
		throw IOException("Can't decode Variant string, it isn't valid UTF8");
	}
	return string_t(string_data, size);
}

VariantDecimalProperties ParquetVariantNode::GetDecimalProperties() const {
	if (kind == Kind::SHREDDED) {
		uint8_t width;
		uint8_t scale;
		view->typed_type.GetDecimalProperties(width, scale);
		return VariantDecimalProperties(width, scale);
	}
	D_ASSERT(kind == Kind::BINARY);
	auto value_metadata = VariantValueMetadata::FromHeaderByte(binary[0]);
	auto payload = binary + 1;
	uint8_t scale = LoadChecked<uint8_t>(payload, binary_end);
	if (scale > Decimal::MAX_WIDTH_DECIMAL) {
		throw IOException("Corrupted VARIANT 'value' buffer, decimal scale %d exceeds the maximum of %d", scale,
		                  Decimal::MAX_WIDTH_DECIMAL);
	}
	auto value_data = payload + sizeof(uint8_t);
	uint32_t width;
	switch (value_metadata.primitive_type) {
	case VariantPrimitiveType::DECIMAL4:
		width = ComputeDecimalWidth<int32_t>(LoadChecked<int32_t>(value_data, binary_end));
		break;
	case VariantPrimitiveType::DECIMAL8:
		width = ComputeDecimalWidth<int64_t>(LoadChecked<int64_t>(value_data, binary_end));
		break;
	default: {
		D_ASSERT(value_metadata.primitive_type == VariantPrimitiveType::DECIMAL16);
		CheckBinaryRead(value_data, sizeof(hugeint_t), binary_end);
		hugeint_t value;
		value.lower = Load<uint64_t>(value_data);
		value.upper = Load<int64_t>(value_data + sizeof(uint64_t));
		auto &limit = Hugeint::POWERS_OF_TEN[Decimal::MAX_WIDTH_DECIMAL];
		if (value >= limit || value <= -limit) {
			throw IOException("Corrupted VARIANT 'value' buffer, DECIMAL16 value exceeds the maximum width of %d",
			                  Decimal::MAX_WIDTH_DECIMAL);
		}
		width = Decimal::MAX_WIDTH_DECIMAL;
		break;
	}
	}
	//! The scale can exceed the digits of the unscaled value (e.g. 0.005), the width must cover it
	return VariantDecimalProperties(MaxValue<uint32_t>(width, scale), scale);
}

ParquetObjectIterator ParquetVariantNode::GetObjectChildren(VariantIterationOrder order) const {
	(void)order;
	if (kind == Kind::SHREDDED) {
		return ParquetObjectIterator(*state, *view, index, binary, binary_end);
	}
	D_ASSERT(kind == Kind::BINARY);
	return ParquetObjectIterator(*state, state->GetMetadata(), binary, binary_end);
}

ParquetArrayIterator ParquetVariantNode::GetArrayChildren() const {
	if (kind == Kind::SHREDDED) {
		return ParquetArrayIterator(*state, *view, index);
	}
	D_ASSERT(kind == Kind::BINARY);
	return ParquetArrayIterator(*state, state->GetMetadata(), binary, binary_end);
}

//===--------------------------------------------------------------------===//
// ParquetObjectIterator
//===--------------------------------------------------------------------===//
void ParquetObjectIterator::Finalize() {
	std::sort(ordered_entries.begin(), ordered_entries.end(),
	          [](const ParquetObjectEntry &a, const ParquetObjectEntry &b) { return a.key < b.key; });
	// the keys of a Variant object have to be unique
	for (idx_t i = 1; i < ordered_entries.size(); i++) {
		if (ordered_entries[i].key == ordered_entries[i - 1].key) {
			throw InvalidInputException("Invalid VARIANT: object contains the key \"%s\" more than once",
			                            ordered_entries[i].key.GetString());
		}
	}
}

ParquetObjectIterator::ParquetObjectIterator(const ParquetVariantIterator &state, const ShreddedGroupView &view,
                                             idx_t index, const_data_ptr_t overlay, const_data_ptr_t overlay_end) {
	//! Typed (shredded) fields - skipping the ones that are missing for this row
	std::set<string> typed_keys;
	for (idx_t i = 0; i < view.fields.size(); i++) {
		auto node = state.ResolveGroup(*view.fields[i], index);
		if (node.IsMissing()) {
			continue;
		}
		auto &name = view.field_names[i];
		typed_keys.insert(name);
		ordered_entries.push_back(
		    ParquetObjectEntry {string_t(name.c_str(), NumericCast<uint32_t>(name.size())), node});
	}
	//! Leftover (overlay) fields from the binary 'value' - typed fields win on key collisions
	if (overlay) {
		BinaryObjectReader reader(state.GetMetadata(), overlay, overlay_end);
		for (idx_t i = 0; i < reader.count; i++) {
			auto key = reader.Key(i);
			if (typed_keys.count(key.GetString())) {
				continue;
			}
			ordered_entries.push_back(
			    ParquetObjectEntry {key, ParquetVariantNode::MakeBinary(state, reader.Child(i), overlay_end)});
		}
	}
	Finalize();
}

ParquetObjectIterator::ParquetObjectIterator(const ParquetVariantIterator &state, const VariantMetadata &metadata,
                                             const_data_ptr_t data, const_data_ptr_t end) {
	BinaryObjectReader reader(metadata, data, end);
	for (idx_t i = 0; i < reader.count; i++) {
		ordered_entries.push_back(
		    ParquetObjectEntry {reader.Key(i), ParquetVariantNode::MakeBinary(state, reader.Child(i), end)});
	}
	Finalize();
}

//===--------------------------------------------------------------------===//
// ParquetArrayIterator
//===--------------------------------------------------------------------===//
ParquetArrayIterator::ParquetArrayIterator(const ParquetVariantIterator &state, const ShreddedGroupView &view,
                                           idx_t index)
    : state(state), shredded(true), element(view.element.get()) {
	auto entry = (*view.list)[index].GetValueUnsafe();
	base = entry.offset;
	length = entry.length;
}

ParquetArrayIterator::ParquetArrayIterator(const ParquetVariantIterator &state, const VariantMetadata &metadata,
                                           const_data_ptr_t data, const_data_ptr_t end)
    : state(state), shredded(false) {
	(void)metadata;
	BinaryArrayReader reader(data, end);
	length = reader.count;
	field_offsets = reader.field_offsets;
	values = reader.values;
	binary_end = end;
	field_offset_size = reader.field_offset_size;
}

ParquetVariantNode ParquetArrayIterator::operator[](idx_t i) const {
	if (shredded) {
		return state.get().ResolveGroup(*element, base + i);
	}
	auto offset = ReadVarLE(field_offset_size, field_offsets + (i * field_offset_size), binary_end);
	auto child = values + offset;
	//! The child's header byte must be readable
	CheckBinaryRead(child, 1, binary_end);
	return ParquetVariantNode::MakeBinary(state.get(), child, binary_end);
}

void ParquetVariantIterator::EmitBinary(const_data_ptr_t data, const_data_ptr_t end, VariantBuilder &builder) const {
	EmitIterator(ParquetVariantNode::MakeBinary(*this, data, end), builder);
}

namespace {

//! BuildVariant source over binary Variant blobs (each row being the metadata followed by the value)
struct ParquetBinaryVariantSource {
	ParquetBinaryVariantSource(ParquetVariantIterator &iterator, Vector &blob) : iterator(iterator) {
		blob.ToUnifiedFormat(blob_format);
	}

	bool Emit(idx_t row, VariantBuilder &builder) {
		if (!blob_format.validity.RowIsValid(blob_format.sel->get_index(row))) {
			return true;
		}
		iterator.BeginRow(row);
		EmitIterator(iterator.BinaryRoot(), builder);
		return false;
	}

	ParquetVariantIterator &iterator;
	UnifiedVectorFormat blob_format;
};

} // namespace

void ParquetVariantConversion::ConvertBinary(Vector &metadata_and_value, Vector &result, idx_t count) {
	ParquetVariantIterator iterator(metadata_and_value);
	ParquetBinaryVariantSource source(iterator, metadata_and_value);
	BuildVariant(source, count, result);
}

namespace {

ParquetVariantNode WalkExtractPath(const ParquetVariantNode &root, const vector<VariantPathComponent> &path) {
	auto current = root;
	for (auto &component : path) {
		if (component.lookup_mode != VariantChildLookupMode::BY_KEY) {
			return ParquetVariantNode::MakeMissing();
		}
		current = current.FindObjectChild(component.key);
	}
	return current;
}

void EmitSparsePath(const vector<VariantPathComponent> &path, idx_t depth, const ParquetVariantNode &leaf,
                    VariantBuilder &builder) {
	if (depth == path.size()) {
		EmitIterator(leaf, builder);
		return;
	}
	builder.EmitObject(
	    1, [&](idx_t) { return string_t(path[depth].key); },
	    [&](idx_t) { EmitSparsePath(path, depth + 1, leaf, builder); });
}

struct ParquetPathVariantSource {
	ParquetPathVariantSource(ParquetVariantIterator &iterator, const vector<VariantPathComponent> &path)
	    : iterator(iterator), path(path) {
	}

	bool Emit(idx_t row, VariantBuilder &builder) {
		iterator.BeginRow(row);
		auto root = iterator.Root(row);
		if (root.IsNull()) {
			return true;
		}
		auto leaf = WalkExtractPath(root, path);
		if (leaf.IsMissing()) {
			return true;
		}
		EmitSparsePath(path, 0, leaf, builder);
		return false;
	}

	ParquetVariantIterator &iterator;
	const vector<VariantPathComponent> &path;
};

} // namespace

void ParquetVariantConversion::ConvertPath(Vector &metadata, Vector &group, Vector &result, idx_t count,
                                           const vector<VariantPathComponent> &path) {
	D_ASSERT(!path.empty());
	ParquetVariantIterator iterator(metadata, group);
	ParquetPathVariantSource source(iterator, path);
	BuildVariant(source, count, result);
}

} // namespace duckdb
