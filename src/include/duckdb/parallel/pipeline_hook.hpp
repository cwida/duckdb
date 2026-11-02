//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parallel/pipeline_hook.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/parallel/pipeline_tracer.hpp"

#include <condition_variable>
#include <deque>

namespace duckdb {

class PipelineTracer;
class Task;
class TaskScheduler;
struct PipelineTraceEntry;
struct QueryTraceEntry;

enum class PipelineHookMode : uint8_t { OFF, DEFERRED, IMMEDIATE };

//! One entry of the policy list: up to `cap` threads for a pipeline, in list order
struct PipelineHookPolicyEntry {
	idx_t query_id;
	idx_t pipeline_id;
	idx_t cap;
};

//! The thread-assignment hook for the progress simulator (docs/simulator_requirements.md, R0.1).
//!
//! Every task of a traced pipeline outside initialize (execute and finalize stages) goes into that pipeline's queue
//! instead of the shared queue; initialize tasks stay in the shared queue, where the query's client thread runs them.
//! Worker threads are attached to pipelines and only run tasks of their pipeline, with one exception: a worker
//! whose pipeline has nothing pending moves, without waiting for a tick, to a pipeline with a finalize-stage task
//! that the worker's own work unblocked, or to a pipeline sharing its sink that has a finalize-stage task pending.
//! So the worker that finishes the last piece of work something waits on (the last pipeline feeding a sink, the
//! last of a group of sibling join builds) runs what that unblocks. In
//! deferred mode a thread changes pipeline only at a tick: between ticks it runs any pending task of its pipeline, and
//! waits, still attached, when there is none. At a tick, every waiting thread is free, and free threads go down the
//! policy list: each listed pipeline with pending tasks gets threads up to its cap, preferring the threads it had
//! before. Threads left over go to pipelines the list does not name, first come, first served. In immediate mode a
//! thread is released as soon as its pipeline has nothing pending and takes the next pipeline from the same list at
//! once.
class PipelineHook {
public:
	PipelineHook(TaskScheduler &scheduler, PipelineTracer &tracer);

public:
	void SetMode(PipelineHookMode mode);
	PipelineHookMode GetMode() const {
		return mode.load();
	}
	bool IsActive() const {
		return mode.load() != PipelineHookMode::OFF;
	}
	static PipelineHookMode ParseMode(const string &mode);

	//! Takes over the tasks of traced pipelines; returns false for a task the hook does not handle
	bool TryEnqueue(shared_ptr<Task> &task);
	//! Takes over a batch of tasks of one event; the tasks it does not handle stay in `tasks`
	void TryEnqueue(vector<shared_ptr<Task>> &tasks);

	//! Worker side: the next task of the worker's pipeline, if any
	bool TryGetTask(idx_t worker_idx, shared_ptr<Task> &task);
	//! Worker side: the worker finished running the task it got from TryGetTask
	void TaskDone(idx_t worker_idx);
	//! Worker side: whether the worker is attached to a pipeline
	bool IsAttached(idx_t worker_idx);
	//! Worker side: wait until there may be work, or for at most timeout_us
	void WaitForWork(idx_t worker_idx, int64_t timeout_us);

	//! Called by the tracer's tick thread at every tick, before the snapshot
	void Tick();

	//! Replaces the policy list; an empty list means first come, first served
	void SetPolicy(vector<PipelineHookPolicyEntry> entries);

	//! Forgets a finished or cancelled query and hands back the tasks it still had queued; the caller destroys them
	//! without holding locks the executor takes
	void RemoveQuery(QueryTraceEntry &query, vector<shared_ptr<Task>> &dropped);

	//! Per pipeline: the workers attached to it and the threads assigned to it at the last tick
	struct Attachment {
		vector<idx_t> workers;
		idx_t assigned_at_tick = 0;
	};
	unordered_map<const PipelineTraceEntry *, Attachment> GetAttachments();
	idx_t GetFreeWorkers();
	//! Tasks the hook does not handle (no traced pipeline) that free workers ran between ticks
	idx_t GetUntracedTasks() const {
		return untraced_tasks.load();
	}
	void CountUntracedTask() {
		untraced_tasks++;
	}

	//! Records when a traced pipeline's first task was submitted, whether or not the hook is on
	void NoteScheduled(Task &task);

	//! Worker time running tasks, waiting attached, and free, summed over workers since the hook first ran
	struct WorkerTimes {
		uint64_t busy_ns = 0;
		uint64_t attached_idle_ns = 0;
		uint64_t free_ns = 0;
	};
	WorkerTimes GetWorkerTimes();

