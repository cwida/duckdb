#include "catch.hpp"

#include "arrow/arrow_test_helper.hpp"
#include "duckdb/common/arrow/arrow_converter.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/arrow/arrow_type_extension.hpp"
#include "duckdb/common/arrow/schema_metadata.hpp"
#include "duckdb/function/table/arrow/arrow_duck_schema.hpp"
#include "duckdb/function/table/arrow/arrow_type_info.hpp"
#include "duckdb/common/identifier.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"

#include "duckdb/common/atomic.hpp"

using namespace duckdb;

namespace {

class RepeatableArrowViewFactory : public ArrowScanFactory {
public:
	explicit RepeatableArrowViewFactory(Connection &source_p) : source(source_p) {
	}

	void GetSchema(ArrowSchema &schema) override {
		auto properties = source.context->GetClientProperties();
		ArrowConverter::ToArrowSchema(&schema, {LogicalType::INTEGER}, {"i"}, properties);
	}

	unique_ptr<ArrowArrayStreamWrapper> ProduceStream(ArrowStreamParameters &parameters) override {
		// Each scan gets its own result, including scans introduced by CTE inlining.
		auto result = source.Query("SELECT 42::INTEGER AS i FROM range(2)");
		REQUIRE_NO_FAIL(*result);
		auto factory =
		    make_uniq<ArrowTestFactory>(result->GetTypes(), IdentifiersToStrings(result->GetNames()), std::move(result),
		                                source.context->GetClientProperties(), *source.context);
		auto stream = factory->ProduceStream(parameters);
		factories.push_back(std::move(factory));
		return stream;
	}

private:
	Connection &source;
	vector<unique_ptr<ArrowTestFactory>> factories;
};

} // namespace

TEST_CASE("Arrow scan view supports repeated CTE references", "[arrow]") {
	DuckDB db;
	Connection source(db);
	Connection con(db);
	auto factory = make_shared_ptr<RepeatableArrowViewFactory>(source);
	con.TableFunction("arrow_scan", {}, {}, factory)->CreateView("v", true, true);

	auto result = con.Query("WITH t AS (SELECT * FROM v) SELECT i FROM t UNION ALL SELECT i FROM t LIMIT 3");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->RowCount() == 3);
	auto rows = result->Collection().GetRows();
	for (idx_t row = 0; row < rows.size(); row++) {
		REQUIRE(rows.GetValue(0, row) == Value::INTEGER(42));
	}
}

TEST_CASE("Arrow scan view supports serializer verification", "[arrow]") {
	DuckDB db;
	Connection source(db);
	Connection con(db);
	auto factory = make_shared_ptr<RepeatableArrowViewFactory>(source);
	con.TableFunction("arrow_scan", {}, {}, factory)->CreateView("v", true, true);
	REQUIRE_NO_FAIL(con.Query("SET debug_verify_serializer = true"));

	string query;
	idx_t expected_count;
	SECTION("Direct scan") {
		query = "SELECT i FROM v";
		expected_count = 2;
	}
	SECTION("Repeated CTE references") {
		query = "WITH t AS (SELECT * FROM v) SELECT i FROM t UNION ALL SELECT i FROM t LIMIT 3";
		expected_count = 3;
	}
	auto result = con.Query(query);
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->RowCount() == expected_count);
	auto rows = result->Collection().GetRows();
	for (idx_t row = 0; row < rows.size(); row++) {
		REQUIRE(rows.GetValue(0, row) == Value::INTEGER(42));
	}
}

static void TestArrowRoundtrip(const string &query, bool export_large_buffer = false,
                               bool lossless_conversion = false) {
	DuckDB db;
	Connection con(db);
	if (export_large_buffer) {
		auto res = con.Query("SET arrow_large_buffer_size=True");
		REQUIRE(!res->HasError());
	}
	if (lossless_conversion) {
		auto res = con.Query("SET arrow_lossless_conversion = true");
		REQUIRE(!res->HasError());
	}
	REQUIRE(ArrowTestHelper::RunArrowComparison(con, query, true));
	REQUIRE(ArrowTestHelper::RunArrowComparison(con, query, false));
}

static void TestArrowRoundtripStringView(const string &query) {
	DuckDB db;
	Connection con(db);
	auto res = con.Query("SET produce_arrow_string_view=True");
	REQUIRE(!res->HasError());
	REQUIRE(ArrowTestHelper::RunArrowComparison(con, query, false));
}

static void TestParquetRoundtrip(const string &path) {
	DBConfig config;
	// This needs to be set since this test will be triggered when testing autoloading
	config.SetOptionByName("allow_unsigned_extensions", true);

	DuckDB db(nullptr, &config);
	Connection con(db);

	// run the query
	auto query = "SELECT * FROM parquet_scan('" + path + "')";
	REQUIRE(ArrowTestHelper::RunArrowComparison(con, query, true));
	REQUIRE(ArrowTestHelper::RunArrowComparison(con, query));
}

TEST_CASE("Test Export Large", "[arrow]") {
	// Test with Regular Buffer Size
	TestArrowRoundtrip("SELECT 'bla' FROM range(10000)");

	TestArrowRoundtrip("SELECT 'bla'::BLOB FROM range(10000)");

	TestArrowRoundtrip("SELECT '3d038406-6275-4aae-bec1-1235ccdeaade'::UUID FROM range(10000) tbl(i)", false, true);

	// Test with Large Buffer Size
	TestArrowRoundtrip("SELECT 'bla' FROM range(10000)", true);

	TestArrowRoundtrip("SELECT 'bla'::BLOB FROM range(10000)", true);

	TestArrowRoundtrip("SELECT '3d038406-6275-4aae-bec1-1235ccdeaade'::UUID FROM range(10000) tbl(i)", true, true);
}

TEST_CASE("Test arrow roundtrip", "[arrow]") {
	TestArrowRoundtrip("SELECT * FROM range(10000) tbl(i) UNION ALL SELECT NULL");
	TestArrowRoundtrip("SELECT m from (select MAP(list_value(1), list_value(2)) from range(5) tbl(i)) tbl(m)");
	TestArrowRoundtrip("SELECT * FROM range(10) tbl(i)");
	TestArrowRoundtrip("SELECT case when i%2=0 then null else i end i FROM range(10) tbl(i)");
	TestArrowRoundtrip("SELECT case when i%2=0 then true else false end b FROM range(10) tbl(i)");
	TestArrowRoundtrip("SELECT case when i%2=0 then i%4=0 else null end b FROM range(10) tbl(i)");
	TestArrowRoundtrip("SELECT 'thisisalongstring'||i::varchar str FROM range(10) tbl(i)");
	TestArrowRoundtrip(
	    "SELECT case when i%2=0 then null else 'thisisalongstring'||i::varchar end str FROM range(10) tbl(i)");
	TestArrowRoundtrip("SELECT {'i': i, 'b': 10-i} str FROM range(10) tbl(i)");
	TestArrowRoundtrip("SELECT case when i%2=0 then {'i': case when i%4=0 then null else i end, 'b': 10-i} else null "
	                   "end str FROM range(10) tbl(i)");
	TestArrowRoundtrip("SELECT [i, i+1, i+2] FROM range(10) tbl(i)");
	TestArrowRoundtrip(
	    "SELECT MAP(LIST_VALUE({'i':1,'j':2},{'i':3,'j':4}),LIST_VALUE({'i':1,'j':2},{'i':3,'j':4})) as a");
	TestArrowRoundtrip(
	    "SELECT MAP(LIST_VALUE({'i':i,'j':i+2},{'i':3,'j':NULL}),LIST_VALUE({'i':i+10,'j':2},{'i':i+4,'j':4})) as a "
	    "FROM range(10) tbl(i)");
	TestArrowRoundtrip("SELECT MAP(['hello', 'world'||i::VARCHAR],[i + 1, NULL]) as a FROM range(10) tbl(i)");
	TestArrowRoundtrip("SELECT (1.5 + i)::DECIMAL(4,2) dec4, (1.5 + i)::DECIMAL(9,3) dec9, (1.5 + i)::DECIMAL(18,3) "
	                   "dec18, (1.5 + i)::DECIMAL(38,3) dec38 FROM range(10) tbl(i)");
	TestArrowRoundtrip(
	    "SELECT case when i%2=0 then null else INTERVAL (i) seconds end AS interval FROM range(10) tbl(i)");
#if STANDARD_VECTOR_SIZE < 64
	// FIXME: there seems to be a bug in the enum arrow reader in this test when run with vsize=2
	return;
#endif
	TestArrowRoundtrip("SELECT * FROM test_all_types()", false, true);
}

