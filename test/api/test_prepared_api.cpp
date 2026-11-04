#include "catch.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

TEST_CASE("Test prepared statements API", "[api]") {
	duckdb::unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);

	// prepare no statements
	REQUIRE_FAIL(con.Prepare(""));

	// PrepareAndExecute with no values
	duckdb::vector<Value> values;
	REQUIRE_FAIL(con.Submit("", values));

	REQUIRE_NO_FAIL(con.Query("CREATE TABLE a (i TINYINT)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO a VALUES (11), (12), (13)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE strings(s VARCHAR)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO strings VALUES (NULL), ('test')"));

	// query using a prepared statement
	// integer:
	result = con.Query("SELECT COUNT(*) FROM a WHERE i=$1", 12);
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
	// strings:
	result = con.Query("SELECT COUNT(*) FROM strings WHERE s=$1", "test");
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
	// multiple parameters
	result = con.Query("SELECT COUNT(*) FROM a WHERE i>$1 AND i<$2", 10, 13);
	REQUIRE(CHECK_COLUMN(result, 0, {2}));

	// test various integer types
	result = con.Query("SELECT COUNT(*) FROM a WHERE i=$1", (int8_t)12);
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
	result = con.Query("SELECT COUNT(*) FROM a WHERE i=$1", (int16_t)12);
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
	result = con.Query("SELECT COUNT(*) FROM a WHERE i=$1", (int32_t)12);
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
	result = con.Query("SELECT COUNT(*) FROM a WHERE i=$1", (int64_t)12);
	REQUIRE(CHECK_COLUMN(result, 0, {1}));

	// create a prepared statement and use it to query
	auto prepare = con.Prepare("SELECT COUNT(*) FROM a WHERE i=$1");

	result = prepare->Execute(12);
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
	result = prepare->Execute(13);
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
	REQUIRE(prepare->GetParameterCount() == 1);
}

TEST_CASE("Test type resolution of function with parameter expressions", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	duckdb::unique_ptr<QueryResult> result;

	// can deduce type of prepared parameter here
	auto prepared = con.Prepare("select 1 + $1");
	REQUIRE(!prepared->HasError());

	result = prepared->Execute(1);
	REQUIRE(CHECK_COLUMN(result, 0, {2}));

	// no prepared statement
	REQUIRE_FAIL(con.Query("SELECT ?"));
}

TEST_CASE("Test prepared EXPLAIN ANALYZE", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);

	auto prepared = con.Prepare("EXPLAIN ANALYZE SELECT 42");
	REQUIRE(!prepared->HasError());
	// run another query between preparing and executing, and execute more than once
	REQUIRE_NO_FAIL(con.Query("SELECT 1"));
	for (idx_t i = 0; i < 2; i++) {
		auto result = prepared->Execute();
		REQUIRE_NO_FAIL(*result);
		auto plan = result->Collection().GetValue(1, 0).ToString();
		REQUIRE(!StringUtil::Contains(plan, "Query profiling is disabled"));
		REQUIRE(StringUtil::Contains(plan, "Total Time"));
	}
}

TEST_CASE("Test prepared statements and dependencies", "[api]") {
	duckdb::unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db), con2(db);

	REQUIRE_NO_FAIL(con.Query("CREATE TABLE a(i TINYINT)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO a VALUES (11), (12), (13)"));

	// query using a prepared statement in con1
	result = con.Query("SELECT COUNT(*) FROM a WHERE i=$1", 12);
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
	// now delete the table in con2
	REQUIRE_NO_FAIL(con2.Query("DROP TABLE a"));

	REQUIRE_NO_FAIL(con.Query("CREATE TABLE a(i TINYINT)"));

	// keep a prepared statement around
	auto prepare = con.Prepare("SELECT COUNT(*) FROM a WHERE i=$1");

	// we can drop the table
	REQUIRE_NO_FAIL(con2.Query("DROP TABLE a"));

	// now the prepared statement fails when executing
	REQUIRE_FAIL(prepare->Execute(11));
}

TEST_CASE("Prepared temp table insert is invalidated after drop", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query("CREATE TEMP TABLE t(i INTEGER)"));
	auto prepared = con.Prepare("INSERT INTO t VALUES (42)");
	REQUIRE(!prepared->HasError());

	REQUIRE_NO_FAIL(con.Query("DROP TABLE t"));

	auto result = prepared->Execute();
	REQUIRE(result->HasError());
	REQUIRE(result->GetError().find("does not exist") != string::npos);
}