	//! Rows for dbsched_scheduler_state()
	vector<vector<Value>> GetStateRows();
	//! Rows for dbsched_trace_tasks(): every task the hook ran, with its queueing, start, and end times. Read in
	//! slices: the number of rows, then up to `count` rows from `offset`
	idx_t GetTaskRowCount();
	vector<vector<Value>> GetTaskRows(idx_t offset, idx_t count);
	//! Rows for dbsched_trace_attachments(): every attach and detach of a worker, read in slices
	idx_t GetAttachmentRowCount();
	vector<vector<Value>> GetAttachmentRows(idx_t offset, idx_t count);
	void ClearTaskLog();

private:
	struct PendingTask {
		shared_ptr<Task> task;
		int64_t enqueued_ns;
		//! A task outside the execute phase: prepare-finish, finish, or a step the sink added
		bool finalize_stage;
		//! The worker whose task was running when this one was submitted, i.e. whose work unblocked it
		idx_t unblocked_by;
	};
	struct HookPipeline {
		shared_ptr<PipelineTraceEntry> entry;
		std::deque<PendingTask> pending;
		idx_t attached = 0;
		idx_t busy = 0;
		idx_t assigned_at_tick = 0;
		//! Size of the last batch of tasks it received: the parallelism of its current step
		idx_t current_step_tasks = 0;
		int64_t eligible_ns = -1;
		//! Its sink's operator id: pipelines of one query with the same one share a sink
		idx_t sink_operator_id = DConstants::INVALID_INDEX;
		//! Whether its query finished or was cancelled; it is erased once no thread runs its tasks
		bool removed = false;
	};
	enum class WorkerState : uint8_t { FREE, ATTACHED_IDLE, BUSY };
	struct Worker {
		HookPipeline *pipeline = nullptr;
		WorkerState state = WorkerState::FREE;
		int64_t state_since_ns = -1;
		//! The pipeline it was attached to before the last tick, preferred when it is reassigned
		HookPipeline *previous = nullptr;
		bool busy = false;
		//! The task it is running, for the task log: its type as a code (task_type_names)
		uint16_t task_type = 0;
		int64_t task_enqueued_ns = 0;
		int64_t task_start_ns = 0;
	};
	struct TaskRecord {
		idx_t query_id;
		idx_t pipeline_id;
		uint16_t task_type;
		idx_t worker;
		int64_t enqueued_ns;
		int64_t start_ns;
		int64_t end_ns;
	};

	static optional_ptr<PipelineTraceEntry> GetTraceEntry(Task &task);
	static bool IsInitializeTask(Task &task);
	static bool IsExecuteTask(Task &task);
	//! A pipeline sharing `pipeline`'s sink whose next pending task is a finalize-stage task, with room under its cap
	HookPipeline *FindSinkFinalize(const HookPipeline &pipeline);
	//! A pipeline whose next pending task is a finalize-stage task the worker unblocked, with room under its cap
	HookPipeline *FindUnblockedFinalize(idx_t worker_idx, const HookPipeline *current);
	//! The worker running the calling thread's task, or INVALID_INDEX for a non-worker thread
	static idx_t SubmittingWorker();
	HookPipeline &GetPipeline(PipelineTraceEntry &entry);
	vector<HookPipeline *> GetCandidates();
	optional_ptr<const std::pair<idx_t, idx_t>> GetPolicyEntry(const HookPipeline &pipeline) const;
	idx_t GetCap(const HookPipeline &pipeline) const;
	void Attach(Worker &worker, HookPipeline &pipeline);
	void Detach(Worker &worker);
	bool AssignImmediate(Worker &worker);
	Worker &GetWorker(idx_t worker_idx);
	idx_t WorkerCount() const;
	//! Moves a worker to the state its attachment and busy flag imply, accounting the time in its old state
	void UpdateState(Worker &worker, int64_t now);
	void ErasePipeline(const PipelineTraceEntry *entry);

private:
	TaskScheduler &scheduler;
	PipelineTracer &tracer;
	atomic<PipelineHookMode> mode {PipelineHookMode::OFF};

	mutex lock;
	std::condition_variable work_cv;
	unordered_map<const PipelineTraceEntry *, unique_ptr<HookPipeline>> pipelines;
	vector<Worker> workers;
	vector<PipelineHookPolicyEntry> policy;
	//! Policy caps by (query id, pipeline id), and the list order
	unordered_map<idx_t, unordered_map<idx_t, std::pair<idx_t, idx_t>>> policy_index;
	atomic<idx_t> untraced_tasks {0};
	WorkerTimes worker_times;
	TraceBlockLog<TaskRecord> task_log;
	//! Task type names, indexed by the codes in the task log
	vector<string> task_type_names;
	unordered_map<string, uint16_t> task_type_codes;
	uint16_t TaskTypeCode(const string &name);
	//! Every attach and detach of a worker, for measuring how attached workers spend their time
	struct AttachRecord {
		idx_t worker;
		idx_t query_id;
		idx_t pipeline_id;
		bool attach;
		int64_t t_ns;
	};
	TraceBlockLog<AttachRecord> attach_log;
};

} // namespace duckdb
