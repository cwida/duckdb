//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/vector/array_vector.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/vector.hpp"

namespace duckdb {

class VectorArrayBuffer : public VectorBuffer {
public:
	explicit VectorArrayBuffer(unique_ptr<Vector> child_vector, idx_t array_size, capacity_t initial_capacity);
	explicit VectorArrayBuffer(const LogicalType &array, capacity_t initial = capacity_t(STANDARD_VECTOR_SIZE));
	~VectorArrayBuffer() override;

public:
	ValidityMask &GetValidityMask() override {
		return validity;
	}
	void SetVectorSize(idx_t new_size) override;
	idx_t Capacity() const override {
		return capacity;
	}
	void ResetCapacity(idx_t capacity) override;
	const ValidityMask &GetValidityMask() const override {
		return validity;
	}
	Vector &GetChild();
	idx_t GetArraySize() const;
	idx_t GetChildSize() const;
	void SetVectorType(VectorType vector_type) override;

public:
	idx_t GetDataSize(const LogicalType &type, idx_t count) const override;
	idx_t GetAllocationSize() const override;
	buffer_ptr<VectorBuffer> Flatten(const LogicalType &type) const override;
	void ToUnifiedFormat(UnifiedVectorFormat &format) const override;
	Value GetValue(const LogicalType &type, idx_t index) const override;
	void SetValue(const LogicalType &type, idx_t index, const Value &val) override;

protected:
	buffer_ptr<VectorBuffer> SliceInternal(const LogicalType &type, idx_t offset, idx_t end) override;
	buffer_ptr<VectorBuffer> ConstantSliceInternal(const LogicalType &type, count_t count) override;
	buffer_ptr<VectorBuffer> ConstantReferenceInternal(const LogicalType &type, idx_t index,
	                                                   count_t count) const override;
	void CopyInternal(const Vector &source, const SelectionVector &source_sel, idx_t source_count, idx_t source_offset,
	                  idx_t target_offset, idx_t copy_count) override;
	buffer_ptr<VectorBuffer> FlattenSliceInternal(const LogicalType &type, const SelectionVector &sel,
	                                              idx_t count) const override;
	void VerifyInternal(const LogicalType &type, const SelectionVector &sel, idx_t count) const override;
	void ReserveInternal(idx_t new_size) override;

private:
	ValidityMask validity;
	unique_ptr<Vector> child;
	// The size of each array in this buffer
	idx_t array_size = 0;
	// How many arrays are currently stored in this buffer
	// The child vector has size (array_size * size)
	idx_t capacity = 0;
};

struct ArrayVector {
	//! Gets a reference to the underlying child-vector of an array
	[[deprecated("Use ArrayVector::GetChild instead")]] DUCKDB_API static const Vector &GetEntry(const Vector &vector);
	//! Gets a reference to the underlying child-vector of an array
	[[deprecated("Use ArrayVector::GetChild or ArrayVector::GetChildMutable instead")]] DUCKDB_API static Vector &
	GetEntry(Vector &vector);
	//! Gets a reference to the underlying child-vector of an array
	DUCKDB_API static const Vector &GetChild(const Vector &vector);
	//! Gets a mutable reference to the underlying child-vector of an array
	DUCKDB_API static Vector &GetChildMutable(Vector &vector);
	//! Gets the total size of the underlying child-vector of an array
	DUCKDB_API static idx_t GetTotalSize(const Vector &vector);

private:
	template <class T>
	static T &GetEntryInternal(T &vector);
};

} // namespace duckdb
