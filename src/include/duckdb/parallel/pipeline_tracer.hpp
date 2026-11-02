//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parallel/pipeline_tracer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/types/value.hpp"

#include <condition_variable>
#include <thread>

namespace duckdb {

class Executor;
class Pipeline;
class PipelineHook;

//! Lifecycle timestamps are nanoseconds since the tracer's epoch, or NOT_SET
static constexpr int64_t TRACE_NOT_SET = -1;

//! An append-only log stored in fixed-size blocks: appending never moves or copies earlier records, so it costs the
//! same however long the log is. Records must be plain data.
template <class T>
class TraceBlockLog {
public:
	static constexpr idx_t BLOCK_SIZE = 16384;

	void Append(const T &record) {
		if (count == blocks.size() * BLOCK_SIZE) {
			blocks.push_back(make_unsafe_uniq_array_uninitialized<T>(BLOCK_SIZE));
		}
		blocks[count / BLOCK_SIZE][count % BLOCK_SIZE] = record;
		count++;
	}
	const T &operator[](idx_t index) const {
		return blocks[index / BLOCK_SIZE][index % BLOCK_SIZE];
	}
	idx_t Size() const {
		return count;
	}
	void Clear() {
		blocks.clear();
		count = 0;
	}

private:
	vector<unsafe_unique_array<T>> blocks;
	idx_t count = 0;
};

//! A pipeline's status, stored as a code in trace records
//! WAITING: execute is done, but finalize has not started (pipelines sharing its sink are still executing, or no
//! worker has picked up its prepare-finish yet)
enum class TraceStatus : uint8_t { UNSCHEDULED, NOT_STARTED, EXECUTING, WAITING, FINALIZING, FINISHED };
const char *TraceStatusName(TraceStatus status);

//! Worker ids recorded per trace row, as a bit mask; workers beyond this many are not listed
static constexpr idx_t TRACE_MAX_LISTED_WORKERS = 128;
struct TraceWorkerMask {
	uint64_t bits[TRACE_MAX_LISTED_WORKERS / 64] = {0, 0};

	void Set(idx_t worker) {
		if (worker < TRACE_MAX_LISTED_WORKERS) {
			bits[worker / 64] |= uint64_t(1) << (worker % 64);
		}
	}
	vector<idx_t> ToList() const;
};

//! Per-pipeline trace state. Counters are running totals since the pipeline started.
struct PipelineTraceEntry : public enable_shared_from_this<PipelineTraceEntry> {
	idx_t query_id = 0;
	idx_t pipeline_id = 0;

	//! Pipeline structure
	vector<idx_t> operator_ids;
	vector<string> operator_names;
	vector<idx_t> dependencies;
	idx_t source_estimated_cardinality = 0;
	//! Number of tasks the pipeline was scheduled with (its maximum parallelism)
	atomic<int64_t> task_count {TRACE_NOT_SET};
	//! Number of tasks of its current step, execute or finalize: the most threads it can use right now
	atomic<int64_t> current_step_tasks {TRACE_NOT_SET};

	//! Running totals
	atomic<uint64_t> source_chunks {0};
	atomic<uint64_t> source_rows {0};
	atomic<uint64_t> sink_rows {0};
	atomic<uint64_t> thread_ns {0};
	//! Thread time and tasks outside the execute phase: the sink's finalize steps, and initializing the sink
	atomic<uint64_t> finalize_thread_ns {0};
	atomic<uint64_t> finalize_tasks {0};
	atomic<uint64_t> initialize_thread_ns {0};
	//! When its initialize task started and ended, and whether a worker ran it (otherwise its query's client thread)
	atomic<int64_t> initialize_start_ns {TRACE_NOT_SET};
	atomic<int64_t> initialize_end_ns {TRACE_NOT_SET};
	atomic<bool> initialize_on_worker {false};
	//! With the hook on: total time its tasks waited in its queue before a worker started them, and tasks started
	atomic<uint64_t> task_wait_ns {0};
	atomic<uint64_t> tasks_started {0};
	//! Threads currently running a task of this pipeline, workers and client threads
	atomic<idx_t> running_threads {0};

	//! Lifecycle
	//! When its first task, of any stage, was submitted: when it became eligible
	atomic<int64_t> eligible_ns {TRACE_NOT_SET};
	atomic<int64_t> scheduled_ns {TRACE_NOT_SET};
	atomic<int64_t> first_task_ns {TRACE_NOT_SET};
	atomic<int64_t> last_source_ns {TRACE_NOT_SET};
	//! When the last of its tasks finished: the end of the execute phase
	atomic<int64_t> execute_end_ns {TRACE_NOT_SET};
	atomic<idx_t> tasks_finished {0};
	//! Whether the pipeline finalizes its sink in a PipelineFinishEvent
	atomic<bool> has_finish_event {false};
	//! When its meta pipeline's complete event finished: after every finalize step, including steps a sink adds
	atomic<int64_t> complete_ns {TRACE_NOT_SET};
	atomic<bool> has_complete_event {false};
	atomic<int64_t> finalize_start_ns {TRACE_NOT_SET};
	atomic<int64_t> finalize_end_ns {TRACE_NOT_SET};

