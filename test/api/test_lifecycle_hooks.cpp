#include "catch.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/extension_manager.hpp"
#include "duckdb/main/query_result.hpp"
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
	auto &register_state = conn.context->GetContext().GetRegisteredState();
	auto state = make_shared_ptr<TestClientContextState>();
	register_state.Insert("test_state", state);
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

	void OnTaskStart(Context &) override {
		counts.starts++;
		if (on_start) {
			on_start();
		}
	}
	void OnTaskStop(Context &) override {
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
	auto &registry = con.context->GetContext().GetRegisteredState();
	registry.Remove("debug_client_context_state");
	{
		TaskNotifier empty(&con.context->GetContext());
		TaskNotifier no_context(nullptr);
	}
	auto first = registry.GetOrCreate<TaskHookState>("hook", first_counts);
	weak_ptr<TaskHookState> first_weak = first;
	{
		TaskNotifier task(&con.context->GetContext());
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
	{ TaskNotifier task(&con.context->GetContext()); }
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
	auto &registry = con.context->GetContext().GetRegisteredState();
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
		TaskNotifier task(&con.context->GetContext());
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
	{ TaskNotifier task(&con.context->GetContext()); }
	REQUIRE(last_counts.starts == 1);
	REQUIRE(last_counts.stops == 1);
}

TEST_CASE("Task hook dispatch remains usable after exceptions", "[api][task_notifier]") {
	TaskHookCounts counts;
	DuckDB db(nullptr);
	Connection con(db);
	auto state = con.context->GetContext().GetRegisteredState().GetOrCreate<TaskHookState>("hook", counts);
	REQUIRE_THROWS_AS(
	    [&] {
		    TaskNotifier task(&con.context->GetContext());
		    throw std::runtime_error("task failed");
	    }(),
	    std::runtime_error);
	REQUIRE(counts.starts == 1);
	REQUIRE(counts.stops == 1);
	state->on_start = [] {
		throw std::runtime_error("start failed");
	};
	REQUIRE_THROWS_AS(TaskNotifier(&con.context->GetContext()), std::runtime_error);
	REQUIRE(counts.starts == 2);
	REQUIRE(counts.stops == 1);
	state->on_start = nullptr;
	{ TaskNotifier task(&con.context->GetContext()); }
	REQUIRE(counts.starts == 3);
	REQUIRE(counts.stops == 2);
}

TEST_CASE("Concurrent task notifications retain states during registry mutation", "[api][task_notifier]") {
	TaskHookCounts stable_counts;
	TaskHookCounts transient_counts;
	DuckDB db(nullptr);
	Connection con(db);
	auto &registry = con.context->GetContext().GetRegisteredState();
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
				TaskNotifier task(&con.context->GetContext());
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
		conn.context->GetContext().GetRegisteredState().Insert("throwing_rollback",
		                                                       make_shared_ptr<ThrowingRollbackState>());
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

namespace {

//! Counts the statement and query notifications and fails StatementEnd while the shared failures last
struct FailingStatementEndState : ClientContextState {
	FailingStatementEndState(string name_p, idx_t &failures_left_p, string &first_failure_p, bool &fatal_after_first_p)
	    : name(std::move(name_p)), failures_left(failures_left_p), first_failure(first_failure_p),
	      fatal_after_first(fatal_after_first_p) {
	}
	using ClientContextState::QueryEnd;
	using ClientContextState::StatementEnd;

	void StatementBegin(ClientContext &) override {
		statement_begins++;
	}
	void StatementEnd(ClientContext &, optional_ptr<ErrorData> error) override {
		statement_ends++;
		if (error && error->HasError()) {
			statement_end_errors++;
		}
		if (failures_left > 0) {
			failures_left--;
			if (first_failure.empty()) {
				first_failure = name;
			}
			if (fail_with_binder_error) {
				throw BinderException(name + " StatementEnd failed");
			}
			if (fail_with_fatal_error || (fatal_after_first && first_failure != name)) {
				throw FatalException(name + " StatementEnd failed");
			}
			throw IOException(name + " StatementEnd failed");
		}
	}
	void QueryEnd(ClientContext &, optional_ptr<ErrorData> error) override {
		query_ends++;
		if (error && error->HasError()) {
			query_end_errors.push_back(error->Message());
		}
	}

	string name;
	//! Shared by the states, so the first StatementEnd dispatched fails whichever state receives it
	idx_t &failures_left;
	//! The name of the state that failed first, shared by the states
	string &first_failure;
	//! Whether the failures after the first one are fatal errors, shared by the states
	bool &fatal_after_first;
	//! Whether the failure is an error of a kind that does not invalidate transactions under the permissive policy
	bool fail_with_binder_error = false;
	//! Whether the failure is a fatal error, which invalidates the database
	bool fail_with_fatal_error = false;
	idx_t statement_begins = 0;
	idx_t statement_ends = 0;
	idx_t statement_end_errors = 0;
	idx_t query_ends = 0;
	vector<string> query_end_errors;
};

//! Two registered states on a connection whose StatementEnd fails the given number of times
struct FailingStatementEnds {
	explicit FailingStatementEnds(Connection &conn_p) : conn(conn_p) {
		alpha = make_shared_ptr<FailingStatementEndState>("alpha", failures_left, first_failure, fatal_after_first);
		beta = make_shared_ptr<FailingStatementEndState>("beta", failures_left, first_failure, fatal_after_first);
		conn.context->GetContext().GetRegisteredState().Insert("alpha", alpha);
		conn.context->GetContext().GetRegisteredState().Insert("beta", beta);
	}

	template <class CALLBACK>
	void ForEachState(CALLBACK &&callback) {
		callback(*alpha);
		callback(*beta);
	}

	void ResetCounts() {
		ForEachState([](FailingStatementEndState &state) {
			state.statement_begins = 0;
			state.statement_ends = 0;
			state.statement_end_errors = 0;
			state.query_ends = 0;
			state.query_end_errors.clear();
		});
	}

	void FailWithBinderError() {
		ForEachState([](FailingStatementEndState &state) { state.fail_with_binder_error = true; });
	}

	//! The message of the error the state that failed first threw
	string FirstError() {
		REQUIRE(!first_failure.empty());
		return first_failure + " StatementEnd failed";
	}

	//! Every state saw the statement begin and end once, and the query end once with the first failure
	void RequireEndedOnce() {
		ForEachState([&](FailingStatementEndState &state) {
			REQUIRE(state.statement_begins == 1);
			REQUIRE(state.statement_ends == 1);
			REQUIRE(state.query_ends == 1);
			REQUIRE(state.query_end_errors.size() == 1);
			REQUIRE_THAT(state.query_end_errors[0], Contains(FirstError()));
		});
		REQUIRE(!conn.context->transaction.HasActiveTransaction());
	}

	Connection &conn;
	idx_t failures_left = 0;
	string first_failure;
	bool fatal_after_first = false;
	shared_ptr<FailingStatementEndState> alpha;
	shared_ptr<FailingStatementEndState> beta;
};

} // namespace

TEST_CASE("A failing StatementEnd ends the query on every state", "[api]") {
	DuckDB db(nullptr);
	Connection conn(db);
	REQUIRE_NO_FAIL(conn.Query("CREATE TABLE t(i INT)"));
	FailingStatementEnds test(conn);

	SECTION("Query reports the failure and rolls the statement back") {
		test.failures_left = 1;
		unique_ptr<QueryResult> result;
		REQUIRE_NOTHROW(result = conn.Query("INSERT INTO t VALUES (1)"));
		REQUIRE(result->HasError());
		REQUIRE_THAT(result->GetError(), Contains(test.FirstError()));
		test.RequireEndedOnce();
		// the state that did not fail saw a statement without error
		REQUIRE(test.alpha->statement_end_errors + test.beta->statement_end_errors == 0);

		REQUIRE_NO_FAIL(conn.Query("SELECT 42"));
		auto count = conn.Query("SELECT count(*) FROM t");
		REQUIRE(CHECK_COLUMN(count, 0, {0}));
	}

	SECTION("Every state fails") {
		test.failures_left = 2;
		unique_ptr<QueryResult> result;
		REQUIRE_NOTHROW(result = conn.Query("SELECT 42"));
		REQUIRE(result->HasError());
		// the first failure is the one reported, also to the states
		REQUIRE_THAT(result->GetError(), Contains(test.FirstError()));
		test.RequireEndedOnce();
		REQUIRE(test.failures_left == 0);
		REQUIRE_NO_FAIL(conn.Query("SELECT 42"));
	}

	SECTION("A statement that failed keeps its own error") {
		test.failures_left = 1;
		unique_ptr<QueryResult> result;
		REQUIRE_NOTHROW(result = conn.Query("SELECT * FROM missing_table"));
		REQUIRE(result->HasError());
		REQUIRE_THAT(result->GetError(), Contains("missing_table"));
		test.ForEachState([](FailingStatementEndState &state) {
			REQUIRE(state.statement_ends == 1);
			REQUIRE(state.statement_end_errors == 1);
			REQUIRE(state.query_ends == 1);
			REQUIRE(state.query_end_errors.size() == 1);
			REQUIRE_THAT(state.query_end_errors[0], Contains("missing_table"));
		});
		REQUIRE(!conn.context->transaction.HasActiveTransaction());
		REQUIRE_NO_FAIL(conn.Query("SELECT 42"));
	}

	SECTION("Stepping with ExecuteTask ends the query") {
		auto handle = conn.Submit("SELECT 42");
		REQUIRE_NO_FAIL(*handle);
		handle->Materialize();
		test.failures_left = 1;
		QueryResultState state = QueryResultState::NOT_READY;
		REQUIRE_NOTHROW([&]() {
			while (!IsTerminal(state = handle->ExecuteTask())) {
				if (state == QueryResultState::BLOCKED || state == QueryResultState::NO_TASKS_AVAILABLE) {
					handle->WaitForTask();
				}
			}
		}());
		REQUIRE(state == QueryResultState::EXECUTION_ERROR);
		REQUIRE(handle->HasError());
		REQUIRE_THAT(handle->GetError(), Contains(test.FirstError()));
		test.RequireEndedOnce();
		// the connection is free before the handle is destroyed
		REQUIRE_NO_FAIL(conn.Query("SELECT 42"));
	}

	SECTION("A statement that wrote aborts its explicit transaction") {
		REQUIRE_NO_FAIL(conn.Query("SET default_transaction_invalidation_policy='SYNTACTIC_ERRORS_DO_NOT_INVALIDATE'"));
		REQUIRE_NO_FAIL(conn.Query("BEGIN"));
		test.ResetCounts();
		// an error of a kind that would leave the transaction valid
		test.FailWithBinderError();
		test.failures_left = 1;
		unique_ptr<QueryResult> result;
		REQUIRE_NOTHROW(result = conn.Query("INSERT INTO t VALUES (1)"));
		REQUIRE(result->HasError());
		REQUIRE_THAT(result->GetError(), Contains("StatementEnd failed"));
		test.ForEachState([](FailingStatementEndState &state) {
			REQUIRE(state.statement_ends == 1);
			REQUIRE(state.query_ends == 1);
		});
		// the row cannot be committed, since the statement reported a failure
		REQUIRE(conn.context->transaction.HasActiveTransaction());
		REQUIRE_FAIL(conn.Query("SELECT 42"));
		REQUIRE_NO_FAIL(conn.Query("ROLLBACK"));
		auto count = conn.Query("SELECT count(*) FROM t");
		REQUIRE(CHECK_COLUMN(count, 0, {0}));
	}

	SECTION("A statement that read follows the invalidation policy") {
		REQUIRE_NO_FAIL(conn.Query("SET default_transaction_invalidation_policy='SYNTACTIC_ERRORS_DO_NOT_INVALIDATE'"));
		REQUIRE_NO_FAIL(conn.Query("BEGIN"));
		test.FailWithBinderError();
		test.failures_left = 1;
		unique_ptr<QueryResult> result;
		REQUIRE_NOTHROW(result = conn.Query("SELECT 42"));
		REQUIRE(result->HasError());
		REQUIRE_THAT(result->GetError(), Contains("StatementEnd failed"));
		// a binder error does not invalidate the transaction under this policy
		REQUIRE(conn.context->transaction.HasActiveTransaction());
		REQUIRE_NO_FAIL(conn.Query("SELECT 42"));
		REQUIRE_NO_FAIL(conn.Query("COMMIT"));

		// an IO error does
		REQUIRE_NO_FAIL(conn.Query("BEGIN"));
		test.ForEachState([](FailingStatementEndState &state) { state.fail_with_binder_error = false; });
		test.failures_left = 1;
		REQUIRE_NOTHROW(result = conn.Query("SELECT 42"));
		REQUIRE(result->HasError());
		REQUIRE(conn.context->transaction.HasActiveTransaction());
		REQUIRE_FAIL(conn.Query("SELECT 42"));
		REQUIRE_NO_FAIL(conn.Query("ROLLBACK"));
	}

	SECTION("The error is processed like the error of a statement") {
		REQUIRE_NO_FAIL(conn.Query("SET errors_as_json = true"));
		test.failures_left = 1;
		unique_ptr<QueryResult> result;
		REQUIRE_NOTHROW(result = conn.Query("SELECT 42"));
		REQUIRE(result->HasError());
		REQUIRE_THAT(result->GetError(), Contains("\"exception_type\":\"IO\""));
		REQUIRE_THAT(result->GetError(), Contains(test.FirstError()));
		REQUIRE_NO_FAIL(conn.Query("SELECT 42"));
	}

	SECTION("Between the statements of a query that expands into several") {
		REQUIRE_NO_FAIL(conn.Query("CREATE TABLE sales(city VARCHAR, year INTEGER, amount INTEGER)"));
		REQUIRE_NO_FAIL(conn.Query("INSERT INTO sales VALUES ('ams', 2023, 10), ('ams', 2024, 20)"));
		test.ResetCounts();
		// the dynamic PIVOT runs a statement for the distinct years before the one that returns the rows
		test.failures_left = 1;
		unique_ptr<QueryResult> result;
		REQUIRE_NOTHROW(result = conn.Query("PIVOT sales ON year USING sum(amount) ORDER BY city"));
		REQUIRE(result->HasError());
		REQUIRE_THAT(result->GetError(), Contains("StatementEnd failed"));
		// the end that failed is not sent again when the query fails
		test.RequireEndedOnce();
		REQUIRE_NO_FAIL(conn.Query("SELECT 42"));
	}
}

TEST_CASE("Destroying a connection whose StatementEnd keeps failing does not terminate", "[api]") {
	DuckDB db(nullptr);
	{
		Connection conn(db);
		FailingStatementEnds test(conn);
		test.failures_left = 3;
		unique_ptr<QueryResult> result;
		REQUIRE_NOTHROW(result = conn.Query("SELECT 42"));
		REQUIRE(result->HasError());
		test.RequireEndedOnce();
		// the query ended, so destroying the connection has nothing to end
	}
	{
		Connection conn(db);
		FailingStatementEnds test(conn);
		auto handle = conn.Submit("SELECT 42");
		REQUIRE_NO_FAIL(*handle);
		test.failures_left = 3;
		// the open result keeps the query active while the context is destroyed
		REQUIRE_NOTHROW(conn.context->Destroy());
		test.ForEachState([](FailingStatementEndState &state) { REQUIRE(state.statement_ends == 1); });
		REQUIRE(!conn.context->transaction.HasActiveTransaction());
	}
	Connection conn(db);
	REQUIRE_NO_FAIL(conn.Query("SELECT 42"));
}

TEST_CASE("A fatal StatementEnd error invalidates the database", "[api]") {
	DuckDB db(nullptr);
	Connection conn(db);
	FailingStatementEnds test(conn);
	test.ForEachState([](FailingStatementEndState &state) { state.fail_with_fatal_error = true; });

	SECTION("Reported on the result") {
		test.failures_left = 1;
		unique_ptr<QueryResult> result;
		REQUIRE_NOTHROW(result = conn.Query("SELECT 42"));
		REQUIRE(result->HasError());
		REQUIRE_THAT(result->GetError(), Contains(test.FirstError()));
	}

	SECTION("Behind the error of the statement") {
		test.failures_left = 1;
		unique_ptr<QueryResult> result;
		REQUIRE_NOTHROW(result = conn.Query("SELECT * FROM missing_table"));
		REQUIRE(result->HasError());
		REQUIRE_THAT(result->GetError(), Contains("missing_table"));
	}

	SECTION("Behind the failure of the state that failed first") {
		// the first state fails with an IO error, the second fatally
		test.ForEachState([](FailingStatementEndState &state) { state.fail_with_fatal_error = false; });
		test.fatal_after_first = true;
		test.failures_left = 2;
		unique_ptr<QueryResult> result;
		REQUIRE_NOTHROW(result = conn.Query("SELECT 42"));
		REQUIRE(result->HasError());
		REQUIRE_THAT(result->GetError(), Contains(test.FirstError()));
		test.ForEachState([&](FailingStatementEndState &state) {
			REQUIRE(state.query_end_errors.size() == 1);
			REQUIRE_THAT(state.query_end_errors[0], Contains(test.FirstError()));
		});
	}

	test.ForEachState([](FailingStatementEndState &state) {
		REQUIRE(state.statement_ends == 1);
		REQUIRE(state.query_ends == 1);
	});
	REQUIRE(!conn.context->transaction.HasActiveTransaction());
	// the fatal error invalidated the database, whichever error the result reported
	auto next = conn.Query("SELECT 42");
	REQUIRE(next->HasError());
	REQUIRE_THAT(next->GetError(), Contains("invalidated"));
}
