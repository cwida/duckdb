#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb.h"
#include "duckdb_v2.h"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/main/appender.hpp"
#include "duckdb/main/capi/capi_internal.hpp"
#include "duckdb/main/capi_v2/capi_v2_internal.hpp"
#include "duckdb/main/parse_iterator.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/relation.hpp"
#include "duckdb/parser/parser_extension.hpp"
#include "duckdb/parser/peg/passthrough_dialect.hpp"
#include "duckdb/parser/sql_statement.hpp"
#include "duckdb/parser/statement/multi_statement.hpp"
#include "duckdb/parser/statement/select_statement.hpp"

#include <fstream>
#include <functional>
#include <atomic>

using namespace duckdb;
using namespace Catch::Matchers;

//===--------------------------------------------------------------------===//
// Helpers
//===--------------------------------------------------------------------===//

//! Records every statement the post-parse hook it is registered with sees
struct PostParseRecorder : public ParserExtensionInfo {
	string name;
	vector<string> queries;
	vector<StatementType> types;
	//! Whether each statement was a fragment the parser wrapped (PostParseExtensionInput::is_fragment)
	vector<bool> fragments;
	//! Every info pointer the hook received (to check it is the registered one)
	vector<ParserExtensionInfo *> seen_infos;
	//! Shared across recorders to check the order in which hooks run
	shared_ptr<vector<string>> order;
	//! Throw a PermissionException for any statement whose query contains this text
	string reject_containing;
	//! Optional extra check run on every statement
	std::function<void(ClientContext &, const SQLStatement &)> on_statement;

	//! Forget what was recorded so far, such as the statements a test ran to set up
	void Clear() {
		queries.clear();
		types.clear();
		fragments.clear();
		seen_infos.clear();
		if (order) {
			order->clear();
		}
	}
};

static void RecordPostParse(PostParseExtensionInput &input, const SQLStatement &statement) {
	auto info = input.info.get();
	auto &recorder = info->Cast<PostParseRecorder>();
	recorder.queries.push_back(statement.query);
	recorder.types.push_back(statement.type);
	recorder.fragments.push_back(input.is_fragment);
	recorder.seen_infos.push_back(info);
	if (recorder.order) {
		recorder.order->push_back(recorder.name + ":" + statement.query);
	}
	if (recorder.on_statement) {
		recorder.on_statement(input.context, statement);
	}
	if (!recorder.reject_containing.empty() && statement.query.find(recorder.reject_containing) != string::npos) {
		throw PermissionException("post-parse hook rejected: %s", statement.query);
	}
}

static shared_ptr<PostParseRecorder> AddRecorder(DBConfig &config, const string &name = "hook") {
	auto recorder = make_shared_ptr<PostParseRecorder>();
	recorder->name = name;
	PostParseExtension extension;
	extension.post_parse_function = RecordPostParse;
	extension.parser_info = recorder;
	PostParseExtension::Register(config, std::move(extension));
	return recorder;
}

static int64_t CountRows(Connection &con, const string &table) {
	auto result = con.Query("SELECT COUNT(*) FROM " + table);
	REQUIRE_NO_FAIL(*result);
	return result->Collection().GetValue(0, 0).GetValue<int64_t>();
}

//===--------------------------------------------------------------------===//
// 1. Every statement is seen once, in order, with its own query, one statement at a time
//===--------------------------------------------------------------------===//

TEST_CASE("Post-parse hook sees every statement of a multi-statement query in order", "[api][post_parse]") {
	DBConfig config;
	auto recorder = AddRecorder(config);
	DuckDB db(nullptr, &config);
	Connection con(db);
	recorder->Clear();

	REQUIRE_NO_FAIL(con.Query("SELECT 1; SELECT 2;\nSELECT 3"));
	REQUIRE(recorder->queries.size() == 3);
	REQUIRE(recorder->queries[0] == "SELECT 1; ");
	REQUIRE(recorder->queries[1] == "SELECT 2;\n");
	REQUIRE(recorder->queries[2] == "SELECT 3");
	for (auto type : recorder->types) {
		REQUIRE(type == StatementType::SELECT_STATEMENT);
	}

	// separators and empty input produce no calls
	recorder->queries.clear();
	REQUIRE_NO_FAIL(con.Query(";;;"));
	REQUIRE(recorder->queries.empty());
}

TEST_CASE("Post-parse hook sees a PIVOT without an IN list as one MULTI_STATEMENT", "[api][post_parse]") {
	// Preprocessing unpacks a MULTI_STATEMENT after the hook ran: the hook sees its parts only as children
	DBConfig config;
	auto recorder = AddRecorder(config);
	vector<vector<StatementType>> children;
	recorder->on_statement = [&](ClientContext &, const SQLStatement &statement) {
		if (statement.type != StatementType::MULTI_STATEMENT) {
			return;
		}
		vector<StatementType> types;
		for (auto &child : statement.Cast<MultiStatement>().statements) {
			types.push_back(child->type);
		}
		children.push_back(std::move(types));
	};
	DuckDB db(nullptr, &config);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE p(k VARCHAR, v INTEGER); INSERT INTO p VALUES ('a', 1), ('b', 2)"));
	recorder->Clear();

	REQUIRE_NO_FAIL(con.Query("PIVOT p ON k USING sum(v)"));
	REQUIRE(recorder->types == vector<StatementType> {StatementType::MULTI_STATEMENT});
	REQUIRE(children ==
	        vector<vector<StatementType>> {{StatementType::CREATE_STATEMENT, StatementType::SELECT_STATEMENT}});
}

TEST_CASE("Post-parse hook sees each statement of an input after the one before it has run", "[api][post_parse]") {
	DBConfig config;
	auto recorder = AddRecorder(config);
	DuckDB db(nullptr, &config);
	Connection con(db);
	Connection probe(db);

	// A client's input is parsed one statement at a time, and each statement runs before the next one is parsed and
	// passed to the hook. When the hook sees the SELECT, the CREATE before it has already run and committed: a
	// second connection can see the table.
	vector<int64_t> table_visible;
	recorder->on_statement = [&](ClientContext &context, const SQLStatement &statement) {
		if (&context == probe.context.get() || statement.type != StatementType::SELECT_STATEMENT) {
			return;
		}
		auto result = probe.Query("SELECT COUNT(*) FROM duckdb_tables() WHERE table_name = 't'");
		REQUIRE_NO_FAIL(*result);
		table_visible.push_back(result->Collection().GetValue(0, 0).GetValue<int64_t>());
	};
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i INT); SELECT 42"));
	REQUIRE(table_visible.size() == 1);
	REQUIRE(table_visible[0] == 1);
}

TEST_CASE("Post-parse hook registered on an open database runs from the next statement", "[api][post_parse]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SELECT 1"));
	auto recorder = AddRecorder(DBConfig::GetConfig(*db.instance));
	REQUIRE_NO_FAIL(con.Query("SELECT 2"));
	REQUIRE(recorder->queries == vector<string> {"SELECT 2"});
}

//===--------------------------------------------------------------------===//
// 2. Client paths
//===--------------------------------------------------------------------===//

TEST_CASE("Post-parse hook runs on the C++ client paths", "[api][post_parse]") {
	DBConfig config;
	auto recorder = AddRecorder(config);
	DuckDB db(nullptr, &config);
	Connection con(db);
	recorder->Clear();

	SECTION("Query") {
		REQUIRE_NO_FAIL(con.Query("SELECT 1; SELECT 2"));
		REQUIRE(recorder->queries.size() == 2);
	}
	SECTION("Submit") {
		auto result = con.Submit("SELECT 1");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(recorder->queries.size() == 1);
	}
	SECTION("Prepare") {
		auto prepared = con.Prepare("SELECT $1::INTEGER");
		REQUIRE(!prepared->HasError());
		REQUIRE(recorder->queries.size() == 1);
		REQUIRE(recorder->queries[0] == "SELECT $1::INTEGER");
		// executing the prepared statement does not parse again
		REQUIRE_NO_FAIL(prepared->Execute(42));
		REQUIRE(recorder->queries.size() == 1);
	}
	SECTION("ExtractStatements") {
		auto statements = con.ExtractStatements("SELECT 1; SELECT 2; SELECT 3");
		REQUIRE(statements.size() == 3);
		REQUIRE(recorder->queries.size() == 3);
		// executing an extracted statement does not call the hook again
		REQUIRE_NO_FAIL(con.Query(std::move(statements[0])));
		REQUIRE(recorder->queries.size() == 3);
	}
}

