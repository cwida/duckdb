//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/window/window_merge_sort_tree.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/merge_sort_tree.hpp"
#include "duckdb/planner/bound_result_modifier.hpp"

#include "duckdb/common/sorting/sort.hpp"

namespace duckdb {

enum class WindowMergeSortStage : uint8_t { INIT, COMBINE, FINALIZE, SORTED, FINISH, FINISHED };

class WindowMergeSortTree;

class WindowMergeSortTreeLocalState : public LocalSinkState {
public:
	WindowMergeSortTreeLocalState(ExecutionContext &context, WindowMergeSortTree &index_tree);

	//! Add a chunk to the local sort
	void Sink(ExecutionContext &context, DataChunk &chunk, const idx_t row_idx,
	          optional_ptr<SelectionVector> filter_sel, idx_t filtered, InterruptState &interrupt);
	//! Sort the data
	void Finalize(ExecutionContext &context, InterruptState &interrupt);
	//! Process sorted leaf data
	virtual void BuildLeaves() = 0;

	//! The index tree we are building
	WindowMergeSortTree &window_tree;
	//! Thread-local sorting data
	optional_ptr<LocalSinkState> local_sink;
	//! Buffer for the sort data
	DataChunk sort_chunk;
	//! Build stage
	WindowMergeSortStage build_stage = WindowMergeSortStage::INIT;
	//! Build task number
	idx_t build_task;

private:
	void ExecuteSortTask(ExecutionContext &context, InterruptState &interrupt);
};

class WindowMergeSortTree {
public:
	using GlobalSortStatePtr = unique_ptr<GlobalSinkState>;
	using LocalSortStatePtr = unique_ptr<LocalSinkState>;

	WindowMergeSortTree(ClientContext &context, const vector<BoundOrderByNode> &orders,
	                    const vector<column_t> &order_idx, const idx_t count, bool unique = false);
	virtual ~WindowMergeSortTree() = default;

	virtual unique_ptr<LocalSinkState> GetLocalState(ExecutionContext &context) = 0;

	//! Make a local sort for a thread
	optional_ptr<LocalSinkState> InitializeLocalSort(ExecutionContext &context) const;

	//! Find the starts of all the blocks (run as part of the single FINALIZE task)
	//! Returns the total number of rows
	virtual idx_t MeasurePayloadBlocks();
	//! Post-sort cleanup (run as the single FINISH task)
	virtual void Finished();

	//! Sort state machine
	bool TryPrepareSortStage(WindowMergeSortTreeLocalState &lstate);
	//! Build the MST in parallel from the sorted data
	void Build(ClientContext &context);

	//! The column indices for sorting
	const vector<column_t> order_idx;
	//! The sorted data schema
	vector<LogicalType> scan_types;
	vector<idx_t> scan_cols;
	//! The sort key columns
	vector<idx_t> key_cols;
	//! The sort specification
	unique_ptr<Sort> sort;
	//! The sorted data
	GlobalSortStatePtr global_sink;
	//! The resulting sorted data
	unique_ptr<ColumnDataCollection> sorted;
	//! Finalize guard
	mutable mutex lock;
	//! Local sort set
	mutable vector<LocalSortStatePtr> local_sinks;
	//! Finalize stage
	atomic<WindowMergeSortStage> build_stage;
	//! Tasks launched
	idx_t total_tasks = 0;
	//! Tasks launched
	idx_t tasks_assigned = 0;
	//! Tasks completed successfully
	atomic<idx_t> tasks_completed;

	// Merge sort trees for various sizes
	// Smaller is probably not worth the effort.
	using MergeSortTree32 = MergeSortTree<uint32_t, uint32_t>;
	using MergeSortTree64 = MergeSortTree<uint64_t, uint64_t>;
	unique_ptr<MergeSortTree32> mst32;
	unique_ptr<MergeSortTree64> mst64;
};

} // namespace duckdb
