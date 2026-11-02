#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/enums/vector_type.hpp"
#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/vector/dictionary_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/union_vector.hpp"
#include "duckdb/common/vector/variant_vector.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"

namespace duckdb {

VectorStructBuffer::VectorStructBuffer(const LogicalType &type, capacity_t capacity)
    : VectorBuffer(VectorType::FLAT_VECTOR, VectorBufferType::STRUCT_BUFFER, count_t(0)), capacity(capacity) {
	auto &child_types = StructType::GetChildTypes(type);
	for (auto &child_type : child_types) {
		children.emplace_back(child_type.second, capacity);
	}
	validity.Resize(capacity);
}

VectorStructBuffer::VectorStructBuffer(vector<Vector> children_p, capacity_t capacity_p)
    : VectorBuffer(VectorType::FLAT_VECTOR, VectorBufferType::STRUCT_BUFFER, count_t(0)),
      children(std::move(children_p)), capacity(capacity_p) {
	validity.Resize(capacity);
}

VectorStructBuffer::VectorStructBuffer(VectorStructBuffer &other, const SelectionVector &sel, count_t count)
    : VectorBuffer(VectorType::FLAT_VECTOR, VectorBufferType::STRUCT_BUFFER, count),
      capacity(MaxValue<idx_t>(count, STANDARD_VECTOR_SIZE)) {
	auto &other_vector = other.children;
	for (auto &child_vector : other_vector) {
		children.emplace_back(child_vector, sel, count);
	}
	// slice the validity mask of the original struct
	auto &original_validity = other.GetValidityMask();
	if (count > STANDARD_VECTOR_SIZE) {
		validity.Resize(count);
	}
	validity.CopySel(original_validity, sel, 0, 0, count);
}

void VectorStructBuffer::SetVectorType(VectorType new_vector_type) {
	vector_type = new_vector_type;
	for (auto &child : children) {
		child.SetVectorType(new_vector_type);
	}
}

void VectorStructBuffer::PrepareChildrenForSetConstant() {
	for (auto &child : children) {
		if (child.GetVectorType() != VectorType::FLAT_VECTOR && child.GetVectorType() != VectorType::CONSTANT_VECTOR) {
			child.Flatten();
		}
		if (child.GetType().InternalType() == PhysicalType::STRUCT) {
			child.BufferMutable().Cast<VectorStructBuffer>().PrepareChildrenForSetConstant();
		}
	}
}

VectorStructBuffer::~VectorStructBuffer() {
}

void VectorStructBuffer::SetVectorSize(idx_t new_size) {
	VectorBuffer::SetVectorSize(new_size);
	for (auto &child : children) {
		FlatVector::SetSize(child, new_size);
	}
}

void VectorStructBuffer::ResetCapacity(idx_t capacity_p) {
	this->capacity = capacity_p;
	validity.Reset(capacity);
}

idx_t VectorStructBuffer::GetDataSize(const LogicalType &type, idx_t count) const {
	idx_t size = validity.GetAllocationSize();
	for (auto &child : children) {
		size += child.GetDataSize(count);
	}
	return size;
}

idx_t VectorStructBuffer::GetAllocationSize() const {
	idx_t size = VectorBuffer::GetAllocationSize();
	size += validity.GetAllocationSize();
	for (auto &child : children) {
		size += child.GetAllocationSize();
	}
	return size;
}

void VectorStructBuffer::VerifyInternal(const LogicalType &type, const SelectionVector &sel, idx_t count) const {
	D_ASSERT(type.InternalType() == PhysicalType::STRUCT);
	D_ASSERT(vector_type == VectorType::FLAT_VECTOR || vector_type == VectorType::CONSTANT_VECTOR);
	auto &child_types = StructType::GetChildTypes(type);
	if (child_types.size() != children.size()) {
		throw InternalException("Struct child count mismatch - expected %d children but found %d", child_types.size(),
		                        children.size());
	}
	for (idx_t child_idx = 0; child_idx < children.size(); child_idx++) {
		auto &child = children[child_idx];
		if (child.GetType() != child_types[child_idx].second) {
			throw InternalException("Struct child type mismatch - expected child of type %s but found child of type %s",
			                        child_types[child_idx].second, child.GetType());
		}
		if (!sel.IsSet() && count == Size()) {
			child.Verify();
		} else {
			child.Verify(sel, count);
		}
		if (child.size() != Size()) {
			throw InternalException("Struct child size mismatch - parent struct had size %d but child has size %d",
			                        Size(), child.size());
		}
		// for any NULL entry in the struct, the child should be NULL as well
		// may produce structs where parent NULLs are not propagated to all children
		if (vector_type == VectorType::CONSTANT_VECTOR) {
			if (child.GetVectorType() != VectorType::CONSTANT_VECTOR) {
				throw InternalException(
				    "Struct constant mismatch - expected a child of a constant struct to also be constant");
			}
			if (!validity.RowIsValid(0)) {
				if (!ConstantVector::IsNull(child)) {
					throw InternalException("Struct NULL mismatch - a child of a NULL struct must always be NULL");
				}
			}
		} else {
			if (child.GetVectorType() == VectorType::FLAT_VECTOR ||
			    child.GetVectorType() == VectorType::CONSTANT_VECTOR) {
				auto child_validity = child.Validity();
				for (idx_t r = 0; r < Size(); r++) {
					if (!validity.RowIsValid(r)) {
						if (child_validity.IsValid(r)) {
							throw InternalException("Struct NULL mismatch - a child of a NULL struct must always be "
							                        "NULL\nStruct type: %s\nChild idx: %llu, Child type: %s\nRow: %llu",
							                        type.ToString(), (uint64_t)child_idx,
							                        child_types[child_idx].second.ToString(), (uint64_t)r);
						}
					}
				}
			}
		}
	}
}

buffer_ptr<VectorBuffer> VectorStructBuffer::SliceInternal(const LogicalType &type, idx_t offset, idx_t end) {
	vector<Vector> result_children;
	for (idx_t i = 0; i < children.size(); i++) {
		result_children.emplace_back(children[i], offset, end);
	}
	auto count = count_t(end - offset);
	auto result = make_buffer<VectorStructBuffer>(std::move(result_children), capacity_t(count));
	result->GetValidityMask().Slice(validity, offset, count);
	result->SetVectorSize(count);
	return result;
}

buffer_ptr<VectorBuffer> VectorStructBuffer::SliceInternal(const LogicalType &type, const SelectionVector &sel,
                                                           idx_t count) {
	return make_buffer<VectorStructBuffer>(*this, sel, count_t(count));
}

buffer_ptr<VectorBuffer> VectorStructBuffer::ConstantSliceInternal(const LogicalType &type, count_t count) {
	vector<Vector> result_children;
	for (idx_t i = 0; i < children.size(); i++) {
		result_children.emplace_back(children[i], *ConstantVector::ZeroSelectionVector(), count);
	}
	auto result = make_buffer<VectorStructBuffer>(std::move(result_children), capacity_t(1ULL));
	result->GetValidityMask().Set(0, validity.RowIsValid(0));
	result->SetVectorType(VectorType::CONSTANT_VECTOR);
	result->SetVectorSize(count);
	return result;
}

void VectorStructBuffer::ToUnifiedFormat(UnifiedVectorFormat &format) const {
	if (vector_type == VectorType::CONSTANT_VECTOR) {
		format.sel = ConstantVector::ZeroSelectionVector(Size(), format.owned_sel);
	} else {
		format.sel = FlatVector::IncrementalSelectionVector();
	}
	format.data = nullptr;
	format.validity = validity;
}

void VectorStructBuffer::SetValue(const LogicalType &type, idx_t index, const Value &val) {
	if (!val.IsNull() && val.type() != type) {
		SetValue(type, index, val.DefaultCastAs(type));
		return;
	}
	validity.Set(index, !val.IsNull());
	if (val.IsNull()) {
		for (auto &child : children) {
			child.SetValue(index, Value());
		}
	} else {
		auto &val_children = StructValue::GetChildren(val);
		D_ASSERT(children.size() == val_children.size());
		for (idx_t i = 0; i < children.size(); i++) {
			children[i].SetValue(index, val_children[i]);
		}
	}
}

Value VectorStructBuffer::GetValue(const LogicalType &type, idx_t index) const {
	if (vector_type == VectorType::CONSTANT_VECTOR) {
		index = 0;
	}
	if (!validity.RowIsValid(index)) {
		return Value(type);
	}
	switch (type.id()) {
	case LogicalTypeId::UNION: {
		// tag is the first child
		auto &tag_vector = children[0];
		auto tag_val = tag_vector.GetValue(index);
		if (tag_val.IsNull()) {
			return Value(type);
		}
		auto tag = tag_val.GetValue<union_tag_t>();
		// member is at tag + 1 (tag is child 0)
		if (static_cast<idx_t>(tag) + 1 >= children.size()) {
			throw DataCorruptionException("Union tag (%d) is out of bounds for a union with %d members", tag,
			                              children.size() - 1);
		}
		auto value = children[tag + 1].GetValue(index);
		auto members = UnionType::CopyMemberTypes(type);
		return Value::UNION(members, tag, std::move(value));
	}
	case LogicalTypeId::VARIANT: {
		duckdb::vector<Value> child_values;
		child_values.emplace_back(children[0].GetValue(index));
		child_values.emplace_back(children[1].GetValue(index));
		child_values.emplace_back(children[2].GetValue(index));
		child_values.emplace_back(children[3].GetValue(index));
		return Value::VARIANT(child_values);
	}
	default: {
		duckdb::vector<Value> child_values;
		for (idx_t i = 0; i < children.size(); i++) {
			child_values.push_back(children[i].GetValue(index));
		}
		return Value::STRUCT(type, std::move(child_values));
	}
	}
}

void VectorStructBuffer::ReserveInternal(idx_t new_size) {
	// resize over the validity
	validity.Resize(new_size);
	// resize the struct children
	for (auto &child : children) {
		child.Reserve(new_size);
	}
	capacity = new_size;
}

void VectorStructBuffer::CopyInternal(const Vector &source, const SelectionVector &source_sel, idx_t source_count,
                                      idx_t source_offset, idx_t target_offset, idx_t copy_count) {
	auto &source_children = StructVector::GetEntries(source);
	D_ASSERT(source_children.size() == children.size());
	for (idx_t i = 0; i < source_children.size(); i++) {
		children[i].Copy(source_children[i], source_sel, source_count, source_offset, target_offset, copy_count);
	}
}

buffer_ptr<VectorBuffer> VectorStructBuffer::Flatten(const LogicalType &type) const {
	if (GetVectorType() == VectorType::FLAT_VECTOR) {
		for (auto &child : children) {
			child.Flatten();
		}
		return nullptr;
	}
	return FlattenSlice(type, *FlatVector::IncrementalSelectionVector(), Size());
}

buffer_ptr<VectorBuffer> VectorStructBuffer::FlattenSliceInternal(const LogicalType &type, const SelectionVector &sel,
                                                                  idx_t count) const {
	// create a new flat struct buffer
	// flatten each child using the same sel
	vector<Vector> result_children;
	for (idx_t i = 0; i < children.size(); i++) {
		result_children.emplace_back(Vector::Ref(children[i]));
		result_children[i].Flatten(sel, count);
	}
	auto result = make_buffer<VectorStructBuffer>(std::move(result_children), capacity_t(count));
	// copy validity using sel
	auto &result_validity = result->GetValidityMask();
	result_validity.CopySel(validity, sel, 0, 0, count);
	result->SetVectorSize(count);
	return result;
}

vector<Vector> &StructVector::GetEntries(Vector &vector) {
	D_ASSERT(vector.GetType().id() == LogicalTypeId::STRUCT || vector.GetType().id() == LogicalTypeId::TUPLE ||
	         vector.GetType().id() == LogicalTypeId::UNION || vector.GetType().id() == LogicalTypeId::VARIANT);

	if (vector.GetVectorType() == VectorType::DICTIONARY_VECTOR) {
		throw InternalException("Struct vectors cannot be dictionary vectors");
	}
	if (vector.GetVectorType() != VectorType::FLAT_VECTOR && vector.GetVectorType() != VectorType::CONSTANT_VECTOR) {
		// non-flat representation (e.g. a shredded variant) - flatten so we can access the struct entries
		vector.Flatten();
	}
	D_ASSERT(vector.GetVectorType() == VectorType::FLAT_VECTOR ||
	         vector.GetVectorType() == VectorType::CONSTANT_VECTOR);
	D_ASSERT(vector.GetBufferRef());
	D_ASSERT(vector.Buffer().GetBufferType() == VectorBufferType::STRUCT_BUFFER);
	return vector.BufferMutable().Cast<VectorStructBuffer>().GetChildren();
}

const vector<Vector> &StructVector::GetEntries(const Vector &vector) {
	return GetEntries((Vector &)vector);
}

const vector<Vector> &VectorIteratorGetStructEntries(const Vector &vector) {
	return StructVector::GetEntries(vector);
}

vector<Vector> &VectorWriterGetStructEntries(Vector &vector) {
	return StructVector::GetEntries(vector);
}

} // namespace duckdb
