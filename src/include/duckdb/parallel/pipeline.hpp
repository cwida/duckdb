//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parallel/pipeline.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/set.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/parallel/executor_task.hpp"

namespace duckdb {

class Executor;
class Event;
class MetaPipeline;
class PipelineExecutor;
class Pipeline;
class PipelineBuildStateData;
class PhysicalCTE;

enum class PipelineInputMode : uint8_t { SCHEDULED_SOURCE, EXTERNAL_INPUT };
enum class PipelineDependencyType : uint8_t { REQUIRED, OPTIONAL_DEPENDENCY };

//! A dependency of a Pipeline on another Pipeline within the same MetaPipeline
struct PipelineDependency {
	PipelineDependency(Pipeline &pipeline_p, PipelineDependencyType type_p);

	weak_ptr<Pipeline> pipeline;
	PipelineDependencyType type;
};

enum class ExternalInputEventState : uint8_t {
	EXTERNAL_INPUT_UNSET,
	EXTERNAL_INPUT_REGISTERED,
	EXTERNAL_INPUT_EVENT_SCHEDULED,
	EXTERNAL_INPUT_COMPLETED_BEFORE_SCHEDULE,
	EXTERNAL_INPUT_COMPLETED
};

class PipelineTask : public ExecutorTask {
	static constexpr const idx_t PARTIAL_CHUNK_COUNT = 50;

public:
	explicit PipelineTask(Pipeline &pipeline_p, shared_ptr<Event> event_p);

	Pipeline &pipeline;
	unique_ptr<PipelineExecutor> pipeline_executor;
	optional_idx reserved_batch_index;

	string TaskType() const override {
		return "PipelineTask";
	}

public:
	TaskExecutionResult ExecuteTask(TaskExecutionMode mode) override;
};

class PipelineBuildState {
public:
	//! How much to increment batch indexes when multiple pipelines share the same source
	constexpr static idx_t BATCH_INCREMENT = 10000000000000;

public:
	PipelineBuildState();
	~PipelineBuildState(); // NOLINT: PipelineBuildStateData is incomplete here

public:
	//! Duplicate eliminated join scan dependencies
	reference_map_t<const PhysicalOperator, reference<Pipeline>> delim_join_dependencies;
	//! Materialized CTE scan dependencies
	reference_map_t<const PhysicalOperator, reference<Pipeline>> cte_dependencies;

public:
	void SetPipelineSource(Pipeline &pipeline, PhysicalOperator &op);
	void SetPipelineSink(Pipeline &pipeline, optional_ptr<PhysicalOperator> op, idx_t sink_pipeline_count);
	void SetPipelineOperators(Pipeline &pipeline, vector<reference<PhysicalOperator>> operators);
	void AddPipelineOperator(Pipeline &pipeline, PhysicalOperator &op);
	shared_ptr<Pipeline> CreateChildPipeline(Executor &executor, Pipeline &pipeline, PhysicalOperator &op);

	optional_ptr<PhysicalOperator> GetPipelineSource(Pipeline &pipeline);
	optional_ptr<PhysicalOperator> GetPipelineSink(Pipeline &pipeline);
	vector<reference<PhysicalOperator>> GetPipelineOperators(Pipeline &pipeline);
	void AddExternalInputCandidate(Pipeline &pipeline, PhysicalOperator &materialized_source,
	                               PhysicalOperator &external_source, shared_ptr<Pipeline> cte_dependency,
	                               PhysicalCTE &cte, idx_t consumer_idx);
	void AddBufferedConsumer(Pipeline &pipeline, PhysicalOperator &materialized_source,
	                         shared_ptr<Pipeline> cte_dependency, PhysicalCTE &cte, idx_t consumer_idx);
	void AddCTEPipelineSelection(PhysicalCTE &cte, Pipeline &pipeline, shared_ptr<Pipeline> cte_dependency,
	                             bool dependency_added);
	//! model_runtime_filter_edges must mirror the executor's runtime-filter scheduling condition (initial
	//! schedule without recursive CTEs), so the analysis sees the edges the executor will add.
	void ResolveExternalInputs(const vector<shared_ptr<MetaPipeline>> &meta_pipelines, bool model_runtime_filter_edges);

private:
	unique_ptr<PipelineBuildStateData> data;
};

//! Progress of a pipeline, split into the source and the (sink-adjusted) pipeline progress
struct PipelineProgress {
	//! Progress as reported by the source, before normalization
	ProgressData source;
	//! Progress of the pipeline: the normalized source progress, adjusted by the sink
	ProgressData pipeline;
};

//! The Pipeline class represents an execution pipeline starting at a
class Pipeline : public enable_shared_from_this<Pipeline> {
	friend class Executor;
	friend class PipelineExecutor;
	friend class PipelineEvent;
	friend class PipelineFinishEvent;
	friend class PipelineBuildState;
	friend class MetaPipeline;

public:
	explicit Pipeline(Executor &execution_context);

