#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parallel/scan_read_ahead.hpp"
#include "test_helpers.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <thread>

#ifndef _WIN32
#include <time.h>
#endif

using namespace duckdb;

namespace {

class CompletionTestTask : public BaseExecutorTask {
public:
	CompletionTestTask(TaskExecutor &executor, shared_ptr<ReadAheadJobCompletion> completion_p,
	                   std::atomic<idx_t> &executed_p, bool fail_p = false)
	    : BaseExecutorTask(executor), completion(std::move(completion_p)), executed(executed_p), fail(fail_p) {
		completion->AddIOTask();
	}
	~CompletionTestTask() override {
		completion->FinishIOTask();
	}

	void ExecuteTask() override {
		++executed;
		if (fail) {
			throw IOException("Injected read-ahead failure");
		}
	}

private:
	shared_ptr<ReadAheadJobCompletion> completion;
	std::atomic<idx_t> &executed;
	bool fail;
};

class CompletionTestSignal {
public:
	void Set() {
		lock_guard<mutex> guard(lock);
		set = true;
		cv.notify_all();
	}
	bool WaitFor(std::chrono::milliseconds timeout) {
		unique_lock<mutex> guard(lock);
		return cv.wait_for(guard, timeout, [&] { return set; });
	}

private:
	mutex lock;
	std::condition_variable cv;
	bool set = false;
};

class CompletionTestCallback : public Task {
public:
	CompletionTestCallback(CompletionTestSignal &entered_p, CompletionTestSignal &release_p)
	    : entered(entered_p), release(release_p) {
	}

	TaskExecutionResult Execute(TaskExecutionMode) override {
		return TaskExecutionResult::TASK_FINISHED;
	}
	void Reschedule() override {
		entered.Set();
		release.WaitFor(std::chrono::seconds(5));
	}

private:
	CompletionTestSignal &entered;
	CompletionTestSignal &release;
};

#if !defined(_WIN32) && !defined(DUCKDB_NO_THREADS)
bool CompletionTestThreadCPUSeconds(double &result) {
	timespec ts;
	if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) {
		return false;
	}
	result = double(ts.tv_sec) + double(ts.tv_nsec) / 1e9;
	return true;
}

class CompletionTestRead : public AsyncTask {
public:
	CompletionTestRead(CompletionTestSignal &started_p, CompletionTestSignal &release_p,
	                   shared_ptr<CompletionTestSignal> destroyed_p)
	    : started(started_p), release(release_p), destroyed(std::move(destroyed_p)) {
	}
	~CompletionTestRead() override {
		destroyed->Set();
	}
	void Execute() override {
		started.Set();
		if (!release.WaitFor(std::chrono::seconds(5))) {
			throw IOException("Timed out waiting to release the test read");
		}
	}

private:
	CompletionTestSignal &started;
	CompletionTestSignal &release;
	shared_ptr<CompletionTestSignal> destroyed;
};
#endif

void DisableCompletionTestWorkers(Connection &con) {
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	REQUIRE_NO_FAIL(con.Query("SET async_threads=0"));
}

} // namespace

TEST_CASE("Read-ahead completion executes queued I/O without background workers", "[scan_read_ahead][intraquery]") {
	DuckDB db(nullptr);
	Connection con(db);
	DisableCompletionTestWorkers(con);
	auto executor = make_shared_ptr<TaskExecutor>(*con.context, TaskSchedulerType::ASYNC);
	auto completion = make_shared_ptr<ReadAheadJobCompletion>(executor);
	std::atomic<idx_t> executed {0};

	SECTION("Successful reads") {
		for (idx_t i = 0; i < 3; i++) {
			executor->ScheduleTask(make_uniq<CompletionTestTask>(*executor, completion, executed));
		}
		completion->WaitForIO();
		REQUIRE(executed == 3);
		REQUIRE(!executor->HasError());
	}
	SECTION("Failed reads still settle their completion") {
		executor->ScheduleTask(make_uniq<CompletionTestTask>(*executor, completion, executed, true));
		completion->WaitForIO();
		REQUIRE(executed == 1);
		REQUIRE(executor->HasError());
	}
	SECTION("Cancelled reads still settle their completion") {
		executor->ScheduleTask(make_uniq<CompletionTestTask>(*executor, completion, executed));
		executor->CancelAndDrain();
		completion->WaitForIO();
		REQUIRE(executed == 0);
	}
	SECTION("Unscheduled reads still settle their completion") {
		auto task = make_uniq<CompletionTestTask>(*executor, completion, executed);
		task.reset();
		completion->WaitForIO();
		REQUIRE(executed == 0);
	}
}

#ifndef DUCKDB_NO_THREADS
TEST_CASE("Read-ahead completion waits until an executed task is destroyed", "[scan_read_ahead][intraquery]") {
	DuckDB db(nullptr);
	Connection con(db);
	DisableCompletionTestWorkers(con);
	auto executor = make_shared_ptr<TaskExecutor>(*con.context, TaskSchedulerType::ASYNC);
	auto completion = make_shared_ptr<ReadAheadJobCompletion>(executor);
	std::atomic<idx_t> executed {0};
	executor->ScheduleTask(make_uniq<CompletionTestTask>(*executor, completion, executed));
	shared_ptr<Task> task;
	REQUIRE(executor->GetTask(task));
	REQUIRE(task->Execute(TaskExecutionMode::PROCESS_ALL) == TaskExecutionResult::TASK_FINISHED);

	CompletionTestSignal started;
	CompletionTestSignal finished;
	std::thread waiter([&] {
		started.Set();
		completion->WaitForIO();
		finished.Set();
	});
	const bool waiter_started = started.WaitFor(std::chrono::seconds(5));
	const bool returned_before_destruction = finished.WaitFor(std::chrono::milliseconds(20));
	task.reset();
	const bool waiter_finished = finished.WaitFor(std::chrono::seconds(5));
	waiter.join();

	REQUIRE(waiter_started);
	REQUIRE(!returned_before_destruction);
	REQUIRE(waiter_finished);
	REQUIRE(executed == 1);
}

