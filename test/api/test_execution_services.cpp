#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/common/query_context.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/execution_services.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/parallel/task_executor.hpp"

using namespace duckdb;

TEST_CASE("Test ExecutionServices", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	con.Query("SET threads=2");

	SECTION("services owned by a ClientContext") {
		auto &services = con.context->Services();
		REQUIRE(services.IsClient());
		REQUIRE(services.TryGetClientContext().get() == con.context.get());
		REQUIRE(&services.GetDatabase() == db.instance.get());
		REQUIRE(services.TryGetQueryProfiler());
		REQUIRE(services.TryGetFileOpener());
		REQUIRE(&services.GetClientConfig() == &ClientConfig::GetConfig(*con.context));
		REQUIRE(&services.GetLogger() == &con.context->GetLogger());
		(void)services.GetTaskScheduler();
		Value result;
		auto lookup = services.TryGetCurrentSetting(Identifier("threads"), result);
		bool found = lookup;
		REQUIRE(found);
		REQUIRE(result.GetValue<int64_t>() == 2);
	}
	SECTION("services created from a DatabaseInstance") {
		auto services = db.instance->Services();
		REQUIRE(!services->IsClient());
		REQUIRE(!services->TryGetClientContext());
		REQUIRE(!services->TryGetQueryProfiler());
		REQUIRE(!services->TryGetClientConfig());
		REQUIRE(&services->GetDatabase() == db.instance.get());
		Value result;
		auto lookup = services->TryGetCurrentSetting(Identifier("default_order"), result);
		bool found = lookup;
		REQUIRE(found);
		(void)services->GetLogger();
		(void)services->GetFileSystem();
		(void)services->GetMetricsManager();
		(void)services->GetTaskScheduler();
	}
	SECTION("TaskExecutor from services") {
		auto services = db.instance->Services();
		TaskExecutor executor(*services);
		executor.WorkOnTasks();
	}
	SECTION("QueryContext holds services") {
		QueryContext invalid;
		REQUIRE(!invalid.Valid());
		REQUIRE(!invalid.GetServices());
		QueryContext valid(*con.context);
		REQUIRE(valid.Valid());
		REQUIRE(valid.GetServices().get() == con.context->services.get());
		QueryContext from_ptr(optional_ptr<ClientContext>(con.context.get()));
		REQUIRE(from_ptr.GetServices().get() == con.context->services.get());
	}
}
