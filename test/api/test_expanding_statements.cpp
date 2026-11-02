#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb.h"
#include "duckdb/common/arrow/arrow_format.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/execution/executor.hpp"
#include "duckdb/execution/operator/helper/physical_result_collector.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/prepared_statement_data.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/main/query_result_stream.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/multi_statement.hpp"
#include "result_wait_helpers.hpp"

#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>

using namespace duckdb;

namespace {

constexpr const char *SALES_PIVOT = "PIVOT sales ON year USING sum(amount) ORDER BY city";
constexpr const char *VOLATILE_DEFAULT = "ALTER TABLE t ADD COLUMN c DOUBLE DEFAULT random()";
constexpr const char *NOT_NULL_COLUMN = "ALTER TABLE t ADD COLUMN n INTEGER NOT NULL DEFAULT 1";
//! Expands like VOLATILE_DEFAULT; its UPDATE fails to bind after the column was added
constexpr const char *FAILING_DEFAULT = "ALTER TABLE t ADD COLUMN c INTEGER DEFAULT nonexistent_function()";
//! Its CREATE TYPE runs, then its SELECT fails to bind
constexpr const char *FAILING_PIVOT = "PIVOT sales ON year USING sum(amount) ORDER BY missing_column";
//! Binder and catalog errors leave a transaction usable, so what invalidates it is the rule for a group alone
constexpr const char *SYNTACTIC_ERRORS_POLICY =
    "SET default_transaction_invalidation_policy = 'SYNTACTIC_ERRORS_DO_NOT_INVALIDATE'";

void CreateSales(Connection &con) {
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE sales(city VARCHAR, year INTEGER, amount INTEGER)"));
	REQUIRE_NO_FAIL(con.Query(
	    "INSERT INTO sales VALUES ('ams', 2023, 10), ('ams', 2024, 20), ('rtm', 2023, 30), ('rtm', 2024, 40)"));
}

void CreateNumbers(Connection &con, idx_t count) {
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT range AS i FROM range(" + std::to_string(count) + ")"));
}

unique_ptr<QueryResult> SubmitGroup(Connection &con, const string &query) {
	auto handle = con.Submit(query);
	if (handle->HasError()) {
		FAIL(handle->GetError());
	}
	return handle;
}

int64_t Count(Connection &con, const string &query) {
	auto result = con.Query(query);
	REQUIRE_NO_FAIL(*result);
	return result->Collection().GetRows().GetValue(0, 0).GetValue<int64_t>();
}

int64_t PivotEnumCount(Connection &con) {
	return Count(con, "SELECT count(*) FROM duckdb_types() WHERE type_name LIKE '__pivot_enum_%'");
}

bool ColumnExists(Connection &con, const string &column) {
	return Count(con, "SELECT count(*) FROM duckdb_columns() WHERE table_name = 't' AND column_name = '" + column +
	                      "'") == 1;
}

vector<string> Names(const vector<Identifier> &identifiers) {
	vector<string> names;
	for (auto &name : identifiers) {
		names.push_back(name.GetIdentifierName());
	}
	return names;
}

vector<string> Names(const QueryResult &result) {
	return Names(result.GetNames());
}

void RequireMetadataDeferred(QueryResult &handle) {
	auto unavailable = Catch::Contains("The metadata of this result is not available yet");
	REQUIRE(!handle.MetadataAvailable());
	REQUIRE_THROWS_WITH(handle.GetTypes(), unavailable);
	REQUIRE_THROWS_WITH(handle.GetNames(), unavailable);
	REQUIRE_THROWS_WITH(handle.ColumnCount(), unavailable);
	REQUIRE_THROWS_WITH(handle.GetStatementProperties(), unavailable);
}

QueryResultState StepToEnd(QueryResult &handle) {
	Deadline deadline;
	QueryResultState state;
	while (!IsTerminal(state = handle.ExecuteTask())) {
		REQUIRE(!deadline.Passed());
		if (state == QueryResultState::BLOCKED || state == QueryResultState::NO_TASKS_AVAILABLE) {
			handle.WaitForTask();
		}
	}
	return state;
}

//! Steps until the statement that produces the result is bound, or the query ends without one
QueryResultState StepToMetadata(QueryResult &handle) {
	Deadline deadline;
	auto state = QueryResultState::NOT_READY;
	while (!handle.MetadataAvailable() && !IsTerminal(state = handle.ExecuteTask())) {
		REQUIRE(!deadline.Passed());
		if (state == QueryResultState::BLOCKED || state == QueryResultState::NO_TASKS_AVAILABLE) {
			handle.WaitForTask();
		}
	}
	return state;
}

void StepUnfinished(QueryResult &handle) {
	for (idx_t step = 0; step < 5; step++) {
		REQUIRE(!IsTerminal(handle.ExecuteTask()));
	}
}

void RequireAborted(Connection &con) {
	auto next = con.Query("SELECT 42");
	REQUIRE(next->HasError());
	REQUIRE(StringUtil::Contains(next->GetError(), "aborted"));
	REQUIRE_NO_FAIL(con.Query("ROLLBACK"));
}

void RequireRefused(const ErrorData &error) {
	REQUIRE(error.HasError());
	REQUIRE(error.Type() == ExceptionType::RESOURCE_IN_USE);
}

unique_ptr<SQLStatement> ParseOne(Connection &con, const string &query) {
	Parser parser(*con.context);
	parser.ParseQuery(query);
	REQUIRE(parser.statements.size() == 1);
	return std::move(parser.statements[0]);
}

//! A statement that preprocessing unpacks into the given ones, the shape the transformer builds for a dynamic PIVOT
unique_ptr<SQLStatement> MultiOf(Connection &con, const vector<string> &queries) {
	auto multi = make_uniq<MultiStatement>();
	for (auto &query : queries) {
		multi->statements.push_back(ParseOne(con, query));
		multi->query += query + ";";
	}
	return std::move(multi);
}

class QueryCounter : public ClientContextState {
public:
	using ClientContextState::QueryEnd;
	void QueryBegin(ClientContext &context) override {
		begins++;
	}
	void QueryEnd(ClientContext &context, optional_ptr<ErrorData> error) override {
		ends++;
	}

	idx_t begins = 0;
	idx_t ends = 0;
};

class OwnResultState : public GlobalSinkState {
public:
	OwnResultState(ClientContext &context, const vector<LogicalType> &types)
	    : collection(make_uniq<ColumnDataCollection>(Allocator::DefaultAllocator(), types)),
	      client_properties(context.GetClientProperties()) {
		collection->InitializeAppend(append_state);
	}

	unique_ptr<ColumnDataCollection> collection;
	ColumnDataAppendState append_state;
	ClientProperties client_properties;
};

//! Builds its own result object, as the collector quack's server installs does
class OwnResultCollector : public PhysicalResultCollector {
public:
	OwnResultCollector(PhysicalPlan &physical_plan, PreparedStatementData &data)
	    : PhysicalResultCollector(physical_plan, data) {
	}

public:
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override {
		return make_uniq<OwnResultState>(context, types);
	}

	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override {
		auto &gstate = input.global_state.Cast<OwnResultState>();
		gstate.collection->Append(gstate.append_state, chunk);
		return SinkResultType::NEED_MORE_INPUT;
	}

	unique_ptr<QueryResult> GetResult(GlobalSinkState &state) const override {
		auto &gstate = state.Cast<OwnResultState>();
		return make_uniq<QueryResult>(statement_type, properties, names, std::move(gstate.collection),
		                              gstate.client_properties);
	}
};

} // namespace

#ifndef DUCKDB_NO_THREADS

//===--------------------------------------------------------------------===//
// One submission
//===--------------------------------------------------------------------===//
TEST_CASE("A dynamic PIVOT is one submitted query", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	CreateSales(con);

	auto handle = SubmitGroup(con, SALES_PIVOT);
	handle->Complete();
	REQUIRE_NO_FAIL(*handle);
	REQUIRE(Names(*handle) == vector<string> {"city", "2023", "2024"});
	REQUIRE(handle->GetTypes() ==
	        vector<LogicalType> {LogicalType::VARCHAR, LogicalType::HUGEINT, LogicalType::HUGEINT});
	REQUIRE(CHECK_COLUMN(handle, 0, {"ams", "rtm"}));
	REQUIRE(CHECK_COLUMN(handle, 1, {10, 30}));
	REQUIRE(CHECK_COLUMN(handle, 2, {20, 40}));
}

