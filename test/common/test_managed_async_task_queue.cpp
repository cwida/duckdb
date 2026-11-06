#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/serializer/async_task_queue.hpp"
#include "test_helpers.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <thread>

using namespace duckdb;

namespace {

//! Task that increments a shared counter once when executed.
class CountingTask : public AsyncTask {
public:
	explicit CountingTask(std::atomic<idx_t> &counter_p) : counter(counter_p) {
	}

	void Execute() override {
		counter++;
	}

private:
	std::atomic<idx_t> &counter;
};

//! Task that always throws when executed.
class ThrowingTask : public AsyncTask {
public:
	void Execute() override {
		throw IOException("Injected async task failure");
	}
};

//! Coordinates how many tasks are running concurrently, blocking each task until released.
class ConcurrencyLatch {
public:
	void Enter() {
		unique_lock<mutex> guard(lock);
		active++;
		max_active = MaxValue(max_active, active);
		entered++;
		cv.notify_all();
		cv.wait(guard, [&]() { return released; });
		active--;
	}

	bool WaitForEntered(idx_t count) {
		unique_lock<mutex> guard(lock);
		return cv.wait_for(guard, std::chrono::seconds(5), [&]() { return entered >= count; });
	}

	void Release() {
		{
			lock_guard<mutex> guard(lock);
			released = true;
		}
		cv.notify_all();
	}

	idx_t MaxActive() {
		lock_guard<mutex> guard(lock);
		return max_active;
	}

private:
	mutex lock;
	std::condition_variable cv;
	idx_t active = 0;
	idx_t max_active = 0;
	idx_t entered = 0;
	bool released = false;
};

//! Task that blocks on a ConcurrencyLatch until released.
class LatchTask : public AsyncTask {
public:
	explicit LatchTask(ConcurrencyLatch &latch_p) : latch(latch_p) {
	}

	void Execute() override {
		latch.Enter();
	}

private:
	ConcurrencyLatch &latch;
};

//! Task that blocks on a ConcurrencyLatch and then throws.
class LatchThrowingTask : public AsyncTask {
public:
	explicit LatchThrowingTask(ConcurrencyLatch &latch_p) : latch(latch_p) {
	}

	void Execute() override {
		latch.Enter();
		throw IOException("Injected async task failure");
	}

private:
	ConcurrencyLatch &latch;
};

unique_ptr<Connection> TaskQueueConnectionWithAsyncThreads(DuckDB &db, idx_t async_threads = 1) {
	auto con = make_uniq<Connection>(db);
	REQUIRE_NO_FAIL(con->Query("SET async_threads=" + to_string(async_threads)));
	return con;
}

unique_ptr<Connection> TaskQueueConnectionWithoutAsyncThreads(DuckDB &db) {
	auto con = make_uniq<Connection>(db);
	REQUIRE_NO_FAIL(con->Query("SET async_threads=0"));
	return con;
}

} // namespace

TEST_CASE("ManagedAsyncTaskQueue runs all registered tasks", "[async_task_queue]") {
	DuckDB db(nullptr);
	auto con = TaskQueueConnectionWithAsyncThreads(db, 4);
	ManagedAsyncTaskQueue queue(con->context->GetContext());
	REQUIRE(queue.IsAsync());

	std::atomic<idx_t> counter(0);
	const idx_t task_count = 50;
	for (idx_t i = 0; i < task_count; i++) {
		queue.Register(make_uniq<CountingTask>(counter), 16);
	}
	queue.WaitAll();
	REQUIRE(counter.load() == task_count);
	queue.Close();
}

TEST_CASE("ManagedAsyncTaskQueue runs tasks concurrently", "[async_task_queue]") {
	DuckDB db(nullptr);
	auto con = TaskQueueConnectionWithAsyncThreads(db, 4);
	ManagedAsyncTaskQueue queue(con->context->GetContext());

	ConcurrencyLatch latch;
	for (idx_t i = 0; i < 4; i++) {
		queue.Register(make_uniq<LatchTask>(latch), 16);
	}
	REQUIRE(latch.WaitForEntered(2));
	latch.Release();
	queue.Close();

	REQUIRE(latch.MaxActive() >= 2);
}