TEST_CASE("Writes to temporary objects register a modification of the temp catalog", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TEMP TABLE t(i INTEGER PRIMARY KEY)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TEMP TABLE audit(i INTEGER)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TEMP SEQUENCE s"));

	auto read = con.Prepare("SELECT * FROM t");
	REQUIRE(!read->HasError());
	REQUIRE(read->GetStatementProperties().modified_databases.empty());

	vector<string> writes {
	    "INSERT INTO t VALUES (1)",
	    "UPDATE t SET i = 2",
	    "DELETE FROM t",
	    "INSERT OR REPLACE INTO t VALUES (1)",
	    "MERGE INTO t USING (SELECT 1 AS i) s ON t.i = s.i WHEN NOT MATCHED THEN INSERT VALUES (s.i)",
	    "CREATE TEMP TABLE t2(i INTEGER)",
	    "CREATE TEMP VIEW v AS SELECT 42",
	    "CREATE TRIGGER trg AFTER INSERT ON t FOR EACH STATEMENT INSERT INTO audit VALUES (1)",
	    "CREATE INDEX t_i ON t(i)",
	    "ALTER TABLE t RENAME TO t3",
	    "COMMENT ON TABLE t IS 'c'",
	    "DROP TABLE t",
	    "SELECT nextval('s')",
	};
	for (auto &write : writes) {
		INFO(write);
		auto prepared = con.Prepare(write);
		REQUIRE(!prepared->HasError());
		auto &modified = prepared->GetStatementProperties().modified_databases;
		REQUIRE(modified.size() == 1);
		REQUIRE(modified.count(Identifier::TempCatalog()) == 1);
	}
}

TEST_CASE("Dropping connection with prepared statement resets dependencies", "[api]") {
	duckdb::unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	auto con = make_uniq<Connection>(db);
	Connection con2(db);

	REQUIRE_NO_FAIL(con->Query("CREATE TABLE a(i TINYINT)"));
	REQUIRE_NO_FAIL(con->Query("INSERT INTO a VALUES (11), (12), (13)"));

	auto prepared = con->Prepare("SELECT COUNT(*) FROM a WHERE i=$1");
	result = prepared->Execute(12);
	REQUIRE(CHECK_COLUMN(result, 0, {1}));

	// we can drop the table
	REQUIRE_NO_FAIL(con2.Query("DROP TABLE a"));

	// after the table is dropped, the prepared statement no longer succeeds when run
	REQUIRE_FAIL(prepared->Execute(12));
	REQUIRE_FAIL(prepared->Execute(12));
}

TEST_CASE("Alter table and prepared statements", "[api]") {
	duckdb::unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	auto con = make_uniq<Connection>(db);
	Connection con2(db);

	REQUIRE_NO_FAIL(con->Query("CREATE TABLE a(i TINYINT)"));
	REQUIRE_NO_FAIL(con->Query("INSERT INTO a VALUES (11), (12), (13)"));

	auto prepared = con->Prepare("SELECT * FROM a WHERE i=$1");
	result = prepared->Execute(12);
	REQUIRE(CHECK_COLUMN(result, 0, {12}));

	REQUIRE(prepared->ColumnCount() == 1);
	REQUIRE(prepared->GetStatementType() == StatementType::SELECT_STATEMENT);
	REQUIRE(prepared->GetTypes()[0].id() == LogicalTypeId::TINYINT);
	REQUIRE(prepared->GetNames()[0] == "i");

	// we can alter the type of the column
	REQUIRE_NO_FAIL(con2.Query("ALTER TABLE a ALTER i TYPE BIGINT USING i"));

	// after the table is altered, the return types change, but the rebind is still successful
	result = prepared->Execute(12);
	REQUIRE(CHECK_COLUMN(result, 0, {12}));
}

