#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/parallel/task_executor.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

using namespace duckdb;

struct SleepingTask : BaseExecutorTask {
	using BaseExecutorTask::BaseExecutorTask;

	void ExecuteTask() override {
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
	}
};

struct CountingTask : BaseExecutorTask {
	CountingTask(TaskExecutor &executor, std::atomic<int> &executed, std::atomic<int> &cancelled)
	    : BaseExecutorTask(executor), executed(executed), cancelled(cancelled) {
	}

	void ExecuteTask() override {
		++executed;
	}

	void Cancel() override {
		++cancelled;
	}

	std::atomic<int> &executed;
	std::atomic<int> &cancelled;
};

TEST_CASE("TaskExecutor drains tasks that are in flight when the workers are removed") {
	DuckDB db;
	Connection con {db};
	REQUIRE_NO_FAIL(con.Query("SET threads=5"));
	REQUIRE_NO_FAIL(con.Query("SET scheduler_process_partial=true"));
	TaskExecutor executor {*con.context};

	// One task per background worker (threads=5, external=1 -> 4 workers).
	for (auto i = 0; i < 4; i++) {
		executor.ScheduleTask(make_uniq<SleepingTask>(executor));
	}

	// Let each worker grab a task and enter its sleep.
	std::this_thread::sleep_for(std::chrono::milliseconds(100));

	// WorkOnTasks finds the producer queue empty, all tasks are in worker hands.
	auto finished = std::async(std::launch::async, [&] { executor.WorkOnTasks(); });

	// Kill the background workers while their tasks are still running.
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));

	REQUIRE(finished.wait_for(std::chrono::seconds(30)) == std::future_status::ready);
}

TEST_CASE("TaskExecutor runs exactly one of ExecuteTask or Cancel for every scheduled task") {
	static constexpr int TASK_COUNT = 64;

	DuckDB db;
	Connection con {db};
	std::atomic<int> executed {0};
	std::atomic<int> cancelled {0};
	{
		TaskExecutor executor {*con.context};
		for (auto i = 0; i < TASK_COUNT; i++) {
			executor.ScheduleTask(make_uniq<CountingTask>(executor, executed, cancelled));
		}
		executor.CancelAndDrain();
	}
	REQUIRE(executed + cancelled == TASK_COUNT);
}

struct SteppingTask : BaseExecutorTask {
	SteppingTask(TaskExecutor &executor, std::atomic<int> &steps, int total)
	    : BaseExecutorTask(executor), steps(steps), total(total) {
	}

	TaskExecutionResult ExecuteTaskStep() override {
		if (++steps >= total) {
			return TaskExecutionResult::TASK_FINISHED;
		}
		return TaskExecutionResult::TASK_NOT_FINISHED;
	}

	std::atomic<int> &steps;
	int total;
};

TEST_CASE("TaskExecutor drains an incremental task to completion") {
	static constexpr int STEPS = 8;

	DuckDB db;
	Connection con {db};
	std::atomic<int> steps {0};
	TaskExecutor executor {*con.context};
	executor.ScheduleTask(make_uniq<SteppingTask>(executor, steps, STEPS));
	// WorkOnTasks drains inline with PROCESS_ALL, which must loop the steps to completion
	executor.WorkOnTasks();
	REQUIRE(steps == STEPS);
}

namespace {

struct BatchTaskCounts {
	std::atomic<idx_t> executed {0};
	std::atomic<idx_t> cancelled {0};
	std::atomic<idx_t> destroyed {0};
	std::atomic<idx_t> steps {0};
};

struct BatchTask : BaseExecutorTask {
	BatchTask(TaskExecutor &executor, shared_ptr<BatchTaskCounts> counts, idx_t step_count = 1)
	    : BaseExecutorTask(executor), counts(std::move(counts)), remaining_steps(step_count) {
	}
	~BatchTask() override {
		counts->destroyed++;
	}

	TaskExecutionResult ExecuteTaskStep() override {
		counts->steps++;
		if (--remaining_steps) {
			return TaskExecutionResult::TASK_NOT_FINISHED;
		}
		counts->executed++;
		return TaskExecutionResult::TASK_FINISHED;
	}
	void Cancel() override {
		counts->cancelled++;
	}

