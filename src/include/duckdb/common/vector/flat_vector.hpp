//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/vector/flat_vector.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector/constant_vector.hpp"

namespace duckdb {

class StandardVectorBuffer : public VectorBuffer {
public:
	StandardVectorBuffer(Allocator &allocator, capacity_t capacity, idx_t type_size);
	StandardVectorBuffer(AllocatedData allocation, capacity_t capacity, idx_t type_size);
	explicit StandardVectorBuffer(capacity_t capacity, idx_t type_size);
	explicit StandardVectorBuffer(data_ptr_t data_ptr_p, count_t count, idx_t type_size);
	explicit StandardVectorBuffer(AllocatedData &&data_p, count_t count, idx_t type_size);

public:
	data_ptr_t GetData() override {
		return data_ptr;
	}
	data_ptr_t GetDataMutable() override;
	idx_t Capacity() const override {
		return capacity;
	}
	void ResetCapacity(idx_t capacity) override;
	ValidityMask &GetValidityMask() override {
		EnsureDataAllocated();
		return validity;
	}
	const ValidityMask &GetValidityMask() const override {
		return validity;
	}
	void SetVectorType(VectorType vector_type) override;

	optional_ptr<Allocator> GetAllocator() const override {
		return allocated_data.GetAllocator();
	}

public:
	idx_t GetAllocationSize() const override;
	void ToUnifiedFormat(UnifiedVectorFormat &format) const override;
	buffer_ptr<VectorBuffer> Flatten(const LogicalType &type) const override;
	Value GetValue(const LogicalType &type, idx_t index) const override;
	void SetValue(const LogicalType &type, idx_t index, const Value &val) override;

protected:
	DUCKDB_API void EnsureDataAllocated();

	buffer_ptr<VectorBuffer> SliceInternal(const LogicalType &type, idx_t offset, idx_t end) override;
	buffer_ptr<VectorBuffer> SliceInternal(const LogicalType &type, const SelectionVector &sel, idx_t count) override;
	buffer_ptr<VectorBuffer> ConstantSliceInternal(const LogicalType &type, count_t count) override;
	void CopyInternal(const Vector &source, const SelectionVector &source_sel, idx_t source_count, idx_t source_offset,
	                  idx_t target_offset, idx_t copy_count) override;
	buffer_ptr<VectorBuffer> FlattenSliceInternal(const LogicalType &type, const SelectionVector &sel,
	                                              idx_t count) const override;
	void VerifyInternal(const LogicalType &type, const SelectionVector &sel, idx_t count) const override;

	virtual buffer_ptr<VectorBuffer> CreateBuffer(AllocatedData &&new_data, count_t count) const;
	void ReserveInternal(idx_t new_size) override;

protected:
	friend struct FlatVector;

