#include "capi_tester.hpp"

using namespace duckdb;
using namespace std;

TEST_CASE("Test C API NUMERIC type support", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;

	REQUIRE(tester.OpenDatabase(nullptr));
	REQUIRE_NO_FAIL(tester.Query("CREATE TABLE t1 (n NUMERIC)"));
	REQUIRE_NO_FAIL(tester.Query("INSERT INTO t1 VALUES (42), ('-1.5'), (NULL), "
	                             "('12345678901234567890123456789012345678901234567890.5')"));

	// the column identifies as NUMERIC; values are consumed as their decimal text, as doubles, or (when they fit) as
	// integers
	result = tester.Query("SELECT n FROM t1");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->ColumnType(0) == DUCKDB_TYPE_NUMERIC);
	REQUIRE(result->Fetch<string>(0, 0) == "42");
	REQUIRE(result->Fetch<string>(0, 1) == "-1.5");
	REQUIRE(result->IsNull(0, 2));
	REQUIRE(result->Fetch<string>(0, 3) == "12345678901234567890123456789012345678901234567890.5");
	REQUIRE(result->Fetch<double>(0, 0) == 42.0);
	REQUIRE(result->Fetch<double>(0, 1) == -1.5);
	REQUIRE(result->Fetch<int64_t>(0, 0) == 42);
	REQUIRE(result->Fetch<int32_t>(0, 1) == -2);

	// the chunk interface: the vector type is NUMERIC and the data are duckdb_string_t values
	result = tester.Query("SELECT n FROM t1");
	REQUIRE_NO_FAIL(*result);
	auto chunk = result->FetchChunk(0);
	REQUIRE(chunk);
	auto vec = duckdb_data_chunk_get_vector(chunk->GetChunk(), 0);
	auto type = duckdb_vector_get_column_type(vec);
	REQUIRE(duckdb_get_type_id(type) == DUCKDB_TYPE_NUMERIC);
	duckdb_destroy_logical_type(&type);
	auto data = reinterpret_cast<duckdb_string_t *>(duckdb_vector_get_data(vec));
	REQUIRE(data);
	auto validity = duckdb_vector_get_validity(vec);
	REQUIRE(duckdb_validity_row_is_valid(validity, 0));
	if (STANDARD_VECTOR_SIZE >= 3) {
		// the NULL is the third row: it sits in the first chunk only when a chunk holds at least three rows
		REQUIRE(!duckdb_validity_row_is_valid(validity, 2));
	}
	REQUIRE(duckdb_string_t_length(data[0]) > 0);

	// the logical type round-trips through the C API
	auto numeric_type = duckdb_create_logical_type(DUCKDB_TYPE_NUMERIC);
	REQUIRE(duckdb_get_type_id(numeric_type) == DUCKDB_TYPE_NUMERIC);
	duckdb_destroy_logical_type(&numeric_type);
}
