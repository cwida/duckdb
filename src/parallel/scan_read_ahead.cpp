
#include "duckdb/parallel/scan_read_ahead.hpp"

#include "duckdb/common/file_system.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/logging/log_type.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parallel/task_scheduler.hpp"

namespace duckdb {

void ReadAheadJobCompletion::AddIOTask() {
	++pending_io_tasks;
}

void ReadAheadJobCompletion::FinishIOTask() {
	const auto previous = pending_io_tasks.fetch_sub(1);
	D_ASSERT(previous > 0);
	if (previous > 1) {
		// I/O tasks still outstanding, nothing to wake yet
		return;
	}
	// wake the parked scan task, if any
	const annotated_lock_guard<annotated_mutex> guard {parked_scan.lock};
	parked_scan.UnblockTasks();
}

bool ReadAheadJobCompletion::TryPark(const InterruptState &interrupt_state) {
	// checking the pending count under the same lock FinishIOTask takes before waking prevents lost wake-ups
	const annotated_lock_guard<annotated_mutex> guard {parked_scan.lock};
	if (pending_io_tasks.load() == 0) {
		return false;
	}
	return parked_scan.BlockTask(interrupt_state);
}

bool ReadAheadJobCompletion::TryPark(TableFunctionInput &data_p) {
	if (data_p.results_execution_mode != AsyncResultsExecutionMode::TASK_EXECUTOR || !data_p.interrupt_state) {
		return false;
	}
	if (!TryPark(*data_p.interrupt_state)) {
		return false;
	}
	data_p.async_result = AsyncResultType::BLOCKED;
	return true;
}

void ReadAheadJobCompletion::WaitForIO() {
	shared_ptr<Task> task;
	while (pending_io_tasks.load() > 0) {
		// run scheduled I/O inline, the tasks may belong to any job but always make progress
		if (executor->GetTask(task)) {
			task->Execute(TaskExecutionMode::PROCESS_ALL);
			task.reset();
			continue;
		}
		// the remaining I/O is running on other threads, wait for it to finish
		TaskScheduler::YieldThread();
	}
}

//! Async task that runs one scan job's I/O and releases the job's pending count when done.
class ReadAheadIOTask : public BaseExecutorTask {
public:
	ReadAheadIOTask(TaskExecutor &executor, unique_ptr<AsyncTask> task_p,
	                shared_ptr<ReadAheadJobCompletion> completion_p, ScanReadAhead &read_ahead_p)
	    : BaseExecutorTask(executor), task(std::move(task_p)), completion(std::move(completion_p)),
	      read_ahead(read_ahead_p) {
		completion->AddIOTask();
	}
	~ReadAheadIOTask() override {
		// If we are done we decrement the pending
		completion->FinishIOTask();
	}

	void ExecuteTask() override {
		try {
			// Does the actual IO
			task->Execute();
		} catch (std::exception &ex) {
			// recording the error through the read-ahead also wakes the threads waiting for progress
			read_ahead.PushError(ErrorData(ex));
		}
	}

private:
	unique_ptr<AsyncTask> task;
	shared_ptr<ReadAheadJobCompletion> completion;
	//! Outlives the task, the read-ahead drains its executor before it is destroyed
	ScanReadAhead &read_ahead;
};

//! Async task that opens one file ahead of decoding
class FileOpenTask : public BaseExecutorTask {
public:
	FileOpenTask(TaskExecutor &executor, std::function<void()> open_fn_p, std::function<void()> cancel_fn_p)
	    : BaseExecutorTask(executor), open_fn(std::move(open_fn_p)), cancel_fn(std::move(cancel_fn_p)) {
	}

	void ExecuteTask() override {
		open_fn();
	}