TEST_CASE("Test arrow NULL value roundtrip", "[arrow]") {
	// null types
	TestArrowRoundtrip("SELECT NULL");
	TestArrowRoundtrip("SELECT [NULL, NULL]");
	TestArrowRoundtrip("SELECT {'x': NULL, 'y': NULL}");
	TestArrowRoundtrip("SELECT [{'x': NULL, 'y': NULL}, {'x': NULL, 'y': NULL}]");
}

TEST_CASE("Test Arrow fixed-size binary format parsing", "[arrow]") {
	// Verify that GetTypeFromFormat correctly parses the size from "w:NN" format strings.
	// Regression test for duckdb/duckdb-wasm#2199: format.find(':') would match colons
	// in extension metadata (e.g. CRS strings like "ogc:crs84"), causing std::stoi to crash.
	{
		string format = "w:16";
		auto type = ArrowType::GetTypeFromFormat(format);
		REQUIRE(type);
		REQUIRE(type->GetDuckType() == LogicalType::BLOB);
	}
	{
		string format = "w:1";
		auto type = ArrowType::GetTypeFromFormat(format);
		REQUIRE(type);
		REQUIRE(type->GetDuckType() == LogicalType::BLOB);
	}
	{
		string format = "w:128";
		auto type = ArrowType::GetTypeFromFormat(format);
		REQUIRE(type);
		REQUIRE(type->GetDuckType() == LogicalType::BLOB);
	}
}

