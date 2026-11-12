#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/common/arrow/arrow_format.hpp"
#include "duckdb/common/arrow/arrow_wrapper.hpp"
#include "duckdb/common/set.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/execution/executor.hpp"
#include "duckdb/execution/operator/helper/physical_result_collector.hpp"
#include "duckdb/main/buffered_data/buffered_data.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/query_result_stream.hpp"
#include "duckdb/parallel/task.hpp"
#include "result_wait_helpers.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace duckdb;

namespace {

//! The consumer's side of the notify callback: it counts notifications and lets a waiter block until one arrives
//! that it has not seen yet
class NotifyChannel {
public:
	std::function<void()> Callback() {
		return [this]() {
			std::lock_guard<std::mutex> guard(lock);
			count++;
			callback_threads.insert(std::this_thread::get_id());
			signal.notify_all();
		};
	}

	QueryParameters Parameters(shared_ptr<ResultFormat> format = nullptr) {
		QueryParameters parameters(std::move(format));
		parameters.notify_callback = Callback();
		return parameters;
	}

	//! Waits for a notification after the last one seen. False when the deadline passed first
	bool Wait(idx_t &seen) {
		Deadline deadline;
		std::unique_lock<std::mutex> guard(lock);
		while (count == seen) {
			if (signal.wait_for(guard, std::chrono::milliseconds(50)) == std::cv_status::timeout && deadline.Passed()) {
				return false;
			}
		}
		seen = count;
		return true;
	}

	idx_t Count() {
		std::lock_guard<std::mutex> guard(lock);
		return count;
	}

	bool RanOn(std::thread::id thread) {
		std::lock_guard<std::mutex> guard(lock);
		return callback_threads.count(thread) > 0;
	}

private:
	std::mutex lock;
	std::condition_variable signal;
	idx_t count = 0;
	duckdb::set<std::thread::id> callback_threads;
};

unique_ptr<QueryResult> SubmitNotifying(Connection &con, const string &query, NotifyChannel &channel,
                                        shared_ptr<ResultFormat> format = nullptr) {
	auto handle = con.Submit(query, channel.Parameters(std::move(format)));
	if (handle->HasError()) {
		FAIL(handle->GetError());
	}
	return handle;
}

idx_t Rows(const DataChunk &chunk) {
	return chunk.size();
}

idx_t Rows(const ArrowArrayWrapper &array) {
	return NumericCast<idx_t>(array.arrow_array.length);
}

//! Drains a stream the way a consumer without a thread of its own does: it pops while units are ready and otherwise
//! sleeps until the next notification. Returns the terminal state
template <class FORMAT>
QueryResultState DrainByNotification(QueryResultStream<FORMAT> &stream, NotifyChannel &channel, idx_t &rows) {
	idx_t seen = 0;
	rows = 0;
	QueryResultState state = QueryResultState::NOT_READY;
	while (true) {
		unique_ptr<typename FORMAT::T> unit;
		state = stream.TryFetch(unit);
		if (state == QueryResultState::READY) {
			// READY always carries a unit
			REQUIRE(unit);
			rows += Rows(*unit);
			continue;
		}
		REQUIRE(!unit);
		if (IsTerminal(state)) {
			return state;
		}
		// Nothing observable: only a notification tells us to look again
		REQUIRE(channel.Wait(seen));
	}
}

//! Polls a handle the same way, until execution finished or failed
QueryResultState PollByNotification(QueryResult &handle, NotifyChannel &channel) {
	idx_t seen = 0;
	QueryResultState state;
	while (!IsTerminal(state = handle.Poll())) {
		REQUIRE(state != QueryResultState::READY);
		REQUIRE(channel.Wait(seen));
	}
	return state;
}

//! Fails every query as it begins
class ThrowingQueryBegin : public ClientContextState {
public:
	void QueryBegin(ClientContext &context) override {
		throw InvalidInputException("the query begin hook failed");
	}
};

//! A blocked task whose wake throws, as a reschedule that runs out of memory does
class FailingWake : public Task {
public:
	TaskExecutionResult Execute(TaskExecutionMode mode) override {
		return TaskExecutionResult::TASK_FINISHED;
	}
	void Reschedule() override {
		called = true;
		throw InvalidInputException("reschedule failed");
	}

public:
	bool called = false;
};

//! A consumer that only waits on notifications needs background workers, whatever the machine's core count
void UseWorkers(Connection &con) {
	REQUIRE_NO_FAIL(con.Query("SET threads=4"));
}

void RequireUsable(Connection &con) {
	auto next = con.Query("SELECT 42");
	REQUIRE(CHECK_COLUMN(next, 0, {42}));
}

} // namespace

