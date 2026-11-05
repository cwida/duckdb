//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/sorting/sort.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/atomic.hpp"
#include "duckdb/execution/physical_operator_states.hpp"
#include "duckdb/execution/progress_data.hpp"
#include "duckdb/common/sorting/sort_projection_column.hpp"
#include "duckdb/planner/bound_result_modifier.hpp"

namespace duckdb {

class SortLocalSinkState;
class SortGlobalSinkState;

class SortLocalSourceState;
class SortGlobalSourceState;

class SortedRun;
class SortedRunScanState;

class SortedRunMerger;
class SortedRunMergerLocalState;
class SortedRunMergerGlobalState;

class TupleDataLayout;
class ColumnDataCollection;

//! Class that sorts the data, follows the PhysicalOperator interface
class Sort {
	friend class SortLocalSinkState;
	friend class SortGlobalSinkState;

	friend class SortLocalSourceState;
	friend class SortGlobalSourceState;

	friend class SortedRun;
	friend class SortedRunScanState;

	friend class SortedRunMerger;
	friend class SortedRunMergerLocalState;
	friend class SortedRunMergerGlobalState;

public:
	Sort(ClientContext &client_context, const vector<BoundOrderByNode> &orders, const vector<LogicalType> &input_types,
	     vector<idx_t> projection_map, bool is_index_sort = false);

public:
	//! The client context
	ClientContext &client_context;

private:
	//! Key orders, expressions, and layout
	unique_ptr<Expression> create_sort_key;
	unique_ptr<Expression> decode_sort_key;
	shared_ptr<TupleDataLayout> key_layout;

	//! Projection map and payload layout (columns that also appear as key eliminated)
	vector<idx_t> payload_projection_map;
	shared_ptr<TupleDataLayout> payload_layout;

	//! Mapping from key/payload layouts to output columns
	vector<idx_t> input_projection_map;
	vector<SortProjectionColumn> output_projection_columns;

	//! Whether to force an approximate sort
	bool is_index_sort;

public:
	//===--------------------------------------------------------------------===//
	// Sink Interface
	//===--------------------------------------------------------------------===//
	unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const;
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const;
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const;
	SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const;
	SinkFinalizeType Finalize(ClientContext &context, OperatorSinkFinalizeInput &input) const;
	ProgressData GetSinkProgress(ClientContext &context, GlobalSinkState &gstate,
	                             const ProgressData source_progress) const;
	//! The number of tuples that have been sorted so far
	idx_t GetSortedCount(GlobalSinkState &gstate) const;
	//! Sink progress of a sort: half of the effort is sinking, half is sorting the sunk tuples
	static ProgressData GetSinkProgress(const ProgressData &source_progress, idx_t sorted_count, idx_t sunk_count,
	                                    MonotonicProgress &monotonic_progress);

public:
	//===--------------------------------------------------------------------===//
	// Source Interface
	//===--------------------------------------------------------------------===//
	unique_ptr<LocalSourceState> GetLocalSourceState(ExecutionContext &context, GlobalSourceState &gstate) const;
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context, GlobalSinkState &sink) const;
	SourceResultType GetData(ExecutionContext &context, DataChunk &chunk, OperatorSourceInput &input) const;
	OperatorPartitionData GetPartitionData(ExecutionContext &context, DataChunk &chunk, GlobalSourceState &gstate,
	                                       LocalSourceState &lstate, const OperatorPartitionInfo &partition_info) const;
	ProgressData GetProgress(ClientContext &context, GlobalSourceState &gstate) const;

public:
	//===--------------------------------------------------------------------===//
	// Non-Standard Interface
	//===--------------------------------------------------------------------===//
	SourceResultType MaterializeColumnData(ExecutionContext &context, OperatorSourceInput &input) const;
	unique_ptr<ColumnDataCollection> GetColumnData(OperatorSourceInput &input) const;

	SourceResultType MaterializeSortedRun(ExecutionContext &context, OperatorSourceInput &input) const;
	unique_ptr<SortedRun> GetSortedRun(GlobalSourceState &global_state);
};

} // namespace duckdb
