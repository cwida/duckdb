//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/sorting/sorted_run_merger.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional_idx.hpp"
#include "duckdb/execution/physical_operator_states.hpp"

namespace duckdb {

class Sort;
class TupleDataLayout;
struct BoundOrderByNode;
struct ProgressData;
class SortedRun;
enum class SortKeyType : uint8_t;
class TaskScheduler;

class SortedRunMerger {
	friend class SortedRunMergerLocalState;
	friend class SortedRunMergerGlobalState;

public:
	SortedRunMerger(const Sort &sort, vector<unique_ptr<SortedRun>> &&sorted_runs, idx_t partition_size, bool external,
	                bool is_index_sort);
	~SortedRunMerger();

public:
	idx_t PartitionCount() const;
	idx_t PartitionRowCount(idx_t partition_count) const;

	//===--------------------------------------------------------------------===//
	// Source Interface
	//===--------------------------------------------------------------------===//
	unique_ptr<LocalSourceState> GetLocalSourceState(ExecutionContext &context, GlobalSourceState &gstate) const;
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const;
	SourceResultType GetData(ExecutionContext &context, DataChunk &chunk, OperatorSourceInput &input) const;
	OperatorPartitionData GetPartitionData(ExecutionContext &context, DataChunk &chunk, GlobalSourceState &gstate,
	                                       LocalSourceState &lstate, const OperatorPartitionInfo &partition_info) const;
	ProgressData GetProgress(ClientContext &context, GlobalSourceState &gstate) const;
	//! Whether all partitions have been scanned
	bool IsFinished(GlobalSourceState &gstate) const;

public:
	//===--------------------------------------------------------------------===//
	// Non-Standard Interface
	//===--------------------------------------------------------------------===//
	SourceResultType MaterializeSortedRun(ExecutionContext &context, OperatorSourceInput &input) const;
	unique_ptr<SortedRun> GetSortedRun(GlobalSourceState &global_state);

private:
	template <SortKeyType SORT_KEY_TYPE>
	optional_idx TemplatedFindSkewedPartitionSplit() const;
	//! Optional output row rank at which to split a partition with disproportionately large payloads.
	optional_idx FindSkewedPartitionSplit() const;

	TaskScheduler &scheduler;

public:
	const Sort &sort;
	vector<unique_ptr<SortedRun>> sorted_runs;
	const idx_t total_count;

	const idx_t partition_size;
	const bool external;
	const bool is_index_sort;

private:
	const optional_idx extra_partition_end;
};

} // namespace duckdb