#ifndef DUCKDB_NO_THREADS

//===--------------------------------------------------------------------===//
// The submission parameter
//===--------------------------------------------------------------------===//
TEST_CASE("Every submission path accepts a notify callback", "[api][query_result_notifier]") {
	DuckDB db(nullptr);
	Connection con(db);
	DrainWatchdog watchdog(con);
	const string query = "SELECT i FROM range(100000) t(i)";

	// Execution ends inside each of these calls, and its end always rings
	NotifyChannel channel;
	SECTION("Submit of a query string") {
		auto handle = SubmitNotifying(con, query, channel);
		REQUIRE(handle->RowCount() == 100000);
	}
	SECTION("Submit of a parsed statement") {
		auto statements = con.ExtractStatements(query);
		REQUIRE(statements.size() == 1);
		auto handle = con.Submit(std::move(statements[0]), channel.Parameters());
		REQUIRE(!handle->HasError());
		REQUIRE(handle->RowCount() == 100000);
	}
	SECTION("Submit of a prepared statement") {
		auto prepared = con.Prepare(query + " WHERE i >= $1");
		REQUIRE(!prepared->HasError());
		vector<Value> values {Value::BIGINT(0)};
		auto handle = prepared->Submit(values, channel.Parameters());
		REQUIRE(!handle->HasError());
		REQUIRE(handle->RowCount() == 100000);
	}
	SECTION("Query") {
		auto result = con.context->Query(query, channel.Parameters());
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->RowCount() == 100000);
	}
	REQUIRE(channel.Count() > 0);
	RequireUsable(con);
}

//===--------------------------------------------------------------------===//
// What rings
//===--------------------------------------------------------------------===//
// Sized for the default vector size: with tiny vectors these scans outlast the watchdog
#if STANDARD_VECTOR_SIZE == DEFAULT_STANDARD_VECTOR_SIZE
TEST_CASE("A consumer drains a stream waiting only on notifications", "[api][query_result_notifier]") {
	DuckDB db(nullptr);
	Connection con(db);
	UseWorkers(con);
	REQUIRE_NO_FAIL(con.Query("SET max_streaming_buffer_size='16KB'"));
	// Several row groups, so the table scan produces several batches for the batch-ordered store
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT range i FROM range(1000000)"));
	DrainWatchdog watchdog(con);

	// The unordered plan uses the simple store, the table scan the batched one
	for (auto query : {"SELECT i FROM range(1000000) t(i)", "SELECT i FROM t"}) {
		INFO(query);
		idx_t rows;
		{
			NotifyChannel channel;
			QueryResultStream<> stream(SubmitNotifying(con, query, channel));
			REQUIRE(DrainByNotification(stream, channel, rows) == QueryResultState::FINISHED);
			REQUIRE(rows == 1000000);
		}
		{
			NotifyChannel channel;
			QueryResultStream<ArrowFormat> stream(
			    SubmitNotifying(con, query, channel, make_shared_ptr<ArrowFormat>(1024)));
			REQUIRE(DrainByNotification(stream, channel, rows) == QueryResultState::FINISHED);
			REQUIRE(rows == 1000000);
		}
		RequireUsable(con);
	}
}

