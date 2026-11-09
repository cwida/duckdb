//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parallel/scan_read_ahead.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/map.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/condition_variable.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/common/deque.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/serializer/async_memory_governor.hpp"
#include "duckdb/parallel/async_result.hpp"
#include "duckdb/parallel/interrupt.hpp"
#include "duckdb/parallel/task_executor.hpp"

#include <functional>

namespace duckdb {
class ClientContext;
struct TableFunctionInput;

class ReadAheadJobCompletion {
public:
	explicit ReadAheadJobCompletion(shared_ptr<TaskExecutor> executor_p)
	    : executor(std::move(executor_p)), pending_io_tasks(0) {
	}

public:
	//! Arm the completion for one I/O task, only called before the job becomes visible to other threads
	void AddIOTask();
	//! Mark one I/O task as completed, waking the scan task when it was the last one
	void FinishIOTask();
	//! Try to park the calling scan task until the job's I/O completes, the last I/O task to finish wakes it.
	bool TryPark(const InterruptState &interrupt_state);
	//! Try to park the scan task through the table function's interrupt state, setting BLOCKED on success
	bool TryPark(TableFunctionInput &data_p);
	//! Block until the job's I/O completed, running scheduled I/O inline so progress does not depend on pool threads
	void WaitForIO();

private:
	//! Executor holding the scheduled I/O, waiters drain it inline
	shared_ptr<TaskExecutor> executor;
	atomic<idx_t> pending_io_tasks;
	//! Holds the scan task parked on this job's I/O
	StateWithBlockableTasks parked_scan;
};

//! Base of the job types driven by ScanReadAhead
struct ScanReadAheadJob {
	virtual ~ScanReadAheadJob();

	//! Settle in-flight I/O
	void SettleIO() {
		if (io_completion) {
			io_completion->WaitForIO();
			io_completion.reset();
		}
	}

	//! Batch index of this job, drives ordered queue admission.
	//! Producers must assign batch indexes densely from 0 in claim order
	idx_t batch_index = 0;
	//! Completion of the job's scheduled I/O
	shared_ptr<ReadAheadJobCompletion> io_completion;
	//! Total bytes of scheduled I/O for this job
	idx_t io_bytes = 0;

private:
	ScanReadAheadJob() = default;
	ScanReadAheadJob(const ScanReadAheadJob &) = delete;
	ScanReadAheadJob &operator=(const ScanReadAheadJob &) = delete;
	template <class JOB_STATE>
	friend struct ScanReadAheadJobWrapper;
};

//! Wraps job-specific state so in-flight I/O is settled before that state is destroyed
template <class JOB_STATE>
struct ScanReadAheadJobWrapper final : public ScanReadAheadJob, public JOB_STATE {
	~ScanReadAheadJobWrapper() override {
		SettleIO();
	}
};

//! Pool of scan states handed back by finished jobs, recycled so learned scan state carries over to later jobs
template <class STATE>
class ScanStatePool {
public:
	void Push(unique_ptr<STATE> state) {
		annotated_lock_guard<annotated_mutex> guard(lock);
		states.push_back(std::move(state));
	}
	//! Pop a recycled state, returns null when none is available
	unique_ptr<STATE> TryPop() {
		annotated_lock_guard<annotated_mutex> guard(lock);
		if (states.empty()) {
			return nullptr;
		}
		auto state = std::move(states.back());
		states.pop_back();
		return state;
	}
	//! Take every pooled state
	vector<unique_ptr<STATE>> TakeAll() {
		annotated_lock_guard<annotated_mutex> guard(lock);
		return std::move(states);
	}

private:
	annotated_mutex lock;
	vector<unique_ptr<STATE>> states DUCKDB_GUARDED_BY(lock);
};

//! Counts one file against the open file budget of all scans until destroyed
class ReadAheadOpenFile {
public:
	ReadAheadOpenFile();
	~ReadAheadOpenFile();
	ReadAheadOpenFile(const ReadAheadOpenFile &) = delete;
	ReadAheadOpenFile &operator=(const ReadAheadOpenFile &) = delete;

	//! Files counted by all scans of the process
	static idx_t Count();
};

//! Outcome of ScanReadAhead::AcquireJob
enum class ScanReadAheadAcquire : uint8_t {
	ACQUIRED,  //! a job with settled I/O is now held
	EXHAUSTED, //! every job has been produced and claimed, the scan is done
	PARKED     //! the scan task parked until the claimed job's I/O completes, the caller must yield
};

//! Drives read-ahead for a scan, its purpose is to keep several scan jobs scheduled ahead of decoding.
//! Job state is held in a ScanReadAheadJobWrapper, callers claim jobs back with unique_ptr_cast.
class ScanReadAhead {
public:
	ScanReadAhead(ClientContext &context, idx_t read_ahead_depth_p,
	              unique_ptr<ManagedAsyncMemoryGovernor> memory_governor_p);
	~ScanReadAhead();

	//! Create the read-ahead driver from the read_ahead_depth setting, returns null when read-ahead is disabled.
	//! -1 means automatic mode, unlimited depth with the backlog bounded by a temp-memory reservation.
	static unique_ptr<ScanReadAhead> Create(ClientContext &context);

public:
	//! Claims the next job and schedules its I/O, filling io_tasks when the I/O was detached to the pool.
	//! Returns null when there are no more jobs to produce.
	//! The callback must assign the job's batch index, densely from 0 in claim order.
	using ProduceJobCallback = std::function<unique_ptr<ScanReadAheadJob>(vector<unique_ptr<AsyncTask>> &io_tasks)>;