TEST_CASE("Test that prepared statements live in the client context", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);

	auto prepared_statement_count = [&con]() {
		auto result = con.Query("SELECT count(*) FROM duckdb_prepared_statements()");
		return result->Collection().GetValue(0, 0).GetValue<int64_t>();
	};
	REQUIRE(prepared_statement_count() == 0);

	auto prepared = con.Prepare("SELECT $1::INTEGER");
	REQUIRE(!prepared->HasError());
	// the statement is prepared under a generated name in the client context
	REQUIRE(prepared_statement_count() == 1);
	REQUIRE(StringUtil::StartsWith(prepared->GetName(), "duckdb_prepare_internal_"));
	auto names = con.Query("SELECT name FROM duckdb_prepared_statements()");
	REQUIRE(CHECK_COLUMN(names, 0, {Value(prepared->GetName())}));

	auto result = prepared->Execute(42);
	REQUIRE(CHECK_COLUMN(result, 0, {42}));

	// destroying the prepared statement deallocates it again
	prepared.reset();
	REQUIRE(prepared_statement_count() == 0);
}

TEST_CASE("Test destructors of prepared statements", "[api]") {
	duckdb::unique_ptr<DuckDB> db;
	duckdb::unique_ptr<Connection> con;
	duckdb::unique_ptr<PreparedStatement> prepare;
	duckdb::unique_ptr<QueryResult> result;

	// test destruction of connection
	db = make_uniq<DuckDB>(nullptr);
	con = make_uniq<Connection>(*db);
	// create a prepared statement
	prepare = con->Prepare("SELECT $1::INTEGER+$2::INTEGER");
	// we can execute it
	result = prepare->Execute(3, 5);
	REQUIRE(CHECK_COLUMN(result, 0, {8}));
	// now destroy the connection
	con.reset();
	// the prepared statement lives in the connection - it can no longer be executed
	REQUIRE_FAIL(prepare->Execute(3, 5));
	// destroying the prepared statement is fine
	prepare.reset();

	// test destruction of db
	// create a connection and prepared statement again
	con = make_uniq<Connection>(*db);
	prepare = con->Prepare("SELECT $1::INTEGER+$2::INTEGER");
	// we can execute it
	result = prepare->Execute(3, 5);
	REQUIRE(CHECK_COLUMN(result, 0, {8}));
	// destroy the db
	db.reset();
	// we can still use the prepared statement
	REQUIRE_NO_FAIL(prepare->Execute(3, 5));
	// and the connection
	REQUIRE_NO_FAIL(con->Query("SELECT 42"));
	// we can also prepare new statements
	prepare = con->Prepare("SELECT $1::INTEGER+$2::INTEGER");
	REQUIRE(!prepare->HasError());
}

TEST_CASE("Test incorrect usage of prepared statements API", "[api]") {
	duckdb::unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query("CREATE TABLE a (i TINYINT)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO a VALUES (11), (12), (13)"));

	// this fails if there is a mismatch between number of arguments in prepare and in variadic
	// too few:
	REQUIRE_FAIL(con.Query("SELECT COUNT(*) FROM a WHERE i=$1 AND i>$2", 11));
	// too many:
	REQUIRE_FAIL(con.Query("SELECT COUNT(*) FROM a WHERE i=$1 AND i>$2", 11, 13, 17));

	// prepare an SQL string with a parse error
	auto prepare = con.Prepare("SELEC COUNT(*) FROM a WHERE i=$1");
	// we cannot execute this prepared statement
	REQUIRE(prepare->HasError());
	REQUIRE_FAIL(prepare->Execute(12));

	// cannot prepare multiple statements at once
	prepare = con.Prepare("SELECT COUNT(*) FROM a WHERE i=$1; SELECT 42+$2;");
	REQUIRE(prepare->HasError());
	REQUIRE_FAIL(prepare->Execute(12));

	// also not in the Query syntax
	REQUIRE_FAIL(con.Query("SELECT COUNT(*) FROM a WHERE i=$1; SELECT 42+$2", 11));
}

TEST_CASE("Test multiple prepared statements", "[api]") {
	duckdb::unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query("CREATE TABLE a (i TINYINT)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO a VALUES (11), (12), (13)"));

	// test that we can have multiple open prepared statements at a time
	auto prepare = con.Prepare("SELECT COUNT(*) FROM a WHERE i=$1");
	auto prepare2 = con.Prepare("SELECT COUNT(*) FROM a WHERE i>$1");

	result = prepare->Execute(12);
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
	result = prepare2->Execute(11);
	REQUIRE(CHECK_COLUMN(result, 0, {2}));
}

