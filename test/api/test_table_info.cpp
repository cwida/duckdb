#include "catch.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

TEST_CASE("Test table info api", "[api]") {
	DuckDB db(nullptr);
	Connection con(db), con2(db);

	//! table is not found!
	auto info = con.TableInfo("test");
	REQUIRE(info.get() == nullptr);

	// after creating, the table can be found
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE test(i INTEGER)"));
	info = con.TableInfo("test");
	REQUIRE(info.get() != nullptr);
	REQUIRE(info->qualified_name.Name() == "test");
	REQUIRE(info->columns.size() == 1);
	REQUIRE(info->columns[0].Name() == "i");

	// table info is transaction sensitive
	REQUIRE_NO_FAIL(con.Query("BEGIN TRANSACTION"));
	// dropping the table in a transaction will result in the table being gone
	REQUIRE_NO_FAIL(con.Query("DROP TABLE test"));
	info = con.TableInfo("test");
	REQUIRE(info.get() == nullptr);

	// but not in a separate connection!
	info = con2.TableInfo("test");
	REQUIRE(info.get() != nullptr);

	// rolling back brings back the table info again
	REQUIRE_NO_FAIL(con.Query("ROLLBACK"));
	info = con.TableInfo("test");
	REQUIRE(info.get() != nullptr);
}

TEST_CASE("Test table info reports the resolved table location", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);

	// An unqualified lookup fills in the resolved catalog and schema.
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i INTEGER)"));
	auto info = con.TableInfo("t");
	REQUIRE(info);
	REQUIRE(info->qualified_name.Catalog() == "memory");
	REQUIRE(info->qualified_name.Schema() == "main");
	REQUIRE(info->qualified_name.Name() == "t");

	// The name is reported with its DDL time casing, not the lookup casing.
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE \"FooBar\"(i INTEGER)"));
	info = con.TableInfo("foobar");
	REQUIRE(info);
	REQUIRE(info->qualified_name.Name() == "FooBar");
}