TEST_CASE("Read-ahead completion wakes every waiter when the final I/O finishes", "[scan_read_ahead][intraquery]") {
	DuckDB db(nullptr);
	Connection con(db);
	DisableCompletionTestWorkers(con);
	auto executor = make_shared_ptr<TaskExecutor>(*con.context, TaskSchedulerType::ASYNC);
	auto completion = make_shared_ptr<ReadAheadJobCompletion>(executor);
	completion->AddIOTask();
	completion->AddIOTask();
	std::atomic<idx_t> started {0};
	std::atomic<idx_t> finished {0};
	vector<std::thread> waiters;
	for (idx_t i = 0; i < 4; i++) {
		waiters.emplace_back([&] {
			++started;
			completion->WaitForIO();
			++finished;
		});
	}
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (started < waiters.size() && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	completion->FinishIOTask();
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	const auto finished_early = finished.load();
	completion->FinishIOTask();
	for (auto &waiter : waiters) {
		waiter.join();
	}
	REQUIRE(finished_early == 0);
	REQUIRE(finished == waiters.size());
	// An already-complete job returns immediately.
	completion->WaitForIO();
}

#ifndef _WIN32
TEST_CASE("Read-ahead job teardown does not spin while I/O is running", "[scan_read_ahead][intraquery][.]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	REQUIRE_NO_FAIL(con.Query("SET async_threads=1"));
	CompletionTestSignal read_started;
	CompletionTestSignal release_read;
	auto read_destroyed = make_shared_ptr<CompletionTestSignal>();
	CompletionTestSignal callback_entered;
	CompletionTestSignal release_callback;
	release_callback.Set();
	auto callback = make_shared_ptr<CompletionTestCallback>(callback_entered, release_callback);
	InterruptState interrupt {weak_ptr<Task>(callback)};
	ScanReadAhead read_ahead(*con.context, 1, nullptr);
	struct JobState {};
	bool produced = false;
	auto produce = [&](vector<unique_ptr<AsyncTask>> &io_tasks) -> unique_ptr<ScanReadAheadJob> {
		if (produced) {
			return nullptr;
		}
		produced = true;
		io_tasks.push_back(make_uniq<CompletionTestRead>(read_started, release_read, read_destroyed));
		return make_uniq<ScanReadAheadJobWrapper<JobState>>();
	};
	TableFunctionInput input(nullptr, nullptr, nullptr);
	input.results_execution_mode = AsyncResultsExecutionMode::TASK_EXECUTOR;
	input.interrupt_state = interrupt;
	unique_ptr<ScanReadAheadJob> job;
	// Park first so the async worker runs the read, rather than executing it on the measuring thread.
	const auto acquired = read_ahead.AcquireJob(*con.context, input, produce, job);
	const bool started = read_started.WaitFor(std::chrono::seconds(5));
	std::thread release([&] {
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		release_read.Set();
	});
	double start_cpu = 0;
	double end_cpu = 0;
	const bool start_valid = CompletionTestThreadCPUSeconds(start_cpu);
	job.reset();
	const bool end_valid = CompletionTestThreadCPUSeconds(end_cpu);
	release.join();
	read_ahead.CancelAndDrain();
	// Task accounting finishes before destruction; keep callback state alive until the I/O task releases it.
	const bool destroyed = read_destroyed->WaitFor(std::chrono::seconds(5));
	read_ahead.ThrowIfError();
	REQUIRE(acquired == ScanReadAheadAcquire::PARKED);
	REQUIRE(started);
	REQUIRE(destroyed);
	REQUIRE(start_valid);
	REQUIRE(end_valid);
	const auto cpu_seconds = end_cpu - start_cpu;
	INFO("CPU seconds while waiting for I/O: " << cpu_seconds);
	REQUIRE(cpu_seconds < 0.1);
}
#endif

TEST_CASE("Read-ahead completion runs reschedule callbacks outside its wait lock", "[scan_read_ahead][intraquery]") {
	DuckDB db(nullptr);
	Connection con(db);
	DisableCompletionTestWorkers(con);
	auto executor = make_shared_ptr<TaskExecutor>(*con.context, TaskSchedulerType::ASYNC);
	auto completion = make_shared_ptr<ReadAheadJobCompletion>(executor);
	completion->AddIOTask();
	CompletionTestSignal callback_entered;
	CompletionTestSignal release_callback;
	auto callback = make_shared_ptr<CompletionTestCallback>(callback_entered, release_callback);
	REQUIRE(completion->TryPark(InterruptState(weak_ptr<Task>(callback))));

	CompletionTestSignal waiter_started;
	CompletionTestSignal waiter_finished;
	std::thread waiter([&] {
		waiter_started.Set();
		completion->WaitForIO();
		waiter_finished.Set();
	});
	const bool started = waiter_started.WaitFor(std::chrono::seconds(5));
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	std::thread io([&] { completion->FinishIOTask(); });
	const bool callback_started = callback_entered.WaitFor(std::chrono::seconds(5));
	const bool finished_before_callback = waiter_finished.WaitFor(std::chrono::seconds(1));
	release_callback.Set();
	io.join();
	waiter.join();

	REQUIRE(started);
	REQUIRE(callback_started);
	REQUIRE(finished_before_callback);
}
#endif
