#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/execution/executor.hpp"
#include "duckdb/execution/operator/helper/physical_result_collector.hpp"
#include "duckdb/main/buffered_data/buffered_data.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/appender.hpp"
#include "duckdb/main/prepared_statement_data.hpp"
#include "duckdb/main/query_result_stream.hpp"
#include "result_wait_helpers.hpp"
#include "test_result_format.hpp"

#include <chrono>
#include <thread>

using namespace duckdb;

namespace {

//! Submit a query, leaving the retention undecided
unique_ptr<QueryResult> Submit(Connection &con, const string &query, QueryParameters parameters = {}) {
	auto handle = con.Submit(query, std::move(parameters));
	if (handle->HasError()) {
		FAIL(handle->GetError());
	}
	return handle;
}

//! Submit a query and settle its result as retained, so that no producer waits for the consumer's choice
unique_ptr<QueryResult> SubmitRetained(Connection &con, const string &query) {
	auto handle = Submit(con, query);
	handle->Materialize();
	return handle;
}

idx_t DrainCursor(QueryResult &result) {
	idx_t rows = 0;
	while (auto chunk = result.Fetch()) {
		rows += chunk->size();
	}
	return rows;
}

//! Step a handle a few times, so its execution has started but has not finished
void StepUnfinished(QueryResult &handle) {
	for (idx_t step = 0; step < 5; step++) {
		REQUIRE(!IsTerminal(handle.ExecuteTask()));
	}
}

//! The error of a statement submitted while another result holds the connection
void RequireRefused(const ErrorData &error) {
	REQUIRE(error.HasError());
	REQUIRE(error.Type() == ExceptionType::RESOURCE_IN_USE);
	REQUIRE(StringUtil::Contains(error.Message(), "connection has an open result"));
}

QueryResultState StepToEnd(QueryResult &handle) {
	Deadline deadline;
	QueryResultState state;
	while (!IsTerminal(state = handle.ExecuteTask())) {
		REQUIRE(!deadline.Passed());
		if (state == QueryResultState::BLOCKED) {
			handle.WaitForTask();
		}
	}
	return state;
}

//! Stands in for an out-of-tree streaming collector: it builds its own result object and keeps the
//! query open, the combination no in-tree collector has
class TestCollectorState : public GlobalSinkState {
public:
	TestCollectorState(ClientContext &context, const vector<LogicalType> &types)
	    : collection(make_uniq<ColumnDataCollection>(Allocator::DefaultAllocator(), types)),
	      client_properties(context.GetClientProperties()) {
		collection->InitializeAppend(append_state);
	}

	unique_ptr<ColumnDataCollection> collection;
	ColumnDataAppendState append_state;
	ClientProperties client_properties;
};

class TestStreamingCollector : public PhysicalResultCollector {
public:
	TestStreamingCollector(PhysicalPlan &physical_plan, PreparedStatementData &data)
	    : PhysicalResultCollector(physical_plan, data) {
	}

public:
	bool IsStreaming() const override {
		return true;
	}

	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override {
		return make_uniq<TestCollectorState>(context, types);
	}

	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override {
		auto &gstate = input.global_state.Cast<TestCollectorState>();
		gstate.collection->Append(gstate.append_state, chunk);
		return SinkResultType::NEED_MORE_INPUT;
	}

	unique_ptr<QueryResult> GetResult(GlobalSinkState &state) const override {
		auto &gstate = state.Cast<TestCollectorState>();
		return make_uniq<QueryResult>(statement_type, properties, names, std::move(gstate.collection),
		                              gstate.client_properties);
	}
};

ScopedConfigSetting UseTestStreamingCollector(ClientConfig &config) {
	return ScopedConfigSetting(
	    config,
	    [](ClientConfig &config) {
		    config.get_result_collector = [](ClientContext &context,
		                                     PreparedStatementData &data) -> unique_ptr<PhysicalOperator> {
			    return make_uniq<TestStreamingCollector>(*data.physical_plan, data);
		    };
	    },
	    [](ClientConfig &config) { config.get_result_collector = nullptr; });
}

} // namespace

#ifndef DUCKDB_NO_THREADS

TEST_CASE("Query returns a completed retained handle", "[api][query_result]") {
	DuckDB db(nullptr);
	Connection con(db);

	auto result = con.Query("SELECT i FROM range(2000) t(i)");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->RowCount() == 2000);
	REQUIRE(result->Collection().GetValue(0, 0).GetValue<int64_t>() == 0);
	REQUIRE(result->Collection().GetValue(0, 1999).GetValue<int64_t>() == 1999);
	REQUIRE(result->Collection().Count() == 2000);
	REQUIRE(!result->ToString().empty());
	// The cursor walks the collection the handle already holds
	REQUIRE(DrainCursor(*result) == 2000);
	// Retention was settled at submission, so no producer ever parked
	REQUIRE(result->GetBufferedData().Lifetime() == ResultLifetime::RETAINED);
	REQUIRE(result->GetBufferedData().PeakBufferedBytes() == 0);
}

TEST_CASE("A submitted query parks for the consumer's choice", "[api][query_result]") {
	DuckDB db(nullptr);
	Connection con(db);

	auto handle = Submit(con, "SELECT i FROM range(500000) t(i)");

	Deadline deadline;
	while (handle->Poll() != QueryResultState::READY) {
		REQUIRE(!deadline.Passed());
		std::this_thread::sleep_for(std::chrono::microseconds(100));
	}
	// READY is the engine waiting on the consumer: a producer is parked with its first chunk
	REQUIRE(handle->GetBufferedData().WaitsOnConsumer());
	REQUIRE(handle->GetBufferedData().Lifetime() == ResultLifetime::UNDECIDED);
	REQUIRE(handle->GetBufferedData().PeakBufferedBytes() == 0);
}

