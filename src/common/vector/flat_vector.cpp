#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/array_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/types/bignum.hpp"

namespace duckdb {
StandardVectorBuffer::StandardVectorBuffer(Allocator &allocator, capacity_t capacity_p, idx_t type_size_p)
    : StandardVectorBuffer(AllocatedData(allocator), capacity_p, type_size_p) {
	EnsureDataAllocated();
}
StandardVectorBuffer::StandardVectorBuffer(AllocatedData allocation, capacity_t capacity_p, idx_t type_size_p)
    : VectorBuffer(VectorType::FLAT_VECTOR, VectorBufferType::STANDARD_BUFFER, count_t(0)), data_ptr(nullptr),
      type_size(type_size_p), capacity(capacity_p), allocated_data(std::move(allocation)) {
	D_ASSERT(!allocated_data.get());
	validity.Resize(capacity);
}

data_ptr_t StandardVectorBuffer::GetDataMutable() {
	EnsureDataAllocated();
	return data_ptr;
}

void StandardVectorBuffer::EnsureDataAllocated() {
	if (!data_ptr && capacity && allocated_data.GetAllocator()) {
		if (!type_size || capacity > DConstants::MAX_VECTOR_SIZE / type_size) {
			throw OutOfRangeException("Vector capacity is too large");
		}
		allocated_data = allocated_data.GetAllocator()->Allocate(capacity * type_size);
		data_ptr = allocated_data.get();
	}
}
StandardVectorBuffer::StandardVectorBuffer(capacity_t capacity, idx_t type_size_p)
    : StandardVectorBuffer(Allocator::DefaultAllocator(), capacity, type_size_p) {
}
StandardVectorBuffer::StandardVectorBuffer(data_ptr_t data_ptr_p, count_t count, idx_t type_size_p)
    : VectorBuffer(VectorType::FLAT_VECTOR, VectorBufferType::STANDARD_BUFFER, count), data_ptr(data_ptr_p),
      type_size(type_size_p), capacity(count) {
	// the validity capacity must cover all entries, even while no mask is allocated (all-valid data)
	validity.Resize(capacity);
}
StandardVectorBuffer::StandardVectorBuffer(AllocatedData &&data_p, count_t count, idx_t type_size_p)
    : VectorBuffer(VectorType::FLAT_VECTOR, VectorBufferType::STANDARD_BUFFER, count), data_ptr(data_p.get()),
      type_size(type_size_p), capacity(data_p.GetSize() / type_size), allocated_data(std::move(data_p)) {
	if (count > capacity) {
		throw InternalException("Count is out of range for capacity");
	}
	// the validity capacity must cover all entries, even while no mask is allocated (all-valid data)
	validity.Resize(capacity);
}

void StandardVectorBuffer::SetVectorType(VectorType new_vector_type) {
	if (new_vector_type == VectorType::CONSTANT_VECTOR && capacity == 0) {
		Reserve(1, VectorAppendMode::ALLOW_RESIZE);
	}
	vector_type = new_vector_type;
}

void StandardVectorBuffer::ResetCapacity(idx_t capacity_p) {
	capacity = capacity_p;
	validity.Reset(capacity);
}

idx_t StandardVectorBuffer::GetAllocationSize() const {
	idx_t size = VectorBuffer::GetAllocationSize();
	size += allocated_data.GetSize();
	size += validity.GetAllocationSize();
	return size;
}

void StandardVectorBuffer::VerifyInternal(const LogicalType &type, const SelectionVector &sel, idx_t count) const {
	D_ASSERT(vector_type == VectorType::FLAT_VECTOR || vector_type == VectorType::CONSTANT_VECTOR);
	if (type_size != GetTypeIdSize(type.InternalType())) {
		throw InternalException("Type size mismatch in flat vector buffer");
	}
	// verify all entries in the sel fit within the validity
	if (sel.IsSet()) {
		for (idx_t i = 0; i < count; i++) {
			auto sel_idx = sel.get_index(i);
			if (sel_idx > validity.Capacity()) {
				throw InternalException("Selection vector index %d out of range for validity capacity %d", sel_idx,
				                        validity.Capacity());
			}
			if (sel_idx > Capacity()) {
				throw InternalException("Selection vector index %d out of range for vector capacity %d", sel_idx,
				                        Capacity());
			}
		}
	} else if (vector_type == VectorType::FLAT_VECTOR) {
		if (count > validity.Capacity()) {
			throw InternalException("Count %d out of range for validity capacity %d", count, validity.Capacity());
		}
		if (count > Capacity()) {
			throw InternalException("Count %d out of range for capacity %d", count, Capacity());
		}
	}
	if (type.id() == LogicalTypeId::VARCHAR) {
		// verify the strings themselves - this catches entries a producer left uninitialized
		// only for actual VARCHAR: BLOB/BIT/VARINT/aggregate states share the physical type but are not UTF8
		// ForceVerify: we only get here in VERIFY_VECTORS mode, which is also run against release builds
		const auto strings = reinterpret_cast<const string_t *>(data_ptr);
		for (idx_t i = 0; i < count; i++) {
			const auto idx = sel.get_index(i);
			if (validity.RowIsValid(idx)) {
				strings[idx].ForceVerify();
			}
		}
	}
}

buffer_ptr<VectorBuffer> StandardVectorBuffer::SliceInternal(const LogicalType &type, idx_t offset, idx_t end) {
	D_ASSERT(end <= capacity);
	auto count = count_t(end - offset);
	auto offset_ptr = data_ptr ? data_ptr + type_size * offset : nullptr;
	auto result = make_buffer<StandardVectorBuffer>(offset_ptr, count, type_size);
	result->GetValidityMask().Slice(validity, offset, count);
	result->AddAuxiliaryData(make_uniq<VectorBufferHolder>(shared_from_this()));
	return result;
}

buffer_ptr<VectorBuffer> StandardVectorBuffer::ConstantSliceInternal(const LogicalType &type, count_t count) {
	// a constant has one physical entry - the logical count is tracked separately from the physical capacity
	auto result = make_buffer<StandardVectorBuffer>(data_ptr, count_t(1ULL), type_size);
	result->GetValidityMask().Set(0, validity.RowIsValid(0));
	result->SetVectorType(VectorType::CONSTANT_VECTOR);
	result->SetVectorSize(count);
	result->AddAuxiliaryData(make_uniq<VectorBufferHolder>(shared_from_this()));
	return result;
}

buffer_ptr<VectorBuffer> StandardVectorBuffer::SliceInternal(const LogicalType &type, const SelectionVector &sel,
                                                             idx_t count) {
	if (!sel.IsSet()) {
		// Incremental selection (rows 0..count-1): produce a flat offset-based sub-view
		// to avoid creating a DictionaryBuffer with a null selection vector.
		return SliceInternal(type, idx_t(0), count);
	}
	Vector child_vector(type, shared_from_this());
	auto entry = make_shared_ptr<DictionaryEntry>(std::move(child_vector));
	return make_buffer<DictionaryBuffer>(sel, count, std::move(entry));
}

buffer_ptr<VectorBuffer> StandardVectorBuffer::CreateBuffer(AllocatedData &&new_data, count_t count) const {
	return make_buffer<StandardVectorBuffer>(std::move(new_data), count, type_size);
}

void StandardVectorBuffer::ReserveInternal(idx_t new_size) {
	D_ASSERT(v_size < new_size);
	auto old_byte_count = Capacity() * type_size;
	auto target_byte_count = new_size * type_size;

	// We have an upper limit of 128GB for a single vector.
	if (target_byte_count > DConstants::MAX_VECTOR_SIZE) {
		throw OutOfRangeException("Cannot resize vector to %s: maximum allowed vector size is %s",
		                          StringUtil::BytesToHumanReadableString(target_byte_count),
		                          StringUtil::BytesToHumanReadableString(DConstants::MAX_VECTOR_SIZE));
	}
	// Copy the data buffer to a resized buffer.
	auto stored_allocator = GetAllocator();
	auto &allocator = stored_allocator ? *stored_allocator : Allocator::DefaultAllocator();
	auto new_data = allocator.Allocate(target_byte_count);
	if (data_ptr) {
		memcpy(new_data.get(), data_ptr, old_byte_count);
	}

	// update the buffer structure in-place
	allocated_data = std::move(new_data);
	data_ptr = allocated_data.get();
	capacity = new_size;
	// resize the validity mask
	validity.Resize(new_size);
}

template <idx_t TYPE_SIZE>
void FixedFlattenCopy(data_ptr_t target, const_data_ptr_t source, const SelectionVector &sel, idx_t count) {
	for (idx_t i = 0; i < count; i++) {
		auto src_idx = sel.get_index(i);
		memcpy(target + i * TYPE_SIZE, source + src_idx * TYPE_SIZE, TYPE_SIZE);
	}
}

void FlattenVectorBuffer(data_ptr_t target, const_data_ptr_t source, const SelectionVector &sel, idx_t count,
                         idx_t type_size) {
	switch (type_size) {
	case 1:
		FixedFlattenCopy<1>(target, source, sel, count);
		break;
	case 2:
		FixedFlattenCopy<2>(target, source, sel, count);
		break;
	case 4:
		FixedFlattenCopy<4>(target, source, sel, count);
		break;
	case 8:
		FixedFlattenCopy<8>(target, source, sel, count);
		break;
	case 16:
		FixedFlattenCopy<16>(target, source, sel, count);
		break;
	default:
		// fallback: use non-fixed-width copy
		for (idx_t i = 0; i < count; i++) {
			auto src_idx = sel.get_index(i);
			memcpy(target + i * type_size, source + src_idx * type_size, type_size);
		}
		break;
	}
}

buffer_ptr<VectorBuffer> StandardVectorBuffer::Flatten(const LogicalType &type) const {
	if (vector_type == VectorType::FLAT_VECTOR) {
		// already a flat vector - bail
		return nullptr;
	}
	return FlattenSlice(type, *FlatVector::IncrementalSelectionVector(), Size());
}

buffer_ptr<VectorBuffer> StandardVectorBuffer::FlattenSliceInternal(const LogicalType &type, const SelectionVector &sel,
                                                                    idx_t count) const {
	// allocate the new buffer
	auto allocated_count = MaxValue<idx_t>(STANDARD_VECTOR_SIZE, count);
	auto target_byte_count = allocated_count * type_size;
	auto stored_allocator = GetAllocator();
	auto &allocator = stored_allocator ? *stored_allocator : Allocator::DefaultAllocator();
	auto new_data = allocator.Allocate(target_byte_count);
	// copy data using sel
	FlattenVectorBuffer(new_data.get(), data_ptr, sel, count, type_size);

	auto result = CreateBuffer(std::move(new_data), count_t(count));
	// copy validity using sel
	auto &result_validity = result->GetValidityMask();
	result_validity.CopySel(validity, sel, 0, 0, count);
	return result;
}

void StandardVectorBuffer::ToUnifiedFormat(UnifiedVectorFormat &format) const {
	if (vector_type == VectorType::CONSTANT_VECTOR) {
		format.sel = ConstantVector::ZeroSelectionVector(Size(), format.owned_sel);
	} else {
		format.sel = FlatVector::IncrementalSelectionVector();
	}
	format.data = data_ptr;
	format.validity = validity;
}

template <idx_t TYPE_SIZE>
void FixedSizeCopy(data_ptr_t target_data, const_data_ptr_t source_data, const SelectionVector &sel,
                   idx_t base_source_offset, idx_t base_target_offset, idx_t copy_count) {
	for (idx_t i = 0; i < copy_count; i++) {
		auto source_idx = sel.get_index(base_source_offset + i);
		auto target_offset = (base_target_offset + i) * TYPE_SIZE;
		auto source_offset = source_idx * TYPE_SIZE;
		memcpy(target_data + target_offset, source_data + source_offset, TYPE_SIZE);
	}
}

void CopyVectorBuffer(data_ptr_t target_data, const_data_ptr_t source_data, const SelectionVector &sel,
                      idx_t base_source_offset, idx_t base_target_offset, idx_t copy_count, idx_t type_size) {
	switch (type_size) {
	case 1:
		FixedSizeCopy<1>(target_data, source_data, sel, base_source_offset, base_target_offset, copy_count);
		break;
	case 2:
		FixedSizeCopy<2>(target_data, source_data, sel, base_source_offset, base_target_offset, copy_count);
		break;
	case 4:
		FixedSizeCopy<4>(target_data, source_data, sel, base_source_offset, base_target_offset, copy_count);
		break;
	case 8:
		FixedSizeCopy<8>(target_data, source_data, sel, base_source_offset, base_target_offset, copy_count);
		break;
	case 16:
		FixedSizeCopy<16>(target_data, source_data, sel, base_source_offset, base_target_offset, copy_count);
		break;
	default:
		// fallback: use non-fixed-width copy
		for (idx_t i = 0; i < copy_count; i++) {
			auto source_idx = sel.get_index(base_source_offset + i);
			auto target_offset = (base_target_offset + i) * type_size;
			auto source_offset = source_idx * type_size;
			memcpy(target_data + target_offset, source_data + source_offset, type_size);
		}
		break;
	}
}

void StandardVectorBuffer::CopyInternal(const Vector &source, const SelectionVector &source_sel, idx_t source_count,
                                        idx_t source_offset, idx_t target_offset, idx_t copy_count) {
	// now copy over the data
	const_data_ptr_t source_data;
	if (source.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		source_data = ConstantVector::GetData(source);
	} else {
		source_data = FlatVector::GetData(source);
	}

	CopyVectorBuffer(data_ptr, source_data, source_sel, source_offset, target_offset, copy_count, type_size);
}

void StandardVectorBuffer::SetValue(const LogicalType &type, idx_t index, const Value &val) {
	EnsureDataAllocated();
	if (!val.IsNull() && val.type() != type) {
		SetValue(type, index, val.DefaultCastAs(type));
		return;
	}
	if (index >= capacity) {
		throw InvalidInputException("Vector::SetValue index %d is out of range for vector with capacity %d", index,
		                            capacity);
	}
	validity.Set(index, !val.IsNull());
	if (val.IsNull()) {
		return;
	}
	switch (type.InternalType()) {
	case PhysicalType::BOOL:
		reinterpret_cast<bool *>(data_ptr)[index] = val.GetValueUnsafe<bool>();
		break;
	case PhysicalType::INT8:
		reinterpret_cast<int8_t *>(data_ptr)[index] = val.GetValueUnsafe<int8_t>();
		break;
	case PhysicalType::INT16:
		reinterpret_cast<int16_t *>(data_ptr)[index] = val.GetValueUnsafe<int16_t>();
		break;
	case PhysicalType::INT32:
		reinterpret_cast<int32_t *>(data_ptr)[index] = val.GetValueUnsafe<int32_t>();
		break;
	case PhysicalType::INT64:
		reinterpret_cast<int64_t *>(data_ptr)[index] = val.GetValueUnsafe<int64_t>();
		break;
	case PhysicalType::INT128:
		reinterpret_cast<hugeint_t *>(data_ptr)[index] = val.GetValueUnsafe<hugeint_t>();
		break;
	case PhysicalType::UINT8:
		reinterpret_cast<uint8_t *>(data_ptr)[index] = val.GetValueUnsafe<uint8_t>();
		break;
	case PhysicalType::UINT16:
		reinterpret_cast<uint16_t *>(data_ptr)[index] = val.GetValueUnsafe<uint16_t>();
		break;
	case PhysicalType::UINT32:
		reinterpret_cast<uint32_t *>(data_ptr)[index] = val.GetValueUnsafe<uint32_t>();
		break;
	case PhysicalType::UINT64:
		reinterpret_cast<uint64_t *>(data_ptr)[index] = val.GetValueUnsafe<uint64_t>();
		break;
	case PhysicalType::UINT128:
		reinterpret_cast<uhugeint_t *>(data_ptr)[index] = val.GetValueUnsafe<uhugeint_t>();
		break;
	case PhysicalType::FLOAT:
		reinterpret_cast<float *>(data_ptr)[index] = val.GetValueUnsafe<float>();
		break;
	case PhysicalType::DOUBLE:
		reinterpret_cast<double *>(data_ptr)[index] = val.GetValueUnsafe<double>();
		break;
	case PhysicalType::INTERVAL:
		reinterpret_cast<interval_t *>(data_ptr)[index] = val.GetValueUnsafe<interval_t>();
		break;
	default:
		throw InternalException("Unimplemented type for StandardVectorBuffer::SetValue");
	}
}

Value StandardVectorBuffer::GetValue(const LogicalType &type, idx_t index) const {
	if (vector_type == VectorType::CONSTANT_VECTOR) {
		index = 0;
	}
	if (!validity.RowIsValid(index)) {
		return Value(type);
	}
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		return Value::BOOLEAN(reinterpret_cast<const bool *>(data_ptr)[index]);
	case LogicalTypeId::TINYINT:
		return Value::TINYINT(reinterpret_cast<const int8_t *>(data_ptr)[index]);
	case LogicalTypeId::SMALLINT:
		return Value::SMALLINT(reinterpret_cast<const int16_t *>(data_ptr)[index]);
	case LogicalTypeId::INTEGER:
		return Value::INTEGER(reinterpret_cast<const int32_t *>(data_ptr)[index]);
	case LogicalTypeId::DATE:
		return Value::DATE(reinterpret_cast<const date_t *>(data_ptr)[index]);
	case LogicalTypeId::TIME:
		return Value::TIME(reinterpret_cast<const dtime_t *>(data_ptr)[index]);
	case LogicalTypeId::TIME_NS:
		return Value::TIME_NS(reinterpret_cast<const dtime_ns_t *>(data_ptr)[index]);
	case LogicalTypeId::TIME_TZ:
		return Value::TIMETZ(reinterpret_cast<const dtime_tz_t *>(data_ptr)[index]);
	case LogicalTypeId::BIGINT:
		return Value::BIGINT(reinterpret_cast<const int64_t *>(data_ptr)[index]);
	case LogicalTypeId::UTINYINT:
		return Value::UTINYINT(reinterpret_cast<const uint8_t *>(data_ptr)[index]);
	case LogicalTypeId::USMALLINT:
		return Value::USMALLINT(reinterpret_cast<const uint16_t *>(data_ptr)[index]);
	case LogicalTypeId::UINTEGER:
		return Value::UINTEGER(reinterpret_cast<const uint32_t *>(data_ptr)[index]);
	case LogicalTypeId::UBIGINT:
		return Value::UBIGINT(reinterpret_cast<const uint64_t *>(data_ptr)[index]);
	case LogicalTypeId::TIMESTAMP:
		return Value::TIMESTAMP(reinterpret_cast<const timestamp_t *>(data_ptr)[index]);
	case LogicalTypeId::TIMESTAMP_NS:
		return Value::TIMESTAMPNS(reinterpret_cast<const timestamp_ns_t *>(data_ptr)[index]);
	case LogicalTypeId::TIMESTAMP_MS:
		return Value::TIMESTAMPMS(reinterpret_cast<const timestamp_ms_t *>(data_ptr)[index]);
	case LogicalTypeId::TIMESTAMP_SEC:
		return Value::TIMESTAMPSEC(reinterpret_cast<const timestamp_sec_t *>(data_ptr)[index]);
	case LogicalTypeId::TIMESTAMP_TZ:
		return Value::TIMESTAMPTZ(reinterpret_cast<const timestamp_tz_t *>(data_ptr)[index]);
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		return Value::TIMESTAMPTZNS(reinterpret_cast<const timestamp_tz_ns_t *>(data_ptr)[index]);
	case LogicalTypeId::HUGEINT:
		return Value::HUGEINT(reinterpret_cast<const hugeint_t *>(data_ptr)[index]);
	case LogicalTypeId::UHUGEINT:
		return Value::UHUGEINT(reinterpret_cast<const uhugeint_t *>(data_ptr)[index]);
	case LogicalTypeId::UUID:
		return Value::UUID(reinterpret_cast<const hugeint_t *>(data_ptr)[index]);
	case LogicalTypeId::DECIMAL: {
		auto width = DecimalType::GetWidth(type);
		auto scale = DecimalType::GetScale(type);
		switch (type.InternalType()) {
		case PhysicalType::INT16:
			return Value::DECIMAL(reinterpret_cast<const int16_t *>(data_ptr)[index], width, scale);
		case PhysicalType::INT32:
			return Value::DECIMAL(reinterpret_cast<const int32_t *>(data_ptr)[index], width, scale);
		case PhysicalType::INT64:
			return Value::DECIMAL(reinterpret_cast<const int64_t *>(data_ptr)[index], width, scale);
		case PhysicalType::INT128:
			return Value::DECIMAL(reinterpret_cast<const hugeint_t *>(data_ptr)[index], width, scale);
		default:
			throw InternalException("Physical type '%s' has a width bigger than 38, which is not supported",
			                        TypeIdToString(type.InternalType()));
		}
	}
	case LogicalTypeId::ENUM: {
		switch (type.InternalType()) {
		case PhysicalType::UINT8:
			return Value::ENUM(reinterpret_cast<const uint8_t *>(data_ptr)[index], type);
		case PhysicalType::UINT16:
			return Value::ENUM(reinterpret_cast<const uint16_t *>(data_ptr)[index], type);
		case PhysicalType::UINT32:
			return Value::ENUM(reinterpret_cast<const uint32_t *>(data_ptr)[index], type);
		default:
			throw InternalException("ENUM can only have unsigned integers as physical types");
		}
	}
	case LogicalTypeId::POINTER:
		return Value::POINTER(reinterpret_cast<const uintptr_t *>(data_ptr)[index]);
	case LogicalTypeId::FLOAT:
		return Value::FLOAT(reinterpret_cast<const float *>(data_ptr)[index]);
	case LogicalTypeId::DOUBLE:
		return Value::DOUBLE(reinterpret_cast<const double *>(data_ptr)[index]);
	case LogicalTypeId::INTERVAL:
		return Value::INTERVAL(reinterpret_cast<const interval_t *>(data_ptr)[index]);
	case LogicalTypeId::VARCHAR: {
		auto str = reinterpret_cast<const string_t *>(data_ptr)[index];
		return Value(str.GetString());
	}
	case LogicalTypeId::BLOB: {
		auto str = reinterpret_cast<const string_t *>(data_ptr)[index];
		return Value::BLOB(const_data_ptr_cast(str.GetData()), str.GetSize());
	}
	case LogicalTypeId::BIGNUM: {
		auto str = reinterpret_cast<const bignum_t *>(data_ptr)[index];
		return Value::BIGNUM(const_data_ptr_cast(str.data.GetData()), str.data.GetSize());
	}
	case LogicalTypeId::GEOMETRY: {
		auto str = reinterpret_cast<const string_t *>(data_ptr)[index];
		if (GeoType::HasCRS(type)) {
			return Value::GEOMETRY(const_data_ptr_cast(str.GetData()), str.GetSize(), GeoType::GetCRS(type));
		}
		return Value::GEOMETRY(const_data_ptr_cast(str.GetData()), str.GetSize());
	}
	case LogicalTypeId::GEOGRAPHY: {
		auto str = reinterpret_cast<const string_t *>(data_ptr)[index];
		if (GeoType::HasCRS(type)) {
			return Value::GEOGRAPHY(const_data_ptr_cast(str.GetData()), str.GetSize(), GeoType::GetCRS(type));
		}
		return Value::GEOGRAPHY(const_data_ptr_cast(str.GetData()), str.GetSize());
	}
	case LogicalTypeId::LEGACY_AGGREGATE_STATE: {
		auto str = reinterpret_cast<const string_t *>(data_ptr)[index];
		return Value::LEGACY_AGGREGATE_STATE(type, const_data_ptr_cast(str.GetData()), str.GetSize());
	}
	case LogicalTypeId::BIT: {
		auto str = reinterpret_cast<const string_t *>(data_ptr)[index];
		return Value::BIT(const_data_ptr_cast(str.GetData()), str.GetSize());
	}
	case LogicalTypeId::SQLNULL:
		return Value();
	case LogicalTypeId::TYPE: {
		auto blob = reinterpret_cast<const string_t *>(data_ptr)[index];
		return Value::TYPE(blob);
	}
	default:
		throw InternalException("Unimplemented type for StandardVectorBuffer::GetValue");
	}
}

