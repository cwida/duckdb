#include "duckdb/parallel/task_executor.hpp"
#include "duckdb/parallel/task_notifier.hpp"
#include "duckdb/parallel/task_scheduler.hpp"

namespace duckdb {

//! The wrapper the executor puts around every scheduled task
//! It owns the cancel check, the task notifier, the error handling and the task accounting, so that none of those
//! depend on what the task itself does
class TaskExecutorTask : public Task {
public:
	TaskExecutorTask(TaskExecutor &executor, unique_ptr<BaseExecutorTask> task_p)
	    : executor(executor), task(std::move(task_p)) {
	}

public:
	TaskExecutionResult Execute(TaskExecutionMode mode) override {
		FinishGuard guard(executor);
		if (executor.HasError() || executor.IsCancelled()) {
			// another task encountered an error, or the executor was cancelled - retire without doing the work
			return RunGuarded([&]() { task->Cancel(); }, "Unknown exception while cancelling a task");
		}
		TaskExecutionResult result;
		try {
			TaskNotifier task_notifier {executor.context};
			// PROCESS_ALL must run to completion, so loop the steps; PROCESS_PARTIAL does one and yields
			do {
				result = task->ExecuteTaskStep();
			} while (result == TaskExecutionResult::TASK_NOT_FINISHED && mode == TaskExecutionMode::PROCESS_ALL);
		} catch (std::exception &ex) {
			executor.PushTaskError(ex);
			return TaskExecutionResult::TASK_ERROR;
		} catch (...) { // LCOV_EXCL_START
			executor.PushTaskError("Unknown exception during task execution");
			return TaskExecutionResult::TASK_ERROR;
		} // LCOV_EXCL_STOP
		if (result == TaskExecutionResult::TASK_NOT_FINISHED) {
			// yielded under PROCESS_PARTIAL: the scheduler re-enqueues this wrapper, keep the task's slot open
			guard.Dismiss();
			return TaskExecutionResult::TASK_NOT_FINISHED;
		}
		D_ASSERT(result == TaskExecutionResult::TASK_FINISHED);
		return TaskExecutionResult::TASK_FINISHED;
	}

	void Deschedule() override {
		throw InternalException("Tasks scheduled on a TaskExecutor cannot be descheduled");
	}

	void Reschedule() override {
		throw InternalException("Tasks scheduled on a TaskExecutor cannot be rescheduled");
	}

	string TaskType() const override {
		return task->TaskType();
	}

	//! Retire a task that never made it into the queue, without touching the executor's accounting
	void Retire() noexcept {
		try {
			RunGuarded([&]() { task->Cancel(); }, "Unknown exception while cancelling a task");
		} catch (...) {
			// Preserve the submission exception if reporting a cancellation error fails.
		}
	}

private:
	//! Settles the executor's task counter on every exit path, so that a drain always terminates
	class FinishGuard {
	public:
		explicit FinishGuard(TaskExecutor &executor) : executor(executor) {
		}
		~FinishGuard() {
			if (!dismissed) {
				executor.FinishTask();
			}
		}
		//! Keep the task's slot open across a yield - the wrapper will run again and finish it later
		void Dismiss() {
			dismissed = true;
		}

	private:
		TaskExecutor &executor;
		bool dismissed = false;
	};