TEST_CASE("ExecuteTask on a parked undecided handle reports READY and runs nothing", "[api][query_result]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));

	auto handle = Submit(con, "SELECT i FROM range(500000) t(i)");
	Deadline deadline;
	QueryResultState state;
	while ((state = handle->ExecuteTask()) != QueryResultState::READY) {
		REQUIRE(state == QueryResultState::NOT_READY);
		REQUIRE(!deadline.Passed());
	}
	auto &buffer = handle->GetBufferedData();
	REQUIRE(buffer.WaitsOnConsumer());
	REQUIRE(buffer.Lifetime() == ResultLifetime::UNDECIDED);
	// The engine waits for the consumer's choice: stepping again decides nothing and runs nothing
	REQUIRE(handle->ExecuteTask() == QueryResultState::READY);
	REQUIRE(handle->ExecuteTask() == QueryResultState::READY);
	REQUIRE(handle->Poll() == QueryResultState::READY);
	REQUIRE(buffer.Lifetime() == ResultLifetime::UNDECIDED);
	REQUIRE(buffer.PeakBufferedBytes() == 0);
	// The choice releases the park
	REQUIRE(handle->Collection().Count() == 500000);
}

TEST_CASE("Materialize returns at once and the workers complete the query", "[api][query_result]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT range i FROM range(200000)"));

	// The unordered plan uses the simple store, the table scan the batched one
	for (auto query : {"SELECT i FROM range(200000) t(i)", "SELECT i FROM t"}) {
		auto handle = Submit(con, query);

		handle->Materialize();
		// Nothing else is asked of the consumer: the workers run the query to completion
		Deadline deadline;
		while (handle->Poll() != QueryResultState::FINISHED) {
			REQUIRE(!deadline.Passed());
			std::this_thread::sleep_for(std::chrono::microseconds(100));
		}
		REQUIRE(handle->Collection().Count() == 200000);
		REQUIRE(handle->GetBufferedData().Lifetime() == ResultLifetime::RETAINED);
		REQUIRE(handle->GetBufferedData().PeakBufferedBytes() == 0);
		// Collecting finished the query, so the connection is free
		auto next = con.Query("SELECT 42");
		REQUIRE(CHECK_COLUMN(next, 0, {42}));
	}
}

TEST_CASE("Collecting a fresh submission takes the retained path", "[api][query_result]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET max_streaming_buffer_size='16KB'"));

	auto handle = Submit(con, "SELECT i FROM range(500000) t(i)");
	DrainWatchdog watchdog(con);
	auto &collection = handle->Collection();
	REQUIRE(collection.Count() == 500000);
	REQUIRE(handle->Collection().GetValue(0, 0).GetValue<int64_t>() == 0);
	REQUIRE(handle->Collection().GetValue(0, 499999).GetValue<int64_t>() == 499999);
	// Producers appended into the collection: nothing was ever staged in the streaming buffer
	REQUIRE(handle->GetBufferedData().Lifetime() == ResultLifetime::RETAINED);
	REQUIRE(handle->GetBufferedData().PeakBufferedBytes() == 0);
	// Collection is idempotent once retained
	REQUIRE(&handle->Collection() == &collection);
}

TEST_CASE("TakeCollection hands the collection over exactly once", "[api][query_result]") {
	DuckDB db(nullptr);
	Connection con(db);

	auto handle = Submit(con, "SELECT i FROM range(1000) t(i)");
	DrainWatchdog watchdog(con);
	auto collection = handle->TakeCollection();
	REQUIRE(collection);
	REQUIRE(collection->Count() == 1000);

	REQUIRE_THROWS_AS(handle->TakeCollection(), InvalidInputException);
	REQUIRE_THROWS_AS(handle->Collection(), InvalidInputException);
	REQUIRE_THROWS_AS(handle->Fetch(), InvalidInputException);
	// RowCount used to report 0 once the collection was taken, same as a result closed before it was
	// ever collected; it now throws so a taken result is distinguishable from an empty one
	REQUIRE_THROWS_AS(handle->RowCount(), InvalidInputException);
	// The collection outlives the handle it came from
	handle.reset();
	REQUIRE(collection->Count() == 1000);
}

TEST_CASE("RowCount throws for a result closed before it was ever collected", "[api][query_result]") {
	DuckDB db(nullptr);
	Connection con(db);

	auto handle = Submit(con, "SELECT i FROM range(1000) t(i)");
	// Closed without ever calling Collection, TakeCollection, Fetch or RowCount
	handle->Close();
	REQUIRE_THROWS_AS(handle->RowCount(), InvalidInputException);
}

TEST_CASE("Fetch resumes where it left off across a Collection call, and stays null after exhaustion",
          "[api][query_result]") {
	DuckDB db(nullptr);
	Connection con(db);

	auto handle = Submit(con, "SELECT i FROM range(5000) t(i)");
	DrainWatchdog watchdog(con);
	auto first = handle->Fetch();
	REQUIRE(first);
	auto first_rows = first->size();

	// Collection() does not disturb the Fetch cursor
	auto &collection = handle->Collection();
	REQUIRE(collection.Count() == 5000);

	idx_t remaining_rows = 0;
	while (auto chunk = handle->Fetch()) {
		remaining_rows += chunk->size();
	}
	REQUIRE(remaining_rows == 5000 - first_rows);
	REQUIRE(!handle->Fetch());
	REQUIRE(!handle->Fetch());
}

TEST_CASE("A retained chunk result completes with every row for unordered and batch-ordered plans",
          "[api][query_result]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=4"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT range i FROM range(400000)"));

	SECTION("unordered") {
		REQUIRE_NO_FAIL(con.Query("SET preserve_insertion_order=false"));
		auto handle = Submit(con, "SELECT i FROM t");
		DrainWatchdog watchdog(con);
		handle->Complete();
		REQUIRE(!handle->HasError());
		REQUIRE(handle->Collection().Count() == 400000);
		vector<int64_t> rows;
		for (auto &row : handle->Collection().Rows()) {
			rows.push_back(row.GetValue(0).GetValue<int64_t>());
		}
		std::sort(rows.begin(), rows.end());
		for (idx_t i = 0; i < rows.size(); i++) {
			REQUIRE(rows[i] == NumericCast<int64_t>(i));
		}
	}
	SECTION("batch ordered") {
		auto handle = Submit(con, "SELECT i FROM t");
		DrainWatchdog watchdog(con);
		handle->Complete();
		REQUIRE(!handle->HasError());
		REQUIRE(handle->Collection().Count() == 400000);
		idx_t i = 0;
		for (auto &row : handle->Collection().Rows()) {
			REQUIRE(row.GetValue(0).GetValue<int64_t>() == NumericCast<int64_t>(i));
			i++;
		}
	}
}

