//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/vector_buffer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/types/selection_vector.hpp"
#include "duckdb/common/types/string_heap.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/storage/buffer/buffer_handle.hpp"
#include "duckdb/common/enums/vector_type.hpp"
#include "duckdb/common/types/size.hpp"

namespace duckdb {

class BufferHandle;
struct LogicalType;
struct ResizeInfo;
struct UnifiedVectorFormat;
class VectorBuffer;
class Vector;
struct ValidityMask;
struct SelCache;
class Serializer;
class Deserializer;

enum class VectorBufferType : uint8_t {
	STANDARD_BUFFER,   // VectorType::FLAT/CONSTANT - Fixed-Size Type - Holds a single array of data
	STRING_BUFFER,     // VectorType::FLAT/CONSTANT - String          - Holds string_t array and StringHeap
	STRUCT_BUFFER,     // VectorType::FLAT/CONSTANT - Struct          - Holds struct child vectors
	LIST_BUFFER,       // VectorType::FLAT/CONSTANT - List            - Holds list_entry_t array and list child vector
	ARRAY_BUFFER,      // VectorType::FLAT/CONSTANT - Array           - Holds array child vector
	DICTIONARY_BUFFER, // VectorType::DICTIONARY    - Any             - Holds SelectionVector and dict child vector
	FSST_BUFFER,       // VectorType::FSST          - String          - Holds string_t array, StringHeap and FSST table
	SHREDDED_BUFFER,   // VectorType::SHREDDED      - Variant         - Holds shredded variant
	SEQUENCE_BUFFER,   // VectorType::SEQUENCE      - Any             - Holds linear numeric sequence (start, increment)
	SHARED_DATA_BUFFER // VectorType::FLAT          - Fixed-Size Type - Holds immutable data shared between owners
};

enum class VectorAppendMode { ALLOW_RESIZE, ERROR_ON_NO_SPACE };

struct AuxiliaryDataHolder {
	virtual ~AuxiliaryDataHolder() = default;

	virtual idx_t GetAllocationSize() const {
		return 0;
	}
};

struct AuxiliaryDataSet {
	vector<unique_ptr<AuxiliaryDataHolder>> data;
};

class PinnedBufferHolder : public AuxiliaryDataHolder {
public:
	explicit PinnedBufferHolder(BufferHandle handle);
	~PinnedBufferHolder() override;

private:
	BufferHandle handle;
};

class AuxiliaryDataSetHolder : public AuxiliaryDataHolder {
public:
	explicit AuxiliaryDataSetHolder(buffer_ptr<AuxiliaryDataSet> buffer) : auxiliary_data(std::move(buffer)) {
	}

private:
	buffer_ptr<AuxiliaryDataSet> auxiliary_data;
};

class VectorBufferHolder : public AuxiliaryDataHolder {
public:
	explicit VectorBufferHolder(buffer_ptr<VectorBuffer> buffer_p) : buffer(std::move(buffer_p)) {
	}

private:
	buffer_ptr<VectorBuffer> buffer;
};

//! The VectorBuffer is a class used by the vector to hold its data
class VectorBuffer : public enable_shared_from_this<VectorBuffer> {
public:
	explicit VectorBuffer(VectorType vector_type, VectorBufferType type, count_t count_p)
	    : vector_type(vector_type), buffer_type(type), v_size(count_p) {
	}
	virtual ~VectorBuffer() {
	}

public:
	virtual data_ptr_t GetData() {
		return nullptr;
	}
	virtual data_ptr_t GetDataMutable() {
		return GetData();
	}
	virtual idx_t Capacity() const {
		throw InternalException("VectorBuffer does not have a capacity");
	}
	virtual void ResetCapacity(idx_t capacity) {
		throw InternalException("VectorBuffer does not have a capacity");
	}
	virtual ValidityMask &GetValidityMask() {
		throw InternalException("VectorBuffer does not have a ValidityMask");
	}
	virtual const ValidityMask &GetValidityMask() const {
		throw InternalException("VectorBuffer does not have a ValidityMask");
	}
	idx_t Size() const {
		return v_size;
	}
	virtual void SetVectorSize(idx_t new_size);
	//! Set only this buffer's vector size without propagating to children (for struct/array buffers)
	void SetVectorSizeOnly(idx_t new_size) {
		v_size = new_size;
	}

	void AddAuxiliaryData(unique_ptr<AuxiliaryDataHolder> aux_data_p) {
		if (!auxiliary_data) {
			auxiliary_data = make_buffer<AuxiliaryDataSet>();
		}
		auxiliary_data->data.push_back(std::move(aux_data_p));
	}
	const buffer_ptr<AuxiliaryDataSet> &GetAuxiliaryData() const {
		return auxiliary_data;
	}
	virtual void ClearAuxiliaryData() {
		auxiliary_data.reset();
	}

	virtual optional_ptr<Allocator> GetAllocator() const {
		return nullptr;
	}

	static buffer_ptr<VectorBuffer> CreateStandardVector(PhysicalType type,
	                                                     capacity_t capacity = capacity_t(STANDARD_VECTOR_SIZE));
	static buffer_ptr<VectorBuffer> CreateStandardVector(const LogicalType &logical_type,
	                                                     capacity_t capacity = capacity_t(STANDARD_VECTOR_SIZE));