TEST_CASE("The end of execution wakes a consumer after the last unit", "[api][query_result_notifier]") {
	DuckDB db(nullptr);
	Connection con(db);
	UseWorkers(con);
	DrainWatchdog watchdog(con);

	// The filter stays above the range scan, which runs on long after the first rows without producing more, so the
	// wake that ends the drain can only be the end of execution. Enough rows that the filter emits them at once instead
	// of caching small chunks until the end
	NotifyChannel channel;
	QueryResultStream<> stream(SubmitNotifying(con, "SELECT i FROM range(100000000) t(i) WHERE i < 5000", channel));
	idx_t rows;
	REQUIRE(DrainByNotification(stream, channel, rows) == QueryResultState::FINISHED);
	REQUIRE(rows == 5000);
}
#endif

TEST_CASE("The undecided park rings once and only when the consumer can see it", "[api][query_result_notifier]") {
	DuckDB db(nullptr);
	Connection con(db);
	// One worker runs the range scan's single producer, which the callback holds
	REQUIRE_NO_FAIL(con.Query("SET threads=2"));
	DrainWatchdog watchdog(con);

	std::mutex lock;
	std::condition_variable signal;
	idx_t rings = 0;
	bool released = false;
	bool held_until_released = false;
	bool first_ring_returned = false;
	QueryParameters parameters;
	parameters.notify_callback = [&]() {
		std::unique_lock<std::mutex> guard(lock);
		if (++rings > 1) {
			return;
		}
		signal.notify_all();
		// Holds the parking worker between its park and the deschedule of its task, which no real callback may do
		held_until_released = signal.wait_for(guard, std::chrono::seconds(10), [&]() { return released; });
		first_ring_returned = true;
		signal.notify_all();
	};
	auto handle = con.Submit("SELECT i FROM range(500000) t(i)", parameters);
	REQUIRE(!handle->HasError());
	{
		std::unique_lock<std::mutex> guard(lock);
		REQUIRE(signal.wait_for(guard, std::chrono::seconds(60), [&]() { return rings > 0; }));
	}
	// The notification announced a state the consumer sees at once, although no task is descheduled yet
	REQUIRE(handle->Poll() == QueryResultState::READY);
	REQUIRE(handle->GetBufferedData().WaitsOnConsumer());
	REQUIRE(handle->GetBufferedData().Lifetime() == ResultLifetime::UNDECIDED);
	{
		std::unique_lock<std::mutex> guard(lock);
		released = true;
		signal.notify_all();
		REQUIRE(signal.wait_for(guard, std::chrono::seconds(60), [&]() { return first_ring_returned; }));
		// Otherwise the worker was let go before the poll, which then proved nothing
		REQUIRE(held_until_released);
	}

	QueryResultStream<> stream(std::move(handle));
	idx_t rows = 0;
	while (auto chunk = stream.Fetch()) {
		rows += chunk->size();
	}
	REQUIRE(!stream.HasError());
	REQUIRE(rows == 500000);
}

TEST_CASE("Later undecided parks do not ring", "[api][query_result_notifier]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=8"));
	// Several row groups, so several producers scan the table and park
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT range i FROM range(500000)"));
	DrainWatchdog watchdog(con);

	NotifyChannel channel;
	auto handle = SubmitNotifying(con, "SELECT i FROM t", channel);
	auto &buffer = handle->GetBufferedData();
	Deadline deadline;
	while (buffer.UndecidedParkCount() < 2) {
		REQUIRE(!deadline.Passed());
		std::this_thread::sleep_for(std::chrono::microseconds(100));
	}
	REQUIRE(handle->Poll() == QueryResultState::READY);
	// The first park rings after it registers, so wait for that ring rather than rely on how Close ends the query
	idx_t seen = 0;
	REQUIRE(channel.Wait(seen));
	// Closing drains every task before the query ends, so every park that registered has finished ringing. Until the
	// decision no unit is appended and execution cannot end, so nothing else can ring
	handle->Close();
	REQUIRE(channel.Count() == 1);
}

