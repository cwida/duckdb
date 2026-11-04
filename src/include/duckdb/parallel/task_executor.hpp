//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parallel/task_executor.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/parallel/task.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/execution/task_error_manager.hpp"
#include "duckdb/common/enums/task_scheduler_type.hpp"

namespace duckdb {

class TaskExecutor;
class TaskExecutorTask;

//! A unit of work scheduled on a TaskExecutor
//! Deliberately not a Task: it cannot be handed to the TaskScheduler directly, it cannot be descheduled or
//! rescheduled, and it cannot report partial progress. The executor wraps it and owns cancellation, error handling
//! and task accounting, so implementations only have to describe their work.
class BaseExecutorTask {
public:
	explicit BaseExecutorTask(TaskExecutor &executor);
	virtual ~BaseExecutorTask() = default;

public:
	//! Perform the task's work in one call - throwing is allowed, the executor captures the error and cancels the
	//! other tasks. Run-to-completion tasks override this.
	virtual void ExecuteTask() {
		throw InternalException("BaseExecutorTask::ExecuteTask was not implemented");
	}
	//! Perform one unit of work, returning TASK_NOT_FINISHED while more remains and TASK_FINISHED when done.
	//! Incremental tasks override this instead of ExecuteTask; the default runs the whole task in one call. The
	//! executor loops this to completion while draining, and calls it once per turn on a background thread so the
	//! task yields cooperatively. The mode never reaches the task: the wrapper owns the loop.
	virtual TaskExecutionResult ExecuteTaskStep() {
		ExecuteTask();
		return TaskExecutionResult::TASK_FINISHED;
	}
	//! Called instead of ExecuteTask when the task is retired without running its work, because another task errored,
	//! because the executor was cancelled, or because the task could not be queued at all. Exactly one of ExecuteTask
	//! or Cancel runs for every task passed to TaskExecutor::ScheduleTask or ScheduleTasks.
	//! Anything a waiter watches must change on every exit path, or that waiter never wakes up. Either settle it here
	//! as well as in ExecuteTask, or settle it in the destructor, which also covers a task that was constructed but
	//! never scheduled. A destructor settle runs after the executor has already counted the task as finished, so it
	//! must not be something a drain is expected to observe.
	virtual void Cancel() {
	}
	virtual string TaskType() const {
		return "UnnamedTask";
	}

protected:
	TaskExecutor &executor;
};

//! The TaskExecutor is a helper class that enables parallel scheduling and execution of tasks
class TaskExecutor {
public:
	explicit TaskExecutor(ClientContext &context, TaskSchedulerType type = TaskSchedulerType::REGULAR);
	explicit TaskExecutor(TaskScheduler &scheduler, TaskSchedulerType type = TaskSchedulerType::REGULAR);
	~TaskExecutor();

	//! Push an error into the TaskExecutor
	void PushError(ErrorData error);
	//! Push the error of a task, which reports an out of memory error when memory runs out while recording it
	void PushTaskError(const std::exception &ex);
	void PushTaskError(const char *message);
	//! Whether or not any task has encountered an error
	bool HasError();
	//! Throw an error that was encountered during execution (if HasError() is true)
	void ThrowError();
	//! Get the first error that was encountered during execution (if HasError() is true)
	ErrorData GetError();
	//! Whether the executor has been cancelled
	bool IsCancelled() const;

	//! Schedule a new task
	void ScheduleTask(unique_ptr<BaseExecutorTask> task);
	//! Schedule a batch, cancelling unscheduled tasks if submission fails.
	void ScheduleTasks(vector<unique_ptr<BaseExecutorTask>> tasks);

	//! Work on tasks until all tasks are finished. Throws an exception if any error occurred while executing the tasks.
	void WorkOnTasks();
	//! Cancel tasks that have not started yet and work on tasks until all tasks are finished. Does not throw.
	void CancelAndDrain();

	//! Get a task - returns true if a task was found
	bool GetTask(shared_ptr<Task> &task);

public:
	//! Joins the executor when the scope is left, so that its tasks never outlive the state they reference.
	//! Only needed for an executor that outlives the scope scheduling onto it, where its own destructor is not the
	//! join. Errors are swallowed: the exception that is unwinding wins, and a recorded task error is still there.
	class JoinGuard {
	public:
		explicit JoinGuard(TaskExecutor &executor);
		~JoinGuard();

	private:
		TaskExecutor &executor;
	};

private:
	//! Work on tasks until all tasks are finished
	void DrainTasks();
	//! Label a task as finished - called by the wrapper the executor puts around every scheduled task
	void FinishTask();

private:
	friend class TaskExecutorTask;

	TaskScheduler &scheduler;
	const TaskSchedulerType type;
	TaskErrorManager error_manager;
	unique_ptr<ProducerToken> token;
	idx_t completed_tasks DUCKDB_GUARDED_BY(token->producer_lock) = 0;
	idx_t total_tasks DUCKDB_GUARDED_BY(token->producer_lock) = 0;
	atomic<bool> cancelled {false};
	optional_ptr<ClientContext> context;
};

} // namespace duckdb