TEST_CASE("Post-parse hook runs on the C API v1 paths", "[api][post_parse][capi]") {
	DBConfig config;
	auto recorder = AddRecorder(config);
	auto wrapper = new DatabaseWrapper();
	wrapper->database = make_shared_ptr<DuckDB>(nullptr, &config);
	auto database = reinterpret_cast<duckdb_database>(wrapper);
	duckdb_connection connection = nullptr;
	REQUIRE(duckdb_connect(database, &connection) == DuckDBSuccess);
	recorder->Clear();

	duckdb_result result;
	REQUIRE(duckdb_query(connection, "SELECT 1; SELECT 2", &result) == DuckDBSuccess);
	duckdb_destroy_result(&result);
	REQUIRE(recorder->queries.size() == 2);

	duckdb_prepared_statement prepared = nullptr;
	REQUIRE(duckdb_prepare(connection, "SELECT 3", &prepared) == DuckDBSuccess);
	duckdb_destroy_prepare(&prepared);
	REQUIRE(recorder->queries.size() == 3);

	duckdb_extracted_statements extracted = nullptr;
	REQUIRE(duckdb_extract_statements(connection, "SELECT 4; SELECT 5", &extracted) == 2);
	REQUIRE(recorder->queries.size() == 5);
	// preparing an extracted statement does not parse again
	REQUIRE(duckdb_prepare_extracted_statement(connection, extracted, 0, &prepared) == DuckDBSuccess);
	duckdb_destroy_prepare(&prepared);
	duckdb_destroy_extracted(&extracted);
	REQUIRE(recorder->queries.size() == 5);

	// rejection through the C API
	recorder->reject_containing = "reject_me";
	REQUIRE(duckdb_query(connection, "SELECT 'reject_me'", &result) == DuckDBError);
	REQUIRE_THAT(duckdb_result_error(&result), Contains("post-parse hook rejected"));
	REQUIRE(duckdb_result_error_type(&result) == DUCKDB_ERROR_PERMISSION);
	duckdb_destroy_result(&result);
	REQUIRE(duckdb_prepare(connection, "SELECT 'reject_me'", &prepared) == DuckDBError);
	REQUIRE_THAT(duckdb_prepare_error(prepared), Contains("post-parse hook rejected"));
	duckdb_destroy_prepare(&prepared);
	REQUIRE(duckdb_extract_statements(connection, "SELECT 1; SELECT 'reject_me'", &extracted) == 0);
	REQUIRE_THAT(duckdb_extract_statements_error(extracted), Contains("post-parse hook rejected"));
	duckdb_destroy_extracted(&extracted);

	duckdb_disconnect(&connection);
	duckdb_close(&database);
}

