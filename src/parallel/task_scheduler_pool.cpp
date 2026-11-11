#include "duckdb/parallel/task_scheduler_pool.hpp"

#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/thread.hpp"
#include "duckdb/storage/block_allocator.hpp"

#ifndef DUCKDB_NO_THREADS
#include "lightweightsemaphore.h"

#include <type_traits>
#endif

#if defined(_WIN32)
#include <windows.h>
#elif defined(__GNUC__)
#include <sched.h>
#include <unistd.h>
#endif

namespace duckdb {

#ifndef DUCKDB_NO_THREADS
typedef duckdb_moodycamel::LightweightSemaphore lightweight_semaphore_t;
// Threshold for AUTO thread pinning.
static constexpr idx_t THREAD_PIN_THRESHOLD = 64;

struct LightWeightSemaphoreWrapper {
	lightweight_semaphore_t s;
};
#endif

struct TaskSchedulerThread {
#ifndef DUCKDB_NO_THREADS
	explicit TaskSchedulerThread(unique_ptr<thread> thread_p) : internal_thread(std::move(thread_p)) {
	}

	unique_ptr<thread> internal_thread;
#endif
};

TaskSchedulerPool::TaskSchedulerPool(DatabaseInstance &db_p, TaskSchedulerType pool_type_p)
    : db(db_p), pool_type(pool_type_p), requested_thread_count(0),
      current_thread_count(pool_type == TaskSchedulerType::REGULAR ? 1 : 0) {
#ifndef DUCKDB_NO_THREADS
	semaphore = make_uniq<LightWeightSemaphoreWrapper>();
#endif
}

TaskSchedulerPool::~TaskSchedulerPool() {
}

void TaskSchedulerPool::SetThreads(idx_t n) {
	requested_thread_count = n;
}

idx_t TaskSchedulerPool::NumberOfThreads() {
	return current_thread_count.load();
}

void TaskSchedulerPool::Signal(idx_t n) {
#ifndef DUCKDB_NO_THREADS
	if (n == 0) {
		return;
	}
	typedef std::make_signed<std::size_t>::type ssize_t;
	semaphore->s.signal(NumericCast<ssize_t>(n));
#endif
}

#ifndef DUCKDB_NO_THREADS
void TaskSchedulerPool::Wait() {
	semaphore->s.wait();
}

bool TaskSchedulerPool::Wait(int64_t timeout_usecs) {
	return semaphore->s.wait(timeout_usecs);
}
#endif

#ifndef DUCKDB_NO_THREADS
static vector<int> GetProcessCPUMask() {
#if defined(__GLIBC__)
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	if (sched_getaffinity(0, sizeof(cpu_set_t), &cpuset) != 0) {
		return {};
	}
	vector<int> available_cpus;
	for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
		if (CPU_ISSET(cpu, &cpuset)) {
			available_cpus.push_back(cpu);
		}
	}
	return available_cpus;
#else
	return {};
#endif
}

// Returns the available CPU ids when the pool should pin its threads, or an empty vector to skip pinning.
// Thread count means the number of threads that the pool should contain.
static vector<int> GetAvailableCPUsForPinning(TaskSchedulerType pool_type, ThreadPinMode pin_thread_mode,
                                              idx_t thread_count) {
	if (pin_thread_mode == ThreadPinMode::OFF) {
		return {};
	}
	// Only pin regular threads.
	if (pool_type != TaskSchedulerType::REGULAR) {
		return {};
	}

	// Only pin when there're enough cores available.
	auto available_cpus = GetProcessCPUMask();
	if (available_cpus.size() < thread_count) {
		return {};
	}

	// If mode is ON, we need to pin the threads.
	if (pin_thread_mode == ThreadPinMode::ON) {
		return available_cpus;
	}

	// Under AUTO mode, only pin when thread count from user setting exceeds the threshold.
	if (thread_count >= THREAD_PIN_THRESHOLD) {
		return available_cpus;
	}

	return {};
}
#endif

void TaskSchedulerPool::RelaunchThreads(TaskScheduler &scheduler, bool destroy) {
#ifndef DUCKDB_NO_THREADS
	auto &config = DBConfig::GetConfig(db);
	auto new_thread_count = destroy ? 0 : requested_thread_count.load();

	idx_t external_threads = 0;
	ThreadPinMode pin_thread_mode = ThreadPinMode::AUTO;
	if (!destroy) {
		// If we are destroying, i.e., calling ~TaskScheduler, we don't want to read the settings
		external_threads = Settings::Get<ExternalThreadsSetting>(config);
		pin_thread_mode = Settings::Get<PinThreadsSetting>(db);
	}

	if (threads.size() == new_thread_count) {
		current_thread_count = threads.size() + (pool_type == TaskSchedulerType::REGULAR ? external_threads : 0);
		return;
	}
	if (threads.size() != new_thread_count) {
		// we are changing the number of threads: clear all threads first
		// we do this even when increasing the number of threads to make sure that all threads follow the current
		// affinity mask
		for (idx_t i = 0; i < threads.size(); i++) {
			*markers[i] = false;
		}
		Signal(threads.size());
		// now join the threads to ensure they are fully stopped before erasing them
		for (idx_t i = 0; i < threads.size(); i++) {
			threads[i]->internal_thread->join();
		}
		// erase the threads/markers
		threads.clear();
		markers.clear();
	}
	if (threads.size() < new_thread_count) {
		// we are increasing the number of threads: launch them and run tasks on them
		idx_t create_new_threads = new_thread_count - threads.size();

		const auto available_cpus = GetAvailableCPUsForPinning(pool_type, pin_thread_mode, new_thread_count);
		for (idx_t i = 0; i < create_new_threads; i++) {
			// launch a thread and assign it a cancellation marker
			auto marker = unique_ptr<atomic<bool>>(new atomic<bool>(true));
			unique_ptr<thread> worker_thread;
			try {
				const auto cpu = threads.size() < available_cpus.size() ? optional_idx(available_cpus[threads.size()])
				                                                        : optional_idx();
				worker_thread =
				    make_uniq<thread>(&TaskScheduler::ExecuteForever, &scheduler, marker.get(), pool_type, cpu);
			} catch (std::exception &ex) {
				// thread constructor failed - this can happen when the system has too many threads allocated
				// in this case we cannot allocate more threads - stop launching them
				break;
			}
			auto thread_wrapper = make_uniq<TaskSchedulerThread>(std::move(worker_thread));

			threads.push_back(std::move(thread_wrapper));
			markers.push_back(std::move(marker));
		}
	}
	current_thread_count = threads.size() + (pool_type == TaskSchedulerType::REGULAR ? external_threads : 0);
	// On destroy the scheduler flushes fallback allocations after joining all pools.
	if (!destroy) {
		BlockAllocator::Get(db).FlushAll();
	}
#endif
}

}; // namespace duckdb
