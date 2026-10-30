#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/execution/operator/scan/physical_column_data_scan.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/parallel/pipeline.hpp"
#include "duckdb/execution/executor.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/storage/storage_info.hpp"

#include <thread>

using namespace duckdb;

void run_query_multiple_times(duckdb::unique_ptr<string> query, duckdb::unique_ptr<Connection> con) {
	for (int i = 0; i < 10; ++i) {
		auto result = con->Query(*query);
	}
}

void change_thread_counts(duckdb::DuckDB &db) {
	auto con = Connection(db);
	for (int i = 0; i < 10; ++i) {
		con.Query("SET threads=10");
		con.Query("SET threads=1");
	}
}

// NumberOfThreads acquired the same lock as RelaunchThreads
// NumberOfThreads is waiting for the lock
// RelaunchThreads is waiting on the thread to finish, while holding the lock
TEST_CASE("Test deadlock issue between NumberOfThreads and RelaunchThreads", "[api]") {
	duckdb::DuckDB db(nullptr);

	int thread_count = 10;
	std::vector<std::thread> threads(thread_count);

	// This query will hit NumberOfThreads because it uses the RadixPartitionedHashtable
	for (int i = 0; i < thread_count; ++i) {
		auto query = make_uniq<string>(R"(
			WITH dataset AS (
			  SELECT * FROM (VALUES
				(1, 'Alice'),
				(2, 'Bob'),
				(3, 'Alice'),
				(4, 'Carol')
			  ) AS t(id, name)
			)
			SELECT DISTINCT name FROM dataset;
		)");

		threads[i] = std::thread(run_query_multiple_times, std::move(query), make_uniq<Connection>(db));
	}

	// Fire off queries that change the thread count,
	// causing us to relaunch the worker threads on every subsequent query.
	change_thread_counts(db);

	for (int i = 0; i < thread_count; ++i) {
		threads[i].join();
	}
}

TEST_CASE("Test database maximum_threads argument", "[api]") {
	// default is number of hw threads
	// FIXME: not yet
	{
		DuckDB db(nullptr);
		REQUIRE(db.NumberOfThreads() == DBConfig().GetSystemMaxThreads());
	}
	// but we can set another value
	{
		DBConfig config;
		config.options.maximum_threads = 10;
		DuckDB db(nullptr, &config);
		REQUIRE(db.NumberOfThreads() == 10);
	}
	// zero is not erlaubt
	{
		DBConfig config;
		config.options.maximum_threads = 0;
		DuckDB db;
		REQUIRE_THROWS(db = DuckDB(nullptr, &config));
	}
}

TEST_CASE("Test external threads", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &config = DBConfig::GetConfig(*db.instance);
	auto options = config.GetOptions();

	con.Query("SET threads=13");
	REQUIRE(config.options.maximum_threads == 13);
	REQUIRE(db.NumberOfThreads() == 13);
	con.Query("SET external_threads=13");
	REQUIRE(Settings::Get<ExternalThreadsSetting>(config) == 13);
	REQUIRE(db.NumberOfThreads() == 13);

	con.Query("SET external_threads=0");
	REQUIRE(Settings::Get<ExternalThreadsSetting>(config) == 0);
	REQUIRE(db.NumberOfThreads() == 13);

	auto res = con.Query("SET external_threads=-1");
	REQUIRE(res->HasError());
	REQUIRE(StringUtil::Contains(res->GetError(), "out of range"));

	res = con.Query("SET external_threads=14");
	REQUIRE(res->HasError());
	REQUIRE(StringUtil::Contains(res->GetError(), "smaller"));

	con.Query("SET external_threads=5");
	REQUIRE(Settings::Get<ExternalThreadsSetting>(config) == 5);
	REQUIRE(db.NumberOfThreads() == 13);

	con.Query("RESET external_threads");
	REQUIRE(Settings::Get<ExternalThreadsSetting>(config) == 1);
	REQUIRE(db.NumberOfThreads() == 13);

	con.Query("RESET threads");
	REQUIRE(config.options.maximum_threads == DBConfig().GetSystemMaxThreads());
	REQUIRE(db.NumberOfThreads() == DBConfig().GetSystemMaxThreads());
}

#ifndef DUCKDB_NO_THREADS
namespace {

struct ThreadCapSourceState : public GlobalSourceState {
	explicit ThreadCapSourceState(idx_t limit_p) : limit(limit_p) {
	}
	idx_t limit;

	idx_t MaxThreads() override {
		return limit;
	}
};

struct ThreadCapOperatorState : public GlobalOperatorState {
	explicit ThreadCapOperatorState(idx_t limit_p) : limit(limit_p) {
	}
	idx_t limit;

	idx_t MaxThreads(idx_t source_max_threads) override {
		return limit;
	}
};

struct ThreadCapSinkState : public GlobalSinkState {
	explicit ThreadCapSinkState(idx_t limit_p) : limit(limit_p) {
	}
	idx_t limit;
	idx_t calls = 0;
	idx_t input_limit = 0;

	idx_t MaxThreads(idx_t source_max_threads) override {
		calls++;
		input_limit = source_max_threads;
		return limit;
	}
};

class ThreadCapOperator : public PhysicalOperator {
public:
	ThreadCapOperator(PhysicalPlan &plan, idx_t limit_p)
	    : PhysicalOperator(plan, PhysicalOperatorType::INVALID, {LogicalType::INTEGER}, 0), limit(limit_p) {
	}

	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override {
		return make_uniq<ThreadCapSourceState>(limit);
	}
	unique_ptr<GlobalOperatorState> GetGlobalOperatorState(ClientContext &context) const override {
		return make_uniq<ThreadCapOperatorState>(limit);
	}
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override {
		return make_uniq<ThreadCapSinkState>(limit);
	}
	bool IsSource() const override {
		return true;
	}
	bool IsSink() const override {
		return true;
	}
	bool ParallelSource() const override {
		return true;
	}
	bool ParallelOperator() const override {
		return true;
	}
	bool ParallelSink() const override {
		return true;
	}

private:
	idx_t limit;
};

} // namespace