TEST_CASE("ManagedAsyncTaskQueue honors max_active_tasks concurrency cap", "[async_task_queue]") {
	DuckDB db(nullptr);
	auto con = TaskQueueConnectionWithAsyncThreads(db, 4);
	ManagedAsyncTaskQueue queue(con->context->GetContext(), 1);

	ConcurrencyLatch latch;
	for (idx_t i = 0; i < 3; i++) {
		queue.Register(make_uniq<LatchTask>(latch), 16);
	}
	REQUIRE(latch.WaitForEntered(1));
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	REQUIRE(latch.MaxActive() == 1);
	latch.Release();
	queue.Close();

	REQUIRE(latch.MaxActive() == 1);
}

TEST_CASE("ManagedAsyncTaskQueue runs synchronously without async threads", "[async_task_queue]") {
	DuckDB db(nullptr);
	auto con = TaskQueueConnectionWithoutAsyncThreads(db);
	ManagedAsyncTaskQueue queue(con->context->GetContext());
	REQUIRE(!queue.IsAsync());

	std::atomic<idx_t> counter(0);
	queue.Register(make_uniq<CountingTask>(counter), 16);
	// Without async threads the task runs inline before Register returns.
	REQUIRE(counter.load() == 1);
	queue.Register(make_uniq<CountingTask>(counter), 16);
	REQUIRE(counter.load() == 2);

	queue.WaitAll();
	queue.Close();
	REQUIRE(counter.load() == 2);
}

TEST_CASE("ManagedAsyncTaskQueue rethrows asynchronous task errors", "[async_task_queue]") {
	DuckDB db(nullptr);
	auto con = TaskQueueConnectionWithAsyncThreads(db, 2);
	ManagedAsyncTaskQueue queue(con->context->GetContext());

	queue.Register(make_uniq<ThrowingTask>(), 16);
	try {
		queue.Close();
		FAIL("Expected async task failure");
	} catch (const Exception &ex) {
		string error = ex.what();
		REQUIRE(error.find("Injected async task failure") != string::npos);
	}
}

TEST_CASE("ManagedAsyncTaskQueue rethrows synchronous task errors", "[async_task_queue]") {
	DuckDB db(nullptr);
	auto con = TaskQueueConnectionWithoutAsyncThreads(db);
	ManagedAsyncTaskQueue queue(con->context->GetContext());

	try {
		queue.Register(make_uniq<ThrowingTask>(), 16);
		FAIL("Expected synchronous task failure");
	} catch (const Exception &ex) {
		string error = ex.what();
		REQUIRE(error.find("Injected async task failure") != string::npos);
	}
	queue.Close();
}

TEST_CASE("ManagedAsyncTaskQueue applies backpressure without deadlock", "[async_task_queue]") {
	DuckDB db(nullptr);
	auto con = TaskQueueConnectionWithAsyncThreads(db, 2);
	ManagedAsyncTaskQueue queue(con->context->GetContext());

	std::atomic<idx_t> counter(0);
	const idx_t task_count = 20;
	for (idx_t i = 0; i < task_count; i++) {
		queue.Register(make_uniq<CountingTask>(counter), 1ULL << 20);
		queue.ApplyBackpressure();
	}
	queue.WaitAll();
	REQUIRE(counter.load() == task_count);
	queue.Close();
}

TEST_CASE("ManagedAsyncTaskQueue close drains without explicit WaitAll and is idempotent", "[async_task_queue]") {
	DuckDB db(nullptr);
	auto con = TaskQueueConnectionWithAsyncThreads(db, 2);
	ManagedAsyncTaskQueue queue(con->context->GetContext());

	std::atomic<idx_t> counter(0);
	const idx_t task_count = 10;
	for (idx_t i = 0; i < task_count; i++) {
		queue.Register(make_uniq<CountingTask>(counter), 16);
	}
	queue.Close();
	REQUIRE(counter.load() == task_count);

	// Double close and post-close error checks must not throw.
	queue.Close();
	queue.RethrowTaskError();
}

TEST_CASE("ManagedAsyncTaskQueue closes after failure with pending tasks", "[async_task_queue]") {
	DuckDB db(nullptr);
	auto con = TaskQueueConnectionWithAsyncThreads(db, 1);
	ManagedAsyncTaskQueue queue(con->context->GetContext(), 1);

	std::atomic<idx_t> counter(0);
	ConcurrencyLatch latch;
	queue.Register(make_uniq<LatchThrowingTask>(latch), 16);
	queue.Register(make_uniq<CountingTask>(counter), 16);
	REQUIRE(latch.WaitForEntered(1));
	latch.Release();
	while (!queue.HasError()) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	try {
		queue.Close();
		FAIL("Expected async task failure");
	} catch (const Exception &ex) {
		string error = ex.what();
		REQUIRE(error.find("Injected async task failure") != string::npos);
	}
}
