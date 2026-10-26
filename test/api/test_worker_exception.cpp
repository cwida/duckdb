#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/parallel/task_scheduler.hpp"

#include <chrono>
#include <new>
#include <thread>

using namespace duckdb;

#ifndef DUCKDB_NO_THREADS

//! How long a worker waits before it retries after an exception
static constexpr auto RETRY_WAIT = std::chrono::milliseconds(10);
//! How often a worker retries the task it holds before the exception leaves the worker
static constexpr idx_t MAX_TASK_RETRIES = 1000;

//! An exception type outside the standard exception hierarchy
struct NonStandardError {};

//! Blocks when it runs, and its first `failures` deschedules throw, like a worker that cannot allocate
struct UndeschedulableTask : public Task {
	explicit UndeschedulableTask(idx_t failures, bool standard_error = true)
	    : failures(failures), standard_error(standard_error) {
	}

	TaskExecutionResult Execute(TaskExecutionMode mode) override {
		executions++;
		return TaskExecutionResult::TASK_BLOCKED;
	}

	void Deschedule() override {
		auto now = std::chrono::steady_clock::now();
		auto attempt = ++attempts;
		if (attempt > 1 && now - last_attempt < shortest_gap) {
			shortest_gap = now - last_attempt;
		}
		last_attempt = now;
		if (attempt <= failures) {
			while (fail_after_stop && *fail_after_stop) {
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			if (!standard_error) {
				throw NonStandardError();
			}
			throw std::bad_alloc();
		}
		descheduled = true;
	}

	const idx_t failures;
	const bool standard_error;
	//! The marker of the worker, a failing deschedule waits until it is false
	optional_ptr<const atomic<bool>> fail_after_stop;
	atomic<idx_t> executions {0};
	atomic<idx_t> attempts {0};
	atomic<bool> descheduled {false};
	std::chrono::steady_clock::time_point last_attempt;
	//! The shortest time between two attempts
	std::chrono::steady_clock::duration shortest_gap = std::chrono::steady_clock::duration::max();
};

struct FinishingTask : public Task {
	TaskExecutionResult Execute(TaskExecutionMode mode) override {
		done = true;
		return TaskExecutionResult::TASK_FINISHED;
	}

	atomic<bool> done {false};
};

template <class CONDITION>
static bool WaitFor(CONDITION condition) {
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (!condition() && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return condition();
}

TEST_CASE("A worker thread that cannot deschedule a blocked task tries again", "[scheduler]") {
	DuckDB db;
	Connection con(db);
	// a single worker thread
	REQUIRE_NO_FAIL(con.Query("SET threads=2"));
	auto &scheduler = TaskScheduler::GetScheduler(*con.context);
	auto producer = scheduler.CreateProducer();

	auto blocked = make_shared_ptr<UndeschedulableTask>(5);
	scheduler.ScheduleTask(*producer, blocked);
	REQUIRE(WaitFor([&]() { return blocked->descheduled.load(); }));
	// the task ran once, and the worker waited before every new attempt
	REQUIRE(blocked->executions == 1);
	REQUIRE(blocked->attempts == 6);
	// sleeps can return early, e.g. on Windows, so only require a wait of about half of RETRY_WAIT
	REQUIRE(blocked->shortest_gap >= RETRY_WAIT / 2);

	// the worker still runs tasks
	auto next = make_shared_ptr<FinishingTask>();
	scheduler.ScheduleTask(*producer, next);
	REQUIRE(WaitFor([&]() { return next->done.load(); }));
}

TEST_CASE("A worker thread tries again after an exception that is not a std::exception", "[scheduler]") {
	DuckDB db;
	Connection con(db);
	// a single worker thread
	REQUIRE_NO_FAIL(con.Query("SET threads=2"));
	auto &scheduler = TaskScheduler::GetScheduler(*con.context);
	auto producer = scheduler.CreateProducer();

	auto blocked = make_shared_ptr<UndeschedulableTask>(1, false);
	scheduler.ScheduleTask(*producer, blocked);
	REQUIRE(WaitFor([&]() { return blocked->descheduled.load(); }));
	REQUIRE(blocked->executions == 1);
	REQUIRE(blocked->attempts == 2);
}

TEST_CASE("A worker thread that stops first deschedules the blocked task it holds", "[scheduler]") {
	DuckDB db;
	Connection con(db);
	// no worker threads of the pool, the test runs its own
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	auto &scheduler = TaskScheduler::GetScheduler(*con.context);
	auto producer = scheduler.CreateProducer();

	atomic<bool> marker {true};
	auto blocked = make_shared_ptr<UndeschedulableTask>(1);
	blocked->fail_after_stop = marker;
	scheduler.ScheduleTask(*producer, blocked);
	std::thread worker([&]() { scheduler.ExecuteForever(&marker); });
	auto attempted = WaitFor([&]() { return blocked->attempts.load() > 0; });
	// the deschedule throws once the worker is told to stop
	marker = false;
	worker.join();
	REQUIRE(attempted);
	REQUIRE(blocked->descheduled);
	REQUIRE(blocked->executions == 1);
	REQUIRE(blocked->attempts == 2);
}

TEST_CASE("A worker thread that can never deschedule the task it holds gives up after its retries", "[scheduler][.]") {
	DuckDB db;
	Connection con(db);
	// no worker threads of the pool, the test runs its own
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	auto &scheduler = TaskScheduler::GetScheduler(*con.context);
	auto producer = scheduler.CreateProducer();

	atomic<bool> marker {true};
	auto blocked = make_shared_ptr<UndeschedulableTask>(NumericLimits<idx_t>::Maximum());
	scheduler.ScheduleTask(*producer, blocked);
	// in a thread of the pool, the exception ends the process
	atomic<bool> rethrown {false};
	std::thread worker([&]() {
		try {
			scheduler.ExecuteForever(&marker);
		} catch (std::bad_alloc &) {
			rethrown = true;
		}
	});
	worker.join();
	REQUIRE(rethrown);
	REQUIRE(blocked->executions == 1);
	REQUIRE(blocked->attempts == MAX_TASK_RETRIES + 1);
	REQUIRE(!blocked->descheduled);
}

#endif