TEST_CASE("Test table info on a view keeps the open transaction", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE u(k INTEGER)"));
	REQUIRE_NO_FAIL(con.Query("CREATE VIEW vw AS SELECT 1 AS k"));

	REQUIRE_NO_FAIL(con.Query("BEGIN TRANSACTION"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO u VALUES (1)"));
	REQUIRE(con.TableInfo("vw") == nullptr);
	REQUIRE_THROWS_AS(con.Table("vw"), CatalogException);
	REQUIRE_THROWS_WITH(con.Table("vw"), Catch::Matchers::Contains("Table vw does not exist!"));
	REQUIRE_THROWS_WITH(con.Table(Identifier::InvalidCatalog(), Identifier::DefaultSchema(), "vw"),
	                    Catch::Matchers::Contains("Table vw does not exist!"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO u VALUES (2)"));
	REQUIRE_NO_FAIL(con.Query("COMMIT"));

	auto result = con.Query("SELECT k FROM u ORDER BY k");
	REQUIRE(CHECK_COLUMN(result, 0, {1, 2}));
}

TEST_CASE("Test table info on a temporary view that hides a table", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE shadowed AS SELECT 'table' AS k"));
	REQUIRE_NO_FAIL(con.Query("CREATE TEMPORARY VIEW shadowed AS SELECT 'temporary view' AS k"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE u(k INTEGER)"));

	auto result = con.Query("SELECT k FROM shadowed");
	REQUIRE(CHECK_COLUMN(result, 0, {"temporary view"}));

	REQUIRE_NO_FAIL(con.Query("BEGIN TRANSACTION"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO u VALUES (1)"));
	REQUIRE(con.TableInfo("shadowed") == nullptr);
	REQUIRE_THROWS_AS(con.Table("shadowed"), CatalogException);
	auto info = con.TableInfo("memory", "main", "shadowed");
	REQUIRE(info);
	REQUIRE(info->qualified_name.Catalog() == "memory");
	REQUIRE(info->qualified_name.Name() == "shadowed");
	REQUIRE(info->columns.size() == 1);
	REQUIRE_NO_FAIL(con.Query("INSERT INTO u VALUES (2)"));
	REQUIRE_NO_FAIL(con.Query("COMMIT"));

	result = con.Query("SELECT k FROM u ORDER BY k");
	REQUIRE(CHECK_COLUMN(result, 0, {1, 2}));
}

TEST_CASE("Test connection table resolves a schema or catalog qualifier as SQL does", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto three_part = [&]() {
		return con.Table(Identifier::InvalidCatalog(), "x", "t");
	};
	auto two_part = [&]() {
		return con.Table("x", "t");
	};

	SECTION("schema and attached database share the name, the schema holds a view") {
		REQUIRE_NO_FAIL(con.Query("CREATE SCHEMA x"));
		REQUIRE_NO_FAIL(con.Query("CREATE VIEW memory.x.t AS SELECT 'schema view' AS k"));
		REQUIRE_NO_FAIL(con.Query("ATTACH ':memory:' AS x"));
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE x.main.t AS SELECT 'catalog table' AS k"));

		auto sql = con.Query("SELECT * FROM x.t");
		REQUIRE(sql->HasError());
		REQUIRE(sql->GetErrorType() == ExceptionType::BINDER);
		REQUIRE_THROWS_AS(three_part(), BinderException);
		REQUIRE_THROWS_WITH(three_part(), Catch::Matchers::Contains("Ambiguous reference to catalog or schema"));
		REQUIRE_THROWS_AS(two_part(), BinderException);
		REQUIRE_THROWS_WITH(two_part(), Catch::Matchers::Contains("Ambiguous reference to catalog or schema"));
	}
	SECTION("schema and attached database share the name, the schema holds a table") {
		REQUIRE_NO_FAIL(con.Query("CREATE SCHEMA x"));
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE memory.x.t AS SELECT 'schema table' AS k"));
		REQUIRE_NO_FAIL(con.Query("ATTACH ':memory:' AS x"));
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE x.main.t AS SELECT 'catalog table' AS k"));

		auto sql = con.Query("SELECT * FROM x.t");
		REQUIRE(sql->HasError());
		REQUIRE(sql->GetErrorType() == ExceptionType::BINDER);
		REQUIRE_THROWS_AS(three_part(), BinderException);
		REQUIRE_THROWS_WITH(three_part(), Catch::Matchers::Contains("Ambiguous reference to catalog or schema"));
		REQUIRE_THROWS_AS(two_part(), BinderException);
		REQUIRE_THROWS_WITH(two_part(), Catch::Matchers::Contains("Ambiguous reference to catalog or schema"));
	}
	SECTION("an ambiguous qualifier inside a transaction aborts it as SQL does") {
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE u(k INTEGER)"));
		REQUIRE_NO_FAIL(con.Query("CREATE SCHEMA x"));
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE memory.x.t AS SELECT 'schema table' AS k"));
		REQUIRE_NO_FAIL(con.Query("ATTACH ':memory:' AS x"));
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE x.main.t AS SELECT 'catalog table' AS k"));

		for (bool use_two_part : {false, true}) {
			REQUIRE_NO_FAIL(con.Query("BEGIN TRANSACTION"));
			REQUIRE_NO_FAIL(con.Query("INSERT INTO u VALUES (1)"));
			REQUIRE_THROWS_AS(use_two_part ? two_part() : three_part(), BinderException);
			auto next = con.Query("INSERT INTO u VALUES (2)");
			REQUIRE(next->HasError());
			REQUIRE(next->GetErrorType() == ExceptionType::TRANSACTION);
			REQUIRE_NO_FAIL(con.Query("ROLLBACK"));
		}

		REQUIRE_NO_FAIL(con.Query("BEGIN TRANSACTION"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO u VALUES (1)"));
		auto sql = con.Query("SELECT * FROM x.t");
		REQUIRE(sql->HasError());
		REQUIRE(sql->GetErrorType() == ExceptionType::BINDER);
		auto next = con.Query("INSERT INTO u VALUES (2)");
		REQUIRE(next->HasError());
		REQUIRE(next->GetErrorType() == ExceptionType::TRANSACTION);
		REQUIRE_NO_FAIL(con.Query("ROLLBACK"));
	}
	SECTION("a catalog qualifier resolves through the search path as SQL does") {
		REQUIRE_NO_FAIL(con.Query("ATTACH ':memory:' AS x"));
		REQUIRE_NO_FAIL(con.Query("CREATE SCHEMA x.s"));
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE x.main.t AS SELECT 1 AS k"));
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE x.s.t AS SELECT 2 AS k"));
		REQUIRE_NO_FAIL(con.Query("SET search_path = 'x.s'"));

		auto sql = con.Query("SELECT * FROM x.t");
		REQUIRE(CHECK_COLUMN(sql, 0, {2}));
		auto result = three_part()->Execute();
		REQUIRE(CHECK_COLUMN(result, 0, {2}));
		result = two_part()->Execute();
		REQUIRE(CHECK_COLUMN(result, 0, {2}));
	}
	SECTION("only an attached database has the name") {
		REQUIRE_NO_FAIL(con.Query("ATTACH ':memory:' AS x"));
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE x.main.t AS SELECT 'catalog table' AS k"));

		auto sql = con.Query("SELECT * FROM x.t");
		REQUIRE(CHECK_COLUMN(sql, 0, {"catalog table"}));
		auto result = three_part()->Execute();
		REQUIRE(CHECK_COLUMN(result, 0, {"catalog table"}));
		result = two_part()->Execute();
		REQUIRE(CHECK_COLUMN(result, 0, {"catalog table"}));
	}
	SECTION("only a schema has the name") {
		REQUIRE_NO_FAIL(con.Query("CREATE SCHEMA x"));
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE x.t AS SELECT 'schema table' AS k"));

		auto sql = con.Query("SELECT * FROM x.t");
		REQUIRE(CHECK_COLUMN(sql, 0, {"schema table"}));
		auto result = three_part()->Execute();
		REQUIRE(CHECK_COLUMN(result, 0, {"schema table"}));
		result = two_part()->Execute();
		REQUIRE(CHECK_COLUMN(result, 0, {"schema table"}));
	}
}

TEST_CASE("Test connection table reports a missing table", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE SCHEMA s"));

	REQUIRE_THROWS_AS(con.Table("missing"), CatalogException);
	REQUIRE_THROWS_WITH(con.Table("missing"), Catch::Matchers::Contains("Table missing does not exist!"));
	REQUIRE_THROWS_WITH(con.Table("s", "missing"), Catch::Matchers::Contains("Table s.missing does not exist!"));
	REQUIRE_THROWS_WITH(con.Table(Identifier::InvalidCatalog(), "s", "missing"),
	                    Catch::Matchers::Contains("Table s.missing does not exist!"));
}
