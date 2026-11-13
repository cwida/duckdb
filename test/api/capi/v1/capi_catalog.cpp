#include "capi_tester.hpp"
#include "duckdb.h"

#include <cstring> // for strcmp

using namespace duckdb;

//----------------------------------------------------------------------------------------------------------------------
// Test Catalog Interface in C API
//----------------------------------------------------------------------------------------------------------------------

static void Execute(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
	auto conn = (duckdb_connection)duckdb_scalar_function_get_extra_info(info);

	duckdb_client_context context;
	duckdb_connection_get_client_context(conn, &context);

	// Get first argument (schema name)
	auto input_size = duckdb_data_chunk_get_size(input);

	auto catalog_vector = duckdb_data_chunk_get_vector(input, 0);
	auto catalog_data = (duckdb_string_t *)duckdb_vector_get_data(catalog_vector);

	auto schema_vector = duckdb_data_chunk_get_vector(input, 1);
	auto schema_data = (duckdb_string_t *)duckdb_vector_get_data(schema_vector);

	auto name_vector = duckdb_data_chunk_get_vector(input, 2);
	auto name_data = (duckdb_string_t *)duckdb_vector_get_data(name_vector);

	auto result_data = (bool *)duckdb_vector_get_data(output);

	for (idx_t row_idx = 0; row_idx < input_size; row_idx++) {
		auto catalog_str = catalog_data[row_idx];
		string catalog_name(duckdb_string_t_data(&catalog_str), duckdb_string_t_length(catalog_str));

		duckdb_catalog catalog = duckdb_client_context_get_catalog(context, catalog_name.c_str());
		if (catalog == nullptr) {
			result_data[row_idx] = false;
			continue;
		}

		// Verify that this is a duck catalog
		auto catalog_type = duckdb_catalog_get_type_name(catalog);
		if (strcmp(catalog_type, "duckdb") != 0) {
			duckdb_destroy_catalog(&catalog);
			duckdb_destroy_client_context(&context);
			duckdb_scalar_function_set_error(info, "Catalog type is not duckdb");
			return;
		}

		auto schema_str = schema_data[row_idx];
		auto name_str = name_data[row_idx];

		string schema_name(duckdb_string_t_data(&schema_str), duckdb_string_t_length(schema_str));
		string name(duckdb_string_t_data(&name_str), duckdb_string_t_length(name_str));

		auto entry = duckdb_catalog_get_entry(catalog, context, DUCKDB_CATALOG_ENTRY_TYPE_TABLE, schema_name.c_str(),
		                                      name.c_str());

		// Check result
		result_data[row_idx] = (entry != nullptr);

		if (entry) {
			// Verify that we actually got what we expected
			auto entry_type = duckdb_catalog_entry_get_type(entry);
			if (entry_type != DUCKDB_CATALOG_ENTRY_TYPE_TABLE) {
				duckdb_destroy_catalog_entry(&entry);
				duckdb_destroy_catalog(&catalog);
				duckdb_destroy_client_context(&context);
				duckdb_scalar_function_set_error(info, "Catalog entry type is not TABLE");
				return;
			}

			auto entry_name = duckdb_catalog_entry_get_name(entry);
			if (strcmp(entry_name, name.c_str()) != 0) {
				duckdb_destroy_catalog_entry(&entry);
				duckdb_destroy_catalog(&catalog);
				duckdb_destroy_client_context(&context);
				duckdb_scalar_function_set_error(info, "Catalog entry name does not match");
				return;
			}
		}

		duckdb_destroy_catalog_entry(&entry);
		duckdb_destroy_catalog_entry(&entry);

		duckdb_destroy_catalog(&catalog);
		duckdb_destroy_catalog(&catalog);
	}

	duckdb_destroy_client_context(&context);
}

TEST_CASE("Test Catalog Interface in C API", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;

	REQUIRE(tester.OpenDatabase(nullptr));

	// Get the client context to pass as extra info
	duckdb_client_context context;
	duckdb_connection_get_client_context(tester.connection, &context);

	// We cant access the catalog outside of a transaction
	auto catalog = duckdb_client_context_get_catalog(context, "duckdb");
	REQUIRE(catalog == nullptr);

	duckdb_destroy_client_context(&context);
	duckdb_destroy_catalog(&catalog);

	// We need a scalar function to get the catalog (from within a transaction)
	auto func = duckdb_create_scalar_function();
	duckdb_scalar_function_set_name(func, "test_lookup_function");
	auto arg_type = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
	auto ret_type = duckdb_create_logical_type(DUCKDB_TYPE_BOOLEAN);
	duckdb_scalar_function_add_parameter(func, arg_type);
	duckdb_scalar_function_add_parameter(func, arg_type);
	duckdb_scalar_function_add_parameter(func, arg_type);
	duckdb_scalar_function_set_return_type(func, ret_type);
	duckdb_scalar_function_set_function(func, Execute);
	duckdb_scalar_function_set_extra_info(func, tester.connection, nullptr);

	REQUIRE(duckdb_register_scalar_function(tester.connection, func) == DuckDBSuccess);
	duckdb_destroy_scalar_function(&func);
	duckdb_destroy_logical_type(&ret_type);
	duckdb_destroy_logical_type(&arg_type);

	// Execute a query to test with non-existing catalog
	result = tester.Query("SELECT test_lookup_function('non_existing_catalog', 'main', 'test_table') AS exists;");
	REQUIRE_NO_FAIL(*result);

	// Execute a query to test the catalog interface with a non-existing schema
	result = tester.Query("SELECT test_lookup_function('memory', 'foobar', 'test_table') AS exists;");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<bool>(0, 0) == false);

	// Execute a query to test the catalog interface with a non-existing table
	result = tester.Query("SELECT test_lookup_function('memory', 'main', 'test_table') AS exists;");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<bool>(0, 0) == false);

	// Now create a table and test again
	REQUIRE_NO_FAIL(tester.Query("CREATE TABLE test_table(i INTEGER);"));

	result = tester.Query("SELECT test_lookup_function('memory', 'main', 'test_table') AS exists;");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<bool>(0, 0) == true);
}

