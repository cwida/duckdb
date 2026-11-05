//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/vector/sequence_vector.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/vector.hpp"

namespace duckdb {

class SequenceBuffer : public VectorBuffer {
public:
	explicit SequenceBuffer(int64_t start, int64_t increment, count_t seq_count);

	int64_t start;
	int64_t increment;

public:
	idx_t Capacity() const override {
		return Size();
	}
	idx_t GetDataSize(const LogicalType &type, idx_t count) const override;
	idx_t GetAllocationSize() const override;
	string ToString(const LogicalType &type, idx_t count) const override;
	Value GetValue(const LogicalType &type, idx_t index) const override;
	bool TrySerialize(Serializer &serializer, const LogicalType &type, bool compressed_serialization) const override;
	static buffer_ptr<VectorBuffer> Deserialize(Deserializer &deserializer, const LogicalType &type, idx_t count);

protected:
	buffer_ptr<VectorBuffer> FlattenSliceInternal(const LogicalType &type, const SelectionVector &sel,
	                                              idx_t count) const override;
};

struct SequenceVector {
	static void GetSequence(const Vector &vector, int64_t &start, int64_t &increment);
};

} // namespace duckdb
