//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/set/physical_trampoline.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/deque.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/execution/operator/scan/physical_column_data_scan.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/execution/physical_operator.hpp"

namespace duckdb {

class Pipeline;
class RecursiveCTEPipelineSchedulePlan;
struct RecursiveExecutorPool;

class MetaPipeline;

//! One pipeline owning a distinct sink that must be reset before a wave executes
struct TrampolineSinkReset {
	shared_ptr<Pipeline> pipeline;
	//! The sink has exactly one producing pipeline
	bool single_producer;
	//! The sink belongs to an invariant build, which parallelizes independently of the wave
	bool invariant_build;
};

//! Per-branch pipeline bookkeeping precomputed at pipeline-construction time
struct TrampolineBranchPipelines {
	//! Every pipeline of the branch's meta pipelines, deduplicated
	vector<shared_ptr<Pipeline>> pipelines;
	//! One owning pipeline per distinct non-trampoline sink, with its single-producer flag
	vector<TrampolineSinkReset> sink_resets;
	//! Sink resets of the active subset, excluding retained invariant builds
	vector<TrampolineSinkReset> active_sink_resets;
	//! Meta pipelines whose materialized state can survive later waves
	reference_set_t<const MetaPipeline> invariant_meta_pipelines;
	//! Schedule over every pipeline of the branch
	unique_ptr<RecursiveCTEPipelineSchedulePlan> plan;
	//! Schedule without the invariant builds, used once they are materialized
	unique_ptr<RecursiveCTEPipelineSchedulePlan> active_plan;
	//! Per-stage flag of `plan`: a FINISH of the trampoline sink with no operator finalization
	vector<bool> trivial_finish;
	//! The same flag for `active_plan`
	vector<bool> active_trivial_finish;
};

//! Trampoline-style iteration: rows carry a selector column; selector 0 emits a
//! row, selector t in 1..n routes it to branch plan t for the next round.
//! children[0] is the seed, children[1..n] are the branch plans.
class PhysicalTrampoline : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::TRAMPOLINE;

public:
	PhysicalTrampoline(PhysicalPlan &physical_plan, Identifier ctename, TableIndex table_index,
	                   vector<LogicalType> types, idx_t selector_idx, PhysicalOperator &seed,
	                   const vector<reference<PhysicalOperator>> &branches, idx_t estimated_cardinality);
	~PhysicalTrampoline() override;

	Identifier ctename;
	TableIndex table_index;
	//! Index of the selector column in the schema
	idx_t selector_idx;
	//! Placeholder collection the branch scans are planned against; rebound per wave
	shared_ptr<ColumnDataCollection> working_table;
	//! One detached meta pipeline per branch, re-executed by the operator
	vector<shared_ptr<MetaPipeline>> branch_meta_pipelines;
	//! All branches are linear in the trampoline input, so waves may be merged per branch
	bool merge_waves = false;
	//! Per branch, whether its pending waves may merge: true for every branch when all
	//! are linear and repeatable, and for an individual linear repeatable branch whose
	//! selector is provably the constant 0 (its output is only ever emitted, so no
	//! consumer can observe its wave boundaries)
	vector<bool> branch_wave_merge;
	//! Operators whose results cannot be retained across waves
	reference_set_t<const PhysicalOperator> non_repeatable_operators;

	idx_t BranchCount() const {
		return children.size() - 1;
	}

public:
	// Source interface
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override;
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;

	bool IsSource() const override {
		return true;
	}

public:
	// Sink interface
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;
	SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const override;
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;
	unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const override;

	bool IsSink() const override {
		return true;
	}

	bool ParallelSink() const override {
		return true;
	}

	InsertionOrderPreservingMap<string> ParamsToString() const override;

public:
	void BuildPipelines(Pipeline &current, MetaPipeline &meta_pipeline) override;

	vector<const_reference<PhysicalOperator>> GetSources() const override;

private:
	void ExecuteBranchPipelines(ExecutionContext &context, idx_t branch_idx) const;

private:
	//! Recursive scans of each branch plan, rebound to the current wave before execution
	vector<vector<reference<PhysicalColumnDataScan>>> branch_recursive_scans;
	//! Precomputed pipelines, schedules and sink-reset lists per branch
	vector<TrampolineBranchPipelines> branch_pipelines;
	mutable shared_ptr<RecursiveExecutorPool> shared_executor_pool;

	friend class TrampolineState;
};

} // namespace duckdb
