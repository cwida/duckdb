//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/row/integer_partitioned_tuple_data.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/row/partitioned_tuple_data.hpp"

namespace duckdb {

class ClientContext;

//! IntegerPartitionedTupleData partitions on the value of an integer column with known bounds: one partition per
//! value in [min, min + value_count), one for NULL keys and one for keys outside the bounds. Every partition except
//! the overflow partition holds a single key.
class IntegerPartitionedTupleData : public PartitionedTupleData {
public:
	IntegerPartitionedTupleData(BufferManager &buffer_manager, shared_ptr<TupleDataLayout> layout_ptr, MemoryTag tag,
	                            idx_t key_col_idx, const Value &min_value, idx_t value_count,
	                            QueryContext context = QueryContext());
	IntegerPartitionedTupleData(IntegerPartitionedTupleData &other);
	~IntegerPartitionedTupleData() override;

	//! Whether keys of this type can be partitioned on
	static bool SupportsType(const LogicalType &type);
	//! Whether a key with these statistics can be partitioned on directly within the memory budget: sets the smallest
	//! key and the number of keys within the bounds
	static bool TryBoundedKey(ClientContext &context, const LogicalType &key_type,
	                          optional_ptr<const BaseStatistics> stats, Value &min_value, idx_t &value_count);
	//! The number of partitions needed for the given number of key values
	static idx_t PartitionCountFor(idx_t value_count) {
		return value_count + 2;
	}

	//! The partition holding the NULL keys
	idx_t NullPartition() const {
		return value_count;
	}
	//! The partition holding keys outside the bounds - the only one that may hold more than one key
	idx_t OverflowPartition() const {
		return value_count + 1;
	}
	bool IsSingleKeyPartition(idx_t partition_idx) const {
		return partition_idx != OverflowPartition();
	}

private:
	void Initialize();
	void ComputePartitionIndices(const Vector &keys, const SelectionVector &append_sel, idx_t append_count,
	                             Vector &partition_indices) const;

protected:
	//===--------------------------------------------------------------------===//
	// Integer Partitioning interface implementation
	//===--------------------------------------------------------------------===//
	void ComputePartitionIndices(PartitionedTupleDataAppendState &state, DataChunk &input,
	                             const SelectionVector &append_sel, const idx_t append_count) override;
	void ComputePartitionIndices(Vector &row_locations, idx_t count, Vector &partition_indices,
	                             unique_ptr<Vector> &utility_vector, const SelectionVector &append_sel) const override;
	idx_t MaxPartitionIndex() const override {
		return OverflowPartition();
	}

private:
	//! The index of the key column
	const idx_t key_col_idx;
	//! The smallest key value within the bounds
	const Value min_value;
	//! The number of key values within the bounds
	const idx_t value_count;
};

} // namespace duckdb
