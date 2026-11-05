#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/main/parse_iterator.hpp"
#include "duckdb/main/statement_iterator.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/sql_statement.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/parsed_data/create_info.hpp"

using namespace duckdb;

// ParseIterator and StatementIterator no longer share a contract. Both bind their ClientContext at
// construction, so Peek()/GetStatement() take no context argument:
//   ParseIterator : Peek() parses + buffers one statement; GetStatement() returns it (no
//                   preprocessing). GetStatement() before Peek returns nullptr.
//   StatementIterator: Peek() answers "is there more input?" (parses ahead); GetStatement()
//                   returns the next user statement as parsed, or nullptr once the input is
//                   exhausted. GetStatement is self-sufficient (works without a prior Peek).
// So the two are tested separately below.

//===--------------------------------------------------------------------===//
// ParseIterator
//===--------------------------------------------------------------------===//

static vector<unique_ptr<SQLStatement>> DrainParse(ParseIterator &it) {
	vector<unique_ptr<SQLStatement>> result;
	while (it.Peek()) {
		auto stmt = it.GetStatement();
		REQUIRE(stmt);
		result.push_back(std::move(stmt));
	}
	return result;
}

TEST_CASE("ParseIterator: single statement", "[api][parse_iterator]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &ctx = *con.context;

	ParseIterator it(ctx, "SELECT 1;");
	REQUIRE(it.Peek());
	auto stmt = it.GetStatement();
	REQUIRE(stmt);
	REQUIRE(stmt->type == StatementType::SELECT_STATEMENT);
	REQUIRE_FALSE(it.Peek());
	REQUIRE_FALSE(it.GetStatement());
}

TEST_CASE("ParseIterator: empty and whitespace input yields nothing", "[api][parse_iterator]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &ctx = *con.context;

	ParseIterator empty(ctx, "");
	REQUIRE_FALSE(empty.Peek());
	ParseIterator ws(ctx, "   \n\t  ");
	REQUIRE_FALSE(ws.Peek());
}

TEST_CASE("ParseIterator: multiple statements in order", "[api][parse_iterator]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &ctx = *con.context;

	ParseIterator it(ctx, "SELECT 1; SELECT 2; SELECT 3;");
	auto stmts = DrainParse(it);
	REQUIRE(stmts.size() == 3);
	for (auto &stmt : stmts) {
		REQUIRE(stmt->type == StatementType::SELECT_STATEMENT);
	}
}

TEST_CASE("ParseIterator: Peek is idempotent until consumed", "[api][parse_iterator]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &ctx = *con.context;

	ParseIterator it(ctx, "SELECT 1; SELECT 2;");
	REQUIRE(it.Peek());
	REQUIRE(it.Peek());
	REQUIRE(it.Peek());
	REQUIRE(it.GetStatement());
	REQUIRE(it.Peek());
	REQUIRE(it.GetStatement());
	REQUIRE_FALSE(it.Peek());
}

TEST_CASE("ParseIterator: GetStatement without prior Peek returns nullptr", "[api][parse_iterator]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &ctx = *con.context;

	ParseIterator it(ctx, "SELECT 1;");
	REQUIRE_FALSE(it.GetStatement());
	REQUIRE(it.Peek());
	REQUIRE(it.GetStatement());
}

TEST_CASE("ParseIterator: mixed statement types", "[api][parse_iterator]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &ctx = *con.context;

	ParseIterator it(ctx, "CREATE TABLE t (a INT); INSERT INTO t VALUES (1); SELECT * FROM t;");
	auto stmts = DrainParse(it);
	REQUIRE(stmts.size() == 3);
	REQUIRE(stmts[0]->type == StatementType::CREATE_STATEMENT);
	REQUIRE(stmts[1]->type == StatementType::INSERT_STATEMENT);
	REQUIRE(stmts[2]->type == StatementType::SELECT_STATEMENT);
}

TEST_CASE("ParseIterator: parser errors surface through Peek", "[api][parse_iterator]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &ctx = *con.context;

	ParseIterator it(ctx, "SELECT FROM;");
	REQUIRE_THROWS(it.Peek());
	// A parse error ends the iteration, instead of parsing the failing statement again
	REQUIRE_FALSE(it.Peek());
	REQUIRE_FALSE(it.HasMore());
}

