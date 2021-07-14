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
	using FrameBounds = std::pair<idx_t, idx_t>;

	WindowSegmentTree(AggregateFunction &aggregate, FunctionData *bind_info, const LogicalType &result_type,
	                  ChunkCollection *input);
	~WindowSegmentTree();

	//! First row contains the result.
	const Vector &Compute(idx_t start, idx_t end);

private:
	void ConstructTree();
	void ExtractFrame(idx_t begin, idx_t end);
	void WindowSegmentValue(idx_t l_idx, idx_t begin, idx_t end);
	void AggregateInit();
	const Vector &AggegateFinal();

	//! The aggregate that the window function is computed over
	AggregateFunction aggregate;
	//! The bind info of the aggregate
	FunctionData *bind_info;
	//! The result type of the window function
	LogicalType result_type;

	//! Data pointer that contains a single state, used for intermediate window segment aggregation
	vector<data_t> state;
	//! Input data chunk, used for intermediate window segment aggregation
	DataChunk inputs;
	//! A vector of pointers to "state", used for intermediate window segment aggregation
	Vector statep;
	//! The frame boundaries, used for the window functions
	FrameBounds frame;
	//! Reused result value container for the window functions (20% of runtime)
	Vector result;
	//! Reused result state container for the window functions
	Vector statev;

	//! The actual window segment tree: an array of aggregate states that represent all the intermediate nodes
	unique_ptr<data_t[]> levels_flat_native;
	//! For each level, the starting location in the levels_flat_native array
	vector<idx_t> levels_flat_start;

	//! The total number of internal nodes of the tree, stored in levels_flat_native
	idx_t internal_nodes;

	//! The (sorted) input chunk collection on which the tree is built
	ChunkCollection *input_ref;

	// TREE_FANOUT needs to cleanly divide STANDARD_VECTOR_SIZE
	static constexpr idx_t TREE_FANOUT = 64;
};

} // namespace duckdb
