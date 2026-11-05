#pragma once

#include "duckdb/common/array.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/types/hyperloglog.hpp"
#include "duckdb/execution/aggregate_hashtable.hpp"
#include "duckdb/execution/executor.hpp"
#include "duckdb/execution/operator/set/physical_recursive_cte.hpp"
#include "duckdb/execution/operator/set/recursive_cte_scheduling.hpp"

namespace duckdb {

class InterruptState;
class Logger;

enum class RecursiveCTESourcePhase : uint8_t {
	INITIAL,
	SCANNING_UNION,
	RECURSING_KEY,
	//! A recursive epoch is in progress; its per-iteration setup already ran and must not rerun
	EXECUTING_RECURSION,
	DRAINING_FINAL_KEY_STATE,
	FINISHED
};

//! Epoch-stable secondary index over a proper subset of USING KEY columns.
class RecursiveCTEPartialKeyIndex {
public:
	struct Entry {
		hash_t hash;
		data_ptr_t address;
		idx_t next;
	};

	RecursiveCTEPartialKeyIndex(Allocator &allocator, const vector<LogicalType> &full_key_types,
	                            vector<idx_t> key_indices);

	//! Key and address selections can differ when addresses have already been compacted.
	void AddGroups(DataChunk &full_keys, const SelectionVector &key_selection, Vector &group_addresses,
	               const SelectionVector &address_selection, idx_t group_count);
	idx_t GetHead(hash_t hash) const;
	const Entry &GetEntry(idx_t entry_idx) const;
	idx_t Count() const;
	idx_t SizeInBytes() const;

	vector<idx_t> key_indices;

private:
	void Resize(idx_t capacity);

	DataChunk partial_keys;
	DataChunk selected_keys;
	Vector hashes;
	vector<idx_t> heads;
	vector<Entry> entries;
};

//! The source state of a recursive CTE - tracks the rows emitted to estimate the progress, as the number of recursive
//! iterations is not known up front
class RecursiveCTEGlobalSourceState : public GlobalSourceState {
public:
	void StartIteration(idx_t row_count);
	void StartFinalDrain(idx_t row_count);
	void AddEmittedRows(idx_t row_count) {
		emitted_rows.fetch_add(row_count, std::memory_order_relaxed);
	}
	void Finish() {
		finished.store(true, std::memory_order_relaxed);
	}
	ProgressData GetProgress();

private:
	atomic<idx_t> emitted_rows {0};
	atomic<bool> finished {false};
	//! Protects the iteration statistics, which are updated once per iteration
	mutex iteration_lock;
	//! Whether the final state of a USING KEY recursion is being emitted (its size is then known)
	bool draining = false;
	idx_t iterations = 0;
	//! Rows of the first, the current and the previous iteration
	idx_t anchor_rows = 0;
	idx_t iteration_rows = 0;
	idx_t previous_iteration_rows = 0;
	//! Rows emitted before the current iteration started
	idx_t iteration_start_rows = 0;
	MonotonicProgress progress;
};

//! The part of the recursion every mode shares: the operator, the scheduler and the metrics of one invocation.
//! RecursiveCTEUnionState and RecursiveCTEKeyedState own the state of their mode and implement the sink and source
//! sides of the operator on top of it.
class RecursiveCTEState : public GlobalSinkState {
public:
	RecursiveCTEState(ClientContext &context, const PhysicalRecursiveCTE &op);
	~RecursiveCTEState() override;

	//! Sink side of the operator in this mode; the local state is private to the mode's source file
	virtual unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const = 0;
	virtual SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) = 0;
	virtual SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) = 0;
	//! Source side: hands out result rows and drives the recursive iterations. May return BLOCKED
	//! while a recursive epoch waits for blocked inner work; the interrupt state re-enters it.
	virtual SourceResultType GetData(ExecutionContext &context, DataChunk &chunk, RecursiveCTEGlobalSourceState &source,
	                                 const InterruptState &interrupt_state) = 0;
	//! Frontier the recursive member scans in the current iteration
	virtual const ColumnDataCollection &CurrentInputTable() const = 0;
	//! Rows and chunks of the recurring state the member can scan in the current iteration
	virtual idx_t RecurringStateRows() const = 0;
	virtual idx_t RecurringStateChunks() const = 0;
	idx_t CurrentInputCount() const {
		return CurrentInputTable().Count();
	}

	void RecordSinkMetrics(idx_t wait_ns, idx_t work_ns, idx_t rows);

	const PhysicalRecursiveCTE &GetOperator() const {
		return op;
	}
	RecursiveCTEMetrics &GetMetrics() {
		return metrics;
	}
	const RecursiveCTEMetrics &GetMetrics() const {
		return metrics;
	}
	RecursiveCTEEpochMetrics &GetEpochMetrics() {
		D_ASSERT(epoch_metrics);
		return *epoch_metrics;
	}
	RecursiveCTESchedulerState &GetScheduler() {
		return scheduler;
	}
	const RecursiveCTESchedulerState &GetScheduler() const {
		return scheduler;
	}
	RecursiveCTEEpochProgress &GetEpochProgress() {
		return epoch_progress;
	}
	bool AllowsExecutorReuse() const {
		return allow_executor_reuse;
	}
	bool HasMaterializedInvariantPipelines() const {
		return invariant_meta_pipelines_materialized;
	}
	void MarkInvariantPipelinesMaterialized() {
		invariant_meta_pipelines_materialized = true;
	}

protected:
	//! Logs the invocation; the mode destructors call it while their state is still alive
	void LogMetrics(const vector<unique_ptr<RecursiveCTEPartialKeyIndex>> &partial_key_indexes);

	const PhysicalRecursiveCTE &op;
	ClientContext &context;
	const bool allow_executor_reuse;
	RecursiveCTEMetrics metrics;
	RecursiveCTESchedulerState scheduler;
	RecursiveCTEEpochProgress epoch_progress;
	RecursiveCTESourcePhase source_phase = RecursiveCTESourcePhase::INITIAL;
	//! Whether invariant recursive meta-pipelines have already been materialized for this state
	bool invariant_meta_pipelines_materialized = false;
	//! Optional epoch distributions and capacity metrics, allocated only when structured logging is active
	unique_ptr<RecursiveCTEEpochMetrics> epoch_metrics;
};

} // namespace duckdb
