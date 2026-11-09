//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/vector/struct_vector.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"

namespace duckdb {

class VectorStructBuffer : public VectorBuffer {
public:
	explicit VectorStructBuffer(const LogicalType &struct_type, capacity_t capacity = capacity_t(STANDARD_VECTOR_SIZE));
	VectorStructBuffer(vector<Vector> children, capacity_t capacity);
	VectorStructBuffer(VectorStructBuffer &other, const SelectionVector &sel, count_t count);
	~VectorStructBuffer() override;

public:
	ValidityMask &GetValidityMask() override {
		return validity;
	}
	void SetVectorSize(idx_t new_size) override;
	idx_t Capacity() const override {
		return capacity;
	}
	void SetCapacity(idx_t new_capacity) {
		capacity = new_capacity;
	}
	void ResetCapacity(idx_t capacity) override;
	const ValidityMask &GetValidityMask() const override {
		return validity;
	}
	const vector<Vector> &GetChildren() const {
		return children;
	}
	vector<Vector> &GetChildren() {
		return children;
	}
	void SetVectorType(VectorType vector_type) override;
	void PrepareChildrenForSetConstant();

public:
	idx_t GetDataSize(const LogicalType &type, idx_t count) const override;
	idx_t GetAllocationSize() const override;
	void ToUnifiedFormat(UnifiedVectorFormat &format) const override;
	buffer_ptr<VectorBuffer> Flatten(const LogicalType &type) const override;
	Value GetValue(const LogicalType &type, idx_t index) const override;
	void SetValue(const LogicalType &type, idx_t index, const Value &val) override;

protected:
	buffer_ptr<VectorBuffer> SliceInternal(const LogicalType &type, idx_t offset, idx_t end) override;
	buffer_ptr<VectorBuffer> SliceInternal(const LogicalType &type, const SelectionVector &sel, idx_t count) override;
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
	//! child vectors used for nested data
	vector<Vector> children;
	//! The capacity of the struct
	idx_t capacity;
};

struct StructVector {
	DUCKDB_API static const vector<Vector> &GetEntries(const Vector &vector);
	DUCKDB_API static vector<Vector> &GetEntries(Vector &vector);
};

} // namespace duckdb
