#include "catch.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/extension_manager.hpp"
#include "duckdb/parallel/task_notifier.hpp"
#include "test_helpers.hpp"

#include <functional>
#include <thread>

using namespace duckdb;
using namespace Catch::Matchers;

struct TestClientContextState : ClientContextState {
	vector<string> query_errors;
	vector<string> transaction_errors;

	TestClientContextState() = default;
	TestClientContextState(const TestClientContextState &) = delete;

	void QueryEnd(ClientContext &, optional_ptr<ErrorData> error) override {
		if (error && error->HasError()) {
			query_errors.push_back(error->Message());
		}
	}

	void TransactionRollback(MetaTransaction &transaction, ClientContext &context,
	                         optional_ptr<ErrorData> error) override {
		if (error && error->HasError()) {
			transaction_errors.push_back(error->Message());
		}
	}
};

shared_ptr<TestClientContextState> WithLifecycleState(const Connection &conn) {
	auto &register_state = conn.context->registered_state;
	auto state = make_shared_ptr<TestClientContextState>();
	register_state->Insert("test_state", state);
	return state;
}

TEST_CASE("Test ClientContextState", "[api]") {
	DuckDB db(nullptr);
	Connection conn(db);
	conn.Query("CREATE TABLE my_table(i INT)");
	auto state = WithLifecycleState(conn);
	REQUIRE_NO_FAIL(conn.Query("SET default_transaction_invalidation_policy='SYNTACTIC_ERRORS_DO_NOT_INVALIDATE'"));

	const TableFunction table_fun(
	    "raise_exception_tf", {},
	    [](ClientContext &, TableFunctionInput &, DataChunk &) {
		    throw std::runtime_error("This is a test exception.");
	    },
	    [](ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
	       vector<Identifier> &names) -> unique_ptr<FunctionData> {
		    return_types.push_back(LogicalType::VARCHAR);
		    names.push_back("message");
		    return nullptr;
	    });

	ExtensionInfo extension_info {};
	ExtensionActiveLoad load_info {*db.instance, extension_info, "test_extension", ""};
	ExtensionLoader loader {load_info};
	loader.RegisterFunction(table_fun);

	SECTION("No error, No explicit transaction") {
		REQUIRE_NO_FAIL(conn.Query("SELECT * FROM my_table"));
		REQUIRE(state->query_errors.empty());
		REQUIRE(state->transaction_errors.empty());
	}

	SECTION("Error, No explicit transaction") {
		REQUIRE_FAIL(conn.Query("SELECT * FROM this_table_does_not_exist"));
		REQUIRE((state->query_errors.size() == 1));
		REQUIRE_THAT(state->query_errors.at(0), Contains("Table with name this_table_does_not_exist does not exist!"));
		REQUIRE((state->transaction_errors.size() == 1));
		REQUIRE_THAT(state->transaction_errors.at(0),
		             Contains("Table with name this_table_does_not_exist does not exist!"));
	}

	SECTION("No error, Explicit commit") {
		conn.BeginTransaction();
		REQUIRE_NO_FAIL(conn.Query("SELECT * FROM my_table"));
		conn.Commit();
		REQUIRE(state->query_errors.empty());
		REQUIRE(state->transaction_errors.empty());
	}

	SECTION("No error, Explicit rollback") {
		conn.BeginTransaction();
		REQUIRE_NO_FAIL(conn.Query("SELECT * FROM my_table"));
		conn.Rollback();
		REQUIRE(state->query_errors.empty());
		REQUIRE(state->transaction_errors.empty());
	}

	SECTION("Binding error, Explicit rollback") {
		// These errors do not invalidate the transaction...
		conn.BeginTransaction();
		REQUIRE_FAIL(conn.Query("SELECT * FROM this_table_does_not_exist_1"));
		REQUIRE_FAIL(conn.Query("SELECT * FROM this_table_does_not_exist_2"));
		REQUIRE_NO_FAIL(conn.Query("SELECT * FROM my_table"));
		conn.Rollback();
		REQUIRE((state->query_errors.size() == 2));
		REQUIRE_THAT(state->query_errors.at(0),
		             Contains("Table with name this_table_does_not_exist_1 does not exist!"));
		REQUIRE_THAT(state->query_errors.at(1),
		             Contains("Table with name this_table_does_not_exist_2 does not exist!"));
		REQUIRE((state->transaction_errors.empty()));
	}

	SECTION("Binding error, Explicit commit") {
		// These errors do not invalidate the transaction...
		conn.BeginTransaction();
		REQUIRE_FAIL(conn.Query("SELECT * FROM this_table_does_not_exist_1"));
		REQUIRE_FAIL(conn.Query("SELECT * FROM this_table_does_not_exist_2"));
		REQUIRE_NO_FAIL(conn.Query("SELECT * FROM my_table"));
		conn.Commit();
		REQUIRE((state->query_errors.size() == 2));
		REQUIRE_THAT(state->query_errors.at(0),
		             Contains("Table with name this_table_does_not_exist_1 does not exist!"));
		REQUIRE_THAT(state->query_errors.at(1),
		             Contains("Table with name this_table_does_not_exist_2 does not exist!"));
		REQUIRE((state->transaction_errors.empty()));
	}

	SECTION("Runtime error, Explicit commit") {
		conn.BeginTransaction();
		REQUIRE_NO_FAIL(conn.Query("INSERT INTO my_table VALUES (1)"));
		REQUIRE_FAIL(conn.Query("SELECT * FROM raise_exception_tf()"));
		REQUIRE_FAIL(conn.Query("CREATE TABLE my_table2(i INT)"));
		conn.Commit();
		REQUIRE((state->query_errors.size() == 2));
		REQUIRE_THAT(state->query_errors.at(0), Contains("This is a test exception."));
		REQUIRE_THAT(state->query_errors.at(1), Contains("Current transaction is aborted"));
		REQUIRE((state->transaction_errors.size() == 1));
		REQUIRE_THAT(state->transaction_errors.at(0),
		             Contains("Current transaction is aborted") || Contains("Failed to commit"));
		REQUIRE_FAIL(conn.Query("SELECT * FROM my_table2"));
	}

	SECTION("Runtime error, No explicit transaction") {
		REQUIRE_FAIL(conn.Query("SELECT * FROM raise_exception_tf()"));
		REQUIRE((state->query_errors.size() == 1));
		REQUIRE_THAT(state->query_errors.at(0), Contains("This is a test exception."));
		REQUIRE((state->transaction_errors.size() == 1));
		REQUIRE_THAT(state->transaction_errors.at(0), Contains("This is a test exception."));
	}

	SECTION("Manually invalidated transaction, Explicit commit") {
		conn.BeginTransaction();
		REQUIRE_NO_FAIL(conn.Query("CREATE TABLE my_table2(i INT)"));
		auto &transaction = conn.context->ActiveTransaction();
		ValidChecker::Invalidate(transaction, "42");
		try {
			conn.Commit();
		} catch (...) {
			// Ignore
		}
		REQUIRE((state->transaction_errors.size() == 1));
		REQUIRE_FAIL(conn.Query("SELECT * FROM my_table2"));
	}

	SECTION("Manually invalidated transaction, Explicit rollback") {
		conn.BeginTransaction();
		REQUIRE_NO_FAIL(conn.Query("CREATE TABLE my_table2(i INT)"));
		auto &transaction = conn.context->ActiveTransaction();
		ValidChecker::Invalidate(transaction, "42");
		conn.Rollback();
		REQUIRE((state->transaction_errors.size() == 1));
		REQUIRE_FAIL(conn.Query("SELECT * FROM my_table2"));
	}
}
// ClientContextState