TEST_CASE("ParseIterator: movable", "[api][parse_iterator]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &ctx = *con.context;

	// Move-constructible (it binds a ClientContext reference, so it is not move-assignable).
	ParseIterator it(ctx, "SELECT 1; SELECT 2;");
	REQUIRE(it.Peek());
	REQUIRE(it.GetStatement());

	ParseIterator moved(std::move(it));
	REQUIRE(moved.Peek());
	REQUIRE(moved.GetStatement());
	REQUIRE_FALSE(moved.Peek());
}

TEST_CASE("ParseIterator: separators are skipped, no trailing separator needed", "[api][parse_iterator]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &ctx = *con.context;

	ParseIterator only_seps(ctx, ";;;;;;;;;;;");
	REQUIRE_FALSE(only_seps.Peek());

	ParseIterator heavy(ctx, ";;;;;;;;;; SELECT 42;;;;;; SELECT 1000");
	auto stmts = DrainParse(heavy);
	REQUIRE(stmts.size() == 2);
	REQUIRE(StringUtil::Contains(stmts[0]->query, "42"));
	REQUIRE(StringUtil::Contains(stmts[1]->query, "1000"));
}

TEST_CASE("ParseIterator: statement query text is populated and normalized", "[api][parse_iterator]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &ctx = *con.context;

	ParseIterator it(ctx, "SELECT 1; SELECT 2;");
	REQUIRE(it.Peek());
	auto s0 = it.GetStatement();
	REQUIRE(s0);
	REQUIRE(StringUtil::Contains(s0->query, "SELECT 1"));
	REQUIRE_FALSE(StringUtil::Contains(s0->query, "SELECT 2"));
	REQUIRE(s0->stmt_location.offset == 0);
	REQUIRE(s0->stmt_location.length == s0->query.size());

	Parser reparser(ctx);
	reparser.ParseQuery(s0->query);
	REQUIRE(reparser.statements.size() == 1);
}

TEST_CASE("ParseIterator: CREATE propagates query into CreateInfo", "[api][parse_iterator]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &ctx = *con.context;

	ParseIterator it(ctx, "CREATE TABLE t (a INT);");
	REQUIRE(it.Peek());
	auto stmt = it.GetStatement();
	REQUIRE(stmt);
	auto &create = stmt->Cast<CreateStatement>();
	REQUIRE(create.info->sql == stmt->query);
	REQUIRE(StringUtil::Contains(create.info->sql, "CREATE TABLE"));
}

TEST_CASE("ParseIterator: semicolons in strings and comments handled", "[api][parse_iterator]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &ctx = *con.context;

	ParseIterator in_string(ctx, "SELECT 'a;b;c' AS x;");
	REQUIRE(DrainParse(in_string).size() == 1);

	ParseIterator with_comments(ctx, "/* block */ SELECT 1; -- line comment\nSELECT 2;");
	REQUIRE(DrainParse(with_comments).size() == 2);
}

//===--------------------------------------------------------------------===//
// StatementIterator (Peek = "more input?", Get = the next user statement)
//===--------------------------------------------------------------------===//

static vector<unique_ptr<SQLStatement>> DrainStatements(StatementIterator &it) {
	vector<unique_ptr<SQLStatement>> result;
	while (it.Peek()) {
		auto stmt = it.GetStatement();
		REQUIRE(stmt);
		result.push_back(std::move(stmt));
	}
	return result;
}

TEST_CASE("StatementIterator: single statement", "[api][statement_iterator]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &ctx = *con.context;

	StatementIterator it {ParseIterator(ctx, "SELECT 1;")};
	REQUIRE(it.Peek());
	auto stmt = it.GetStatement();
	REQUIRE(stmt);
	REQUIRE(stmt->type == StatementType::SELECT_STATEMENT);
	REQUIRE_FALSE(it.Peek());
	REQUIRE_FALSE(it.GetStatement());
}

TEST_CASE("StatementIterator: empty input yields nothing", "[api][statement_iterator]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &ctx = *con.context;

	StatementIterator it {ParseIterator(ctx, "")};
	REQUIRE_FALSE(it.Peek());
	REQUIRE_FALSE(it.GetStatement());
}

TEST_CASE("StatementIterator: multiple statements in order", "[api][statement_iterator]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &ctx = *con.context;

	StatementIterator it {ParseIterator(ctx, "SELECT 1; SELECT 2; SELECT 3;")};
	auto stmts = DrainStatements(it);
	REQUIRE(stmts.size() == 3);
	for (auto &stmt : stmts) {
		REQUIRE(stmt->type == StatementType::SELECT_STATEMENT);
	}
}

