//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/vector/fsst_vector.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/vector/string_vector.hpp"

namespace duckdb {

class VectorFSSTStringBuffer : public VectorStringBuffer {
public:
	explicit VectorFSSTStringBuffer(capacity_t capacity);

public:
	void AddDecoder(buffer_ptr<void> &duckdb_fsst_decoder_p, const idx_t string_block_limit) {
		duckdb_fsst_decoder = duckdb_fsst_decoder_p;
		decompress_buffer.resize(string_block_limit + 1);
	}
	void *GetDecoder() const {
		return duckdb_fsst_decoder.get();
	}
	vector<unsigned char> &GetDecompressBuffer() const {
		return decompress_buffer;
	}
	void SetCount(idx_t count) {
		v_size = count;
	}
	void SetVectorType(VectorType vector_type) override;

public:
	Value GetValue(const LogicalType &type, idx_t index) const override;

protected:
	buffer_ptr<VectorBuffer> ConstantReferenceInternal(const LogicalType &type, idx_t index,
	                                                   count_t count) const override;
	buffer_ptr<VectorBuffer> FlattenSliceInternal(const LogicalType &type, const SelectionVector &sel,
	                                              idx_t count) const override;
	void VerifyInternal(const LogicalType &type, const SelectionVector &sel, idx_t count) const override;

private:
	buffer_ptr<void> duckdb_fsst_decoder;
	mutable vector<unsigned char> decompress_buffer;
};

struct FSSTVector {
	static inline const ValidityMask &Validity(const Vector &vector) {
		D_ASSERT(vector.GetVectorType() == VectorType::FSST_VECTOR);
		return vector.Buffer().GetValidityMask();
	}
	static inline ValidityMask &Validity(Vector &vector) {
		D_ASSERT(vector.GetVectorType() == VectorType::FSST_VECTOR);
		return vector.BufferMutable().GetValidityMask();
	}
	static inline void SetValidity(Vector &vector, ValidityMask &new_validity) {
		D_ASSERT(vector.GetVectorType() == VectorType::FSST_VECTOR);
		auto &validity = vector.BufferMutable().GetValidityMask();
		validity.Initialize(new_validity);
	}
	static inline const string_t *GetCompressedData(const Vector &vector) {
		D_ASSERT(vector.GetVectorType() == VectorType::FSST_VECTOR);
		return reinterpret_cast<const string_t *>(vector.GetBufferRef()->GetData());
	}
	static inline string_t *GetCompressedData(Vector &vector) {
		D_ASSERT(vector.GetVectorType() == VectorType::FSST_VECTOR);
		return reinterpret_cast<string_t *>(vector.BufferMutable().GetData());
	}

	DUCKDB_API static string_t AddCompressedString(Vector &vector, string_t data);
	DUCKDB_API static string_t AddCompressedString(Vector &vector, const char *data, idx_t len);
	DUCKDB_API static void Create(Vector &vector, buffer_ptr<void> &duckdb_fsst_decoder, const idx_t string_block_limit,
	                              idx_t capacity);
	DUCKDB_API static void *GetDecoder(const Vector &vector);
	DUCKDB_API static vector<unsigned char> &GetDecompressBuffer(const Vector &vector);
	//! Setting the string count is required to be able to correctly flatten the vector
	DUCKDB_API static void SetCount(Vector &vector, idx_t count);

private:
	static VectorFSSTStringBuffer &GetFSSTBuffer(const Vector &vector);
	static StringHeap &GetStringHeap(const Vector &vector);
};

} // namespace duckdb