TEST_CASE("An ALTER that expands is one submitted query", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	Connection observer(db);
	CreateNumbers(con, 10);

	SECTION("a volatile default") {
		auto handle = SubmitGroup(con, VOLATILE_DEFAULT);
		handle->Complete();
		REQUIRE_NO_FAIL(*handle);
		REQUIRE(Count(observer, "SELECT count(*) FROM t WHERE c IS NULL") == 0);
		REQUIRE_NO_FAIL(con.Query("INSERT INTO t (i) VALUES (10)"));
		REQUIRE(Count(observer, "SELECT count(*) FROM t WHERE c IS NULL") == 0);
	}
	SECTION("a NOT NULL column") {
		auto handle = SubmitGroup(con, NOT_NULL_COLUMN);
		handle->Complete();
		REQUIRE_NO_FAIL(*handle);
		REQUIRE(Count(observer, "SELECT count(*) FROM t WHERE n = 1") == 10);
		REQUIRE_FAIL(con.Query("INSERT INTO t (i, n) VALUES (10, NULL)"));
	}
}

TEST_CASE("IMPORT DATABASE is one submitted query", "[api][expanding_statements]") {
	auto path = TestCreatePath("expanding_statements_import");
	{
		DuckDB source(nullptr);
		Connection con(source);
		CreateSales(con);
		CreateNumbers(con, 10);
		REQUIRE_NO_FAIL(con.Query("EXPORT DATABASE '" + path + "'"));
	}
	DuckDB db(nullptr);
	Connection con(db);
	auto handle = SubmitGroup(con, "IMPORT DATABASE '" + path + "'");
	handle->Complete();
	REQUIRE_NO_FAIL(*handle);
	REQUIRE(Count(con, "SELECT count(*) FROM sales") == 4);
	REQUIRE(Count(con, "SELECT count(*) FROM t") == 10);
}

TEST_CASE("Submitting a parsed statement that expands runs it", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	CreateSales(con);
	auto statement = ParseOne(con, SALES_PIVOT);
	REQUIRE(statement->type == StatementType::MULTI_STATEMENT);

	auto handle = con.Submit(std::move(statement));
	REQUIRE_NO_FAIL(*handle);
	handle->Complete();
	REQUIRE_NO_FAIL(*handle);
	REQUIRE(CHECK_COLUMN(handle, 0, {"ams", "rtm"}));
}

TEST_CASE("A PRAGMA reparsed into one statement reports that statement", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	CreateSales(con);
	auto handle = SubmitGroup(con, "PRAGMA show_tables");
	REQUIRE(handle->MetadataAvailable());
	REQUIRE(handle->GetStatementType() == StatementType::SELECT_STATEMENT);
	// What consumers that stream a SELECT by default rely on
	REQUIRE(StreamsByDefault(handle->GetStatementType(), *handle));
	handle->Complete();
	REQUIRE(CHECK_COLUMN(handle, 0, {"sales"}));
}

TEST_CASE("A statement that preprocesses into nothing finishes empty", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto handle = con.Submit(make_uniq<MultiStatement>());
	REQUIRE_NO_FAIL(*handle);
	REQUIRE(handle->GetStatementType() == StatementType::MULTI_STATEMENT);
	REQUIRE(handle->MetadataAvailable());
	REQUIRE(handle->ColumnCount() == 0);
	REQUIRE(handle->Poll() == QueryResultState::FINISHED);
	REQUIRE(!handle->IsOpen());
	REQUIRE(Count(con, "SELECT 42") == 42);
}

//===--------------------------------------------------------------------===//
// The group's transaction
//===--------------------------------------------------------------------===//
TEST_CASE("Polling a group to FINISHED does not commit it", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	Connection observer(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=2"));
	CreateNumbers(con, 1000);

	auto handle = SubmitGroup(con, VOLATILE_DEFAULT);
	handle->Materialize();
	Deadline deadline;
	while (handle->Poll() != QueryResultState::FINISHED) {
		REQUIRE(!deadline.Passed());
		std::this_thread::sleep_for(std::chrono::microseconds(100));
	}
	REQUIRE(handle->IsOpen());
	handle->Close();
	REQUIRE(!ColumnExists(observer, "c"));
}

TEST_CASE("Stepping a group to FINISHED commits it", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	Connection observer(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	CreateNumbers(con, 1000);

	auto handle = SubmitGroup(con, VOLATILE_DEFAULT);
	REQUIRE(StepToEnd(*handle) == QueryResultState::FINISHED);
	REQUIRE(!handle->IsOpen());
	REQUIRE(ColumnExists(observer, "c"));
	REQUIRE(Count(observer, "SELECT count(*) FROM t WHERE c IS NULL") == 0);
}

//===--------------------------------------------------------------------===//
// Driving a group
//===--------------------------------------------------------------------===//
TEST_CASE("A poll-only consumer reaches the result statement of a group", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=2"));
	CreateSales(con);

	auto handle = SubmitGroup(con, SALES_PIVOT);
	Deadline deadline;
	while (!handle->MetadataAvailable()) {
		REQUIRE(!IsTerminal(handle->Poll()));
		REQUIRE(!deadline.Passed());
		std::this_thread::sleep_for(std::chrono::microseconds(100));
	}
	REQUIRE(Names(*handle) == vector<string> {"city", "2023", "2024"});
	handle->Materialize();
	while (handle->Poll() != QueryResultState::FINISHED) {
		REQUIRE(!deadline.Passed());
		std::this_thread::sleep_for(std::chrono::microseconds(100));
	}
	REQUIRE(CHECK_COLUMN(handle, 0, {"ams", "rtm"}));
}

TEST_CASE("A single-threaded consumer drives a group itself", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	CreateSales(con);

	auto handle = SubmitGroup(con, SALES_PIVOT);
	REQUIRE(!IsTerminal(StepToMetadata(*handle)));
	REQUIRE(handle->MetadataAvailable());
	handle->Complete();
	REQUIRE_NO_FAIL(*handle);
	REQUIRE(CHECK_COLUMN(handle, 0, {"ams", "rtm"}));
}

TEST_CASE("A single-threaded consumer sees a later statement's bind failure", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	CreateNumbers(con, 10);

	auto handle = SubmitGroup(con, FAILING_DEFAULT);
	REQUIRE(StepToEnd(*handle) == QueryResultState::EXECUTION_ERROR);
	REQUIRE(handle->GetErrorType() == ExceptionType::CATALOG);
	REQUIRE(StringUtil::Contains(handle->GetError(), "nonexistent_function"));
	REQUIRE(!handle->IsOpen());
	REQUIRE(!ColumnExists(con, "c"));
}

TEST_CASE("ExecutionIsFinished stays false between the statements of a group", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	CreateNumbers(con, 100000);

	auto handle = SubmitGroup(con, VOLATILE_DEFAULT);
	Deadline deadline;
	while (true) {
		auto state = handle->ExecuteTask();
		if (IsTerminal(state)) {
			REQUIRE(state == QueryResultState::FINISHED);
			break;
		}
		if (con.context->ExecutionIsFinished()) {
			REQUIRE(handle->ExecuteTask() == QueryResultState::FINISHED);
			break;
		}
		REQUIRE(!deadline.Passed());
	}
	REQUIRE(ColumnExists(con, "c"));
}

//===--------------------------------------------------------------------===//
// The result statement
//===--------------------------------------------------------------------===//
TEST_CASE("EXPLAIN of a dynamic PIVOT returns the plan rows", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	CreateSales(con);

	auto handle = SubmitGroup(con, "EXPLAIN PIVOT sales ON year USING sum(amount)");
	REQUIRE(handle->GetStatementType() == StatementType::MULTI_STATEMENT);
	handle->Complete();
	REQUIRE_NO_FAIL(*handle);
	REQUIRE(Names(*handle) == vector<string> {"explain_key", "explain_value"});
	REQUIRE(handle->RowCount() == 1);
}

TEST_CASE("A row-returning statement followed by another is refused before any row", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i INTEGER)"));

	unique_ptr<QueryResult> handle;
	SECTION("as the first statement") {
		handle = con.Submit(MultiOf(con, {"INSERT INTO t VALUES (1) RETURNING i", "CREATE TABLE u(i INTEGER)"}));
	}
	SECTION("after an earlier statement ran") {
		handle = con.Submit(MultiOf(
		    con, {"CREATE TABLE u(i INTEGER)", "INSERT INTO t VALUES (1) RETURNING i", "CREATE TABLE w(i INTEGER)"}));
		handle->Complete();
	}
	REQUIRE(handle->HasError());
	REQUIRE(handle->GetErrorType() == ExceptionType::NOT_IMPLEMENTED);
	REQUIRE(StringUtil::Contains(handle->GetError(), "followed by others"));
	REQUIRE(Count(con, "SELECT count(*) FROM t") == 0);
	REQUIRE(Count(con, "SELECT count(*) FROM duckdb_tables() WHERE table_name IN ('u', 'w')") == 0);
}

TEST_CASE("Query refuses a statement that expands into two row-returning statements", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto result = con.Query(MultiOf(con, {"SELECT 1", "SELECT 2"}));
	REQUIRE(result->HasError());
	REQUIRE(result->GetErrorType() == ExceptionType::NOT_IMPLEMENTED);
	REQUIRE(StringUtil::Contains(result->GetError(), "followed by others"));
}

//===--------------------------------------------------------------------===//
// Metadata
//===--------------------------------------------------------------------===//
TEST_CASE("A group's metadata is deferred until its result statement is bound", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	CreateSales(con);

	auto handle = SubmitGroup(con, SALES_PIVOT);
	REQUIRE(handle->GetStatementType() == StatementType::MULTI_STATEMENT);
	RequireMetadataDeferred(*handle);

	REQUIRE(!IsTerminal(StepToMetadata(*handle)));
	REQUIRE(handle->MetadataAvailable());
	REQUIRE(handle->GetStatementType() == StatementType::MULTI_STATEMENT);
	REQUIRE(handle->GetStatementProperties().return_type == StatementReturnType::QUERY_RESULT);
	REQUIRE(Names(*handle) == vector<string> {"city", "2023", "2024"});
	REQUIRE(handle->GetTypes() ==
	        vector<LogicalType> {LogicalType::VARCHAR, LogicalType::HUGEINT, LogicalType::HUGEINT});
	handle->Complete();
	REQUIRE(CHECK_COLUMN(handle, 0, {"ams", "rtm"}));
}

TEST_CASE("A group without rows reports NOTHING when it ends", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	CreateNumbers(con, 10);

	auto handle = SubmitGroup(con, VOLATILE_DEFAULT);
	REQUIRE(handle->GetStatementType() == StatementType::MULTI_STATEMENT);
	Deadline deadline;
	QueryResultState state;
	while (!IsTerminal(state = handle->ExecuteTask())) {
		REQUIRE(!handle->MetadataAvailable());
		REQUIRE(!deadline.Passed());
	}
	REQUIRE(state == QueryResultState::FINISHED);
	REQUIRE(handle->MetadataAvailable());
	auto &properties = handle->GetStatementProperties();
	REQUIRE(properties.return_type == StatementReturnType::NOTHING);
	REQUIRE(!properties.IsReadOnly());
	REQUIRE(Names(*handle) == vector<string> {"Success"});
	REQUIRE(handle->GetTypes() == vector<LogicalType> {LogicalType::BOOLEAN});
	REQUIRE(handle->RowCount() == 0);
	REQUIRE(handle->Collection().ColumnCount() == 1);
}

TEST_CASE("A group's properties include the databases its earlier statements modified", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	CreateSales(con);

	auto handle = SubmitGroup(con, SALES_PIVOT);
	handle->Complete();
	REQUIRE_NO_FAIL(*handle);
	auto &properties = handle->GetStatementProperties();
	REQUIRE(properties.modified_databases.count(Identifier("temp")) == 1);
	REQUIRE(properties.read_databases.count(Identifier("memory")) == 1);
	REQUIRE(!properties.IsReadOnly());
}

TEST_CASE("An Arrow result of a group without rows has the Success schema", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	CreateNumbers(con, 10);

	auto handle = con.Submit(VOLATILE_DEFAULT, make_shared_ptr<ArrowFormat>(1024));
	REQUIRE_NO_FAIL(*handle);
	handle->Complete();
	REQUIRE_NO_FAIL(*handle);
	auto &schema = handle->FormatState<ArrowFormat>().Schema();
	REQUIRE(schema.n_children == 1);
	REQUIRE(string(schema.children[0]->name) == "Success");
	REQUIRE(string(schema.children[0]->format) == "b");
	REQUIRE(!handle->Fetch<ArrowFormat>());
}

TEST_CASE("The v1 C API reports MULTI for a dynamic PIVOT", "[api][expanding_statements]") {
	duckdb_database db;
	duckdb_connection con;
	REQUIRE(duckdb_open(nullptr, &db) == DuckDBSuccess);
	REQUIRE(duckdb_connect(db, &con) == DuckDBSuccess);
	REQUIRE(duckdb_query(con, "CREATE TABLE sales(city VARCHAR, year INTEGER, amount INTEGER)", nullptr) ==
	        DuckDBSuccess);
	REQUIRE(duckdb_query(con, "INSERT INTO sales VALUES ('ams', 2023, 10), ('rtm', 2024, 40)", nullptr) ==
	        DuckDBSuccess);

	duckdb_result result;
	auto state = duckdb_query(con, SALES_PIVOT, &result);
	auto statement_type = duckdb_result_statement_type(result);
	auto column_count = duckdb_column_count(&result);
	duckdb_destroy_result(&result);
	duckdb_disconnect(&con);
	duckdb_close(&db);
	REQUIRE(state == DuckDBSuccess);
	REQUIRE(statement_type == DUCKDB_STATEMENT_TYPE_MULTI);
	REQUIRE(column_count == 3);
}

//===--------------------------------------------------------------------===//
// Ending a group early
//===--------------------------------------------------------------------===//
TEST_CASE("An expanded statement abandoned at any step applies all or nothing", "[api][expanding_statements]") {
	bool user_transaction = false;
	SECTION("autocommit") {
	}
	SECTION("inside a user transaction") {
		user_transaction = true;
	}
	bool finished = false;
	for (idx_t steps = 0; !finished; steps++) {
		DuckDB db(nullptr);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("SET threads=1"));
		CreateNumbers(con, 10);
		if (user_transaction) {
			REQUIRE_NO_FAIL(con.Query("BEGIN"));
		}
		auto handle = SubmitGroup(con, VOLATILE_DEFAULT);
		for (idx_t step = 0; step < steps && !finished; step++) {
			finished = handle->ExecuteTask() == QueryResultState::FINISHED;
		}
		handle->Close();
		if (user_transaction) {
			if (finished) {
				REQUIRE_NO_FAIL(con.Query("COMMIT"));
			} else {
				RequireAborted(con);
			}
		}
		REQUIRE(ColumnExists(con, "c") == finished);
		if (finished) {
			REQUIRE(Count(con, "SELECT count(*) FROM t WHERE c IS NULL") == 0);
		}
	}
}