static void SetupUnionTable(Connection &con, idx_t num_rows, bool with_nulls = false) {
	REQUIRE_NO_FAIL(con.Query("CREATE OR REPLACE TABLE union_tbl(u UNION(i INT, s VARCHAR))"));
	// Insert alternating int and string union members via separate statements
	REQUIRE_NO_FAIL(con.Query("INSERT INTO union_tbl SELECT union_value(i := i::INT) FROM range(" +
	                          to_string(num_rows) + ") tbl(i) WHERE i % 2 = 0"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO union_tbl SELECT union_value(s := 'val' || i::VARCHAR) FROM range(" +
	                          to_string(num_rows) + ") tbl(i) WHERE i % 2 = 1"));
	if (with_nulls) {
		REQUIRE_NO_FAIL(con.Query("INSERT INTO union_tbl SELECT NULL::UNION(i INT, s VARCHAR) FROM range(" +
		                          to_string(num_rows / 5) + ") tbl(i)"));
	}
}

TEST_CASE("Test Arrow UNION type roundtrip", "[arrow]") {
	DuckDB db;
	Connection con(db);

	// Small union with mixed tags
	SetupUnionTable(con, 10);
	REQUIRE(ArrowTestHelper::RunArrowComparison(con, "SELECT * FROM union_tbl", false));
	REQUIRE(ArrowTestHelper::RunArrowComparison(con, "SELECT * FROM union_tbl", true));

	// Union with NULLs
	SetupUnionTable(con, 10, true);
	REQUIRE(ArrowTestHelper::RunArrowComparison(con, "SELECT * FROM union_tbl", false));
	REQUIRE(ArrowTestHelper::RunArrowComparison(con, "SELECT * FROM union_tbl", true));

	// Single-member union
	REQUIRE_NO_FAIL(con.Query("CREATE OR REPLACE TABLE union_tbl(u UNION(i INT))"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO union_tbl SELECT union_value(i := i::INT) FROM range(10) tbl(i)"));
	REQUIRE(ArrowTestHelper::RunArrowComparison(con, "SELECT * FROM union_tbl", false));
	REQUIRE(ArrowTestHelper::RunArrowComparison(con, "SELECT * FROM union_tbl", true));

	// All-NULL union column
	REQUIRE_NO_FAIL(con.Query("CREATE OR REPLACE TABLE union_tbl(u UNION(i INT, s VARCHAR))"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO union_tbl SELECT NULL::UNION(i INT, s VARCHAR) FROM range(10) tbl(i)"));
	REQUIRE(ArrowTestHelper::RunArrowComparison(con, "SELECT * FROM union_tbl", false));
	REQUIRE(ArrowTestHelper::RunArrowComparison(con, "SELECT * FROM union_tbl", true));

	// Single-tag-only: all rows use the same member, other member completely empty
	REQUIRE_NO_FAIL(con.Query("CREATE OR REPLACE TABLE union_tbl(u UNION(i INT, s VARCHAR))"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO union_tbl SELECT union_value(i := i::INT) FROM range(10000) tbl(i)"));
	REQUIRE(ArrowTestHelper::RunArrowComparison(con, "SELECT * FROM union_tbl", false));
	REQUIRE(ArrowTestHelper::RunArrowComparison(con, "SELECT * FROM union_tbl", true));

	// Union alongside other columns
	REQUIRE_NO_FAIL(con.Query("CREATE OR REPLACE TABLE union_tbl(id INT, u UNION(i INT, s VARCHAR), label VARCHAR)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO union_tbl SELECT i, union_value(i := i::INT), 'row' || i::VARCHAR "
	                          "FROM range(10000) tbl(i) WHERE i % 2 = 0"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO union_tbl SELECT i, union_value(s := 'val' || i::VARCHAR), 'row' || "
	                          "i::VARCHAR FROM range(10000) tbl(i) WHERE i % 2 = 1"));
	REQUIRE(ArrowTestHelper::RunArrowComparison(con, "SELECT * FROM union_tbl", false));
	REQUIRE(ArrowTestHelper::RunArrowComparison(con, "SELECT * FROM union_tbl", true));

	// Nested struct as union member
	REQUIRE_NO_FAIL(con.Query("CREATE OR REPLACE TABLE union_tbl(u UNION(a INT, b STRUCT(x INT, y VARCHAR)))"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO union_tbl SELECT union_value(a := i::INT) FROM range(10000) tbl(i) "
	                          "WHERE i % 2 = 0"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO union_tbl SELECT union_value(b := ROW(i, 'v' || i::VARCHAR)) "
	                          "FROM range(10000) tbl(i) WHERE i % 2 = 1"));
	REQUIRE(ArrowTestHelper::RunArrowComparison(con, "SELECT * FROM union_tbl", false));
	REQUIRE(ArrowTestHelper::RunArrowComparison(con, "SELECT * FROM union_tbl", true));

	// Union inside a struct
	REQUIRE_NO_FAIL(con.Query("CREATE OR REPLACE TABLE union_tbl(s STRUCT(tag INT, u UNION(i INT, v VARCHAR)))"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO union_tbl SELECT ROW(i, union_value(i := i::INT)) "
	                          "FROM range(10000) tbl(i) WHERE i % 2 = 0"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO union_tbl SELECT ROW(i, union_value(v := i::VARCHAR)) "
	                          "FROM range(10000) tbl(i) WHERE i % 2 = 1"));
	REQUIRE(ArrowTestHelper::RunArrowComparison(con, "SELECT * FROM union_tbl", false));
	REQUIRE(ArrowTestHelper::RunArrowComparison(con, "SELECT * FROM union_tbl", true));

	// Large batch - exercises chunk_offset across multiple scan passes (> STANDARD_VECTOR_SIZE)
	SetupUnionTable(con, 10000);
	REQUIRE(ArrowTestHelper::RunArrowComparison(con, "SELECT * FROM union_tbl", false));
	REQUIRE(ArrowTestHelper::RunArrowComparison(con, "SELECT * FROM union_tbl", true));

	// Large batch with NULLs
	SetupUnionTable(con, 10000, true);
	REQUIRE(ArrowTestHelper::RunArrowComparison(con, "SELECT * FROM union_tbl", false));
	REQUIRE(ArrowTestHelper::RunArrowComparison(con, "SELECT * FROM union_tbl", true));

	// Three-member union, large batch
	REQUIRE_NO_FAIL(con.Query("CREATE OR REPLACE TABLE union_tbl3(u UNION(a INT, b FLOAT, c VARCHAR))"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO union_tbl3 SELECT union_value(a := i::INT) FROM range(10000) tbl(i) "
	                          "WHERE i % 3 = 0"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO union_tbl3 SELECT union_value(b := (i * 1.5)::FLOAT) FROM range(10000) "
	                          "tbl(i) WHERE i % 3 = 1"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO union_tbl3 SELECT union_value(c := 'str' || i::VARCHAR) FROM "
	                          "range(10000) tbl(i) WHERE i % 3 = 2"));
	REQUIRE(ArrowTestHelper::RunArrowComparison(con, "SELECT * FROM union_tbl3", false));
	REQUIRE(ArrowTestHelper::RunArrowComparison(con, "SELECT * FROM union_tbl3", true));
}

// Regression for duckdb#22444: under arrow_lossless_conversion a BOOLEAN child of a
// nested type is declared as arrow.bool8 (byte-packed) in the schema, so its data must
// be byte-packed too. Nested BOOLEAN children used to be written bit-packed, so every
// row past the first read back wrong.
TEST_CASE("Test Arrow nested BOOLEAN roundtrip", "[arrow]") {
	// BOOLEAN inside each container type.
	TestArrowRoundtrip("SELECT {'b': (i % 2 = 0)}::STRUCT(b BOOLEAN) AS s FROM range(64) tbl(i)", false, true);
	TestArrowRoundtrip("SELECT [(i % 2 = 0), (i % 3 = 0)]::BOOLEAN[] AS l FROM range(64) tbl(i)", false, true);
	TestArrowRoundtrip("SELECT [(i % 2 = 0), true, (i % 3 = 0)]::BOOLEAN[3] AS a FROM range(64) tbl(i)", false, true);
	TestArrowRoundtrip("SELECT MAP {'x': (i % 2 = 0), 'y': (i % 3 = 0)} AS m FROM range(64) tbl(i)", false, true);
	TestArrowRoundtrip("SELECT union_value(b := (i % 2 = 0))::UNION(i INT, b BOOLEAN) AS u FROM range(64) tbl(i)",
	                   false, true);

	// BOOLEAN as a MAP key: keys used to collapse to one byte value, crashing ingest with
	// a duplicate-key error.
	TestArrowRoundtrip("SELECT MAP {true: i, false: i + 1} AS m FROM range(64) tbl(i)", false, true);

	// Two levels of nesting.
	TestArrowRoundtrip("SELECT [{'b': (i % 2 = 0)}]::STRUCT(b BOOLEAN)[] AS l FROM range(64) tbl(i)", false, true);
}

// A constant BOOLEAN child reaches the arrow.bool8 conversion with a non-identity
// selection (every row maps to the single value); it must still expand to that value
// for every row.
TEST_CASE("Test Arrow constant BOOLEAN child roundtrip", "[arrow]") {
	TestArrowRoundtrip("SELECT {'c': true, 'v': (i % 2 = 0)}::STRUCT(c BOOLEAN, v BOOLEAN) AS s FROM range(64) tbl(i)",
	                   false, true);
	TestArrowRoundtrip("SELECT [true, true]::BOOLEAN[] AS l FROM range(64) tbl(i)", false, true);
}

// A LIST(BOOLEAN) whose total child element count per chunk exceeds STANDARD_VECTOR_SIZE:
// guards the converted vector being sized to the actual child count rather than 2048.
TEST_CASE("Test Arrow large LIST(BOOLEAN) roundtrip", "[arrow]") {
	TestArrowRoundtrip("SELECT [(i % 2 = 0), (i % 3 = 0), (i % 5 = 0)]::BOOLEAN[] AS l FROM range(10000) tbl(i)", false,
	                   true);
}
TEST_CASE("Test Arrow Extension Types", "[arrow][.]") {
	// UUID
	TestArrowRoundtrip("SELECT '2d89ebe6-1e13-47e5-803a-b81c87660b66'::UUID str FROM range(5) tbl(i)", false, true);

	// HUGEINT
	TestArrowRoundtrip("SELECT '170141183460469231731687303715884105727'::HUGEINT str FROM range(5) tbl(i)", false,
	                   true);

	// UHUGEINT
	TestArrowRoundtrip("SELECT '170141183460469231731687303715884105727'::UHUGEINT str FROM range(5) tbl(i)", false,
	                   true);

	// UHUGEINT (lossy - should export as Decimal(38,0), not extension type)
	{
		DuckDB db;
		Connection con(db);
		auto client_properties = con.context->GetClientProperties();
		ArrowSchema schema;
		schema.Init();
		vector<LogicalType> types = {LogicalType::UHUGEINT};
		vector<string> names = {"col"};
		ArrowConverter::ToArrowSchema(&schema, types, names, client_properties);
		REQUIRE(schema.n_children == 1);
		REQUIRE(string(schema.children[0]->format) == "d:38,0");
		schema.release(&schema);
	}

	// BIT
	TestArrowRoundtrip("SELECT '0101011'::BIT str FROM range(5) tbl(i)", false, true);

	// TIME_TZ
	TestArrowRoundtrip("SELECT '02:30:00+04'::TIMETZ str FROM range(5) tbl(i)", false, true);

	// BIGNUM
	TestArrowRoundtrip("SELECT 85070591730234614260976917445211069672::BIGNUM str FROM range(5) tbl(i)", false, true);

	TestArrowRoundtrip("SELECT 85070591730234614260976917445211069672::BIGNUM str FROM range(5) tbl(i)", true, true);
}

TEST_CASE("Test Arrow Extension Types - JSON", "[arrow][.]") {
	DBConfig config;
	DuckDB db(nullptr, &config);
	Connection con(db);

	if (!db.ExtensionIsLoaded("json")) {
		return;
	}

	// JSON
	TestArrowRoundtrip("SELECT '{\"name\":\"Pedro\", \"age\":28, \"car\":\"VW Fox\"}'::JSON str FROM range(5) tbl(i)",
	                   false, true);
}

TEST_CASE("Test Arrow String View", "[arrow][.]") {
	// Test Small Strings
	TestArrowRoundtripStringView("SELECT (i*10^i)::varchar str FROM range(5) tbl(i)");

	// Test Small Strings + Nulls
	TestArrowRoundtripStringView("SELECT (i*10^i)::varchar str FROM range(5) tbl(i) UNION SELECT NULL");

	// Test Big Strings
	TestArrowRoundtripStringView("SELECT 'Imaverybigstringmuchbiggerthanfourbytes' str FROM range(5) tbl(i)");

	// Test Big Strings + Nulls
	TestArrowRoundtripStringView("SELECT 'Imaverybigstringmuchbiggerthanfourbytes'||i::varchar str FROM range(5) "
	                             "tbl(i) UNION SELECT NULL order by str");

	// Test Mix of Small/Big/NULL Strings
	TestArrowRoundtripStringView(
	    "SELECT 'Imaverybigstringmuchbiggerthanfourbytes'||i::varchar str FROM range(10000) tbl(i) UNION "
	    "SELECT NULL UNION SELECT (i*10^i)::varchar str FROM range(10000) tbl(i)");
}

TEST_CASE("Test TPCH arrow roundtrip", "[arrow][.]") {
	DBConfig config;
	DuckDB db(nullptr, &config);
	Connection con(db);
	if (!db.ExtensionIsLoaded("tpch")) {
		return;
	}
	con.Query("CALL dbgen(sf=0.5)");

	// REQUIRE(ArrowTestHelper::RunArrowComparison(con, "SELECT * FROM lineitem;", false));
	// REQUIRE(ArrowTestHelper::RunArrowComparison(con, "SELECT l_orderkey, l_shipdate, l_comment FROM lineitem ORDER BY
	// l_orderkey DESC;", false)); REQUIRE(ArrowTestHelper::RunArrowComparison(con, "SELECT lineitem FROM lineitem;",
	// false)); REQUIRE(ArrowTestHelper::RunArrowComparison(con, "SELECT [lineitem] FROM lineitem;", false));

	con.Query("create table lineitem_no_constraint as from lineitem;");
	con.Query("update lineitem_no_constraint set l_comment=null where l_orderkey%2=0;");

	// REQUIRE(ArrowTestHelper::RunArrowComparison(con, "SELECT * FROM lineitem_no_constraint;", false));
	REQUIRE(ArrowTestHelper::RunArrowComparison(
	    con, "SELECT l_orderkey, l_shipdate, l_comment FROM lineitem_no_constraint ORDER BY l_orderkey DESC;", true));
	REQUIRE(
	    ArrowTestHelper::RunArrowComparison(con, "SELECT lineitem_no_constraint FROM lineitem_no_constraint;", true));
	REQUIRE(
	    ArrowTestHelper::RunArrowComparison(con, "SELECT [lineitem_no_constraint] FROM lineitem_no_constraint;", true));
}

TEST_CASE("Test Parquet Files round-trip", "[arrow][.]") {
	std::vector<std::string> data;
	// data.emplace_back("data/parquet-testing/7-set.snappy.arrow2.parquet");
	//	data.emplace_back("data/parquet-testing/adam_genotypes.parquet");
	data.emplace_back("data/parquet-testing/apkwan.parquet");
	data.emplace_back("data/parquet-testing/aws1.snappy.parquet");
	// not supported by arrow
	// data.emplace_back("data/parquet-testing/aws2.parquet");
	data.emplace_back("data/parquet-testing/binary_string.parquet");
	data.emplace_back("data/parquet-testing/blob.parquet");
	data.emplace_back("data/parquet-testing/boolean_stats.parquet");
	// arrow can't read this
	// data.emplace_back("data/parquet-testing/broken-arrow.parquet");
	data.emplace_back("data/parquet-testing/bug1554.parquet");
	data.emplace_back("data/parquet-testing/bug1588.parquet");
	data.emplace_back("data/parquet-testing/bug1589.parquet");
	data.emplace_back("data/parquet-testing/bug1618_struct_strings.parquet");
	data.emplace_back("data/parquet-testing/bug2267.parquet");
	data.emplace_back("data/parquet-testing/bug2557.parquet");
	// slow
	// data.emplace_back("data/parquet-testing/bug687_nulls.parquet");
	// data.emplace_back("data/parquet-testing/complex.parquet");
	data.emplace_back("data/parquet-testing/data-types.parquet");
	data.emplace_back("data/parquet-testing/date.parquet");
	// arrow can't read this because it's a time with a timezone and it's not supported by arrow
	//	data.emplace_back("data/parquet-testing/date_stats.parquet");
	data.emplace_back("data/parquet-testing/decimal_stats.parquet");
	data.emplace_back("data/parquet-testing/decimals.parquet");
	data.emplace_back("data/parquet-testing/enum.parquet");
	data.emplace_back("data/parquet-testing/filter_bug1391.parquet");
	//	data.emplace_back("data/parquet-testing/fixed.parquet");
	// slow
	// data.emplace_back("data/parquet-testing/leftdate3_192_loop_1.parquet");
	data.emplace_back("data/parquet-testing/lineitem-top10000.gzip.parquet");
	data.emplace_back("data/parquet-testing/manyrowgroups.parquet");
	data.emplace_back("data/parquet-testing/manyrowgroups2.parquet");
	//	data.emplace_back("data/parquet-testing/map.parquet");
	// Can't roundtrip NaNs
	data.emplace_back("data/parquet-testing/nan-float.parquet");
	// null byte in file
	// data.emplace_back("data/parquet-testing/nullbyte.parquet");
	// data.emplace_back("data/parquet-testing/nullbyte_multiple.parquet");
	// borked
	// data.emplace_back("data/parquet-testing/p2.parquet");
	// data.emplace_back("data/parquet-testing/p2strings.parquet");
	data.emplace_back("data/parquet-testing/pandas-date.parquet");
	data.emplace_back("data/parquet-testing/signed_stats.parquet");
	data.emplace_back("data/parquet-testing/silly-names.parquet");
	// borked
	// data.emplace_back("data/parquet-testing/simple.parquet");
	// data.emplace_back("data/parquet-testing/sorted.zstd_18_131072_small.parquet");
	data.emplace_back("data/parquet-testing/struct.parquet");
	data.emplace_back("data/parquet-testing/struct_skip_test.parquet");
	data.emplace_back("data/parquet-testing/timestamp-ms.parquet");
	data.emplace_back("data/parquet-testing/timestamp.parquet");
	data.emplace_back("data/parquet-testing/unsigned.parquet");
	data.emplace_back("data/parquet-testing/unsigned_stats.parquet");
	data.emplace_back("data/parquet-testing/userdata1.parquet");
	data.emplace_back("data/parquet-testing/varchar_stats.parquet");
	data.emplace_back("data/parquet-testing/zstd.parquet");

	for (auto &parquet_path : data) {
		TestParquetRoundtrip(parquet_path);
	}
}

//===--------------------------------------------------------------------===//
// Registered extension type with a NESTED (struct) storage type
//===--------------------------------------------------------------------===//
// A synthetic extension whose Arrow STORAGE type is a struct with a DIFFERENT child count than the
// logical type: logical = STRUCT(a BIGINT, b BIGINT) registered under the alias "arrow_test_pair",
// storage = STRUCT(packed STRUCT(a BIGINT, b BIGINT)) — a single wrapping child. This mirrors the shape
// of e.g. the canonical arrow.parquet.variant extension (a logical VARIANT over struct<metadata, value>
// storage). The appender initializes and appends with the extension's INTERNAL (storage) type, so it
// must also FINALIZE with it: before the fix, FinalizeChild handed the finalizer the LOGICAL type, and
// the struct finalizer walked the logical child count over the storage-built child_data — out of bounds.

namespace {

//! Engagement counters: the roundtrip silently degrades to a plain-struct roundtrip (values still
//! compare equal!) if the extension is not picked up — assert every stage actually ran.
static std::atomic<int> pair_populate_calls {0};
static std::atomic<int> pair_get_type_calls {0};
static std::atomic<int> pair_duck_to_arrow_calls {0};
static std::atomic<int> pair_arrow_to_duck_calls {0};

LogicalType PairStorageMemberType() {
	return LogicalType::STRUCT({{"a", LogicalType::BIGINT}, {"b", LogicalType::BIGINT}});
}

LogicalType PairLogicalType() {
	// The extension registry keys on (alias, type id), and the schema conversion consults it for
	// alias-carrying types — the logical pair type therefore carries an alias. (A SQL cast to a
	// CREATE TYPE alias resolves the alias away, so the test produces values through a registered
	// scalar function whose RETURN type carries it — the same way extension-defined types do.)
	return PairStorageMemberType().WithAlias("arrow_test_pair");
}

// pair_typed(STRUCT(a, b)) -> the alias-carrying logical pair type (children pass through).
void PairTypedFunction(DataChunk &args, ExpressionState &, Vector &result) {
	auto &in = args.data[0];
	in.Flatten();
	auto &in_entries = StructVector::GetEntries(in);
	auto &out_entries = StructVector::GetEntries(result);
	out_entries[0].Reference(in_entries[0]);
	out_entries[1].Reference(in_entries[1]);
	for (idx_t i = 0; i < args.size(); i++) {
		if (FlatVector::IsNull(in, i)) {
			FlatVector::SetNull(result, i, true);
		}
	}
	result.SetVectorType(VectorType::FLAT_VECTOR);
}

LogicalType PairStorageType() {
	return LogicalType::STRUCT({{"packed", PairStorageMemberType()}});
}

// DuckDB -> Arrow: wrap the logical pair rows in the single storage child (buffers shared, no copy).
void PairDuckToArrow(ClientContext &, const Vector &source, Vector &result, idx_t count) {
	++pair_duck_to_arrow_calls;
	source.Flatten();
	auto &packed = StructVector::GetEntries(result)[0];
	// Reinterpret, not Reference: the storage child and the logical pair type have the same physical layout
	// but differ in the ALIAS, which LogicalType equality compares - so Reference would trip its type
	// assertion in an assertion-enabled build.
	packed.Reinterpret(source);
	for (idx_t i = 0; i < count; i++) {
		if (FlatVector::IsNull(source, i)) {
			FlatVector::SetNull(result, i, true);
		}
	}
}

// Arrow -> DuckDB: unwrap the storage child back into the logical pair rows.
void PairArrowToDuck(ClientContext &, Vector &source, Vector &result, idx_t count) {
	++pair_arrow_to_duck_calls;
	source.Flatten();
	auto &packed = StructVector::GetEntries(source)[0];
	packed.Flatten();
	auto &packed_entries = StructVector::GetEntries(packed);
	auto &result_entries = StructVector::GetEntries(result);
	result_entries[0].Reference(packed_entries[0]);
	result_entries[1].Reference(packed_entries[1]);
	for (idx_t i = 0; i < count; i++) {
		if (FlatVector::IsNull(source, i) || FlatVector::IsNull(packed, i)) {
			FlatVector::SetNull(result, i, true);
		}
	}
}

// Declares the STORAGE schema (struct<packed: struct<a, b>>) tagged with the extension name. All names
// and formats are string literals (static storage), so no ownership registration is needed.
void PairPopulateSchema(DuckDBArrowSchemaHolder &root_holder, ArrowSchema &schema, const LogicalType &, ClientContext &,
                        const ArrowTypeExtension &extension) {
	++pair_populate_calls;
	const auto metadata = ArrowSchemaMetadata::ArrowCanonicalType(extension.GetInfo().GetExtensionName());
	root_holder.metadata_info.emplace_back(metadata.SerializeMetadata());
	schema.metadata = root_holder.metadata_info.back().get();

	auto release_child = [](ArrowSchema *child) {
		child->release = nullptr;
	};

	schema.format = "+s";
	schema.n_children = 1;
	root_holder.nested_children.emplace_back();
	root_holder.nested_children.back().resize(1);
	root_holder.nested_children_ptr.emplace_back();
	root_holder.nested_children_ptr.back().push_back(&root_holder.nested_children.back()[0]);
	schema.children = root_holder.nested_children_ptr.back().data();

	auto &packed = *schema.children[0];
	packed.format = "+s";
	packed.name = "packed";
	packed.flags = ARROW_FLAG_NULLABLE;
	packed.release = release_child;
	packed.n_children = 2;
	root_holder.nested_children.emplace_back();
	root_holder.nested_children.back().resize(2);
	root_holder.nested_children_ptr.emplace_back();
	root_holder.nested_children_ptr.back().push_back(&root_holder.nested_children.back()[0]);
	root_holder.nested_children_ptr.back().push_back(&root_holder.nested_children.back()[1]);
	packed.children = root_holder.nested_children_ptr.back().data();

	const char *member_names[] = {"a", "b"};
	for (idx_t i = 0; i < 2; i++) {
		auto &member = *packed.children[i];
		member.format = "l";
		member.name = member_names[i];
		member.flags = ARROW_FLAG_NULLABLE;
		member.release = release_child;
	}
}

// Maps the tagged schema back: the LOGICAL type for the result, with type info describing the STORAGE
// tree for the reader's buffer walk.
unique_ptr<ArrowType> PairGetType(ClientContext &, const ArrowSchema &, const ArrowSchemaMetadata &) {
	++pair_get_type_calls;
	vector<shared_ptr<ArrowType>> members;
	members.push_back(make_shared_ptr<ArrowType>(LogicalType::BIGINT));
	members.push_back(make_shared_ptr<ArrowType>(LogicalType::BIGINT));
	auto packed = make_shared_ptr<ArrowType>(PairStorageMemberType(), make_uniq<ArrowStructInfo>(std::move(members)));
	vector<shared_ptr<ArrowType>> children;
	children.push_back(std::move(packed));
	return make_uniq<ArrowType>(PairLogicalType(), make_uniq<ArrowStructInfo>(std::move(children)));
}

} // namespace

TEST_CASE("Test Arrow extension type with nested storage", "[arrow]") {
	DuckDB db(nullptr, nullptr);
	// register on the INSTANCE's config — the extension registry is not carried over from a user config
	DBConfig::GetConfig(*db.instance)
	    .RegisterArrowExtension({"arrow_test.pair", &PairPopulateSchema, &PairGetType,
	                             make_shared_ptr<ArrowTypeExtensionData>(PairLogicalType(), PairStorageType(),
	                                                                     PairArrowToDuck, PairDuckToArrow)});
	Connection con(db);

	// values reach the boundary through a function whose RETURN type carries the registered alias
	ScalarFunction fn("pair_typed", {PairStorageMemberType()}, PairLogicalType(), PairTypedFunction);
	CreateScalarFunctionInfo fn_info(fn);
	con.context->RunFunctionInTransaction(
	    [&]() { Catalog::GetSystemCatalog(*con.context).CreateFunction(*con.context, fn_info); });

	// plain values (multiple vectors' worth) + NULL rows + NULL members
	REQUIRE(
	    ArrowTestHelper::RunArrowComparison(con, "SELECT pair_typed({'a': i, 'b': i + 1}) AS p FROM range(3000) t(i)"));
	REQUIRE(ArrowTestHelper::RunArrowComparison(
	    con, "SELECT pair_typed(CASE WHEN i % 3 = 0 THEN NULL WHEN i % 3 = 1 THEN {'a': i, 'b': NULL} "
	         "ELSE {'a': NULL, 'b': i} END) AS p FROM range(100) t(i)"));

	// the roundtrip must have gone THROUGH the extension (schema declared, values converted both ways) —
	// otherwise the comparison above passes vacuously on a plain-struct roundtrip
	REQUIRE(pair_populate_calls.load() > 0);
	REQUIRE(pair_duck_to_arrow_calls.load() > 0);
	REQUIRE(pair_get_type_calls.load() > 0);
	REQUIRE(pair_arrow_to_duck_calls.load() > 0);
}

TEST_CASE("Test Arrow VARIANT roundtrip", "[arrow]") {
	// VARIANT travels as the canonical arrow.parquet.variant extension type: struct<metadata: binary,
	// value: binary> storage carrying the Variant spec's binary encoding.
	vector<string> queries = {
	    "SELECT 42::VARIANT AS v",
	    "SELECT {'a': i, 'b': 'x' || i::VARCHAR}::VARIANT AS v FROM range(3000) t(i)",
	    "SELECT [i, i + 1, NULL]::VARIANT AS v FROM range(100) t(i)",
	    // a nested VARIANT child is sized by its parent list, not by STANDARD_VECTOR_SIZE
	    "SELECT [x::VARIANT FOR x IN range(5000)] AS v",
	    "SELECT {'k': [x::VARIANT FOR x IN range(3000)]} AS v",
	    // mixed types, no NULLs
	    "SELECT CASE WHEN i % 2 = 0 THEN i::VARIANT ELSE ('s' || i::VARCHAR)::VARIANT END AS v FROM range(10) t(i)",
	    "SELECT CASE WHEN i % 2 = 0 THEN NULL ELSE i::VARIANT END AS v FROM range(10) t(i)",
	    "SELECT CASE WHEN i % 3 = 0 THEN NULL WHEN i % 3 = 1 THEN i::VARIANT "
	    "ELSE ('s' || i::VARCHAR)::VARIANT END AS v FROM range(100) t(i)",
	};
	for (auto &query : queries) {
		INFO("query: " << query);
		TestArrowRoundtrip(query);
	}
}

//===--------------------------------------------------------------------===//
// arrow.parquet.variant — foreign-writer shapes
//===--------------------------------------------------------------------===//
// DuckDB's own export always produces struct<metadata, value> in that order, with plain binary
// children. The canonical spec is wider: the fields may appear in ANY order, may be dictionary- or
// run-end-encoded, and a shredded variant carries a typed_value field. These tests imitate such
// foreign writers — by mutating a DuckDB-exported schema/array, or by hand-building the Arrow C Data
// structures outright.

namespace {

enum class VariantMutation : uint8_t {
	NONE,             // control
	SWAP_FIELD_ORDER, // struct<value, metadata> — spec-valid, must be resolved by name
	SHREDDED_FIELD,   // value renamed typed_value — a (fully) shredded variant, unsupported
	UNKNOWN_FIELD,    // value renamed to a foreign name — invalid
	UNION_FORMAT      // parent format +us:0,1 — not a struct
};

struct MutatedVariantData {
	vector<LogicalType> types;
	vector<string> names;
	duckdb::unique_ptr<QueryResult> result;
	ClientProperties options;
	ClientContext *context;
	VariantMutation mutation;
};

void ApplyVariantSchemaMutation(ArrowSchema &column, VariantMutation mutation) {
	switch (mutation) {
	case VariantMutation::SWAP_FIELD_ORDER:
		std::swap(column.children[0], column.children[1]);
		break;
	case VariantMutation::SHREDDED_FIELD:
		column.children[1]->name = "typed_value";
		break;
	case VariantMutation::UNKNOWN_FIELD:
		column.children[1]->name = "sidecar";
		break;
	case VariantMutation::UNION_FORMAT:
		column.format = "+us:0,1";
		break;
	case VariantMutation::NONE:
		break;
	}
}

int MutatedVariantGetSchema(ArrowArrayStream *stream, ArrowSchema *out) {
	auto &data = *reinterpret_cast<MutatedVariantData *>(stream->private_data);
	ArrowConverter::ToArrowSchema(out, data.types, data.names, data.options);
	ApplyVariantSchemaMutation(*out->children[0], data.mutation);
	return 0;
}

int MutatedVariantGetNext(ArrowArrayStream *stream, ArrowArray *out) {
	auto &data = *reinterpret_cast<MutatedVariantData *>(stream->private_data);
	auto chunk = data.result->Fetch();
	if (!chunk || chunk->size() == 0) {
		return 0;
	}
	auto extension_types = ArrowTypeExtensionData::GetExtensionTypes(*data.context, data.types);
	ArrowConverter::ToArrowArray(*chunk, out, data.options, extension_types);
	if (data.mutation == VariantMutation::SWAP_FIELD_ORDER) {
		auto &column = *out->children[0];
		std::swap(column.children[0], column.children[1]);
	}
	return 0;
}

const char *MutatedVariantGetLastError(ArrowArrayStream *) {
	return nullptr;
}

void MutatedVariantRelease(ArrowArrayStream *stream) {
	if (!stream || !stream->private_data) {
		return;
	}
	delete reinterpret_cast<MutatedVariantData *>(stream->private_data);
	stream->private_data = nullptr;
	stream->release = nullptr;
}

void MakeMutatedVariantStream(Connection &con, const string &query, VariantMutation mutation,
                              ArrowArrayStream &stream) {
	auto result = con.Query(query);
	REQUIRE(!result->HasError());
	auto data = make_uniq<MutatedVariantData>();
	data->types = result->GetTypes();
	data->names = IdentifiersToStrings(result->GetNames());
	data->options = con.context->GetClientProperties();
	data->context = con.context.get();
	data->result = std::move(result);
	data->mutation = mutation;
	stream.get_schema = MutatedVariantGetSchema;
	stream.get_next = MutatedVariantGetNext;
	stream.get_last_error = MutatedVariantGetLastError;
	stream.release = MutatedVariantRelease;
	stream.private_data = data.release();
}

//! Scans the stream and expects the BIND to refuse it with `expected_error` (a refusal may surface as
//! an error result or as an exception escaping the relation, depending on where the bind runs).
void ExpectVariantScanError(Connection &con, ArrowArrayStream &stream, const string &expected_error) {
	auto params = ArrowTestHelper::ConstructArrowScan(stream);
	bool refused = false;
	string error_message;
	try {
		auto result = con.TableFunction("arrow_scan", {}, {}, params)->Execute();
		if (result->HasError()) {
			refused = true;
			error_message = result->GetError();
		}
	} catch (std::exception &ex) {
		refused = true;
		error_message = ex.what();
	}
	REQUIRE(refused);
	REQUIRE(StringUtil::Contains(error_message, expected_error));
	if (stream.release) {
		stream.release(&stream);
	}
}

} // namespace

TEST_CASE("Test Arrow VARIANT foreign field order and refusals", "[arrow]") {
	DuckDB db;
	Connection con(db);
	const string query = "SELECT CASE WHEN i % 3 = 0 THEN NULL ELSE {'a': i}::VARIANT END AS v FROM range(50) t(i)";

	{ // control: the unmutated stream round-trips
		ArrowArrayStream stream;
		MakeMutatedVariantStream(con, query, VariantMutation::NONE, stream);
		REQUIRE(ArrowTestHelper::RunArrowComparison(con, query, stream));
	}
	{ // struct<value, metadata> is spec-valid: the fields are resolved by NAME, not position
		ArrowArrayStream stream;
		MakeMutatedVariantStream(con, query, VariantMutation::SWAP_FIELD_ORDER, stream);
		REQUIRE(ArrowTestHelper::RunArrowComparison(con, query, stream));
	}
	{ // a fully shredded variant (metadata + typed_value) is refused clearly, not misread as value bytes
		ArrowArrayStream stream;
		MakeMutatedVariantStream(con, query, VariantMutation::SHREDDED_FIELD, stream);
		ExpectVariantScanError(con, stream, "shredded");
	}
	{ // an unknown field name is refused
		ArrowArrayStream stream;
		MakeMutatedVariantStream(con, query, VariantMutation::UNKNOWN_FIELD, stream);
		ExpectVariantScanError(con, stream, "unexpected or duplicate field");
	}
	{ // a non-struct storage type is refused, even with a plausible child count
		ArrowArrayStream stream;
		MakeMutatedVariantStream(con, query, VariantMutation::UNION_FORMAT, stream);
		ExpectVariantScanError(con, stream, "must have a struct storage type");
	}
}

//===--------------------------------------------------------------------===//
// arrow.parquet.variant — hand-built dictionary / run-end-encoded metadata
//===--------------------------------------------------------------------===//

namespace {

//! Hand-built Arrow C Data for a variant column of `count` integer rows (1..count) whose METADATA
//! child is dictionary- or run-end-encoded — every row shares one metadata blob, which is exactly the
//! shape those encodings exist for. All memory lives on this holder; the STREAM release frees it (the
//! schema/array releases are no-ops, mirroring how children are owned in the C Data interface).
struct HandBuiltVariantHolder {
	enum class Encoding : uint8_t { DICTIONARY, RUN_END };

	Encoding encoding = Encoding::DICTIONARY;
	idx_t count = 0;
	bool array_served = false;

	// data buffers
	string metadata_bytes;          // the one shared metadata blob
	string value_bytes;             // concatenated per-row value blobs
	vector<int32_t> value_offsets;  // count + 1
	vector<int8_t> dict_indices;    // DICTIONARY: count zeros
	vector<int32_t> run_ends;       // RUN_END: {count}
	vector<int32_t> single_offsets; // {0, len(metadata_bytes)} — the 1-element binary child

	// buffer pointer tables
	const void *no_buffers[1] = {nullptr};
	const void *value_buffers[3] = {nullptr, nullptr, nullptr};
	const void *indices_buffers[2] = {nullptr, nullptr};
	const void *single_binary_buffers[3] = {nullptr, nullptr, nullptr};
	const void *run_ends_buffers[2] = {nullptr, nullptr};

	// schema nodes
	unsafe_unique_array<char> extension_metadata;
	ArrowSchema s_root {}, s_col {}, s_metadata {}, s_value {}, s_typed_value {}, s_dict {}, s_run_ends {}, s_values {};
	ArrowSchema *s_root_children[1] = {nullptr};
	ArrowSchema *s_col_children[3] = {nullptr, nullptr, nullptr};
	ArrowSchema *s_ree_children[2] = {nullptr, nullptr};

	// array nodes
	ArrowArray a_root {}, a_col {}, a_metadata {}, a_value {}, a_dict {}, a_run_ends {}, a_values {};
	ArrowArray *a_root_children[1] = {nullptr};
	ArrowArray *a_col_children[2] = {nullptr, nullptr};
	ArrowArray *a_ree_children[2] = {nullptr, nullptr};
};

void HandBuiltSchemaRelease(ArrowSchema *schema) {
	// memory is owned by the holder, freed with the stream
	schema->release = nullptr;
}

void HandBuiltArrayRelease(ArrowArray *array) {
	array->release = nullptr;
}

duckdb::unique_ptr<HandBuiltVariantHolder> BuildEncodedVariant(Connection &con, idx_t count,
                                                               HandBuiltVariantHolder::Encoding encoding,
                                                               bool with_typed_value = false) {
	auto holder = make_uniq<HandBuiltVariantHolder>();
	holder->encoding = encoding;
	holder->count = count;

	// take the REAL encodings from the parquet extension's own conversion instead of hand-writing spec
	// bytes (cast away the PARQUET_VARIANT alias — an aliased struct does not bind struct_extract)
	auto encoded = con.Query("SELECT pv.metadata, pv.value FROM (SELECT "
	                         "variant_to_parquet_variant((i + 1)::VARIANT)::STRUCT(metadata BLOB, value BLOB) AS pv "
	                         "FROM range(" +
	                         to_string(count) + ") t(i))");
	REQUIRE(!encoded->HasError());
	holder->value_offsets.push_back(0);
	auto encoded_rows = encoded->Collection().GetRows();
	for (idx_t row = 0; row < count; row++) {
		auto metadata = StringValue::Get(encoded_rows.GetValue(0, row));
		auto value = StringValue::Get(encoded_rows.GetValue(1, row));
		if (row == 0) {
			holder->metadata_bytes = metadata;
		} else {
			// integer-only variants carry no dictionary keys => identical metadata across the rows,
			// which is the shape a dictionary/run-end encoding exists for
			REQUIRE(metadata == holder->metadata_bytes);
		}
		holder->value_bytes += value;
		holder->value_offsets.push_back(static_cast<int32_t>(holder->value_bytes.size()));
	}
	holder->single_offsets = {0, static_cast<int32_t>(holder->metadata_bytes.size())};

	// ---- schema ----
	const auto tag = ArrowSchemaMetadata::ArrowCanonicalType("arrow.parquet.variant");
	holder->extension_metadata = tag.SerializeMetadata();

	auto &s_value = holder->s_value;
	s_value.format = "z";
	s_value.name = "value";
	s_value.flags = ARROW_FLAG_NULLABLE;
	s_value.release = HandBuiltSchemaRelease;

	auto &s_metadata = holder->s_metadata;
	s_metadata.name = "metadata";
	s_metadata.release = HandBuiltSchemaRelease;
	if (encoding == HandBuiltVariantHolder::Encoding::DICTIONARY) {
		s_metadata.format = "c"; // int8 indices
		holder->s_dict.format = "z";
		holder->s_dict.name = "";
		holder->s_dict.release = HandBuiltSchemaRelease;
		s_metadata.dictionary = &holder->s_dict;
	} else {
		s_metadata.format = "+r";
		holder->s_run_ends.format = "i";
		holder->s_run_ends.name = "run_ends";
		holder->s_run_ends.release = HandBuiltSchemaRelease;
		holder->s_values.format = "z";
		holder->s_values.name = "values";
		holder->s_values.release = HandBuiltSchemaRelease;
		holder->s_ree_children[0] = &holder->s_run_ends;
		holder->s_ree_children[1] = &holder->s_values;
		s_metadata.n_children = 2;
		s_metadata.children = holder->s_ree_children;
	}

	auto &s_col = holder->s_col;
	s_col.format = "+s";
	s_col.name = "v";
	s_col.flags = ARROW_FLAG_NULLABLE;
	s_col.metadata = holder->extension_metadata.get();
	s_col.release = HandBuiltSchemaRelease;
	holder->s_col_children[0] = &s_metadata;
	holder->s_col_children[1] = &s_value;
	s_col.n_children = 2;
	s_col.children = holder->s_col_children;
	if (with_typed_value) {
		holder->s_typed_value.format = "z";
		holder->s_typed_value.name = "typed_value";
		holder->s_typed_value.flags = ARROW_FLAG_NULLABLE;
		holder->s_typed_value.release = HandBuiltSchemaRelease;
		holder->s_col_children[2] = &holder->s_typed_value;
		s_col.n_children = 3;
	}

	auto &s_root = holder->s_root;
	s_root.format = "+s";
	s_root.name = "";
	s_root.release = HandBuiltSchemaRelease;
	holder->s_root_children[0] = &s_col;
	s_root.n_children = 1;
	s_root.children = holder->s_root_children;

	// ---- array ----
	auto &a_value = holder->a_value;
	a_value.length = static_cast<int64_t>(count);
	a_value.n_buffers = 3;
	holder->value_buffers[1] = holder->value_offsets.data();
	holder->value_buffers[2] = holder->value_bytes.data();
	a_value.buffers = holder->value_buffers;
	a_value.release = HandBuiltArrayRelease;

	auto &a_metadata = holder->a_metadata;
	a_metadata.length = static_cast<int64_t>(count);
	a_metadata.release = HandBuiltArrayRelease;
	if (encoding == HandBuiltVariantHolder::Encoding::DICTIONARY) {
		holder->dict_indices.assign(count, 0);
		a_metadata.n_buffers = 2;
		holder->indices_buffers[1] = holder->dict_indices.data();
		a_metadata.buffers = holder->indices_buffers;
		auto &a_dict = holder->a_dict;
		a_dict.length = 1;
		a_dict.n_buffers = 3;
		holder->single_binary_buffers[1] = holder->single_offsets.data();
		holder->single_binary_buffers[2] = holder->metadata_bytes.data();
		a_dict.buffers = holder->single_binary_buffers;
		a_dict.release = HandBuiltArrayRelease;
		a_metadata.dictionary = &a_dict;
	} else {
		holder->run_ends = {static_cast<int32_t>(count)};
		a_metadata.n_buffers = 1;
		a_metadata.buffers = holder->no_buffers;
		auto &a_run_ends = holder->a_run_ends;
		a_run_ends.length = 1;
		a_run_ends.n_buffers = 2;
		holder->run_ends_buffers[1] = holder->run_ends.data();
		a_run_ends.buffers = holder->run_ends_buffers;
		a_run_ends.release = HandBuiltArrayRelease;
		auto &a_values = holder->a_values;
		a_values.length = 1;
		a_values.n_buffers = 3;
		holder->single_binary_buffers[1] = holder->single_offsets.data();
		holder->single_binary_buffers[2] = holder->metadata_bytes.data();
		a_values.buffers = holder->single_binary_buffers;
		a_values.release = HandBuiltArrayRelease;
		holder->a_ree_children[0] = &a_run_ends;
		holder->a_ree_children[1] = &a_values;
		a_metadata.n_children = 2;
		a_metadata.children = holder->a_ree_children;
	}

	auto &a_col = holder->a_col;
	a_col.length = static_cast<int64_t>(count);
	a_col.n_buffers = 1;
	a_col.buffers = holder->no_buffers;
	a_col.release = HandBuiltArrayRelease;
	holder->a_col_children[0] = &a_metadata;
	holder->a_col_children[1] = &a_value;
	a_col.n_children = 2;
	a_col.children = holder->a_col_children;

	auto &a_root = holder->a_root;
	a_root.length = static_cast<int64_t>(count);
	a_root.n_buffers = 1;
	a_root.buffers = holder->no_buffers;
	a_root.release = HandBuiltArrayRelease;
	holder->a_root_children[0] = &a_col;
	a_root.n_children = 1;
	a_root.children = holder->a_root_children;
	return holder;
}

int HandBuiltGetSchema(ArrowArrayStream *stream, ArrowSchema *out) {
	auto &holder = *reinterpret_cast<HandBuiltVariantHolder *>(stream->private_data);
	*out = holder.s_root;
	return 0;
}

int HandBuiltGetNext(ArrowArrayStream *stream, ArrowArray *out) {
	auto &holder = *reinterpret_cast<HandBuiltVariantHolder *>(stream->private_data);
	if (holder.array_served) {
		return 0;
	}
	holder.array_served = true;
	*out = holder.a_root;
	return 0;
}

void HandBuiltRelease(ArrowArrayStream *stream) {
	if (!stream || !stream->private_data) {
		return;
	}
	delete reinterpret_cast<HandBuiltVariantHolder *>(stream->private_data);
	stream->private_data = nullptr;
	stream->release = nullptr;
}

void MakeHandBuiltStream(duckdb::unique_ptr<HandBuiltVariantHolder> holder, ArrowArrayStream &stream) {
	stream.get_schema = HandBuiltGetSchema;
	stream.get_next = HandBuiltGetNext;
	stream.get_last_error = MutatedVariantGetLastError;
	stream.release = HandBuiltRelease;
	stream.private_data = holder.release();
}

} // namespace

TEST_CASE("Test Arrow VARIANT dictionary and run-end encoded metadata", "[arrow]") {
	DuckDB db;
	Connection con(db);
	const idx_t count = 5;
	const string expected = "SELECT (i + 1)::VARIANT AS v FROM range(5) t(i)";

	for (auto encoding : {HandBuiltVariantHolder::Encoding::DICTIONARY, HandBuiltVariantHolder::Encoding::RUN_END}) {
		ArrowArrayStream stream;
		MakeHandBuiltStream(BuildEncodedVariant(con, count, encoding), stream);
		REQUIRE(ArrowTestHelper::RunArrowComparison(con, expected, stream));
	}

	{ // a partially shredded variant (metadata + value + typed_value) is refused with the clear message
		ArrowArrayStream stream;
		MakeHandBuiltStream(BuildEncodedVariant(con, count, HandBuiltVariantHolder::Encoding::DICTIONARY, true),
		                    stream);
		ExpectVariantScanError(con, stream, "shredded");
	}
}

namespace {

//! The schema export resolves settings and extension population through the client context, which
//! needs an active transaction — run it inside one, the way every real caller (a query) already does.
void ExportVariantSchema(Connection &con, ArrowSchema &schema) {
	auto result = con.Query("SELECT 42::VARIANT AS v");
	REQUIRE(!result->HasError());
	con.context->RunFunctionInTransaction([&]() {
		auto properties = con.context->GetClientProperties();
		ArrowConverter::ToArrowSchema(&schema, result->GetTypes(), IdentifiersToStrings(result->GetNames()),
		                              properties);
	});
}

} // namespace

TEST_CASE("Test Arrow VARIANT export layouts and schema flags", "[arrow]") {
	{ // default layout + the spec's nullability: metadata non-nullable, value nullable
		DuckDB db;
		Connection con(db);
		ArrowSchema schema;
		ExportVariantSchema(con, schema);
		auto &col = *schema.children[0];
		REQUIRE(string(col.format) == "+s");
		REQUIRE(col.n_children == 2);
		REQUIRE(string(col.children[0]->name) == "metadata");
		REQUIRE((col.children[0]->flags & ARROW_FLAG_NULLABLE) == 0);
		REQUIRE(string(col.children[1]->name) == "value");
		REQUIRE((col.children[1]->flags & ARROW_FLAG_NULLABLE) != 0);
		REQUIRE(string(col.children[0]->format) == "z");
		schema.release(&schema);
	}
	{ // the declared child format must match the layout the appender actually writes: large binary
		DuckDB db;
		Connection con(db);
		REQUIRE(!con.Query("SET arrow_large_buffer_size=true")->HasError());
		ArrowSchema schema;
		ExportVariantSchema(con, schema);
		REQUIRE(string(schema.children[0]->children[0]->format) == "Z");
		schema.release(&schema);
		REQUIRE(ArrowTestHelper::RunArrowComparison(con, "SELECT {'a': i}::VARIANT AS v FROM range(100) t(i)"));
	}
	{ // ... and binary view at arrow_output_version >= 1.4
		DuckDB db;
		Connection con(db);
		REQUIRE(!con.Query("SET arrow_output_version='1.4'")->HasError());
		ArrowSchema schema;
		ExportVariantSchema(con, schema);
		REQUIRE(string(schema.children[0]->children[0]->format) == "vz");
		schema.release(&schema);
		REQUIRE(ArrowTestHelper::RunArrowComparison(con, "SELECT {'a': i}::VARIANT AS v FROM range(100) t(i)"));
	}
	{ // NULL rows must not put NULLs into the non-nullable metadata child (backfilled with the minimal
	  // encoding instead)
		DuckDB db;
		Connection con(db);
		auto result = con.Query("SELECT CASE WHEN i % 2 = 0 THEN NULL ELSE i::VARIANT END AS v FROM range(4) t(i)");
		REQUIRE(!result->HasError());
		// Accumulate over EVERY chunk: at STANDARD_VECTOR_SIZE=2 these four rows arrive in two chunks, so a
		// single Fetch() would see one NULL rather than two.
		int64_t total_rows = 0;
		int64_t total_nulls = 0;
		while (auto chunk = result->Fetch()) {
			if (chunk->size() == 0) {
				break;
			}
			ArrowArray array;
			con.context->RunFunctionInTransaction([&]() {
				auto properties = con.context->GetClientProperties();
				auto extension_types = ArrowTypeExtensionData::GetExtensionTypes(*con.context, result->GetTypes());
				ArrowConverter::ToArrowArray(*chunk, &array, properties, extension_types);
			});
			auto &col = *array.children[0];
			// The invariant under test: the non-nullable metadata child never carries a NULL, whatever the
			// parent's validity is.
			REQUIRE(col.children[0]->null_count == 0);
			total_rows += col.length;
			total_nulls += col.null_count;
			array.release(&array);
		}
		REQUIRE(total_rows == 4);
		REQUIRE(total_nulls == 2);
	}
}
