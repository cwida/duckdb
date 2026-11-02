//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/vector/list_vector.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector/dictionary_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"

namespace duckdb {

class VectorListBuffer : public StandardVectorBuffer {
public:
	explicit VectorListBuffer(Allocator &allocator, capacity_t capacity, unique_ptr<Vector> vector);
	explicit VectorListBuffer(AllocatedData allocated_data, capacity_t capacity, unique_ptr<Vector> vector);
	explicit VectorListBuffer(Allocator &allocator, capacity_t capacity, const LogicalType &list_type,
	                          capacity_t child_capacity = capacity_t(STANDARD_VECTOR_SIZE));
	explicit VectorListBuffer(capacity_t capacity, const LogicalType &list_type,
	                          capacity_t child_capacity = capacity_t(STANDARD_VECTOR_SIZE));
	explicit VectorListBuffer(data_ptr_t data, count_t count, const Vector &vector);
	explicit VectorListBuffer(data_ptr_t data, count_t count, const VectorListBuffer &parent);
	explicit VectorListBuffer(AllocatedData allocated_data, count_t count, const VectorListBuffer &parent);
	explicit VectorListBuffer(AllocatedData allocated_data, count_t count, VectorListBuffer &parent);
	~VectorListBuffer() override;

public:
	Vector &GetChild() {
		return *child;
	}
	const Vector &GetChild() const {
		return *child;
	}
	unique_ptr<Vector> &GetChildMutable() {
		return child;
	}
	void Reserve(idx_t to_reserve);

	void AppendToChild(const Vector &to_append, idx_t to_append_size);
	void AppendToChild(const Vector &to_append, const SelectionVector &sel, idx_t to_append_size);

	void PushBack(const Value &insert);

	idx_t GetChildSize() const {
		return child->size();
	}

	idx_t GetChildCapacity() const;
	void SetChildSize(idx_t new_size);

public:
	idx_t GetDataSize(const LogicalType &type, idx_t count) const override;
	idx_t GetAllocationSize() const override;
	void ToUnifiedFormat(UnifiedVectorFormat &format) const override;
	buffer_ptr<VectorBuffer> Flatten(const LogicalType &type) const override;
	Value GetValue(const LogicalType &type, idx_t index) const override;
	void SetValue(const LogicalType &type, idx_t index, const Value &val) override;

protected:
	buffer_ptr<VectorBuffer> SliceInternal(const LogicalType &type, idx_t offset, idx_t end) override;
	buffer_ptr<VectorBuffer> ConstantSliceInternal(const LogicalType &type, count_t count) override;
	buffer_ptr<VectorBuffer> CreateBuffer(AllocatedData &&new_data, count_t count) const override;
	void CopyInternal(const Vector &source, const SelectionVector &source_sel, idx_t source_count, idx_t source_offset,
	                  idx_t target_offset, idx_t copy_count) override;
	buffer_ptr<VectorBuffer> FlattenSliceInternal(const LogicalType &type, const SelectionVector &sel,
	                                              idx_t count) const override;
	void VerifyInternal(const LogicalType &type, const SelectionVector &sel, idx_t count) const override;

private:
	//! child vectors used for nested data
	unique_ptr<Vector> child;
};

struct ListVector {
	[[deprecated("Use FlatVector::GetData<list_entry_t> instead")]] static inline const list_entry_t *
	GetData(const Vector &v) {
		if (v.GetVectorType() == VectorType::DICTIONARY_VECTOR) {
			throw InternalException("ListVector::GetData called on dictionary vector");
		}
		return FlatVector::GetData<const list_entry_t>(v);
	}
	[[deprecated("Use FlatVector::GetData<list_entry_t> instead")]] static inline list_entry_t *GetData(Vector &v) {
		if (v.GetVectorType() == VectorType::DICTIONARY_VECTOR) {
			throw InternalException("ListVector::GetData called on dictionary vector");
		}
		return FlatVector::GetDataMutable<list_entry_t>(v);
	}
	//! Gets a reference to the underlying child-vector of a list
	[[deprecated("Use ListVector::GetChild instead")]] DUCKDB_API static const Vector &GetEntry(const Vector &vector);
	//! Gets a reference to the underlying child-vector of a list
	[[deprecated("Use ListVector::GetChild or ListVector::GetChildMutable instead")]] DUCKDB_API static Vector &
	GetEntry(Vector &vector);
	//! Gets a reference to the underlying child-vector of a list
	DUCKDB_API static const Vector &GetChild(const Vector &vector);
	//! Gets a mutable reference to the underlying child-vector of a list
	DUCKDB_API static Vector &GetChildMutable(Vector &vector);
	//! Gets the total size of the underlying child-vector of a list
	DUCKDB_API static idx_t GetListSize(const Vector &vector);
	//! Sets the total size of the underlying child-vector of a list
	DUCKDB_API static void SetListSize(Vector &vec, idx_t size);
	//! Gets the total capacity of the underlying child-vector of a list
	DUCKDB_API static idx_t GetListCapacity(const Vector &vector);
	//! Sets the total capacity of the underlying child-vector of a list
	DUCKDB_API static void Reserve(Vector &vec, idx_t required_capacity);
	DUCKDB_API static void Append(Vector &target, const Vector &source, idx_t source_size);
	DUCKDB_API static void Append(Vector &target, const Vector &source, const SelectionVector &sel, idx_t source_size);
	DUCKDB_API static void PushBack(Vector &target, const Value &insert);
	//! Returns the child_vector of list starting at offset until offset + count, and its length
	DUCKDB_API static idx_t GetConsecutiveChildList(const Vector &list, Vector &result, idx_t offset, idx_t count);
	//! Returns information to only copy a section of a list child vector
	DUCKDB_API static ConsecutiveChildListInfo GetConsecutiveChildListInfo(const Vector &list, idx_t offset,
	                                                                       idx_t count);
	//! Slice and flatten a child vector to only contain a consecutive subsection of the child entries
	DUCKDB_API static void GetConsecutiveChildSelVector(const Vector &list, SelectionVector &sel, idx_t offset,
	                                                    idx_t count);
	//! Returns the total number of entries in the list
	DUCKDB_API static idx_t GetTotalEntryCount(const Vector &list);

private:
	template <class T>
	static T &GetEntryInternal(T &vector);
};

} // namespace duckdb
