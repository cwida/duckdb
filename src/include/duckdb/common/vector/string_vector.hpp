//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/vector/string_vector.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"

namespace duckdb {

struct StringHeapHolder : AuxiliaryDataHolder {
	explicit StringHeapHolder(Allocator &allocator) : heap(allocator) {
	}

	idx_t GetAllocationSize() const override;

	StringHeap heap;
};

class VectorStringBuffer : public StandardVectorBuffer {
public:
	VectorStringBuffer();
	explicit VectorStringBuffer(Allocator &allocator);
	VectorStringBuffer(Allocator &allocator, capacity_t capacity);
	VectorStringBuffer(AllocatedData allocation, capacity_t capacity);
	explicit VectorStringBuffer(capacity_t capacity);
	explicit VectorStringBuffer(data_ptr_t data_ptr_p, count_t count);
	explicit VectorStringBuffer(AllocatedData &&data_p, count_t count);
	VectorStringBuffer(AllocatedData &&data_p, count_t count, const VectorStringBuffer &other);

public:
	StringHeap &GetHeap() {
		if (heap) {
			return *heap;
		}
		auto allocator = GetAllocator();
		if (allocator) {
			heap = AllocateHeap(*allocator);
		} else {
			heap = AllocateHeap();
		}
		return *heap;
	}

	void ClearAuxiliaryData() override {
		StandardVectorBuffer::ClearAuxiliaryData();
		heap = nullptr;
	}

	ArenaAllocator &GetStringAllocator() {
		return GetHeap().GetAllocator();
	}

public:
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
	StringHeap &AllocateHeap(Allocator &allocator);
	StringHeap &AllocateHeap();

private:
	optional_ptr<StringHeap> heap;
};

struct StringVector {
	//! Add a string to the string heap of the vector (auxiliary data)
	DUCKDB_API static string_t AddString(Vector &vector, const char *data, idx_t len);
	//! Add a string or a blob to the string heap of the vector (auxiliary data)
	//! This function is the same as ::AddString, except the added data does not need to be valid UTF8
	DUCKDB_API static string_t AddStringOrBlob(Vector &vector, const char *data, idx_t len);
	//! Add a string to the string heap of the vector (auxiliary data)
	DUCKDB_API static string_t AddString(Vector &vector, const char *data);
	//! Add a string to the string heap of the vector (auxiliary data)
	DUCKDB_API static string_t AddString(Vector &vector, string_t data);
	//! Add a string to the string heap of the vector (auxiliary data)
	DUCKDB_API static string_t AddString(Vector &vector, const string &data);
	//! Add a string or a blob to the string heap of the vector (auxiliary data)
	//! This function is the same as ::AddString, except the added data does not need to be valid UTF8
	DUCKDB_API static string_t AddStringOrBlob(Vector &vector, string_t data);
	//! Allocates an empty string of the specified size, and returns a writable pointer that can be used to store the
	//! result of an operation
	DUCKDB_API static string_t EmptyString(Vector &vector, idx_t len);
	//! Returns a reference to the underlying VectorStringBuffer - throws an error if vector is not of type VARCHAR
	DUCKDB_API static VectorStringBuffer &GetStringBuffer(Vector &vector);
	//! Returns a reference to the string allocator
	DUCKDB_API static ArenaAllocator &GetStringAllocator(Vector &vector);
	//! Returns a reference to the string heap
	DUCKDB_API static StringHeap &GetStringHeap(Vector &vector);
	//! Adds a reference to a handle that stores strings of this vector
	DUCKDB_API static void AddHandle(Vector &vector, BufferHandle handle);
	//! Add a reference from this vector to the string heap of the provided vector
	DUCKDB_API static void AddHeapReference(Vector &vector, const Vector &other);
	//! Add a reference from this vector to the auxiliary data
	DUCKDB_API static void AddAuxiliaryData(Vector &vector, unique_ptr<AuxiliaryDataHolder> data);

	//! Allocate a buffer to store up to "len" bytes for a string
	//! This can be turned into a proper string by using FinalizeBuffer afterwards
	//! Note that alloc_len only has to be an upper bound, the final string may be smaller
	static inline data_ptr_t AllocateShrinkableBuffer(ArenaAllocator &allocator, idx_t alloc_len) {
		return allocator.Allocate(alloc_len);
	}
	//! Finalize a buffer allocated with AllocateShrinkableBuffer into a string of size str_len
	//! str_len must be <= alloc_len
	static inline string_t FinalizeShrinkableBuffer(ArenaAllocator &allocator, data_ptr_t buffer, idx_t alloc_len,
	                                                idx_t str_len) {
		D_ASSERT(str_len <= alloc_len);
		D_ASSERT(buffer == allocator.GetHead()->data.get() + allocator.GetHead()->current_position - alloc_len);
		bool is_not_inlined = str_len > string_t::INLINE_LENGTH;
		idx_t shrink_count = alloc_len - (str_len * is_not_inlined);
		allocator.ShrinkHead(shrink_count);
		return string_t(const_char_ptr_cast(buffer), UnsafeNumericCast<uint32_t>(str_len));
	}
};

} // namespace duckdb