	Executor &executor;

public:
	ClientContext &GetClientContext();

	void AddDependency(shared_ptr<Pipeline> &pipeline);
	void AddDataflowDependency(shared_ptr<Pipeline> &pipeline);
	void RemoveDataflowDependency(const shared_ptr<Pipeline> &pipeline);
	//! The dependencies of this pipeline on pipelines of other MetaPipelines
	vector<weak_ptr<Pipeline>> GetDependencies() const;
	//! The dependencies of this pipeline on pipelines of the same MetaPipeline
	const vector<PipelineDependency> &GetIntraDependencies() const {
		return intra_dependencies;
	}
	//! All pipelines this pipeline waits on before it can start: 'intra_dependencies' and 'dependencies'
	vector<shared_ptr<Pipeline>> GetAllDependencies() const;
	const vector<weak_ptr<Pipeline>> &GetDataflowDependencies() const {
		return dataflow_dependencies;
	}
	bool HasDataflowDependencies() const {
		return !dataflow_dependencies.empty();
	}

	void Ready();
	void Reset();
	void ResetSink();
	void ResetSinkForReschedule(idx_t scheduled_workers, bool single_producer);
	//! Invalidate one operator's global state for the next reschedule round: reusable state is
	//! reset in place, all other state is destroyed and lazily recreated by
	//! InitializeOperatorState. Must run exactly once per operator per round, while no pipeline
	//! sharing the operator executes.
	static void InvalidateOperatorStateForReschedule(ClientContext &client, PhysicalOperator &op);
	//! Create missing global operator state under the operator's lock. Safe to call from
	//! concurrently scheduling pipelines: the first caller initializes shared state, later
	//! callers observe it. On the execute scheduling paths this runs after the pipeline's
	//! dependencies have finished, so state construction may consume dependency-produced sink
	//! state there; the external-input initialization path calls it before dependency completion.
	void InitializeOperatorState();
	//! Reset the pipeline-local global source state for the next reschedule
	void ResetSourceForReschedule();
	void ResetSource(bool force);
	void PrepareExternalInput();
	void ClearSource();
	void Schedule(shared_ptr<Event> &event);
	void PrepareFinalize();

	//! Compute the maximum number of threads for parallel execution of this pipeline
	//! Returns 1 if the pipeline cannot be parallelized
	idx_t GetMaxThreads();

	string ToString() const;
	void Print() const;
	void PrintDependencies() const;

	//! Returns query progress
	bool GetProgress(ProgressData &progress_data);
	//! Returns the progress of the source and of the pipeline separately
	void GetDetailedProgress(PipelineProgress &progress);

	//! Returns a list of all operators (including source and sink) involved in this pipeline
	vector<reference<PhysicalOperator>> GetOperators();
	vector<const_reference<PhysicalOperator>> GetOperators() const;
	const vector<reference<PhysicalOperator>> &GetIntermediateOperators() const;

	optional_ptr<PhysicalOperator> GetSink() {
		return sink;
	}

	optional_ptr<PhysicalOperator> GetSource() {
		return source;
	}

	bool HasSourceState() {
		return bool(GetSourceState());
	}

	//! Returns whether any of the operators in the pipeline care about preserving order
	bool IsOrderDependent() const;
	//! Marks this pipeline as fed externally instead of by scheduled source tasks
	void SetExternalInput(const vector<reference<Pipeline>> &producer_pipelines);
	bool IsExternalInput() const {
		return input_mode == PipelineInputMode::EXTERNAL_INPUT;
	}
	const vector<weak_ptr<Pipeline>> &GetExternalInputProducers() const {
		return external_input_producers;
	}
	bool HasExternalInputProducer(const Pipeline &pipeline) const;
	void SetExternalInputEvent(const shared_ptr<Event> &event);
	void CompleteExternalInput();
	bool CanUseExternalInput(const OperatorPartitionInfo &source_partition_info) const;
	PipelineExternalInputCost GetExternalInputCost() const;
	bool CanStopSourceEarly() const;