TEST_CASE("An execution error surfaces on every retained-side call", "[api][query_result]") {
	DuckDB db(nullptr);
	Connection con(db);

	// The cast fails at a late row, long after the submission succeeded
	auto handle =
	    Submit(con, "SELECT (CASE WHEN i = 40000 THEN 'boom' ELSE i::VARCHAR END)::INT FROM range(50000) t(i)");
	REQUIRE(!handle->HasError());

	handle->Materialize();
	Deadline deadline;
	while (handle->Poll() != QueryResultState::EXECUTION_ERROR) {
		REQUIRE(!deadline.Passed());
		std::this_thread::sleep_for(std::chrono::microseconds(100));
	}
	REQUIRE(handle->HasError());
	REQUIRE(StringUtil::Contains(handle->GetError(), "boom"));
	REQUIRE_THROWS_AS(handle->Collection(), InvalidInputException);
	// GetValue throws the query's own error, not an internal one
	bool threw_query_error = false;
	try {
		handle->Collection().GetValue(0, 0);
	} catch (const std::exception &ex) {
		threw_query_error = StringUtil::Contains(ErrorData(ex).Message(), "boom");
	}
	REQUIRE(threw_query_error);
	REQUIRE(handle->RowCount() == 0);

	auto next = con.Query("SELECT 42");
	REQUIRE(CHECK_COLUMN(next, 0, {42}));
}

TEST_CASE("Poll observes an interrupt on a materializing handle", "[api][query_result]") {
	DuckDB db(nullptr);
	Connection con(db);

	auto handle = Submit(con, "SELECT i FROM range(100000000000) t(i) WHERE i % 10 = 0");
	handle->Materialize();
	con.Interrupt();

	Deadline deadline;
	QueryResultState state;
	while (!IsTerminal(state = handle->Poll())) {
		REQUIRE(!deadline.Passed());
		std::this_thread::sleep_for(std::chrono::microseconds(100));
	}
	REQUIRE(state == QueryResultState::EXECUTION_ERROR);
	REQUIRE(StringUtil::Contains(handle->GetError(), "INTERRUPT"));

	con.context->ClearInterrupt();
	auto next = con.Query("SELECT 42");
	REQUIRE(CHECK_COLUMN(next, 0, {42}));
}

TEST_CASE("A side-effecting statement can be streamed and applies its effect on drain", "[api][query_result]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i INTEGER)"));

	for (auto query : {"INSERT INTO t VALUES (1), (2) RETURNING i", "CREATE TABLE ctas AS SELECT 42 AS i",
	                   "WITH ins AS (INSERT INTO t VALUES (3) RETURNING i) SELECT i FROM ins"}) {
		INFO(query);
		auto handle = Submit(con, query);
		// Nothing settles the retention at submission: the consumer's first call does
		REQUIRE(handle->GetBufferedData().Lifetime() == ResultLifetime::UNDECIDED);
		QueryResultStream<> stream(std::move(handle));
		DrainWatchdog watchdog(con);
		REQUIRE(stream.GetBufferedData().Lifetime() == ResultLifetime::DRAINING);
		while (auto chunk = stream.Fetch()) {
		}
		REQUIRE(!stream.HasError());
	}
	auto rows = con.Query("SELECT i FROM t ORDER BY i");
	REQUIRE(CHECK_COLUMN(rows, 0, {1, 2, 3}));
	auto created = con.Query("SELECT i FROM ctas");
	REQUIRE(CHECK_COLUMN(created, 0, {42}));
}

TEST_CASE("A DDL submission finishes without a consumer call", "[api][query_result]") {
	DuckDB db(nullptr);
	Connection con(db);
	Connection observer(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=2"));

	// No producer waits for a choice, since the statement produces no rows
	auto handle = Submit(con, "CREATE TABLE t(i BIGINT)");
	Deadline deadline;
	while (handle->Poll() != QueryResultState::FINISHED) {
		REQUIRE(!deadline.Passed());
		std::this_thread::sleep_for(std::chrono::microseconds(100));
	}
	REQUIRE(handle->GetBufferedData().Lifetime() == ResultLifetime::UNDECIDED);
	REQUIRE(handle->ExecuteTask() == QueryResultState::FINISHED);
	handle->Close();

	auto count = observer.Query("SELECT count(*) FROM t");
	REQUIRE(CHECK_COLUMN(count, 0, {0}));
}

TEST_CASE("A submitted insert parks until the consumer chooses", "[api][query_result]") {
	DuckDB db(nullptr);
	Connection con(db);
	Connection observer(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=2"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i BIGINT)"));

	auto handle = Submit(con, "INSERT INTO t VALUES (1)");
	Deadline deadline;
	auto state = handle->Poll();
	while (state != QueryResultState::READY && !IsTerminal(state)) {
		REQUIRE(!deadline.Passed());
		std::this_thread::sleep_for(std::chrono::microseconds(100));
		state = handle->Poll();
	}
	// The row count parks for the consumer's choice, so the workers cannot finish the query
	REQUIRE(state == QueryResultState::READY);
	REQUIRE(handle->GetBufferedData().Lifetime() == ResultLifetime::UNDECIDED);
	auto empty = observer.Query("SELECT count(*) FROM t");
	REQUIRE(CHECK_COLUMN(empty, 0, {0}));

	handle->Materialize();
	REQUIRE(StepToEnd(*handle) == QueryResultState::FINISHED);
	auto rows = observer.Query("SELECT i FROM t");
	REQUIRE(CHECK_COLUMN(rows, 0, {1}));
}

