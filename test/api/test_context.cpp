#include "catch.hpp"
#include "duckdb.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/query_context.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/context.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/parallel/task_executor.hpp"
#include "duckdb/storage/temporary_memory_manager.hpp"

using namespace duckdb;

TEST_CASE("Test Context", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET max_execution_time=12345"));

	SECTION("services owned by a ClientContext") {
		auto &context = con.context->GetContext();
		REQUIRE(context.IsClient());
		REQUIRE(context.TryGetClientContext().get() == con.context.get());
		REQUIRE(&context.GetDatabase() == db.instance.get());
		REQUIRE(context.TryGetQueryProfiler());
		REQUIRE(context.TryGetFileOpener());
		REQUIRE(&context.GetClientConfig() == &ClientConfig::GetConfig(*con.context));
		REQUIRE(&context.GetLogger() == &con.context->GetLogger());
		(void)context.GetTaskScheduler();
		Value result;
		auto lookup = context.TryGetCurrentSetting(Identifier("max_execution_time"), result);
		bool found = lookup;
		REQUIRE(found);
		REQUIRE(result.GetValue<int64_t>() == 12345);
	}
	SECTION("services created from a DatabaseInstance") {
		auto context = db.instance->Services();
		REQUIRE(!context->IsClient());
		REQUIRE(!context->TryGetClientContext());
		REQUIRE(!context->TryGetQueryProfiler());
		REQUIRE(!context->TryGetClientConfig());
		REQUIRE(&context->GetDatabase() == db.instance.get());
		Value result;
		auto lookup = context->TryGetCurrentSetting(Identifier("max_execution_time"), result);
		bool found = lookup;
		REQUIRE(found);
		// the connection-local SET is not visible through the database-wide services
		REQUIRE(result.GetValue<int64_t>() == 0);
		(void)context->GetLogger();
		(void)context->GetFileSystem();
		(void)context->GetMetricsManager();
		(void)context->GetTaskScheduler();
	}
	SECTION("TaskExecutor from services") {
		auto context = db.instance->Services();
		TaskExecutor executor(*context);
		executor.WorkOnTasks();
	}
	SECTION("QueryContext holds services") {
		QueryContext invalid;
		REQUIRE(!invalid.Valid());
		REQUIRE(!invalid.GetContext());
		QueryContext valid(*con.context);
		REQUIRE(valid.Valid());
		REQUIRE(valid.GetContext().get() == &con.context->GetContext());
		QueryContext from_ptr(optional_ptr<ClientContext>(con.context.get()));
		REQUIRE(from_ptr.GetContext().get() == &con.context->GetContext());
	}
}

TEST_CASE("Test Context keeps the connection operator_memory_limit", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	REQUIRE_NO_FAIL(con.Query("SET memory_limit='1GiB'"));
	REQUIRE_NO_FAIL(con.Query("SET temp_directory='" + TestCreatePath("test_context_operator_memory") + "'"));
	REQUIRE_NO_FAIL(con.Query("SET operator_memory_limit='8MiB'"));

	auto &context = con.context->GetContext();
	auto state = TemporaryMemoryManager::Get(context).Register(context);
	state->SetMinimumReservation(1);
	state->SetRemainingSizeAndUpdateReservation(context, 64ULL * 1024 * 1024);
	REQUIRE(state->GetReservation() == 8ULL * 1024 * 1024);
}