	void SetOnce(atomic<int64_t> &field, int64_t value);
	//! When the pipeline finished, or TRACE_NOT_SET
	int64_t FinishedAt() const;
	void SetMax(atomic<int64_t> &field, int64_t value);
};

//! Per-query trace state
struct QueryTraceEntry {
	idx_t query_id = 0;
	string tag;
	string query;
	int64_t submitted_ns = TRACE_NOT_SET;
	int64_t finished_ns = TRACE_NOT_SET;
	bool error = false;
	vector<shared_ptr<PipelineTraceEntry>> pipelines;
};

//! Traces pipeline progress for the progress simulator: every tick (a fixed time grid set by the setting
//! dbsched_tick_interval_us), it snapshots each active pipeline's running totals and the pipeline each worker is on.
//! The snapshots and the per-query and per-pipeline records are read through the dbsched_trace_* table functions.
class PipelineTracer {
public:
	static constexpr idx_t MAX_WORKERS = 1024;

	PipelineTracer();
	~PipelineTracer();

	//! The hook is ticked before every snapshot
	void SetHook(PipelineHook &hook_p) {
		hook = &hook_p;
	}

public:
	//! The worker index of the calling thread, set for regular worker threads only
	static void SetCurrentWorker(idx_t worker_idx);
	static optional_idx GetCurrentWorker();

	//! Nanoseconds since the tracer's epoch
	int64_t Now() const;

	//! Starts, retimes, or stops the tick thread. An interval of 0 stops it
	void SetTickInterval(idx_t interval_us);
	idx_t GetTickInterval() const {
		return tick_interval_us.load();
	}

	//! Registers a query's pipelines when the executor has built them
	shared_ptr<QueryTraceEntry> RegisterQuery(Executor &executor, const vector<shared_ptr<Pipeline>> &pipelines,
	                                          string tag, string query);
	//! Marks a query as finished; its records stay readable until Reset
	void FinishQuery(QueryTraceEntry &query, bool error);

	//! Called by a thread before and after it runs a task of the pipeline
	void TaskStarted(PipelineTraceEntry &entry);
	void TaskFinished(PipelineTraceEntry &entry, int64_t start_ns);
	//! Called when a task of the pipeline completes its share of the work
	void TaskCompleted(PipelineTraceEntry &entry);
	//! Called before and after a thread runs a task of the pipeline outside its execute phase
	void StageTaskStarted(PipelineTraceEntry &entry, bool initialize);
	void StageTaskFinished(PipelineTraceEntry &entry, int64_t start_ns, bool initialize);

	//! Clears all records and restarts the tick count
	void Reset();

	//! Rows for the dbsched_trace_* table functions. The interval and tick tables can be long, so they are read in
	//! slices: the number of rows, then up to `count` rows from `offset`
	idx_t GetIntervalRowCount();
	vector<vector<Value>> GetIntervalRows(idx_t offset, idx_t count);
	idx_t GetTickRowCount();
	vector<vector<Value>> GetTickRows(idx_t offset, idx_t count);
	vector<vector<Value>> GetPipelineRows();
	vector<vector<Value>> GetQueryRows();

private:
	struct IntervalRow {
		idx_t tick;
		int64_t t_ns;
		idx_t query_id;
		idx_t pipeline_id;
		TraceStatus status;
		//! Threads on the pipeline: attached to it when the hook is on, running its tasks otherwise
		idx_t threads;
		TraceWorkerMask workers;
		idx_t running;
		idx_t assigned;
		uint64_t source_chunks;
		uint64_t source_rows;
		uint64_t sink_rows;
		uint64_t thread_ns;
		uint64_t finalize_thread_ns;
	};
	struct TickRow {
		idx_t tick;
		int64_t t_ns;
		idx_t busy_workers;
		idx_t busy_external;
		idx_t active_queries;
		idx_t active_pipelines;
		//! With the hook on: workers attached to no pipeline, and tasks of untraced work run between ticks
		idx_t free_workers;
		idx_t untraced_tasks;
		//! With the hook on: worker time running tasks, waiting attached to a pipeline, and free (running totals)
		uint64_t busy_ns;
		uint64_t attached_idle_ns;
		uint64_t free_ns;
		//! Time the tick thread spent on this tick: the hook's assignment and the snapshot
		int64_t tick_work_ns;
	};

	void TickLoop();
	void TakeSnapshot(int64_t tick_start_ns);
	static TraceStatus GetStatus(const PipelineTraceEntry &entry);

private:
	const std::chrono::steady_clock::time_point epoch;
	optional_ptr<PipelineHook> hook;

	mutex lock;
	//! Queries registered and not yet finished
	vector<shared_ptr<QueryTraceEntry>> active_queries;
	//! Queries finished since the last Reset
	vector<shared_ptr<QueryTraceEntry>> finished_queries;
	//! Queries finished since the last tick, which get one more snapshot
	vector<shared_ptr<QueryTraceEntry>> recently_finished;
	TraceBlockLog<IntervalRow> interval_rows;
	TraceBlockLog<TickRow> tick_rows;
	idx_t next_tick = 0;
	int64_t last_tick_ns = TRACE_NOT_SET;
	atomic<idx_t> next_query_id {0};

	//! Tasks currently run by threads that are not workers (client threads)
	atomic<idx_t> external_running {0};
	//! The pipeline each worker is running a task of, or nullptr
	unique_ptr<atomic<PipelineTraceEntry *>[]> worker_pipelines;

	//! Tick thread
	mutex config_lock;
	atomic<idx_t> tick_interval_us {0};
	mutex thread_lock;
	std::condition_variable thread_cv;
	bool stop_thread = false;
	unique_ptr<std::thread> tick_thread;
};

} // namespace duckdb