namespace {

struct TaskHookCounts {
	atomic<idx_t> starts {0};
	atomic<idx_t> stops {0};
	atomic<idx_t> destroyed {0};
};

struct TaskHookState : ClientContextState {
	explicit TaskHookState(TaskHookCounts &counts) : counts(counts) {
	}
	~TaskHookState() override {
		counts.destroyed++;
	}

	void OnTaskStart(ClientContext &) override {
		counts.starts++;
		if (on_start) {
			on_start();
		}
	}
	void OnTaskStop(ClientContext &) override {
		counts.stops++;
		if (on_stop) {
			on_stop();
		}
	}

	TaskHookCounts &counts;
	std::function<void()> on_start;
	std::function<void()> on_stop;
};

} // namespace

TEST_CASE("Task notifications observe registration separately at start and stop", "[api][task_notifier]") {
	TaskHookCounts first_counts;
	TaskHookCounts second_counts;
	DuckDB db(nullptr);
	Connection con(db);
	auto &registry = *con.context->registered_state;
	registry.Remove("debug_client_context_state");
	{
		TaskNotifier empty(*con.context);
		TaskNotifier no_context(nullptr);
	}
	auto first = registry.GetOrCreate<TaskHookState>("hook", first_counts);
	weak_ptr<TaskHookState> first_weak = first;
	{
		TaskNotifier task(*con.context);
		REQUIRE(first_counts.starts == 1);
		registry.Remove("hook");
		first.reset();
		REQUIRE(first_weak.expired());
		registry.Insert("hook", make_shared_ptr<TaskHookState>(second_counts));
	}
	REQUIRE(first_counts.stops == 0);
	REQUIRE(first_counts.destroyed == 1);
	REQUIRE(second_counts.starts == 0);
	REQUIRE(second_counts.stops == 1);
	auto second = registry.Get<TaskHookState>("hook");
	REQUIRE(registry.GetOrCreate<TaskHookState>("hook", first_counts) == second);
	registry.Insert("hook", make_shared_ptr<TaskHookState>(first_counts));
	registry.Remove("missing");
	{ TaskNotifier task(*con.context); }
	REQUIRE(first_counts.starts == 1);
	REQUIRE(second_counts.starts == 1);
	REQUIRE(second_counts.stops == 2);
}