TEST_CASE("Test prepared statements and transactions", "[api]") {
	duckdb::unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);

	// create prepared statements in a transaction
	REQUIRE_NO_FAIL(con.Query("BEGIN TRANSACTION"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE a (i TINYINT)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO a VALUES (11), (12), (13)"));

	auto prepare = con.Prepare("SELECT COUNT(*) FROM a WHERE i=$1");
	auto prepare2 = con.Prepare("SELECT COUNT(*) FROM a WHERE i>$1");

	result = prepare->Execute(12);
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
	result = prepare2->Execute(11);
	REQUIRE(CHECK_COLUMN(result, 0, {2}));
	// now if we rollback our prepared statements are invalidated
	REQUIRE_NO_FAIL(con.Query("ROLLBACK"));

	REQUIRE_FAIL(prepare->Execute(12));
	REQUIRE_FAIL(prepare2->Execute(11));
}

TEST_CASE("Test prepared statement parameter counting", "[api]") {
	duckdb::unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);

	auto p0 = con.Prepare("SELECT 42");
	REQUIRE(!p0->HasError());
	REQUIRE(p0->GetNamedParameterMap().empty());

	auto p1 = con.Prepare("SELECT $1::int");
	REQUIRE(!p1->HasError());
	REQUIRE(p1->GetParameterCount() == 1);

	p1 = con.Prepare("SELECT ?::int");
	REQUIRE(!p1->HasError());
	REQUIRE(p1->GetParameterCount() == 1);

	auto p2 = con.Prepare("SELECT $1::int");
	REQUIRE(!p2->HasError());
	REQUIRE(p2->GetParameterCount() == 1);

	auto p3 = con.Prepare("SELECT ?::int, ?::string");
	REQUIRE(!p3->HasError());
	REQUIRE(p3->GetParameterCount() == 2);

	auto p4 = con.Prepare("SELECT $1::int, $2::string");
	REQUIRE(!p4->HasError());
	REQUIRE(p4->GetParameterCount() == 2);
}

TEST_CASE("Test ANALYZE", "[api]") {
	duckdb::unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);

	// ANALYZE runs without errors, note that ANALYZE is actually just ignored
	REQUIRE_NO_FAIL(con.Query("ANALYZE"));
	REQUIRE_NO_FAIL(con.Query("VACUUM"));

	auto prep = con.Prepare("ANALYZE");
	REQUIRE(!prep->HasError());
	auto res = prep->Execute();
	REQUIRE(!res->HasError());

	prep = con.Prepare("VACUUM");
	REQUIRE(!prep->HasError());
	res = prep->Execute();
	REQUIRE(!res->HasError());
}

TEST_CASE("Test DECIMAL with PreparedStatement", "[api]") {
	duckdb::unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);

	auto ps = con.Prepare("SELECT $1::DECIMAL(4,1), $2::DECIMAL(9,1), $3::DECIMAL(18,3), $4::DECIMAL(38,8)");
	result = ps->Execute(1.1, 100.1, 1401.123, "12481204981084098124.12398");
	REQUIRE(CHECK_COLUMN(result, 0, {1.1}));
	REQUIRE(CHECK_COLUMN(result, 1, {100.1}));
	REQUIRE(CHECK_COLUMN(result, 2, {1401.123}));
	REQUIRE(CHECK_COLUMN(result, 3, {12481204981084098124.12398}));
}

TEST_CASE("Test BLOB with PreparedStatement", "[api]") {
	duckdb::unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);

	// Creating a blob buffer with almost ALL ASCII chars
	uint8_t num_chars = 256 - 5; // skipping: '\0', '\n', '\15', ',', '\32'
	auto blob_chars = make_unsafe_uniq_array<char>(num_chars);
	char ch = '\0';
	idx_t buf_idx = 0;
	for (idx_t i = 0; i < 255; ++i, ++ch) {
		// skip chars: '\0', new line, shift in, comma, and crtl+Z
		if (ch == '\0' || ch == '\n' || ch == '\15' || ch == ',' || ch == '\32') {
			continue;
		}
		blob_chars[buf_idx] = ch;
		++buf_idx;
	}

	REQUIRE_NO_FAIL(con.Query("CREATE TABLE blobs (b BYTEA);"));

	// Insert blob values through a PreparedStatement
	Value blob_val = Value::BLOB(const_data_ptr_cast(blob_chars.get()), num_chars);
	duckdb::unique_ptr<PreparedStatement> ps = con.Prepare("INSERT INTO blobs VALUES (?::BYTEA)");
	ps->Execute(blob_val);
	REQUIRE(!ps->HasError());
	ps.reset();

	// Testing if the bytes are stored correctly
	result = con.Query("SELECT OCTET_LENGTH(b) FROM blobs");
	REQUIRE(CHECK_COLUMN(result, 0, {num_chars}));

	result = con.Query("SELECT count(b) FROM blobs");
	REQUIRE(CHECK_COLUMN(result, 0, {1}));

	result = con.Query("SELECT b FROM blobs");
	REQUIRE(CHECK_COLUMN(result, 0, {blob_val}));

	blob_chars.reset();
}