TEST_CASE("A parked insert holds the connection until its consumer settles it", "[api][query_result]") {
	for (bool in_transaction : {false, true}) {
		for (bool complete : {true, false}) {
			INFO("in a transaction: " << in_transaction << ", completed: " << complete);
			DuckDB db(nullptr);
			Connection con(db);
			Connection observer(db);
			REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i BIGINT)"));
			if (in_transaction) {
				REQUIRE_NO_FAIL(con.Query("BEGIN TRANSACTION"));
			}

			auto handle = Submit(con, "INSERT INTO t VALUES (1)");
			REQUIRE(handle->GetBufferedData().Lifetime() == ResultLifetime::UNDECIDED);
			auto refused = con.Query("SELECT 42");
			RequireRefused(refused->GetErrorObject());
			if (complete) {
				handle->Complete();
				REQUIRE(!handle->HasError());
				REQUIRE(handle->Fetch()->GetValue(0, 0).GetValue<int64_t>() == 1);
				REQUIRE_NO_FAIL(con.Query("SELECT 42"));
			} else {
				handle.reset();
				auto next = con.Query("SELECT 42");
				// An abandoned write leaves nothing in the transaction that may commit
				REQUIRE(next->HasError() == in_transaction);
			}
			if (in_transaction) {
				REQUIRE_NO_FAIL(con.Query(complete ? "COMMIT" : "ROLLBACK"));
			}
			auto count = observer.Query("SELECT count(*) FROM t");
			REQUIRE(CHECK_COLUMN(count, 0, {complete ? 1 : 0}));
		}
	}
}

TEST_CASE("Multi-statement text chains completed results", "[api][query_result]") {
	DuckDB db(nullptr);
	Connection con(db);

	auto result = con.Query("CREATE TABLE t AS SELECT 42 AS i; SELECT i FROM t; SELECT 84 AS i;");
	REQUIRE_NO_FAIL(*result);
	// Every statement of the chain ran to completion and kept its result
	REQUIRE(CHECK_COLUMN(result, 0, {42}));
	REQUIRE(result->next);
	auto &last = *result->next;
	REQUIRE(CHECK_COLUMN(last, 0, {84}));
	REQUIRE(!last.next);
	REQUIRE(last.RowCount() == 1);

	// A submission takes a single statement, and so does a parameterized eager query
	auto handle = con.Submit("SELECT 1; SELECT 2;");
	REQUIRE(handle->HasError());
	REQUIRE_FAIL(con.Query("SELECT $1; SELECT $1;", 1));
	auto single = con.Query("SELECT $1::INT", 7);
	REQUIRE(CHECK_COLUMN(single, 0, {7}));
}

TEST_CASE("A prepared statement gets a fresh store on every submission", "[api][query_result]") {
	DuckDB db(nullptr);
	Connection con(db);

	auto prepared = con.Prepare("SELECT i FROM range($1) t(i)");
	REQUIRE(!prepared->HasError());

	DrainWatchdog watchdog(con);
	auto first = prepared->Submit(1000);
	auto &first_buffer = first->GetBufferedData();
	REQUIRE(first->Collection().Count() == 1000);

	auto second = prepared->Submit(2000);
	REQUIRE(&second->GetBufferedData() != &first_buffer);
	REQUIRE(second->Collection().Count() == 2000);
	// The first result is still readable: it holds its own collection
	REQUIRE(first->Collection().Count() == 1000);
}

TEST_CASE("A custom collector hands out its own result object", "[api][query_result]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &config = ClientConfig::GetConfig(*con.context);
	DrainWatchdog watchdog(con);

	SECTION("a streaming collector keeps the query open until the next statement") {
		auto setting = UseTestStreamingCollector(config);
		auto result = con.Submit("SELECT i FROM range(3000) t(i)");
		REQUIRE(!result->HasError());
		REQUIRE(result->RowCount() == 3000);
		// Nothing can end the query of a result the collector built, so the next statement abandons it
		auto while_open = con.Query("SELECT 42");
		REQUIRE(CHECK_COLUMN(while_open, 0, {42}));
		REQUIRE(result->RowCount() == 3000);
	}
	auto next = con.Query("SELECT 42");
	REQUIRE(CHECK_COLUMN(next, 0, {42}));
}

TEST_CASE("A custom collector refuses a submission that asks for a format", "[api][query_result]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &config = ClientConfig::GetConfig(*con.context);
	DrainWatchdog watchdog(con);

	auto refuse = [&](shared_ptr<ResultFormat> format, const char *expected) {
		auto setting = UseTestStreamingCollector(config);
		auto refused = con.Submit("SELECT i FROM range(1000) t(i)", std::move(format));
		REQUIRE(refused->HasError());
		REQUIRE(refused->GetErrorType() == ExceptionType::INVALID_INPUT);
		REQUIRE(StringUtil::Contains(refused->GetError(), expected));
	};
	SECTION("a format that is not the chunk format") {
		refuse(make_shared_ptr<TestFormat>(1024), "A result format cannot be combined with a custom result collector");
	}
	SECTION("the buffer-managed chunk format") {
		refuse(ChunkFormat::BufferManaged(),
		       "A buffer-managed result cannot be combined with a custom result collector");
	}
	auto next = con.Query("SELECT 42");
	REQUIRE(CHECK_COLUMN(next, 0, {42}));
}

TEST_CASE("A collector hook that hands back the default sink accepts a format", "[api][query_result]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &config = ClientConfig::GetConfig(*con.context);
	DrainWatchdog watchdog(con);

	ScopedConfigSetting setting(
	    config, [](ClientConfig &config) { config.get_result_collector = PhysicalResultCollector::GetResultCollector; },
	    [](ClientConfig &config) { config.get_result_collector = nullptr; });

	QueryParameters parameters;
	parameters.format = make_shared_ptr<TestFormat>(4096);
	auto formatted = con.context->Query("SELECT i FROM range(20000) t(i)", parameters);
	REQUIRE_NO_FAIL(*formatted);
	REQUIRE(formatted->RowCount() == 20000);

	parameters.format = ChunkFormat::BufferManaged();
	auto buffered = con.Submit("SELECT i FROM range(1000) t(i)", parameters);
	REQUIRE(!buffered->HasError());
	REQUIRE(buffered->RowCount() == 1000);

	auto next = con.Query("SELECT 42");
	REQUIRE(CHECK_COLUMN(next, 0, {42}));
}

