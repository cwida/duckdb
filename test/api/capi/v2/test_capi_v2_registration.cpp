#include "test_capi_v2.hpp"

// ---------------------------------------------------------------------------
// V2 registration tests: the scope registrations land in, and which database
// an object can be registered on.
// ---------------------------------------------------------------------------

namespace test_capi_v2 {

namespace {

bool Succeeds(duckdb_v2_connection_handle conn, const char *sql) {
	duckdb_v2_result_handle result = nullptr;
	auto rc = Query(conn, sql, &result, nullptr);
	if (rc == DUCKDB_V2_ERROR_NONE) {
		DrainRowCount(result);
	}
	duckdb_v2_result_destroy(&result);
	return rc == DUCKDB_V2_ERROR_NONE;
}

duckdb_v2_custom_type_handle MakeIntegerAlias(duckdb_v2_factory_handle factory, const char *name) {
	duckdb_v2_custom_type_handle type = nullptr;
	REQUIRE(duckdb_v2_custom_type_create(factory, &type, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto name_str = Convert(name);
	REQUIRE(duckdb_v2_custom_type_set_name(type, &name_str, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto integer = MakeType(factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	REQUIRE(duckdb_v2_custom_type_set_base_type(type, integer, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_logical_type_destroy(&integer);
	return type;
}

} // namespace

TEST_CASE("V2 registration: a connection's registrations join its transaction", "[capi_v2][registration]") {
	EnvFixture fx;
	ExecSQL(fx.conn, "BEGIN TRANSACTION");
	auto type = MakeIntegerAlias(fx.factory, "rolled_back_alias");
	REQUIRE(duckdb_v2_connection_register_custom_type(fx.conn, type, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_custom_type_destroy(&type);
	REQUIRE(Succeeds(fx.conn, "SELECT 1::rolled_back_alias"));
	ExecSQL(fx.conn, "ROLLBACK");
	REQUIRE_FALSE(Succeeds(fx.conn, "SELECT 1::rolled_back_alias"));
}

TEST_CASE("V2 registration: an object registers only on its factory's database", "[capi_v2][registration]") {
	EnvFixture fx;
	duckdb_v2_instance_handle other = nullptr;
	REQUIRE(OpenInstance(fx.env, duckdb_v2_str {nullptr, 0}, &other, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_connection_handle other_conn = nullptr;
	REQUIRE(duckdb_v2_connection_create(other, &other_conn, nullptr) == DUCKDB_V2_ERROR_NONE);

	auto type = MakeIntegerAlias(fx.factory, "foreign_alias");
	duckdb_v2_error_info_handle err = nullptr;
	REQUIRE(duckdb_v2_connection_register_custom_type(other_conn, type, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_str msg = {nullptr, 0};
	duckdb_v2_error_info_get_text(err, &msg);
	REQUIRE(Convert(msg).find("different database") != std::string::npos);
	duckdb_v2_error_info_destroy(&err);

	// The same object still registers on its own database.
	REQUIRE(duckdb_v2_connection_register_custom_type(fx.conn, type, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_custom_type_destroy(&type);
	duckdb_v2_connection_destroy(&other_conn);
	duckdb_v2_instance_destroy(&other);
}

} // namespace test_capi_v2
