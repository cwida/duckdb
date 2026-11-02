#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/parallel/pipeline_hook.hpp"
#include "duckdb/parallel/pipeline_tracer.hpp"

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

TaskScheduler::TaskScheduler(DatabaseInstance &db)
    : db(db), tracer(make_uniq<PipelineTracer>()), hook(make_uniq<PipelineHook>(*this, *tracer)) {
	tracer->SetHook(*hook);
	for (uint8_t i = 0; i < TASK_SCHEDULER_TYPE_COUNT; i++) {
		pools[i] = make_uniq<TaskSchedulerPool>(db, static_cast<TaskSchedulerType>(i));
		queues[i] = make_uniq<TaskSchedulerQueue>(static_cast<TaskSchedulerType>(i));
	}
}

TaskScheduler::~TaskScheduler() {
#ifndef DUCKDB_NO_THREADS
	try {
		for (auto &pool : pools) {
			pool->RelaunchThreads(*this, true);
		}
		BlockAllocator::Get(db).FlushAll();
	} catch (...) {
		// nothing we can do in the destructor if this fails
	}
#endif
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
	if (pool_type == TaskSchedulerType::REGULAR) {
		hook->NoteScheduled(*task);
		if (hook->TryEnqueue(task)) {
			return;
		}
	}
	GetQueue(pool_type).Enqueue(token, std::move(task));
	SignalForTaskType(pool_type, 1);
}