TEST_CASE("The end of a materialized result wakes a waiting consumer", "[api][query_result_notifier]") {
	DuckDB db(nullptr);
	Connection con(db);
	// Few workers, so execution is still running when the consumer first polls
	REQUIRE_NO_FAIL(con.Query("SET threads=2"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT range i FROM range(1000000)"));
	DrainWatchdog watchdog(con);

	for (auto query : {"SELECT i FROM range(1000000) t(i)", "SELECT i FROM t"}) {
		INFO(query);
		NotifyChannel channel;
		auto handle = SubmitNotifying(con, query, channel);
		// Retained rows never pass through the buffer, so only the end of execution rings
		handle->Materialize();
		REQUIRE(PollByNotification(*handle, channel) == QueryResultState::FINISHED);
		REQUIRE(handle->RowCount() == 1000000);
	}
	RequireUsable(con);
}

TEST_CASE("A worker error wakes a waiting consumer", "[api][query_result_notifier]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=2"));
	const auto rows = std::to_string(8 * STANDARD_VECTOR_SIZE);
	const auto boom = std::to_string(4 * STANDARD_VECTOR_SIZE);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT range i FROM range(" + rows + ")"));
	DrainWatchdog watchdog(con);

	const string cast = "SELECT (CASE WHEN i = " + boom + " THEN 'boom' ELSE i::VARCHAR END)::INT ";
	const vector<string> queries {cast + "FROM range(" + rows + ") t(i)", cast + "FROM t"};
	SECTION("a stream") {
		for (auto &query : queries) {
			INFO(query);
			NotifyChannel channel;
			QueryResultStream<> stream(SubmitNotifying(con, query, channel));
			idx_t drained;
			REQUIRE(DrainByNotification(stream, channel, drained) == QueryResultState::EXECUTION_ERROR);
			REQUIRE(StringUtil::Contains(stream.GetError(), "boom"));
			RequireUsable(con);
		}
	}
#if STANDARD_VECTOR_SIZE == DEFAULT_STANDARD_VECTOR_SIZE
	SECTION("a materialized result") {
		// The failing row comes late, so the consumer is waiting when it fails, and nothing else rings meanwhile
		NotifyChannel channel;
		auto handle = SubmitNotifying(
		    con, "SELECT sum(CASE WHEN i = 20000000 THEN error('boom') ELSE i END) FROM range(40000000) t(i)", channel);
		handle->Materialize();
		REQUIRE(PollByNotification(*handle, channel) == QueryResultState::EXECUTION_ERROR);
		REQUIRE(StringUtil::Contains(handle->GetError(), "boom"));
		RequireUsable(con);
	}
#endif
}

TEST_CASE("An interrupt a worker observes wakes a waiting consumer", "[api][query_result_notifier]") {
	DuckDB db(nullptr);
	Connection con(db);
	UseWorkers(con);

	NotifyChannel channel;
	auto handle = SubmitNotifying(con, "SELECT sum(i) FROM range(100000000000) t(i)", channel);
	handle->Materialize();
	idx_t seen = channel.Count();
	// Interrupt only sets a flag; the wake comes from the worker that runs into it
	con.Interrupt();
	REQUIRE(channel.Wait(seen));
	REQUIRE(PollByNotification(*handle, channel) == QueryResultState::EXECUTION_ERROR);
	REQUIRE(StringUtil::Contains(handle->GetError(), "INTERRUPT"));

	con.context->ClearInterrupt();
	RequireUsable(con);
}

TEST_CASE("A deadline a worker observes wakes a waiting consumer", "[api][query_result_notifier]") {
	DuckDB db(nullptr);
	Connection con(db);
	UseWorkers(con);
	REQUIRE_NO_FAIL(con.Query("SET max_execution_time=200"));
	DrainWatchdog watchdog(con);

	NotifyChannel channel;
	auto handle = SubmitNotifying(con, "SELECT sum(i) FROM range(100000000000) t(i)", channel);
	handle->Materialize();
	// A retained aggregate rings nothing before it ends, so the first ring is the error, whenever a worker ran into it
	idx_t seen = 0;
	REQUIRE(channel.Wait(seen));
	REQUIRE(handle->Poll() == QueryResultState::EXECUTION_ERROR);
	REQUIRE(StringUtil::Contains(handle->GetError(), "maximum execution time"));
	REQUIRE_NO_FAIL(con.Query("SET max_execution_time=0"));
	RequireUsable(con);
}