TEST_CASE("A custom collector refuses a submission that asks for a buffer-managed result", "[api][query_result]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &config = ClientConfig::GetConfig(*con.context);
	DrainWatchdog watchdog(con);

	QueryParameters parameters;
	{
		auto setting = UseTestStreamingCollector(config);
		parameters.format = ChunkFormat::BufferManaged();
		auto refused = con.Submit("SELECT i FROM range(1000) t(i)", parameters);
		REQUIRE(refused->HasError());
		REQUIRE(refused->GetErrorType() == ExceptionType::INVALID_INPUT);
		REQUIRE(StringUtil::Contains(refused->GetError(), "buffer-managed result cannot be combined"));

		// The in-memory chunk format is the store the collector builds anyway
		parameters.format = ChunkFormat::InMemory();
		auto accepted = con.Submit("SELECT i FROM range(1000) t(i)", parameters);
		REQUIRE(!accepted->HasError());
		REQUIRE(accepted->RowCount() == 1000);
	}
	auto next = con.Query("SELECT 42");
	REQUIRE(CHECK_COLUMN(next, 0, {42}));
}

TEST_CASE("A handle destroyed without collecting releases the query", "[api][query_result]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET max_streaming_buffer_size='16KB'"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE fanout AS SELECT range i FROM range(200000)"));

	SECTION("a plain submission") {
		auto handle = Submit(con, "SELECT i FROM range(1000000) t(i)");
		REQUIRE(con.context->transaction.HasActiveTransaction());
		handle.reset();
		REQUIRE(!con.context->transaction.HasActiveTransaction());
	}
	SECTION("a fan-out plan with parked producers") {
		// One scan feeds two consumers: dropping the handle must unwind the parked producers
		auto handle = Submit(con, "WITH c AS MATERIALIZED (SELECT i FROM fanout) "
		                          "SELECT t1.i FROM c t1 JOIN c t2 USING (i)");
		Deadline deadline;
		while (!handle->GetBufferedData().WaitsOnConsumer()) {
			REQUIRE(!deadline.Passed());
			std::this_thread::sleep_for(std::chrono::microseconds(100));
		}
		handle.reset();
	}
	auto next = con.Query("SELECT 42");
	REQUIRE(CHECK_COLUMN(next, 0, {42}));
}

TEST_CASE("An unfinished insert that is closed or destroyed leaves no rows", "[api][query_result]") {
	DuckDB db(nullptr);
	Connection con(db);
	Connection observer(db);
	// Without worker threads the insert only advances when this thread steps it
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i BIGINT)"));

	auto handle = Submit(con, "INSERT INTO t SELECT i FROM range(1000000) t(i)");
	StepUnfinished(*handle);
	SECTION("closed") {
		handle->Close();
	}
	SECTION("destroyed") {
		handle.reset();
	}

	auto count = observer.Query("SELECT count(*) FROM t");
	REQUIRE(CHECK_COLUMN(count, 0, {0}));
	auto next = con.Query("SELECT 42");
	REQUIRE(CHECK_COLUMN(next, 0, {42}));
}

#if STANDARD_VECTOR_SIZE >= 512
TEST_CASE("An insert whose worker failed unobserved leaves no rows when closed", "[api][query_result]") {
	DuckDB db(nullptr);
	Connection con(db);
	Connection observer(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=2"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i BIGINT)"));

	// The failing row is deep into the input, so the worker appends rows before it fails
	auto handle = Submit(con, "INSERT INTO t SELECT CASE WHEN i = 200000 THEN error('boom') ELSE i END "
	                          "FROM range(1000000) t(i)");

	auto &executor = Executor::Get(*con.context);
	Deadline deadline;
	while (!executor.HasError()) {
		REQUIRE(!deadline.Passed());
		std::this_thread::sleep_for(std::chrono::microseconds(100));
	}
	// Nothing observed the failure, so it is on the executor alone
	REQUIRE(!handle->HasError());
	handle->Close();

	auto count = observer.Query("SELECT count(*) FROM t");
	REQUIRE(CHECK_COLUMN(count, 0, {0}));
}
#endif

TEST_CASE("An unfinished insert closed inside a transaction invalidates it", "[api][query_result]") {
	DuckDB db(nullptr);
	Connection con(db);
	Connection observer(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i BIGINT)"));
	REQUIRE_NO_FAIL(con.Query("BEGIN TRANSACTION"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t VALUES (42)"));

	auto handle = Submit(con, "INSERT INTO t SELECT i FROM range(1000000) t(i)");
	StepUnfinished(*handle);
	handle->Close();

	auto next = con.Query("SELECT 42");
	REQUIRE(next->HasError());
	REQUIRE(StringUtil::Contains(next->GetError(), "aborted"));
	REQUIRE_NO_FAIL(con.Query("ROLLBACK"));
	auto count = observer.Query("SELECT count(*) FROM t");
	REQUIRE(CHECK_COLUMN(count, 0, {0}));
	auto after = con.Query("SELECT 42");
	REQUIRE(CHECK_COLUMN(after, 0, {42}));
}

TEST_CASE("Every statement entry point is refused while a result is open", "[api][query_result]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT range i FROM range(1000000)"));
	auto prepared = con.Prepare("SELECT 42");
	REQUIRE(!prepared->HasError());
	auto relation = con.Table("t");
	auto statements = con.ExtractStatements("SELECT 42; SELECT 42; SELECT 42;");

	auto handle = Submit(con, "SELECT i FROM t");
	StepUnfinished(*handle);

	RequireRefused(con.Query("SELECT 42")->GetErrorObject());
	RequireRefused(con.Query(std::move(statements[0]))->GetErrorObject());
	RequireRefused(con.Submit("SELECT 42")->GetErrorObject());
	RequireRefused(con.Submit(std::move(statements[1]))->GetErrorObject());
	RequireRefused(con.Prepare("SELECT 42")->GetErrorObject());
	RequireRefused(con.Prepare(std::move(statements[2]))->GetErrorObject());
	vector<Value> no_values;
	RequireRefused(prepared->Execute()->GetErrorObject());
	RequireRefused(prepared->Submit(no_values)->GetErrorObject());
	RequireRefused(relation->Execute()->GetErrorObject());
	RequireRefused(con.context->Submit(relation, QueryParameters())->GetErrorObject());
	// The refusal comes before parsing
	RequireRefused(con.Query("SELEC 42")->GetErrorObject());
	RequireRefused(con.Query("COMMIT")->GetErrorObject());
	RequireRefused(con.Query("ROLLBACK")->GetErrorObject());
	REQUIRE_THROWS_WITH(con.BeginTransaction(), Catch::Contains("connection has an open result"));
	REQUIRE_THROWS_WITH(con.Commit(), Catch::Contains("connection has an open result"));
	REQUIRE_THROWS_WITH(con.Rollback(), Catch::Contains("connection has an open result"));

	// None of the refusals touched the open result
	handle->Complete();
	REQUIRE_NO_FAIL(*handle);
	REQUIRE(handle->RowCount() == 1000000);
	auto next = con.Query("SELECT 42");
	REQUIRE(CHECK_COLUMN(next, 0, {42}));
}