	//! Settle the claimed job's scheduled I/O, blocking until it completed, and release its budget charge
	void WaitForJob(ScanReadAheadJob &job);
	//! Acquire the next job, producing and claiming as needed; on ACQUIRED the job's I/O has been settled.
	//! On PARKED the caller must yield holding the claimed job and settle it with WaitForJob when it resumes.
	ScanReadAheadAcquire AcquireJob(ClientContext &context, TableFunctionInput &data_p,
	                                const ProduceJobCallback &claim_and_schedule, unique_ptr<ScanReadAheadJob> &job);

	//! Push an error onto the async executor
	void PushError(ErrorData error);

	//! Schedule a file-open closure on the async pool, opening files ahead of decoding
	//! cancel_fn runs instead when the open is retired without running, and must settle whatever a scan waits on
	void ScheduleFileOpen(std::function<void()> open_fn, std::function<void()> cancel_fn);
	//! Whether another file may be opened when files_ahead files are already opening or open ahead of the scan
	//! and the files held open by all scans are within the open file budget
	bool CanScheduleOpen(idx_t files_ahead) const;
	//! Run one queued async task inline, returns false when none is queued
	bool TryRunPendingTask();
	//! Throw the first error recorded on the async executor, if there is one
	void ThrowIfError();
	//! Cancel the tasks that have not started yet and wait for the ones that are running. The scheduled closures
	//! reference the scan they were scheduled for, so they must not outlive it
	void CancelAndDrain();

private:
	//! Settles the reservation taken by TryReserveSlot
	struct ProducerReservation {
		explicit ProducerReservation(ScanReadAhead &read_ahead) : read_ahead(read_ahead) {
		}
		~ProducerReservation() {
			if (!committed) {
				read_ahead.ReleaseSlot();
				read_ahead.pending_io_bytes -= MINIMUM_JOB_IO_CHARGE;
			}
			--read_ahead.active_producers;
			// a pushed job was signalled already, a released slot frees one thread and a done scan all of them
			if (!committed || read_ahead.IsDone()) {
				read_ahead.SignalProgress(read_ahead.IsDone());
			}
		}

		ScanReadAhead &read_ahead;
		bool committed = false;
	};

	//! Try to produce one job into the queue.
	bool TryProduceJob(const ProduceJobCallback &claim_and_schedule);
	//! Check if scan is done, i.e., no more jobs to do
	bool IsDone() const;
	//! Whether any thread holds a reserved slot it has not pushed a job for yet
	bool HasActiveProducers() const;
	//! Pop the oldest queued job
	unique_ptr<ScanReadAheadJob> ClaimJob();
	//! Mark the scan as done, i.e., no more jobs to produce
	void SetDone();
	//! Whether jobs are held back from admission, used to detect gaps in the batch indexes on exhaustion
	bool HasPendingJobs() const {
		lock_guard<mutex> guard(lock);
		return !pending_jobs.empty();
	}
	//! The number of jobs that may be in flight under the current budget
	idx_t ReservationDepth() const;
	//! Reserve an in-flight job slot for producing a job
	bool TryReserveSlot();
	//! Schedule the job's I/O and admit the job to the queue
	void PushJob(unique_ptr<ScanReadAheadJob> job, vector<unique_ptr<AsyncTask>> io_tasks);
	//! Release a read-ahead slot
	void ReleaseSlot();
	//! Wake a thread waiting for another thread to push a job, release a slot or finish producing, or all of them
	void SignalProgress(bool wake_all);
	//! SignalProgress for a caller that holds the lock
	void NotifyProgress(bool wake_all);
	//! Wake one waiting thread, or all of them
	void WakeWaiters(bool wake_all);
	//! Wait until the read-ahead made progress since the given epoch, or the timeout passed
	void WaitForProgress(ClientContext &context, idx_t epoch);
	idx_t ProgressEpoch() const;

private:
	//! Minimum budget charge per job, beyond its scheduled I/O a job carries scan-state overhead (decode buffers)
	static constexpr idx_t MINIMUM_JOB_IO_CHARGE = 16ULL * 1024 * 1024;
	//! How long a thread yields for another thread to push a job before it waits for it
	static constexpr idx_t PRODUCER_YIELD_MICROS = 1000;

private:
	ClientContext &context;
	//! Maximum number of jobs scheduled ahead of decoding, unlimited in the -1 auto mode
	const idx_t read_ahead_depth;
	//! Async memory governor
	unique_ptr<ManagedAsyncMemoryGovernor> memory_governor;
	//! Maximum files opening or open ahead of decoding
	const idx_t open_window;
	//! Maximum files held open by all scans, a share of the file descriptor limit
	const idx_t open_file_budget;
	//! Backlog budget granted by the reservation, refreshed whenever a job is pushed
	atomic<idx_t> backlog_budget {0};

	mutable mutex lock;
	deque<unique_ptr<ScanReadAheadJob>> ready_queue;
	//! Jobs pushed out of order, held back until all earlier batch indexes are admitted to the queue
	map<idx_t, unique_ptr<ScanReadAheadJob>> pending_jobs;
	//! The batch index the queue admits next
	idx_t next_batch_index = 0;
	//! Jobs scheduled ahead of decoding
	atomic<idx_t> active_jobs {0};
	//! Budget charge of the reserved producer slots and scheduled jobs, released per job in WaitForJob
	atomic<idx_t> pending_io_bytes {0};
	atomic<bool> done {false};
	//! Threads that reserved a slot but have not pushed their job yet
	atomic<idx_t> active_producers {0};
	//! Incremented whenever a waiting thread could make progress
	atomic<idx_t> progress_epoch {0};
	//! Threads waiting for progress, only these need a notification
	atomic<idx_t> waiting_threads {0};
	condition_variable progress;
	//! Async I/O executor (async pool)
	shared_ptr<TaskExecutor> executor;
};

} // namespace duckdb