TEST_CASE("An error whose interruption fails still wakes a waiting consumer", "[api][query_result_notifier]") {
	DuckDB db(nullptr);
	Connection con(db);
	// No worker threads: every task runs inside this thread's ExecuteTask, so nothing else touches the result sink
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	optional_ptr<PhysicalOperator> sink;
	auto &config = ClientConfig::GetConfig(*con.context);
	ScopedConfigSetting setting(
	    config,
	    [&](ClientConfig &config) {
		    config.get_result_collector = [&](ClientContext &context, PreparedStatementData &data) {
			    auto collector = PhysicalResultCollector::GetResultCollector(context, data);
			    sink = collector.get();
			    return collector;
		    };
	    },
	    [](ClientConfig &config) { config.get_result_collector = nullptr; });

	NotifyChannel channel;
	auto handle = SubmitNotifying(con, "SELECT 42", channel);
	Deadline deadline;
	while (handle->ExecuteTask() != QueryResultState::READY) {
		REQUIRE(!deadline.Passed());
	}
	REQUIRE(sink);
	// Interrupting the pipelines wakes this task, and its wake throws
	auto failed_wake = make_shared_ptr<FailingWake>();
	{
		lock_guard<mutex> operator_guard(sink->lock);
		REQUIRE(sink->sink_state);
		annotated_lock_guard<annotated_mutex> state_guard(sink->sink_state->lock);
		InterruptState interrupt {weak_ptr<Task>(failed_wake)};
		REQUIRE(sink->sink_state->BlockTask(interrupt));
	}
	auto before = channel.Count();
	Executor::Get(*con.context).PushTaskError("boom");
	REQUIRE(failed_wake->called);
	REQUIRE(channel.Count() > before);
	REQUIRE(handle->Poll() == QueryResultState::EXECUTION_ERROR);
	REQUIRE(StringUtil::Contains(handle->GetError(), "boom"));
	RequireUsable(con);
}

TEST_CASE("A consumer that waits after a step that finished execution is woken", "[api][query_result_notifier]") {
	DuckDB db(nullptr);
	Connection con(db);
	// No worker threads: every task runs inside the consumer's own ExecuteTask, so every ring comes from its thread
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	DrainWatchdog watchdog(con);

	NotifyChannel channel;
	auto handle = SubmitNotifying(con, "SELECT i FROM range(100000) t(i)", channel);
	handle->Materialize();
	// A retained result rings only at the end of execution. The step that gets there reports NOT_READY, as every
	// step that ran a task does, and only its ring tells the consumer that a poll now finds the end
	Deadline deadline;
	while (channel.Count() == 0) {
		handle->ExecuteTask();
		REQUIRE(!deadline.Passed());
	}
	idx_t seen = 0;
	REQUIRE(channel.Wait(seen));
	REQUIRE(channel.RanOn(std::this_thread::get_id()));
	REQUIRE(handle->Poll() == QueryResultState::FINISHED);
	REQUIRE(handle->RowCount() == 100000);
}

//===--------------------------------------------------------------------===//
// A statement that expands into several
//===--------------------------------------------------------------------===//
TEST_CASE("A dynamic PIVOT streams to a consumer that waits only on notifications", "[api][query_result_notifier]") {
	DuckDB db(nullptr);
	Connection con(db);
	UseWorkers(con);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE sales(city VARCHAR, year INTEGER, amount INTEGER)"));
	REQUIRE_NO_FAIL(con.Query(
	    "INSERT INTO sales VALUES ('ams', 2023, 10), ('ams', 2024, 20), ('rtm', 2023, 30), ('rtm', 2024, 40)"));
	DrainWatchdog watchdog(con);

	// The type creation ends first; its end rings, and the consumer's next call starts the PIVOT itself
	NotifyChannel channel;
	QueryResultStream<> stream(SubmitNotifying(con, "PIVOT sales ON year USING sum(amount) ORDER BY city", channel));
	idx_t rows;
	REQUIRE(DrainByNotification(stream, channel, rows) == QueryResultState::FINISHED);
	REQUIRE(rows == 2);
	RequireUsable(con);
}

