//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/set/physical_trampoline.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/deque.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/execution/operator/scan/physical_column_data_scan.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/parser/trampoline_branch_options.hpp"

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
};

//! Trampoline-style iteration: rows carry a selector column; selector 0 emits a
//! row, selector t in 1..n routes it to branch plan t for the next round.
//! children[0] is the seed, children[1..n] are the branch plans.
//!
//! Design notes:
//!
//! Waves. One wave of branch k is the bag of rows a single producing execution
//! (the seed or one branch invocation) routes to k. A branch is invoked once per
//! nonempty wave. Linear, repeatable branches whose rows only reach equally
//! unobservable targets cannot observe wave boundaries, so their pending waves
//! may merge (branch_wave_merge, computed by a backward fixed point over the
//! selector routing graph at physical planning). Nonlinear or volatile branches
//! observe exact boundaries and are never merged automatically. Explicit MERGE
//! and DISTINCT still coalesce their round at the boundary.
//!
//! Rounds. frontier reads, MERGE, and DISTINCT make generation boundaries
//! observable, so execution switches to lockstep rounds: a round seals every
//! pending wave at once, and the frontier is the bag union of the round's input
//! waves across all branches, excluding emitted rows. frontier_rounds carries
//! the binding-time contract; the snapshot is materialized only while a frontier
//! scan survived planning (frontier_scans_present).
//!
//! Worker sizing. A wave parallelizes on its own row count, and independent sources
//! (parallel base-table scans, source-task operators) on their estimated row count,
//! both against a shared minimum-work threshold. The estimate is the planner's output
//! cardinality, not scan work, so a measured arbitration backs it up per branch: a wave
//! whose wall time contradicts its route triggers one probe wave on the other route,
//! and the faster of two comparable waves pins the branch's source routing for the
//! invocation. The trigger bounds (a parallel wave at or below 2 ms, an inline wave at
//! 8 ms or above) and the 2x row-comparability window are heuristics sized around the
//! measured event-machinery cost, not guarantees. One probe per branch per invocation,
//! no arbitration under lockstep rounds, with a single configured thread, or until the
//! branch's invariant builds are retained (a branch whose builds never materialize keeps
//! the estimate route), and contaminated or incomparable probes fall back to the
//! estimate. Large waves always parallelize through the frontier-sized workers. The
//! recursive CTE sizes on frontier work alone and reads no independent-source estimate.
//!
//! Admission (BRANCH DISTINCT). Round-wide full-row admission against all rows
//! ever routed to the branch, yielding one admitted wave per round. Admission
//! hashes physical values, so collated payloads are rejected at bind time, and
//! a correlation suffix joins the admission identity.
//!
//! TEE ordering. BRANCH k TEE emits the arriving row (selector rewritten to 0)
//! before the branch transforms it and before DISTINCT admission arbitrates the
//! round, so a row rejected by admission is still emitted.
//!
//! Correlation. Correlated trampolines append the outer domain suffix through
//! the seed. Branch domain feeds are built as the distinct domains of the
//! branch's current wave (FlattenDependentJoins::CreateDomainSource) and
//! surviving frontier reads are wrapped in a null-safe semi join against the
//! wave's domains. Every branch's output must be anchored in its wave
//! (TrampolineWaveAnchoring), so nothing is produced or evaluated for outer
//! rows whose wave is empty.
//!
//! SQL export. When the trampoline requires rounds, the exported text must
//! retain a round witness (a frontier reference or a surviving MERGE/DISTINCT
//! branch); an export that lost the last witness is rejected, because
//! re-binding it would silently drop the round contract. Trampolines without a
//! round contract export without a witness. EXPLAIN (SQL) requires CTE
//! inlining.
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
	//! Per branch, whether its pending waves may merge: the branch is linear and
	//! repeatable, and so is every branch its rows can transitively reach along the
	//! selector routing graph (coarsened partitioning must stay unobservable)
	vector<bool> branch_wave_merge;
	//! Options of branch i+1: input mode (DEFAULT, DISTINCT or MERGE) and the TEE flag
	vector<TrampolineBranchOptions> branch_options;
	//! Whether any branch read frontier.<ctename> at binding: the round contract
	//! holds even when optimization pruned every surviving frontier scan
	bool frontier_rounds = false;
	//! Whether any branch destination coalesces its round (DISTINCT or MERGE)
	bool has_coalescing_branch = false;
	//! Whether emitted rows are deduplicated online against all prior emissions
	bool emit_distinct = false;

	//! Lockstep rounds are required whenever their boundary is observable
	bool RequiresRounds() const {
		return frontier_rounds || has_coalescing_branch;
	}
	//! Placeholder collection the frontier scans are planned against
	shared_ptr<ColumnDataCollection> frontier_table;
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
	ProgressData GetProgress(ClientContext &context, GlobalSourceState &gstate) const override;

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
	//! Frontier scans per branch, rebound to the frozen snapshot each round
	vector<vector<reference<PhysicalColumnDataScan>>> branch_frontier_scans;
	//! Whether any frontier scan survived into the physical plan; only then is
	//! the round's frontier snapshot materialized
	bool frontier_scans_present = false;
	//! Precomputed pipelines, schedules and sink-reset lists per branch
	vector<TrampolineBranchPipelines> branch_pipelines;
	mutable shared_ptr<RecursiveExecutorPool> shared_executor_pool;
	mutable atomic<idx_t> metrics_invocations {0};

	friend class TrampolineState;
};

} // namespace duckdb