TEST_CASE("A statement refused inside a transaction leaves the transaction intact", "[api][query_result]") {
	DuckDB db(nullptr);
	Connection con(db);
	Connection observer(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i BIGINT)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t VALUES (42)"));
	REQUIRE_NO_FAIL(con.Query("BEGIN TRANSACTION"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t VALUES (43)"));

	idx_t expected = 2;
	SECTION("a write, then a query") {
		auto handle = Submit(con, "INSERT INTO t SELECT i FROM range(1000000) t(i)");
		StepUnfinished(*handle);
		RequireRefused(con.Query("SELECT 42")->GetErrorObject());
		handle->Complete();
		REQUIRE_NO_FAIL(*handle);
		expected += 1000000;
	}
	SECTION("a write, then COMMIT") {
		auto handle = Submit(con, "INSERT INTO t SELECT i FROM range(1000000) t(i)");
		StepUnfinished(*handle);
		RequireRefused(con.Query("COMMIT")->GetErrorObject());
		REQUIRE_THROWS_WITH(con.Commit(), Catch::Contains("connection has an open result"));
		handle->Complete();
		REQUIRE_NO_FAIL(*handle);
		expected += 1000000;
	}
	SECTION("a read-only stream, then COMMIT") {
		auto handle = Submit(con, "SELECT i FROM range(1000000) t(i)");
		StepUnfinished(*handle);
		RequireRefused(con.Query("COMMIT")->GetErrorObject());
		QueryResultStream<> stream(std::move(handle));
		REQUIRE(DrainStream(stream)->RowCount() == 1000000);
	}
	REQUIRE_NO_FAIL(con.Query("COMMIT"));
	auto count = observer.Query("SELECT count(*) FROM t");
	REQUIRE(CHECK_COLUMN(count, 0, {Value::BIGINT(NumericCast<int64_t>(expected))}));
}

TEST_CASE("A statement refused in autocommit leaves the open write to commit", "[api][query_result]") {
	DuckDB db(nullptr);
	Connection con(db);
	Connection observer(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i BIGINT)"));

	auto handle = Submit(con, "INSERT INTO t SELECT i FROM range(1000000) t(i)");
	StepUnfinished(*handle);
	RequireRefused(con.Query("SELECT 42")->GetErrorObject());
	auto during = observer.Query("SELECT count(*) FROM t");
	REQUIRE(CHECK_COLUMN(during, 0, {0}));

	handle->Complete();
	REQUIRE_NO_FAIL(*handle);
	auto after = observer.Query("SELECT count(*) FROM t");
	REQUIRE(CHECK_COLUMN(after, 0, {1000000}));
}

TEST_CASE("Every way a result ends frees the connection", "[api][query_result]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i BIGINT)"));
	DrainWatchdog watchdog(con);

	// Kept alive across the next statement, so that ending the result, not destroying it, frees the connection
	unique_ptr<QueryResult> handle;
	unique_ptr<QueryResultStream<>> stream;
	SECTION("a stream drained to the end") {
		stream = make_uniq<QueryResultStream<>>(Submit(con, "SELECT i FROM range(100000) t(i)"));
		REQUIRE(DrainStream(*stream)->RowCount() == 100000);
	}
	SECTION("a completed result") {
		handle = Submit(con, "SELECT i FROM range(100000) t(i)");
		handle->Complete();
		REQUIRE_NO_FAIL(*handle);
	}
	SECTION("a write stepped to FINISHED") {
		handle = SubmitRetained(con, "INSERT INTO t SELECT i FROM range(100000) t(i)");
		REQUIRE(StepToEnd(*handle) == QueryResultState::FINISHED);
	}
	SECTION("a failure found while binding") {
		handle = con.Submit("SELECT * FROM no_such_table");
		REQUIRE(handle->HasError());
	}
	SECTION("a failure found while running") {
		handle =
		    Submit(con, "SELECT (CASE WHEN i = 90000 THEN 'boom' ELSE i::VARCHAR END)::INT FROM range(100000) t(i)");
		handle->Complete();
		REQUIRE(handle->HasError());
	}
	SECTION("a closed result") {
		handle = Submit(con, "SELECT i FROM range(1000000) t(i)");
		StepUnfinished(*handle);
		handle->Close();
	}
	SECTION("a destroyed result") {
		handle = Submit(con, "SELECT i FROM range(1000000) t(i)");
		StepUnfinished(*handle);
		handle.reset();
	}
	auto next = con.Query("SELECT 42");
	REQUIRE(CHECK_COLUMN(next, 0, {42}));
}

