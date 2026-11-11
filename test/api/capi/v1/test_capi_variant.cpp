#include "capi_tester.hpp"

using namespace duckdb;
using namespace std;

TEST_CASE("Test C API VARIANT type support", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;

	REQUIRE(tester.OpenDatabase(nullptr));
	REQUIRE_NO_FAIL(tester.Query("CREATE TABLE t1 (data VARIANT)"));
	REQUIRE_NO_FAIL(tester.Query("INSERT INTO t1 VALUES (42::VARIANT)"));
	REQUIRE_NO_FAIL(tester.Query("INSERT INTO t1 VALUES ('hello world'::VARIANT)"));

	result = tester.Query("SELECT data FROM t1");
	REQUIRE_NO_FAIL(*result);

	REQUIRE(result->ColumnType(0) == DUCKDB_TYPE_VARIANT);

	auto chunk = result->FetchChunk(0);
	REQUIRE(chunk);

	auto vec = duckdb_data_chunk_get_vector(chunk->GetChunk(), 0);

	auto type = duckdb_vector_get_column_type(vec);

	REQUIRE(duckdb_get_type_id(type) == DUCKDB_TYPE_VARIANT);

	duckdb_destroy_logical_type(&type);
}

static void VariantParameterBind(duckdb_bind_info info) {
	auto parameter = duckdb_bind_get_parameter(info, 0);
	auto text = duckdb_get_varchar(parameter);
	auto bind_data = new string(text);
	duckdb_free(text);
	duckdb_destroy_value(&parameter);
	duckdb_bind_set_bind_data(info, bind_data, [](void *data) { delete reinterpret_cast<string *>(data); });

	auto type = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
	duckdb_bind_add_result_column(info, "value", type);
	duckdb_destroy_logical_type(&type);
}

static void VariantParameterInit(duckdb_init_info info) {
	duckdb_init_set_init_data(info, new bool(false), [](void *data) { delete reinterpret_cast<bool *>(data); });
}

static void VariantParameterFunction(duckdb_function_info info, duckdb_data_chunk output) {
	auto &bind_data = *reinterpret_cast<string *>(duckdb_function_get_bind_data(info));
	auto &done = *reinterpret_cast<bool *>(duckdb_function_get_init_data(info));
	if (done) {
		duckdb_data_chunk_set_size(output, 0);
		return;
	}
	done = true;
	duckdb_vector_assign_string_element(duckdb_data_chunk_get_vector(output, 0), 0, bind_data.c_str());
	duckdb_data_chunk_set_size(output, 1);
}

TEST_CASE("Test C API VARIANT type created from its type id", "[capi]") {
	auto type = duckdb_create_logical_type(DUCKDB_TYPE_VARIANT);
	REQUIRE(duckdb_get_type_id(type) == DUCKDB_TYPE_VARIANT);

	auto vector = duckdb_create_vector(type, 1);
	REQUIRE(vector);
	auto vector_type = duckdb_vector_get_column_type(vector);
	REQUIRE(duckdb_get_type_id(vector_type) == DUCKDB_TYPE_VARIANT);
	duckdb_destroy_logical_type(&vector_type);
	duckdb_destroy_vector(&vector);

	CAPITester tester;
	REQUIRE(tester.OpenDatabase(nullptr));
	auto function = duckdb_create_table_function();
	duckdb_table_function_set_name(function, "variant_parameter");
	duckdb_table_function_add_parameter(function, type);
	duckdb_table_function_set_bind(function, VariantParameterBind);
	duckdb_table_function_set_init(function, VariantParameterInit);
	duckdb_table_function_set_function(function, VariantParameterFunction);
	REQUIRE(duckdb_register_table_function(tester.connection, function) == DuckDBSuccess);
	duckdb_destroy_table_function(&function);
	duckdb_destroy_logical_type(&type);

	auto result = tester.Query("SELECT * FROM variant_parameter(42::VARIANT)");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<string>(0, 0) == "42");
	result = tester.Query("SELECT * FROM variant_parameter({'a': [1, 2]}::VARIANT)");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<string>(0, 0) == "{'a': [1, 2]}");
	result = tester.Query("SELECT * FROM variant_parameter('hello')");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<string>(0, 0) == "hello");
}
