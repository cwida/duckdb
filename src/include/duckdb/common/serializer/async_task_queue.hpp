//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/serializer/async_task_queue.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/deque.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/serializer/async_memory_governor.hpp"
#include "duckdb/parallel/async_result.hpp"

#include <functional>

namespace duckdb {

class Context;
class TaskExecutor;

//! Completion callback for one generic async task. The error is set when the task failed.
using AsyncTaskCompletionCallback = std::function<void(idx_t size, optional_ptr<const ErrorData> error)>;

//! One unit of generic async work, tagged with a byte size used for memory accounting.
class AsyncTaskRequest {
public:
	AsyncTaskRequest() = default;
	AsyncTaskRequest(unique_ptr<AsyncTask> task, idx_t size, AsyncTaskCompletionCallback completion = nullptr);

	//! The byte size reported for this task (used to bound queued/in-flight memory).
	idx_t Size() const;

	unique_ptr<AsyncTask> task;
	idx_t size = 0;
	AsyncTaskCompletionCallback completion;
};

//! Minimal generic async task scheduler.
//! Each drain task executes exactly one request, so up to max_active_tasks requests run concurrently.
//! Memory policy lives in the ManagedAsyncTaskQueue wrapper; this is the bare scheduling/draining layer.
class AsyncTaskQueue {
	friend class AsyncTaskQueueTask;
	friend class AsyncTaskQueueTaskGuard;

public:
	DUCKDB_API explicit AsyncTaskQueue(Context &context, idx_t max_active_tasks = 0);
	DUCKDB_API ~AsyncTaskQueue();

	AsyncTaskQueue(const AsyncTaskQueue &) = delete;
	AsyncTaskQueue &operator=(const AsyncTaskQueue &) = delete;

public:
	//! Return whether tasks are drained by async scheduler tasks. If false, Submit runs the task synchronously.
	DUCKDB_API bool IsAsync() const;
	//! Return whether the async task executor has captured an error.
	DUCKDB_API bool HasError();
	//! Submit one owned task to the configured sync/async path.
	DUCKDB_API void Submit(AsyncTaskRequest request);
	//! Return queued/in-flight bytes whose tasks have not completed yet.
	DUCKDB_API idx_t PendingBytes();
	//! Execute one queued task owned by this queue, or yield if the tasks are already running.
	DUCKDB_API void WorkOnPendingTask();
	//! Wait until all submitted tasks have completed.
	DUCKDB_API void Flush();
	//! Wait for all tasks and close the queue.
	DUCKDB_API void Close();
	//! Surface an error thrown by an async drain task.
	DUCKDB_API void RethrowTaskError();

private:
	//! Schedule one drain task per still-unclaimed pending request, up to max_active_tasks.
	void ScheduleTasksInternal();
	//! Release one scheduled/running task slot and its in-flight byte accounting.
	void FinishTask(idx_t task_size);
	//! Release a task slot for a scheduled task that never started because another task failed.
	void CancelScheduledTask();
	//! Release multiple reserved task slots that were never scheduled.
	void CancelScheduledTasks(idx_t task_count);

	//! Async task entry point that drains exactly one pending request.
	void DrainRequest();
	//! Run one task and invoke its completion callback.
	void ExecuteRequest(AsyncTaskRequest request);
	//! Invoke a completion callback outside the queue lock.
	void CompleteRequest(AsyncTaskRequest &request, idx_t size, optional_ptr<const ErrorData> error);
	//! Throw if a mutating API is used after Close().
	void VerifyOpen() const;
	//! Throw if the queue still owns registered or scheduled work.
	void VerifyDrained() const;
	//! Fail and discard queued tasks after an async failure once all scheduled tasks have stopped.
	void CancelPendingRequestsAfterFailure(const ErrorData &error) noexcept;

private:
	Context &context;
	//! Maximum scheduled/running tasks for this queue.
	idx_t max_active_tasks = 1;

	//! Protects state shared between the submitting thread and async tasks.
	mutex lock;
	//! Tasks waiting for an async drain task.
	deque<AsyncTaskRequest> pending_requests;
	//! Bytes queued in pending_requests that have not been claimed by a task yet.
	idx_t pending_bytes = 0;
	//! Bytes owned by running tasks that have not completed yet.
	idx_t in_flight_bytes = 0;
	//! Scheduled or running tasks for this queue.
	idx_t active_tasks = 0;
	//! Scheduled tasks that have not yet claimed a request.
	idx_t pending_tasks = 0;
	//! Set after Close() has drained the queue. Further submissions are rejected.
	bool closed = false;

