#include "test_capi_v2.hpp"

#include <cstring>

// ---------------------------------------------------------------------------
// V2 custom type tests: bind a name to a base type, register it, and refer to
// it from SQL. A custom type shares its base type's representation, so values
// flow through the base type's accessors; only the name it goes by changes.
// ---------------------------------------------------------------------------

namespace test_capi_v2 {

namespace {

duckdb_v2_identifier_t TypeIdent(const char *s) {
	return Convert(s);
}

// Create a custom type on the connection with the given name and base type.
duckdb_v2_custom_type_handle MakeCustomType(duckdb_v2_connection_handle conn, const char *name,
                                            duckdb_v2_logical_type_handle base) {
	duckdb_v2_custom_type_handle type = nullptr;
	REQUIRE(duckdb_v2_custom_type_create(FactoryOf(conn), &type, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto name_str = TypeIdent(name);
	REQUIRE(duckdb_v2_custom_type_set_name(type, &name_str, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_custom_type_set_base_type(type, base, nullptr) == DUCKDB_V2_ERROR_NONE);
	return type;
}

// Run a query producing a single VARCHAR cell.
std::string QueryText(duckdb_v2_connection_handle conn, const char *sql) {
	duckdb_v2_result_handle result = nullptr;
	REQUIRE(Query(conn, sql, &result) == DUCKDB_V2_ERROR_NONE);
	auto chunk = StepChunk(result);
	REQUIRE(chunk != nullptr);
	duckdb_v2_vector_handle vec = nullptr;
	duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr);
	duckdb_v2_vector_view view {};
	duckdb_v2_vector_get_view(vec, &view, nullptr);
	auto out = Convert(Convert(static_cast<const duckdb_v2_bytes *>(view.data)[SelAt(view.sel, 0)]));
	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_result_destroy(&result);
	return out;
}

// Whether a query fails, either at bind time or while its (lazily executed) stream is stepped.
bool TypeQueryFails(duckdb_v2_connection_handle conn, const char *sql) {
	duckdb_v2_result_handle result = nullptr;
	auto rc = Query(conn, sql, &result);
	auto status = DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
	while (rc == DUCKDB_V2_ERROR_NONE) {
		duckdb_v2_data_chunk_handle chunk = nullptr;
		rc = duckdb_v2_result_step(result, &chunk, &status, nullptr);
		if (chunk) {
			duckdb_v2_data_chunk_destroy(&chunk);
		}
		if (rc != DUCKDB_V2_ERROR_NONE || status == DUCKDB_V2_RESULT_STEP_STATUS_FINISHED ||
		    status == DUCKDB_V2_RESULT_STEP_STATUS_CANCELLED) {
			break;
		}
		if (status == DUCKDB_V2_RESULT_STEP_STATUS_WAITING) {
			rc = duckdb_v2_result_wait(result, nullptr);
		}
	}
	duckdb_v2_result_destroy(&result);
	return rc != DUCKDB_V2_ERROR_NONE;
}

} // namespace

TEST_CASE("V2 custom type: register on connection and use in SQL", "[capi_v2][custom_type]") {
	EnvFixture fx;
	auto integer = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);

	auto type = MakeCustomType(fx.conn, "TEMPERATURE", integer);
	REQUIRE(duckdb_v2_connection_register_custom_type(fx.conn, type, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_custom_type_destroy(&type) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(type == nullptr);
	duckdb_v2_logical_type_destroy(&integer);

	// The name resolves in SQL, and a value of it reports the custom name rather than the base type's.
	REQUIRE(QueryText(fx.conn, "SELECT typeof(CAST(42 AS temperature))") == "TEMPERATURE");
	// The representation is the base type's, so the base type's operations still apply.
	REQUIRE(QueryText(fx.conn, "SELECT (CAST(42 AS temperature)::INTEGER + 1)::VARCHAR") == "43");
	// And it can be used as a column type.
	ExecSQL(fx.conn, "CREATE TABLE readings (t temperature)");
	ExecSQL(fx.conn, "INSERT INTO readings VALUES (7), (9)");
	REQUIRE(QueryText(fx.conn, "SELECT sum(t::INTEGER)::VARCHAR FROM readings") == "16");
	REQUIRE(QueryText(fx.conn, "SELECT typeof(t) FROM readings LIMIT 1") == "TEMPERATURE");
}

TEST_CASE("V2 custom type: base type keeps its parameters", "[capi_v2][custom_type]") {
	EnvFixture fx;

	// A parameterised base type carries its parameters into the custom type.
	auto decimal =
	    MakeType(fx.factory, "decimal", nullptr, {MakeInt32Value(fx.factory, 5), MakeInt32Value(fx.factory, 2)});
	auto type = MakeCustomType(fx.conn, "MONEY", decimal);
	REQUIRE(duckdb_v2_connection_register_custom_type(fx.conn, type, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_custom_type_destroy(&type);
	duckdb_v2_logical_type_destroy(&decimal);

	REQUIRE(QueryText(fx.conn, "SELECT typeof(CAST(1.5 AS money))") == "MONEY");
	// The base type's width and scale still apply.
	REQUIRE(QueryText(fx.conn, "SELECT CAST(1.005 AS money)::VARCHAR") == "1.01");
	REQUIRE(TypeQueryFails(fx.conn, "SELECT CAST(1000 AS money)"));
}

TEST_CASE("V2 custom type: registration refusals", "[capi_v2][custom_type]") {
	EnvFixture fx;
	auto integer = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	auto any = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_ANY);

	// No name.
	{
		duckdb_v2_custom_type_handle type = nullptr;
		REQUIRE(duckdb_v2_custom_type_create(fx.factory, &type, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_custom_type_set_base_type(type, integer, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_connection_register_custom_type(fx.conn, type, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
		duckdb_v2_custom_type_destroy(&type);
	}

	// No base type.
	{
		duckdb_v2_custom_type_handle type = nullptr;
		REQUIRE(duckdb_v2_custom_type_create(fx.factory, &type, nullptr) == DUCKDB_V2_ERROR_NONE);
		auto name_str = TypeIdent("no_base");
		REQUIRE(duckdb_v2_custom_type_set_name(type, &name_str, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_connection_register_custom_type(fx.conn, type, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
		duckdb_v2_custom_type_destroy(&type);
	}

	// ANY is a signature wildcard, not something a registered type can be built on.
	{
		auto type = MakeCustomType(fx.conn, "any_base", any);
		REQUIRE(duckdb_v2_connection_register_custom_type(fx.conn, type, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
		duckdb_v2_custom_type_destroy(&type);
	}

	duckdb_v2_logical_type_destroy(&integer);
	duckdb_v2_logical_type_destroy(&any);
}

TEST_CASE("V2 custom type: null arguments and destroy null-safety", "[capi_v2][custom_type]") {
	EnvFixture fx;
	auto integer = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);

	duckdb_v2_custom_type_handle type = nullptr;
	REQUIRE(duckdb_v2_custom_type_create(nullptr, &type, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(type == nullptr);
	REQUIRE(duckdb_v2_custom_type_create(fx.factory, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_connection_register_custom_type(nullptr, type, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	REQUIRE(duckdb_v2_custom_type_create(fx.factory, &type, nullptr) == DUCKDB_V2_ERROR_NONE);
	// A view with a null pointer but a non-zero length is rejected; the empty view is a legitimate (empty) name,
	// which registration then refuses.
	auto name_str = duckdb_v2_identifier_t {nullptr, 4};
	REQUIRE(duckdb_v2_custom_type_set_name(type, &name_str, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	auto name_str2 = TypeIdent(nullptr);
	REQUIRE(duckdb_v2_custom_type_set_name(type, &name_str2, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto name_str3 = TypeIdent("x");
	REQUIRE(duckdb_v2_custom_type_set_name(nullptr, &name_str3, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_custom_type_set_base_type(type, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_custom_type_set_base_type(nullptr, integer, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_connection_register_custom_type(fx.conn, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_custom_type_destroy(&type) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(type == nullptr);

	// Destroy is null-safe on both the pointer and the handle behind it.
	REQUIRE(duckdb_v2_custom_type_destroy(&type) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_custom_type_destroy(nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_logical_type_destroy(&integer);
}

} // namespace test_capi_v2