TEST_CASE("Task hook callbacks can change registration without invalidating dispatch", "[api][task_notifier]") {
	TaskHookCounts first_counts;
	TaskHookCounts sibling_counts;
	TaskHookCounts second_counts;
	TaskHookCounts last_counts;
	DuckDB db(nullptr);
	Connection con(db);
	auto &registry = *con.context->registered_state;
	auto first = make_shared_ptr<TaskHookState>(first_counts);
	auto second = make_shared_ptr<TaskHookState>(second_counts);
	weak_ptr<TaskHookState> first_weak = first;
	weak_ptr<TaskHookState> second_weak = second;
	bool start_alive = false;
	bool stop_alive = false;
	first->on_start = [&] {
		registry.Remove("hook");
		registry.Remove("sibling");
		registry.Insert("hook", std::move(second));
		start_alive = !first_weak.expired();
	};
	second->on_stop = [&] {
		registry.Remove("hook");
		registry.GetOrCreate<TaskHookState>("last", last_counts);
		stop_alive = !second_weak.expired();
	};
	registry.Insert("hook", std::move(first));
	registry.Insert("sibling", make_shared_ptr<TaskHookState>(sibling_counts));
	{
		TaskNotifier task(*con.context);
		REQUIRE(start_alive);
		REQUIRE(first_weak.expired());
		REQUIRE(sibling_counts.starts == 1);
		REQUIRE(sibling_counts.destroyed == 1);
		REQUIRE(second_counts.starts == 0);
	}
	REQUIRE(stop_alive);
	REQUIRE(second_weak.expired());
	REQUIRE(first_counts.starts == 1);
	REQUIRE(first_counts.stops == 0);
	REQUIRE(first_counts.destroyed == 1);
	REQUIRE(second_counts.stops == 1);
	REQUIRE(second_counts.destroyed == 1);
	REQUIRE(last_counts.stops == 0);
	{ TaskNotifier task(*con.context); }
	REQUIRE(last_counts.starts == 1);
	REQUIRE(last_counts.stops == 1);
}