TEST_CASE("StatementIterator: mixed statement types", "[api][statement_iterator]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &ctx = *con.context;

	StatementIterator it {ParseIterator(ctx, "CREATE TABLE t (a INT); INSERT INTO t VALUES (1); SELECT * FROM t;")};
	auto stmts = DrainStatements(it);
	REQUIRE(stmts.size() == 3);
	REQUIRE(stmts[0]->type == StatementType::CREATE_STATEMENT);
	REQUIRE(stmts[1]->type == StatementType::INSERT_STATEMENT);
	REQUIRE(stmts[2]->type == StatementType::SELECT_STATEMENT);
}

TEST_CASE("StatementIterator: separator-only input yields nothing", "[api][statement_iterator]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &ctx = *con.context;

	StatementIterator it {ParseIterator(ctx, ";;;;;;")};
	REQUIRE_FALSE(it.Peek());
}

TEST_CASE("StatementIterator: Peek is a pure predicate, GetStatement does the work", "[api][statement_iterator]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &ctx = *con.context;

	StatementIterator it {ParseIterator(ctx, "SELECT 1; SELECT 2;")};
	// Repeated Peek does not consume.
	REQUIRE(it.Peek());
	REQUIRE(it.Peek());
	auto a = it.GetStatement();
	REQUIRE(a);
	REQUIRE(it.Peek());
	auto b = it.GetStatement();
	REQUIRE(b);
	REQUIRE_FALSE(it.Peek());
}

TEST_CASE("StatementIterator: GetStatement works without a prior Peek", "[api][statement_iterator]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &ctx = *con.context;

	// Unlike ParseIterator, StatementIterator's GetStatement is self-sufficient: it pulls
	// on demand, no priming Peek required.
	StatementIterator it {ParseIterator(ctx, "SELECT 7;")};
	auto stmt = it.GetStatement();
	REQUIRE(stmt);
	REQUIRE(StringUtil::Contains(stmt->query, "7"));
	REQUIRE_FALSE(it.Peek());
}

TEST_CASE("StatementIterator: parser errors surface through Peek", "[api][statement_iterator]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &ctx = *con.context;

	StatementIterator it {ParseIterator(ctx, "SELECT FROM;")};
	REQUIRE_THROWS(it.Peek());
}

TEST_CASE("StatementIterator: movable", "[api][statement_iterator]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &ctx = *con.context;

	StatementIterator it {ParseIterator(ctx, "SELECT 1; SELECT 2;")};
	REQUIRE(it.Peek());
	REQUIRE(it.GetStatement());

	StatementIterator moved(std::move(it));
	REQUIRE(moved.Peek());
	REQUIRE(moved.GetStatement());
	REQUIRE_FALSE(moved.Peek());
}

TEST_CASE("StatementIterator: a SET reaches the later statements of the same string", "[api][statement_iterator]") {
	DuckDB db(nullptr);
	Connection con(db);

	// the default division of two integers is a float division
	auto result = con.Query("SELECT 2/4");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(CHECK_COLUMN(result, 0, {0.5}));

	// a SET between the statements of one string changes how the later statements parse
	result = con.Query("SET integer_division=true; SELECT 2/4");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(CHECK_COLUMN(result, 0, {0}));
}

TEST_CASE("StatementIterator: a statement the engine expands is yielded as written", "[api][statement_iterator]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &ctx = *con.context;
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t (k VARCHAR, v INTEGER)"));

	StatementIterator it {ParseIterator(ctx,
	                                    "PIVOT t ON k USING sum(v); ALTER TABLE t ADD COLUMN r DOUBLE DEFAULT "
	                                    "random(); PRAGMA show_tables; BEGIN; PIVOT t ON k USING sum(v); COMMIT;")};
	auto stmts = DrainStatements(it);
	REQUIRE(stmts.size() == 6);
	REQUIRE(stmts[0]->type == StatementType::MULTI_STATEMENT);
	REQUIRE(stmts[1]->type == StatementType::MULTI_STATEMENT);
	REQUIRE(stmts[2]->type == StatementType::PRAGMA_STATEMENT);
	REQUIRE(stmts[3]->type == StatementType::TRANSACTION_STATEMENT);
	REQUIRE(stmts[4]->type == StatementType::MULTI_STATEMENT);
	REQUIRE(stmts[5]->type == StatementType::TRANSACTION_STATEMENT);
	REQUIRE(stmts[1]->query == "ALTER TABLE t ADD COLUMN r DOUBLE DEFAULT random(); ");
}
