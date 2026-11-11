#include "duckdb/common/vector/array_vector.hpp"
#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/vector/dictionary_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"

namespace duckdb {

VectorArrayBuffer::VectorArrayBuffer(unique_ptr<Vector> child_vector, idx_t array_size, capacity_t initial_capacity)
    : VectorBuffer(VectorType::FLAT_VECTOR, VectorBufferType::ARRAY_BUFFER, count_t(0)), child(std::move(child_vector)),
      array_size(array_size), capacity(initial_capacity) {
	D_ASSERT(array_size != 0);
	validity.Resize(initial_capacity);
}

VectorArrayBuffer::VectorArrayBuffer(const LogicalType &array, capacity_t initial)
    : VectorArrayBuffer(make_uniq<Vector>(ArrayType::GetChildType(array), initial * ArrayType::GetSize(array)),
                        ArrayType::GetSize(array), initial) {
}

VectorArrayBuffer::~VectorArrayBuffer() {
}

Vector &VectorArrayBuffer::GetChild() {
	return *child;
}

idx_t VectorArrayBuffer::GetArraySize() const {
	return array_size;
}

idx_t VectorArrayBuffer::GetChildSize() const {
	return capacity * array_size;
}

void VectorArrayBuffer::SetVectorSize(idx_t new_size) {
	VectorBuffer::SetVectorSize(new_size);
	if (vector_type == VectorType::CONSTANT_VECTOR) {
		FlatVector::SetSize(*child, array_size);
		return;
	}
	FlatVector::SetSize(*child, new_size * array_size);
}

void VectorArrayBuffer::SetVectorType(VectorType new_vector_type) {
	vector_type = new_vector_type;
}

void VectorArrayBuffer::ResetCapacity(idx_t capacity) {
	this->capacity = capacity;
	validity.Reset(capacity);
}

idx_t VectorArrayBuffer::GetDataSize(const LogicalType &type, idx_t count) const {
	idx_t size = VectorBuffer::GetAllocationSize();
	size += validity.GetAllocationSize();
	size += child->GetDataSize(count * array_size);
	return size;
}

idx_t VectorArrayBuffer::GetAllocationSize() const {
	idx_t size = VectorBuffer::GetAllocationSize();
	size += validity.GetAllocationSize();
	size += child->GetAllocationSize();
	return size;
}

void VectorArrayBuffer::VerifyInternal(const LogicalType &type, const SelectionVector &sel, idx_t count) const {
	D_ASSERT(type.InternalType() == PhysicalType::ARRAY);
	if (!sel.IsSet() && count == Size()) {
		child->Verify();
		return;
	}
	// flat vector case - only verify children for valid (non-NULL) entries
	idx_t selected_child_count = 0;
	for (idx_t i = 0; i < count; i++) {
		auto oidx = sel.get_index(i);
		if (validity.RowIsValid(oidx)) {
			selected_child_count += array_size;
		}
	}
	SelectionVector child_sel(selected_child_count);
	idx_t child_count = 0;
	for (idx_t i = 0; i < count; i++) {
		auto oidx = sel.get_index(i);
		if (validity.RowIsValid(oidx)) {
			for (idx_t r = 0; r < array_size; r++) {
				child_sel.set_index(child_count++, oidx * array_size + r);
			}
		}
	}
	child->Verify(child_sel, child_count);
	// FIXME: verify NULL-ness in child
}

buffer_ptr<VectorBuffer> VectorArrayBuffer::Flatten(const LogicalType &type) const {
	if (vector_type == VectorType::FLAT_VECTOR) {
		// already flat - recursively flatten the child vector
		child->Flatten();
		return nullptr;
	}
	return FlattenSlice(type, *FlatVector::IncrementalSelectionVector(), Size());
}

buffer_ptr<VectorBuffer> VectorArrayBuffer::FlattenSliceInternal(const LogicalType &type, const SelectionVector &sel,
                                                                 idx_t count) const {
	// now flatten the child vector
	auto target_child_size = count * array_size;
	auto child_result = make_uniq<Vector>(Vector::Ref(*child));

	SelectionVector child_sel(target_child_size);
	for (idx_t array_idx = 0; array_idx < count; array_idx++) {
		auto src_array_idx = sel.get_index(array_idx);
		for (idx_t elem_idx = 0; elem_idx < array_size; elem_idx++) {
			auto position = array_idx * array_size + elem_idx;
			auto src_position = src_array_idx * array_size + elem_idx;
			child_sel.set_index(position, src_position);
		}
	}
	// flatten the child using the child selection vector
	child_result->Flatten(child_sel, target_child_size);

	// construct the result
	auto result = make_buffer<VectorArrayBuffer>(std::move(child_result), array_size, capacity_t(count));

	// copy over the validity
	auto &result_validity = result->GetValidityMask();
	result_validity.CopySel(validity, sel, 0, 0, count);

	result->SetVectorSize(count);
	return result;
}

buffer_ptr<VectorBuffer> VectorArrayBuffer::SliceInternal(const LogicalType &type, idx_t offset, idx_t end) {
	auto count = count_t(end - offset);
	auto new_child = make_uniq<Vector>(*child, offset * array_size, end * array_size);

	auto result = make_buffer<VectorArrayBuffer>(std::move(new_child), array_size, capacity_t(count));
	result->GetValidityMask().Slice(validity, offset, count);
	result->SetVectorSize(count);
	return result;
}

buffer_ptr<VectorBuffer> VectorArrayBuffer::ConstantSliceInternal(const LogicalType &type, count_t count) {
	auto child_vector = make_uniq<Vector>(Vector::Ref(*child));
	auto result = make_buffer<VectorArrayBuffer>(std::move(child_vector), array_size, capacity_t(1ULL));
	result->GetValidityMask().Set(0, validity.RowIsValid(0));
	result->SetVectorType(VectorType::CONSTANT_VECTOR);
	result->SetVectorSize(count);
	return result;
}

buffer_ptr<VectorBuffer> VectorArrayBuffer::ConstantReferenceInternal(const LogicalType &type, idx_t index,
                                                                      count_t count) const {
	if (!validity.RowIsValid(index)) {
		// the elements of a NULL constant array are NULL as well
		return VectorBuffer::ConstantReferenceInternal(type, index, count);
	}
	// the constant holds the elements of the array, flattened from the child
	SelectionVector child_sel(array_size);
	for (idx_t elem_idx = 0; elem_idx < array_size; elem_idx++) {
		child_sel.set_index(elem_idx, index * array_size + elem_idx);
	}
	auto result_child = make_uniq<Vector>(Vector::Ref(*child));
	result_child->Flatten(child_sel, array_size);
	auto result = make_buffer<VectorArrayBuffer>(std::move(result_child), array_size, capacity_t(1ULL));
	result->SetVectorType(VectorType::CONSTANT_VECTOR);
	result->SetVectorSize(count);
	return result;
}

void VectorArrayBuffer::ReserveInternal(idx_t new_size) {
	// resize the validity
	validity.Resize(new_size);
	// resize the child
	child->Reserve(new_size * array_size);
	capacity = new_size;
}

void VectorArrayBuffer::ToUnifiedFormat(UnifiedVectorFormat &format) const {
	if (vector_type == VectorType::CONSTANT_VECTOR) {
		format.sel = ConstantVector::ZeroSelectionVector(Size(), format.owned_sel);
	} else {
		format.sel = FlatVector::IncrementalSelectionVector();
	}
	format.data = nullptr;
	format.validity = validity;
}

void VectorArrayBuffer::CopyInternal(const Vector &source, const SelectionVector &source_sel, idx_t source_count,
                                     idx_t source_offset, idx_t target_offset, idx_t copy_count) {
	D_ASSERT(ArrayType::GetSize(source.GetType()) == array_size);

	auto &source_child = ArrayVector::GetChild(source);

	// Create a selection vector for the child elements
	SelectionVector child_sel(copy_count * array_size);
	for (idx_t i = 0; i < copy_count; i++) {
		auto source_idx = source_sel.get_index(source_offset + i);
		for (idx_t j = 0; j < array_size; j++) {
			child_sel.set_index(i * array_size + j, source_idx * array_size + j);
		}
	}
	child->Copy(source_child, child_sel, source_count * array_size, 0, target_offset * array_size,
	            copy_count * array_size);
}

void VectorArrayBuffer::SetValue(const LogicalType &type, idx_t index, const Value &val) {
	if (!val.IsNull() && val.type() != type) {
		SetValue(type, index, val.DefaultCastAs(type));
		return;
	}
	validity.Set(index, !val.IsNull());
	if (val.IsNull()) {
		for (idx_t i = 0; i < array_size; i++) {
			child->SetValue(index * array_size + i, Value());
		}
	} else {
		auto &val_children = ArrayValue::GetChildren(val);
		for (idx_t i = 0; i < array_size; i++) {
			child->SetValue(index * array_size + i, val_children[i]);
		}
	}
}

Value VectorArrayBuffer::GetValue(const LogicalType &type, idx_t index) const {
	if (vector_type == VectorType::CONSTANT_VECTOR) {
		index = 0;
	}
	if (!validity.RowIsValid(index)) {
		return Value(type);
	}
	auto stride = ArrayType::GetSize(type);
	auto offset = index * stride;
	duckdb::vector<Value> children;
	for (idx_t i = offset; i < offset + stride; i++) {
		children.push_back(child->GetValue(i));
	}
	return Value::ARRAY(ArrayType::GetChildType(type), std::move(children));
}

template <class T>
T &ArrayVector::GetEntryInternal(T &vector) {
	D_ASSERT(vector.GetType().id() == LogicalTypeId::ARRAY);
	if (vector.GetVectorType() == VectorType::DICTIONARY_VECTOR) {
		auto &child = DictionaryVector::Child(vector);
		return GetEntryInternal<T>(child);
	}
	D_ASSERT(vector.GetVectorType() == VectorType::FLAT_VECTOR ||
	         vector.GetVectorType() == VectorType::CONSTANT_VECTOR);
	D_ASSERT(vector.GetBufferRef());
	D_ASSERT(vector.Buffer().GetBufferType() == VectorBufferType::ARRAY_BUFFER);
	return vector.GetBufferRef()->template Cast<VectorArrayBuffer>().GetChild();
}

const Vector &ArrayVector::GetChild(const Vector &vector) {
	return GetEntryInternal<const Vector>(vector);
}

Vector &ArrayVector::GetChildMutable(Vector &vector) {
	return GetEntryInternal<Vector>(vector);
}

const Vector &ArrayVector::GetEntry(const Vector &vector) {
	return GetChild(vector);
}

Vector &ArrayVector::GetEntry(Vector &vector) {
	return GetChildMutable(vector);
}

idx_t ArrayVector::GetTotalSize(const Vector &vector) {
	D_ASSERT(vector.GetType().id() == LogicalTypeId::ARRAY);
	if (vector.GetVectorType() == VectorType::DICTIONARY_VECTOR) {
		auto &child = DictionaryVector::Child(vector);
		return ArrayVector::GetTotalSize(child);
	}
	return vector.Buffer().Cast<VectorArrayBuffer>().GetChildSize();
}

} // namespace duckdb
