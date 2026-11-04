#include "catch.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

static void CreateSimpleTable(Connection &con) {
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE a (i TINYINT)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO a VALUES (11), (12), (13)"));
}

static void ModifySimpleTable(Connection &con) {
	REQUIRE_NO_FAIL(con.Query("INSERT INTO a VALUES (14)"));
	REQUIRE_NO_FAIL(con.Query("DELETE FROM a where i=12"));
}

static void CheckSimpleQuery(Connection &con) {
	auto statements = con.ExtractStatements("SELECT COUNT(*) FROM a WHERE i=12");
	REQUIRE(statements.size() == 1);
	duckdb::vector<duckdb::Value> values = {Value(12)};
	auto pending_result = con.Submit("SELECT COUNT(*) FROM a WHERE i=?", values);

	if (pending_result->HasError()) {
		printf("%s\n", pending_result->GetError().c_str());
	}

	REQUIRE(!pending_result->HasError());

	pending_result->Complete();
	REQUIRE(CHECK_COLUMN(pending_result, 0, {1}));
}

static void CheckCatalogErrorQuery(Connection &con) {
	duckdb::vector<Value> values = {Value(12)};
	auto pending_result = con.Submit("SELECT COUNT(*) FROM b WHERE i=?", values);
	REQUIRE((pending_result->HasError() && pending_result->GetErrorType() == ExceptionType::CATALOG));
}

static void CheckConversionErrorQuery(Connection &con) {
	// Check query with invalid prepared value
	duckdb::vector<Value> values = {Value("fawakaaniffoo")};
	auto pending_result = con.Submit("SELECT COUNT(*) FROM a WHERE i=?", values);
	REQUIRE(!pending_result->HasError());
	pending_result->Complete();
	REQUIRE((pending_result->HasError() && pending_result->GetErrorType() == ExceptionType::CONVERSION));
}

static void CheckSimpleQueryAfterModification(Connection &con) {
	duckdb::vector<Value> values = {Value(14)};
	auto pending_result = con.Submit("SELECT COUNT(*) FROM a WHERE i=?", values);
	REQUIRE(!pending_result->HasError());
	pending_result->Complete();
	REQUIRE(CHECK_COLUMN(pending_result, 0, {1}));
}

TEST_CASE("Pending Query with Parameters", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);

	CreateSimpleTable(con);
	CheckSimpleQuery(con);
	CheckSimpleQuery(con);
}

TEST_CASE("Pending Query with Parameters Catalog Error", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);

	CreateSimpleTable(con);

	CheckCatalogErrorQuery(con);

	// Verify things are still sane
	CheckSimpleQuery(con);
}

TEST_CASE("Pending Query with Parameters Type Conversion Error", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);

	CreateSimpleTable(con);

	CheckConversionErrorQuery(con);

	// Verify things are still sane
	CheckSimpleQuery(con);
}

TEST_CASE("Pending Query with Parameters with transactions", "[api]") {
	DuckDB db(nullptr);
	Connection con1(db);
	Connection con2(db);
	duckdb::vector<Value> empty_values = {};

	CreateSimpleTable(con1);

	// CheckConversionErrorQuery(con1);

	// Begin a transaction in the PrepareAndExecute
	auto pending_result1 = con1.Submit("BEGIN TRANSACTION", empty_values);
	if (pending_result1->HasError()) {
		printf("%s\n", pending_result1->GetError().c_str());
	}
	REQUIRE(!pending_result1->HasError());

	pending_result1->Complete();
	REQUIRE(!pending_result1->HasError());
	CheckSimpleQuery(con1);

	// Modify table on other connection, leaving transaction open
	con2.BeginTransaction();
	ModifySimpleTable(con2);
	CheckSimpleQueryAfterModification(con2);

	// con1 sees nothing: both transactions are open
	CheckSimpleQuery(con1);

	con2.Commit();

	// con1 still sees nothing: its transaction was started before con2's
	CheckSimpleQuery(con1);

	// con 1 commits
	auto pending_result2 = con1.Submit("COMMIT", empty_values);
	pending_result2->Complete();
	REQUIRE(!pending_result2->HasError());

	// now con1 should see changes from con2
	CheckSimpleQueryAfterModification(con1);
	CheckSimpleQueryAfterModification(con2);
}
