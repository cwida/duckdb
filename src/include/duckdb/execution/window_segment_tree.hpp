//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/window_segment_tree.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/chunk_collection.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/function/aggregate_function.hpp"

namespace duckdb {

class WindowSegmentTree {
public:
	WindowSegmentTree(AggregateFunction &aggregate, TypeId result_type, ChunkCollection *input);
	Value Compute(idx_t start, idx_t end);

private:
	void ConstructTree();
	void WindowSegmentValue(idx_t l_idx, idx_t begin, idx_t end);
	void AggregateInit();
	Value AggegateFinal();

	AggregateFunction aggregate;
	vector<data_t> state;
	Vector statep;
	TypeId result_type;
	unique_ptr<data_t[]> levels_flat_native;
	vector<idx_t> levels_flat_start;
	unique_ptr<Vector[]> inputs;

	ChunkCollection *input_ref;

	static constexpr idx_t TREE_FANOUT = 64; // this should cleanly divide STANDARD_VECTOR_SIZE
};

} // namespace duckdb