static void LookupCollation(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
	auto conn = (duckdb_connection)duckdb_scalar_function_get_extra_info(info);
	duckdb_client_context context;
	duckdb_connection_get_client_context(conn, &context);
	auto catalog = duckdb_client_context_get_catalog(context, "system");

	auto input_size = duckdb_data_chunk_get_size(input);
	auto name_data = (duckdb_string_t *)duckdb_vector_get_data(duckdb_data_chunk_get_vector(input, 0));
	auto result_data = (bool *)duckdb_vector_get_data(output);
	for (idx_t row_idx = 0; row_idx < input_size; row_idx++) {
		auto name_str = name_data[row_idx];
		string name(duckdb_string_t_data(&name_str), duckdb_string_t_length(name_str));
		auto entry =
		    duckdb_catalog_get_entry(catalog, context, DUCKDB_CATALOG_ENTRY_TYPE_COLLATION, "main", name.c_str());
		// a collation is reported under its own name and type
		result_data[row_idx] = entry != nullptr &&
		                       duckdb_catalog_entry_get_type(entry) == DUCKDB_CATALOG_ENTRY_TYPE_COLLATION &&
		                       name == duckdb_catalog_entry_get_name(entry);
		duckdb_destroy_catalog_entry(&entry);
	}
	duckdb_destroy_catalog(&catalog);
	duckdb_destroy_client_context(&context);
}

TEST_CASE("Test collation lookup in the C API catalog interface", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;
	REQUIRE(tester.OpenDatabase(nullptr));

	auto func = duckdb_create_scalar_function();
	duckdb_scalar_function_set_name(func, "test_collation_lookup");
	auto arg_type = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
	auto ret_type = duckdb_create_logical_type(DUCKDB_TYPE_BOOLEAN);
	duckdb_scalar_function_add_parameter(func, arg_type);
	duckdb_scalar_function_set_return_type(func, ret_type);
	duckdb_scalar_function_set_function(func, LookupCollation);
	duckdb_scalar_function_set_extra_info(func, tester.connection, nullptr);
	REQUIRE(duckdb_register_scalar_function(tester.connection, func) == DuckDBSuccess);
	duckdb_destroy_scalar_function(&func);
	duckdb_destroy_logical_type(&ret_type);
	duckdb_destroy_logical_type(&arg_type);

	// a collation resolves to the scalar function that implements it, a missing one to NULL
	result = tester.Query("SELECT test_collation_lookup('nocase'), test_collation_lookup('does_not_exist')");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<bool>(0, 0) == true);
	REQUIRE(result->Fetch<bool>(1, 0) == false);
}

static void IntegerIdentity(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
	auto count = duckdb_data_chunk_get_size(input);
	auto input_data = (int32_t *)duckdb_vector_get_data(duckdb_data_chunk_get_vector(input, 0));
	auto result_data = (int32_t *)duckdb_vector_get_data(output);
	for (idx_t i = 0; i < count; i++) {
		result_data[i] = input_data[i];
	}
}

TEST_CASE("Test collate_ functions without a VARCHAR overload are not collations", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;
	REQUIRE(tester.OpenDatabase(nullptr));

	auto func = duckdb_create_scalar_function();
	duckdb_scalar_function_set_name(func, "collate_int");
	auto int_type = duckdb_create_logical_type(DUCKDB_TYPE_INTEGER);
	duckdb_scalar_function_add_parameter(func, int_type);
	duckdb_scalar_function_set_return_type(func, int_type);
	duckdb_scalar_function_set_function(func, IntegerIdentity);
	REQUIRE(duckdb_register_scalar_function(tester.connection, func) == DuckDBSuccess);
	duckdb_destroy_scalar_function(&func);
	duckdb_destroy_logical_type(&int_type);

	result = tester.Query("SELECT count(*) FROM pragma_collations() WHERE collname = 'int'");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<int64_t>(0, 0) == 0);

	result = tester.Query("SELECT 'a' COLLATE nocas = 'a'");
	REQUIRE_FAIL(result);
	REQUIRE_THAT(result->ErrorMessage(), Catch::Matchers::Contains("Did you mean \"nocase\""));
}