TEST_CASE("PREPARE for INSERT with dates", "[prepared]") {
	duckdb::unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);

	// prepared DATE insert
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE dates(d DATE)"));
	REQUIRE_NO_FAIL(con.Query("PREPARE s1 AS INSERT INTO dates VALUES ($1)"));
	REQUIRE_NO_FAIL(con.Query("EXECUTE s1 (DATE '1992-01-01')"));

	result = con.Query("SELECT * FROM dates");
	REQUIRE(CHECK_COLUMN(result, 0, {Value::DATE(1992, 1, 1)}));

	REQUIRE_NO_FAIL(con.Query("DELETE FROM dates"));

	auto prepared = con.Prepare("INSERT INTO dates VALUES ($1)");
	REQUIRE_NO_FAIL(prepared->Execute(Value::DATE(1992, 1, 3)));

	result = con.Query("SELECT * FROM dates");
	REQUIRE(CHECK_COLUMN(result, 0, {Value::DATE(1992, 1, 3)}));
}

TEST_CASE("PREPARE multiple statements", "[prepared]") {
	duckdb::unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);

	string query = "SELECT $1::INTEGER; SELECT $1::INTEGER;";
	// cannot prepare multiple statements like this
	auto prepared = con.Prepare(query);
	REQUIRE(prepared->HasError());
	// we can use ExtractStatements to execute the individual statements though
	auto statements = con.ExtractStatements(query);
	for (auto &statement : statements) {
		string stmt = query.substr(statement->stmt_location.offset, statement->stmt_location.length);
		prepared = con.Prepare(stmt);
		REQUIRE(!prepared->HasError());

		result = prepared->Execute(1);
		REQUIRE(CHECK_COLUMN(result, 0, {1}));
	}
}

static duckdb::unique_ptr<QueryResult> TestExecutePrepared(Connection &con, string query) {
	auto prepared = con.Prepare(query);
	REQUIRE(!prepared->HasError());
	return prepared->Execute();
}