TEST_CASE("Post-parse hook runs on the C API v2 statement iterator", "[api][post_parse][capi_v2]") {
	duckdb_v2_environment_handle env = nullptr;
	duckdb_v2_instance_handle instance = nullptr;
	duckdb_v2_connection_handle conn = nullptr;
	REQUIRE(duckdb_v2_environment_create(&env, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_instance_create(env, &instance, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_str in_memory {nullptr, 0};
	REQUIRE(duckdb_v2_instance_attach(instance, nullptr, &in_memory, nullptr, true, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_create(instance, &conn, nullptr) == DUCKDB_V2_ERROR_NONE);
	// register on the running database
	auto recorder = AddRecorder(DBConfig::GetConfig(*duckdb::capiv2::Convert(conn)->context));
	recorder->reject_containing = "reject_me";

	duckdb_v2_statement_iterator_handle iter = nullptr;
	REQUIRE(duckdb_v2_parse_sql(conn, "SELECT 1; SELECT 2; SELECT 'reject_me'; SELECT 4", &iter, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	// nothing is parsed up front
	REQUIRE(recorder->queries.empty());
	for (idx_t i = 0; i < 2; i++) {
		duckdb_v2_sql_statement_handle stmt = nullptr;
		REQUIRE(duckdb_v2_statement_iterator_next(iter, &stmt, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(stmt != nullptr);
		REQUIRE(recorder->queries.size() == i + 1);
		duckdb_v2_sql_statement_destroy(&stmt);
	}
	// the third statement is rejected
	duckdb_v2_sql_statement_handle stmt = nullptr;
	duckdb_v2_error_info_handle err = nullptr;
	REQUIRE(duckdb_v2_statement_iterator_next(iter, &stmt, &err) != DUCKDB_V2_ERROR_NONE);
	REQUIRE(stmt == nullptr);
	REQUIRE(err != nullptr);
	duckdb_v2_str text {nullptr, 0};
	REQUIRE(duckdb_v2_error_info_get_text(err, &text) == DUCKDB_V2_ERROR_NONE);
	REQUIRE_THAT(string(text.ptr, text.len), Contains("post-parse hook rejected"));
	duckdb_v2_error_info_destroy(&err);
	REQUIRE(recorder->queries.size() == 3);
	// and the iterator is spent: the fourth statement is never parsed
	REQUIRE(duckdb_v2_statement_iterator_next(iter, &stmt, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(stmt == nullptr);
	REQUIRE(recorder->queries.size() == 3);
	duckdb_v2_statement_iterator_destroy(&iter);

	duckdb_v2_connection_destroy(&conn);
	duckdb_v2_instance_destroy(&instance);
	duckdb_v2_environment_destroy(&env);
}

//===--------------------------------------------------------------------===//
// 3. Rejection
//===--------------------------------------------------------------------===//

TEST_CASE("Post-parse hook rejection stops the input like a parse error", "[api][post_parse]") {
	DBConfig config;
	auto recorder = AddRecorder(config);
	recorder->reject_containing = "reject_me";
	DuckDB db(nullptr, &config);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i INT)"));
	recorder->queries.clear();

	SECTION("Rejected statement") {
		auto result = con.Query("INSERT INTO t VALUES (1); SELECT 'reject_me'; INSERT INTO t VALUES (3)");
		REQUIRE_FAIL(result);
		REQUIRE(result->GetErrorType() == ExceptionType::PERMISSION);
		REQUIRE_THAT(result->GetError(), Contains("post-parse hook rejected"));
		// the hook saw statements 1 and 2 only; statement 1 ran, statement 3 did not
		REQUIRE(recorder->queries.size() == 2);
		REQUIRE(CountRows(con, "t") == 1);
	}
	SECTION("Syntax error in the same position") {
		auto result = con.Query("INSERT INTO t VALUES (1); SELECT 1 FROM WHERE; INSERT INTO t VALUES (3)");
		REQUIRE_FAIL(result);
		REQUIRE(result->GetErrorType() == ExceptionType::PARSER);
		REQUIRE(recorder->queries.size() == 1);
		REQUIRE(CountRows(con, "t") == 1);
	}
	SECTION("Rejected statement inside an explicit transaction") {
		REQUIRE_NO_FAIL(con.Query("BEGIN"));
		auto result = con.Query("INSERT INTO t VALUES (1); SELECT 'reject_me'; INSERT INTO t VALUES (3)");
		REQUIRE_FAIL(result);
		REQUIRE(result->GetErrorType() == ExceptionType::PERMISSION);
		// as after a syntax error at the same position (next section), the transaction is aborted
		REQUIRE_FAIL(con.Query("SELECT 42"));
		REQUIRE_NO_FAIL(con.Query("ROLLBACK"));
		REQUIRE(CountRows(con, "t") == 0);
	}
	SECTION("Syntax error inside an explicit transaction") {
		REQUIRE_NO_FAIL(con.Query("BEGIN"));
		auto result = con.Query("INSERT INTO t VALUES (1); SELECT 1 FROM WHERE; INSERT INTO t VALUES (3)");
		REQUIRE_FAIL(result);
		REQUIRE(result->GetErrorType() == ExceptionType::PARSER);
		REQUIRE_FAIL(con.Query("SELECT 42"));
		REQUIRE_NO_FAIL(con.Query("ROLLBACK"));
		REQUIRE(CountRows(con, "t") == 0);
	}
	SECTION("Prepare, Submit and ExtractStatements") {
		auto prepared = con.Prepare("SELECT 'reject_me'");
		REQUIRE(prepared->HasError());
		REQUIRE_THAT(prepared->GetError(), Contains("post-parse hook rejected"));
		auto submitted = con.Submit("SELECT 'reject_me'");
		REQUIRE(submitted->HasError());
		REQUIRE_THAT(submitted->GetError(), Contains("post-parse hook rejected"));
		REQUIRE_THROWS_WITH(con.ExtractStatements("SELECT 1; SELECT 'reject_me'; SELECT 3"),
		                    Contains("post-parse hook rejected"));
		REQUIRE(recorder->queries.size() == 4);
	}
	// the connection stays usable afterwards
	REQUIRE_NO_FAIL(con.Query("SELECT 42"));
}

TEST_CASE("Post-parse hook rejection exhausts a ParseIterator", "[api][post_parse]") {
	// The client APIs stop at the first error anyway; code that drives a ParseIterator itself relies on the
	// iterator ending at a rejected statement
	DBConfig config;
	auto recorder = AddRecorder(config);
	recorder->reject_containing = "reject_me";
	DuckDB db(nullptr, &config);
	Connection con(db);
	recorder->Clear();

	ParseIterator iterator(*con.context, "SELECT 1; SELECT 'reject_me'; SELECT 3");
	REQUIRE(iterator.Peek());
	REQUIRE(iterator.GetStatement());
	REQUIRE_THROWS_WITH(iterator.Peek(), Contains("post-parse hook rejected"));
	// nothing after the rejected statement is parsed or yielded
	REQUIRE_FALSE(iterator.Peek());
	REQUIRE_FALSE(iterator.HasMore());
	REQUIRE(iterator.GetStatement() == nullptr);
	REQUIRE(recorder->queries == vector<string> {"SELECT 1; ", "SELECT 'reject_me'; "});
}

//===--------------------------------------------------------------------===//
// 4. Not called for parses the engine makes for itself, or for stored definitions
//===--------------------------------------------------------------------===//

TEST_CASE("Post-parse hook is not called for internal parses", "[api][post_parse]") {
	DBConfig config;
	auto recorder = AddRecorder(config);
	DuckDB db(nullptr, &config);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i INT)"));
	recorder->queries.clear();
	recorder->types.clear();

	SECTION("Default macro created on first use") {
		auto result = con.Query("SELECT pg_typeof(42)");
		REQUIRE(CHECK_COLUMN(result, 0, {"integer"}));
		REQUIRE(recorder->queries.size() == 1);
		REQUIRE(recorder->queries[0] == "SELECT pg_typeof(42)");
	}
	SECTION("Default table macro created on first use") {
		auto result = con.Query("SELECT COUNT(*) > 0 FROM duckdb_profiling_settings()");
		REQUIRE(CHECK_COLUMN(result, 0, {true}));
		REQUIRE(recorder->queries.size() == 1);
	}
	SECTION("Parser::ParseQuery without a client context") {
		auto builtin = Parser::GetBuiltinParser();
		builtin.ParseQuery("SELECT 1; SELECT 2");
		REQUIRE(builtin.statements.size() == 2);
		auto without_hooks = Parser::WithoutPostParseHooks(*con.context);
		without_hooks.ParseQuery("SELECT 1; SELECT 2");
		REQUIRE(without_hooks.statements.size() == 2);
		REQUIRE(recorder->queries.empty());
	}
}

TEST_CASE("Post-parse hook does not see definitions stored in a database file", "[api][post_parse]") {
	// Views, macros and column defaults are stored already parsed, so a file's definitions are bound for the client
	// without passing the hook. Only the client's own statements do, the ATTACH among them.
	auto path = TestCreatePath("post_parse_stored_definitions.db");
	DeleteDatabase(path);
	{
		DuckDB writer(path);
		Connection con(writer);
		REQUIRE_NO_FAIL(con.Query("CREATE VIEW v AS SELECT current_setting('threads') AS s"));
		REQUIRE_NO_FAIL(con.Query("CREATE MACRO m() AS current_setting('threads')"));
		REQUIRE_NO_FAIL(con.Query("CREATE MACRO tm() AS TABLE SELECT current_setting('threads') AS s"));
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE d(i INTEGER, s VARCHAR DEFAULT current_setting('threads'))"));
	}
	DBConfig config;
	auto recorder = AddRecorder(config);
	recorder->reject_containing = "current_setting";

	SECTION("attached") {
		DuckDB db(nullptr, &config);
		Connection con(db);
		recorder->Clear();
		REQUIRE_NO_FAIL(con.Query("ATTACH '" + path + "' AS x"));
		REQUIRE(recorder->queries.size() == 1);
		REQUIRE_THAT(recorder->queries[0], Contains(path));
		// each of these evaluates current_setting, which the hook rejects wherever it sees it
		REQUIRE_NO_FAIL(con.Query("SELECT s FROM x.v"));
		REQUIRE_NO_FAIL(con.Query("SELECT x.main.m()"));
		REQUIRE_NO_FAIL(con.Query("SELECT s FROM x.main.tm()"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO x.d(i) VALUES (1)"));
		auto result = con.Query("SELECT s IS NOT NULL FROM x.d");
		REQUIRE(CHECK_COLUMN(result, 0, {true}));
		REQUIRE(recorder->queries.size() == 6);
	}
	SECTION("opened as the main database") {
		DuckDB db(path, &config);
		Connection con(db);
		recorder->Clear();
		REQUIRE_NO_FAIL(con.Query("SELECT s FROM v"));
		REQUIRE_NO_FAIL(con.Query("SELECT m()"));
		REQUIRE(recorder->queries == vector<string> {"SELECT s FROM v", "SELECT m()"});
	}
	DeleteDatabase(path);
}

TEST_CASE("Opening a database does not call the post-parse hook", "[api][post_parse]") {
	// Startup runs its transactions through Connection::BeginTransaction/Commit, which do not parse SQL text, so a
	// hook sees nothing before the first client statement. (Engine code that does run SQL text on an internal
	// Connection, such as WAL replay's error handling, goes through the client path and is seen like any client.)
	DBConfig config;
	auto recorder = AddRecorder(config);
	DuckDB db(nullptr, &config);
	Connection con(db);
	REQUIRE(recorder->queries.empty());
	REQUIRE_NO_FAIL(con.Query("SELECT 42"));
	REQUIRE(recorder->queries.size() == 1);
}

TEST_CASE("Post-parse hook does not see the subqueries duckdb_logs reads the logs with", "[api][post_parse]") {
	DBConfig config;
	auto recorder = AddRecorder(config);
	DuckDB db(nullptr, &config);
	Connection con(db);
	// reject any statement that names the function the engine's own log join uses
	recorder->reject_containing = "duckdb_log_contexts";

	SECTION("in-memory log storage, denormalized") {
		REQUIRE_NO_FAIL(con.Query("CALL enable_logging(storage='memory')"));
		REQUIRE_NO_FAIL(con.Query("SELECT 42"));
		recorder->Clear();
		REQUIRE_NO_FAIL(con.Query("SELECT count(*) >= 0 FROM duckdb_logs(denormalized_table=true)"));
		REQUIRE(recorder->queries == vector<string> {"SELECT count(*) >= 0 FROM duckdb_logs(denormalized_table=true)"});
	}
	SECTION("file log storage") {
		auto dir = TestJoinPath(LocalTestDirectoryPath(), "post_parse_file_logs");
		TestDeleteDirectory(dir);
		TestCreateDirectory(dir);
		recorder->reject_containing = "read_csv";
		REQUIRE_NO_FAIL(con.Query("CALL enable_logging(storage='file', storage_path='" + dir + "')"));
		REQUIRE_NO_FAIL(con.Query("SELECT 42"));
		recorder->Clear();
		REQUIRE_NO_FAIL(con.Query("SELECT count(*) >= 0 FROM duckdb_logs()"));
		REQUIRE(recorder->queries[0] == "SELECT count(*) >= 0 FROM duckdb_logs()");
		// the engine's read_csv_auto subquery is not seen; the column types it reads the log files with are type
		// strings, parsed while it is bound, and reach the hook as fragments
		for (idx_t i = 1; i < recorder->queries.size(); i++) {
			REQUIRE(recorder->fragments[i]);
			REQUIRE_THAT(recorder->queries[i], StartsWith("CREATE TABLE tbl ("));
		}
		REQUIRE_NO_FAIL(con.Query("CALL disable_logging()"));
		TestDeleteDirectory(dir);
	}
}

TEST_CASE("Post-parse hook sees a statement once under reparse verification", "[api][post_parse]") {
	DBConfig config;
	auto recorder = AddRecorder(config);
	DuckDB db(nullptr, &config);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET debug_verify_statement = 'reparse_statement'"));
	recorder->Clear();
	REQUIRE_NO_FAIL(con.Query("SELECT 42"));
	REQUIRE(recorder->queries == vector<string> {"SELECT 42"});
}

//===--------------------------------------------------------------------===//
// 5. Statements produced by a parser_override extension or a CONNECT-ed grammar
//===--------------------------------------------------------------------===//

static ParserOverrideResult FixedStatementsOverride(ParserExtensionInfo *, const string &query, ParserOptions &) {
	if (query.find("OVERRIDE ME") == string::npos) {
		return ParserOverrideResult();
	}
	// parse with the built-in options: the client's options would re-enter this override
	Parser parser(ParserOptions::Builtin());
	parser.ParseQuery("SELECT 1; SELECT 'reject_me'; SELECT 3");
	if (query.find("ALL") == string::npos) {
		parser.statements.erase(parser.statements.begin() + 1);
	}
	return ParserOverrideResult(std::move(parser.statements));
}

TEST_CASE("Post-parse hook sees statements produced by a parser_override", "[api][post_parse]") {
	DBConfig config;
	ParserExtension override_extension;
	override_extension.parser_override = FixedStatementsOverride;
	ParserExtension::Register(config, std::move(override_extension));
	auto recorder = AddRecorder(config);
	recorder->reject_containing = "reject_me";
	DuckDB db(nullptr, &config);
	Connection con(db);
	recorder->Clear();

	// with the default setting the override is not consulted, but the hook still runs
	REQUIRE_FAIL(con.Query("OVERRIDE ME"));
	REQUIRE(recorder->queries.empty());
	REQUIRE_NO_FAIL(con.Query("SET allow_parser_override_extension = 'fallback'"));
	REQUIRE(recorder->queries.size() == 1);
	recorder->queries.clear();
	recorder->types.clear();

	auto result = con.Query("OVERRIDE ME");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
	REQUIRE(result->next);
	REQUIRE(CHECK_COLUMN(result->next, 0, {3}));
	REQUIRE(recorder->types.size() == 2);
	REQUIRE(recorder->types[0] == StatementType::SELECT_STATEMENT);
	REQUIRE(recorder->types[1] == StatementType::SELECT_STATEMENT);

	// a rejected overridden statement ends the input
	recorder->queries.clear();
	result = con.Query("OVERRIDE ME ALL");
	REQUIRE_FAIL(result);
	REQUIRE_THAT(result->GetError(), Contains("post-parse hook rejected"));
	REQUIRE(recorder->queries.size() == 2);
}

TEST_CASE("Post-parse hook sees a parser_override's statements when ParseQuery parses", "[api][post_parse]") {
	// query() parses its argument with Parser::ParseQuery, not ParseIterator; with the override consulted, the
	// statements it produces reach the hook on ParseQuery's override exit
	DBConfig config;
	ParserExtension override_extension;
	override_extension.parser_override = FixedStatementsOverride;
	ParserExtension::Register(config, std::move(override_extension));
	auto recorder = AddRecorder(config);
	DuckDB db(nullptr, &config);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET allow_parser_override_extension = 'fallback'"));
	recorder->Clear();
	recorder->reject_containing = "reject_me";

	// the override turns "OVERRIDE ME ALL" into three statements, one of which the hook rejects. The text is built
	// when query()'s argument is evaluated: the client's own statement does not contain it, so the override does not
	// take over the client's input, only the parse of query()'s argument
	auto result = con.Query("SELECT * FROM query('OVERRIDE' || ' ME ALL')");
	REQUIRE_FAIL(result);
	REQUIRE_THAT(result->GetError(), Contains("post-parse hook rejected"));
	bool saw_override_statement = false;
	for (auto &query : recorder->queries) {
		if (query.find("reject_me") != string::npos) {
			saw_override_statement = true;
		}
	}
	REQUIRE(saw_override_statement);
}

TEST_CASE("Post-parse hook sees statements parsed with a CONNECT-ed grammar", "[api][post_parse]") {
	// While CONNECT-ed, the remote database's grammar parses client statements; the passthrough grammar turns them
	// into PASSTHROUGH_STATEMENTs carrying the SQL meant for the remote. The hook sees those too.
	DBConfig config;
	auto recorder = AddRecorder(config);
	DuckDB db(nullptr, &config);
	Connection con(db);
	recorder->Clear();

	PassthroughDialect dialect;
	auto &client_config = ClientConfig::GetConfig(*con.context);
	client_config.connected_grammar = dialect.GetCompiledGrammar(*con.context);
	auto statements = con.ExtractStatements("CREATE GRANT; SELECT 42;");
	client_config.connected_grammar = nullptr;

	REQUIRE(statements.size() == 2);
	REQUIRE(recorder->types.size() == 2);
	REQUIRE(recorder->types[0] == StatementType::PASSTHROUGH_STATEMENT);
	REQUIRE(recorder->types[1] == StatementType::PASSTHROUGH_STATEMENT);
	REQUIRE(StringUtil::StartsWith(recorder->queries[0], "CREATE GRANT;"));
}

//===--------------------------------------------------------------------===//
// 6. Registered info and order
//===--------------------------------------------------------------------===//

TEST_CASE("Post-parse hooks receive their info and run in registration order", "[api][post_parse]") {
	DBConfig config;
	auto order = make_shared_ptr<vector<string>>();
	auto first = AddRecorder(config, "first");
	first->order = order;
	// in between: a PostParseExtension without a function is skipped, and a ParserExtension is not a hook
	PostParseExtension::Register(config, PostParseExtension());
	ParserExtension::Register(config, ParserExtension());
	auto second = AddRecorder(config, "second");
	second->order = order;
	DuckDB db(nullptr, &config);
	Connection con(db);
	first->Clear();
	second->Clear();

	REQUIRE_NO_FAIL(con.Query("SELECT 1; SELECT 2"));
	REQUIRE(*order == vector<string> {"first:SELECT 1; ", "second:SELECT 1; ", "first:SELECT 2", "second:SELECT 2"});
	REQUIRE(first->seen_infos.size() == 2);
	REQUIRE(second->seen_infos.size() == 2);
	for (auto info : first->seen_infos) {
		REQUIRE(info == first.get());
	}
	for (auto info : second->seen_infos) {
		REQUIRE(info == second.get());
	}

	// when the first hook rejects, the second hook does not see the statement
	first->reject_containing = "reject_me";
	order->clear();
	REQUIRE_FAIL(con.Query("SELECT 'reject_me'"));
	REQUIRE(*order == vector<string> {"first:SELECT 'reject_me'"});
}

//===--------------------------------------------------------------------===//
// 7. Client SQL text parsed outside ParseIterator
//===--------------------------------------------------------------------===//

TEST_CASE("Post-parse hook runs on Parser::ParseQuery and ParseSelectNode with a client context", "[api][post_parse]") {
	DBConfig config;
	auto recorder = AddRecorder(config);
	DuckDB db(nullptr, &config);
	Connection con(db);
	recorder->Clear();

	Parser parser(*con.context);
	parser.ParseQuery("SELECT 1; SELECT 2");
	REQUIRE(parser.statements.size() == 2);
	REQUIRE(recorder->queries.size() == 2);
	REQUIRE(recorder->fragments == vector<bool> {false, false});

	// ParseSelectNode parses a whole statement, which is not a fragment
	recorder->Clear();
	parser.ParseSelectNode("SELECT 3");
	REQUIRE(recorder->queries == vector<string> {"SELECT 3"});
	REQUIRE(recorder->fragments == vector<bool> {false});
}

TEST_CASE("Post-parse hook runs on Connection::RelationFromQuery", "[api][post_parse]") {
	DBConfig config;
	auto recorder = AddRecorder(config);
	DuckDB db(nullptr, &config);
	Connection con(db);
	recorder->Clear();

	auto relation = con.RelationFromQuery("SELECT 42 AS v");
	REQUIRE(recorder->queries.size() == 1);
	REQUIRE(recorder->queries[0] == "SELECT 42 AS v");
	REQUIRE(recorder->types[0] == StatementType::SELECT_STATEMENT);
	// running the relation parses no SQL text
	auto result = relation->Execute();
	REQUIRE(CHECK_COLUMN(result, 0, {42}));
	REQUIRE(recorder->queries.size() == 1);

	// a rejection fails RelationFromQuery
	recorder->reject_containing = "reject_me";
	REQUIRE_THROWS_WITH(con.RelationFromQuery("SELECT 'reject_me' AS v"), Contains("post-parse hook rejected"));
	REQUIRE(recorder->queries.size() == 2);

	// a statement the caller passes in already parsed is not given to the hook (build it with a context-free
	// parser so producing it does not itself fire the hook)
	auto parser = Parser::GetBuiltinParser();
	parser.ParseQuery("SELECT 'reject_me' AS v");
	auto parsed =
	    con.RelationFromQuery(unique_ptr_cast<SQLStatement, SelectStatement>(std::move(parser.statements[0])));
	auto parsed_result = parsed->Execute();
	REQUIRE(CHECK_COLUMN(parsed_result, 0, {"reject_me"}));
	REQUIRE(recorder->queries.size() == 2);
}

TEST_CASE("Post-parse hook runs at every flush of a query appender", "[api][post_parse][appender]") {
	DBConfig config;
	auto recorder = AddRecorder(config);
	DuckDB db(nullptr, &config);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i INTEGER)"));
	recorder->Clear();

	const string insert = "INSERT INTO t SELECT * FROM appended_data";
	{
		QueryAppender appender(con, insert, {LogicalType::INTEGER});
		// the appender parses its query when it flushes, not when it is created
		REQUIRE(recorder->queries.empty());
		appender.AppendRow(1);
		appender.Flush();
		REQUIRE(recorder->queries.size() == 1);
		REQUIRE(recorder->queries[0] == insert);
		REQUIRE(recorder->types[0] == StatementType::INSERT_STATEMENT);
		// every flush parses the query again, and the hook sees each one
		appender.AppendRow(2);
		appender.Flush();
		REQUIRE(recorder->queries.size() == 2);
		REQUIRE(recorder->queries[1] == insert);
	}
	REQUIRE(CountRows(con, "t") == 2);

	// a rejection fails the flush, and its rows are not appended
	recorder->reject_containing = "reject_me";
	{
		QueryAppender appender(con, "INSERT INTO t SELECT * FROM appended_data WHERE 'reject_me' <> ''",
		                       {LogicalType::INTEGER});
		appender.AppendRow(3);
		REQUIRE_THROWS_WITH(appender.Flush(), Contains("post-parse hook rejected"));
	}
	recorder->reject_containing.clear();
	REQUIRE(CountRows(con, "t") == 2);
}

TEST_CASE("Post-parse hook sees the INSERT a table appender builds", "[api][post_parse][appender]") {
	DBConfig config;
	auto recorder = AddRecorder(config);
	DuckDB db(nullptr, &config);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i INTEGER)"));
	recorder->Clear();

	SECTION("Appender, at every flush") {
		{
			Appender appender(con, "t");
			appender.AppendRow(1);
			appender.Flush();
			REQUIRE(recorder->types == vector<StatementType> {StatementType::INSERT_STATEMENT});
			REQUIRE_THAT(recorder->queries[0], StartsWith("INSERT INTO ") && Contains("FROM __duckdb_internal"));
			appender.AppendRow(2);
			appender.Close();
		}
		REQUIRE(recorder->queries.size() == 2);
		REQUIRE(CountRows(con, "t") == 2);
		// a rejection fails the flush, and its rows are not appended
		recorder->reject_containing = "__duckdb_internal";
		{
			Appender appender(con, "t");
			appender.AppendRow(3);
			REQUIRE_THROWS_WITH(appender.Flush(), Contains("post-parse hook rejected"));
		}
		recorder->reject_containing.clear();
		REQUIRE(CountRows(con, "t") == 2);
	}
	SECTION("Connection::Append") {
		auto description = con.TableInfo("t");
		REQUIRE(description);
		DataChunk chunk;
		chunk.Initialize(Allocator::DefaultAllocator(), {LogicalType::INTEGER});
		chunk.data[0].Append(Value::INTEGER(42));
		chunk.SetChildCardinality(1);
		ColumnDataCollection collection(Allocator::DefaultAllocator(), {LogicalType::INTEGER});
		collection.Append(chunk);
		con.Append(*description, collection);
		REQUIRE(recorder->types == vector<StatementType> {StatementType::INSERT_STATEMENT});
		REQUIRE_THAT(recorder->queries[0], Contains("FROM __duckdb_internal"));
		REQUIRE(CountRows(con, "t") == 1);
		recorder->reject_containing = "__duckdb_internal";
		REQUIRE_THROWS_WITH(con.Append(*description, collection), Contains("post-parse hook rejected"));
		recorder->reject_containing.clear();
		REQUIRE(CountRows(con, "t") == 1);
	}
}

TEST_CASE("Post-parse hook runs at every flush of a C API query appender", "[api][post_parse][appender][capi]") {
	DBConfig config;
	auto recorder = AddRecorder(config);
	auto wrapper = new DatabaseWrapper();
	wrapper->database = make_shared_ptr<DuckDB>(nullptr, &config);
	auto database = reinterpret_cast<duckdb_database>(wrapper);
	duckdb_connection connection = nullptr;
	REQUIRE(duckdb_connect(database, &connection) == DuckDBSuccess);
	duckdb_result result;
	REQUIRE(duckdb_query(connection, "CREATE TABLE t(i INTEGER)", &result) == DuckDBSuccess);
	duckdb_destroy_result(&result);
	recorder->Clear();
	recorder->reject_containing = "reject_me";

	auto append_and_flush = [&](const char *query, int32_t value) {
		duckdb_logical_type types[1] = {duckdb_create_logical_type(DUCKDB_TYPE_INTEGER)};
		duckdb_appender appender = nullptr;
		auto state = duckdb_appender_create_query(connection, query, 1, types, nullptr, nullptr, &appender);
		duckdb_destroy_logical_type(&types[0]);
		REQUIRE(state == DuckDBSuccess);
		REQUIRE(duckdb_appender_begin_row(appender) == DuckDBSuccess);
		REQUIRE(duckdb_append_int32(appender, value) == DuckDBSuccess);
		REQUIRE(duckdb_appender_end_row(appender) == DuckDBSuccess);
		auto flushed = duckdb_appender_flush(appender);
		string error = flushed == DuckDBSuccess ? "" : duckdb_appender_error(appender);
		duckdb_appender_destroy(&appender);
		return error;
	};
	REQUIRE(append_and_flush("INSERT INTO t SELECT * FROM appended_data", 1).empty());
	REQUIRE(recorder->queries == vector<string> {"INSERT INTO t SELECT * FROM appended_data"});
	REQUIRE_THAT(append_and_flush("INSERT INTO t SELECT * FROM appended_data WHERE 'reject_me' <> ''", 2),
	             Contains("post-parse hook rejected"));
	REQUIRE(recorder->queries.size() == 2);

	REQUIRE(duckdb_query(connection, "SELECT COUNT(*) FROM t", &result) == DuckDBSuccess);
	REQUIRE(duckdb_value_int64(&result, 0, 0) == 1);
	duckdb_destroy_result(&result);
	duckdb_disconnect(&connection);
	duckdb_close(&database);
}

// The subquery the replacement scan below claims unresolved names with, whether the callback reports a failed claim,
// and the code the claim returned. File-scope because the callback is a C function.
static string post_parse_repl_sql;
static bool post_parse_repl_reports_error = true;
static DUCKDB_V2_ERROR post_parse_repl_rc = DUCKDB_V2_ERROR_NONE;

static void PostParseReplClaimSubquery(duckdb_v2_replacement_scan_info_handle info, duckdb_v2_context_handle,
                                       duckdb_v2_error_info_handle *err) {
	duckdb_v2_str sql {post_parse_repl_sql.c_str(), post_parse_repl_sql.size()};
	post_parse_repl_rc =
	    duckdb_v2_replacement_scan_set_subquery(info, &sql, post_parse_repl_reports_error ? err : nullptr);
}

TEST_CASE("Post-parse hook sees the subquery a C API v2 replacement scan supplies", "[api][post_parse][capi_v2]") {
	post_parse_repl_reports_error = true;
	post_parse_repl_rc = DUCKDB_V2_ERROR_NONE;
	duckdb_v2_environment_handle env = nullptr;
	duckdb_v2_instance_handle instance = nullptr;
	duckdb_v2_connection_handle conn = nullptr;
	REQUIRE(duckdb_v2_environment_create(&env, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_instance_create(env, &instance, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_str in_memory {nullptr, 0};
	REQUIRE(duckdb_v2_instance_attach(instance, nullptr, &in_memory, nullptr, true, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_create(instance, &conn, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_replacement_scan_handle scan = nullptr;
	duckdb_v2_factory_handle factory = nullptr;
	REQUIRE(duckdb_v2_connection_get_factory(conn, &factory, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_replacement_scan_create(factory, &scan, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_replacement_scan_set_callback(scan, PostParseReplClaimSubquery, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_replacement_scan(conn, scan, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_replacement_scan_destroy(&scan) == DUCKDB_V2_ERROR_NONE);
	// the v2 connection is a Connection: run the queries through it
	auto &con = *duckdb::capiv2::Convert(conn);
	auto recorder = AddRecorder(DBConfig::GetConfig(*con.context));
	recorder->reject_containing = "reject_me";

	SECTION("the subquery is seen while the client's statement is bound") {
		post_parse_repl_sql = "SELECT 42 AS v";
		auto result = con.Query("SELECT v FROM unresolved_name");
		REQUIRE(CHECK_COLUMN(result, 0, {42}));
		REQUIRE(recorder->queries == vector<string> {"SELECT v FROM unresolved_name", "SELECT 42 AS v"});
	}
	SECTION("a rejection fails the claim and the query") {
		post_parse_repl_sql = "SELECT 'reject_me' AS v";
		auto result = con.Query("SELECT v FROM unresolved_name");
		REQUIRE(result->HasError());
		REQUIRE_THAT(result->GetError(), Contains("post-parse hook rejected"));
		REQUIRE(post_parse_repl_rc != DUCKDB_V2_ERROR_NONE);
	}
	SECTION("a callback that ignores the failed claim leaves the name unresolved") {
		post_parse_repl_sql = "SELECT 'reject_me' AS v";
		post_parse_repl_reports_error = false;
		auto result = con.Query("SELECT v FROM unresolved_name");
		REQUIRE(result->HasError());
		REQUIRE_THAT(result->GetError(), Contains("unresolved_name"));
		REQUIRE(post_parse_repl_rc != DUCKDB_V2_ERROR_NONE);
		REQUIRE(recorder->queries.size() == 2);
	}

	post_parse_repl_reports_error = true;
	duckdb_v2_connection_destroy(&conn);
	duckdb_v2_instance_destroy(&instance);
	duckdb_v2_environment_destroy(&env);
}

//===--------------------------------------------------------------------===//
// 8. SQL parsed while the client's statement is preprocessed or bound
//===--------------------------------------------------------------------===//

TEST_CASE("Post-parse hook sees the statements a PRAGMA expands to", "[api][post_parse]") {
	DBConfig config;
	auto recorder = AddRecorder(config);
	DuckDB db(nullptr, &config);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i INT)"));
	recorder->Clear();

	REQUIRE_NO_FAIL(con.Query("PRAGMA table_info('t')"));
	REQUIRE(recorder->types ==
	        vector<StatementType> {StatementType::PRAGMA_STATEMENT, StatementType::SELECT_STATEMENT});
	REQUIRE(recorder->queries[1] == "SELECT * FROM pragma_table_info('t');");
	// a PRAGMA that runs a function, not a query, expands to nothing
	REQUIRE_NO_FAIL(con.Query("PRAGMA disable_profiling"));
	REQUIRE(recorder->types.size() == 3);
	REQUIRE(recorder->types[2] == StatementType::PRAGMA_STATEMENT);

	// a rejected expansion fails the PRAGMA
	recorder->reject_containing = "pragma_table_info";
	auto result = con.Query("PRAGMA table_info('t')");
	REQUIRE_FAIL(result);
	REQUIRE(result->GetErrorType() == ExceptionType::PERMISSION);
	REQUIRE_THAT(result->GetError(), Contains("post-parse hook rejected: SELECT * FROM pragma_table_info('t');"));
	REQUIRE_NO_FAIL(con.Query("SELECT 42"));
}

TEST_CASE("Post-parse hook sees the SQL IMPORT DATABASE reads from its files", "[api][post_parse]") {
	auto dir = TestJoinPath(LocalTestDirectoryPath(), "post_parse_import");
	TestDeleteDirectory(dir);
	TestCreateDirectory(dir);
	auto write_file = [&](const string &name, const string &content) {
		std::ofstream out(TestJoinPath(dir, name));
		out << content;
	};
	DBConfig config;
	auto recorder = AddRecorder(config);
	recorder->reject_containing = "reject_me";
	DuckDB db(nullptr, &config);
	Connection con(db);
	recorder->Clear();
	const string import = "IMPORT DATABASE '" + dir + "'";

	SECTION("every statement in schema.sql and load.sql is seen") {
		write_file("schema.sql", "CREATE TABLE a(i INTEGER);\n");
		write_file("load.sql", "COPY a FROM 'a.csv' (FORMAT csv, HEADER false);\n");
		write_file("a.csv", "1\n2\n");
		REQUIRE_NO_FAIL(con.Query(import));
		REQUIRE(recorder->types == vector<StatementType> {StatementType::PRAGMA_STATEMENT,
		                                                  StatementType::CREATE_STATEMENT,
		                                                  StatementType::COPY_STATEMENT});
		REQUIRE(recorder->queries[0] == import);
		REQUIRE(recorder->queries[1] == "CREATE TABLE a(i INTEGER);\n");
		// the COPY, with its file path rewritten to the import directory
		REQUIRE_THAT(recorder->queries[2], Contains(TestJoinPath(dir, "a.csv")));
		REQUIRE(CountRows(con, "a") == 2);
	}
	SECTION("every statement is seen before the first one is bound") {
		// The SELECT fails if it is bound, and it comes before the rejected statement. The import fails with the
		// hook's rejection, not with the SELECT's error, so the hook saw every statement before any was bound.
		write_file("schema.sql",
		           "CREATE TABLE a(i INTEGER);\nSELECT * FROM missing_table;\nCREATE TABLE reject_me(i INTEGER);\n");
		write_file("load.sql", "");
		auto result = con.Query(import);
		REQUIRE_FAIL(result);
		REQUIRE(result->GetErrorType() == ExceptionType::PERMISSION);
		REQUIRE_THAT(result->GetError(), Contains("post-parse hook rejected: CREATE TABLE reject_me"));
		REQUIRE(recorder->types.size() == 4);
		// nothing of the import is left
		REQUIRE_FAIL(con.Query("SELECT * FROM a"));
	}
	TestDeleteDirectory(dir);
}

TEST_CASE("Post-parse hook sees both statements COPY FROM DATABASE expands to", "[api][post_parse]") {
	// Without (SCHEMA) or (DATA), COPY FROM DATABASE is the copy_database PRAGMA, which expands to a copy of the
	// schema and a copy of the data
	DBConfig config;
	auto recorder = AddRecorder(config);
	DuckDB db(nullptr, &config);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("ATTACH ':memory:' AS src"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE src.t AS SELECT 42 AS i"));
	REQUIRE_NO_FAIL(con.Query("ATTACH ':memory:' AS dst"));
	recorder->Clear();

	REQUIRE_NO_FAIL(con.Query("COPY FROM DATABASE src TO dst"));
	REQUIRE(recorder->types == vector<StatementType> {StatementType::PRAGMA_STATEMENT,
	                                                  StatementType::COPY_DATABASE_STATEMENT,
	                                                  StatementType::COPY_DATABASE_STATEMENT});
	REQUIRE_THAT(recorder->queries[1], Contains("(SCHEMA)"));
	REQUIRE_THAT(recorder->queries[2], Contains("(DATA)"));
	REQUIRE(CountRows(con, "dst.t") == 1);
}

TEST_CASE("Post-parse hook sees the SQL query() and query_table() bind", "[api][post_parse]") {
	DBConfig config;
	auto recorder = AddRecorder(config);
	DuckDB db(nullptr, &config);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i INT); INSERT INTO t VALUES (42)"));
	recorder->Clear();
	recorder->reject_containing = "reject_me";

	SECTION("query()") {
		auto result = con.Query("SELECT * FROM query('SELECT 42')");
		REQUIRE(CHECK_COLUMN(result, 0, {42}));
		REQUIRE(recorder->queries == vector<string> {"SELECT * FROM query('SELECT 42')", "SELECT 42"});
		REQUIRE(recorder->types[1] == StatementType::SELECT_STATEMENT);
		// the argument is evaluated, then parsed: the hook sees the SQL that is bound, not the expression
		result = con.Query("SELECT * FROM query('SELECT ''reject' || '_me''')");
		REQUIRE_FAIL(result);
		REQUIRE(result->GetErrorType() == ExceptionType::PERMISSION);
		REQUIRE_THAT(result->GetError(), Contains("post-parse hook rejected: SELECT 'reject_me'"));
		REQUIRE(recorder->queries.size() == 4);
	}
	SECTION("query_table()") {
		auto result = con.Query("SELECT * FROM query_table('t')");
		REQUIRE(CHECK_COLUMN(result, 0, {42}));
		REQUIRE(recorder->queries.size() == 2);
		REQUIRE(recorder->queries[1] == "FROM t");
		result = con.Query("SELECT * FROM query_table(['t', 't'])");
		REQUIRE(CHECK_COLUMN(result, 0, {42, 42}));
		REQUIRE(recorder->queries.size() == 4);
		REQUIRE(recorder->queries[3] == "FROM t UNION ALL FROM t");
		result = con.Query("SELECT * FROM query_table('reject' || '_me')");
		REQUIRE_FAIL(result);
		REQUIRE_THAT(result->GetError(), Contains("post-parse hook rejected: FROM reject_me"));
	}
}

TEST_CASE("Post-parse hook sees the SQL json_serialize_plan binds", "[api][post_parse]") {
	DBConfig config;
	auto recorder = AddRecorder(config);
	DuckDB db(nullptr, &config);
	if (!db.ExtensionIsLoaded("json")) {
		return;
	}
	Connection con(db);
	recorder->Clear();

	// a column argument is evaluated once, at execution
	auto result = con.Query("SELECT json_serialize_plan(s) ->> 'error' FROM (VALUES ('SELECT 42')) v(s)");
	REQUIRE(CHECK_COLUMN(result, 0, {"false"}));
	REQUIRE(recorder->queries.size() == 2);
	REQUIRE(recorder->queries[1] == "SELECT 42");
	// the function parses its argument each time it is evaluated: a constant argument is evaluated while the outer
	// statement is bound, and again by constant folding
	recorder->Clear();
	result = con.Query("SELECT json_serialize_plan('SELECT 43') ->> 'error'");
	REQUIRE(CHECK_COLUMN(result, 0, {"false"}));
	REQUIRE(recorder->queries.size() >= 2);
	for (idx_t i = 1; i < recorder->queries.size(); i++) {
		REQUIRE(recorder->queries[i] == "SELECT 43");
	}

	// a rejection is the function's error result, and the statement is not bound
	recorder->reject_containing = "reject_me";
	result = con.Query("SELECT j ->> 'error_type', j ->> 'error_message' FROM "
	                   "(SELECT json_serialize_plan('SELECT ''reject' || '_me''') AS j)");
	REQUIRE(CHECK_COLUMN(result, 0, {"permission"}));
	REQUIRE(CHECK_COLUMN(result, 1, {"post-parse hook rejected: SELECT 'reject_me'"}));

	// with several statements, every one is seen before the first is bound: the first fails if it is bound, and the
	// result is the hook's rejection of the second
	result = con.Query("SELECT j ->> 'error_type', j ->> 'error_message' FROM "
	                   "(SELECT json_serialize_plan('SELECT * FROM missing_table; SELECT ''reject' || '_me''') AS j)");
	REQUIRE(CHECK_COLUMN(result, 0, {"permission"}));
	REQUIRE(CHECK_COLUMN(result, 1, {"post-parse hook rejected: SELECT 'reject_me'"}));

	// json_serialize_sql only parses, without the client's context: the hook does not see its argument
	recorder->Clear();
	result = con.Query("SELECT json_serialize_sql('SELECT ''reject' || '_me''') ->> 'error'");
	REQUIRE(CHECK_COLUMN(result, 0, {"false"}));
	REQUIRE(recorder->queries.size() == 1);
}

TEST_CASE("Post-parse hook sees the statement json_execute_serialized_sql runs", "[api][post_parse]") {
	DBConfig config;
	auto recorder = AddRecorder(config);
	DuckDB db(nullptr, &config);
	if (!db.ExtensionIsLoaded("json")) {
		return;
	}
	Connection con(db);
	// reject by the syntax tree: the table function's statement is deserialized and has no query text
	vector<ClientContext *> contexts;
	recorder->on_statement = [&contexts](ClientContext &context, const SQLStatement &statement) {
		contexts.push_back(&context);
		if (statement.ToString().find("reject_me") != string::npos) {
			throw PermissionException("post-parse hook rejected the tree: %s", statement.ToString());
		}
	};
	// missing_table fails the statement if it is bound before the hook sees it
	const string serialized = "json_serialize_sql('SELECT ''reject' || '_me'' FROM missing_table')";
	recorder->Clear();

	// the PRAGMA renders the deserialized statement as SQL, which is parsed again with the client's context
	auto result = con.Query("PRAGMA json_execute_serialized_sql(" + serialized + ")");
	REQUIRE_FAIL(result);
	REQUIRE_THAT(result->GetError(), Contains("post-parse hook rejected the tree") && Contains("reject_me"));
	REQUIRE(recorder->queries.size() == 2);
	REQUIRE_THAT(recorder->queries[1], Contains("reject_me"));

	// the table function binds and runs the deserialized statement on a connection of its own; it gives the statement
	// to the hook with the client's context first, without query text
	recorder->Clear();
	contexts.clear();
	result = con.Query("SELECT * FROM json_execute_serialized_sql(" + serialized + ")");
	REQUIRE_FAIL(result);
	REQUIRE_THAT(result->GetError(), Contains("post-parse hook rejected the tree") && Contains("reject_me"));
	REQUIRE(recorder->queries.size() == 2);
	REQUIRE(recorder->queries[1].empty());
	REQUIRE(recorder->types[1] == StatementType::SELECT_STATEMENT);
	REQUIRE(!recorder->fragments[1]);
	REQUIRE(contexts.size() == 2);
	REQUIRE(contexts[1] == con.context.get());

	// a statement the hook allows runs
	recorder->Clear();
	result = con.Query("SELECT * FROM json_execute_serialized_sql(json_serialize_sql('SELECT 42'))");
	REQUIRE(CHECK_COLUMN(result, 0, {42}));
	REQUIRE(recorder->queries.size() == 2);
	REQUIRE(recorder->queries[1].empty());
}

TEST_CASE("Post-parse hook is called concurrently from worker threads", "[api][post_parse]") {
	// json_serialize_plan parses its argument on the worker threads that evaluate it, so the hook is called from
	// several threads at once and must be safe for that. Every row's argument differs, so each is parsed; this checks
	// that every call is made. A data race inside a hook would need ThreadSanitizer to show.
	struct Counter : public ParserExtensionInfo {
		std::atomic<idx_t> calls {0};
	};
	auto counter = make_shared_ptr<Counter>();
	DBConfig config;
	PostParseExtension ext;
	ext.post_parse_function = [](PostParseExtensionInput &input, const SQLStatement &) {
		auto &c = input.info->Cast<Counter>();
		c.calls++;
	};
	ext.parser_info = counter;
	PostParseExtension::Register(config, std::move(ext));
	DuckDB db(nullptr, &config);
	if (!db.ExtensionIsLoaded("json")) {
		return;
	}
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=8"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE big AS SELECT 'SELECT ' || i AS s FROM range(100000) t(i)"));
	auto result = con.Query("SELECT count(*) FROM big WHERE json_serialize_plan(s) ->> 'error' = 'false'");
	REQUIRE(CHECK_COLUMN(result, 0, {100000}));
	// one call for each row's argument, besides the client's own statements
	REQUIRE(counter->calls >= 100000);
}

//===--------------------------------------------------------------------===//
// 9. Fragments of the client's SQL, which the Parse*List helpers wrap in a statement they build
//===--------------------------------------------------------------------===//

TEST_CASE("Post-parse hook sees a COPY ORDER_BY option expression", "[api][post_parse]") {
	// COPY ... (ORDER_BY '<expr list>') parses the option string with Parser::ParseOrderList while the COPY is bound
	// (bind_helpers.cpp), with the client's parser: the hook sees the SELECT it wraps the list in, as a fragment,
	// before the expression is bound.
	auto dir = TestJoinPath(LocalTestDirectoryPath(), "post_parse_copy");
	TestDeleteDirectory(dir);
	TestCreateDirectory(dir);
	auto out = TestJoinPath(dir, "o.csv");
	DBConfig config;
	auto recorder = AddRecorder(config);
	DuckDB db(nullptr, &config);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(a INTEGER)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t VALUES (3), (1), (2)"));
	recorder->Clear();

	const string copy = "COPY (SELECT a FROM t) TO '" + out +
	                    "' (FORMAT csv, ORDER_BY 'a + CAST(current_setting(''threads'') AS INTEGER)')";
	REQUIRE_NO_FAIL(con.Query(copy));
	bool saw_order_by = false;
	for (idx_t i = 0; i < recorder->queries.size(); i++) {
		auto &q = recorder->queries[i];
		if (q.find("current_setting") != string::npos && q.find("ORDER BY") != string::npos) {
			REQUIRE(recorder->fragments[i]);
			saw_order_by = true;
		}
	}
	REQUIRE(saw_order_by);

	// a denied function in the ORDER_BY string is rejected before the COPY runs
	recorder->reject_containing = "current_setting";
	auto result = con.Query(copy);
	REQUIRE_FAIL(result);
	REQUIRE(result->GetErrorType() == ExceptionType::PERMISSION);
	REQUIRE_THAT(result->GetError(), Contains("post-parse hook rejected"));
	TestDeleteDirectory(dir);
}

TEST_CASE("Post-parse hook sees the relational API's expression strings", "[api][post_parse]") {
	// The relational API parses its expression strings (Filter, Project, Values and the others) with the Parse*List
	// helpers and the connection's parser: the hook sees each, as a fragment, before it is bound.
	DBConfig config;
	auto recorder = AddRecorder(config);
	DuckDB db(nullptr, &config);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(a INTEGER)"));
	recorder->reject_containing = "current_setting";

	REQUIRE_THROWS_WITH(con.Table("t")->Filter("current_setting('threads')::INTEGER > 0")->Execute(),
	                    Contains("post-parse hook rejected"));
	REQUIRE_THROWS_WITH(con.Table("t")->Project("current_setting('threads') AS v")->Execute(),
	                    Contains("post-parse hook rejected"));
	REQUIRE_THROWS_WITH(con.Values("(current_setting('threads'))")->Execute(), Contains("post-parse hook rejected"));
}

TEST_CASE("Post-parse hook marks the statements built around a fragment", "[api][post_parse]") {
	DBConfig config;
	auto recorder = AddRecorder(config);
	DuckDB db(nullptr, &config);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(a INTEGER, b INTEGER)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t VALUES (1, 10), (2, 20)"));
	recorder->Clear();

	// the client's own statement is not a fragment
	REQUIRE_NO_FAIL(con.Query("SELECT 1"));
	REQUIRE(recorder->fragments == vector<bool> {false});

	SECTION("Relation::Aggregate wraps its group list in SELECT 42 GROUP BY") {
		recorder->Clear();
		auto result = con.Table("t")->Aggregate("sum(b)", "a")->Order("1")->Execute();
		REQUIRE(CHECK_COLUMN(result, 0, {10, 20}));
		bool saw_group_by = false;
		for (idx_t i = 0; i < recorder->queries.size(); i++) {
			REQUIRE(recorder->fragments[i]);
			if (recorder->queries[i] == "SELECT 42 GROUP BY a") {
				saw_group_by = true;
			}
		}
		REQUIRE(saw_group_by);
	}
	SECTION("Relation::Update wraps its SET list in UPDATE tbl SET") {
		recorder->Clear();
		con.Table("t")->Update("b = b + 1", "a = 1");
		bool saw_update = false;
		for (idx_t i = 0; i < recorder->queries.size(); i++) {
			REQUIRE(recorder->fragments[i]);
			if (recorder->queries[i] == "UPDATE tbl SET b = b + 1") {
				REQUIRE(recorder->types[i] == StatementType::UPDATE_STATEMENT);
				saw_update = true;
			}
		}
		REQUIRE(saw_update);
		REQUIRE(CountRows(con, "t WHERE b = 11") == 1);
	}
	SECTION("Connection::Values wraps its list in VALUES") {
		recorder->Clear();
		auto result = con.Values("(1, 2)")->Execute();
		REQUIRE(CHECK_COLUMN(result, 0, {1}));
		REQUIRE(CHECK_COLUMN(result, 1, {2}));
		bool saw_values = false;
		for (idx_t i = 0; i < recorder->queries.size(); i++) {
			REQUIRE(recorder->fragments[i]);
			if (recorder->queries[i] == "VALUES (1, 2)") {
				REQUIRE(recorder->types[i] == StatementType::SELECT_STATEMENT);
				saw_values = true;
			}
		}
		REQUIRE(saw_values);
	}
	SECTION("a subquery in a fragment, with the table it names, is part of the statement") {
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE restricted(x INTEGER)"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO restricted VALUES (2)"));
		recorder->Clear();
		auto result = con.Table("t")->Filter("a IN (SELECT x FROM restricted)")->Execute();
		REQUIRE(CHECK_COLUMN(result, 0, {2}));
		bool saw_filter = false;
		for (idx_t i = 0; i < recorder->queries.size(); i++) {
			REQUIRE(recorder->fragments[i]);
			if (recorder->queries[i] == "SELECT a IN (SELECT x FROM restricted)") {
				saw_filter = true;
			}
		}
		REQUIRE(saw_filter);
		// so a hook that checks the names in a fragment can reject it
		recorder->reject_containing = "restricted";
		REQUIRE_THROWS_WITH(con.Table("t")->Filter("a IN (SELECT x FROM restricted)")->Execute(),
		                    Contains("post-parse hook rejected"));
	}
	SECTION("a type string is wrapped in CREATE TABLE tbl") {
		auto dir = TestJoinPath(LocalTestDirectoryPath(), "post_parse_fragment_csv");
		TestDeleteDirectory(dir);
		TestCreateDirectory(dir);
		auto csv = TestJoinPath(dir, "f.csv");
		{
			std::ofstream out(csv);
			out << "1\n2\n";
		}
		recorder->Clear();
		auto result =
		    con.Query("SELECT sum(a) FROM read_csv('" + csv + "', columns = {'a': 'INTEGER'}, header = false)");
		REQUIRE(CHECK_COLUMN(result, 0, {3}));
		// the client's statement, then the column list read_csv parses its type string with
		REQUIRE(recorder->fragments.size() >= 2);
		REQUIRE_FALSE(recorder->fragments[0]);
		bool saw_create = false;
		for (idx_t i = 1; i < recorder->queries.size(); i++) {
			REQUIRE(recorder->fragments[i]);
			if (recorder->types[i] == StatementType::CREATE_STATEMENT) {
				REQUIRE_THAT(recorder->queries[i], StartsWith("CREATE TABLE tbl (") && Contains("INTEGER"));
				saw_create = true;
			}
		}
		REQUIRE(saw_create);
		TestDeleteDirectory(dir);
	}
}

TEST_CASE("Post-parse hook is given a fragment only as one statement of the wrapper's type", "[api][post_parse]") {
	// A fragment that parses into more statements than the wrapper is rejected before any hook sees it, so no statement
	// the fragment added reaches a hook marked as a fragment
	DBConfig config;
	auto recorder = AddRecorder(config);
	DuckDB db(nullptr, &config);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(a INTEGER)"));

	SECTION("an expression list") {
		recorder->Clear();
		REQUIRE_THROWS_WITH(con.Table("t")->Filter("a = 1; ATTACH ':memory:' AS z")->Execute(),
		                    Contains("Expected a single SELECT statement"));
		REQUIRE(recorder->queries.empty());
	}
	SECTION("a type string") {
		auto dir = TestJoinPath(LocalTestDirectoryPath(), "post_parse_fragment_count");
		TestDeleteDirectory(dir);
		TestCreateDirectory(dir);
		auto csv = TestJoinPath(dir, "f.csv");
		{
			std::ofstream out(csv);
			out << "1\n";
		}
		recorder->Clear();
		auto result = con.Query("SELECT * FROM read_csv('" + csv +
		                        "', columns = {'a': 'INTEGER); ATTACH '':memory:'' AS z; CREATE TABLE u (b INTEGER'}, "
		                        "header = false)");
		REQUIRE_FAIL(result);
		REQUIRE_THAT(result->GetError(), Contains("can not be converted to a DuckDB Type"));
		// only the client's own statement
		REQUIRE(recorder->queries.size() == 1);
		REQUIRE_FALSE(recorder->fragments[0]);
		TestDeleteDirectory(dir);
	}
	// and nothing in the fragment ran
	auto result = con.Query("SELECT count(*) FROM duckdb_databases() WHERE database_name = 'z'");
	REQUIRE(CHECK_COLUMN(result, 0, {0}));
}

TEST_CASE("Post-parse hook rejection of a type string surfaces as the hook's error", "[api][post_parse]") {
	// Parsing a type string catches ParserException only, so a hook's rejection is not replaced by a type error unless
	// the hook throws ParserException itself, which post_parse_function_t says not to do
	DBConfig config;
	auto recorder = AddRecorder(config);
	DuckDB db(nullptr, &config);
	Connection con(db);
	auto dir = TestJoinPath(LocalTestDirectoryPath(), "post_parse_type_reject");
	TestDeleteDirectory(dir);
	TestCreateDirectory(dir);
	auto csv = TestJoinPath(dir, "f.csv");
	{
		std::ofstream out(csv);
		out << "1\n";
	}
	recorder->reject_containing = "CREATE TABLE tbl";
	auto result = con.Query("SELECT * FROM read_csv('" + csv + "', columns = {'a': 'INTEGER'}, header = false)");
	REQUIRE_FAIL(result);
	REQUIRE(result->GetErrorType() == ExceptionType::PERMISSION);
	REQUIRE_THAT(result->GetError(), Contains("post-parse hook rejected"));
	// a type string that does not parse is still reported as a type error
	recorder->reject_containing.clear();
	result = con.Query("SELECT * FROM read_csv('" + csv + "', columns = {'a': 'INTEGER)'}, header = false)");
	REQUIRE_FAIL(result);
	REQUIRE_THAT(result->GetError(), Contains("can not be converted to a DuckDB Type"));
	// a hook that rejects a type string by throwing ParserException is reported as the type error, without its message
	recorder->on_statement = [](ClientContext &, const SQLStatement &statement) {
		if (statement.query.find("CREATE TABLE tbl") != string::npos) {
			throw ParserException("rejected by the hook");
		}
	};
	result = con.Query("SELECT * FROM read_csv('" + csv + "', columns = {'a': 'INTEGER'}, header = false)");
	REQUIRE_FAIL(result);
	REQUIRE_THAT(result->GetError(), Contains("can not be converted to a DuckDB Type"));
	REQUIRE_THAT(result->GetError(), !Contains("rejected by the hook"));
	TestDeleteDirectory(dir);
}
