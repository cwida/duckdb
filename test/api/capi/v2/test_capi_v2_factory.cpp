#include "test_capi_v2.hpp"

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// V2 factory tests: where a factory comes from, and what it resolves in the
// scope of an instance (built-ins only) versus a connection or a context.
// ---------------------------------------------------------------------------

namespace test_capi_v2 {

namespace {

// An instance that has not been started: no database attached, no connection.
struct UnstartedInstance {
	duckdb_v2_environment_handle env = nullptr;
	duckdb_v2_instance_handle instance = nullptr;
	duckdb_v2_factory_handle factory = nullptr;
	UnstartedInstance() {
		duckdb_v2_environment_create(&env, nullptr);
		duckdb_v2_instance_create(env, &instance, nullptr);
		duckdb_v2_instance_get_factory(instance, &factory, nullptr);
	}
	~UnstartedInstance() {
		duckdb_v2_instance_destroy(&instance);
		duckdb_v2_environment_destroy(&env);
	}
};

DUCKDB_V2_ERROR TypeFromText(duckdb_v2_factory_handle factory, const char *text,
                             duckdb_v2_logical_type_handle *out_type) {
	auto text_str = Convert(text);
	return duckdb_v2_factory_create_type_from_text(factory, &text_str, out_type, nullptr);
}

DUCKDB_V2_ERROR TypeFromName(duckdb_v2_factory_handle factory, const std::vector<const char *> &parts,
                             duckdb_v2_logical_type_handle *out_type) {
	std::vector<duckdb_v2_identifier_t> part_views;
	for (auto *part : parts) {
		part_views.push_back(Convert(part));
	}
	duckdb_v2_qname_handle qname = nullptr;
	REQUIRE(duckdb_v2_qname_create(part_views.data(), part_views.size(), &qname, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto rc = duckdb_v2_factory_create_type_from_name(factory, qname, nullptr, nullptr, 0, out_type, nullptr);
	duckdb_v2_qname_destroy(&qname);
	return rc;
}

} // namespace

TEST_CASE("V2 factory: getters borrow one factory per source", "[capi_v2][factory]") {
	EnvFixture fx;
	duckdb_v2_factory_handle again = nullptr;
	REQUIRE(duckdb_v2_connection_get_factory(fx.conn, &again, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(again == fx.factory);

	duckdb_v2_factory_handle from_instance = nullptr;
	REQUIRE(duckdb_v2_instance_get_factory(fx.instance, &from_instance, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(from_instance != nullptr);
	REQUIRE(from_instance != fx.factory);

	duckdb_v2_factory_handle out = nullptr;
	REQUIRE(duckdb_v2_instance_get_factory(nullptr, &out, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_connection_get_factory(nullptr, &out, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_context_get_factory(nullptr, &out, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_connection_get_factory(fx.conn, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(out == nullptr);
}

TEST_CASE("V2 factory: an instance's factory creates built-in types and values", "[capi_v2][factory]") {
	UnstartedInstance fx;

	// Parameterless and parameterized built-ins, by id, by name and from text.
	auto integer = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	REQUIRE(Render(integer) == "INTEGER");
	auto decimal =
	    MakeType(fx.factory, "decimal", nullptr, {MakeInt32Value(fx.factory, 18), MakeInt32Value(fx.factory, 3)});
	REQUIRE(Render(decimal) == "DECIMAL(18,3)");
	duckdb_v2_logical_type_handle nested = nullptr;
	REQUIRE(TypeFromText(fx.factory, "STRUCT(a INTEGER, b VARCHAR[])", &nested) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(Render(nested) == "STRUCT(a INTEGER, b VARCHAR[])");

	// Element types are inferred with the built-in rules.
	duckdb_v2_value_handle children[2] = {MakeInt32Value(fx.factory, 1), MakeInt64Value(fx.factory, 2)};
	duckdb_v2_value_handle list = nullptr;
	REQUIRE(duckdb_v2_value_create_list(fx.factory, nullptr, children, 2, &list, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(Render(list) == "[1, 2]");
	duckdb_v2_logical_type_handle list_type = nullptr;
	REQUIRE(duckdb_v2_value_get_logical_type(list, &list_type, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(Render(list_type) == "BIGINT[]");

	// Built-in casts work.
	auto text = MakeVarcharValue(fx.factory, "42");
	duckdb_v2_value_handle cast = nullptr;
	REQUIRE(duckdb_v2_value_cast(fx.factory, text, integer, &cast, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(Render(cast) == "42");

	// Chunks and collections are allocated through the instance, which starts it.
	duckdb_v2_logical_type_handle types[1] = {integer};
	duckdb_v2_data_chunk_handle chunk = nullptr;
	REQUIRE(duckdb_v2_data_chunk_create(fx.factory, types, 1, &chunk, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_column_data_collection_handle collection = nullptr;
	REQUIRE(duckdb_v2_column_data_collection_create(fx.factory, types, 1, &collection, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);

	duckdb_v2_column_data_collection_destroy(&collection);
	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_value_destroy(&cast);
	duckdb_v2_value_destroy(&text);
	duckdb_v2_logical_type_destroy(&list_type);
	duckdb_v2_value_destroy(&list);
	for (auto &child : children) {
		duckdb_v2_value_destroy(&child);
	}
	duckdb_v2_logical_type_destroy(&nested);
	duckdb_v2_logical_type_destroy(&decimal);
	duckdb_v2_logical_type_destroy(&integer);
}

TEST_CASE("V2 factory: an instance's factory refuses what needs a catalog", "[capi_v2][factory]") {
	EnvFixture fx;
	ExecSQL(fx.conn, "CREATE TYPE mood AS ENUM ('happy', 'sad')");
	duckdb_v2_factory_handle instance_factory = nullptr;
	REQUIRE(duckdb_v2_instance_get_factory(fx.instance, &instance_factory, nullptr) == DUCKDB_V2_ERROR_NONE);

	// The connection's factory resolves the catalog type; the instance's does not.
	duckdb_v2_logical_type_handle t = nullptr;
	REQUIRE(TypeFromText(fx.factory, "mood", &t) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_logical_type_destroy(&t);
	REQUIRE(TypeFromName(fx.factory, {"mood"}, &t) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_logical_type_destroy(&t);

	REQUIRE(TypeFromText(instance_factory, "mood", &t) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(t == nullptr);
	REQUIRE(TypeFromText(instance_factory, "STRUCT(m mood)", &t) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(TypeFromName(instance_factory, {"mood"}, &t) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(TypeFromName(instance_factory, {"memory", "main", "mood"}, &t) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(t == nullptr);
}

TEST_CASE("V2 factory: a connection's factory works inside an open transaction", "[capi_v2][factory]") {
	EnvFixture fx;
	ExecSQL(fx.conn, "BEGIN TRANSACTION");
	ExecSQL(fx.conn, "CREATE TYPE mood AS ENUM ('happy', 'sad')");

	// The type created in the open transaction is visible to the connection's factory.
	duckdb_v2_logical_type_handle mood = nullptr;
	REQUIRE(TypeFromText(fx.factory, "mood", &mood) == DUCKDB_V2_ERROR_NONE);
	auto text = MakeVarcharValue(fx.factory, "sad");
	duckdb_v2_value_handle cast = nullptr;
	REQUIRE(duckdb_v2_value_cast(fx.factory, text, mood, &cast, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(Render(cast) == "sad");
	ExecSQL(fx.conn, "ROLLBACK");

	duckdb_v2_value_destroy(&cast);
	duckdb_v2_value_destroy(&text);
	duckdb_v2_logical_type_destroy(&mood);
}

} // namespace test_capi_v2