	ValidityMask validity;
	data_ptr_t data_ptr;
	idx_t type_size;
	idx_t capacity;
	AllocatedData allocated_data;
};

template <class T>
struct VectorWriter;
template <class T>
struct VectorScatterWriter;

struct FlatVector {
	static void VerifyFlatVector(const Vector &vector) {
#ifdef DUCKDB_DEBUG_NO_SAFETY
		D_ASSERT(vector.GetVectorType() == VectorType::FLAT_VECTOR);
#else
		if (vector.GetVectorType() != VectorType::FLAT_VECTOR) {
			throw InternalException("Operation requires a flat vector but a non-flat vector was encountered");
		}
#endif
	}
	static void VerifyFlatOrConst(const Vector &vector) {
#ifdef DUCKDB_DEBUG_NO_SAFETY
		D_ASSERT(vector.GetVectorType() == VectorType::CONSTANT_VECTOR ||
		         vector.GetVectorType() == VectorType::FLAT_VECTOR);
#else
		if (vector.GetVectorType() != VectorType::CONSTANT_VECTOR &&
		    vector.GetVectorType() != VectorType::FLAT_VECTOR) {
			throw InternalException(
			    "Operation requires a flat or constant vector but a non-flat/non-constant vector was encountered");
		}
#endif
	}
	static inline const_data_ptr_t GetData(Vector &vector) {
		VerifyFlatOrConst(vector);
		return GetDataUnsafe(vector);
	}
	static inline const_data_ptr_t GetData(const Vector &vector) {
		VerifyFlatOrConst(vector);
		return GetDataUnsafe(vector);
	}
	static inline data_ptr_t GetDataMutable(Vector &vector) {
		VerifyFlatOrConst(vector);
		return GetDataMutableUnsafe(vector);
	}
	static inline const_data_ptr_t GetDataUnsafe(const Vector &vector) {
		return vector.GetBufferRef() ? vector.GetBufferRef()->GetData() : nullptr;
	}
	static inline data_ptr_t GetDataMutableUnsafe(Vector &vector) {
		return vector.GetBufferRef() ? vector.BufferMutable().GetDataMutable() : nullptr;
	}
	template <class T>
	static inline const T *GetData(const Vector &vector) {
		ConstantVector::VerifyVectorType<T>(vector);
		return GetDataUnsafe<T>(vector);
	}
	template <class T>
	static inline T *GetDataMutable(Vector &vector) {
		ConstantVector::VerifyVectorType<T>(vector);
		return GetDataMutableUnsafe<T>(vector);
	}
	static inline idx_t GetCapacity(const Vector &vector) {
		auto &buffer_ref = vector.GetBufferRef();
		if (!buffer_ref) {
			return 0;
		}
		auto &buffer = *buffer_ref;
		if (buffer.GetVectorType() != VectorType::FLAT_VECTOR) {
			throw InternalException("FlatVector::GetCapacity requires a flat vector buffer");
		}
		return buffer.Capacity();
	}
	static void SetSize(Vector &vector, idx_t new_size) {
		auto &buffer_ref = vector.GetBufferRef();
		if (!buffer_ref) {
			if (new_size != 0) {
				throw InternalException("Calling FlatVector::SetSize on a vector without a buffer with non-zero size");
			}
			return;
		}
		buffer_ref->SetVectorSize(new_size);
	}
	template <class T>
	static inline const T *GetDataUnsafe(const Vector &vector) {
		return reinterpret_cast<const T *>(GetData(vector));
	}
	template <class T>
	static inline T *GetDataMutableUnsafe(Vector &vector) {
		return reinterpret_cast<T *>(GetDataMutableUnsafe(vector));
	}
	static void SetData(Vector &vector, data_ptr_t data, count_t count);
	template <class T>
	static inline T GetValue(Vector &vector, idx_t idx) {
		VerifyFlatVector(vector);
		return FlatVector::GetData<T>(vector)[idx];
	}
	static inline const ValidityMask &Validity(const Vector &vector) {
		VerifyFlatVector(vector);
		return vector.Buffer().GetValidityMask();
	}
	static inline ValidityMask &ValidityMutable(Vector &vector) {
		VerifyFlatVector(vector);
		return vector.BufferMutable().GetValidityMask();
	}
	static inline void SetValidity(Vector &vector, const ValidityMask &new_validity) {
		VerifyFlatVector(vector);
		auto &validity = vector.BufferMutable().GetValidityMask();
		validity.Initialize(new_validity);
	}
	static void CopyValidity(Vector &vector, const Vector &source, idx_t count);
	DUCKDB_API static void SetNull(Vector &vector, idx_t idx, bool is_null);
	static inline bool IsNull(const Vector &vector, idx_t idx) {
		D_ASSERT(vector.GetVectorType() == VectorType::FLAT_VECTOR);
		auto &validity = vector.Buffer().GetValidityMask();
		return !validity.RowIsValid(idx);
	}
	DUCKDB_API static const SelectionVector *IncrementalSelectionVector();

	template <class T>
	static VectorWriter<T> Writer(Vector &vector, idx_t count, idx_t offset) {
		PrepareForWrite(vector, offset + count);
		return VectorWriter<T>(vector, count, offset);
	}
	template <class T>
	static VectorWriter<T> Writer(Vector &vector, idx_t count) {
		return Writer<T>(vector, count, 0ULL);
	}
	template <class T>
	static VectorScatterWriter<T> ScatterWriter(Vector &vector) {
		return VectorScatterWriter<T>(vector);
	}

private:
	DUCKDB_API static void PrepareForWrite(Vector &vector, idx_t size);
};

} // namespace duckdb

#include "duckdb/common/vector/vector_writer.hpp"
