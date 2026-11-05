#include "capi_tester.hpp"

using namespace duckdb;

TEST_CASE("Test Custom Configuration Options in C API", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;

	// Create a connection and get client context
	REQUIRE(tester.OpenDatabase(nullptr));
	duckdb_client_context context;
	duckdb_connection_get_client_context(tester.connection, &context);
	REQUIRE(context != nullptr);

	// Setup some types for config options
	auto str_type = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
	auto int_type = duckdb_create_logical_type(DUCKDB_TYPE_INTEGER);

	auto default_str_value = duckdb_create_varchar("default_string");
	auto default_int_value = duckdb_create_int32(42);

	// Test 1: Create a string config option
	auto str_opt = duckdb_create_config_option();
	REQUIRE(str_opt != nullptr);
	duckdb_config_option_set_name(str_opt, "test_string_option");
	duckdb_config_option_set_type(str_opt, str_type);
	duckdb_config_option_set_description(str_opt, "A test string configuration option");
	duckdb_config_option_set_default_value(str_opt, default_str_value);
	REQUIRE(duckdb_register_config_option(tester.connection, str_opt) == DuckDBSuccess);

	// Get and verify the string option
	result = tester.Query("SELECT current_setting('test_string_option')");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<string>(0, 0) == "default_string");

	// Set and verify the string option
	REQUIRE_NO_FAIL(tester.Query("SET test_string_option = 'new_value'"));
	result = tester.Query("SELECT current_setting('test_string_option')");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<string>(0, 0) == "new_value");

	// Also get it from the client context
	duckdb_config_option_scope scope;
	auto retrieved_str = duckdb_client_context_get_config_option(context, "test_string_option", &scope);
	REQUIRE(retrieved_str != nullptr);
	duckdb_destroy_value(&retrieved_str);

	// By default, the scope should be SESSION
	REQUIRE(scope == DUCKDB_CONFIG_OPTION_SCOPE_SESSION);

	duckdb_destroy_config_option(&str_opt);
	duckdb_destroy_config_option(&str_opt);

	// Fetch it from the duckdb_settings() function
	result =
	    tester.Query("SELECT name, value, input_type, scope FROM duckdb_settings() WHERE name = 'test_string_option'");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->ChunkCount() == 1);
	REQUIRE(result->Fetch<string>(0, 0) == "test_string_option");
	REQUIRE(result->Fetch<string>(1, 0) == "new_value");
	REQUIRE(result->Fetch<string>(2, 0) == "VARCHAR");
	REQUIRE(result->Fetch<string>(3, 0) == "LOCAL");

	// Test 2: Create an integer config option, with the type inferred from the default value
	auto int_opt = duckdb_create_config_option();
	REQUIRE(int_opt != nullptr);
	duckdb_config_option_set_name(int_opt, "test_integer_option");
	duckdb_config_option_set_description(int_opt, "A test integer configuration option");
	duckdb_config_option_set_default_value(int_opt, default_int_value);
	duckdb_config_option_set_default_scope(int_opt, DUCKDB_CONFIG_OPTION_SCOPE_GLOBAL);
	REQUIRE(duckdb_register_config_option(tester.connection, int_opt) == DuckDBSuccess);

	// Get and verify the integer option
	result = tester.Query("SELECT current_setting('test_integer_option')");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<int32_t>(0, 0) == 42);

	// Also get it from the client context. Try without specifying scope first
	auto retrieved_int = duckdb_client_context_get_config_option(context, "test_integer_option", nullptr);
	REQUIRE(retrieved_int != nullptr);
	REQUIRE(duckdb_get_int32(retrieved_int) == 42);
	duckdb_destroy_value(&retrieved_int);

	// By default, the scope should be GLOBAL, as that was what we defined this option with
	retrieved_int = duckdb_client_context_get_config_option(context, "test_integer_option", &scope);
	REQUIRE(retrieved_int != nullptr);
	duckdb_destroy_value(&retrieved_int);

	// Also check in duckdb_settings()
	result =
	    tester.Query("SELECT name, value, input_type, scope FROM duckdb_settings() WHERE name = 'test_integer_option'");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->ChunkCount() == 1);
	REQUIRE(result->Fetch<string>(0, 0) == "test_integer_option");
	REQUIRE(result->Fetch<string>(1, 0) == "42");
	REQUIRE(result->Fetch<string>(2, 0) == "INTEGER");
	REQUIRE(result->Fetch<string>(3, 0) == "GLOBAL");

	duckdb_destroy_config_option(&int_opt);
	duckdb_destroy_config_option(&int_opt);

	// Cleanup
	duckdb_destroy_client_context(&context);
	duckdb_destroy_logical_type(&str_type);
	duckdb_destroy_logical_type(&int_type);

	duckdb_destroy_value(&default_str_value);
	duckdb_destroy_value(&default_int_value);
}

TEST_CASE("Issue #25056: duckdb_set_config crashes on a boolean option set to false", "[capi]") {
	// debug_force_mbedtls_unsafe consults the attached databases when it is disabled, but there is no
	// database instance yet when the option is set on a config
	for (const auto &alias : {"force_mbedtls_unsafe", "debug_force_mbedtls_unsafe"}) {
		for (const auto &value : {"true", "false", "1", "0"}) {
			duckdb_config config;
			REQUIRE(duckdb_create_config(&config) == DuckDBSuccess);
			REQUIRE(duckdb_set_config(config, alias, value) == DuckDBSuccess);
			duckdb_destroy_config(&config);
		}
	}
}
