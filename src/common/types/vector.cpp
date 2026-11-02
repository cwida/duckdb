#include "duckdb/common/vector/array_vector.hpp"
#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/vector/dictionary_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/sequence_vector.hpp"
#include "duckdb/common/vector/shredded_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/assert.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/fsst.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/types/null_value.hpp"
#include "duckdb/common/types/sel_cache.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector_cache.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/enums/debug_verification_mode.hpp"
#include "duckdb/common/types/geometry.hpp"
#include "duckdb/main/config.hpp"

namespace duckdb {

enum class VectorConstructorAction { REFERENCE_VECTOR };

Vector::Vector(LogicalType type_p, buffer_ptr<VectorBuffer> buffer_p)
    : type(std::move(type_p)), buffer(std::move(buffer_p)) {
}

Vector::Vector(LogicalType type_p, idx_t capacity, VectorDataInitialization initialize) : type(std::move(type_p)) {
	Initialize(initialize, capacity);
}

Vector::Vector(LogicalType type_p, data_ptr_t dataptr, idx_t count) : type(std::move(type_p)) {
	if (!dataptr) {
		return;
	}
	if (!type.IsValid()) {
		throw InternalException("Cannot create a vector of type INVALID!");
	}
	if (type.IsNested()) {
		throw InternalException("Cannot create a nested vector from a single data pointer");
	}
	if (type.InternalType() == PhysicalType::VARCHAR) {
		buffer = make_buffer<VectorStringBuffer>(dataptr, count_t(count));
	} else {
		auto type_size = GetTypeIdSize(type.InternalType());
		buffer = make_buffer<StandardVectorBuffer>(dataptr, count_t(count), type_size);
	}
}

Vector::Vector(const VectorCache &cache) : type(cache.GetType()) {
	ResetFromCache(cache);
}

Vector::Vector(const Vector &other, VectorConstructorAction) : type(other.type) {
	Reference(other);
}

Vector::Vector(const Vector &other, const SelectionVector &sel, idx_t count) : type(other.type) {
	Slice(other, sel, count);
}

Vector::Vector(const Vector &other, idx_t offset, idx_t end) : type(other.type) {
	Slice(other, offset, end);
}

Vector::Vector(const Value &value, count_t count) : type(value.type()) {
	Reference(value, count);
}

Vector::Vector(Vector &&other) noexcept : type(std::move(other.type)), buffer(std::move(other.buffer)) {
}

idx_t Vector::size() const {
	if (!buffer) {
		return 0;
	}
	return buffer->Size();
}

void Vector::CheckCapacity(idx_t capacity) const {
	if (!buffer) {
		// no buffer - we accept any capacity
		return;
	}
	if (GetVectorType() == VectorType::CONSTANT_VECTOR) {
		// constant vectors can fit any
		return;
	}
	idx_t buffer_capacity = buffer->Capacity();
	if (capacity > buffer_capacity) {
		throw InternalException("Vector::CheckCapacity - capacity %d exceeds buffer capacity %d", capacity,
		                        buffer_capacity);
	}
}

Vector Vector::Ref(const Vector &other) {
	return Vector(other, VectorConstructorAction::REFERENCE_VECTOR);
}

void Vector::Reference(const Value &value, count_t count) {
	ConstantVector::Reference(*this, value, count);
}

void Vector::Reference(const Vector &other) {
	if (other.GetType().id() != GetType().id()) {
		throw InternalException("Vector::Reference used on vector of different type (source %s referenced %s)",
		                        GetType(), other.GetType());
	}
	D_ASSERT(other.GetType() == GetType());
	ConstReference(other);
}

void Vector::ReferenceAndSetType(const Vector &other) {
	type = other.GetType();
	Reference(other);
}

void CheckTypeIsReinterpretable(const LogicalType &a, const LogicalType &b) {
	if (DBConfigOptions::global_verification_mode != DebugVerificationMode::VERIFY_VECTORS) {
		return;
	}
	bool left_is_nested = a.IsNested();
	bool right_is_nested = b.IsNested();
	if (left_is_nested != right_is_nested) {
		throw InternalException("Vector::Reinterpret (%s -> %s) - nested mismatch in reinterpret - either both need to "
		                        "be nested or neither should be nested",
		                        a, b);
	}
	auto left_internal = a.InternalType();
	auto right_internal = b.InternalType();
	if (!left_is_nested) {
		// non-nested types - type size should be identical
		if (GetTypeIdSize(left_internal) != GetTypeIdSize(right_internal)) {
			throw InternalException(
			    "Vector::Reinterpret (%s -> %s) - attempting to reinterpret between types with different type sizes", a,
			    b);
		}
		return;
	}
	if (left_internal != right_internal) {
		throw InternalException(
		    "Vector::Reinterpret (%s -> %s) - attempting to reinterpret between different nested types", a, b);
	}
	// recurse into children
	switch (left_internal) {
	case PhysicalType::STRUCT: {
		auto &left_child_types = StructType::GetChildTypes(a);
		auto &right_child_types = StructType::GetChildTypes(b);
		if (left_child_types.size() != right_child_types.size()) {
			throw InternalException(
			    "Vector::Reinterpret (%s -> %s) - attempting to reinterpret between struct types of different sizes", a,
			    b);
		}
		for (idx_t child_idx = 0; child_idx < left_child_types.size(); ++child_idx) {
			CheckTypeIsReinterpretable(left_child_types[child_idx].second, right_child_types[child_idx].second);
		}
		break;
	}
	case PhysicalType::LIST: {
		auto &left_child_type = ListType::GetChildType(a);
		auto &right_child_type = ListType::GetChildType(b);
		CheckTypeIsReinterpretable(left_child_type, right_child_type);
		break;
	}
	case PhysicalType::ARRAY: {
		auto &left_child_type = ArrayType::GetChildType(a);
		auto &right_child_type = ArrayType::GetChildType(b);
		CheckTypeIsReinterpretable(left_child_type, right_child_type);
		break;
	}
	default:
		throw InternalException("Unsupported nested type in CheckTypeIsReinterpretable");
	}
}

void Vector::Reinterpret(const Vector &other) {
	auto &this_type = GetType();
	auto &other_type = other.GetType();
	if (DBConfigOptions::global_verification_mode == DebugVerificationMode::VERIFY_VECTORS) {
		CheckTypeIsReinterpretable(this_type, other_type);
	}
	ConstReference(other);
	if (GetVectorType() == VectorType::DICTIONARY_VECTOR && other_type != this_type) {
		Vector new_vector(this_type, nullptr);
		new_vector.Reinterpret(DictionaryVector::Child(other));
		auto &old_dict = buffer->Cast<DictionaryBuffer>();
		auto new_entry = make_shared_ptr<DictionaryEntry>(std::move(new_vector));
		// reinterpret re-mints the entry; the id and global flag are one contract and must survive together
		new_entry->id = old_dict.GetEntry().id;
		new_entry->global_dictionary = old_dict.GetEntry().global_dictionary;
		buffer = make_buffer<DictionaryBuffer>(old_dict.GetSelVector(), old_dict.Capacity(), std::move(new_entry));
	}
}

void Vector::ConstReference(const Vector &other) const {
	AssignSharedPointer(buffer, other.buffer);
}

void Vector::ResetFromCache(const VectorCache &cache) {
	cache.ResetFromCache(*this);
}

void Vector::Slice(const Vector &other, idx_t offset, idx_t end) {
	buffer = other.buffer->Slice(GetType(), offset, end);
	if (!buffer) {
		buffer = other.buffer;
	}
}

void Vector::Slice(const Vector &other, const SelectionVector &sel, idx_t count) {
	Reference(other);
	Slice(sel, count);
}

void Vector::Slice(const SelectionVector &sel, idx_t count) {
	if (!sel.IsSet() && count == size()) {
		// no-op: no selection vector, and the requested count matches the current size
		return;
	}
	if (!buffer) {
		return;
	}
	auto new_buffer = buffer->Slice(GetType(), sel, count);
	if (new_buffer) {
		buffer = std::move(new_buffer);
	}
	FlatVector::SetSize(*this, count_t(count));
}

void Vector::Slice(const SelectionVector &sel, idx_t count, SelCache &cache) {
	if (!sel.IsSet() && count == size()) {
		return;
	}
	if (!buffer) {
		return;
	}
	auto new_buffer = buffer->SliceWithCache(cache, GetType(), sel, count);
	if (new_buffer) {
		buffer = std::move(new_buffer);
	}
	FlatVector::SetSize(*this, count_t(count));
}

void Vector::Dictionary(idx_t dictionary_size, const SelectionVector &sel, idx_t count) {
	if (dictionary_size != size()) {
		throw InternalException("Vector::Dictionary called with mismatching dictionary size");
	}
	Slice(sel, count);
}

void Vector::Dictionary(const Vector &dict, idx_t dictionary_size, const SelectionVector &sel, idx_t count) {
	if (dict.size() != dictionary_size) {
		throw InternalException("Vector::Dictionary called with mismatching dictionary size");
	}
	Reference(dict);
	Dictionary(dictionary_size, sel, count);
}

void Vector::Dictionary(buffer_ptr<DictionaryEntry> reusable_dict, const SelectionVector &sel, idx_t sel_count) {
	if (type.InternalType() == PhysicalType::STRUCT) {
		throw InternalException("Struct vectors cannot be dictionaries");
	}
	D_ASSERT(type == reusable_dict->data.GetType());
	buffer = make_buffer<DictionaryBuffer>(sel, sel_count, std::move(reusable_dict));
}

void Vector::Initialize(VectorDataInitialization data_initialize, idx_t capacity_p) {
	auto internal_type = type.InternalType();
	auto capacity = capacity_t(capacity_p);
	if (internal_type == PhysicalType::STRUCT) {
		buffer = make_buffer<VectorStructBuffer>(type, capacity);
	} else if (internal_type == PhysicalType::LIST) {
		buffer = make_buffer<VectorListBuffer>(capacity, type);
		if (data_initialize == VectorDataInitialization::ZERO_INITIALIZE) {
			auto data = buffer->GetData();
			memset(data, 0, capacity * sizeof(list_entry_t));
		}
	} else if (internal_type == PhysicalType::ARRAY) {
		buffer = make_buffer<VectorArrayBuffer>(type, capacity);
	} else {
		auto type_size = GetTypeIdSize(internal_type);
		if (type_size == 0) {
			throw InternalException("Trying to create buffer for zero-length type");
		}
		buffer = VectorBuffer::CreateStandardVector(type, capacity);
		if (data_initialize == VectorDataInitialization::ZERO_INITIALIZE) {
			auto data = buffer->GetData();
			memset(data, 0, capacity * type_size);
		}
	}
}

void Vector::AddAuxiliaryData(unique_ptr<AuxiliaryDataHolder> data) {
	buffer->AddAuxiliaryData(std::move(data));
}

void Vector::AddHeapReference(const Vector &other) {
	if (other.GetVectorType() == VectorType::DICTIONARY_VECTOR) {
		AddHeapReference(DictionaryVector::Child(other));
		return;
	}
	auto &auxiliary_data = other.buffer->GetAuxiliaryData();
	if (!auxiliary_data) {
		return;
	}
	AddAuxiliaryData(make_uniq<AuxiliaryDataSetHolder>(auxiliary_data));
}

void Vector::Reserve(idx_t to_reserve_p) {
	if (GetVectorType() == VectorType::CONSTANT_VECTOR) {
		// a constant holds one physical entry regardless of its logical size
		return;
	}
	auto reserve_size = VectorBuffer::GetReserveSize(to_reserve_p);
	if (!buffer) {
		Initialize(VectorDataInitialization::UNINITIALIZED, reserve_size);
		return;
	}
	auto capacity = buffer->Capacity();
	if (reserve_size <= capacity) {
		return;
	}
	buffer->Reserve(reserve_size, VectorAppendMode::ALLOW_RESIZE);
}

void Vector::Resize(idx_t, idx_t to_reserve_p) {
	Reserve(to_reserve_p);
}

void Vector::Append(const Value &value, VectorAppendMode append_mode) {
	buffer->AppendValue(GetType(), value, append_mode);
}

void Vector::Append(const Vector &source, idx_t count, VectorAppendMode append_mode) {
	buffer->Append(source, *FlatVector::IncrementalSelectionVector(), count, append_mode);
}

void Vector::Append(const Vector &source, const SelectionVector &sel, idx_t count, VectorAppendMode append_mode) {
	buffer->Append(source, sel, count, append_mode);
}

void Vector::Copy(const Vector &source, const SelectionVector &source_sel, idx_t source_count, idx_t source_offset,
                  idx_t target_offset, idx_t copy_count) {
	D_ASSERT(source.GetType() == GetType());
	buffer->Copy(source, source_sel, source_count, source_offset, target_offset, copy_count);
}

void Vector::SetValue(idx_t index, const Value &val) {
	buffer->SetValue(GetType(), index, val);
}

Value Vector::GetValueInternal(const Vector &v_p, idx_t index_p) {
	return v_p.buffer->GetValue(v_p.GetType(), index_p);
}

Value Vector::GetValue(const Vector &v_p, idx_t index_p) {
	auto value = GetValueInternal(v_p, index_p);
	// the value's type is reconstructed from the data - restore the source type so that the alias survives
	if (v_p.GetType().HasAlias()) {
		value = value.WithType(v_p.GetType());
	}
	if (v_p.GetType().id() != LogicalTypeId::LEGACY_AGGREGATE_STATE &&
	    value.type().id() != LogicalTypeId::LEGACY_AGGREGATE_STATE) {
		D_ASSERT(v_p.GetType() == value.type());
	}
	return value;
}

Value Vector::GetValue(idx_t index) const {
	return GetValue(*this, index);
}

// LCOV_EXCL_START
string VectorTypeToString(VectorType type) {
	switch (type) {
	case VectorType::FLAT_VECTOR:
		return "FLAT";
	case VectorType::FSST_VECTOR:
		return "FSST";
	case VectorType::SEQUENCE_VECTOR:
		return "SEQUENCE";
	case VectorType::DICTIONARY_VECTOR:
		return "DICTIONARY";
	case VectorType::CONSTANT_VECTOR:
		return "CONSTANT";
	case VectorType::SHREDDED_VECTOR:
		return "SHREDDED";
	default:
		return "UNKNOWN";
	}
}

string Vector::ToString(idx_t) const {
	return ToString();
}

void Vector::Print(idx_t) const {
	Print();
}

idx_t Vector::GetDataSize() const {
	return GetDataSize(size());
}

idx_t Vector::GetDataSize(idx_t count) const {
	if (!buffer) {
		return 0;
	}
	return Buffer().GetDataSize(type, count);
}

idx_t Vector::GetAllocationSize(idx_t cardinality) const {
	return GetDataSize();
}

idx_t Vector::GetAllocationSize() const {
	if (!buffer) {
		return 0;
	}
	return Buffer().GetAllocationSize();
}

string Vector::ToString() const {
	auto count = size();
	string retval =
	    VectorTypeToString(GetVectorType()) + " " + GetType().ToString() + ": " + to_string(count) + " = [ ";
	retval += Buffer().ToString(GetType(), count);
	retval += "]";
	return retval;
}

void Vector::Print() const {
	Printer::Print(ToString());
}
// LCOV_EXCL_STOP

void Vector::Flatten(idx_t count) const {
	Flatten();
}

void Vector::Flatten() const {
	auto new_buffer = Buffer().Flatten(GetType());
	if (new_buffer) {
		buffer = std::move(new_buffer);
	}
}

void Vector::Flatten(const SelectionVector &sel, idx_t count) const {
	auto new_buffer = Buffer().FlattenSlice(GetType(), sel, count);
	if (new_buffer) {
		buffer = std::move(new_buffer);
	}
}

void Vector::ToUnifiedFormat(idx_t count, UnifiedVectorFormat &format) const {
	ToUnifiedFormat(format);
}

void Vector::ToUnifiedFormat(UnifiedVectorFormat &format) const {
	format.physical_type = GetType().InternalType();
	auto vtype = GetVectorType();
	if (vtype != VectorType::FLAT_VECTOR && vtype != VectorType::CONSTANT_VECTOR &&
	    vtype != VectorType::DICTIONARY_VECTOR) {
		// FSST/SEQUENCE/SHREDDED: flatten first so the buffer can provide unified format
		Flatten();
	}
	Buffer().ToUnifiedFormat(format);
}

void Vector::RecursiveToUnifiedFormat(const Vector &input, idx_t, RecursiveUnifiedVectorFormat &data) {
	RecursiveToUnifiedFormat(input, data);
}

void Vector::RecursiveToUnifiedFormat(const Vector &input, RecursiveUnifiedVectorFormat &data) {
	input.ToUnifiedFormat(data.unified);
	data.logical_type = input.GetType();

	if (input.GetType().InternalType() == PhysicalType::LIST) {
		auto &child = ListVector::GetChild(input);
		data.children.emplace_back();
		RecursiveToUnifiedFormat(child, data.children.back());

	} else if (input.GetType().InternalType() == PhysicalType::ARRAY) {
		auto &child = ArrayVector::GetChild(input);
		data.children.emplace_back();
		RecursiveToUnifiedFormat(child, data.children.back());

	} else if (input.GetType().InternalType() == PhysicalType::STRUCT) {
		auto &children = StructVector::GetEntries(input);
		for (idx_t i = 0; i < children.size(); i++) {
			data.children.emplace_back();
		}
		for (idx_t i = 0; i < children.size(); i++) {
			RecursiveToUnifiedFormat(children[i], data.children[i]);
		}
	}
}

void Vector::Sequence(int64_t start, int64_t increment, idx_t count) {
	this->buffer = make_buffer<SequenceBuffer>(start, increment, count_t(count));
}

void Vector::Shred(Vector &shredded_data, idx_t capacity) {
	if (GetType().id() != LogicalTypeId::VARIANT) {
		throw InternalException("Vector::Shred can only be used on variant vectors");
	}
	auto &shredded_type = shredded_data.GetType();
	if (shredded_type.id() != LogicalTypeId::STRUCT || StructType::GetChildCount(shredded_type) != 2) {
		throw InternalException("Vector::Shred parameter must be a struct with two children");
	}
	this->buffer = make_buffer<ShreddedVectorBuffer>(shredded_data, count_t(capacity));
}

// FIXME: This should ideally be const
void Vector::Serialize(Serializer &serializer, bool compressed_serialization) {
	auto &logical_type = GetType();

	auto count = size();
	UnifiedVectorFormat vdata;

	// serialize compressed vectors to save space, but skip this if serializing into older versions
	if (!serializer.ShouldSerialize(StorageVersion::V1_3_0)) {
		compressed_serialization = false;
	}
	if (compressed_serialization && GetVectorType() == VectorType::CONSTANT_VECTOR && count >= 1) {
		serializer.WriteProperty(90, "vector_type", VectorType::CONSTANT_VECTOR);
		// Resize to 1 so that size() == count == 1 during the recursive call, then restore
		FlatVector::SetSize(*this, 1);
		Vector::Serialize(serializer, false); // just serialize one value
		FlatVector::SetSize(*this, count);
		return;
	}
	if (buffer && buffer->TrySerialize(serializer, logical_type, compressed_serialization)) {
		return;
	}
	ToUnifiedFormat(vdata);

	if (logical_type.id() == LogicalTypeId::GEOMETRY && serializer.ShouldSerialize(Geometry::VERSION_ADDED)) {
		serializer.WriteProperty<GeometryStorageType>(99, "geometry_format", GeometryStorageType::WKB);
	}

	const bool has_validity_mask = (count > 0) && vdata.validity.CanHaveNull();
	serializer.WriteProperty(100, "has_validity_mask", has_validity_mask);
	if (has_validity_mask) {
		ValidityMask flat_mask(count);
		flat_mask.Initialize();
		for (idx_t i = 0; i < count; ++i) {
			auto row_idx = vdata.sel->get_index(i);
			flat_mask.Set(i, vdata.validity.RowIsValid(row_idx));
		}
		serializer.WriteProperty(101, "validity", const_data_ptr_cast(flat_mask.GetData()),
		                         flat_mask.ValidityMaskSize(count));
	}
	if (TypeIsConstantSize(logical_type.InternalType())) {
		// constant size type: simple copy
		idx_t write_size = GetTypeIdSize(logical_type.InternalType()) * count;
		auto ptr = make_unsafe_uniq_array_uninitialized<data_t>(write_size);
		VectorOperations::WriteToStorage(*this, ptr.get());
		serializer.WriteProperty(102, "data", ptr.get(), write_size);
	} else if (logical_type.id() == LogicalTypeId::GEOMETRY) {
		auto geoms = UnifiedVectorFormat::GetData<string_t>(vdata);

		// Are we targeting an older serialization version?
		if (!serializer.ShouldSerialize(StorageVersion::V1_5_0)) {
			// Serialize data as old-style SPATIAL format
			string blob;
			serializer.WriteList(102, "data", count, [&](Serializer::List &list, idx_t i) {
				auto idx = vdata.sel->get_index(i);
				if (!vdata.validity.RowIsValid(idx)) {
					list.WriteElement(NullValue<string_t>());
				} else {
					Geometry::ToSpatialGeometry(geoms[idx], blob);
					list.WriteElement(blob);
				}
			});
		} else {
			// Serialize as WKB format
			serializer.WriteList(102, "data", count, [&](Serializer::List &list, idx_t i) {
				auto idx = vdata.sel->get_index(i);
				auto wkb = !vdata.validity.RowIsValid(idx) ? NullValue<string_t>() : geoms[idx];
				list.WriteElement(wkb);
			});
		}
	} else {
		switch (logical_type.InternalType()) {
		case PhysicalType::VARCHAR: {
			auto strings = UnifiedVectorFormat::GetData<string_t>(vdata);
			// new way to serialize strings, two blobs, first lengths, then string bytes
			if (serializer.ShouldSerialize(StorageVersion::V2_0_0)) {
				// we write all the lengths whether the string is null or not. lets not pull a parquet.
				auto length_data_length = sizeof(uint32_t) * count;
				auto length_data = make_unsafe_uniq_array_uninitialized<data_t>(length_data_length);
				idx_t byte_data_length = 0;

				// write all the lengths
				auto lengths_write_ptr = reinterpret_cast<uint32_t *>(length_data.get());
				for (idx_t i = 0; i < count; i++) {
					auto idx = vdata.sel->get_index(i);
					auto this_length = vdata.validity.RowIsValid(idx) ? strings[idx].GetSize() : 0;
					lengths_write_ptr[i] = UnsafeNumericCast<uint32_t>(this_length);
					byte_data_length += this_length;
				}

				// write the bytes
				auto byte_data = make_unsafe_uniq_array_uninitialized<data_t>(byte_data_length);
				// write the non-null strings
				auto string_write_ptr = byte_data.get();
				for (idx_t i = 0; i < count; i++) {
					auto idx = vdata.sel->get_index(i);
					if (!vdata.validity.RowIsValid(idx)) {
						continue;
					}
					auto this_length = strings[idx].GetSize();
					memcpy(string_write_ptr, strings[idx].GetData(), this_length);
					string_write_ptr += this_length;
				}
				serializer.WriteProperty(107, "byte_data_length", optional_idx(byte_data_length));
				// we do not encode length_data_length because its not required
				serializer.WriteProperty(108, "length_data", length_data.get(), length_data_length);
				serializer.WriteProperty(109, "byte_data", byte_data.get(), byte_data_length);
			} else { // old and slow way: list
				serializer.WriteList(102, "data", count, [&](Serializer::List &list, idx_t i) {
					auto idx = vdata.sel->get_index(i);
					auto str = !vdata.validity.RowIsValid(idx) ? NullValue<string_t>() : strings[idx];
					list.WriteElement(str);
				});
			}

			break;
		}
		case PhysicalType::STRUCT: {
			auto &entries = StructVector::GetEntries(*this);

			// Serialize entries as a list
			serializer.WriteList(103, "children", entries.size(), [&](Serializer::List &list, idx_t i) {
				list.WriteObject([&](Serializer &object) { entries[i].Serialize(object, compressed_serialization); });
			});
			break;
		}
		case PhysicalType::LIST: {
			auto &child = ListVector::GetChildMutable(*this);
			auto list_size = ListVector::GetListSize(*this);

			// serialize the list entries in a flat array
			auto entries = make_unsafe_uniq_array_uninitialized<list_entry_t>(count);
			auto source_array = UnifiedVectorFormat::GetData<list_entry_t>(vdata);
			for (idx_t i = 0; i < count; i++) {
				auto idx = vdata.sel->get_index(i);
				auto source = source_array[idx];
				if (vdata.validity.RowIsValid(idx)) {
					entries[i].offset = source.offset;
					entries[i].length = source.length;
				} else {
					entries[i].offset = 0;
					entries[i].length = 0;
				}
			}
			serializer.WriteProperty(104, "list_size", list_size);
			serializer.WriteList(105, "entries", count, [&](Serializer::List &list, idx_t i) {
				list.WriteObject([&](Serializer &object) {
					object.WriteProperty(100, "offset", entries[i].offset);
					object.WriteProperty(101, "length", entries[i].length);
				});
			});
			serializer.WriteObject(106, "child",
			                       [&](Serializer &object) { child.Serialize(object, compressed_serialization); });
			break;
		}
		case PhysicalType::ARRAY: {
			Vector serialized_vector(Vector::Ref(*this));
			serialized_vector.Flatten();

			auto &child = ArrayVector::GetChildMutable(serialized_vector);
			auto array_size = ArrayType::GetSize(serialized_vector.GetType());
			serializer.WriteProperty<uint64_t>(103, "array_size", array_size);
			serializer.WriteObject(104, "child",
			                       [&](Serializer &object) { child.Serialize(object, compressed_serialization); });
			break;
		}
		default:
			throw InternalException("Unimplemented variable width type for Vector::Serialize!");
		}
	}
}

class StringDeserializeHolder : public AuxiliaryDataHolder {
public:
	explicit StringDeserializeHolder(idx_t size) {
		data = unique_ptr<data_t[]>(new data_t[size]);
	}
	unique_ptr<data_t[]> data;
};

static void CheckDeserializedListIndex(idx_t i, idx_t count) {
	if (i >= count) {
		throw SerializationException("Failed to deserialize vector: more entries than the vector size %llu", count);
	}
}

static void CheckDeserializedListCount(idx_t list_count, idx_t count) {
	if (list_count != count) {
		throw SerializationException("Failed to deserialize vector: expected %llu entries but got %llu", count,
		                             list_count);
	}
}

void Vector::Deserialize(Deserializer &deserializer, idx_t count) {
	auto &logical_type = GetType();

	const auto vtype = // older versions that only supported flat vectors did not serialize vector_type,
	    deserializer.ReadPropertyWithExplicitDefault<VectorType>(90, "vector_type", VectorType::FLAT_VECTOR);

	// first handle deserialization of compressed vector types
	if (vtype == VectorType::CONSTANT_VECTOR) {
		Vector::Deserialize(deserializer, 1); // read a vector of size 1
		Vector::SetVectorType(VectorType::CONSTANT_VECTOR);
		return;
	} else if (vtype != VectorType::FLAT_VECTOR) {
		buffer = VectorBuffer::Deserialize(deserializer, vtype, logical_type, count);
		return;
	}

	auto geometry_format = GeometryStorageType::WKB;
	if (logical_type.id() == LogicalTypeId::GEOMETRY) {
		// Try to read the geometry format, but default to the old SPATIAL format for older versions that did not
		// serialize this property
		geometry_format = deserializer.ReadPropertyWithExplicitDefault<GeometryStorageType>(
		    99, "geometry_format", GeometryStorageType::SPATIAL);
	}

	auto &validity = FlatVector::ValidityMutable(*this);
	auto validity_count = MaxValue<idx_t>(count, STANDARD_VECTOR_SIZE);
	validity.Reset(validity_count);
	const auto has_validity_mask = deserializer.ReadProperty<bool>(100, "has_validity_mask");
	if (has_validity_mask) {
		validity.Initialize(validity_count);
		deserializer.ReadProperty(101, "validity", data_ptr_cast(validity.GetData()), validity.ValidityMaskSize(count));
	}

	if (TypeIsConstantSize(logical_type.InternalType())) {
		// constant size type: read fixed amount of data
		auto column_size = GetTypeIdSize(logical_type.InternalType()) * count;
		auto ptr = make_unsafe_uniq_array_uninitialized<data_t>(column_size);
		deserializer.ReadProperty(102, "data", ptr.get(), column_size);

		VectorOperations::ReadFromStorage(ptr.get(), count, *this);
	} else if (logical_type.id() == LogicalTypeId::GEOMETRY) {
		auto blobs = FlatVector::GetDataMutable<string_t>(*this);

		if (geometry_format == GeometryStorageType::WKB) {
			idx_t entry_count = 0;
			deserializer.ReadList(102, "data", [&](Deserializer::List &list, idx_t i) {
				CheckDeserializedListIndex(i, count);
				entry_count++;
				auto geom = list.ReadElement<string>();
				if (validity.RowIsValid(i)) {
					blobs[i] = StringVector::AddStringOrBlob(*this, geom);
				}
			});
			CheckDeserializedListCount(entry_count, count);
		} else if (geometry_format == GeometryStorageType::SPATIAL) {
			// Try to read old SPATIAL format and convert to new GEOMETRY format
			idx_t entry_count = 0;
			deserializer.ReadList(102, "data", [&](Deserializer::List &list, idx_t i) {
				CheckDeserializedListIndex(i, count);
				entry_count++;
				auto blob = list.ReadElement<string>();
				if (validity.RowIsValid(i)) {
					Geometry::FromSpatialGeometry(blob, blobs[i], *this);
				}
			});
			CheckDeserializedListCount(entry_count, count);
		} else {
			throw InternalException("Unsupported geometry format in vector serialization");
		}

	} else {
		switch (logical_type.InternalType()) {
		case PhysicalType::VARCHAR: {
			auto strings = FlatVector::GetDataMutable<string_t>(*this);
			auto byte_data_length =
			    deserializer.ReadPropertyWithExplicitDefault<optional_idx>(107, "byte_data_length", optional_idx());
			if (byte_data_length.IsValid()) { // new serialization
				auto length_data_length = count * sizeof(uint32_t);
				auto length_data = make_unsafe_uniq_array_uninitialized<data_t>(length_data_length);
				deserializer.ReadProperty(108, "length_data", length_data.get(), length_data_length);

				auto byte_data_buffer = make_uniq<StringDeserializeHolder>(byte_data_length.GetIndex());
				// directly read into a string buffer we can glue to the vector
				deserializer.ReadProperty(109, "byte_data", byte_data_buffer->data.get(), byte_data_length.GetIndex());
				auto lengths_read_ptr = reinterpret_cast<uint32_t *>(length_data.get());
				auto byte_read_ptr = reinterpret_cast<const char *>(byte_data_buffer->data.get());
				StringVector::AddAuxiliaryData(*this, std::move(byte_data_buffer));

				idx_t remaining_bytes = byte_data_length.GetIndex();
				for (idx_t i = 0; i < count; ++i) {
					if (!validity.RowIsValid(i)) {
						continue;
					}
					if (lengths_read_ptr[i] > remaining_bytes) {
						throw SerializationException(
						    "Failed to deserialize vector: string lengths exceed the serialized string data");
					}
					remaining_bytes -= lengths_read_ptr[i];
					strings[i] = string_t(byte_read_ptr, lengths_read_ptr[i]);
					byte_read_ptr += lengths_read_ptr[i];
				}
			} else { // this is ye olde way of string serialization
				idx_t entry_count = 0;
				deserializer.ReadList(102, "data", [&](Deserializer::List &list, idx_t i) {
					CheckDeserializedListIndex(i, count);
					entry_count++;
					auto str = list.ReadElement<string>();
					if (validity.RowIsValid(i)) {
						strings[i] = StringVector::AddStringOrBlob(*this, str);
					}
				});
				CheckDeserializedListCount(entry_count, count);
			}
			break;
		}
		case PhysicalType::STRUCT: {
			auto &entries = StructVector::GetEntries(*this);
			// Deserialize entries as a list
			idx_t child_count = 0;
			deserializer.ReadList(103, "children", [&](Deserializer::List &list, idx_t i) {
				CheckDeserializedListIndex(i, entries.size());
				child_count++;
				list.ReadObject([&](Deserializer &obj) { entries[i].Deserialize(obj, count); });
			});
			CheckDeserializedListCount(child_count, entries.size());
			break;
		}
		case PhysicalType::LIST: {
			// Read the list size
			auto list_size = deserializer.ReadProperty<uint64_t>(104, "list_size");
			ListVector::Reserve(*this, list_size);
			ListVector::SetListSize(*this, list_size);

			// Read the entries
			auto list_entries = FlatVector::GetDataMutable<list_entry_t>(*this);
			idx_t entry_count = 0;
			deserializer.ReadList(105, "entries", [&](Deserializer::List &list, idx_t i) {
				CheckDeserializedListIndex(i, count);
				entry_count++;
				list.ReadObject([&](Deserializer &obj) {
					list_entries[i].offset = obj.ReadProperty<uint64_t>(100, "offset");
					list_entries[i].length = obj.ReadProperty<uint64_t>(101, "length");
				});
				if (!validity.RowIsValid(i)) {
					list_entries[i] = list_entry_t(0, 0);
				} else if (list_entries[i].offset > list_size ||
				           list_entries[i].length > list_size - list_entries[i].offset) {
					throw SerializationException("Failed to deserialize vector: list entry is out of range of the "
					                             "child vector of size %llu",
					                             list_size);
				}
			});
			CheckDeserializedListCount(entry_count, count);

			// Read the child vector
			deserializer.ReadObject(106, "child", [&](Deserializer &obj) {
				auto &child = ListVector::GetChildMutable(*this);
				child.Deserialize(obj, list_size);
			});
			break;
		}
		case PhysicalType::ARRAY: {
			auto array_size = deserializer.ReadProperty<uint64_t>(103, "array_size");
			if (array_size != ArrayType::GetSize(logical_type)) {
				throw SerializationException("Failed to deserialize vector: array size %llu does not match type %s",
				                             array_size, logical_type.ToString());
			}
			deserializer.ReadObject(104, "child", [&](Deserializer &obj) {
				auto &child = ArrayVector::GetChildMutable(*this);
				child.Deserialize(obj, array_size * count);
			});
			break;
		}
		default:
			throw InternalException("Unimplemented variable width type for Vector::Deserialize!");
		}
	}
}

void Vector::SetVectorType(VectorType new_vector_type) {
	if (new_vector_type != VectorType::FLAT_VECTOR && new_vector_type != VectorType::CONSTANT_VECTOR) {
		throw InternalException("SetVectorType can only be used with FLAT / CONSTANT vectors");
	}
	if (buffer) {
		// FIXME: should we allow vectors without a buffer?
		BufferMutable().SetVectorType(new_vector_type);
	}
}

void Vector::FlattenAndSetConstant() {
	if (GetVectorType() != VectorType::FLAT_VECTOR && GetVectorType() != VectorType::CONSTANT_VECTOR) {
		Flatten();
	}
	// Struct buffers propagate vector type changes to their children. A flat or constant struct vector can still
	// contain non-flat descendants, e.g. after slicing a list of structs, so normalize only those descendants first.
	if (GetType().InternalType() == PhysicalType::STRUCT) {
		BufferMutable().Cast<VectorStructBuffer>().PrepareChildrenForSetConstant();
	}
	SetVectorType(VectorType::CONSTANT_VECTOR);
}

void Vector::Verify(idx_t) const {
	Verify();
}

void Vector::Verify() const {
	if (DBConfigOptions::global_verification_mode != DebugVerificationMode::VERIFY_VECTORS) {
		return;
	}
	if (!buffer) {
		return;
	}
	buffer->Verify(GetType());
}

void Vector::Verify(const SelectionVector &sel, idx_t count) const {
	if (DBConfigOptions::global_verification_mode != DebugVerificationMode::VERIFY_VECTORS) {
		return;
	}
	if (!buffer) {
		return;
	}
	buffer->Verify(GetType(), sel, count);
}

void Vector::DebugTransformToDictionary(Vector &vector) {
	const auto count = vector.size();
	if (count == 0) {
		return;
	}
	if (vector.GetVectorType() != VectorType::FLAT_VECTOR) {
		// only supported for flat vectors currently
		return;
	}
	// convert vector to dictionary vector
	// first create an inverted vector of twice the size with NULL values every other value
	// i.e. [1, 2, 3] is converted into [NULL, 3, NULL, 2, NULL, 1]
	idx_t verify_count = count * 2;
	SelectionVector inverted_sel(verify_count);
	idx_t offset = 0;
	for (idx_t i = 0; i < count; i++) {
		idx_t current_index = count - i - 1;
		inverted_sel.set_index(offset++, current_index);
		inverted_sel.set_index(offset++, current_index);
	}
	auto reusable_dict = DictionaryVector::CreateReusableDictionary(vector.type, verify_count);
	auto &inverted_vector = reusable_dict->data;
	inverted_vector.Slice(vector, inverted_sel, verify_count);
	inverted_vector.Flatten();
	// now insert the NULL values at every other position
	for (idx_t i = 0; i < count; i++) {
		FlatVector::SetNull(inverted_vector, i * 2, true);
	}
	// construct the selection vector pointing towards the original values
	// we start at the back, (verify_count - 1) and move backwards
	SelectionVector original_sel(count);
	offset = 0;
	for (idx_t i = 0; i < count; i++) {
		original_sel.set_index(offset++, verify_count - 1 - i * 2);
	}
	// now slice the inverted vector with the inverted selection vector
	if (vector.GetType().InternalType() == PhysicalType::STRUCT) {
		// Reusable dictionary API does not work for STRUCT
		vector.Dictionary(inverted_vector, verify_count, original_sel, count);
	} else {
		vector.Dictionary(reusable_dict, original_sel, count);
	}
	vector.Verify();
}

void Vector::DebugShuffleNestedVector(Vector &vector) {
	const auto count = vector.size();
	switch (vector.GetType().id()) {
	case LogicalTypeId::TUPLE:
	case LogicalTypeId::STRUCT: {
		auto &entries = StructVector::GetEntries(vector);
		// recurse into child elements
		for (auto &entry : entries) {
			Vector::DebugShuffleNestedVector(entry);
		}
		break;
	}
	case LogicalTypeId::LIST: {
		if (vector.GetVectorType() != VectorType::FLAT_VECTOR) {
			break;
		}
		auto list_entries = FlatVector::GetDataMutable<list_entry_t>(vector);
		idx_t child_count = 0;
		for (idx_t r = 0; r < count; r++) {
			if (FlatVector::IsNull(vector, r)) {
				continue;
			}
			child_count += list_entries[r].length;
		}
		if (child_count == 0) {
			break;
		}
		auto &child_vector = ListVector::GetChildMutable(vector);
		// reverse the order of all lists
		SelectionVector child_sel(child_count);
		idx_t position = child_count;
		for (idx_t r = 0; r < count; r++) {
			if (FlatVector::IsNull(vector, r)) {
				continue;
			}
			// move this list to the back
			position -= list_entries[r].length;
			for (idx_t k = 0; k < list_entries[r].length; k++) {
				child_sel.set_index(position + k, list_entries[r].offset + k);
			}
			// adjust the offset to this new position
			list_entries[r].offset = position;
		}
		child_vector.Slice(child_sel, child_count);
		child_vector.Flatten();
		ListVector::SetListSize(vector, child_count);

		// recurse into child elements
		Vector::DebugShuffleNestedVector(child_vector);
		break;
	}
	default:
		break;
	}
}

} // namespace duckdb
