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
#include "duckdb/execution/physical_operator.hpp"

namespace duckdb {

class Pipeline;
class RecursiveCTEPipelineSchedulePlan;
struct RecursiveExecutorPool;

//! Per-branch pipeline bookkeeping precomputed at pipeline-construction time
struct TrampolineBranchPipelines {
	//! Every pipeline of the branch's meta pipelines, deduplicated
	vector<shared_ptr<Pipeline>> pipelines;
	//! One owning pipeline per distinct non-trampoline sink, with its single-producer flag
	vector<pair<shared_ptr<Pipeline>, bool>> sink_resets;
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
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;

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
	//! Cached schedule plan per branch
	vector<unique_ptr<RecursiveCTEPipelineSchedulePlan>> branch_schedule_plans;
	//! Precomputed pipeline and sink-reset lists per branch
	vector<TrampolineBranchPipelines> branch_pipelines;
	mutable shared_ptr<RecursiveExecutorPool> shared_executor_pool;

	friend class TrampolineState;
};

} // namespace duckdb