TEST_CASE("A result polled to FINISHED holds the connection until a call ends it", "[api][query_result]") {
	DuckDB db(nullptr);
	Connection con(db);
	// Workers run the statement, since polling executes nothing
	REQUIRE_NO_FAIL(con.Query("SET threads=2"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i BIGINT)"));

	auto handle = SubmitRetained(con, "INSERT INTO t SELECT i FROM range(1000) t(i)");
	Deadline deadline;
	while (handle->Poll() != QueryResultState::FINISHED) {
		REQUIRE(!deadline.Passed());
		std::this_thread::sleep_for(std::chrono::microseconds(100));
	}
	RequireRefused(con.Query("SELECT 42")->GetErrorObject());

	handle->Complete();
	REQUIRE_NO_FAIL(*handle);
	auto count = con.Query("SELECT count(*) FROM t");
	REQUIRE(CHECK_COLUMN(count, 0, {1000}));
}

TEST_CASE("An interrupted result holds the connection until its next call", "[api][query_result]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));

	auto handle = Submit(con, "SELECT i FROM range(1000000) t(i)");
	StepUnfinished(*handle);
	con.Interrupt();
	RequireRefused(con.Query("SELECT 42")->GetErrorObject());

	// The refusal leaves the interrupt pending for the open result
	REQUIRE(handle->ExecuteTask() == QueryResultState::EXECUTION_ERROR);
	REQUIRE(handle->GetErrorType() == ExceptionType::INTERRUPT);
	auto next = con.Query("SELECT 42");
	REQUIRE(CHECK_COLUMN(next, 0, {42}));
}

//! Fails the begin of the next query once, after the query is already active
class FailingQueryBegin : public ClientContextState {
public:
	void QueryBegin(ClientContext &context) override {
		if (fail) {
			fail = false;
			throw IOException("query begin failed");
		}
	}

	bool fail = true;
};

TEST_CASE("A query whose begin failed does not hold the connection", "[api][query_result]") {
	DuckDB db(nullptr);
	Connection con(db);
	// The debug build's state checks that every query end follows its own begin, which a begin hook that throws
	// breaks whichever hook runs first
	con.context->registered_state->Remove("debug_client_context_state");
	con.context->registered_state->Insert("failing_query_begin", make_shared_ptr<FailingQueryBegin>());

	auto failed = con.Query("SELECT 42");
	REQUIRE(failed->HasError());
	REQUIRE(StringUtil::Contains(failed->GetError(), "query begin failed"));
	con.context->registered_state->Remove("failing_query_begin");

	auto next = con.Query("SELECT 42");
	REQUIRE(CHECK_COLUMN(next, 0, {42}));
}

TEST_CASE("An appender is refused while a result is open", "[api][query_result]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE u(i BIGINT)"));

	auto handle = Submit(con, "SELECT i FROM range(1000000) t(i)");
	StepUnfinished(*handle);
	{
		Appender appender(con, "u");
		appender.AppendRow(Value::BIGINT(1));
		REQUIRE_THROWS_WITH(appender.Close(), Catch::Contains("connection has an open result"));
	}
	handle->Close();

	Appender appender(con, "u");
	appender.AppendRow(Value::BIGINT(1));
	appender.Close();
	auto count = con.Query("SELECT count(*) FROM u");
	REQUIRE(CHECK_COLUMN(count, 0, {1}));
}

TEST_CASE("Cancelling the transaction abandons an open result", "[api][query_result]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i BIGINT)"));

	auto handle = Submit(con, "INSERT INTO t SELECT i FROM range(1000000) t(i)");
	StepUnfinished(*handle);
	con.context->CancelTransaction();
	REQUIRE_THROWS(handle->ExecuteTask());

	auto count = con.Query("SELECT count(*) FROM t");
	REQUIRE(CHECK_COLUMN(count, 0, {0}));
}

TEST_CASE("An unfinished statement that expands into several rolls back as a whole when closed",
          "[api][query_result]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT range i FROM range(1000000)"));

	// A volatile default expands into ADD COLUMN, UPDATE and SET DEFAULT, which run in the query's own transaction
	auto handle = con.Submit("ALTER TABLE t ADD COLUMN c DOUBLE DEFAULT random()");
	REQUIRE(!handle->HasError());
	StepUnfinished(*handle);
	handle->Close();

	// The column the group added went with it
	REQUIRE(!con.context->transaction.HasActiveTransaction());
	REQUIRE_FAIL(con.Query("SELECT c FROM t"));
	auto count = con.Query("SELECT count(*) FROM t");
	REQUIRE(CHECK_COLUMN(count, 0, {1000000}));
}

TEST_CASE("An insert that finished executing but that no call ended leaves no rows when closed",
          "[api][query_result]") {
	DuckDB db(nullptr);
	Connection con(db);
	Connection observer(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=2"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i BIGINT)"));

	auto handle = SubmitRetained(con, "INSERT INTO t SELECT i FROM range(1000) t(i)");
	REQUIRE(WaitForExecution(con));
	handle->Close();

	auto count = observer.Query("SELECT count(*) FROM t");
	REQUIRE(CHECK_COLUMN(count, 0, {0}));
}

TEST_CASE("Polling an insert to FINISHED does not end it", "[api][query_result]") {
	DuckDB db(nullptr);
	Connection con(db);
	Connection observer(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=2"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i BIGINT)"));

	auto handle = SubmitRetained(con, "INSERT INTO t SELECT i FROM range(1000) t(i)");
	Deadline deadline;
	while (handle->Poll() != QueryResultState::FINISHED) {
		REQUIRE(!deadline.Passed());
		std::this_thread::sleep_for(std::chrono::microseconds(100));
	}
	REQUIRE(handle->IsOpen());
	handle->Close();

	auto count = observer.Query("SELECT count(*) FROM t");
	REQUIRE(CHECK_COLUMN(count, 0, {0}));
}

TEST_CASE("Stepping an insert to FINISHED commits it", "[api][query_result]") {
	DuckDB db(nullptr);
	Connection con(db);
	Connection observer(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i BIGINT)"));

	auto handle = SubmitRetained(con, "INSERT INTO t SELECT i FROM range(1000) t(i)");
	REQUIRE(StepToEnd(*handle) == QueryResultState::FINISHED);

	// The step that reported FINISHED ended the query, before any other call on the handle
	auto count = observer.Query("SELECT count(*) FROM t");
	REQUIRE(CHECK_COLUMN(count, 0, {1000}));
	REQUIRE(!handle->IsOpen());
	REQUIRE(!con.context->transaction.HasActiveTransaction());
	REQUIRE(handle->ExecuteTask() == QueryResultState::FINISHED);
	REQUIRE(handle->Poll() == QueryResultState::FINISHED);
	REQUIRE(CHECK_COLUMN(*handle, 0, {1000}));

	handle.reset();
	count = observer.Query("SELECT count(*) FROM t");
	REQUIRE(CHECK_COLUMN(count, 0, {1000}));
}