TEST_CASE("Task hook dispatch remains usable after exceptions", "[api][task_notifier]") {
	TaskHookCounts counts;
	DuckDB db(nullptr);
	Connection con(db);
	auto state = con.context->registered_state->GetOrCreate<TaskHookState>("hook", counts);
	REQUIRE_THROWS_AS(
	    [&] {
		    TaskNotifier task(*con.context);
		    throw std::runtime_error("task failed");
	    }(),
	    std::runtime_error);
	REQUIRE(counts.starts == 1);
	REQUIRE(counts.stops == 1);
	state->on_start = [] {
		throw std::runtime_error("start failed");
	};
	REQUIRE_THROWS_AS(TaskNotifier(*con.context), std::runtime_error);
	REQUIRE(counts.starts == 2);
	REQUIRE(counts.stops == 1);
	state->on_start = nullptr;
	{ TaskNotifier task(*con.context); }
	REQUIRE(counts.starts == 3);
	REQUIRE(counts.stops == 2);
}

TEST_CASE("Concurrent task notifications retain states during registry mutation", "[api][task_notifier]") {
	TaskHookCounts stable_counts;
	TaskHookCounts transient_counts;
	DuckDB db(nullptr);
	Connection con(db);
	auto &registry = *con.context->registered_state;
	registry.GetOrCreate<TaskHookState>("stable", stable_counts);
	constexpr idx_t thread_count = 8;
	constexpr idx_t task_count = 2000;
	atomic<idx_t> ready {0};
	atomic<bool> start {false};
	vector<std::thread> threads;
	for (idx_t t = 0; t < thread_count; t++) {
		threads.emplace_back([&] {
			ready++;
			while (!start.load()) {
				std::this_thread::yield();
			}
			for (idx_t i = 0; i < task_count; i++) {
				TaskNotifier task(*con.context);
			}
		});
	}
	while (ready != thread_count) {
		std::this_thread::yield();
	}
	start = true;
	for (idx_t i = 0; i < task_count; i++) {
		registry.Insert("transient", make_shared_ptr<TaskHookState>(transient_counts));
		std::this_thread::yield();
		registry.Remove("transient");
	}
	for (auto &thread : threads) {
		thread.join();
	}
	REQUIRE(stable_counts.starts == thread_count * task_count);
	REQUIRE(stable_counts.stops == thread_count * task_count);
	REQUIRE(transient_counts.destroyed == task_count);
}

struct ThrowingRollbackState : ClientContextState {
	void TransactionRollback(MetaTransaction &transaction, ClientContext &context,
	                         optional_ptr<ErrorData> error) override {
		throw IOException("rollback failed");
	}
};

TEST_CASE("Dropping a connection whose rollback fails does not terminate", "[api]") {
	DuckDB db(nullptr);
	{
		Connection conn(db);
		conn.context->registered_state->Insert("throwing_rollback", make_shared_ptr<ThrowingRollbackState>());
		REQUIRE_NO_FAIL(conn.Query("BEGIN"));
		REQUIRE_NO_FAIL(conn.Query("CREATE TABLE t(i INT)"));
		// an explicit rollback reports the failure
		REQUIRE_FAIL(conn.Query("ROLLBACK"));

		REQUIRE_NO_FAIL(conn.Query("BEGIN"));
		REQUIRE_NO_FAIL(conn.Query("SELECT 42"));
		// leaving the scope destroys the connection with the transaction still open
	}
	Connection conn(db);
	REQUIRE_NO_FAIL(conn.Query("SELECT 42"));
}