	template <class FUNC>
	TaskExecutionResult RunGuarded(FUNC &&callback, const char *unknown_error) {
		try {
			callback();
		} catch (std::exception &ex) {
			executor.PushTaskError(ex);
			return TaskExecutionResult::TASK_ERROR;
		} catch (...) { // LCOV_EXCL_START
			executor.PushTaskError(unknown_error);
			return TaskExecutionResult::TASK_ERROR;
		} // LCOV_EXCL_STOP
		return TaskExecutionResult::TASK_FINISHED;
	}

private:
	TaskExecutor &executor;
	unique_ptr<BaseExecutorTask> task;
};

TaskExecutor::TaskExecutor(TaskScheduler &scheduler, TaskSchedulerType type_p)
    : scheduler(scheduler), type(type_p), token(scheduler.CreateProducer()) {
}

TaskExecutor::TaskExecutor(ClientContext &context_p, TaskSchedulerType type_p)
    : TaskExecutor(TaskScheduler::GetScheduler(context_p), type_p) {
	context = context_p;
}

TaskExecutor::~TaskExecutor() {
	// tasks can still be queued if we unwound between scheduling them and draining them
	// they hold a reference to this executor, so they must not outlive it
	try {
		CancelAndDrain();
	} catch (...) { // NOLINT
	}
}

void TaskExecutor::PushError(ErrorData error) {
	error_manager.PushError(std::move(error));
}

void TaskExecutor::PushTaskError(const std::exception &ex) {
	error_manager.PushTaskError([&]() { return ErrorData(ex); });
}

void TaskExecutor::PushTaskError(const char *message) {
	error_manager.PushTaskError([&]() { return ErrorData(message); });
}

bool TaskExecutor::HasError() {
	return error_manager.HasError();
}

void TaskExecutor::ThrowError() {
	error_manager.ThrowException();
}

ErrorData TaskExecutor::GetError() {
	return error_manager.GetError();
}

bool TaskExecutor::IsCancelled() const {
	return cancelled;
}

void TaskExecutor::ScheduleTask(unique_ptr<BaseExecutorTask> task) {
	// wrap before taking ownership of a slot, so that a failure to allocate the wrapper needs no rollback
	shared_ptr<TaskExecutorTask> scheduled_task;
	try {
		scheduled_task = make_shared_ptr<TaskExecutorTask>(*this, std::move(task));
	} catch (...) {
		if (task) {
			TaskExecutorTask unscheduled_task(*this, std::move(task));
			unscheduled_task.Retire();
		}
		throw;
	}
	{
		const annotated_lock_guard<annotated_mutex> lock(token->producer_lock);
		++total_tasks;
	}
	try {
		// hold on to our own reference, so that a task that fails to queue can still be retired
		scheduler.ScheduleTask(*token, scheduled_task, type);
	} catch (...) {
		{
			const annotated_lock_guard<annotated_mutex> lock(token->producer_lock);
			// We failed to schedule the task, so we decrement the total number of tasks, instead of incrementing
			// completed tasks count.
			--total_tasks;
			token->producer_cv.notify_one();
		}
		scheduled_task->Retire();
		throw;
	}
}

void TaskExecutor::ScheduleTasks(vector<unique_ptr<BaseExecutorTask>> tasks) {
	if (tasks.empty()) {
		return;
	}
	if (tasks.size() == 1) {
		ScheduleTask(std::move(tasks.front()));
		return;
	}
	vector<shared_ptr<Task>> scheduled_tasks;
	bool accounted = false;
	try {
#ifdef DUCKDB_NO_THREADS
		// The no-threads bulk queue can publish a prefix before an allocation fails.
		for (auto &task : tasks) {
			ScheduleTask(std::move(task));
		}
#else
		scheduled_tasks.reserve(tasks.size());
		for (auto &task : tasks) {
			scheduled_tasks.push_back(make_shared_ptr<TaskExecutorTask>(*this, std::move(task)));
		}
		{
			const annotated_lock_guard<annotated_mutex> lock(token->producer_lock);
			total_tasks += tasks.size();
		}
		accounted = true;
		// The threaded bulk queue publishes all tasks or none, moving ownership on success.
		scheduler.ScheduleTasks(*token, scheduled_tasks, type);
#endif
	} catch (...) {
		if (accounted) {
			const annotated_lock_guard<annotated_mutex> lock(token->producer_lock);
			total_tasks -= tasks.size();
			token->producer_cv.notify_one();
		}
		for (auto &task : scheduled_tasks) {
			static_cast<TaskExecutorTask &>(*task).Retire();
		}
		for (auto &task : tasks) {
			if (task) {
				TaskExecutorTask unscheduled_task(*this, std::move(task));
				unscheduled_task.Retire();
			}
		}
		throw;
	}
}

void TaskExecutor::FinishTask() {
	const annotated_lock_guard<annotated_mutex> lk(token->producer_lock);
	++completed_tasks;
	token->producer_cv.notify_one();
}

void TaskExecutor::DrainTasks() {
	// wait for all active tasks to finish, executing queued tasks on this thread where possible
	shared_ptr<Task> task_from_producer;
	while (true) {
		{
			annotated_unique_lock<annotated_mutex> lk(token->producer_lock);
			if (completed_tasks == total_tasks) {
				break;
			}
			if (!scheduler.GetTaskFromProducerLocked(*token, task_from_producer)) {
				token->producer_cv.wait(lk);
				continue;
			}
		}

		const auto res = task_from_producer->Execute(TaskExecutionMode::PROCESS_ALL);
		std::ignore = res;
		// PROCESS_ALL runs a task to completion, so a drain only ever sees a finished or errored task
		D_ASSERT(res == TaskExecutionResult::TASK_FINISHED || res == TaskExecutionResult::TASK_ERROR);
		task_from_producer.reset();
	}
}

void TaskExecutor::WorkOnTasks() {
	DrainTasks();

	// check if we ran into any errors while executing the tasks
	if (HasError()) {
		// throw the error
		ThrowError();
	}
}

void TaskExecutor::CancelAndDrain() {
	// make tasks that have not started yet bail out instead of executing their work
	cancelled = true;
	DrainTasks();
}

bool TaskExecutor::GetTask(shared_ptr<Task> &task) {
	return scheduler.GetTaskFromProducer(*token, task);
}

BaseExecutorTask::BaseExecutorTask(TaskExecutor &executor) : executor(executor) {
}

TaskExecutor::JoinGuard::JoinGuard(TaskExecutor &executor) : executor(executor) {
}

TaskExecutor::JoinGuard::~JoinGuard() {
	try {
		executor.WorkOnTasks();
	} catch (...) { // NOLINT
	}
}

} // namespace duckdb