	//! Async task executor. If absent, tasks are executed synchronously on submission.
	//! Keep this after task-accounting fields so queued task destructors can still release slots.
	unique_ptr<TaskExecutor> executor;
};

//! Generic, memory-managed, multi-producer async task queue.
//! Tasks are drained on the ASYNC TaskScheduler pool; queued and in-flight bytes are bounded by a shared
//! TemporaryMemoryManager reservation. Falls back to synchronous execution when async_threads == 0.
//!
//! Contract:
//! - Register is safe to call concurrently from multiple threads.
//! - Up to max_active_tasks tasks run concurrently; each drain task executes one task.
//! - With async_threads == 0, Register executes the task inline on the caller.
//! - The first task error is captured, further scheduling stops, and it is rethrown from WaitAll/Close.
//!   Partial completion is possible on error.
//! - Call WaitAll then Close in the consumer's finalize step; the destructor asserts the queue is drained.
class ManagedAsyncTaskQueue {
public:
	//! max_active_tasks == 0 -> use TaskScheduler::NumberOfAsyncThreads().
	DUCKDB_API explicit ManagedAsyncTaskQueue(Context &context, idx_t max_active_tasks = 0);
	DUCKDB_API ~ManagedAsyncTaskQueue();

	ManagedAsyncTaskQueue(const ManagedAsyncTaskQueue &) = delete;
	ManagedAsyncTaskQueue &operator=(const ManagedAsyncTaskQueue &) = delete;

public:
	//! Whether tasks are drained asynchronously (false => Register runs the task synchronously).
	DUCKDB_API bool IsAsync() const;
	//! Return whether the async task executor has captured an error.
	DUCKDB_API bool HasError();
	//! Hand off one unit of work. byte_size feeds the memory accounting (use the serialized payload size).
	//! The task's Execute() runs on an ASYNC-pool thread (or synchronously if !IsAsync()); it MUST throw on
	//! failure (the first error is captured, scheduling stops, and it is rethrown from WaitAll/Close).
	DUCKDB_API void Register(unique_ptr<AsyncTask> task, idx_t byte_size);
	//! Block the calling (producer) thread, helping drain, until queued+in-flight bytes fall under the budget.
	DUCKDB_API void ApplyBackpressure();
	//! Drain everything; after WaitAll returns (no error) all registered tasks have completed.
	DUCKDB_API void WaitAll();
	//! WaitAll + release the memory reservation + reject further Register calls.
	DUCKDB_API void Close();
	//! Surface an error thrown by an async drain task.
	DUCKDB_API void RethrowTaskError();

private:
	//! Whether scheduling should respect the in-flight window, or force all pending tasks to drain.
	enum class SchedulePolicy : uint8_t { THRESHOLD, FORCE };

private:
	//! Submit pending tasks to the low-level queue, bounded by the in-flight window unless forced.
	void SchedulePendingTasks(SchedulePolicy policy = SchedulePolicy::THRESHOLD);
	//! Grow the shared reservation coarsely to cover the current backlog.
	void UpdateMemoryState();
	//! Return queued + submitted bytes that have not completed yet. Caller must hold lock.
	idx_t TotalPendingBytes() const;
	//! Move one pending task into a submission to the low-level queue.
	bool TakePendingTaskRequest(AsyncTaskRequest &request, SchedulePolicy policy);
	//! Wrap a request with submitted-byte accounting that runs when the task completes.
	void AddCompletionAccounting(AsyncTaskRequest &request);
	//! Release byte accounting for one submitted task and refill the in-flight window.
	void CompleteSubmittedTask(idx_t size, optional_ptr<const ErrorData> error);
	//! Throw if a mutating API is used after Close().
	void VerifyOpen() const;
	//! Throw if the queue still owns registered or submitted work.
	void VerifyDrained() const;
	//! Fail and discard queued tasks after an async failure once all submitted tasks have stopped.
	void CancelPendingTasksAfterFailure(const ErrorData &error) noexcept;

private:
	Context &context;
	//! Low-level one-unit-per-task scheduler.
	unique_ptr<AsyncTaskQueue> task_queue;
	//! Shared TemporaryMemoryManager reservation governor bounding queued async task data.
	ManagedAsyncMemoryGovernor memory_governor;
	//! Maximum number of submitted/running tasks for this queue (the in-flight window).
	idx_t max_active_drain_tasks = 1;

	//! Protects state shared between registering threads and async completion callbacks.
	mutex lock;
	//! Tasks queued for submission to the low-level queue.
	deque<AsyncTaskRequest> pending_requests;
	//! Bytes queued in pending_requests that have not been submitted yet.
	idx_t pending_bytes = 0;
	//! Bytes submitted to the low-level queue that have not completed yet.
	idx_t submitted_bytes = 0;
	//! Submitted tasks that have not completed yet.
	idx_t submitted_requests = 0;
	//! Set after Close() has drained the queue. Further registration is rejected.
	bool closed = false;
};

} // namespace duckdb
