#include "test_capi_v2.hpp"

namespace test_capi_v2 {

namespace {

duckdb_v2_logical_type_handle TypeFromText(duckdb_v2_factory_handle factory, const char *text) {
	duckdb_v2_logical_type_handle t = nullptr;
	auto text_str = Convert(text);
	REQUIRE(duckdb_v2_factory_create_type_from_text(factory, &text_str, &t, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(t != nullptr);
	return t;
}

struct TypeSet {
	std::vector<duckdb_v2_logical_type_handle> types;
	TypeSet(duckdb_v2_factory_handle factory, std::initializer_list<const char *> texts) {
		for (auto text : texts) {
			types.push_back(TypeFromText(factory, text));
		}
	}
	~TypeSet() {
		for (auto &t : types) {
			duckdb_v2_logical_type_destroy(&t);
		}
	}
};

DUCKDB_V2_ERROR CommonType(duckdb_v2_factory_handle factory, const std::vector<duckdb_v2_logical_type_handle> &types,
                           duckdb_v2_logical_type_handle *out_type) {
	return duckdb_v2_factory_create_common_type(factory, types.data(), types.size(), out_type, nullptr);
}

std::string CommonTypeText(duckdb_v2_factory_handle factory, std::initializer_list<const char *> texts) {
	TypeSet set(factory, texts);
	duckdb_v2_logical_type_handle result = nullptr;
	REQUIRE(CommonType(factory, set.types, &result) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(result != nullptr);
	auto text = Render(result);
	duckdb_v2_logical_type_destroy(&result);
	return text;
}

void CheckResolves(duckdb_v2_factory_handle factory) {
	REQUIRE(CommonTypeText(factory, {"INTEGER"}) == "INTEGER");
	REQUIRE(CommonTypeText(factory, {"INTEGER", "BIGINT"}) == "BIGINT");
	REQUIRE(CommonTypeText(factory, {"BIGINT", "INTEGER"}) == "BIGINT");
	REQUIRE(CommonTypeText(factory, {"TINYINT", "INTEGER", "DOUBLE"}) == "DOUBLE");
	REQUIRE(CommonTypeText(factory, {"DECIMAL(4,1)", "DECIMAL(5,4)"}) == "DECIMAL(7,4)");
	REQUIRE(CommonTypeText(factory, {"INTEGER[]", "BIGINT[]"}) == "BIGINT[]");
	REQUIRE(CommonTypeText(factory, {"STRUCT(a INTEGER)", "STRUCT(a BIGINT)"}) == "STRUCT(a BIGINT)");
}

} // namespace

TEST_CASE("V2: common_type resolves through a connection's factory", "[capi_v2][logical_type][common_type]") {
	EnvFixture fx;
	CheckResolves(fx.factory);
}

TEST_CASE("V2: common_type resolves through an instance's factory", "[capi_v2][logical_type][common_type]") {
	EnvFixture fx;
	duckdb_v2_factory_handle factory = nullptr;
	REQUIRE(duckdb_v2_instance_get_factory(fx.instance, &factory, nullptr) == DUCKDB_V2_ERROR_NONE);
	CheckResolves(factory);
}

TEST_CASE("V2: common_type agrees with value_create_list", "[capi_v2][logical_type][common_type]") {
	EnvFixture fx;
	auto small = MakeValueFromText(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_SMALLINT, "1");
	auto large = MakeValueFromText(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_UBIGINT, "2");
	duckdb_v2_value_handle elements[] = {small, large};
	duckdb_v2_value_handle list = nullptr;
	REQUIRE(duckdb_v2_value_create_list(fx.factory, nullptr, elements, 2, &list, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_logical_type_handle list_type = nullptr;
	REQUIRE(duckdb_v2_value_get_logical_type(list, &list_type, nullptr) == DUCKDB_V2_ERROR_NONE);

	auto common = CommonTypeText(fx.factory, {"SMALLINT", "UBIGINT"});
	REQUIRE(Render(list_type) == common + "[]");

	duckdb_v2_logical_type_destroy(&list_type);
	duckdb_v2_value_destroy(&list);
	duckdb_v2_value_destroy(&small);
	duckdb_v2_value_destroy(&large);
}

TEST_CASE("V2: common_type error paths", "[capi_v2][logical_type][common_type]") {
	EnvFixture fx;
	duckdb_v2_logical_type_handle result = nullptr;

	// No common type: the engine's error, with the out-param cleared.
	TypeSet incompatible(fx.factory, {"INTEGER", "BLOB"});
	result = reinterpret_cast<duckdb_v2_logical_type_handle>(&fx);
	REQUIRE(CommonType(fx.factory, incompatible.types, &result) == DUCKDB_V2_ERROR_QUERY_NOT_IMPLEMENTED);
	REQUIRE(result == nullptr);

	// An empty set has nothing to resolve.
	REQUIRE(duckdb_v2_factory_create_common_type(fx.factory, nullptr, 0, &result, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_factory_create_common_type(fx.factory, incompatible.types.data(), 0, &result, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_factory_create_common_type(fx.factory, nullptr, 2, &result, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);

	// A NULL entry.
	TypeSet one(fx.factory, {"INTEGER"});
	duckdb_v2_logical_type_handle with_null[] = {one.types[0], nullptr};
	REQUIRE(duckdb_v2_factory_create_common_type(fx.factory, with_null, 2, &result, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(result == nullptr);

	// ANY is a signature wildcard, not a data type.
	auto any = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_ANY);
	duckdb_v2_logical_type_handle with_any[] = {one.types[0], any};
	REQUIRE(duckdb_v2_factory_create_common_type(fx.factory, with_any, 2, &result, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_logical_type_destroy(&any);

	// Null factory / out-param.
	REQUIRE(duckdb_v2_factory_create_common_type(nullptr, one.types.data(), 1, &result, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_factory_create_common_type(fx.factory, one.types.data(), 1, nullptr, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
}

} // namespace test_capi_v2