void FlatVector::PrepareForWrite(Vector &vector, idx_t size) {
	vector.CheckCapacity(size);
	if (vector.GetBufferRef()) {
		auto &buffer = vector.BufferMutable();
		if (buffer.GetBufferType() == VectorBufferType::STANDARD_BUFFER ||
		    buffer.GetBufferType() == VectorBufferType::STRING_BUFFER) {
			buffer.Cast<StandardVectorBuffer>().EnsureDataAllocated();
		}
	}
	SetSize(vector, size);
}

void FlatVector::SetData(Vector &vector, data_ptr_t data, count_t count) {
	VerifyFlatVector(vector);
	if (vector.GetType().InternalType() == PhysicalType::ARRAY) {
		throw InternalException("SetData not supported for array");
	}
	if (vector.GetType().InternalType() == PhysicalType::STRUCT) {
		throw InternalException("SetData not supported for struct");
	}
	// Preserve the validity mask from the old buffer before replacing it.
	// FIXME: this can maybe be removed in the future - it seems only the Arrow conversion code relies on this behavior
	auto old_validity = vector.Buffer().GetValidityMask();
	if (vector.GetType().InternalType() == PhysicalType::LIST) {
		auto &current_buffer = vector.BufferMutable().Cast<VectorListBuffer>();
		vector.SetBuffer(make_buffer<VectorListBuffer>(data, count, current_buffer.GetChild()));
	} else if (vector.GetType().InternalType() == PhysicalType::VARCHAR) {
		vector.SetBuffer(make_buffer<VectorStringBuffer>(data, count));
	} else {
		auto type_size = GetTypeIdSize(vector.GetType().InternalType());
		vector.SetBuffer(make_buffer<StandardVectorBuffer>(data, count, type_size));
	}
	vector.BufferMutable().GetValidityMask() = std::move(old_validity);
	// set the size of the new buffer so child->size() works for list parents using GetChildSize()
	vector.BufferMutable().SetVectorSize(count);
}

