#include "duckdb/execution/operator/persistent/copy_file_lifecycle.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/parallel/task_scheduler.hpp"

namespace duckdb {

void CopyFileLifecycleExecutor::WaitForJob(CopyFileLifecycleJob &job, CopyFileLifecycleWaitMode mode) {
	while (!job.IsFinished()) {
		if (mode == CopyFileLifecycleWaitMode::INTERRUPTIBLE) {
			context.InterruptCheck();
		}
		WorkOnTaskOrYield();
	}
	job.Rethrow();
}

void CopyFileLifecycleExecutor::WaitAll(CopyFileLifecycleWaitMode mode) {
	if (mode == CopyFileLifecycleWaitMode::DRAIN) {
		executor.WorkOnTasks();
		ThrowError();
		return;
	}
	while (pending_tasks.load(std::memory_order_acquire) > 0) {
		context.InterruptCheck();
		WorkOnTaskOrYield();
	}
	ThrowError();
}

void CopyFileLifecycleExecutor::FinishTask() {
	--pending_tasks;
}

void CopyFileLifecycleExecutor::PushError(const std::exception_ptr &error_p) {
	lock_guard<mutex> guard(error_lock);
	if (!error) {
		error = error_p;
	}
}

std::exception_ptr CopyFileLifecycleExecutor::GetError() {
	lock_guard<mutex> guard(error_lock);
	return error;
}

bool CopyFileLifecycleExecutor::WorkOnTask(bool throw_error) {
	shared_ptr<Task> task;
	if (!executor.GetTask(task)) {
		return false;
	}
#ifdef D_ASSERT_IS_ENABLED
	const auto result = task->Execute(TaskExecutionMode::PROCESS_ALL);
	D_ASSERT(result != TaskExecutionResult::TASK_BLOCKED);
#else
	task->Execute(TaskExecutionMode::PROCESS_ALL);
#endif
	task.reset();
	if (throw_error) {
		ThrowError();
	}
	return true;
}

void CopyFileLifecycleExecutor::WorkOnTaskOrYield() {
	if (!WorkOnTask()) {
		TaskScheduler::YieldThread();
	}
}

void CopyFileLifecycleExecutor::WaitForTaskSlot(CopyFileLifecycleWaitMode mode) {
	while (pending_tasks >= max_pending_tasks) {
		if (mode == CopyFileLifecycleWaitMode::INTERRUPTIBLE) {
			context.InterruptCheck();
		}
		if (async_threads == 0 || mode == CopyFileLifecycleWaitMode::DRAIN) {
			WorkOnTaskOrYield();
		} else {
			TaskScheduler::YieldThread();
		}
	}
}

void CopyFileLifecycleExecutor::ThrowError() {
	lock_guard<mutex> guard(error_lock);
	if (error) {
		std::rethrow_exception(error);
	}
}

} // namespace duckdb
