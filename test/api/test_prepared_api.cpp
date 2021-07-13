#include "catch.hpp"
#include "test_helpers.hpp"

using namespace duckdb;
using namespace std;

TEST_CASE("Test prepared statements API", "[api]") {
	unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);

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
	REQUIRE(prepare->n_param == 1);
}

TEST_CASE("Test type resolution of function with parameter expressions", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	unique_ptr<QueryResult> result;
	con.EnableQueryVerification();

	// can deduce type of prepared parameter here
	auto prepared = con.Prepare("select 1 + $1");
	REQUIRE(prepared->error.empty());

	result = prepared->Execute(1);
	REQUIRE(CHECK_COLUMN(result, 0, {2}));
}

TEST_CASE("Test prepared statements and dependencies", "[api]") {
	unique_ptr<QueryResult> result;
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

TEST_CASE("Dropping connection with prepared statement resets dependencies", "[api]") {
	unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	auto con = make_unique<Connection>(db);
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
	unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	auto con = make_unique<Connection>(db);
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

	// after the table is altered, the return types change, so we fail executing the statement
	REQUIRE_FAIL(prepared->Execute(12));
	REQUIRE_FAIL(prepared->Execute(12));
}

TEST_CASE("Test destructors of prepared statements", "[api]") {
	unique_ptr<DuckDB> db;
	unique_ptr<Connection> con;
	unique_ptr<PreparedStatement> prepare;
	unique_ptr<QueryResult> result;

	// test destruction of connection
	db = make_unique<DuckDB>(nullptr);
	con = make_unique<Connection>(*db);
	// create a prepared statement
	prepare = con->Prepare("SELECT $1::INTEGER+$2::INTEGER");
	// we can execute it
	result = prepare->Execute(3, 5);
	REQUIRE(CHECK_COLUMN(result, 0, {8}));
	// now destroy the connection
	con.reset();
	// we can still use the prepared statement: the connection is alive until the prepared statement is dropped
	REQUIRE_NO_FAIL(prepare->Execute(3, 5));
	// destroying the prepared statement is fine
	prepare.reset();

	// test destruction of db
	// create a connection and prepared statement again
	con = make_unique<Connection>(*db);
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
	REQUIRE(prepare->success);
}

TEST_CASE("Test incorrect usage of prepared statements API", "[api]") {
	unique_ptr<QueryResult> result;
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
	REQUIRE(!prepare->success);
	REQUIRE_THROWS(prepare->Execute(12));

	// cannot prepare multiple statements at once
	prepare = con.Prepare("SELECT COUNT(*) FROM a WHERE i=$1; SELECT 42+$2;");
	REQUIRE(!prepare->success);
	REQUIRE_THROWS(prepare->Execute(12));

	// also not in the Query syntax
	REQUIRE_FAIL(con.Query("SELECT COUNT(*) FROM a WHERE i=$1; SELECT 42+$2", 11));
}

TEST_CASE("Test multiple prepared statements", "[api]") {
	unique_ptr<QueryResult> result;
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
	unique_ptr<QueryResult> result;
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
	unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);

	auto p0 = con.Prepare("SELECT 42");
	REQUIRE(p0->success);
	REQUIRE(p0->n_param == 0);

	auto p1 = con.Prepare("SELECT $1::int");
	REQUIRE(p1->success);
	REQUIRE(p1->n_param == 1);

	p1 = con.Prepare("SELECT ?::int");
	REQUIRE(p1->success);
	REQUIRE(p1->n_param == 1);

	auto p2 = con.Prepare("SELECT $1::int");
	REQUIRE(p2->success);
	REQUIRE(p2->n_param == 1);

	auto p3 = con.Prepare("SELECT ?::int, ?::string");
	REQUIRE(p3->success);
	REQUIRE(p3->n_param == 2);

	auto p4 = con.Prepare("SELECT $1::int, $2::string");
	REQUIRE(p4->success);
	REQUIRE(p4->n_param == 2);

	auto p5 = con.Prepare("SELECT $2::int, $2::string");
	REQUIRE(!p5->success);
}

TEST_CASE("Test ANALYZE", "[api]") {
	unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);
	con.EnableQueryVerification();

	// ANALYZE runs without errors, note that ANALYZE is actually just ignored
	REQUIRE_NO_FAIL(con.Query("ANALYZE"));
	REQUIRE_NO_FAIL(con.Query("VACUUM"));

	auto prep = con.Prepare("ANALYZE");
	REQUIRE(prep->success);
	auto res = prep->Execute();
	REQUIRE(res->success);

	prep = con.Prepare("VACUUM");
	REQUIRE(prep->success);
	res = prep->Execute();
	REQUIRE(res->success);
}

TEST_CASE("Test DECIMAL with PreparedStatement", "[api]") {
	unique_ptr<QueryResult> result;
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
	unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);

	// Creating a blob buffer with almost ALL ASCII chars
	uint8_t num_chars = 256 - 5; // skipping: '\0', '\n', '\15', ',', '\32'
	unique_ptr<char[]> blob_chars(new char[num_chars]);
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
	Value blob_val = Value::BLOB((const_data_ptr_t)blob_chars.get(), num_chars);
	unique_ptr<PreparedStatement> ps = con.Prepare("INSERT INTO blobs VALUES (?::BYTEA)");
	ps->Execute(blob_val);
	REQUIRE(ps->success);
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
	unique_ptr<QueryResult> result;
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
	unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);

	string query = "SELECT $1::INTEGER; SELECT $1::INTEGER;";
	// cannot prepare multiple statements like this
	auto prepared = con.Prepare(query);
	REQUIRE(!prepared->success);
	// we can use ExtractStatements to execute the individual statements though
	auto statements = con.ExtractStatements(query);
	for (auto &statement : statements) {
		string stmt = query.substr(statement->stmt_location, statement->stmt_length);
		prepared = con.Prepare(stmt);
		REQUIRE(prepared->success);

		result = prepared->Execute(1);
		REQUIRE(CHECK_COLUMN(result, 0, {1}));
	}
}

static unique_ptr<QueryResult> TestExecutePrepared(Connection &con, string query) {
	auto prepared = con.Prepare(query);
	REQUIRE(prepared->success);
	return prepared->Execute();
}

TEST_CASE("Prepare all types of statements", "[prepared]") {
	unique_ptr<QueryResult> result;
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