void FlatVector::SetNull(Vector &vector, idx_t idx, bool is_null) {
	D_ASSERT(vector.GetVectorType() == VectorType::FLAT_VECTOR);
	vector.BufferMutable().GetValidityMask().Set(idx, !is_null);
	if (!is_null) {
		return;
	}

	auto &type = vector.GetType();
	auto internal_type = type.InternalType();

	// Set all child entries to NULL.
	if (internal_type == PhysicalType::STRUCT) {
		auto &entries = StructVector::GetEntries(vector);
		for (auto &entry : entries) {
			FlatVector::SetNull(entry, idx, is_null);
		}
		return;
	}

	// Set all child entries to NULL.
	if (internal_type == PhysicalType::ARRAY) {
		auto &child = ArrayVector::GetChildMutable(vector);
		auto array_size = ArrayType::GetSize(type);
		auto child_offset = idx * array_size;
		for (idx_t i = 0; i < array_size; i++) {
			FlatVector::SetNull(child, child_offset + i, is_null);
		}
	}
}

void FlatVector::CopyValidity(Vector &target, const Vector &source, idx_t count) {
	if (source.GetVectorType() == VectorType::FLAT_VECTOR) {
		SetValidity(target, Validity(source));
		return;
	}
	auto &result_validity = ValidityMutable(target);
	if (source.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		if (ConstantVector::IsNull(source)) {
			result_validity.SetAllInvalid(count);
		} else {
			result_validity.Reset(count);
		}
		return;
	}
	auto validity = source.Validity();
	if (!validity.CanHaveNull()) {
		result_validity.Reset(count);
		return;
	}
	for (idx_t r = 0; r < count; r++) {
		if (!validity.IsValid(r)) {
			result_validity.SetInvalid(r);
		}
	}
}

} // namespace duckdb