	idx_t GetBaseBatchIndex() const {
		return base_batch_index;
	}

	//! Registers a new batch index for a pipeline executor - returns the current minimum batch index
	idx_t RegisterNewBatchIndex();

	//! Updates the batch index of a pipeline (and returns the new minimum batch index)
	idx_t UpdateBatchIndex(idx_t old_index, idx_t new_index);

private:
	//! Tells the progress verifier (if any) that a new run of this pipeline starts
	void NotifyProgressReset();

private:
	//! Whether or not the pipeline has been readied
	bool ready;
	//! Whether or not the pipeline has been initialized
	atomic<bool> initialized;
	//! The source of this pipeline
	optional_ptr<PhysicalOperator> source;
	//! The chain of intermediate operators
	vector<reference<PhysicalOperator>> operators;
	//! The sink (i.e. destination) for data; this is e.g. a hash table to-be-built
	optional_ptr<PhysicalOperator> sink;

	//! The global source state
	shared_ptr<GlobalSourceState> source_state DUCKDB_GUARDED_BY(source_state_lock);
	//! Lock for resetting or inspecting the global source state pointer
	annotated_mutex source_state_lock;

	//! The parent pipelines (i.e. pipelines that are dependent on this pipeline to finish)
	vector<weak_ptr<Pipeline>> parents;
	//! The dependencies of this pipeline in other MetaPipelines
	vector<weak_ptr<Pipeline>> dependencies;
	//! The dependencies of this pipeline in the same MetaPipeline (or sibling MetaPipelines in case of recursive
	//! dependencies)
	vector<PipelineDependency> intra_dependencies;
	//! Pipelines that must be initialized before this pipeline can consume their dataflow output
	vector<weak_ptr<Pipeline>> dataflow_dependencies;
	//! Pipelines that push input into this pipeline instead of scanning its source
	vector<weak_ptr<Pipeline>> external_input_producers;

	//! The base batch index of this pipeline
	idx_t base_batch_index = 0;
	//! How this pipeline receives input chunks
	PipelineInputMode input_mode = PipelineInputMode::SCHEDULED_SOURCE;
	//! Event that represents execution of an externally fed pipeline
	weak_ptr<Event> external_input_event DUCKDB_GUARDED_BY(external_input_lock);
	ExternalInputEventState external_input_event_state DUCKDB_GUARDED_BY(external_input_lock) =
	    ExternalInputEventState::EXTERNAL_INPUT_UNSET;
	//! Lock for one-time external input initialization
	annotated_mutex external_input_lock;
	//! Lock for accessing the set of batch indexes
	mutex batch_lock;
	//! The set of batch indexes that are currently being processed
	//! Despite batch indexes being unique - this is a multiset
	//! The reason is that when we start a new pipeline we insert the current minimum batch index as a placeholder
	//! Which leads to duplicate entries in the set of active batch indexes
	multiset<idx_t> batch_indexes;

private:
	void RemoveDependency(const shared_ptr<Pipeline> &pipeline);
	//! Add a dependency on a pipeline within the same MetaPipeline (or sibling MetaPipelines in case of recursive
	//! dependencies)
	void AddIntraDependency(Pipeline &dependency, PipelineDependencyType type);
	//! Remove an optional dependency on the given pipeline, returns whether one was removed
	bool RemoveOptionalIntraDependency(const Pipeline &dependency);
	//! Copy all dependencies (within and across MetaPipelines) of 'other'
	void InheritDependencies(const Pipeline &other);
	void ClearExternalInput();
	void ScheduleSequentialTask(shared_ptr<Event> &event);
	bool LaunchScanTasks(shared_ptr<Event> &event, idx_t max_threads);
	void ResetSinkAndOperators();
	void ResetBatchIndexes();
	shared_ptr<GlobalSourceState> GetSourceState();
	void SetSourceState(shared_ptr<GlobalSourceState> state);
	void FinishSourceAndPreventBlocking(ClientContext &context);
	void PreventSourceBlocking();
	void PreventSinkBlocking();
	void PreventBlocking();

	bool TryGetMaxThreads(idx_t &max_threads);
	bool ScheduleParallel(shared_ptr<Event> &event);
	void ScheduleExternalInputEvent(shared_ptr<Event> event);
};

} // namespace duckdb