TEST_CASE("Stepping a zero-row stream to FINISHED ends it", "[api][query_result]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	REQUIRE_NO_FAIL(con.Query("CREATE SEQUENCE s"));
	// No row is sunk, so no producer waits for the retention decision and the engine finishes undecided
	const string query = "SELECT n FROM (SELECT nextval('s') n FROM range(5)) q WHERE n > 100";

	SECTION("on autocommit the step commits") {
		auto handle = Submit(con, query);
		REQUIRE(StepToEnd(*handle) == QueryResultState::FINISHED);
		REQUIRE(!handle->IsOpen());
		REQUIRE(!con.context->transaction.HasActiveTransaction());
		REQUIRE(handle->ExecuteTask() == QueryResultState::FINISHED);
		REQUIRE(handle->RowCount() == 0);
	}
	SECTION("inside a transaction closing it afterwards keeps the transaction") {
		REQUIRE_NO_FAIL(con.Query("BEGIN TRANSACTION"));
		auto handle = Submit(con, query);
		REQUIRE(StepToEnd(*handle) == QueryResultState::FINISHED);
		handle->Close();
		REQUIRE_NO_FAIL(con.Query("SELECT 42"));
		REQUIRE_NO_FAIL(con.Query("COMMIT"));
	}
	SECTION("a stream opened afterwards reports the end at once") {
		auto handle = Submit(con, query);
		REQUIRE(StepToEnd(*handle) == QueryResultState::FINISHED);
		QueryResultStream<> stream(std::move(handle));
		REQUIRE(!stream.Fetch());
		REQUIRE(!stream.HasError());
	}
}

TEST_CASE("A commit that fails when a step ends the query is reported by that step", "[api][query_result]") {
	DuckDB db(nullptr);
	Connection con(db);
	Connection other(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=2"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i INTEGER PRIMARY KEY)"));

	auto handle = SubmitRetained(con, "INSERT INTO t VALUES (1)");
	REQUIRE(WaitForExecution(con));
	// The key is only in this handle's transaction, so the other connection commits it first
	REQUIRE_NO_FAIL(other.Query("INSERT INTO t VALUES (1)"));

	REQUIRE(handle->ExecuteTask() == QueryResultState::EXECUTION_ERROR);
	REQUIRE(handle->HasError());
	REQUIRE(StringUtil::Contains(handle->GetError(), "Failed to commit"));
	REQUIRE(StringUtil::Contains(handle->GetError(), "duplicate key"));
	REQUIRE(!con.context->transaction.HasActiveTransaction());
	auto count = con.Query("SELECT count(*) FROM t");
	REQUIRE(CHECK_COLUMN(count, 0, {1}));
}

TEST_CASE("A step can end the query after its connection is gone", "[api][query_result]") {
	DuckDB db(nullptr);
	unique_ptr<QueryResult> handle;
	{
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("SET threads=1"));
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i BIGINT)"));
		handle = SubmitRetained(con, "INSERT INTO t SELECT i FROM range(100000) t(i)");
	}
	// The handle holds the last reference to the context, which the ending step releases
	REQUIRE(StepToEnd(*handle) == QueryResultState::FINISHED);
	REQUIRE(CHECK_COLUMN(*handle, 0, {100000}));

	Connection observer(db);
	auto count = observer.Query("SELECT count(*) FROM t");
	REQUIRE(CHECK_COLUMN(count, 0, {100000}));
}

TEST_CASE("An unfinished write to a temporary object closed inside a transaction invalidates it",
          "[api][query_result]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	REQUIRE_NO_FAIL(con.Query("CREATE TEMP TABLE t AS SELECT range i FROM range(1000000)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TEMP TABLE k(i BIGINT PRIMARY KEY)"));
	REQUIRE_NO_FAIL(con.Query("BEGIN TRANSACTION"));

	string statement;
	// A catalog change runs as a single task, so closing it before any step is the abort
	bool step = true;
	SECTION("insert") {
		statement = "INSERT INTO t SELECT i FROM range(1000000) t(i)";
	}
	SECTION("update") {
		statement = "UPDATE t SET i = i + 1";
	}
	SECTION("delete") {
		statement = "DELETE FROM t WHERE i % 2 = 0";
	}
	SECTION("insert or replace") {
		statement = "INSERT OR REPLACE INTO k SELECT i FROM range(1000000) t(i)";
	}
	SECTION("create table as") {
		statement = "CREATE TEMP TABLE t2 AS SELECT i FROM t";
	}
	SECTION("alter") {
		statement = "ALTER TABLE t RENAME COLUMN i TO j";
		step = false;
	}
	SECTION("drop") {
		statement = "DROP TABLE t";
		step = false;
	}
	auto handle = Submit(con, statement);
	if (step) {
		StepUnfinished(*handle);
	}
	handle->Close();

	auto next = con.Query("SELECT 42");
	REQUIRE(next->HasError());
	REQUIRE(StringUtil::Contains(next->GetError(), "aborted"));
	REQUIRE_NO_FAIL(con.Query("ROLLBACK"));
	auto rows = con.Query("SELECT count(*) = 1000000 AND sum(i) = 499999500000 FROM t");
	REQUIRE(CHECK_COLUMN(rows, 0, {true}));
	auto keys = con.Query("SELECT count(*) FROM k");
	REQUIRE(CHECK_COLUMN(keys, 0, {0}));
	REQUIRE_FAIL(con.Query("SELECT * FROM t2"));
}

TEST_CASE("An unfinished prepared insert into a temporary table closed inside a transaction invalidates it",
          "[api][query_result]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	REQUIRE_NO_FAIL(con.Query("CREATE TEMP TABLE t(i BIGINT)"));
	auto prepared = con.Prepare("INSERT INTO t SELECT i FROM range($1) t(i)");
	REQUIRE(!prepared->HasError());
	REQUIRE_NO_FAIL(con.Query("BEGIN TRANSACTION"));

	auto handle = prepared->Submit(1000000);
	REQUIRE(!handle->HasError());
	StepUnfinished(*handle);
	handle->Close();

	auto next = con.Query("SELECT 42");
	REQUIRE(next->HasError());
	REQUIRE(StringUtil::Contains(next->GetError(), "aborted"));
	REQUIRE_NO_FAIL(con.Query("ROLLBACK"));
	auto count = con.Query("SELECT count(*) FROM t");
	REQUIRE(CHECK_COLUMN(count, 0, {0}));
}

#endif