	inline VectorType GetVectorType() const {
		return vector_type;
	}
	virtual void SetVectorType(VectorType vector_type);
	//! Set only this buffer's vector type without propagating to children (for struct/array buffers)
	void SetVectorTypeOnly(VectorType new_vector_type) {
		vector_type = new_vector_type;
	}

	inline VectorBufferType GetBufferType() const {
		return buffer_type;
	}

public:
	//! Returns the actual size to reserve (a power-of-two)
	static idx_t GetReserveSize(idx_t required_capacity);

	//! Flatten the vector buffer, converting it to a FLAT_VECTOR
	//! Returns a new buffer, or nullptr if already flat
	virtual buffer_ptr<VectorBuffer> Flatten(const LogicalType &type) const;
	//! Flatten the vector buffer, converting it to a FLAT_VECTOR
	//! The selection vector maps output indices to source indices in this buffer
	buffer_ptr<VectorBuffer> FlattenSlice(const LogicalType &type, const SelectionVector &sel, idx_t count) const;
	//! Returns the total (uncompressed) data size
	virtual idx_t GetDataSize(const LogicalType &type, idx_t count) const;
	//! Returns the total amount of bytes allocated by the vector buffer
	virtual idx_t GetAllocationSize() const;
	void Verify(const LogicalType &type) const;
	void Verify(const LogicalType &type, const SelectionVector &sel, idx_t count) const;
	//! Get the value at the given index directly from the buffer's data
	virtual Value GetValue(const LogicalType &type, idx_t index) const;
	//! Set the value at the given index (flat/constant vectors only)
	virtual void SetValue(const LogicalType &type, idx_t index, const Value &val);
	//! Resize if the vector does not have enough capacity, or throw an error, depending on VectorAppendMode
	void Reserve(idx_t required_capacity, VectorAppendMode append_mode);
	//! Append a value to the vector (flat / constant vectors only)
	void AppendValue(const LogicalType &type, const Value &val, VectorAppendMode append_mode);
	//! Append a vector to this buffer, sliced by the source_sel
	void Append(const Vector &source, const SelectionVector &sel, idx_t append_size, VectorAppendMode append_mode);
	//! Copy data from another vector into this vectors' buffer
	void Copy(const Vector &source, const SelectionVector &source_sel, idx_t source_count, idx_t source_offset,
	          idx_t target_offset, idx_t copy_count);
	//! Produce a string representation of buffer contents (debug only)
	virtual string ToString(const LogicalType &type, idx_t count) const;
	virtual string ToString(const LogicalType &type) const;
	//! Slice the buffer with a selection vector, returning a new buffer
	buffer_ptr<VectorBuffer> Slice(const LogicalType &type, const SelectionVector &sel, idx_t count);
	//! Slice the buffer with an offset range, returning a new buffer
	buffer_ptr<VectorBuffer> Slice(const LogicalType &type, idx_t offset, idx_t end);
	//! Slice the buffer with a selection vector, returning a new buffer
	virtual buffer_ptr<VectorBuffer> SliceWithCache(SelCache &cache, const LogicalType &type,
	                                                const SelectionVector &sel, idx_t count);
	//! Create a UnifiedVectorFormat from the buffer's data
	virtual void ToUnifiedFormat(UnifiedVectorFormat &format) const;
	//! Serialize the buffer as-is (including the vector_type), returns false if the vector must be serialized flat
	virtual bool TrySerialize(Serializer &serializer, const LogicalType &type, bool compressed_serialization) const;
	//! Deserialize a buffer written by TrySerialize
	static buffer_ptr<VectorBuffer> Deserialize(Deserializer &deserializer, VectorType vector_type,
	                                            const LogicalType &type, idx_t count);

protected:
	//! Slice a constant vector with a specific count
	buffer_ptr<VectorBuffer> ConstantSlice(const LogicalType &type, count_t count);
	//! Slice a constant vector with a specific count
	virtual buffer_ptr<VectorBuffer> ConstantSliceInternal(const LogicalType &type, count_t count);
	//! Slice the buffer with a selection vector, returning a new buffer
	virtual buffer_ptr<VectorBuffer> SliceInternal(const LogicalType &type, const SelectionVector &sel, idx_t count);
	//! Slice the buffer with an offset range, returning a new buffer
	virtual buffer_ptr<VectorBuffer> SliceInternal(const LogicalType &type, idx_t offset, idx_t end);
	//! Copy data from another vector into this vectors' buffer
	virtual void CopyInternal(const Vector &source, const SelectionVector &source_sel, idx_t source_count,
	                          idx_t source_offset, idx_t target_offset, idx_t copy_count);
	virtual buffer_ptr<VectorBuffer> FlattenSliceInternal(const LogicalType &type, const SelectionVector &sel,
	                                                      idx_t count) const;

	virtual void VerifyInternal(const LogicalType &type, const SelectionVector &sel, idx_t count) const;

	//! Resize the buffer's data allocation
	virtual void ReserveInternal(idx_t new_size);

protected:
	VectorType vector_type;
	VectorBufferType buffer_type;
	buffer_ptr<AuxiliaryDataSet> auxiliary_data;
	idx_t v_size;

public:
	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}
};

} // namespace duckdb