	shared_ptr<BatchTaskCounts> counts;
	idx_t remaining_steps;
};

struct FailingBatchTask : BatchTask {
	using BatchTask::BatchTask;

	TaskExecutionResult ExecuteTaskStep() override {
		counts->executed++;
		throw std::runtime_error("injected batch execution failure");
	}
	void Cancel() override {
		counts->cancelled++;
		throw std::runtime_error("injected batch cancellation failure");
	}
};

} // namespace

TEST_CASE("TaskExecutor drains empty, single and multi-task batches", "[api][task_batch]") {
	DuckDB db;
	Connection con(db);
#ifndef DUCKDB_NO_THREADS
	REQUIRE_NO_FAIL(con.Query("SET threads=4"));
	REQUIRE_NO_FAIL(con.Query("SET async_threads=2"));
#else
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	REQUIRE_NO_FAIL(con.Query("SET async_threads=0"));
#endif
	REQUIRE_NO_FAIL(con.Query("SET scheduler_process_partial=true"));
	for (auto pool : {TaskSchedulerType::REGULAR, TaskSchedulerType::ASYNC}) {
		for (idx_t task_count : {0, 1, 64, 1025}) {
			auto counts = make_shared_ptr<BatchTaskCounts>();
			{
				TaskExecutor executor(*con.context, pool);
				vector<unique_ptr<BaseExecutorTask>> tasks;
				for (idx_t i = 0; i < task_count; i++) {
					tasks.push_back(make_uniq<BatchTask>(executor, counts, 3));
				}
				executor.ScheduleTasks(std::move(tasks));
				executor.WorkOnTasks();
				REQUIRE(counts->executed == task_count);
				REQUIRE(counts->cancelled == 0);
				REQUIRE(counts->steps == task_count * 3);
			}
		}
	}
}

TEST_CASE("TaskExecutor drains batch execution and cancellation failures", "[api][task_batch]") {
	DuckDB db;
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	auto counts = make_shared_ptr<BatchTaskCounts>();
	TaskExecutor executor(*con.context);
	vector<unique_ptr<BaseExecutorTask>> tasks;
	tasks.push_back(make_uniq<FailingBatchTask>(executor, counts));
	for (idx_t i = 1; i < 64; i++) {
		tasks.push_back(make_uniq<BatchTask>(executor, counts));
	}
	executor.ScheduleTasks(std::move(tasks));
	SECTION("Execution failure retires the remainder") {
		REQUIRE_THROWS(executor.WorkOnTasks());
		REQUIRE(counts->executed == 1);
		REQUIRE(counts->cancelled == 63);
	}
	SECTION("Cancellation failure still drains the batch") {
		REQUIRE_NOTHROW(executor.CancelAndDrain());
		REQUIRE(counts->executed == 0);
		REQUIRE(counts->cancelled == 64);
	}
	REQUIRE(executor.HasError());
	REQUIRE(counts->destroyed == 64);
}

#ifndef DUCKDB_NO_THREADS
TEST_CASE("TaskExecutor accepts concurrent batch submissions", "[api][task_batch]") {
	DuckDB db;
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=8"));
	auto counts = make_shared_ptr<BatchTaskCounts>();
	TaskExecutor executor(*con.context);
	vector<std::thread> producers;
	for (idx_t p = 0; p < 4; p++) {
		producers.emplace_back([&] {
			for (idx_t batch = 0; batch < 16; batch++) {
				vector<unique_ptr<BaseExecutorTask>> tasks;
				for (idx_t i = 0; i < 64; i++) {
					tasks.push_back(make_uniq<BatchTask>(executor, counts));
				}
				executor.ScheduleTasks(std::move(tasks));
			}
		});
	}
	for (auto &producer : producers) {
		producer.join();
	}
	executor.WorkOnTasks();
	REQUIRE(counts->executed == 4 * 16 * 64);
	REQUIRE(counts->cancelled == 0);
}
#endif
