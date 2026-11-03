//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/vector/constant_vector.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/vector.hpp"

namespace duckdb {

struct ConstantVector {
	template <class T>
	static void VerifyVectorType(const Vector &vector) {
#ifdef DUCKDB_DEBUG_NO_SAFETY
		D_ASSERT(StorageTypeCompatible<T>(vector.GetType().InternalType()));
#else
		if (!StorageTypeCompatible<T>(vector.GetType().InternalType())) {
			ThrowVectorTypeMismatch(GetTypeId<T>(), vector.GetType().InternalType());
		}
#endif
	}
	[[noreturn]] DUCKDB_API static void ThrowVectorTypeMismatch(PhysicalType expected, PhysicalType found);

	static void VerifyConstantVector(const Vector &vector) {
#ifdef DUCKDB_DEBUG_NO_SAFETY
		D_ASSERT(vector.GetVectorType() == VectorType::CONSTANT_VECTOR);
#else
		if (vector.GetVectorType() != VectorType::CONSTANT_VECTOR) {
			throw InternalException("Operation requires a constant vector but a non-constant vector was encountered");
		}
#endif
	}

	static inline const_data_ptr_t GetData(const Vector &vector) {
		VerifyConstantVector(vector);
		return vector.GetBufferRef() ? vector.GetBufferRef()->GetData() : nullptr;
	}
	static inline data_ptr_t GetData(Vector &vector) {
		VerifyConstantVector(vector);
		return vector.GetBufferRef() ? vector.BufferMutable().GetDataMutable() : nullptr;
	}
	template <class T>
	static inline const T *GetDataUnsafe(const Vector &vector) {
		return reinterpret_cast<const T *>(GetData(vector));
	}
	template <class T>
	static inline T *GetDataUnsafe(Vector &vector) {
		return reinterpret_cast<T *>(GetData(vector));
	}
	template <class T>
	static inline const T *GetData(const Vector &vector) {
		VerifyVectorType<T>(vector);
		return GetDataUnsafe<T>(vector);
	}
	template <class T>
	static inline T *GetData(Vector &vector) {
		VerifyVectorType<T>(vector);
		return GetDataUnsafe<T>(vector);
	}
	static inline bool IsNull(const Vector &vector) {
		D_ASSERT(vector.GetVectorType() == VectorType::CONSTANT_VECTOR);
		auto &validity = vector.Buffer().GetValidityMask();
		return !validity.RowIsValid(0);
	}
	//! Sets a vector to be a constant NULL vector
	DUCKDB_API static void SetNull(Vector &vector, count_t count);
	DUCKDB_API static void SetNull(Vector &vector, bool is_null);
	static inline ValidityMask &Validity(Vector &vector) {
		D_ASSERT(vector.GetVectorType() == VectorType::CONSTANT_VECTOR);
		auto &validity = vector.BufferMutable().GetValidityMask();
		return validity;
	}
	static inline const ValidityMask &Validity(const Vector &vector) {
		D_ASSERT(vector.GetVectorType() == VectorType::CONSTANT_VECTOR);
		auto &validity = vector.Buffer().GetValidityMask();
		return validity;
	}
	DUCKDB_API static const SelectionVector *ZeroSelectionVector(idx_t count, SelectionVector &owned_sel);
	DUCKDB_API static const SelectionVector *ZeroSelectionVector();
	//! Turns "vector" into a constant vector by referencing a value
	DUCKDB_API static void Reference(Vector &vector, const Value &value, count_t count);
	//! Turns "vector" into a constant vector by referencing a value within the source vector
	DUCKDB_API static void Reference(Vector &vector, count_t count, const Vector &source, idx_t position,
	                                 idx_t source_count);

	static const sel_t ZERO_VECTOR[STANDARD_VECTOR_SIZE];
};

} // namespace duckdb
