#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/parallel/task_executor.hpp"
#include "duckdb/parallel/task_scheduler.hpp"

#include <chrono>
#include <thread>

using namespace duckdb;

//! Yields after every step until stopped, like a long-running query under scheduler_process_partial
struct EndlessTask : BaseExecutorTask {
	EndlessTask(TaskExecutor &executor, const atomic<bool> &stop) : BaseExecutorTask(executor), stop(stop) {
	}

	TaskExecutionResult ExecuteTaskStep() override {
		return stop ? TaskExecutionResult::TASK_FINISHED : TaskExecutionResult::TASK_NOT_FINISHED;
	}

	const atomic<bool> &stop;
};

//! Finishes in a single step, like a short query
struct ShortTask : BaseExecutorTask {
	ShortTask(TaskExecutor &executor, atomic<bool> &done) : BaseExecutorTask(executor), done(done) {
	}

	void ExecuteTask() override {
		done = true;
	}

	atomic<bool> &done;
};

TEST_CASE("An older query is not starved by newer queries", "[scheduler]") {
	DuckDB db;
	Connection con(db);
	// a single worker thread, which puts unfinished tasks back in the queue after every step
	REQUIRE_NO_FAIL(con.Query("SET threads=2"));
	REQUIRE_NO_FAIL(con.Query("SET scheduler_process_partial=true"));

	// every TaskExecutor has its own producer, like the Executor of a query
	TaskExecutor old_query(*con.context);
	atomic<bool> stop {false};
	vector<unique_ptr<TaskExecutor>> new_queries;
	for (idx_t i = 0; i < 3; i++) {
		new_queries.push_back(make_uniq<TaskExecutor>(*con.context));
		new_queries.back()->ScheduleTask(make_uniq<EndlessTask>(*new_queries.back(), stop));
	}

	// the newer queries keep the worker busy, but the older query's short task must still get a turn
	atomic<bool> done {false};
	old_query.ScheduleTask(make_uniq<ShortTask>(old_query, done));
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!done && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	CHECK(done);

	stop = true;
	old_query.WorkOnTasks();
	for (auto &new_query : new_queries) {
		new_query->WorkOnTasks();
	}
}

#if defined(__linux__) && defined(__GLIBC__) && !defined(DUCKDB_NO_THREADS)
#include <pthread.h>
#include <sched.h>

namespace {
struct CPUIdTestAffinity {
	CPUIdTestAffinity() {
		valid = pthread_getaffinity_np(pthread_self(), sizeof(original), &original) == 0;
	}
	~CPUIdTestAffinity() {
		if (valid) {
			pthread_setaffinity_np(pthread_self(), sizeof(original), &original);
		}
	}
	static int32_t Pin(idx_t cpu) {
		cpu_set_t mask;
		CPU_ZERO(&mask);
		CPU_SET(cpu, &mask);
		return pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask);
	}
	cpu_set_t original;
	bool valid;
};

struct CPUIdObservation {
	atomic<bool> done {false};
	idx_t estimated = DConstants::INVALID_INDEX;
	int32_t actual = -1;
	int32_t pin_result = 0;
};

struct ObserveCPUIdTask : BaseExecutorTask {
	ObserveCPUIdTask(TaskExecutor &executor, CPUIdObservation &result, optional_idx move_to)
	    : BaseExecutorTask(executor), result(result), move_to(move_to) {
	}
	void ExecuteTask() override {
		if (move_to.IsValid()) {
			result.pin_result = CPUIdTestAffinity::Pin(move_to.GetIndex());
		}
		result.estimated = TaskScheduler::GetEstimatedCPUId();
		result.actual = sched_getcpu();
		result.done = true;
	}
	CPUIdObservation &result;
	optional_idx move_to;
};
} // namespace

TEST_CASE("Scheduler CPU estimates follow pinning and unpinned worker migration", "[scheduler][scheduler_cpu]") {
	CPUIdTestAffinity affinity;
	if (!affinity.valid) {
		return;
	}
	vector<idx_t> cpus;
	for (idx_t cpu = 0; cpu < CPU_SETSIZE; cpu++) {
		if (CPU_ISSET(cpu, &affinity.original)) {
			cpus.push_back(cpu);
		}
	}
	if (cpus.size() < 2) {
		return;
	}
	const auto first_cpu = cpus.back();
	const auto second_cpu = cpus[cpus.size() - 2];
	DBConfig config;
	config.options.maximum_threads = 1;
	config.options.async_threads = 0;
	DuckDB db(nullptr, &config);
	Connection con(db);

	auto observe = [&](TaskSchedulerType pool_type, idx_t expected, optional_idx move_to = optional_idx()) {
		CPUIdObservation result;
		TaskExecutor executor(TaskScheduler::GetScheduler(*con.context), pool_type);
		executor.ScheduleTask(make_uniq<ObserveCPUIdTask>(executor, result, move_to));
		auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!result.done && std::chrono::steady_clock::now() < deadline) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		CHECK(result.done);
		executor.WorkOnTasks();
		CHECK(result.pin_result == 0);
		CHECK(result.actual == static_cast<int32_t>(expected));
		CHECK(result.estimated == expected);
	};

	// Worker zero must report the selected OS CPU, including after a relaunch.
	REQUIRE_NO_FAIL(con.Query("SET pin_threads='on'"));
	for (auto cpu : {first_cpu, second_cpu}) {
		REQUIRE(CPUIdTestAffinity::Pin(cpu) == 0);
		REQUIRE_NO_FAIL(con.Query("SET threads=2"));
		observe(TaskSchedulerType::REGULAR, cpu);
		REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	}

	// Unpinned regular and async workers must query their current CPU after migration.
	REQUIRE(CPUIdTestAffinity::Pin(first_cpu) == 0);
	REQUIRE_NO_FAIL(con.Query("SET pin_threads='off'"));
	REQUIRE_NO_FAIL(con.Query("SET threads=2"));
	observe(TaskSchedulerType::REGULAR, second_cpu, second_cpu);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	REQUIRE_NO_FAIL(con.Query("SET async_threads=1"));
	observe(TaskSchedulerType::ASYNC, second_cpu, second_cpu);
	REQUIRE_NO_FAIL(con.Query("SET async_threads=0"));

	// External callers with no CPU or an invalid CPU keep using the dynamic estimate.
	auto &scheduler = TaskScheduler::GetScheduler(*con.context);
	atomic<bool> running {false};
	REQUIRE(CPUIdTestAffinity::Pin(first_cpu) == 0);
	REQUIRE_NOTHROW(scheduler.ExecuteForever(&running));
	REQUIRE(CPUIdTestAffinity::Pin(second_cpu) == 0);
	CHECK(TaskScheduler::GetEstimatedCPUId() == second_cpu);
	REQUIRE_NOTHROW(scheduler.ExecuteForever(&running, TaskSchedulerType::REGULAR, CPU_SETSIZE));
	CHECK(sched_getcpu() == static_cast<int32_t>(second_cpu));
	REQUIRE(CPUIdTestAffinity::Pin(first_cpu) == 0);
	CHECK(TaskScheduler::GetEstimatedCPUId() == first_cpu);
}
#endif