	//! The open never runs, so settle the state the scan waits on - it only ever leaves OPENING in one of these two
	void Cancel() override {
		cancel_fn();
	}

private:
	std::function<void()> open_fn;
	std::function<void()> cancel_fn;
};

ScanReadAheadJob::~ScanReadAheadJob() = default;

//! The file descriptor limit is per process, so all scans share one count
static atomic<idx_t> &OpenFileCount() {
	static atomic<idx_t> count {0};
	return count;
}

ReadAheadOpenFile::ReadAheadOpenFile() {
	++OpenFileCount();
}

ReadAheadOpenFile::~ReadAheadOpenFile() {
	--OpenFileCount();
}

idx_t ReadAheadOpenFile::Count() {
	return OpenFileCount().load();
}

static idx_t GetOpenFileBudget(ClientContext &context) {
	idx_t file_descriptor_limit = Settings::Get<DebugFileDescriptorLimitSetting>(context);
	if (file_descriptor_limit == 0) {
		auto system_limit = FileSystem::GetFileDescriptorLimit();
		if (!system_limit.IsValid()) {
			return NumericLimits<idx_t>::Maximum();
		}
		file_descriptor_limit = system_limit.GetIndex();
	}
	// files hold their descriptors until their readers close, so only use a share of the limit
	return FileSystem::GetFileDescriptorShare(file_descriptor_limit);
}

ScanReadAhead::ScanReadAhead(ClientContext &context, idx_t read_ahead_depth_p,
                             unique_ptr<ManagedAsyncMemoryGovernor> memory_governor_p)
    : context(context), read_ahead_depth(read_ahead_depth_p), memory_governor(std::move(memory_governor_p)),
      open_window(MaxValue<idx_t>(TaskScheduler::GetScheduler(context).NumberOfAsyncThreads() * 2,
                                  memory_governor ? 0 : read_ahead_depth)),
      open_file_budget(GetOpenFileBudget(context)) {
	D_ASSERT(read_ahead_depth_p > 0);
	backlog_budget = memory_governor ? memory_governor->BackpressureBudget() : NumericLimits<idx_t>::Maximum();
	executor = make_shared_ptr<TaskExecutor>(context, TaskSchedulerType::ASYNC);
}

ScanReadAhead::~ScanReadAhead() {
	CancelAndDrain();
}

void ScanReadAhead::CancelAndDrain() {
	executor->CancelAndDrain();
}

unique_ptr<ScanReadAhead> ScanReadAhead::Create(ClientContext &context) {
	if (TaskScheduler::GetScheduler(context).NumberOfAsyncThreads() == 0) {
		// read-ahead schedules its I/O on the async pool, without async threads there is nothing to gain
		return nullptr;
	}
	const auto configured_depth = Settings::Get<ReadAheadDepthSetting>(context);
	if (configured_depth == 0) {
		return nullptr;
	}
	if (configured_depth < 0) {
		// automatic mode, unlimited depth, the backlog is bounded by a temp-memory reservation instead
		return make_uniq<ScanReadAhead>(context, NumericLimits<idx_t>::Maximum(),
		                                make_uniq<ManagedAsyncMemoryGovernor>(context));
	}
	return make_uniq<ScanReadAhead>(context, NumericCast<idx_t>(configured_depth), nullptr);
}

bool ScanReadAhead::TryProduceJob(const ProduceJobCallback &claim_and_schedule) {
	ThrowIfError();
	if (IsDone() || !TryReserveSlot()) {
		return false;
	}
	ProducerReservation reservation(*this);
	try {
		vector<unique_ptr<AsyncTask>> io_tasks;
		auto job = claim_and_schedule(io_tasks);
		if (!job) {
			// an async open that failed while we were claiming must fail the scan, not quiet-finish it
			ThrowIfError();
			SetDone();
			return false;
		}
		PushJob(std::move(job), std::move(io_tasks));
		reservation.committed = true;
	} catch (std::exception &ex) {
		PushError(ErrorData(ex));
		throw;
	} catch (...) { // LCOV_EXCL_START
		PushError(ErrorData("Unknown exception while producing a read-ahead job"));
		throw;
	} // LCOV_EXCL_STOP
	return true;
}

bool ScanReadAhead::IsDone() const {
	return done.load();
}

bool ScanReadAhead::HasActiveProducers() const {
	return active_producers.load() > 0;
}

unique_ptr<ScanReadAheadJob> ScanReadAhead::ClaimJob() {
	lock_guard<mutex> guard(lock);
	if (ready_queue.empty()) {
		return nullptr;
	}
	auto job = std::move(ready_queue.front());
	ready_queue.pop_front();
	ReleaseSlot();
	NotifyProgress(false);
	return job;
}

void ScanReadAhead::WaitForJob(ScanReadAheadJob &job) {
	job.SettleIO();
	// the job's I/O has completed, release its budget charge
	const auto budget = backlog_budget.load();
	const auto previous = pending_io_bytes.fetch_sub(job.io_bytes);
	if (previous >= budget && previous - job.io_bytes < budget) {
		// the scan is no longer over budget, a waiting thread can produce
		SignalProgress(false);
	}
	job.io_bytes = 0;
	ThrowIfError();
}

ScanReadAheadAcquire ScanReadAhead::AcquireJob(ClientContext &context, TableFunctionInput &data_p,
                                               const ProduceJobCallback &claim_and_schedule,
                                               unique_ptr<ScanReadAheadJob> &job) {
	D_ASSERT(!job);
	optional<std::chrono::steady_clock::time_point> yield_deadline;
	while (true) {
		const auto epoch = ProgressEpoch();
		// keep the queue full before claiming
		while (TryProduceJob(claim_and_schedule)) {
		}
		job = ClaimJob();
		if (!job && IsDone() && !HasActiveProducers()) {
			// re-check the queue, a job may have been pushed between the claim and the done check
			job = ClaimJob();
			if (!job) {
				if (HasPendingJobs()) {
					// with no producers left the held-back jobs can never be admitted, their rows would be lost
					throw InternalException("Read-ahead exhausted with jobs still held back, "
					                        "batch indexes must be gapless from 0");
				}
				return ScanReadAheadAcquire::EXHAUSTED;
			}
		}
		if (!job) {
			// another thread is between claiming an assignment and pushing its job, a short claim is caught by
			// yielding and a long one is waited for
			const auto now = std::chrono::steady_clock::now();
			if (!yield_deadline) {
				yield_deadline = now + std::chrono::microseconds(PRODUCER_YIELD_MICROS);
			}
			if (now < *yield_deadline) {
				context.InterruptCheck();
				TaskScheduler::YieldThread();
			} else {
				WaitForProgress(context, epoch);
			}
			continue;
		}
		// park until the job's scheduled I/O completes, the last I/O task to finish wakes the scan task
		// a parked task frees its thread for one that opens another file, so not while the open file budget is full
		if (CanScheduleOpen(0) && job->io_completion->TryPark(data_p)) {
			return ScanReadAheadAcquire::PARKED;
		}
		// parking is not available to this caller, the open file budget is full or the I/O has already completed
		WaitForJob(*job);
		return ScanReadAheadAcquire::ACQUIRED;
	}
}

void ScanReadAhead::SetDone() {
	done = true;
}

idx_t ScanReadAhead::ReservationDepth() const {
	// over budget one job stays queued, claimed jobs hold the budget until their task resumes
	const bool over_budget =
	    pending_io_bytes.load() >= backlog_budget.load() || ReadAheadOpenFile::Count() >= open_file_budget;
	return over_budget ? 1 : read_ahead_depth;
}

bool ScanReadAhead::TryReserveSlot() {
	// take the minimum job charge up front so concurrent producers cannot all pass the budget check together
	const bool over_memory_budget = pending_io_bytes.fetch_add(MINIMUM_JOB_IO_CHARGE) >= backlog_budget.load();
	// queued jobs keep their files open, so the open file budget bounds the queue too
	const bool over_budget = over_memory_budget || ReadAheadOpenFile::Count() >= open_file_budget;
	const idx_t depth = over_budget ? 1 : read_ahead_depth;
	if (active_jobs.fetch_add(1) >= depth) {
		--active_jobs;
		pending_io_bytes -= MINIMUM_JOB_IO_CHARGE;
		if (active_jobs.load() < ReservationDepth()) {
			// a concurrent attempt failed on this attempt's charge or slot, let it retry
			SignalProgress(false);
		}
		return false;
	}
	++active_producers;
	return true;
}

void ScanReadAhead::PushJob(unique_ptr<ScanReadAheadJob> job, vector<unique_ptr<AsyncTask>> io_tasks) {
	auto completion = make_shared_ptr<ReadAheadJobCompletion>(executor);
	job->io_completion = completion;
	// wrap all reads before scheduling any, a wrapped task settles the completion even when scheduling throws
	vector<unique_ptr<BaseExecutorTask>> read_tasks;
	read_tasks.reserve(io_tasks.size());
	for (auto &task : io_tasks) {
		job->io_bytes += task->GetIOSize();
		read_tasks.push_back(make_uniq<ReadAheadIOTask>(*executor, std::move(task), completion, *this));
	}
	job->io_bytes = MaxValue<idx_t>(job->io_bytes, MINIMUM_JOB_IO_CHARGE);
	// the minimum charge was taken by TryReserveSlot, charge the remainder
	pending_io_bytes += job->io_bytes - MINIMUM_JOB_IO_CHARGE;
	// schedule the reads detached on the async pool right away, logged under their own label
	if (!read_tasks.empty()) {
		DUCKDB_LOG(context, AsyncTaskScheduleLogType, "READ_AHEAD", read_tasks.size());
	}
	executor->ScheduleTasks(std::move(read_tasks));
	lock_guard<mutex> guard(lock);
	if (memory_governor) {
		// Reserve room for the next job before admission checks the budget.
		memory_governor->UpdateReservation(pending_io_bytes.load() + MINIMUM_JOB_IO_CHARGE);
		backlog_budget = memory_governor->BackpressureBudget();
	}
	DUCKDB_LOG(context, ReadAheadBudgetLogType, pending_io_bytes.load(), backlog_budget.load(), active_jobs.load());
	// producers push concurrently, so admit jobs to the queue in batch-index order
	if (job->batch_index < next_batch_index || !pending_jobs.emplace(job->batch_index, std::move(job)).second) {
		throw InternalException("Read-ahead jobs must have unique batch indexes, assigned densely from 0");
	}
	idx_t admitted = 0;
	while (!pending_jobs.empty() && pending_jobs.begin()->first == next_batch_index) {
		ready_queue.push_back(std::move(pending_jobs.begin()->second));
		pending_jobs.erase(pending_jobs.begin());
		next_batch_index++;
		admitted++;
	}
	NotifyProgress(admitted > 1);
}

void ScanReadAhead::PushError(ErrorData error) {
	executor->PushError(std::move(error));
	SignalProgress(true);
}

void ScanReadAhead::ScheduleFileOpen(std::function<void()> open_fn, std::function<void()> cancel_fn) {
	executor->ScheduleTask(make_uniq<FileOpenTask>(*executor, std::move(open_fn), std::move(cancel_fn)));
}

bool ScanReadAhead::CanScheduleOpen(idx_t files_ahead) const {
	return files_ahead < open_window && ReadAheadOpenFile::Count() < open_file_budget;
}

bool ScanReadAhead::TryRunPendingTask() {
	shared_ptr<Task> task;
	if (!executor->GetTask(task)) {
		return false;
	}
	task->Execute(TaskExecutionMode::PROCESS_ALL);
	return true;
}

void ScanReadAhead::ThrowIfError() {
	if (executor->HasError()) {
		executor->ThrowError();
	}
}

void ScanReadAhead::ReleaseSlot() {
	D_ASSERT(active_jobs.load() > 0);
	active_jobs--;
}

void ScanReadAhead::WakeWaiters(bool wake_all) {
	if (wake_all) {
		progress.notify_all();
	} else {
		progress.notify_one();
	}
}

void ScanReadAhead::NotifyProgress(bool wake_all) {
	++progress_epoch;
	if (waiting_threads.load() > 0) {
		WakeWaiters(wake_all);
	}
}

void ScanReadAhead::SignalProgress(bool wake_all) {
	++progress_epoch;
	if (waiting_threads.load() == 0) {
		return;
	}
	{
		// a waiter that saw the old epoch is blocked in the wait once the lock is free
		lock_guard<mutex> guard(lock);
	}
	WakeWaiters(wake_all);
}

void ScanReadAhead::WaitForProgress(ClientContext &context, idx_t epoch) {
	context.BlockingInterruptCheck();
	{
		unique_lock<mutex> guard(lock);
		++waiting_threads;
		// the timeout bounds the interrupt latency
		progress.wait_for(guard, std::chrono::milliseconds(10), [&]() { return progress_epoch.load() != epoch; });
		--waiting_threads;
	}
	context.BlockingInterruptCheck();
}

idx_t ScanReadAhead::ProgressEpoch() const {
	return progress_epoch.load();
}

} // namespace duckdb