TEST_CASE("A PIVOT that does not finish in autocommit leaves no enum type", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	CreateSales(con);
	auto before = PivotEnumCount(con);

	SECTION("closed before it ran") {
		auto handle = SubmitGroup(con, SALES_PIVOT);
		handle->Close();
	}
	SECTION("closed during its SELECT") {
		auto handle = SubmitGroup(con, SALES_PIVOT);
		REQUIRE(!IsTerminal(StepToMetadata(*handle)));
		handle->Close();
	}
	SECTION("failing in its SELECT") {
		auto handle = SubmitGroup(con, FAILING_PIVOT);
		handle->Complete();
		REQUIRE(handle->GetErrorType() == ExceptionType::BINDER);
	}
	REQUIRE(PivotEnumCount(con) == before);
	REQUIRE(Count(con, "SELECT 42") == 42);
}

TEST_CASE("A PIVOT abandoned during its SELECT keeps the user's transaction", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	Connection observer(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	CreateSales(con);
	REQUIRE_NO_FAIL(con.Query("BEGIN"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO sales VALUES ('utr', 2023, 50)"));

	auto handle = SubmitGroup(con, SALES_PIVOT);
	REQUIRE(!IsTerminal(StepToMetadata(*handle)));
	handle->Close();
	REQUIRE_NO_FAIL(con.Query("COMMIT"));
	REQUIRE(Count(observer, "SELECT count(*) FROM sales") == 5);
}

TEST_CASE("A PIVOT abandoned before its SELECT is bound invalidates the user's transaction",
          "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	Connection observer(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	CreateSales(con);
	REQUIRE_NO_FAIL(con.Query("BEGIN"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO sales VALUES ('utr', 2023, 50)"));

	auto handle = SubmitGroup(con, SALES_PIVOT);
	handle->Close();
	RequireAborted(con);
	REQUIRE(Count(observer, "SELECT count(*) FROM sales") == 4);
}

TEST_CASE("A group abandoned before a writing statement started invalidates the user's transaction",
          "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	Connection observer(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i INTEGER)"));
	auto set_variable = con.Query("SET VARIABLE expanding_marker = 0");
	REQUIRE_NO_FAIL(*set_variable);
	REQUIRE(set_variable->GetStatementProperties().IsReadOnly());
	REQUIRE_NO_FAIL(con.Query("BEGIN"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t VALUES (1)"));

	auto handle = con.Submit(MultiOf(con, {"SET VARIABLE expanding_marker = 1", "CREATE TABLE u(i INTEGER)"}));
	REQUIRE_NO_FAIL(*handle);
	handle->Close();
	RequireAborted(con);
	REQUIRE(Count(observer, "SELECT count(*) FROM t") == 0);
	REQUIRE(Count(observer, "SELECT count(*) FROM duckdb_tables() WHERE table_name = 'u'") == 0);
}

TEST_CASE("A read-only statement before a failing one keeps the user's transaction", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	Connection observer(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i INTEGER)"));
	REQUIRE_NO_FAIL(con.Query(SYNTACTIC_ERRORS_POLICY));
	REQUIRE_NO_FAIL(con.Query("BEGIN"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t VALUES (1)"));

	auto handle = con.Submit(MultiOf(con, {"SET VARIABLE expanding_marker = 1", "SELECT * FROM missing"}));
	REQUIRE_NO_FAIL(*handle);
	handle->Complete();
	REQUIRE(handle->GetErrorType() == ExceptionType::CATALOG);
	REQUIRE_NO_FAIL(con.Query("COMMIT"));
	REQUIRE(Count(observer, "SELECT count(*) FROM t") == 1);
}

TEST_CASE("A group leaves the user's invalidation policy in place", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	Connection observer(db);
	CreateNumbers(con, 10);
	REQUIRE_NO_FAIL(con.Query("BEGIN"));
	REQUIRE_NO_FAIL(con.Query("SET current_transaction_invalidation_policy = 'SYNTACTIC_ERRORS_DO_NOT_INVALIDATE'"));

	REQUIRE_NO_FAIL(con.Query(VOLATILE_DEFAULT));
	REQUIRE_FAIL(con.Query("SELECT * FROM missing"));
	REQUIRE(Count(con, "SELECT 42") == 42);
	REQUIRE_NO_FAIL(con.Query("COMMIT"));
	REQUIRE(ColumnExists(observer, "c"));
}

TEST_CASE("A stream on a group inside a user transaction ends it as the handle does", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	Connection observer(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	CreateSales(con);
	CreateNumbers(con, 10);
	REQUIRE_NO_FAIL(con.Query(SYNTACTIC_ERRORS_POLICY));
	REQUIRE_NO_FAIL(con.Query("BEGIN"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t VALUES (10)"));

	SECTION("closed before its result statement started") {
		QueryResultStream<> stream(SubmitGroup(con, SALES_PIVOT));
		stream.Close();
	}
	SECTION("an earlier statement fails") {
		QueryResultStream<> stream(SubmitGroup(con, FAILING_DEFAULT));
		REQUIRE(!stream.Fetch());
		REQUIRE(stream.GetErrorType() == ExceptionType::CATALOG);
	}
	RequireAborted(con);
	REQUIRE(Count(observer, "SELECT count(*) FROM t") == 10);
	REQUIRE(!ColumnExists(observer, "c"));
}

TEST_CASE("An error inside a group rolls it back", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	Connection observer(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	CreateNumbers(con, 10);

	SECTION("autocommit") {
		auto handle = SubmitGroup(con, FAILING_DEFAULT);
		handle->Complete();
		REQUIRE(handle->GetErrorType() == ExceptionType::CATALOG);
		REQUIRE(Count(con, "SELECT 42") == 42);
	}
	SECTION("inside a user transaction") {
		REQUIRE_NO_FAIL(con.Query(SYNTACTIC_ERRORS_POLICY));
		REQUIRE_NO_FAIL(con.Query("BEGIN"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO t VALUES (10)"));
		auto handle = SubmitGroup(con, FAILING_DEFAULT);
		handle->Complete();
		REQUIRE(handle->GetErrorType() == ExceptionType::CATALOG);
		RequireAborted(con);
		REQUIRE(Count(observer, "SELECT count(*) FROM t") == 10);
	}
	REQUIRE(!ColumnExists(observer, "c"));
}

TEST_CASE("A group whose first statement fails to bind keeps the user's transaction", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	Connection observer(db);
	CreateNumbers(con, 10);
	REQUIRE_NO_FAIL(con.Query(SYNTACTIC_ERRORS_POLICY));
	REQUIRE_NO_FAIL(con.Query("BEGIN"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t VALUES (10)"));

	auto handle = con.Submit("ALTER TABLE missing ADD COLUMN c DOUBLE DEFAULT random()");
	REQUIRE(handle->HasError());
	REQUIRE(handle->GetErrorType() == ExceptionType::CATALOG);
	REQUIRE_NO_FAIL(con.Query("COMMIT"));
	REQUIRE(Count(observer, "SELECT count(*) FROM t") == 11);
}

TEST_CASE("Under the default policy a group whose first statement fails to bind invalidates the user's transaction",
          "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	Connection observer(db);
	CreateNumbers(con, 10);
	REQUIRE_NO_FAIL(con.Query("BEGIN"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t VALUES (10)"));

	auto handle = con.Submit("ALTER TABLE missing ADD COLUMN c DOUBLE DEFAULT random()");
	REQUIRE(handle->HasError());
	REQUIRE(handle->GetErrorType() == ExceptionType::CATALOG);
	RequireAborted(con);
	REQUIRE(Count(observer, "SELECT count(*) FROM t") == 10);
}

TEST_CASE("A PIVOT whose SELECT fails invalidates the user's transaction", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	Connection observer(db);
	CreateSales(con);
	REQUIRE_NO_FAIL(con.Query(SYNTACTIC_ERRORS_POLICY));
	REQUIRE_NO_FAIL(con.Query("BEGIN"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO sales VALUES ('utr', 2023, 50)"));

	auto handle = SubmitGroup(con, FAILING_PIVOT);
	handle->Complete();
	REQUIRE(handle->GetErrorType() == ExceptionType::BINDER);
	RequireAborted(con);
	REQUIRE(Count(observer, "SELECT count(*) FROM sales") == 4);
}

//===--------------------------------------------------------------------===//
// Interrupts
//===--------------------------------------------------------------------===//
TEST_CASE("An interrupt during a group cancels it and rolls it back", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	CreateNumbers(con, 1000000);

	auto handle = SubmitGroup(con, VOLATILE_DEFAULT);
	StepUnfinished(*handle);
	con.Interrupt();
	REQUIRE(StepToEnd(*handle) == QueryResultState::EXECUTION_ERROR);
	REQUIRE(handle->GetErrorType() == ExceptionType::INTERRUPT);
	REQUIRE(!ColumnExists(con, "c"));
}

TEST_CASE("An interrupt on a statement boundary cancels the query", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=2"));
	CreateNumbers(con, 10);

	auto handle = SubmitGroup(con, VOLATILE_DEFAULT);
	// Without a consumer call the group stays on its first statement, which the workers finish
	auto &first_statement = Executor::Get(*con.context);
	Deadline deadline;
	while (!first_statement.ExecutionIsFinished()) {
		REQUIRE(!deadline.Passed());
		std::this_thread::sleep_for(std::chrono::microseconds(100));
	}
	con.Interrupt();
	REQUIRE(handle->Poll() == QueryResultState::EXECUTION_ERROR);
	REQUIRE(handle->GetErrorType() == ExceptionType::INTERRUPT);
	REQUIRE(!ColumnExists(con, "c"));
}

//===--------------------------------------------------------------------===//
// Refusals at submission
//===--------------------------------------------------------------------===//
TEST_CASE("Parameters on a statement that expands are refused", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	CreateSales(con);
	CreateNumbers(con, 10);
	vector<Value> values {Value::INTEGER(1)};

	unique_ptr<QueryResult> handle;
	SECTION("a PIVOT") {
		handle = con.Submit("PIVOT sales ON year USING sum(amount + $1)", values);
	}
	SECTION("an ALTER") {
		handle = con.Submit("ALTER TABLE t ADD COLUMN c INTEGER DEFAULT ((random() * 0)::INTEGER + $1)", values);
	}
	REQUIRE(handle->HasError());
	REQUIRE(StringUtil::Contains(handle->GetError(), "parameters are not supported for a statement that expands"));
	REQUIRE(PivotEnumCount(con) == 0);
	REQUIRE(!ColumnExists(con, "c"));
}

TEST_CASE("A statement that expands into a transaction statement is refused before anything runs",
          "[api][expanding_statements]") {
	auto path = TestCreatePath("expanding_statements_import_with_begin");
	{
		DuckDB source(nullptr);
		Connection con(source);
		CreateSales(con);
		REQUIRE_NO_FAIL(con.Query("EXPORT DATABASE '" + path + "'"));
	}
	auto schema_path = path + "/schema.sql";
	std::stringstream schema;
	schema << std::ifstream(schema_path).rdbuf();
	std::ofstream(schema_path) << "BEGIN TRANSACTION;\n" << schema.str();

	DuckDB db(nullptr);
	Connection con(db);
	bool explicit_transaction = false;
	SECTION("autocommit") {
	}
	SECTION("inside a user transaction") {
		explicit_transaction = true;
		REQUIRE_NO_FAIL(con.Query("BEGIN"));
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE kept(i INTEGER)"));
	}
	auto handle = con.Submit("IMPORT DATABASE '" + path + "'");
	REQUIRE(handle->HasError());
	REQUIRE(StringUtil::Contains(handle->GetError(), "cannot contain a transaction statement"));
	handle.reset();
	if (explicit_transaction) {
		// The refusal is invalid input, which aborts the user's transaction like any other
		REQUIRE_FAIL(con.Query("SELECT 42"));
		REQUIRE_NO_FAIL(con.Query("ROLLBACK"));
	}
	REQUIRE(Count(con, "SELECT count(*) FROM duckdb_tables() WHERE table_name IN ('sales', 'kept')") == 0);
	REQUIRE_THROWS_WITH(con.context->BindStatement(ParseOne(con, "IMPORT DATABASE '" + path + "'")),
	                    Catch::Contains("cannot contain a transaction statement"));
}

TEST_CASE("A group that fails before its metadata is known reports like any failed result",
          "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	CreateSales(con);

	auto handle = SubmitGroup(con, FAILING_PIVOT);
	handle->Complete();
	REQUIRE(handle->HasError());
	REQUIRE(handle->MetadataAvailable());
	REQUIRE(handle->ColumnCount() == 0);
	REQUIRE(handle->GetNames().empty());

	auto result = con.Query(FAILING_PIVOT);
	REQUIRE(result->HasError());
	REQUIRE(result->ColumnCount() == 0);
}

TEST_CASE("The v1 C API reports no columns for a dynamic PIVOT that failed", "[api][expanding_statements]") {
	duckdb_database db;
	duckdb_connection con;
	REQUIRE(duckdb_open(nullptr, &db) == DuckDBSuccess);
	REQUIRE(duckdb_connect(db, &con) == DuckDBSuccess);
	REQUIRE(duckdb_query(con, "CREATE TABLE sales(city VARCHAR, year INTEGER, amount INTEGER)", nullptr) ==
	        DuckDBSuccess);
	REQUIRE(duckdb_query(con, "INSERT INTO sales VALUES ('ams', 2023, 10)", nullptr) == DuckDBSuccess);

	duckdb_result result;
	auto state = duckdb_query(con, FAILING_PIVOT, &result);
	auto column_count = duckdb_column_count(&result);
	duckdb_destroy_result(&result);
	duckdb_disconnect(&con);
	duckdb_close(&db);
	REQUIRE(state == DuckDBError);
	REQUIRE(column_count == 0);
}

TEST_CASE("A collector hook sees only the result statement of a group", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	CreateSales(con);
	CreateNumbers(con, 10);

	vector<StatementType> consulted;
	auto &config = ClientConfig::GetConfig(*con.context);
	config.get_result_collector = [&consulted](ClientContext &context,
	                                           PreparedStatementData &data) -> unique_ptr<PhysicalOperator> {
		consulted.push_back(data.statement_type);
		return PhysicalResultCollector::GetResultCollector(context, data);
	};
	SECTION("a PIVOT through Submit") {
		auto handle = SubmitGroup(con, SALES_PIVOT);
		REQUIRE(handle->MetadataAvailable());
		REQUIRE(consulted == vector<StatementType> {StatementType::SELECT_STATEMENT});
		handle->Complete();
		REQUIRE(CHECK_COLUMN(handle, 0, {"ams", "rtm"}));
	}
	SECTION("a PIVOT through Query") {
		auto result = con.Query(SALES_PIVOT);
		REQUIRE(CHECK_COLUMN(result, 0, {"ams", "rtm"}));
		REQUIRE(consulted == vector<StatementType> {StatementType::SELECT_STATEMENT});
	}
	SECTION("a group without rows") {
		auto result = con.Query(VOLATILE_DEFAULT);
		REQUIRE_NO_FAIL(*result);
		REQUIRE(consulted.empty());
	}
	config.get_result_collector = nullptr;
}

TEST_CASE("A collector that builds its own result completes a group inside the submission",
          "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	CreateSales(con);

	auto &config = ClientConfig::GetConfig(*con.context);
	config.get_result_collector = [](ClientContext &context,
	                                 PreparedStatementData &data) -> unique_ptr<PhysicalOperator> {
		if (data.properties.return_type != StatementReturnType::QUERY_RESULT) {
			return PhysicalResultCollector::GetResultCollector(context, data);
		}
		return make_uniq<OwnResultCollector>(*data.physical_plan, data);
	};
	auto handle = con.Submit(SALES_PIVOT);
	config.get_result_collector = nullptr;
	REQUIRE_NO_FAIL(*handle);
	REQUIRE(!handle->IsOpen());
	REQUIRE(CHECK_COLUMN(handle, 0, {"ams", "rtm"}));
	REQUIRE(PivotEnumCount(con) == 1);
}

//===--------------------------------------------------------------------===//
// Per query and per statement
//===--------------------------------------------------------------------===//
TEST_CASE("Profiling a statement that expands records one profile per statement", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	CreateSales(con);
	CreateNumbers(con, 10);
	REQUIRE_NO_FAIL(con.Query("PRAGMA enable_profiling='no_output'"));
	auto &profiler = QueryProfiler::Get(*con.context);

	auto pivot = SubmitGroup(con, SALES_PIVOT);
	pivot->Complete();
	REQUIRE_NO_FAIL(*pivot);
	REQUIRE(StringUtil::StartsWith(profiler.GetQuerySQL(), "SELECT"));
	REQUIRE(StringUtil::Contains(profiler.GetQuerySQL(), "__pivot_enum_"));
	pivot.reset();

	auto alter = SubmitGroup(con, VOLATILE_DEFAULT);
	alter->Complete();
	REQUIRE_NO_FAIL(*alter);
	REQUIRE(StringUtil::StartsWith(profiler.GetQuerySQL(), "ALTER"));
}

TEST_CASE("The query log records each statement of a group", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	CreateSales(con);
	REQUIRE_NO_FAIL(con.Query("CALL enable_logging('QueryLog')"));

	auto handle = SubmitGroup(con, SALES_PIVOT);
	handle->Complete();
	REQUIRE_NO_FAIL(*handle);
	REQUIRE_NO_FAIL(con.Query("CALL disable_logging()"));
	REQUIRE(Count(con, "SELECT count(*) FROM duckdb_logs WHERE type = 'QueryLog' AND message LIKE '%__pivot_enum_%'") ==
	        2);
}

TEST_CASE("max_execution_time bounds the whole group", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	CreateNumbers(con, 10);
	REQUIRE_NO_FAIL(con.Query("SET max_execution_time=20"));

	auto handle = SubmitGroup(con, VOLATILE_DEFAULT);
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	REQUIRE(StepToEnd(*handle) == QueryResultState::EXECUTION_ERROR);
	REQUIRE(StringUtil::Contains(handle->GetError(), "maximum execution time"));
	REQUIRE_NO_FAIL(con.Query("SET max_execution_time=0"));
	REQUIRE(!ColumnExists(con, "c"));
}

TEST_CASE("Registered states see one QueryBegin and QueryEnd per group", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	CreateSales(con);
	auto counter = con.context->registered_state->GetOrCreate<QueryCounter>("expanding_statements_query_counter");

	auto handle = SubmitGroup(con, SALES_PIVOT);
	handle->Complete();
	REQUIRE_NO_FAIL(*handle);
	REQUIRE(counter->begins == 1);
	REQUIRE(counter->ends == 1);
}

//===--------------------------------------------------------------------===//
// The connection and Query
//===--------------------------------------------------------------------===//
TEST_CASE("A group holds the connection between its statements", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	CreateNumbers(con, 100000);

	auto handle = SubmitGroup(con, VOLATILE_DEFAULT);
	Deadline deadline;
	QueryResultState state;
	idx_t checked_steps = 0;
	while (!IsTerminal(state = handle->ExecuteTask())) {
		RequireRefused(con.Query("SELECT 1")->GetErrorObject());
		RequireRefused(con.Prepare("SELECT 1")->GetErrorObject());
		auto signature = con.context->BindStatement(ParseOne(con, "SELECT 1"));
		REQUIRE(signature.types == vector<LogicalType> {LogicalType::INTEGER});
		REQUIRE(con.TableInfo(Identifier("main"), Identifier("t")));
		checked_steps++;
		REQUIRE(!deadline.Passed());
	}
	REQUIRE(state == QueryResultState::FINISHED);
	REQUIRE(checked_steps > 0);
	REQUIRE(Count(con, "SELECT count(*) FROM t WHERE c IS NULL") == 0);
}

TEST_CASE("Query on a statement that expands matches the reference", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	CreateSales(con);
	CreateNumbers(con, 10);

	SECTION("a PIVOT") {
		auto result = con.Query(SALES_PIVOT);
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->GetStatementType() == StatementType::MULTI_STATEMENT);
		REQUIRE(Names(*result) == vector<string> {"city", "2023", "2024"});
		REQUIRE(result->GetTypes() ==
		        vector<LogicalType> {LogicalType::VARCHAR, LogicalType::HUGEINT, LogicalType::HUGEINT});
		REQUIRE(CHECK_COLUMN(result, 0, {"ams", "rtm"}));
		REQUIRE(CHECK_COLUMN(result, 1, {10, 30}));
		REQUIRE(CHECK_COLUMN(result, 2, {20, 40}));
	}
	SECTION("an ALTER") {
		auto result = con.Query(VOLATILE_DEFAULT);
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->GetStatementType() == StatementType::MULTI_STATEMENT);
		REQUIRE(result->GetStatementProperties().return_type == StatementReturnType::NOTHING);
		REQUIRE(Names(*result) == vector<string> {"Success"});
		REQUIRE(Count(con, "SELECT count(*) FROM t WHERE c IS NULL") == 0);
	}
}

//===--------------------------------------------------------------------===//
// Streams
//===--------------------------------------------------------------------===//
TEST_CASE("A stream opened on a group drains the rows of its result statement", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	CreateSales(con);

	QueryResultStream<> stream(SubmitGroup(con, SALES_PIVOT));
	REQUIRE(!stream.MetadataAvailable());
	idx_t rows = 0;
	while (auto chunk = stream.Fetch()) {
		rows += chunk->size();
	}
	REQUIRE(!stream.HasError());
	REQUIRE(rows == 2);
	REQUIRE(stream.MetadataAvailable());
	REQUIRE(stream.ColumnCount() == 3);
	REQUIRE(!stream.IsOpen());
}

TEST_CASE("Closing a stream on a group before its result statement started ends it", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	CreateSales(con);
	auto before = PivotEnumCount(con);

	{
		QueryResultStream<> stream(SubmitGroup(con, SALES_PIVOT));
		SECTION("before anything ran") {
		}
		SECTION("after an earlier statement ran") {
			REQUIRE(!stream.MetadataAvailable());
			REQUIRE(!IsTerminal(stream.ExecuteTask()));
			REQUIRE(!stream.MetadataAvailable());
		}
		stream.Close();
		REQUIRE(!stream.IsOpen());
	}
	REQUIRE(PivotEnumCount(con) == before);
	REQUIRE(Count(con, "SELECT 42") == 42);
}

TEST_CASE("A stream on a group without rows ends empty and keeps its effect", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	Connection observer(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	CreateNumbers(con, 10);

	QueryResultStream<> stream(SubmitGroup(con, VOLATILE_DEFAULT));
	SECTION("through Fetch") {
		REQUIRE(!stream.Fetch());
	}
	SECTION("through TryFetch") {
		Deadline deadline;
		unique_ptr<DataChunk> chunk;
		QueryResultState state;
		while (!IsTerminal(state = stream.TryFetch(chunk))) {
			REQUIRE(!chunk);
			REQUIRE(!deadline.Passed());
			stream.ExecuteTask();
		}
		REQUIRE(state == QueryResultState::FINISHED);
	}
	REQUIRE(!stream.HasError());
	REQUIRE(!stream.IsOpen());
	REQUIRE(stream.MetadataAvailable());
	REQUIRE(stream.GetStatementProperties().return_type == StatementReturnType::NOTHING);
	REQUIRE(Count(observer, "SELECT count(*) FROM t WHERE c IS NULL") == 0);
}

TEST_CASE("TryFetch on a group reaches the rows of its result statement", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=2"));
	CreateSales(con);

	QueryResultStream<> stream(SubmitGroup(con, SALES_PIVOT));
	Deadline deadline;
	idx_t rows = 0;
	unique_ptr<DataChunk> chunk;
	QueryResultState state;
	while (!IsTerminal(state = stream.TryFetch(chunk))) {
		// A unit comes with READY and only with READY
		REQUIRE((state == QueryResultState::READY) == (chunk != nullptr));
		if (chunk) {
			rows += chunk->size();
		}
		REQUIRE(!deadline.Passed());
		std::this_thread::sleep_for(std::chrono::microseconds(100));
	}
	REQUIRE(state == QueryResultState::FINISHED);
	REQUIRE(rows == 2);
	REQUIRE(stream.GetNames()[0].GetIdentifierName() == "city");
}

TEST_CASE("A stream does not open on a group that is retained", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	CreateSales(con);

	auto handle = SubmitGroup(con, SALES_PIVOT);
	handle->Materialize();
	REQUIRE_THROWS_WITH(QueryResultStream<>(std::move(handle)), Catch::Contains("being retained"));
	REQUIRE(Count(con, "SELECT 42") == 42);
}

TEST_CASE("An interrupt reaches a stream on a group", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));

	SECTION("while an earlier statement runs") {
		CreateNumbers(con, 1000000);
		QueryResultStream<> stream(SubmitGroup(con, VOLATILE_DEFAULT));
		for (idx_t step = 0; step < 5; step++) {
			REQUIRE(!IsTerminal(stream.ExecuteTask()));
		}
		con.Interrupt();
		REQUIRE(!stream.Fetch());
		REQUIRE(stream.GetErrorType() == ExceptionType::INTERRUPT);
	}
	SECTION("after the last statement finished") {
		CreateNumbers(con, 10);
		QueryResultStream<> stream(SubmitGroup(con, VOLATILE_DEFAULT));
		Deadline deadline;
		while (stream.ExecuteTask() != QueryResultState::FINISHED) {
			REQUIRE(!deadline.Passed());
		}
		con.Interrupt();
		unique_ptr<DataChunk> chunk;
		REQUIRE(stream.TryFetch(chunk) == QueryResultState::EXECUTION_ERROR);
		REQUIRE(stream.GetErrorType() == ExceptionType::INTERRUPT);
	}
	REQUIRE(!ColumnExists(con, "c"));
}

TEST_CASE("An error in an earlier statement reaches a stream on a group", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	CreateSales(con);
	CreateNumbers(con, 10);
	auto before = PivotEnumCount(con);

	SECTION("through Fetch") {
		QueryResultStream<> stream(SubmitGroup(con, FAILING_DEFAULT));
		REQUIRE(!stream.Fetch());
		REQUIRE(stream.GetErrorType() == ExceptionType::CATALOG);
		REQUIRE(!stream.IsOpen());
		REQUIRE_THROWS(stream.Fetch());
	}
	SECTION("through TryFetch") {
		QueryResultStream<> stream(SubmitGroup(con, FAILING_PIVOT));
		Deadline deadline;
		unique_ptr<DataChunk> chunk;
		QueryResultState state;
		while (!IsTerminal(state = stream.TryFetch(chunk))) {
			REQUIRE(!deadline.Passed());
			stream.ExecuteTask();
		}
		REQUIRE(state == QueryResultState::EXECUTION_ERROR);
		REQUIRE(stream.GetErrorType() == ExceptionType::BINDER);
		REQUIRE(!stream.IsOpen());
	}
	REQUIRE(!ColumnExists(con, "c"));
	REQUIRE(PivotEnumCount(con) == before);
}

TEST_CASE("A poll-only consumer of a stream on a group reaches its rows", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=2"));
	CreateSales(con);

	QueryResultStream<> stream(SubmitGroup(con, SALES_PIVOT));
	Deadline deadline;
	QueryResultState state;
	while ((state = stream.Poll()) != QueryResultState::READY) {
		REQUIRE(!IsTerminal(state));
		REQUIRE(!deadline.Passed());
		std::this_thread::sleep_for(std::chrono::microseconds(100));
	}
	unique_ptr<DataChunk> chunk;
	REQUIRE(stream.TryFetch(chunk) == QueryResultState::READY);
	REQUIRE(chunk);
	idx_t rows = chunk->size();
	while (auto next = stream.Fetch()) {
		rows += next->size();
	}
	REQUIRE(rows == 2);
}

TEST_CASE("A stream on a group reports its format state once its metadata is known", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	CreateSales(con);
	CreateNumbers(con, 10);

	SECTION("a group without rows") {
		QueryResultStream<> stream(SubmitGroup(con, VOLATILE_DEFAULT));
		REQUIRE_THROWS_WITH(stream.FormatState(), Catch::Contains("not available yet"));
		REQUIRE(!stream.Fetch());
		stream.FormatState();
		REQUIRE(stream.ColumnCount() == 1);
	}
	SECTION("a group that failed") {
		auto handle = SubmitGroup(con, FAILING_PIVOT);
		handle->Complete();
		REQUIRE(handle->HasError());
		handle->FormatState<ChunkFormat>();
	}
	SECTION("an Arrow stream") {
		QueryResultStream<ArrowFormat> stream(con.Submit(SALES_PIVOT, make_shared_ptr<ArrowFormat>(1024)));
		REQUIRE_THROWS_WITH(stream.FormatState(), Catch::Contains("not available yet"));
		idx_t rows = 0;
		while (auto array = stream.Fetch()) {
			rows += NumericCast<idx_t>(array->arrow_array.length);
		}
		REQUIRE(!stream.HasError());
		REQUIRE(rows == 2);
		REQUIRE(stream.FormatState().Schema().n_children == 3);
	}
	SECTION("a stream in another format is refused") {
		REQUIRE_THROWS_WITH(QueryResultStream<ArrowFormat>(SubmitGroup(con, SALES_PIVOT)), Catch::Contains("format"));
	}
}

//===--------------------------------------------------------------------===//
// Outliving the connection
//===--------------------------------------------------------------------===//
TEST_CASE("A handle on a group that outlives its connection ends safely", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	auto con = make_uniq<Connection>(db);
	REQUIRE_NO_FAIL(con->Query("SET threads=2"));
	CreateSales(*con);
	auto handle = SubmitGroup(*con, SALES_PIVOT);
	con.reset();

	SECTION("Complete") {
		handle->Complete();
		REQUIRE(CHECK_COLUMN(handle, 0, {"ams", "rtm"}));
	}
	SECTION("Materialize") {
		handle->Materialize();
		REQUIRE(StepToEnd(*handle) == QueryResultState::FINISHED);
		REQUIRE(handle->RowCount() == 2);
	}
	SECTION("Poll") {
		handle->Materialize();
		Deadline deadline;
		while (handle->Poll() != QueryResultState::FINISHED) {
			REQUIRE(!deadline.Passed());
			std::this_thread::sleep_for(std::chrono::microseconds(100));
		}
		handle->Close();
	}
	SECTION("Close") {
		handle->Close();
	}
	SECTION("a call after the query ended") {
		handle->Complete();
		REQUIRE_NO_FAIL(*handle);
		handle->Materialize();
		handle->Close();
	}
	handle.reset();
	Connection after(db);
	REQUIRE(Count(after, "SELECT 42") == 42);
}

TEST_CASE("A stream on a group that outlives its connection drains safely", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	auto con = make_uniq<Connection>(db);
	REQUIRE_NO_FAIL(con->Query("SET threads=1"));
	CreateSales(*con);
	auto stream = make_uniq<QueryResultStream<>>(SubmitGroup(*con, SALES_PIVOT));
	con.reset();

	idx_t rows = 0;
	while (auto chunk = stream->Fetch()) {
		rows += chunk->size();
	}
	REQUIRE(!stream->HasError());
	REQUIRE(rows == 2);
	stream.reset();
	Connection after(db);
	REQUIRE(Count(after, "SELECT 42") == 42);
}

TEST_CASE("A stream on a group without rows that outlives its connection ends safely", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	auto con = make_uniq<Connection>(db);
	REQUIRE_NO_FAIL(con->Query("SET threads=1"));
	CreateNumbers(*con, 10);
	auto stream = make_uniq<QueryResultStream<>>(SubmitGroup(*con, VOLATILE_DEFAULT));
	con.reset();

	SECTION("through Fetch") {
		REQUIRE(!stream->Fetch());
	}
	SECTION("through TryFetch") {
		Deadline deadline;
		unique_ptr<DataChunk> chunk;
		while (!IsTerminal(stream->TryFetch(chunk))) {
			REQUIRE(!deadline.Passed());
			stream->ExecuteTask();
		}
	}
	REQUIRE(!stream->HasError());
	stream.reset();
	Connection after(db);
	REQUIRE(ColumnExists(after, "c"));
}

//===--------------------------------------------------------------------===//
// Binding
//===--------------------------------------------------------------------===//
TEST_CASE("BindStatement on a reparsed PRAGMA binds the reparsed statement", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	CreateSales(con);

	auto signature = con.context->BindStatement(ParseOne(con, "PRAGMA table_info('sales')"));
	REQUIRE(!signature.metadata_deferred);
	REQUIRE(signature.names.size() == 6);
	REQUIRE(signature.names[0].GetIdentifierName() == "cid");
}

TEST_CASE("BindStatement on a statement that expands reports deferred metadata", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	CreateSales(con);
	CreateNumbers(con, 10);

	SECTION("a PIVOT") {
		auto signature = con.context->BindStatement(ParseOne(con, SALES_PIVOT));
		REQUIRE(signature.metadata_deferred);
		REQUIRE(Names(signature.names) == vector<string> {"unknown"});
		REQUIRE(signature.types == vector<LogicalType> {LogicalTypeId::UNKNOWN});
		REQUIRE(signature.parameters.empty());
	}
	SECTION("an ALTER") {
		auto signature = con.context->BindStatement(ParseOne(con, VOLATILE_DEFAULT));
		REQUIRE(signature.metadata_deferred);
		REQUIRE(Names(signature.names) == vector<string> {"unknown"});
		REQUIRE(signature.types == vector<LogicalType> {LogicalTypeId::UNKNOWN});
	}
	SECTION("with a parameter") {
		REQUIRE_THROWS_WITH(con.context->BindStatement(ParseOne(con, "PIVOT sales ON year USING sum(amount + $1)")),
		                    Catch::Contains("parameters are not supported for a statement that expands"));
	}
	REQUIRE(PivotEnumCount(con) == 0);
	REQUIRE(!ColumnExists(con, "c"));
}

TEST_CASE("BindStatement on a statement that expands into nothing reports no columns", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto signature = con.context->BindStatement(make_uniq<MultiStatement>());
	REQUIRE(!signature.metadata_deferred);
	REQUIRE(signature.names.empty());
	REQUIRE(signature.types.empty());
	REQUIRE(signature.properties.return_type == StatementReturnType::NOTHING);
}

TEST_CASE("BindStatement on a statement whose expansion fails keeps the user's transaction",
          "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	CreateSales(con);

	REQUIRE_NO_FAIL(con.Query("BEGIN"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO sales VALUES ('utr', 2025, 50)"));
	REQUIRE_THROWS_WITH(con.context->BindStatement(ParseOne(con, "PRAGMA no_such_pragma")),
	                    Catch::Contains("no_such_pragma"));
	REQUIRE(Count(con, "SELECT count(*) FROM sales") == 5);
	REQUIRE_NO_FAIL(con.Query("COMMIT"));
	REQUIRE(Count(con, "SELECT count(*) FROM sales") == 5);
}

//===--------------------------------------------------------------------===//
// Preparing
//===--------------------------------------------------------------------===//
namespace {

unique_ptr<PreparedStatement> PrepareGroup(Connection &con, const string &query) {
	auto prepared = con.Prepare(query);
	if (prepared->HasError()) {
		FAIL(prepared->GetError());
	}
	return prepared;
}

void RequirePreparedDeferred(PreparedStatement &prepared, StatementType statement_type) {
	REQUIRE(!prepared.MetadataAvailable());
	REQUIRE(prepared.GetStatementType() == statement_type);
	REQUIRE(prepared.ColumnCount() == 1);
	REQUIRE(prepared.GetNames()[0].GetIdentifierName() == "unknown");
	REQUIRE(prepared.GetTypes() == vector<LogicalType> {LogicalTypeId::UNKNOWN});
	REQUIRE(prepared.GetParameterCount() == 0);
	REQUIRE(prepared.GetStatementProperties().always_require_rebind);
}

} // namespace

TEST_CASE("Preparing a statement that expands runs nothing and defers its metadata", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	CreateSales(con);
	CreateNumbers(con, 10);

	SECTION("a PIVOT") {
		RequirePreparedDeferred(*PrepareGroup(con, SALES_PIVOT), StatementType::MULTI_STATEMENT);
	}
	SECTION("an ALTER") {
		RequirePreparedDeferred(*PrepareGroup(con, VOLATILE_DEFAULT), StatementType::MULTI_STATEMENT);
	}
	SECTION("a parsed PIVOT") {
		auto prepared = con.Prepare(ParseOne(con, SALES_PIVOT));
		REQUIRE(!prepared->HasError());
		RequirePreparedDeferred(*prepared, StatementType::MULTI_STATEMENT);
	}
	REQUIRE(PivotEnumCount(con) == 0);
	REQUIRE(!ColumnExists(con, "c"));
}

TEST_CASE("Executing a prepared statement that expands runs it", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	Connection observer(db);
	CreateSales(con);
	CreateNumbers(con, 10);

	SECTION("a PIVOT") {
		auto result = PrepareGroup(con, SALES_PIVOT)->Execute();
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->GetStatementType() == StatementType::MULTI_STATEMENT);
		REQUIRE(Names(*result) == vector<string> {"city", "2023", "2024"});
		REQUIRE(CHECK_COLUMN(result, 0, {"ams", "rtm"}));
		REQUIRE(CHECK_COLUMN(result, 1, {10, 30}));
		REQUIRE(CHECK_COLUMN(result, 2, {20, 40}));
	}
	SECTION("a parsed PIVOT") {
		auto result = con.Prepare(ParseOne(con, SALES_PIVOT))->Execute();
		REQUIRE_NO_FAIL(*result);
		REQUIRE(CHECK_COLUMN(result, 0, {"ams", "rtm"}));
	}
	SECTION("an ALTER") {
		auto result = PrepareGroup(con, VOLATILE_DEFAULT)->Execute();
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->GetStatementProperties().return_type == StatementReturnType::NOTHING);
		REQUIRE(ColumnExists(observer, "c"));
		REQUIRE(Count(observer, "SELECT count(*) FROM t WHERE c IS NULL") == 0);
	}
}

TEST_CASE("Preparing a statement that expands into nothing defers it", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto prepared = con.Prepare(make_uniq<MultiStatement>());
	REQUIRE(!prepared->HasError());
	REQUIRE(!prepared->MetadataAvailable());
	auto result = prepared->Execute();
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->ColumnCount() == 0);
	REQUIRE(result->GetStatementProperties().return_type == StatementReturnType::NOTHING);

	// In the requested format, as any other result
	vector<Value> no_values;
	auto arrow = prepared->Submit(no_values, QueryParameters(make_shared_ptr<ArrowFormat>(STANDARD_VECTOR_SIZE)));
	REQUIRE_NO_FAIL(*arrow);
	REQUIRE(arrow->FormatState<ArrowFormat>().Schema().n_children == 0);
	REQUIRE(!arrow->Fetch<ArrowFormat>());
}

TEST_CASE("A statement that preprocesses into nothing finishes empty in the requested format",
          "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto handle = con.Submit(make_uniq<MultiStatement>(), make_shared_ptr<ArrowFormat>(STANDARD_VECTOR_SIZE));
	REQUIRE_NO_FAIL(*handle);
	REQUIRE(handle->MetadataAvailable());
	REQUIRE(handle->FormatState<ArrowFormat>().Schema().n_children == 0);
	REQUIRE(!handle->Fetch<ArrowFormat>());
	REQUIRE(!handle->IsOpen());
}

TEST_CASE("Preparing IMPORT DATABASE runs nothing until it is executed", "[api][expanding_statements]") {
	auto path = TestCreatePath("expanding_statements_prepared_import");
	{
		DuckDB source(nullptr);
		Connection con(source);
		CreateSales(con);
		CreateNumbers(con, 10);
		REQUIRE_NO_FAIL(con.Query("EXPORT DATABASE '" + path + "'"));
	}
	DuckDB db(nullptr);
	Connection con(db);
	auto prepared = PrepareGroup(con, "IMPORT DATABASE '" + path + "'");
	RequirePreparedDeferred(*prepared, StatementType::PRAGMA_STATEMENT);
	REQUIRE(Count(con, "SELECT count(*) FROM duckdb_tables()") == 0);

	REQUIRE_NO_FAIL(prepared->Execute());
	REQUIRE(Count(con, "SELECT count(*) FROM sales") == 4);
	REQUIRE(Count(con, "SELECT count(*) FROM t") == 10);
}

TEST_CASE("A prepared statement that expands expands again at each execution", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	CreateSales(con);
	auto prepared = PrepareGroup(con, SALES_PIVOT);

	auto first = prepared->Execute();
	REQUIRE_NO_FAIL(*first);
	REQUIRE(Names(*first) == vector<string> {"city", "2023", "2024"});

	REQUIRE_NO_FAIL(con.Query("INSERT INTO sales VALUES ('ams', 2025, 50)"));
	auto second = prepared->Execute();
	REQUIRE_NO_FAIL(*second);
	REQUIRE(Names(*second) == vector<string> {"city", "2023", "2024", "2025"});
	REQUIRE(CHECK_COLUMN(second, 3, {50, Value()}));
}

TEST_CASE("Submitting a prepared statement that expands defers the result's metadata", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	CreateSales(con);
	auto prepared = PrepareGroup(con, SALES_PIVOT);

	auto handle = prepared->Submit();
	REQUIRE_NO_FAIL(*handle);
	REQUIRE(handle->GetStatementType() == StatementType::MULTI_STATEMENT);
	RequireMetadataDeferred(*handle);
	REQUIRE(!IsTerminal(StepToMetadata(*handle)));
	REQUIRE(Names(*handle) == vector<string> {"city", "2023", "2024"});
	handle->Complete();
	REQUIRE(CHECK_COLUMN(handle, 0, {"ams", "rtm"}));
}

TEST_CASE("A result outlives the prepared statement that expands into it", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	CreateSales(con);
	auto prepared = PrepareGroup(con, SALES_PIVOT);

	auto handle = prepared->Submit();
	REQUIRE_NO_FAIL(*handle);
	prepared.reset();
	handle->Complete();
	REQUIRE_NO_FAIL(*handle);
	REQUIRE(CHECK_COLUMN(handle, 0, {"ams", "rtm"}));
	REQUIRE_NO_FAIL(con.Query("SELECT 42"));
}

TEST_CASE("Parameters on a prepared statement that expands are refused", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	CreateSales(con);
	CreateNumbers(con, 10);
	constexpr const char *PARAMETER_PIVOT = "PIVOT sales ON year USING sum(amount + $1)";

	unique_ptr<PreparedStatement> prepared;
	SECTION("a PIVOT") {
		prepared = con.Prepare(PARAMETER_PIVOT);
	}
	SECTION("a parsed PIVOT") {
		prepared = con.Prepare(ParseOne(con, PARAMETER_PIVOT));
	}
	SECTION("an ALTER") {
		prepared = con.Prepare("ALTER TABLE t ADD COLUMN c INTEGER DEFAULT ((random() * 0)::INTEGER + $1)");
	}
	REQUIRE(prepared->HasError());
	REQUIRE(StringUtil::Contains(prepared->GetError(), "parameters are not supported for a statement that expands"));
	REQUIRE(PivotEnumCount(con) == 0);
	REQUIRE(!ColumnExists(con, "c"));
}

TEST_CASE("Values for a prepared statement that expands are refused", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	CreateSales(con);
	auto prepared = PrepareGroup(con, SALES_PIVOT);

	unique_ptr<QueryResult> result;
	SECTION("executed") {
		result = prepared->Execute(42);
	}
	SECTION("submitted") {
		result = prepared->Submit(42);
	}
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), "parameters are not supported for a statement that expands"));
	REQUIRE(PivotEnumCount(con) == 0);

	auto rows = prepared->Execute();
	REQUIRE_NO_FAIL(*rows);
	REQUIRE(CHECK_COLUMN(rows, 0, {"ams", "rtm"}));
}

TEST_CASE("A prepared statement that expands runs in the transaction of its execution", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	Connection observer(db);
	CreateNumbers(con, 10);

	SECTION("prepared in autocommit, executed inside a user transaction") {
		auto prepared = PrepareGroup(con, VOLATILE_DEFAULT);
		REQUIRE_NO_FAIL(con.Query("BEGIN"));
		REQUIRE_NO_FAIL(prepared->Execute());
		REQUIRE(ColumnExists(con, "c"));
		REQUIRE(!ColumnExists(observer, "c"));
		REQUIRE_NO_FAIL(con.Query("ROLLBACK"));
		REQUIRE(!ColumnExists(con, "c"));
	}
	SECTION("prepared inside a user transaction, executed in autocommit") {
		REQUIRE_NO_FAIL(con.Query("BEGIN"));
		auto prepared = PrepareGroup(con, VOLATILE_DEFAULT);
		REQUIRE_NO_FAIL(con.Query("COMMIT"));
		REQUIRE_NO_FAIL(prepared->Execute());
		REQUIRE(ColumnExists(observer, "c"));
		REQUIRE(Count(observer, "SELECT count(*) FROM t WHERE c IS NULL") == 0);
	}
}

TEST_CASE("A prepared statement reports whether its result schema is known before execution",
          "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);

	SECTION("a SELECT") {
		auto prepared = PrepareGroup(con, "SELECT 42 AS answer");
		REQUIRE(prepared->MetadataAvailable());
		REQUIRE(prepared->GetTypes() == vector<LogicalType> {LogicalType::INTEGER});
	}
	SECTION("a parameter whose type is not known") {
		auto prepared = PrepareGroup(con, "SELECT $1");
		REQUIRE(!prepared->MetadataAvailable());
		REQUIRE(prepared->GetNames()[0].GetIdentifierName() == "unknown");
		REQUIRE(prepared->GetTypes() == vector<LogicalType> {LogicalTypeId::UNKNOWN});
		REQUIRE(prepared->GetParameterCount() == 1);
	}
}

TEST_CASE("Preparing a PRAGMA prepares the statement it reparses into", "[api][expanding_statements]") {
	DuckDB db(nullptr);
	Connection con(db);
	CreateSales(con);

	unique_ptr<PreparedStatement> prepared;
	SECTION("as text") {
		prepared = con.Prepare("PRAGMA show_tables");
	}
	SECTION("parsed") {
		prepared = con.Prepare(ParseOne(con, "PRAGMA show_tables"));
	}
	REQUIRE(!prepared->HasError());
	REQUIRE(prepared->MetadataAvailable());
	REQUIRE(prepared->GetStatementType() == StatementType::SELECT_STATEMENT);
	REQUIRE(prepared->GetNames()[0].GetIdentifierName() == "name");
	auto result = prepared->Execute();
	REQUIRE(CHECK_COLUMN(result, 0, {"sales"}));
}

#endif
