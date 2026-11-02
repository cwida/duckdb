//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parallel/task_scheduler_pool.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/task_scheduler_type.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/atomic.hpp"

namespace duckdb {

class TaskScheduler;
class DatabaseInstance;
struct LightWeightSemaphoreWrapper;
struct QueueProducerToken;
struct TaskSchedulerThread;

class TaskSchedulerPool {
public:
	explicit TaskSchedulerPool(DatabaseInstance &db, TaskSchedulerType pool_type);
	~TaskSchedulerPool();

public:
	void SetThreads(idx_t n);
	idx_t NumberOfThreads();
	//! The number of background worker threads, without external threads
	idx_t NumberOfWorkerThreads() {
		return worker_thread_count.load();
	}
	void RelaunchThreads(TaskScheduler &scheduler, bool destroy);
	void Signal(idx_t n);
#ifndef DUCKDB_NO_THREADS
	void Wait();
	bool Wait(int64_t timeout_usecs);
#endif

private:
	DatabaseInstance &db;
	//! The type of this pool
	const TaskSchedulerType pool_type;
	//! The active background threads of the task scheduler
	vector<unique_ptr<TaskSchedulerThread>> threads;
	//! Markers used by the various threads, if the markers are set to "false" the thread execution is stopped
	vector<unique_ptr<atomic<bool>>> markers;
	//! Requested thread count (set by the 'threads' setting)
	atomic<idx_t> requested_thread_count;
	//! The amount of threads currently running
	atomic<idx_t> current_thread_count;
	//! The amount of background worker threads currently running
	atomic<idx_t> worker_thread_count {0};
#ifndef DUCKDB_NO_THREADS
	//! Semaphore to signal threads in this pool to wake up and execute a task
	unique_ptr<LightWeightSemaphoreWrapper> semaphore;
#endif
};

}; // namespace duckdb
