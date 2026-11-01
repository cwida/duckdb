#include "duckdb/parallel/task_scheduler.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parallel/task_scheduler_pool.hpp"
#include "duckdb/parallel/task_scheduler_queue.hpp"
#include "duckdb/storage/block_allocator.hpp"
#ifndef DUCKDB_NO_THREADS
#include "concurrentqueue.h"
#include "duckdb/common/thread.hpp"

#include <thread>
#endif

#if defined(_WIN32)
#include <windows.h>
#elif defined(__GNUC__)
#include <sched.h>
#include <unistd.h>
#if defined(__GLIBC__)
#include <pthread.h>
#endif
#endif

namespace duckdb {

TaskScheduler::TaskScheduler(DatabaseInstance &db) : db(db) {
	for (uint8_t i = 0; i < TASK_SCHEDULER_TYPE_COUNT; i++) {
		pools[i] = make_uniq<TaskSchedulerPool>(db, static_cast<TaskSchedulerType>(i));
		queues[i] = make_uniq<TaskSchedulerQueue>(static_cast<TaskSchedulerType>(i));
	}
	BlockAllocator::Get(db).SetScheduler(*this);
}

TaskScheduler::~TaskScheduler() {
#ifndef DUCKDB_NO_THREADS
	try {
		for (auto &pool : pools) {
			pool->RelaunchThreads(*this, true);
		}
		BlockAllocator::Get(db).FlushOnShutdown();
	} catch (...) {
		// nothing we can do in the destructor if this fails
	}
#endif
	BlockAllocator::Get(db).ClearScheduler();
}

TaskScheduler &TaskScheduler::GetScheduler(ClientContext &context) {
	return GetScheduler(DatabaseInstance::GetDatabase(context));
}

TaskScheduler &TaskScheduler::GetScheduler(DatabaseInstance &db) {
	return db.GetScheduler();
}

unique_ptr<ProducerToken> TaskScheduler::CreateProducer() {
	return make_uniq<ProducerToken>(queues);
}

void TaskScheduler::ScheduleTask(ProducerToken &token, shared_ptr<Task> task) {
	ScheduleTask(token, std::move(task), TaskSchedulerType::REGULAR);
}

void TaskScheduler::ScheduleTasks(ProducerToken &producer, vector<shared_ptr<Task>> &tasks) {
	ScheduleTasks(producer, tasks, TaskSchedulerType::REGULAR);
}

void TaskScheduler::ScheduleTasks(ProducerToken &producer, vector<shared_ptr<Task>> &tasks, idx_t threads_to_wake) {
	GetQueue(TaskSchedulerType::REGULAR).EnqueueBulk(producer, tasks);
	SignalForTaskType(TaskSchedulerType::REGULAR, threads_to_wake);
}

bool TaskScheduler::GetTaskFromProducer(ProducerToken &token, shared_ptr<Task> &task) {
	const annotated_lock_guard<annotated_mutex> lock(token.producer_lock);
	return GetTaskFromProducerLocked(token, task);
}

bool TaskScheduler::GetTaskFromProducerLocked(ProducerToken &token, shared_ptr<Task> &task) {
	for (auto &queue : queues) {
		if (queue->DequeueFromProducerLocked(token, task)) {
			return true;
		}
	}
	return false;
}

TaskSchedulerPool &TaskScheduler::GetPool(TaskSchedulerType pool_type) {
	return *pools[static_cast<uint8_t>(pool_type)];
}

TaskSchedulerQueue &TaskScheduler::GetQueue(TaskSchedulerType pool_type) const {
	return *queues[static_cast<uint8_t>(pool_type)];
}

void TaskScheduler::SetThreadsInternal(TaskSchedulerType pool_type, idx_t n) {
	GetPool(pool_type).SetThreads(n);
}

void TaskScheduler::ScheduleTask(ProducerToken &token, shared_ptr<Task> task, TaskSchedulerType pool_type) {
	GetQueue(pool_type).Enqueue(token, std::move(task));
	SignalForTaskType(pool_type, 1);
}

void TaskScheduler::ScheduleTasks(ProducerToken &producer, vector<shared_ptr<Task>> &tasks,
                                  TaskSchedulerType pool_type) {
	GetQueue(pool_type).EnqueueBulk(producer, tasks);
	SignalForTaskType(pool_type, tasks.size());
}

bool TaskScheduler::GetTaskInternal(ConsumerToken &consumer, shared_ptr<Task> &task) {
	for (auto &queue : queues) {
		if (queue->Dequeue(consumer, task)) {
			return true;
		}
	}
	return false;
}

//! The task a worker thread took from a queue. It stays here until the worker is done with it, so that an exception
//! in the bookkeeping of the worker neither loses the task nor runs it twice
struct TaskScheduler::WorkerTask {
	shared_ptr<Task> task;
	//! The queue the task came from
	optional_ptr<TaskSchedulerQueue> queue;
	//! Whether the worker tried to read the execution mode, the task runs in the default mode when that threw
	bool read_mode = false;
	//! Whether the task is running, and whether it ran with the given result
	bool executing = false;
	bool executed = false;
	TaskExecutionResult result = TaskExecutionResult::TASK_FINISHED;
	//! How often the worker retried the task after an exception
	idx_t retries = 0;
};

bool TaskScheduler::TryDequeueTask(TaskSchedulerQueue &queue, ConsumerToken &consumer, WorkerTask &worker_task) {
	if (queue.Dequeue(consumer, worker_task.task)) {
		worker_task.queue = queue;
		return true;
	}

	if (queue.GetTasksInQueue() > 0) {
		// failed to dequeue but there are still tasks remaining - signal again to retry
		SignalForTaskType(queue.GetPoolType(), 1);
	}
	return false;
}

//! Runs the task unless it ran already, then reschedules or releases it. After an exception outside of the task
//! itself, the worker calls this again for the same task, and it continues where it stopped
void TaskScheduler::ProcessTask(const DBConfig &config, WorkerTask &worker_task) {
	auto &task = worker_task.task;
	if (!worker_task.executed) {
		auto process_mode = TaskExecutionMode::PROCESS_ALL;
		if (!worker_task.read_mode) {
			worker_task.read_mode = true;
			if (Settings::Get<SchedulerProcessPartialSetting>(config)) {
				process_mode = TaskExecutionMode::PROCESS_PARTIAL;
			}
		}
		worker_task.executing = true;
		worker_task.result = task->Execute(process_mode);
		worker_task.executing = false;
		worker_task.executed = true;
	}

	switch (worker_task.result) {
	case TaskExecutionResult::TASK_FINISHED:
	case TaskExecutionResult::TASK_ERROR:
		break;
	case TaskExecutionResult::TASK_NOT_FINISHED:
		// task is not finished - reschedule immediately
		// neither the queue nor the signal throws once the task is enqueued, so a retry never enqueues it twice
		ScheduleTask(*task->token, task, worker_task.queue->GetPoolType());
		break;
	case TaskExecutionResult::TASK_BLOCKED:
		task->Deschedule();
		break;
	}
	worker_task = WorkerTask();
}

#ifndef DUCKDB_NO_THREADS
static bool SetThreadAffinity(idx_t cpu_id) {
#if defined(__GLIBC__)
	if (cpu_id >= CPU_SETSIZE) {
		return false;
	}
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(cpu_id, &cpuset);
	return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0;
#else
	return false;
#endif
}
#endif

void TaskScheduler::ExecuteForever(atomic<bool> *marker, const TaskSchedulerType pool_type, optional_idx cpu) {
#ifndef DUCKDB_NO_THREADS
	if (cpu.IsValid() && SetThreadAffinity(cpu.GetIndex())) {
		GetPinnedCPUId() = cpu;
	}
	static constexpr int64_t INITIAL_FLUSH_WAIT = 500000; // initial wait time of 0.5s (in mus) before flushing
	static constexpr int64_t RETRY_WAIT = 10000;    // wait time of 10ms (in mus) before retrying after an exception
	static constexpr idx_t MAX_TASK_RETRIES = 1000; // retries of the held task, about 10s, before rethrowing

	const auto &block_allocator = BlockAllocator::Get(db);
	const auto &config = DBConfig::GetConfig(db);
	auto &pool = GetPool(pool_type);

	// Only a thread that ran a task since its last flush has allocations to flush, both blocks and the jemalloc cache
	// of the allocator, so other threads skip the flush and leave the trim of the system heap to the threads that ran
	// tasks. Reading the settings would be the first use of their thread_local cache in a thread that never ran one,
	// and glibc ends the process when it cannot allocate the registration of a thread_local destructor
	bool flush_pending = false;
	bool ran_task = false;

	unique_ptr<ConsumerToken> consumer;
	WorkerTask worker_task;

	// An exception that leaves this thread ends the process. When the bookkeeping of the thread throws, for example
	// because it cannot allocate, the thread waits and continues where it was, with the task it holds, up to
	// MAX_TASK_RETRIES times
	while (true) {
		try {
			if (!consumer) {
				consumer = make_uniq<ConsumerToken>(queues);
			}
			// loop until the marker is set to false, and the task this thread holds is done
			while (*marker || worker_task.task) {
				if (worker_task.task) {
					// an exception interrupted the task this thread holds, finish it before taking another one
					ProcessTask(config, worker_task);
					continue;
				}
				if (!block_allocator.SupportsFlush()) {
					// allocator can't flush, just start an untimed wait
					pool.Wait();
				} else if (!pool.Wait(INITIAL_FLUSH_WAIT)) {
					// allocator can flush, we flush this threads outstanding allocations after it was idle for 0.5s
					if (flush_pending) {
						block_allocator.ThreadFlush(
						    Settings::Get<AllocatorBackgroundThreadsSetting>(db),
						    StringUtil::ParseFormattedBytes(Settings::Get<AllocatorFlushThresholdSetting>(db)),
						    GetPool(TaskSchedulerType::REGULAR).NumberOfThreads());
						flush_pending = false;
					}
					auto decay_delay = block_allocator.DecayDelay();
					if (!decay_delay.IsValid()) {
						// no decay delay specified - just wait
						pool.Wait();
					} else {
						if (!pool.Wait(UnsafeNumericCast<int64_t>(decay_delay.GetIndex()) * 1000000 -
						               INITIAL_FLUSH_WAIT)) {
							// in total, the thread was idle for the entire decay delay (note: seconds converted to mus)
							// Another thread may have returned allocations during this worker's idle interval.
							// Keep the wait timed until those cached allocations have had a chance to decay.
							block_allocator.ThreadIdle();
							if (!block_allocator.HasPendingCacheDecay()) {
								pool.Wait();
							}
						}
					}
				}

				if (pool_type == TaskSchedulerType::REGULAR) {
					// Regular thread pool picks up tasks from all pools
					for (auto &queue : queues) {
						if (TryDequeueTask(*queue, *consumer, worker_task)) {
							break;
						}
					}
				} else {
					TryDequeueTask(GetQueue(pool_type), *consumer, worker_task);
				}
				if (worker_task.task) {
					flush_pending = ran_task = true;
					ProcessTask(config, worker_task);
				}
			}
			// this thread will exit, flush all of its outstanding allocations
			if (ran_task && block_allocator.SupportsFlush()) {
				block_allocator.ThreadFlush(Settings::Get<AllocatorBackgroundThreadsSetting>(db), 0,
				                            GetPool(TaskSchedulerType::REGULAR).NumberOfThreads());
			}
			break;
		} catch (...) {
			if (worker_task.executing) {
				// the task could not report its own error, so it can neither be dropped nor run again
				throw;
			}
			if (worker_task.task && ++worker_task.retries > MAX_TASK_RETRIES) {
				// a step that keeps failing would hold this thread and whoever waits for the task forever
				throw;
			}
			if (!*marker && !worker_task.task) {
				// the flush at exit only gives memory back, so the thread exits without it
				break;
			}
		}
		std::this_thread::sleep_for(std::chrono::microseconds(RETRY_WAIT));
	}
	if (block_allocator.SupportsFlush()) {
		try {
			block_allocator.ThreadExit();
		} catch (...) {
			// returning the memory of an exiting thread is best effort
		}
	}
#else
	throw NotImplementedException("DuckDB was compiled without threads! Background thread loop is not allowed.");
#endif
}

idx_t TaskScheduler::ExecuteTasks(atomic<bool> *marker, idx_t max_tasks) {
#ifndef DUCKDB_NO_THREADS
	ConsumerToken consumer(queues);
	idx_t completed_tasks = 0;
	// loop until the marker is set to false
	while (*marker && completed_tasks < max_tasks) {
		shared_ptr<Task> task;
		if (!GetTaskInternal(consumer, task)) {
			return completed_tasks;
		}
		auto execute_result = task->Execute(TaskExecutionMode::PROCESS_ALL);

		switch (execute_result) {
		case TaskExecutionResult::TASK_FINISHED:
		case TaskExecutionResult::TASK_ERROR:
			task.reset();
			completed_tasks++;
			break;
		case TaskExecutionResult::TASK_NOT_FINISHED:
			throw InternalException("Task should not return TASK_NOT_FINISHED in PROCESS_ALL mode");
		case TaskExecutionResult::TASK_BLOCKED:
			task->Deschedule();
			task.reset();
			break;
		}
	}
	return completed_tasks;
#else
	throw NotImplementedException("DuckDB was compiled without threads! Background thread loop is not allowed.");
#endif
}

void TaskScheduler::ExecuteTasks(idx_t max_tasks) {
#ifndef DUCKDB_NO_THREADS
	ConsumerToken consumer(queues);
	shared_ptr<Task> task;
	for (idx_t i = 0; i < max_tasks; i++) {
		GetPool(TaskSchedulerType::REGULAR).Wait(TASK_TIMEOUT_USECS);
		if (!GetTaskInternal(consumer, task)) {
			return;
		}
		try {
			auto execute_result = task->Execute(TaskExecutionMode::PROCESS_ALL);
			switch (execute_result) {
			case TaskExecutionResult::TASK_FINISHED:
			case TaskExecutionResult::TASK_ERROR:
				task.reset();
				break;
			case TaskExecutionResult::TASK_NOT_FINISHED:
				throw InternalException("Task should not return TASK_NOT_FINISHED in PROCESS_ALL mode");
			case TaskExecutionResult::TASK_BLOCKED:
				task->Deschedule();
				task.reset();
				break;
			}
		} catch (...) {
			return;
		}
	}
#else
	throw NotImplementedException("DuckDB was compiled without threads! Background thread loop is not allowed.");
#endif
}

idx_t TaskScheduler::NumberOfThreads() {
	return GetPool(TaskSchedulerType::REGULAR).NumberOfThreads();
}

idx_t TaskScheduler::NumberOfAsyncThreads() {
	return GetPool(TaskSchedulerType::ASYNC).NumberOfThreads();
}

idx_t TaskScheduler::GetNumberOfTasks() const {
	idx_t num_tasks = 0;
	for (auto &queue : queues) {
		num_tasks += queue->GetTasksInQueue();
	}
	return num_tasks;
}

idx_t TaskScheduler::GetProducerCount() const {
	// We always create a producer in all queues, so we can just get the producer count of the regular queue here
	return GetQueue(TaskSchedulerType::REGULAR).GetProducerCount();
}

idx_t TaskScheduler::GetTaskCountForProducer(ProducerToken &token) const {
	idx_t task_count = 0;
	for (uint8_t i = 0; i < TASK_SCHEDULER_TYPE_COUNT; i++) {
		task_count += GetQueue(static_cast<TaskSchedulerType>(i)).GetTaskCountForProducer(token);
	}
	return task_count;
}

void TaskScheduler::SetThreads(idx_t total_threads, idx_t external_threads) {
	if (total_threads == 0) {
		throw SyntaxException("Number of threads must be positive!");
	}
#ifndef DUCKDB_NO_THREADS
	if (total_threads < external_threads) {
		throw SyntaxException("Number of threads can't be smaller than number of external threads!");
	}
#else
	if (total_threads != external_threads) {
		throw NotImplementedException(
		    "DuckDB was compiled without threads! Setting total_threads != external_threads is not allowed.");
	}
#endif
	SetThreadsInternal(TaskSchedulerType::REGULAR, total_threads - external_threads);
}

void TaskScheduler::SetAsyncThreads(idx_t n) {
#ifdef DUCKDB_NO_THREADS
	if (n != 0) {
		throw NotImplementedException(
		    "DuckDB was compiled without threads! Setting async threads != 0 is not allowed.");
	}
#endif
	SetThreadsInternal(TaskSchedulerType::ASYNC, n);
}

void TaskScheduler::Signal(idx_t n) {
	Signal(TaskSchedulerType::REGULAR, n);
}

void TaskScheduler::Signal(TaskSchedulerType pool_type, idx_t n) {
	GetPool(pool_type).Signal(n);
}

void TaskScheduler::SignalAllPools(idx_t n) {
	for (auto &pool : pools) {
		pool->Signal(n);
	}
}

void TaskScheduler::SignalForTaskType(TaskSchedulerType task_type, idx_t n) {
	if (task_type == TaskSchedulerType::REGULAR) {
		Signal(TaskSchedulerType::REGULAR, n);
	} else {
		SignalAllPools(n);
	}
}

void TaskScheduler::YieldThread() {
#ifndef DUCKDB_NO_THREADS
	std::this_thread::yield();
#endif
}

optional_idx &TaskScheduler::GetPinnedCPUId() {
#ifndef DUCKDB_NO_THREADS
	static thread_local optional_idx cpu_id;
#else
	static optional_idx cpu_id;
#endif
	return cpu_id;
}

idx_t TaskScheduler::GetEstimatedCPUId() {
	const auto &pinned_cpu_id = GetPinnedCPUId();
	if (pinned_cpu_id.IsValid()) {
		return pinned_cpu_id.GetIndex();
	}
#if defined(__EMSCRIPTEN__)
	// FIXME: Wasm + multithreads can likely be implemented as
	//   return return (idx_t)std::hash<std::thread::id>()(std::this_thread::get_id());
	return 0;
#else
	// this code comes from jemalloc
#if defined(_WIN32)
	return (idx_t)GetCurrentProcessorNumber();
#elif defined(_GNU_SOURCE)
	auto cpu = sched_getcpu();
	if (cpu < 0) {
#ifndef DUCKDB_NO_THREADS
		// fallback to thread id
		return (idx_t)std::hash<std::thread::id>()(std::this_thread::get_id());
#else

		return 0;
#endif
	}
	return (idx_t)cpu;
#elif defined(__aarch64__) && defined(__APPLE__)
	/* Other oses most likely use tpidr_el0 instead */
	uintptr_t c;
	asm volatile("mrs %x0, tpidrro_el0" : "=r"(c)::"memory");
	return (idx_t)(c & ((1 << 3) - 1));
#else
#ifndef DUCKDB_NO_THREADS
	// fallback to thread id
	return (idx_t)std::hash<std::thread::id>()(std::this_thread::get_id());
#else
	return 0;
#endif
#endif
#endif
}

void TaskScheduler::RelaunchThreads() {
	lock_guard<mutex> t(thread_lock);
	for (auto &pool : pools) {
		pool->RelaunchThreads(*this, false);
	}
}

} // namespace duckdb