// SQL cannot supply a sink callback that raises the accumulated source/operator limit.
TEST_CASE("Pipeline sink thread limits cannot raise source or operator caps", "[api]") {
	DBConfig config;
	config.options.maximum_threads = 8;
	DuckDB db(nullptr, &config);
	Connection con(db);
	Executor executor(*con.context);
	PipelineBuildState build_state;
	struct Limits {
		idx_t source, op, sink, expected, sink_input;
	};
	const Limits cases[] = {{1, 8, 8, 1, 1}, {8, 2, 8, 2, 2}, {8, 8, 3, 3, 8},
	                        {0, 8, 8, 0, 0}, {8, 8, 0, 0, 8}, {16, 16, 16, 8, 8}};
	for (const auto &limits : cases) {
		CAPTURE(limits.source, limits.op, limits.sink);
		PhysicalPlan plan(Allocator::Get(*con.context));
		auto &source = plan.Make<ThreadCapOperator>(limits.source);
		auto &op = plan.Make<ThreadCapOperator>(limits.op);
		auto &sink = plan.Make<ThreadCapOperator>(limits.sink);
		Pipeline pipeline(executor);
		build_state.SetPipelineSource(pipeline, source);
		build_state.SetPipelineOperators(pipeline, {op});
		build_state.SetPipelineSink(pipeline, sink, 1);
		pipeline.Ready();
		pipeline.Reset();
		REQUIRE(pipeline.GetMaxThreads() == limits.expected);
		auto &sink_state = sink.sink_state->Cast<ThreadCapSinkState>();
		REQUIRE(sink_state.calls == 1);
		REQUIRE(sink_state.input_limit == limits.sink_input);
	}
}

TEST_CASE("Test async threads", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &scheduler = TaskScheduler::GetScheduler(*con.context);

	con.Query("SET async_threads=0");
	REQUIRE(scheduler.NumberOfAsyncThreads() == 0);

	con.Query("SET async_threads=2");
	REQUIRE(scheduler.NumberOfAsyncThreads() == 2);
}

static idx_t ColumnDataScanMaxThreads(Connection &con, QueryResult &result,
                                      const OperatorPartitionInfo &partition_info) {
	PhysicalPlan physical_plan(Allocator::Get(*con.context));
	auto &scan = physical_plan.Make<PhysicalColumnDataScan>(result.GetTypes(), PhysicalOperatorType::COLUMN_DATA_SCAN,
	                                                        result.Collection().Count(), result.Collection());
	auto source_state = scan.GetGlobalSourceState(*con.context, partition_info);
	return source_state->MaxThreads();
}

#if STANDARD_VECTOR_SIZE <= 4096
TEST_CASE("Column data scan batch sizes preserve source parallelism", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=8"));

	const idx_t small_count = STANDARD_VECTOR_SIZE * 16;
	auto small_result = con.Query(StringUtil::Format("SELECT i FROM range(%llu) t(i)", small_count));
	REQUIRE(!small_result->HasError());
	auto &small_materialized = *small_result;

	REQUIRE(ColumnDataScanMaxThreads(con, small_materialized, OperatorPartitionInfo::NoPartitionInfo()) == 16);
	REQUIRE(ColumnDataScanMaxThreads(con, small_materialized, OperatorPartitionInfo::BatchIndex()) == 8);
	REQUIRE(ColumnDataScanMaxThreads(con, small_materialized,
	                                 OperatorPartitionInfo::BatchIndex(DEFAULT_ROW_GROUP_SIZE)) == 8);

#if STANDARD_VECTOR_SIZE >= 512
	const idx_t large_count = DEFAULT_ROW_GROUP_SIZE * 8 + STANDARD_VECTOR_SIZE;
	auto large_result = con.Query(StringUtil::Format("SELECT i FROM range(%llu) t(i)", large_count));
	REQUIRE(!large_result->HasError());
	auto &large_materialized = *large_result;
	REQUIRE(ColumnDataScanMaxThreads(con, large_materialized, OperatorPartitionInfo::BatchIndex()) == 9);
	REQUIRE(ColumnDataScanMaxThreads(con, large_materialized,
	                                 OperatorPartitionInfo::BatchIndex(DEFAULT_ROW_GROUP_SIZE)) == 9);
#endif
}
#endif
#endif

#ifdef DUCKDB_NO_THREADS
TEST_CASE("Test scheduling with no threads", "[api]") {
	DuckDB db(nullptr);
	Connection con1(db);
	Connection con2(db);

	const auto query_1 = con1.Submit("SELECT 42");
	const auto query_2 = con2.Submit("SELECT 42");
	// Get the completed pipelines. Because "executeTask" was never called, there should be no completed pipelines.
	auto query_1_pipelines = con1.context->GetExecutor().GetCompletedPipelines();
	REQUIRE((query_1_pipelines == 0));

	// Execute the second query
	query_2->Complete();
	REQUIRE_NO_FAIL(*query_2);

	// And even after that, there should still be no completed pipelines for the first query.
	query_1_pipelines = con1.context->GetExecutor().GetCompletedPipelines();
	REQUIRE((query_1_pipelines == 0));
	query_1->Complete();
	REQUIRE_NO_FAIL(*query_1);
}
#endif
