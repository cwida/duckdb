#include "catch.hpp"
#include "duckdb_cpp.hpp"
#include "duckdb_v2.h"
#include "test_cpp_api.hpp"

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Stable C++ API tests: TableDescription. Resolution through the search
// path, the resolved name, the column getters, and the error path.
// ---------------------------------------------------------------------------

namespace {

using namespace duckdb::cxx;

} // namespace

TEST_CASE("Stable C++API: table description resolves and reports columns", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	conn.Execute("CREATE SCHEMA s").Drain();
	conn.Execute("CREATE TABLE s.\"Facts\"(i INTEGER, j VARCHAR DEFAULT 'x', k INTEGER GENERATED ALWAYS AS (i + 1))")
	    .Drain();

	// A partial name resolves to its full location, with the casing the table was created with.
	auto desc = conn.DescribeTable(QualifiedName::Create({"s", "facts"}));
	REQUIRE(desc.GetQualifiedName().Render() == "memory.s.Facts");
	REQUIRE_FALSE(desc.IsReadOnly());

	// Every column in declared order, the generated one included.
	REQUIRE(desc.GetColumnCount() == 3);
	std::vector<ColumnDescription> columns;
	for (idx_t i = 0; i < desc.GetColumnCount(); i++) {
		columns.push_back(desc.GetColumn(i));
	}
	REQUIRE(columns[0].GetName() == "i");
	REQUIRE(columns[1].GetName() == "j");
	REQUIRE(columns[2].GetName() == "k");
	REQUIRE(columns[0].GetType().GetTypeId() == LogicalTypeId::INTEGER);
	REQUIRE(columns[1].GetType().GetTypeId() == LogicalTypeId::VARCHAR);
	REQUIRE(columns[2].GetType().GetTypeId() == LogicalTypeId::INTEGER);
	REQUIRE_FALSE(columns[0].HasDefault());
	REQUIRE(columns[1].HasDefault());
	REQUIRE_FALSE(columns[2].HasDefault());
	REQUIRE_FALSE(columns[0].HasGenerated());
	REQUIRE_FALSE(columns[1].HasGenerated());
	REQUIRE(columns[2].HasGenerated());

	// The description is a snapshot: it outlives the table.
	conn.Execute("DROP TABLE s.\"Facts\"").Drain();
	REQUIRE(desc.GetColumnCount() == 3);
	REQUIRE(desc.GetColumn(1).GetType().ToText() == "VARCHAR");

	// An out-of-range column index is rejected.
	REQUIRE_THROWS_MATCHES(desc.GetColumn(3), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_OUT_OF_RANGE));
}

TEST_CASE("Stable C++API: table description rejects missing tables and views", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	conn.Execute("CREATE VIEW v AS SELECT 42 AS i").Drain();

	REQUIRE_THROWS_MATCHES(conn.DescribeTable(QualifiedName::Create({"no_such_table"})), Exception,
	                       HasErrorCode(DUCKDB_V2_ERROR_DATABASE_CATALOG));
	REQUIRE_THROWS_MATCHES(conn.DescribeTable(QualifiedName::Parse("v")), Exception,
	                       HasErrorCode(DUCKDB_V2_ERROR_DATABASE_CATALOG));
}
