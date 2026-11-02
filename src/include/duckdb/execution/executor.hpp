//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/executor.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/query_result_state.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/pair.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/execution/task_error_manager.hpp"
#include "duckdb/execution/progress_data.hpp"
#include "duckdb/parallel/pipeline.hpp"

#include <condition_variable>

namespace duckdb {
class BufferedData;
class ClientContext;
class DataChunk;
class PhysicalOperator;
class PipelineExecutor;
class ProgressVerifier;
class OperatorState;
class QueryProfiler;
class ThreadContext;
class Task;

struct PipelineEventStack;
struct ProducerToken;
struct ScheduleEventData;

class Executor {
	friend class Pipeline;
	friend class PipelineTask;
	friend class PipelineBuildState;

public:
	static constexpr idx_t WAIT_TIME = 20;

public:
	explicit Executor(ClientContext &context);
	~Executor();

	ClientContext &context;

public:
	static Executor &Get(ClientContext &context);

	void Initialize(PhysicalOperator &physical_plan);
	void Initialize(unique_ptr<PhysicalOperator> physical_plan);

	void CancelTasks();
	//! Whether the thread driving ExecuteTask holds a partially processed task.
	//! `task` is owned by that thread alone, so only it may call this.
	bool HasTaskInProgress() const {
		return task != nullptr;
	}
	//! Run one partial task slice on the calling thread and report the resulting state
	QueryResultState ExecuteTask();
	//! Report the execution state without running any task
	QueryResultState Poll();
	void WaitForTask();
	void SignalTaskRescheduled(lock_guard<mutex> &);

	void Reset();

	vector<LogicalType> GetTypes();

	//! Push a new error
	void PushError(ErrorData exception);
	//! Push the error of a task, which reports an out of memory error when memory runs out while recording it
	void PushTaskError(const std::exception &ex);
	void PushTaskError(const char *message);

	ErrorData GetError();

	//! True if an error has been thrown
	bool HasError();
	//! Throw the exception that was pushed using PushError.
	//! Should only be called if HasError returns true
	void ThrowException();

	//! Work on tasks for this specific executor, until there are no tasks remaining
	bool WorkOnTasks();

	//! Flush a thread context into the client context
	void Flush(ThreadContext &context);

	//! Reschedules a task that was blocked
	void RescheduleTask(shared_ptr<Task> &task);

	//! Add the task to be rescheduled
	void AddToBeRescheduled(shared_ptr<Task> &task);

	//! Returns the progress of the pipelines
	idx_t GetPipelinesProgress(ProgressData &progress);

	void CompletePipeline() {
		completed_pipelines++;
	}
	ProducerToken &GetToken() {
		return *producer;
	}
	void AddEvent(shared_ptr<Event> event);

	void AddRecursiveCTE(PhysicalOperator &rec_cte);
	void ReschedulePipelines(const vector<shared_ptr<MetaPipeline>> &pipelines, vector<shared_ptr<Event>> &events);

	//! Whether or not the root of the pipeline is a result collector object
	bool HasResultCollector();
	//! Whether or not the root of the pipeline is a streaming result collector object
	bool HasStreamingResultCollector();
	//! Returns the query result - can only be used if `HasResultCollector` returns true
	unique_ptr<QueryResult> GetResult();

	//! Returns true if all pipelines have been completed
	bool ExecutionIsFinished();

	void RegisterTask() {
		executor_tasks++;
	}
	void UnregisterTask();

	//! Set the buffer of the result this query produces. Called at submission, before execution starts
	void SetResultBuffer(shared_ptr<BufferedData> result_buffer_p);
	shared_ptr<BufferedData> GetResultBuffer();

	idx_t GetTotalPipelines() const {
		return total_pipelines;
	}

	idx_t GetCompletedPipelines() const {
		return completed_pipelines.load();
	}

	//! The progress verifier of this query (if debug_verify_progress is enabled)
	optional_ptr<ProgressVerifier> GetProgressVerifier() {
		return progress_verifier.get();
	}

private:
	//! Whether the result sink waits on the consumer: a producer is parked for the retention
	//! decision, or for space that only a pop frees
	bool ResultCollectorIsBlocked();
	//! Whether this query's store can park a producer for the consumer at all. A store settled on
	//! retained never parks, so the retained hot path skips the readiness checks
	bool ResultStoreCanPark();
	void InitializeInternal(PhysicalOperator &physical_plan);

	void ScheduleEvents(const vector<shared_ptr<MetaPipeline>> &meta_pipelines);
	void ScheduleEventsInternal(ScheduleEventData &event_data);

	static void VerifyScheduledEvents(const vector<shared_ptr<Event>> &events);
	static void VerifyScheduledEventsInternal(const idx_t i, const vector<reference<Event>> &vertices,
	                                          vector<bool> &visited, vector<bool> &recursion_stack);

	bool NextExecutor();
	//! Interrupt the execution of the pipelines of this executor after an error
	void InterruptPipelines();
	//! Interrupt them from a task, where an exception would end the process
	void TryInterruptPipelines();
	//! The state to report when this thread has no task to run
	QueryResultState IdleState();
	//! Cancel all tasks and throw the recorded error
	void FailExecution();
	//! Advance to the next executor, or record and return FINISHED
	QueryResultState FinishExecution();

	shared_ptr<Pipeline> CreateChildPipeline(Pipeline &current, PhysicalOperator &op);

	void VerifyPipeline(Pipeline &pipeline);
	void VerifyPipelines();

private:
	optional_ptr<PhysicalOperator> physical_plan;
	unique_ptr<PhysicalOperator> owned_plan;

	mutex executor_lock;
	//! All pipelines of the query plan
	vector<shared_ptr<Pipeline>> pipelines;
	//! The root pipelines of the query
	vector<shared_ptr<Pipeline>> root_pipelines;
	//! The recursive CTE's in this query plan
	vector<reference<PhysicalOperator>> recursive_ctes;
	//! The pipeline executor for the root pipeline
	unique_ptr<PipelineExecutor> root_executor;
	//! The current root pipeline index
	idx_t root_pipeline_idx;
	//! The producer of this query
	unique_ptr<ProducerToken> producer;
	//! List of events
	vector<shared_ptr<Event>> events;
	//! The query profiler
	shared_ptr<QueryProfiler> profiler;
	//! Task error manager
	TaskErrorManager error_manager;
	//! Verifies the progress reported by the pipelines (debug setting)
	unique_ptr<ProgressVerifier> progress_verifier;

	//! The amount of completed pipelines of the query
	atomic<idx_t> completed_pipelines;
	//! The total amount of pipelines in the query
	idx_t total_pipelines;
	//! Whether or not execution is cancelled
	bool cancelled;

	//! The last pending execution result (if any)
	QueryResultState execution_result;
	//! The current task in process (if any)
	shared_ptr<Task> task;

	//! Task that have been descheduled
	reference_map_t<Task, shared_ptr<Task>> to_be_rescheduled_tasks;
	//! The semaphore to signal task rescheduling
	std::condition_variable task_reschedule;

	//! Currently alive executor tasks
	atomic<idx_t> executor_tasks;
	//! Leaf lock for the result buffer slot. It must not share executor_lock, which is held while
	//! readiness is checked
	mutex result_buffer_lock;
	//! The buffer of the result this query produces, or null for a query that has none
	shared_ptr<BufferedData> result_buffer;

	//! Total time blocked while waiting on tasks, in microseconds
	atomic<idx_t> blocked_thread_time;
};
} // namespace duckdb