TEST_CASE("An ALTER that expands completes for a consumer that waits only on notifications",
          "[api][query_result_notifier]") {
	DuckDB db(nullptr);
	Connection con(db);
	UseWorkers(con);
	Connection observer(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT range AS i FROM range(10)"));
	DrainWatchdog watchdog(con);

	// An ALTER, an UPDATE and an ALTER: the end of each rings, and each poll starts the next
	NotifyChannel channel;
	auto handle = SubmitNotifying(con, "ALTER TABLE t ADD COLUMN c DOUBLE DEFAULT random()", channel);
	handle->Materialize();
	REQUIRE(PollByNotification(*handle, channel) == QueryResultState::FINISHED);
	handle->Complete();
	REQUIRE_NO_FAIL(*handle);
	auto nulls = observer.Query("SELECT count(*) FROM t WHERE c IS NULL");
	REQUIRE(CHECK_COLUMN(nulls, 0, {0}));
}

//===--------------------------------------------------------------------===//
// The callback's contract
//===--------------------------------------------------------------------===//
TEST_CASE("The queries statement verification runs do not ring the callback", "[api][query_result_notifier]") {
	DuckDB db(nullptr);
	Connection con(db);

	for (string verification : {"explain_statement", "prepared_statement"}) {
		INFO(verification);
		REQUIRE_NO_FAIL(con.Query("SET debug_verify_statement='" + verification + "'"));
		NotifyChannel channel;
		// Retained at submission, so the query's own execution rings once, at its end
		auto result = con.context->Query("SELECT 42", channel.Parameters());
		REQUIRE(CHECK_COLUMN(result, 0, {42}));
		REQUIRE(channel.Count() == 1);
	}
}

TEST_CASE("A throwing callback does not fail the query", "[api][query_result_notifier]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET max_streaming_buffer_size='16KB'"));
	DrainWatchdog watchdog(con);

	std::atomic<idx_t> calls {0};
	QueryParameters parameters;
	parameters.notify_callback = [&calls]() {
		calls++;
		throw std::runtime_error("the callback broke its contract");
	};
	auto handle = con.Submit("SELECT i FROM range(100000) t(i)", parameters);
	REQUIRE(!handle->HasError());
	QueryResultStream<> stream(std::move(handle));
	idx_t rows = 0;
	while (auto chunk = stream.Fetch()) {
		rows += chunk->size();
	}
	REQUIRE(!stream.HasError());
	REQUIRE(rows == 100000);
	REQUIRE(calls.load() > 0);
	RequireUsable(con);
}

//===--------------------------------------------------------------------===//
// The end of the query is a barrier
//===--------------------------------------------------------------------===//
TEST_CASE("A query that fails to begin keeps no copy of the callback", "[api][query_result_notifier]") {
	DuckDB db(nullptr);
	Connection con(db);

	auto captured = std::make_shared<int>(0);
	QueryParameters parameters;
	parameters.notify_callback = [captured]() {
	};
	// The debug build's state checks that every query end follows its own begin, which a begin hook that throws
	// breaks whichever hook runs first
	con.context->GetContext().GetRegisteredState().Remove("debug_client_context_state");
	con.context->GetContext().GetRegisteredState().Insert("throwing_query_begin",
	                                                      make_shared_ptr<ThrowingQueryBegin>());
	auto failed = con.Submit("SELECT 42", parameters);
	REQUIRE(failed->HasError());
	REQUIRE(StringUtil::Contains(failed->GetError(), "the query begin hook failed"));
	// Ours and the copy in the parameters: the engine holds none
	REQUIRE(captured.use_count() == 2);
	con.context->GetContext().GetRegisteredState().Remove("throwing_query_begin");
	RequireUsable(con);
}