void TaskScheduler::ScheduleTasks(ProducerToken &producer, vector<shared_ptr<Task>> &tasks,
                                  TaskSchedulerType pool_type) {
	if (pool_type == TaskSchedulerType::REGULAR) {
		if (!tasks.empty()) {
			hook->NoteScheduled(*tasks[0]);
		}
		// tasks of traced pipelines go to the thread-assignment hook's per-pipeline queues
		hook->TryEnqueue(tasks);
		if (tasks.empty()) {
			return;
		}
	}
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

void TaskScheduler::ExecuteForever(atomic<bool> *marker) {
	ExecuteForever(marker, TaskSchedulerType::REGULAR);
}

bool TaskScheduler::TryDequeueAndProcessTask(const DBConfig &config, TaskSchedulerQueue &queue, ConsumerToken &consumer,
                                             shared_ptr<Task> &task) {
	if (queue.Dequeue(consumer, task)) {
		auto process_mode = TaskExecutionMode::PROCESS_ALL;
		if (Settings::Get<SchedulerProcessPartialSetting>(config)) {
			process_mode = TaskExecutionMode::PROCESS_PARTIAL;
		}
		auto execute_result = task->Execute(process_mode);

		switch (execute_result) {
		case TaskExecutionResult::TASK_FINISHED:
		case TaskExecutionResult::TASK_ERROR:
			task.reset();
			break;
		case TaskExecutionResult::TASK_NOT_FINISHED: {
			// task is not finished - reschedule immediately
			auto &token = *task->token;
			queue.Enqueue(token, std::move(task));
			SignalForTaskType(queue.GetPoolType(), 1);
			break;
		}
		case TaskExecutionResult::TASK_BLOCKED:
			task->Deschedule();
			task.reset();
			break;
		}
		return true;
	}

	if (queue.GetTasksInQueue() > 0) {
		// failed to dequeue but there are still tasks remaining - signal again to retry
		SignalForTaskType(queue.GetPoolType(), 1);
	}
	return false;
}

void TaskScheduler::ProcessHookTask(const DBConfig &config, shared_ptr<Task> &task, idx_t worker_idx) {
	auto process_mode = TaskExecutionMode::PROCESS_ALL;
	if (Settings::Get<SchedulerProcessPartialSetting>(config)) {
		process_mode = TaskExecutionMode::PROCESS_PARTIAL;
	}
	auto execute_result = task->Execute(process_mode);
	hook->TaskDone(worker_idx);
	switch (execute_result) {
	case TaskExecutionResult::TASK_FINISHED:
	case TaskExecutionResult::TASK_ERROR:
		task.reset();
		break;
	case TaskExecutionResult::TASK_NOT_FINISHED: {
		// back to its pipeline's queue, through the hook
		auto &token = *task->token;
		ScheduleTask(token, std::move(task));
		break;
	}
	case TaskExecutionResult::TASK_BLOCKED:
		task->Deschedule();
		task.reset();
		break;
	}
}

void TaskScheduler::ExecuteForever(atomic<bool> *marker, const TaskSchedulerType pool_type) {
#ifndef DUCKDB_NO_THREADS
	static constexpr int64_t INITIAL_FLUSH_WAIT = 500000; // initial wait time of 0.5s (in mus) before flushing

	const auto &block_allocator = BlockAllocator::Get(db);
	const auto &config = DBConfig::GetConfig(db);
	auto &pool = GetPool(pool_type);
	ConsumerToken consumer(queues);

	shared_ptr<Task> task;
	// loop until the marker is set to false
	while (*marker) {
		auto worker = PipelineTracer::GetCurrentWorker();
		if (pool_type == TaskSchedulerType::REGULAR && hook->IsActive() && worker.IsValid() &&
		    worker.GetIndex() < PipelineTracer::MAX_WORKERS) {
			// the thread-assignment hook decides which pipeline this worker runs tasks of
			static constexpr int64_t HOOK_WAIT_USECS = 1000;
			auto worker_idx = worker.GetIndex();
			if (hook->TryGetTask(worker_idx, task)) {
				ProcessHookTask(config, task, worker_idx);
				continue;
			}
			// tasks the hook does not handle (pipelines' initialize tasks, untraced statements) run on any worker
			// with nothing pending, without changing what it is attached to
			bool ran_task = false;
			for (auto &queue : queues) {
				if (TryDequeueAndProcessTask(config, *queue, consumer, task)) {
					ran_task = true;
					break;
				}
			}
			if (ran_task) {
				hook->CountUntracedTask();
				continue;
			}
			hook->WaitForWork(worker_idx, HOOK_WAIT_USECS);
			continue;
		}
		if (!block_allocator.SupportsFlush()) {
			// allocator can't flush, just start an untimed wait
			pool.Wait();
		} else if (!pool.Wait(INITIAL_FLUSH_WAIT)) {
			// allocator can flush, we flush this threads outstanding allocations after it was idle for 0.5s
			block_allocator.ThreadFlush(
			    Settings::Get<AllocatorBackgroundThreadsSetting>(db),
			    StringUtil::ParseFormattedBytes(Settings::Get<AllocatorFlushThresholdSetting>(db)),
			    GetPool(TaskSchedulerType::REGULAR).NumberOfThreads());
			auto decay_delay = Allocator::DecayDelay();
			if (!decay_delay.IsValid()) {
				// no decay delay specified - just wait
				pool.Wait();
			} else {
				if (!pool.Wait(UnsafeNumericCast<int64_t>(decay_delay.GetIndex()) * 1000000 - INITIAL_FLUSH_WAIT)) {
					// in total, the thread was idle for the entire decay delay (note: seconds converted to mus)
					// mark it as idle and start an untimed wait
					Allocator::ThreadIdle();
					pool.Wait();
				}
			}
		}

		if (pool_type == TaskSchedulerType::REGULAR) {
			// Regular thread pool picks up tasks from all pools
			for (auto &queue : queues) {
				if (TryDequeueAndProcessTask(config, *queue, consumer, task)) {
					break;
				}
			}
		} else {
			TryDequeueAndProcessTask(config, GetQueue(pool_type), consumer, task);
		}
	}
	// this thread will exit, flush all of its outstanding allocations
	if (block_allocator.SupportsFlush()) {
		block_allocator.ThreadFlush(Settings::Get<AllocatorBackgroundThreadsSetting>(db), 0,
		                            GetPool(TaskSchedulerType::REGULAR).NumberOfThreads());
		Allocator::ThreadIdle();
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

idx_t TaskScheduler::NumberOfWorkerThreads() {
	return GetPool(TaskSchedulerType::REGULAR).NumberOfWorkerThreads();
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

idx_t TaskScheduler::GetEstimatedCPUId() {
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
