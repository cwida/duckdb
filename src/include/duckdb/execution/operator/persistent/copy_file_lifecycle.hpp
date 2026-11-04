//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/persistent/copy_file_lifecycle.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/parallel/task_executor.hpp"

#include <exception>

namespace duckdb {

class ClientContext;

enum class CopyFileLifecycleWaitMode : uint8_t { INTERRUPTIBLE, DRAIN };

//! A file open, finalize or directory setup scheduled on the executor, which waiters poll for completion
class CopyFileLifecycleJob {
public:
	bool IsFinished() const {
		return finished.load(std::memory_order_acquire);
	}

	void Complete() {
		finished.store(true, std::memory_order_release);
	}

	void CompleteException(const std::exception_ptr &error_p) {
		error = error_p;
		Complete();
	}

	void Rethrow() const {
		if (error) {
			std::rethrow_exception(error);
		}
	}

private:
	atomic<bool> finished {false};
	std::exception_ptr error;
};

template <class FUNC>
class CopyFileLifecycleTask;

//! Runs the file opens and finalizes of a COPY on the async threads, so that they overlap with its writes
class CopyFileLifecycleExecutor {
	static constexpr idx_t MIN_PENDING_TASKS = 4096;

public:
	explicit CopyFileLifecycleExecutor(ClientContext &context_p)
	    : context(context_p), executor(context_p, TaskSchedulerType::ASYNC) {
		auto &scheduler = TaskScheduler::GetScheduler(context);
		async_threads = scheduler.NumberOfAsyncThreads();
		auto regular_threads = scheduler.NumberOfThreads();
		max_pending_tasks = MaxValue<idx_t>(MIN_PENDING_TASKS, (async_threads + regular_threads) * 4);
	}

	~CopyFileLifecycleExecutor() {
		// A queued task's Cancel reaches back into this object (GetError, FinishTask). Join here, while every
		// member is still alive, rather than leaving it to ~TaskExecutor, which runs after error_lock and error
		// have already been destroyed. CancelAndDrain does not throw.
		executor.CancelAndDrain();
	}

public:
	template <class FUNC>
	void Schedule(shared_ptr<CopyFileLifecycleJob> job, CopyFileLifecycleWaitMode mode, FUNC &&task);
	void WaitForJob(CopyFileLifecycleJob &job, CopyFileLifecycleWaitMode mode);
	void WaitAll(CopyFileLifecycleWaitMode mode);
	void WorkOnTaskOrYield();
	void FinishTask();
	void PushError(const std::exception_ptr &error);
	//! The first error pushed by a task, if any
	std::exception_ptr GetError();

private:
	bool WorkOnTask(bool throw_error = true);
	void WaitForTaskSlot(CopyFileLifecycleWaitMode mode);
	void ThrowError();

private:
	ClientContext &context;
	TaskExecutor executor;
	idx_t async_threads;
	idx_t max_pending_tasks;
	atomic<idx_t> pending_tasks {0};
	mutex error_lock;
	std::exception_ptr error;
};

class CopyFileLifecycleTaskFinishGuard {
public:
	explicit CopyFileLifecycleTaskFinishGuard(CopyFileLifecycleExecutor &lifecycle_p) : lifecycle(lifecycle_p) {
	}

	~CopyFileLifecycleTaskFinishGuard() {
		Finish();
	}

	void Finish() {
		if (!finished) {
			lifecycle.FinishTask();
			finished = true;
		}
	}

private:
	CopyFileLifecycleExecutor &lifecycle;
	bool finished = false;
};

template <class FUNC>
class CopyFileLifecycleTask : public BaseExecutorTask {
public:
	CopyFileLifecycleTask(TaskExecutor &executor_p, CopyFileLifecycleExecutor &lifecycle_p,
	                      shared_ptr<CopyFileLifecycleJob> job_p, FUNC task_p)
	    : BaseExecutorTask(executor_p), lifecycle(lifecycle_p), job(std::move(job_p)), task(std::move(task_p)) {
	}

	~CopyFileLifecycleTask() override {
		// Neither ExecuteTask nor Cancel ran, because the executor could not take the task at all. The job
		// waiter and pending_tasks still have to be settled, or a waiter spins with nothing left to run.
		if (settled) {
			return;
		}
		try {
			Cancel();
		} catch (...) { // NOLINT
		}
	}

public:
	void ExecuteTask() override {
		settled = true;
		CopyFileLifecycleTaskFinishGuard finish_guard(lifecycle);
		try {
			task();
			if (!job->IsFinished()) {
				job->Complete();
			}
		} catch (...) {
			auto error = std::current_exception();
			job->CompleteException(error);
			lifecycle.PushError(error);
		}
	}

	void Cancel() override {
		// the task is retired without running - settle the job, WaitForJob spins until it is finished
		settled = true;
		CopyFileLifecycleTaskFinishGuard finish_guard(lifecycle);
		if (job->IsFinished()) {
			return;
		}
		auto error = lifecycle.GetError();
		if (!error) {
			error = std::make_exception_ptr(InternalException("COPY file task was cancelled before it could run"));
		}
		job->CompleteException(error);
	}

	string TaskType() const override {
		return "CopyFileLifecycleTask";
	}

private:
	CopyFileLifecycleExecutor &lifecycle;
	shared_ptr<CopyFileLifecycleJob> job;
	FUNC task;
	//! Whether ExecuteTask or Cancel ran, so the destructor knows the job and the count are settled
	bool settled = false;
};

template <class FUNC>
void CopyFileLifecycleExecutor::Schedule(shared_ptr<CopyFileLifecycleJob> job, CopyFileLifecycleWaitMode mode,
                                         FUNC &&task) {
	WaitForTaskSlot(mode);
	auto job_ref = job;
	using TaskType = CopyFileLifecycleTask<typename std::decay<FUNC>::type>;
	auto lifecycle_task = make_uniq<TaskType>(executor, *this, std::move(job), std::forward<FUNC>(task));
	// past this point the task settles pending_tasks itself, on the execute path and on the cancel path
	++pending_tasks;
	executor.ScheduleTask(std::move(lifecycle_task));
	if (async_threads == 0) {
		WaitForJob(*job_ref, mode);
	}
}

} // namespace duckdb