TEST_CASE("The end of a query stops the callback", "[api][query_result_notifier]") {
	DuckDB db(nullptr);
	Connection con(db);
	UseWorkers(con);
	REQUIRE_NO_FAIL(con.Query("SET max_streaming_buffer_size='16KB'"));
	DrainWatchdog watchdog(con);

	auto end_while_ringing = [&](const std::function<void(QueryResultStream<> &)> &end_query) {
		std::mutex lock;
		std::condition_variable signal;
		bool armed = false;
		bool entered = false;
		bool ending = false;
		bool held_until_ending = false;
		bool returned = false;
		bool ended = false;
		bool ran_after_end = false;
		QueryParameters parameters;
		parameters.notify_callback = [&]() {
			std::unique_lock<std::mutex> guard(lock);
			if (ended) {
				ran_after_end = true;
			}
			if (!armed || entered) {
				return;
			}
			entered = true;
			signal.notify_all();
			// Held until the consumer is about to end the query, and then long enough to be running inside that call
			held_until_ending = signal.wait_for(guard, std::chrono::seconds(10), [&]() { return ending; });
			guard.unlock();
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			guard.lock();
			returned = true;
		};
		auto handle = con.Submit("SELECT i FROM range(1000000000) t(i)", parameters);
		REQUIRE(!handle->HasError());
		QueryResultStream<> stream(std::move(handle));
		{
			std::lock_guard<std::mutex> guard(lock);
			armed = true;
		}
		unique_ptr<DataChunk> chunk;
		Deadline deadline;
		// Popping empties the buffer, so a producer's next unit rings
		while (true) {
			{
				std::lock_guard<std::mutex> guard(lock);
				if (entered) {
					break;
				}
			}
			REQUIRE(!IsTerminal(stream.TryFetch(chunk)));
			REQUIRE(!deadline.Passed());
		}
		{
			std::lock_guard<std::mutex> guard(lock);
			ending = true;
		}
		signal.notify_all();
		end_query(stream);
		{
			std::lock_guard<std::mutex> guard(lock);
			// The callback was running when the ending call began, and that call returned only after it did
			REQUIRE(held_until_ending);
			REQUIRE(returned);
			ended = true;
		}
		// Room for a late ring, which must not come
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
		std::lock_guard<std::mutex> guard(lock);
		REQUIRE(!ran_after_end);
	};
	SECTION("Close") {
		end_while_ringing([](QueryResultStream<> &stream) { stream.Close(); });
	}
	SECTION("an interrupt the consumer observes") {
		end_while_ringing([&con](QueryResultStream<> &stream) {
			con.Interrupt();
			unique_ptr<DataChunk> chunk;
			Deadline deadline;
			QueryResultState state;
			while (!IsTerminal(state = stream.TryFetch(chunk))) {
				REQUIRE(!deadline.Passed());
			}
			REQUIRE(state == QueryResultState::EXECUTION_ERROR);
			con.context->ClearInterrupt();
		});
	}
	RequireUsable(con);
}

TEST_CASE("Each query on a connection gets its own notifier", "[api][query_result_notifier]") {
	DuckDB db(nullptr);
	Connection con(db);
	UseWorkers(con);
	REQUIRE_NO_FAIL(con.Query("SET max_streaming_buffer_size='16KB'"));
	DrainWatchdog watchdog(con);

	NotifyChannel first_channel;
	{
		QueryResultStream<> first(SubmitNotifying(con, "SELECT i FROM range(100000) t(i)", first_channel));
		unique_ptr<DataChunk> chunk;
		first.TryFetch(chunk);
		first.Close();
	}
	auto first_count = first_channel.Count();

	NotifyChannel second_channel;
	QueryResultStream<> second(SubmitNotifying(con, "SELECT i FROM range(100000) t(i)", second_channel));
	idx_t rows;
	REQUIRE(DrainByNotification(second, second_channel, rows) == QueryResultState::FINISHED);
	REQUIRE(rows == 100000);
	REQUIRE(second_channel.Count() > 0);
	REQUIRE(first_channel.Count() == first_count);
}

#endif