TEST_CASE("Prepare all types of statements", "[prepared]") {
	duckdb::unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);
	auto &fs = db.GetFileSystem();

	string csv_path = TestCreatePath("prepared_files");
	if (fs.DirectoryExists(csv_path)) {
		fs.RemoveDirectory(csv_path);
	}

	// TRANSACTION
	REQUIRE_NO_FAIL(TestExecutePrepared(con, "BEGIN TRANSACTION"));
	// SELECT
	result = TestExecutePrepared(con, "SELECT 42");
	REQUIRE(CHECK_COLUMN(result, 0, {42}));
	// CREATE_SCHEMA
	REQUIRE_NO_FAIL(TestExecutePrepared(con, "CREATE SCHEMA test"));
	// CREATE_TABLE
	REQUIRE_NO_FAIL(TestExecutePrepared(con, "CREATE TABLE test.a(i INTEGER)"));
	// CREATE_TABLE
	REQUIRE_NO_FAIL(TestExecutePrepared(con, "CREATE TABLE b(i INTEGER)"));
	// CREATE_INDEX
	REQUIRE_NO_FAIL(TestExecutePrepared(con, "CREATE INDEX i_index ON test.a(i)"));
	// CREATE_VIEW
	REQUIRE_NO_FAIL(TestExecutePrepared(con, "CREATE VIEW v1 AS SELECT * FROM test.a WHERE i=2"));
	// CREATE_SEQUENCE
	REQUIRE_NO_FAIL(TestExecutePrepared(con, "CREATE SEQUENCE seq"));
	// PRAGMA
	REQUIRE_NO_FAIL(TestExecutePrepared(con, "PRAGMA table_info('b')"));
	// EXPLAIN
	REQUIRE_NO_FAIL(TestExecutePrepared(con, "EXPLAIN SELECT 42"));
	// COPY
	REQUIRE_NO_FAIL(TestExecutePrepared(con, "COPY test.a TO '" + csv_path + "'"));
	// INSERT
	REQUIRE_NO_FAIL(TestExecutePrepared(con, "INSERT INTO test.a VALUES (1), (2), (3)"));
	// UPDATE
	REQUIRE_NO_FAIL(TestExecutePrepared(con, "UPDATE test.a SET i=i+1"));
	// DELETE
	REQUIRE_NO_FAIL(TestExecutePrepared(con, "DELETE FROM test.a WHERE i<4"));
	// PREPARE
	REQUIRE_NO_FAIL(TestExecutePrepared(con, "PREPARE p1 AS SELECT * FROM test.a"));
	// EXECUTE
	result = TestExecutePrepared(con, "EXECUTE p1");
	REQUIRE(CHECK_COLUMN(result, 0, {4}));
	// DROP
	REQUIRE_NO_FAIL(TestExecutePrepared(con, "DROP SEQUENCE seq"));
	REQUIRE_NO_FAIL(TestExecutePrepared(con, "DROP VIEW v1"));
	REQUIRE_NO_FAIL(TestExecutePrepared(con, "DROP TABLE test.a CASCADE"));
	REQUIRE_NO_FAIL(TestExecutePrepared(con, "DROP SCHEMA test CASCADE"));

	// TRANSACTION
	REQUIRE_NO_FAIL(TestExecutePrepared(con, "COMMIT"));
}

TEST_CASE("Test ambiguous prepared statement parameter types", "[api]") {
	duckdb::unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);

	result = con.Query("SELECT ?", 42);
	REQUIRE(CHECK_COLUMN(result, 0, {42}));

	result = con.Query("SELECT ?", "hello");
	REQUIRE(CHECK_COLUMN(result, 0, {"hello"}));

	auto prep = con.Prepare("SELECT ?");
	result = prep->Execute(42);
	REQUIRE(CHECK_COLUMN(result, 0, {42}));

	result = prep->Execute("hello");
	REQUIRE(CHECK_COLUMN(result, 0, {"hello"}));
}

TEST_CASE("Test prepared statements with SET", "[api]") {
	duckdb::unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);

	// create a prepared statement and use it to query
	auto prepare = con.Prepare("SET default_null_order=$1");
	REQUIRE(!prepare->HasError());

	// too many parameters
	REQUIRE_FAIL(prepare->Execute("xxx", "yyy"));
	// too few parameters
	REQUIRE_FAIL(prepare->Execute());
	// unsupported setting
	REQUIRE_FAIL(prepare->Execute("unsupported_mode"));
	// this works
	REQUIRE_NO_FAIL(prepare->Execute("NULLS FIRST"));
}

TEST_CASE("Test prepared statements that require rebind", "[api]") {
	DuckDB db(nullptr);
	Connection con1(db);

	auto prepared = con1.Prepare("DROP TABLE IF EXISTS t1");

	Connection con2(db);
	REQUIRE_NO_FAIL(con2.Query("CREATE OR REPLACE TABLE t1 (c1 varchar)"));
	REQUIRE_NO_FAIL(prepared->Execute());
}

class TestExtensionState : public ClientContextState {
public:
	bool CanRequestRebind() override {
		return true;
	}
};

TEST_CASE("Test prepared statements with extension that can request a rebind", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE OR REPLACE TABLE t1 (c1 INTEGER)"));

	// https://github.com/duckdb/duckdb/pull/11096
	con.context->registered_state->Insert("test_extension", make_shared_ptr<TestExtensionState>());

	// SelectStatement
	REQUIRE_NO_FAIL(con.Prepare("SELECT ?")->Execute(42));

	// InsertStatement
	REQUIRE_NO_FAIL(con.Prepare("INSERT INTO t1 VALUES(?)")->Execute(42));

	// UpdateStatement
	REQUIRE_NO_FAIL(con.Prepare("UPDATE t1 SET c1 = ?")->Execute(43));

	// SetVariableStatement
	REQUIRE_NO_FAIL(con.Prepare("SET VARIABLE test_var = ?")->Execute(42));
}
