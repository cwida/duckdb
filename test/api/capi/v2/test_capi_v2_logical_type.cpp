#include "test_capi_v2.hpp"

// ---------------------------------------------------------------------------
// V2 logical_type tests.
//
// Fixtures are V2-native: primitives via create_from_id, composites via
// logical_type resolution from a connection (MakeType and its sugar),
// aliases via logical_type_create_with_alias. V1 appears here only as
// deliberate interop validation: two cross-version round-trip pins, one
// V1-built decimal oracle, and the V1-only zero-entry enum (invalid in SQL,
// so V2 refuses to build it; V2 inspection degrades gracefully), plus the
// MakeV1* seed builders those pins use.
//
// V1 and V2 logical_type handles have different representations: V1 seeds are
// converted with ConvertToV2, which consumes the V1 handle. We do NOT pass
// V2-built handles into V1 functions; the direction here is one-way V1 -> V2
// for fixture setup only.
// ---------------------------------------------------------------------------

namespace test_capi_v2 {

// The type name is a qualified name; every case here names an unqualified type, so it is wrapped in a one-part name.
inline DUCKDB_V2_ERROR LTCreateTypeFromName(duckdb_v2_factory_handle factory, duckdb_v2_identifier_t name,
                                            const duckdb_v2_identifier_t *param_names,
                                            const duckdb_v2_value_handle *param_values, idx_t param_count,
                                            duckdb_v2_logical_type_handle *out_type, duckdb_v2_error_info_handle *err) {
	duckdb_v2_identifier_t parts[1] = {name};
	duckdb_v2_qname_handle qname = nullptr;
	auto rc = duckdb_v2_qname_create(parts, 1, &qname, err);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		if (out_type) {
			*out_type = nullptr;
		}
		return rc;
	}
	rc = duckdb_v2_factory_create_type_from_name(factory, qname, param_names, param_values, param_count, out_type, err);
	duckdb_v2_qname_destroy(&qname);
	return rc;
}

// ===========================================================================
// Lifecycle: create_from_id / destroy
// ===========================================================================

TEST_CASE("V2: logical_type create_from_id primitives", "[capi_v2][logical_type][lifecycle]") {
	EnvFixture fx;
	struct {
		DUCKDB_V2_LOGICAL_TYPE_ID id;
	} cases[] = {
	    {DUCKDB_V2_LOGICAL_TYPE_ID_BOOLEAN},
	    {DUCKDB_V2_LOGICAL_TYPE_ID_TINYINT},
	    {DUCKDB_V2_LOGICAL_TYPE_ID_SMALLINT},
	    {DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER},
	    {DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT},
	    {DUCKDB_V2_LOGICAL_TYPE_ID_UTINYINT},
	    {DUCKDB_V2_LOGICAL_TYPE_ID_USMALLINT},
	    {DUCKDB_V2_LOGICAL_TYPE_ID_UINTEGER},
	    {DUCKDB_V2_LOGICAL_TYPE_ID_UBIGINT},
	    {DUCKDB_V2_LOGICAL_TYPE_ID_HUGEINT},
	    {DUCKDB_V2_LOGICAL_TYPE_ID_UHUGEINT},
	    {DUCKDB_V2_LOGICAL_TYPE_ID_FLOAT},
	    {DUCKDB_V2_LOGICAL_TYPE_ID_DOUBLE},
	    {DUCKDB_V2_LOGICAL_TYPE_ID_DATE},
	    {DUCKDB_V2_LOGICAL_TYPE_ID_TIME},
	    {DUCKDB_V2_LOGICAL_TYPE_ID_TIME_TZ},
	    {DUCKDB_V2_LOGICAL_TYPE_ID_TIME_NS},
	    {DUCKDB_V2_LOGICAL_TYPE_ID_TIMESTAMP_SEC},
	    {DUCKDB_V2_LOGICAL_TYPE_ID_TIMESTAMP_MS},
	    {DUCKDB_V2_LOGICAL_TYPE_ID_TIMESTAMP},
	    {DUCKDB_V2_LOGICAL_TYPE_ID_TIMESTAMP_NS},
	    {DUCKDB_V2_LOGICAL_TYPE_ID_TIMESTAMP_TZ},
	    {DUCKDB_V2_LOGICAL_TYPE_ID_TIMESTAMP_TZ_NS},
	    {DUCKDB_V2_LOGICAL_TYPE_ID_INTERVAL},
	    {DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR},
	    {DUCKDB_V2_LOGICAL_TYPE_ID_BLOB},
	    {DUCKDB_V2_LOGICAL_TYPE_ID_BIT},
	    {DUCKDB_V2_LOGICAL_TYPE_ID_BIGNUM},
	    {DUCKDB_V2_LOGICAL_TYPE_ID_UUID},
	};
	for (auto &c : cases) {
		duckdb_v2_logical_type_handle type = nullptr;
		REQUIRE(duckdb_v2_factory_create_type_from_id(fx.factory, c.id, nullptr, nullptr, 0, &type, nullptr) ==
		        DUCKDB_V2_ERROR_NONE);
		REQUIRE(type != nullptr);
		DUCKDB_V2_LOGICAL_TYPE_ID round_trip = DUCKDB_V2_LOGICAL_TYPE_ID_INVALID;
		REQUIRE(duckdb_v2_logical_type_get_id(type, &round_trip, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(round_trip == c.id);
		REQUIRE(duckdb_v2_logical_type_destroy(&type) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(type == nullptr);
	}
}

TEST_CASE("V2: logical_type create_from_id rejects parameterised ids with no parameters",
          "[capi_v2][logical_type][lifecycle]") {
	EnvFixture fx;
	DUCKDB_V2_LOGICAL_TYPE_ID rejected[] = {
	    DUCKDB_V2_LOGICAL_TYPE_ID_DECIMAL,  DUCKDB_V2_LOGICAL_TYPE_ID_LIST, DUCKDB_V2_LOGICAL_TYPE_ID_STRUCT,
	    DUCKDB_V2_LOGICAL_TYPE_ID_TUPLE,    DUCKDB_V2_LOGICAL_TYPE_ID_MAP,  DUCKDB_V2_LOGICAL_TYPE_ID_ARRAY,
	    DUCKDB_V2_LOGICAL_TYPE_ID_UNION,    DUCKDB_V2_LOGICAL_TYPE_ID_ENUM, DUCKDB_V2_LOGICAL_TYPE_ID_VARIANT,
	    DUCKDB_V2_LOGICAL_TYPE_ID_GEOMETRY,
	};
	for (auto id : rejected) {
		duckdb_v2_logical_type_handle type = nullptr;
		duckdb_v2_error_info_handle err = nullptr;
		REQUIRE(duckdb_v2_factory_create_type_from_id(fx.factory, id, nullptr, nullptr, 0, &type, &err) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		REQUIRE(type == nullptr);
		REQUIRE(err != nullptr);
		duckdb_v2_error_info_destroy(&err);
	}
}

TEST_CASE("V2: logical_type create_from_id rejects sentinel and bind-time-only ids",
          "[capi_v2][logical_type][lifecycle]") {
	EnvFixture fx;
	// INVALID is the zero sentinel; SQLNULL / UNKNOWN exist only for the planner
	// and UDF binding paths. ANY is deliberately NOT in this list: it is the one
	// bind-time id made constructible (a function-signature wildcard passed to
	// parameter / varargs setters), gated instead at data-creating surfaces.
	DUCKDB_V2_LOGICAL_TYPE_ID rejected[] = {
	    DUCKDB_V2_LOGICAL_TYPE_ID_INVALID,
	    DUCKDB_V2_LOGICAL_TYPE_ID_SQLNULL,
	    DUCKDB_V2_LOGICAL_TYPE_ID_UNKNOWN,
	};
	for (auto id : rejected) {
		duckdb_v2_logical_type_handle type = nullptr;
		duckdb_v2_error_info_handle err = nullptr;
		REQUIRE(duckdb_v2_factory_create_type_from_id(fx.factory, id, nullptr, nullptr, 0, &type, &err) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		REQUIRE(type == nullptr);
		REQUIRE(err != nullptr);
		duckdb_v2_error_info_destroy(&err);
	}
}

TEST_CASE("V2: logical_type create_from_id binds the parameterised kinds", "[capi_v2][logical_type][lifecycle]") {
	// With parameters the id resolves to its canonical name and binds through
	// the same path as create_type_from_name: same type, either way in.
	EnvFixture fx;

	auto by_id = [&](DUCKDB_V2_LOGICAL_TYPE_ID id, const std::vector<const char *> *names,
	                 std::vector<duckdb_v2_value_handle> values) {
		std::vector<duckdb_v2_str> name_views;
		if (names) {
			for (auto *n : *names) {
				name_views.push_back(Convert(n));
			}
		}
		duckdb_v2_logical_type_handle t = nullptr;
		auto rc = duckdb_v2_factory_create_type_from_id(fx.factory, id, names ? name_views.data() : nullptr,
		                                                values.data(), values.size(), &t, nullptr);
		for (auto &v : values) {
			duckdb_v2_value_destroy(&v);
		}
		REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
		REQUIRE(t != nullptr);
		return t;
	};

	auto dec = by_id(DUCKDB_V2_LOGICAL_TYPE_ID_DECIMAL, nullptr,
	                 {MakeInt32Value(fx.factory, 18), MakeInt32Value(fx.factory, 3)});
	REQUIRE(Render(dec) == "DECIMAL(18,3)");
	duckdb_v2_logical_type_destroy(&dec);

	auto list =
	    by_id(DUCKDB_V2_LOGICAL_TYPE_ID_LIST, nullptr, {MakeTypeValue(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER)});
	REQUIRE(Render(list) == "INTEGER[]");
	duckdb_v2_logical_type_destroy(&list);

	auto arr = by_id(DUCKDB_V2_LOGICAL_TYPE_ID_ARRAY, nullptr,
	                 {MakeTypeValue(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR), MakeInt32Value(fx.factory, 3)});
	REQUIRE(Render(arr) == "VARCHAR[3]");
	duckdb_v2_logical_type_destroy(&arr);

	auto map = by_id(DUCKDB_V2_LOGICAL_TYPE_ID_MAP, nullptr,
	                 {MakeTypeValue(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR),
	                  MakeTypeValue(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER)});
	REQUIRE(Render(map) == "MAP(VARCHAR, INTEGER)");
	duckdb_v2_logical_type_destroy(&map);

	std::vector<const char *> field_names = {"id", "name"};
	auto st = by_id(DUCKDB_V2_LOGICAL_TYPE_ID_STRUCT, &field_names,
	                {MakeTypeValue(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER),
	                 MakeTypeValue(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR)});
	REQUIRE(Render(st) == "STRUCT(id INTEGER, \"name\" VARCHAR)");
	duckdb_v2_logical_type_destroy(&st);

	auto e = by_id(DUCKDB_V2_LOGICAL_TYPE_ID_ENUM, nullptr,
	               {MakeVarcharValue(fx.factory, "sad"), MakeVarcharValue(fx.factory, "happy")});
	REQUIRE(Render(e) == "ENUM('sad', 'happy')");
	duckdb_v2_logical_type_destroy(&e);

	// The id form and the name form land on the same type.
	auto from_name =
	    MakeType(fx.factory, "decimal", nullptr, {MakeInt32Value(fx.factory, 18), MakeInt32Value(fx.factory, 3)});
	auto from_id = by_id(DUCKDB_V2_LOGICAL_TYPE_ID_DECIMAL, nullptr,
	                     {MakeInt32Value(fx.factory, 18), MakeInt32Value(fx.factory, 3)});
	bool equal = false;
	REQUIRE(duckdb_v2_logical_type_is_equal(from_name, from_id, &equal, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(equal);
	duckdb_v2_logical_type_destroy(&from_name);
	duckdb_v2_logical_type_destroy(&from_id);
}

TEST_CASE("V2: logical_type create_from_id parameter validation", "[capi_v2][logical_type][lifecycle]") {
	EnvFixture fx;
	duckdb_v2_logical_type_handle out = nullptr;

	// A primitive id takes no parameters.
	duckdb_v2_value_handle one[1] = {MakeInt32Value(fx.factory, 1)};
	REQUIRE(duckdb_v2_factory_create_type_from_id(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER, nullptr, one, 1, &out,
	                                              nullptr) == DUCKDB_V2_ERROR_QUERY_BINDER);
	REQUIRE(out == nullptr);
	duckdb_v2_identifier_t invalid_name[1] = {Convert("\x80")};
	REQUIRE(duckdb_v2_factory_create_type_from_id(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER, invalid_name, one, 1,
	                                              &out, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	duckdb_v2_value_destroy(&one[0]);

	// Bind errors surface: DECIMAL width 0 is out of range.
	duckdb_v2_value_handle zero[1] = {MakeInt32Value(fx.factory, 0)};
	REQUIRE(duckdb_v2_factory_create_type_from_id(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_DECIMAL, nullptr, zero, 1, &out,
	                                              nullptr) == DUCKDB_V2_ERROR_QUERY_BINDER);
	REQUIRE(out == nullptr);
	duckdb_v2_value_destroy(&zero[0]);

	// param_count > 0 with no value array, and a hole inside one.
	REQUIRE(duckdb_v2_factory_create_type_from_id(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_LIST, nullptr, nullptr, 1, &out,
	                                              nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_value_handle holed[1] = {nullptr};
	REQUIRE(duckdb_v2_factory_create_type_from_id(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_LIST, nullptr, holed, 1, &out,
	                                              nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	// A null connection is refused before anything else.
	REQUIRE(duckdb_v2_factory_create_type_from_id(nullptr, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER, nullptr, nullptr, 0, &out,
	                                              nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
}

TEST_CASE("V2: logical_type create_from_id null out param", "[capi_v2][logical_type][lifecycle]") {
	EnvFixture fx;
	REQUIRE(duckdb_v2_factory_create_type_from_id(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER, nullptr, nullptr, 0,
	                                              nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
}

TEST_CASE("V2: logical_type create_from_id leaves pre-existing err untouched on success",
          "[capi_v2][logical_type][lifecycle]") {
	EnvFixture fx;
	// Belt-and-braces check of the error-info contract: on success the
	// library leaves the slot untouched. A stale info from a prior failure
	// survives; the return code is authoritative.
	duckdb_v2_error_info_handle err = nullptr;
	REQUIRE(duckdb_v2_factory_create_type_from_id(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_DECIMAL, nullptr, nullptr, 0,
	                                              nullptr, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(err != nullptr);

	duckdb_v2_logical_type_handle t = nullptr;
	REQUIRE(duckdb_v2_factory_create_type_from_id(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER, nullptr, nullptr, 0,
	                                              &t, &err) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(err != nullptr);
	DUCKDB_V2_ERROR code = DUCKDB_V2_ERROR_NONE;
	duckdb_v2_error_info_get_code(err, &code);
	REQUIRE(code == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_error_info_destroy(&err);
	duckdb_v2_logical_type_destroy(&t);
}

TEST_CASE("V2: logical_type destroy is null-safe", "[capi_v2][logical_type][lifecycle]") {
	// Passing a nullptr slot pointer is a no-op.
	REQUIRE(duckdb_v2_logical_type_destroy(nullptr) == DUCKDB_V2_ERROR_NONE);
	// Passing a slot that already holds nullptr is a no-op.
	duckdb_v2_logical_type_handle already_null = nullptr;
	REQUIRE(duckdb_v2_logical_type_destroy(&already_null) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(already_null == nullptr);
}

// ===========================================================================
// Common introspection: get_id, get_name
// ===========================================================================

TEST_CASE("V2: logical_type get_id null handle / null out", "[capi_v2][logical_type][id]") {
	EnvFixture fx;
	DUCKDB_V2_LOGICAL_TYPE_ID id = DUCKDB_V2_LOGICAL_TYPE_ID_BOOLEAN;
	REQUIRE(duckdb_v2_logical_type_get_id(nullptr, &id, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	duckdb_v2_logical_type_handle t = nullptr;
	duckdb_v2_factory_create_type_from_id(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER, nullptr, nullptr, 0, &t,
	                                      nullptr);
	REQUIRE(duckdb_v2_logical_type_get_id(t, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_logical_type_destroy(&t);
}

TEST_CASE("V2: logical_type get_name is the canonical id name when no alias is set", "[capi_v2][logical_type][name]") {
	// Never the empty view: the un-aliased arm returns the id's canonical
	// fixed name, exactly the vocabulary logical_type_create consumes.
	EnvFixture fx;
	duckdb_v2_logical_type_handle t = nullptr;
	duckdb_v2_factory_create_type_from_id(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER, nullptr, nullptr, 0, &t,
	                                      nullptr);
	duckdb_v2_str name = {nullptr, 0};
	REQUIRE(duckdb_v2_logical_type_get_name(t, &name, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(name == "INTEGER");
	duckdb_v2_logical_type_destroy(&t);

	auto dec =
	    MakeType(fx.factory, "decimal", nullptr, {MakeInt32Value(fx.factory, 18), MakeInt32Value(fx.factory, 3)});
	REQUIRE(duckdb_v2_logical_type_get_name(dec, &name, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(name == "DECIMAL");
	duckdb_v2_logical_type_destroy(&dec);

	// The spaced canonical spellings come through verbatim.
	duckdb_v2_factory_create_type_from_id(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_TIMESTAMP_TZ, nullptr, nullptr, 0, &t,
	                                      nullptr);
	REQUIRE(duckdb_v2_logical_type_get_name(t, &name, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(name == "TIMESTAMP WITH TIME ZONE");
	REQUIRE(name.len > 0);
	duckdb_v2_logical_type_destroy(&t);
}

TEST_CASE("V2: logical_type get_name prefers the alias when set", "[capi_v2][logical_type][name]") {
	EnvFixture fx;
	auto base = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_logical_type_handle t = nullptr;
	auto alias_name_str = Convert("my_int");
	auto alias_rc = duckdb_v2_factory_create_type_with_alias(fx.factory, base, &alias_name_str, &t, nullptr);
	duckdb_v2_logical_type_destroy(&base);
	REQUIRE(alias_rc == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_str alias = {nullptr, 0};
	REQUIRE(duckdb_v2_logical_type_get_name(t, &alias, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(alias == "my_int");

	// Borrowed pointer is valid as long as the type is alive: a second call
	// returns the same string contents.
	duckdb_v2_str alias2 = {nullptr, 0};
	duckdb_v2_logical_type_get_name(t, &alias2, nullptr);
	REQUIRE(alias2 == "my_int");

	duckdb_v2_logical_type_destroy(&t);
}

TEST_CASE("V2: logical_type get_name reads an alias set on a STRUCT", "[capi_v2][logical_type][name]") {
	// Mirrors the spatial / extension-type path: a composite with an alias
	// (e.g. STRUCT{x:double, y:double} aliased "POINT_2D").
	EnvFixture fx;
	auto base =
	    MakeStructType(fx.factory, {"x", "y"}, {DUCKDB_V2_LOGICAL_TYPE_ID_DOUBLE, DUCKDB_V2_LOGICAL_TYPE_ID_DOUBLE});
	duckdb_v2_logical_type_handle t = nullptr;
	auto alias_name_str = Convert("POINT_2D");
	auto alias_rc = duckdb_v2_factory_create_type_with_alias(fx.factory, base, &alias_name_str, &t, nullptr);
	duckdb_v2_logical_type_destroy(&base);
	REQUIRE(alias_rc == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_str alias = {nullptr, 0};
	REQUIRE(duckdb_v2_logical_type_get_name(t, &alias, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(alias == "POINT_2D");

	// The id is still STRUCT — alias is metadata, not type identity.
	DUCKDB_V2_LOGICAL_TYPE_ID id = DUCKDB_V2_LOGICAL_TYPE_ID_INVALID;
	duckdb_v2_logical_type_get_id(t, &id, nullptr);
	REQUIRE(id == DUCKDB_V2_LOGICAL_TYPE_ID_STRUCT);

	duckdb_v2_logical_type_destroy(&t);
}

TEST_CASE("V2: logical_type get_name null handle / null out", "[capi_v2][logical_type][name]") {
	EnvFixture fx;
	duckdb_v2_str alias = {nullptr, 0};
	REQUIRE(duckdb_v2_logical_type_get_name(nullptr, &alias, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	duckdb_v2_logical_type_handle t = nullptr;
	duckdb_v2_factory_create_type_from_id(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER, nullptr, nullptr, 0, &t,
	                                      nullptr);
	REQUIRE(duckdb_v2_logical_type_get_name(t, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_logical_type_destroy(&t);
}

// ===========================================================================
// Fixtures shared by the sections below (V2-built via the connection)
// ===========================================================================

namespace {

duckdb_v2_logical_type_handle MakeEnum(duckdb_v2_factory_handle factory, const char **values, idx_t count) {
	std::vector<duckdb_v2_value_handle> entries;
	for (idx_t i = 0; i < count; i++) {
		entries.push_back(MakeVarcharValue(factory, values[i]));
	}
	return MakeType(factory, "enum", nullptr, std::move(entries));
}

duckdb_v2_logical_type_handle MakeStruct(duckdb_v2_factory_handle factory) {
	return MakeStructType(factory, {"id", "name"},
	                      {DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR});
}

duckdb_v2_logical_type_handle MakeUnion(duckdb_v2_factory_handle factory) {
	std::vector<const char *> names = {"i", "s"};
	return MakeType(factory, "union", &names,
	                {MakeTypeValue(factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER),
	                 MakeTypeValue(factory, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR)});
}

// V1 seed builders for the interop pins below: the two cross-version
// round-trip tests and the V1-only zero-entry enum pin.
duckdb_v2_logical_type_handle MakeV1Struct() {
	duckdb_logical_type members[2];
	members[0] = duckdb_create_logical_type(DUCKDB_TYPE_INTEGER);
	members[1] = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
	const char *names[2] = {"id", "name"};
	auto v1 = duckdb_create_struct_type(members, names, 2);
	// Free the member fixtures before any REQUIRE: Catch2 throws on failure
	// and would otherwise skip these destroys, leaking the LogicalTypes.
	duckdb_destroy_logical_type(&members[0]);
	duckdb_destroy_logical_type(&members[1]);
	REQUIRE(v1 != nullptr);
	return ConvertToV2(v1);
}

duckdb_v2_logical_type_handle MakeV1Union() {
	duckdb_logical_type members[2];
	members[0] = duckdb_create_logical_type(DUCKDB_TYPE_INTEGER);
	members[1] = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
	const char *names[2] = {"i", "s"};
	auto v1 = duckdb_create_union_type(members, names, 2);
	duckdb_destroy_logical_type(&members[0]);
	duckdb_destroy_logical_type(&members[1]);
	REQUIRE(v1 != nullptr);
	return ConvertToV2(v1);
}

duckdb_v2_logical_type_handle MakeV1Enum(const char **values, idx_t count) {
	auto v1 = duckdb_create_enum_type(values, count);
	REQUIRE(v1 != nullptr);
	return ConvertToV2(v1);
}

// ===========================================================================
// to_text / create_from_text
// ===========================================================================

std::string V2TypeText(duckdb_v2_logical_type_handle t) {
	return Render(t);
}

duckdb_v2_logical_type_handle V2TypeFromText(duckdb_v2_factory_handle factory, const std::string &text) {
	duckdb_v2_logical_type_handle t = nullptr;
	auto text_str = Convert(text);
	REQUIRE(duckdb_v2_factory_create_type_from_text(factory, &text_str, &t, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(t != nullptr);
	return t;
}

DUCKDB_V2_LOGICAL_TYPE_ID V2TypeIdOf(duckdb_v2_logical_type_handle t) {
	DUCKDB_V2_LOGICAL_TYPE_ID id = DUCKDB_V2_LOGICAL_TYPE_ID_INVALID;
	REQUIRE(duckdb_v2_logical_type_get_id(t, &id, nullptr) == DUCKDB_V2_ERROR_NONE);
	return id;
}

bool V2TypesEqual(duckdb_v2_logical_type_handle a, duckdb_v2_logical_type_handle b) {
	bool equal = false;
	REQUIRE(duckdb_v2_logical_type_is_equal(a, b, &equal, nullptr) == DUCKDB_V2_ERROR_NONE);
	return equal;
}

// ---------------------------------------------------------------------------
// Param inspection helpers
// ---------------------------------------------------------------------------

idx_t V2ParamCount(duckdb_v2_logical_type_handle t) {
	idx_t count = 99;
	REQUIRE(duckdb_v2_logical_type_get_param_count(t, &count, nullptr) == DUCKDB_V2_ERROR_NONE);
	return count;
}

// Reads param `index`, requiring the name to match (nullptr = positional,
// i.e. the {NULL, 0} view). Returns the owned value; caller destroys.
duckdb_v2_value_handle V2GetParam(duckdb_v2_logical_type_handle t, idx_t index, const char *expected_name) {
	duckdb_v2_str name = {reinterpret_cast<const char *>(0x1), 99};
	duckdb_v2_value_handle value = nullptr;
	REQUIRE(duckdb_v2_logical_type_get_param(t, index, &name, &value, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(value != nullptr);
	// Destroy before failing so a name mismatch cannot leak the value.
	const bool name_ok = expected_name ? (name == expected_name) : (name.ptr == nullptr && name.len == 0);
	if (!name_ok) {
		auto got = Convert(name);
		duckdb_v2_value_destroy(&value);
		FAIL("param name mismatch at index " << index << ": got '" << got << "'");
	}
	return value;
}

// Unwraps a TYPE param into an owned logical type.
duckdb_v2_logical_type_handle V2ParamType(duckdb_v2_logical_type_handle t, idx_t index, const char *expected_name) {
	auto v = V2GetParam(t, index, expected_name);
	duckdb_v2_logical_type_handle out = nullptr;
	auto rc = duckdb_v2_value_get_type(v, &out, nullptr);
	duckdb_v2_value_destroy(&v);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
	return out;
}

uint8_t V2ParamU8(duckdb_v2_logical_type_handle t, idx_t index) {
	auto v = V2GetParam(t, index, nullptr);
	return ConsumeValue<uint8_t>(v);
}

int64_t V2ParamI64(duckdb_v2_logical_type_handle t, idx_t index) {
	auto v = V2GetParam(t, index, nullptr);
	return ConsumeValue<int64_t>(v);
}

std::string V2ParamVarchar(duckdb_v2_logical_type_handle t, idx_t index, const char *expected_name) {
	auto v = V2GetParam(t, index, expected_name);
	return ConsumeValue<std::string>(v);
}

// ---------------------------------------------------------------------------
// Param value builders + create helpers
// ---------------------------------------------------------------------------

// Same, expecting failure: checks out nulling + err population, destroys the
// borrowed values, and returns the code.
DUCKDB_V2_ERROR MakeTypeErr(duckdb_v2_factory_handle factory, const char *name, const std::vector<const char *> *names,
                            std::vector<duckdb_v2_value_handle> values) {
	std::vector<duckdb_v2_str> name_views;
	if (names) {
		for (auto *n : *names) {
			name_views.push_back(Convert(n));
		}
	}
	DUCKDB_V2_ERROR rc = DUCKDB_V2_ERROR_NONE;
	bool out_nulled = false;
	bool err_set = false;
	auto t = reinterpret_cast<duckdb_v2_logical_type_handle>(0x1);
	duckdb_v2_error_info_handle err = nullptr;
	rc = LTCreateTypeFromName(factory, Convert(name), names ? name_views.data() : nullptr,
	                          values.empty() ? nullptr : values.data(), values.size(), &t, &err);
	out_nulled = (t == nullptr);
	err_set = (err != nullptr);
	duckdb_v2_error_info_destroy(&err);
	// Assert only after the fixtures are destroyed, so a failure cannot leak.
	for (auto &v : values) {
		duckdb_v2_value_destroy(&v);
	}
	REQUIRE(rc != DUCKDB_V2_ERROR_NONE);
	REQUIRE(out_nulled);
	REQUIRE(err_set);
	return rc;
}

// Reconstruction name: get_name returns exactly the vocabulary
// logical_type_create consumes (alias when set, else the canonical id
// name), so the duality below holds by construction.
std::string V2KindName(duckdb_v2_logical_type_handle t) {
	duckdb_v2_str name = {nullptr, 0};
	REQUIRE(duckdb_v2_logical_type_get_name(t, &name, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(name.len > 0);
	return Convert(name);
}

// The param round trip the design pins: create(name, params(t)) equals t.
void RequireParamRoundTrip(duckdb_v2_factory_handle factory, duckdb_v2_logical_type_handle t) {
	idx_t count = V2ParamCount(t);
	std::vector<std::string> name_storage(count);
	std::vector<duckdb_v2_str> names(count);
	std::vector<duckdb_v2_value_handle> values(count, nullptr);
	bool any_named = false;
	for (idx_t i = 0; i < count; i++) {
		duckdb_v2_str name = {nullptr, 0};
		auto param_rc = duckdb_v2_logical_type_get_param(t, i, &name, &values[i], nullptr);
		if (param_rc != DUCKDB_V2_ERROR_NONE) {
			// Destroy the values collected so far before failing.
			for (auto &v : values) {
				duckdb_v2_value_destroy(&v);
			}
		}
		REQUIRE(param_rc == DUCKDB_V2_ERROR_NONE);
		if (name.ptr) {
			name_storage[i] = Convert(name);
			names[i] = Convert(name_storage[i]);
			any_named = true;
		} else {
			names[i] = duckdb_v2_str {nullptr, 0};
		}
	}
	auto kind_name = V2KindName(t);
	duckdb_v2_logical_type_handle rebuilt = nullptr;
	DUCKDB_V2_ERROR rc = DUCKDB_V2_ERROR_NONE;
	rc = LTCreateTypeFromName(factory, Convert(kind_name), any_named ? names.data() : nullptr,
	                          values.empty() ? nullptr : values.data(), values.size(), &rebuilt, nullptr);
	for (auto &v : values) {
		duckdb_v2_value_destroy(&v);
	}
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
	const bool equal = V2TypesEqual(t, rebuilt);
	duckdb_v2_logical_type_destroy(&rebuilt);
	REQUIRE(equal);
}

} // namespace

TEST_CASE("V2: LOGICAL_TYPE_ID_TYPE mirrors duckdb::LogicalTypeId::TYPE", "[capi_v2][logical_type][type_value]") {
	REQUIRE(static_cast<uint32_t>(duckdb::LogicalTypeId::TYPE) ==
	        static_cast<uint32_t>(DUCKDB_V2_LOGICAL_TYPE_ID_TYPE));
}

TEST_CASE("V2: logical_type to_text renders primitives", "[capi_v2][logical_type][to_text]") {
	EnvFixture fx;
	struct {
		DUCKDB_V2_LOGICAL_TYPE_ID id;
		const char *expected;
	} cases[] = {
	    {DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER, "INTEGER"},
	    {DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR, "VARCHAR"},
	    {DUCKDB_V2_LOGICAL_TYPE_ID_BOOLEAN, "BOOLEAN"},
	    // Engine spellings for the tz kinds carry spaces; pinned here because
	    // from_text must accept exactly what to_text emits.
	    {DUCKDB_V2_LOGICAL_TYPE_ID_TIMESTAMP_TZ, "TIMESTAMP WITH TIME ZONE"},
	    {DUCKDB_V2_LOGICAL_TYPE_ID_TIME_TZ, "TIME WITH TIME ZONE"},
	    {DUCKDB_V2_LOGICAL_TYPE_ID_TIMESTAMP_SEC, "TIMESTAMP_S"},
	};
	for (auto &c : cases) {
		duckdb_v2_logical_type_handle t = nullptr;
		REQUIRE(duckdb_v2_factory_create_type_from_id(fx.factory, c.id, nullptr, nullptr, 0, &t, nullptr) ==
		        DUCKDB_V2_ERROR_NONE);
		REQUIRE(V2TypeText(t) == c.expected);
		duckdb_v2_logical_type_destroy(&t);
	}
}

TEST_CASE("V2: logical_type to_text renders composites", "[capi_v2][logical_type][to_text]") {
	EnvFixture fx;

	auto dec =
	    MakeType(fx.factory, "decimal", nullptr, {MakeInt32Value(fx.factory, 18), MakeInt32Value(fx.factory, 3)});
	REQUIRE(V2TypeText(dec) == "DECIMAL(18,3)");
	duckdb_v2_logical_type_destroy(&dec);

	auto list = MakeListType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	REQUIRE(V2TypeText(list) == "INTEGER[]");
	duckdb_v2_logical_type_destroy(&list);

	auto arr = MakeType(fx.factory, "array", nullptr,
	                    {MakeTypeValue(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER), MakeInt32Value(fx.factory, 3)});
	REQUIRE(V2TypeText(arr) == "INTEGER[3]");
	duckdb_v2_logical_type_destroy(&arr);

	auto map = MakeMapType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	REQUIRE(V2TypeText(map) == "MAP(VARCHAR, INTEGER)");
	duckdb_v2_logical_type_destroy(&map);

	auto s = MakeStruct(fx.factory); // STRUCT(id INTEGER, name VARCHAR)
	// SQLIdentifier quotes field names that collide with keywords ("name").
	REQUIRE(V2TypeText(s) == "STRUCT(id INTEGER, \"name\" VARCHAR)");
	duckdb_v2_logical_type_destroy(&s);

	auto u = MakeUnion(fx.factory); // UNION(i INTEGER, s VARCHAR)
	REQUIRE(V2TypeText(u) == "UNION(i INTEGER, s VARCHAR)");
	duckdb_v2_logical_type_destroy(&u);

	const char *enum_names[] = {"a", "bb"};
	auto e = MakeEnum(fx.factory, enum_names, 2);
	REQUIRE(V2TypeText(e) == "ENUM('a', 'bb')");
	duckdb_v2_logical_type_destroy(&e);
}

TEST_CASE("V2: logical_type to_text renders an aliased type as its alias", "[capi_v2][logical_type][to_text]") {
	EnvFixture fx;
	auto base = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_logical_type_handle t = nullptr;
	auto alias_name_str = Convert("my_int");
	auto alias_rc = duckdb_v2_factory_create_type_with_alias(fx.factory, base, &alias_name_str, &t, nullptr);
	duckdb_v2_logical_type_destroy(&base);
	REQUIRE(alias_rc == DUCKDB_V2_ERROR_NONE);
	REQUIRE(V2TypeText(t) == "my_int");
	duckdb_v2_logical_type_destroy(&t);
}

TEST_CASE("V2: logical_type to_text null handle / short buffer", "[capi_v2][logical_type][to_text]") {
	EnvFixture fx;
	char buf[64] = {};
	idx_t len = 0;
	REQUIRE(duckdb_v2_logical_type_to_text(nullptr, buf, sizeof(buf), &len, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	duckdb_v2_logical_type_handle t = nullptr;
	duckdb_v2_factory_create_type_from_id(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER, nullptr, nullptr, 0, &t,
	                                      nullptr);
	REQUIRE(duckdb_v2_logical_type_to_text(t, buf, sizeof(buf), nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	// Sizing excludes the terminator; the buffer must still have room for it.
	REQUIRE(duckdb_v2_logical_type_to_text(t, nullptr, 0, &len, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(len == 7);
	REQUIRE(duckdb_v2_logical_type_to_text(t, buf, len, &len, nullptr) == DUCKDB_V2_ERROR_INPUT_OBJECT_SIZE);
	REQUIRE(len == 7);
	REQUIRE(duckdb_v2_logical_type_to_text(t, buf, len + 1, &len, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(std::string(buf) == "INTEGER");
	duckdb_v2_logical_type_destroy(&t);
}

TEST_CASE("V2: logical_type create_from_text parses primitives, case-insensitive",
          "[capi_v2][logical_type][from_text]") {
	EnvFixture f;

	auto t = V2TypeFromText(f.factory, "INTEGER");
	REQUIRE(V2TypeIdOf(t) == DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_logical_type_destroy(&t);

	t = V2TypeFromText(f.factory, "integer");
	REQUIRE(V2TypeIdOf(t) == DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_logical_type_destroy(&t);

	// TYPE is a first-class type name: the type of TYPE values.
	t = V2TypeFromText(f.factory, "TYPE");
	REQUIRE(V2TypeIdOf(t) == DUCKDB_V2_LOGICAL_TYPE_ID_TYPE);
	duckdb_v2_logical_type_destroy(&t);
}

TEST_CASE("V2: logical_type create_from_text parses parameterized kinds", "[capi_v2][logical_type][from_text]") {
	EnvFixture f;

	auto dec = V2TypeFromText(f.factory, "DECIMAL(18,3)");
	REQUIRE(V2TypeIdOf(dec) == DUCKDB_V2_LOGICAL_TYPE_ID_DECIMAL);
	REQUIRE(V2ParamU8(dec, 0) == 18);
	REQUIRE(V2ParamU8(dec, 1) == 3);
	duckdb_v2_logical_type_destroy(&dec);

	auto list = V2TypeFromText(f.factory, "INTEGER[]");
	REQUIRE(V2TypeIdOf(list) == DUCKDB_V2_LOGICAL_TYPE_ID_LIST);
	auto child = V2ParamType(list, 0, nullptr);
	REQUIRE(V2TypeIdOf(child) == DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_logical_type_destroy(&child);
	duckdb_v2_logical_type_destroy(&list);

	auto arr = V2TypeFromText(f.factory, "INTEGER[3]");
	REQUIRE(V2TypeIdOf(arr) == DUCKDB_V2_LOGICAL_TYPE_ID_ARRAY);
	REQUIRE(V2ParamI64(arr, 1) == 3);
	duckdb_v2_logical_type_destroy(&arr);

	auto s = V2TypeFromText(f.factory, "STRUCT(a INTEGER, b VARCHAR)");
	REQUIRE(V2TypeIdOf(s) == DUCKDB_V2_LOGICAL_TYPE_ID_STRUCT);
	auto field_a = V2ParamType(s, 0, "a");
	REQUIRE(V2TypeIdOf(field_a) == DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_logical_type_destroy(&field_a);
	auto field_b = V2ParamType(s, 1, "b");
	REQUIRE(V2TypeIdOf(field_b) == DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR);
	duckdb_v2_logical_type_destroy(&field_b);
	duckdb_v2_logical_type_destroy(&s);

	// TUPLE is the unnamed struct: the STRUCT param view with positional names.
	auto tup = V2TypeFromText(f.factory, "TUPLE(INTEGER, VARCHAR)");
	REQUIRE(V2TypeIdOf(tup) == DUCKDB_V2_LOGICAL_TYPE_ID_TUPLE);
	REQUIRE(V2ParamCount(tup) == 2);
	auto el0 = V2ParamType(tup, 0, nullptr);
	REQUIRE(V2TypeIdOf(el0) == DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_logical_type_destroy(&el0);
	auto el1 = V2ParamType(tup, 1, nullptr);
	REQUIRE(V2TypeIdOf(el1) == DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR);
	duckdb_v2_logical_type_destroy(&el1);
	duckdb_v2_logical_type_destroy(&tup);

	auto map = V2TypeFromText(f.factory, "MAP(VARCHAR, INTEGER)");
	REQUIRE(V2TypeIdOf(map) == DUCKDB_V2_LOGICAL_TYPE_ID_MAP);
	duckdb_v2_logical_type_destroy(&map);

	auto u = V2TypeFromText(f.factory, "UNION(i INTEGER, s VARCHAR)");
	REQUIRE(V2TypeIdOf(u) == DUCKDB_V2_LOGICAL_TYPE_ID_UNION);
	duckdb_v2_logical_type_destroy(&u);

	auto e = V2TypeFromText(f.factory, "ENUM('x', 'y')");
	REQUIRE(V2TypeIdOf(e) == DUCKDB_V2_LOGICAL_TYPE_ID_ENUM);
	REQUIRE(V2ParamCount(e) == 2);
	REQUIRE(V2ParamVarchar(e, 1, nullptr) == "y");
	duckdb_v2_logical_type_destroy(&e);

	// Deep nesting: composite parameters compose recursively.
	auto deep = V2TypeFromText(f.factory, "STRUCT(a INTEGER[], m MAP(VARCHAR, DECIMAL(10,2)))[]");
	REQUIRE(V2TypeIdOf(deep) == DUCKDB_V2_LOGICAL_TYPE_ID_LIST);
	REQUIRE(V2TypeText(deep) == "STRUCT(a INTEGER[], m MAP(VARCHAR, DECIMAL(10,2)))[]");
	duckdb_v2_logical_type_destroy(&deep);
}

// Deliberate cross-version pin: the composite fixtures are V1-built and the
// V2 reconstruction is compared against them via logical_type_is_equal.
TEST_CASE("V2: logical_type from_text(to_text) round-trips constructible kinds",
          "[capi_v2][logical_type][from_text][to_text]") {
	EnvFixture f;

	auto require_round_trip = [&](duckdb_v2_logical_type_handle t) {
		auto text = V2TypeText(t);
		auto parsed = V2TypeFromText(f.factory, text);
		REQUIRE(V2TypesEqual(t, parsed));
		// And the other direction: the canonical rendering is stable.
		REQUIRE(V2TypeText(parsed) == text);
		duckdb_v2_logical_type_destroy(&parsed);
	};

	// Every id create_from_id accepts.
	DUCKDB_V2_LOGICAL_TYPE_ID primitives[] = {
	    DUCKDB_V2_LOGICAL_TYPE_ID_BOOLEAN,
	    DUCKDB_V2_LOGICAL_TYPE_ID_TINYINT,
	    DUCKDB_V2_LOGICAL_TYPE_ID_SMALLINT,
	    DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER,
	    DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT,
	    DUCKDB_V2_LOGICAL_TYPE_ID_UTINYINT,
	    DUCKDB_V2_LOGICAL_TYPE_ID_USMALLINT,
	    DUCKDB_V2_LOGICAL_TYPE_ID_UINTEGER,
	    DUCKDB_V2_LOGICAL_TYPE_ID_UBIGINT,
	    DUCKDB_V2_LOGICAL_TYPE_ID_HUGEINT,
	    DUCKDB_V2_LOGICAL_TYPE_ID_UHUGEINT,
	    DUCKDB_V2_LOGICAL_TYPE_ID_FLOAT,
	    DUCKDB_V2_LOGICAL_TYPE_ID_DOUBLE,
	    DUCKDB_V2_LOGICAL_TYPE_ID_DATE,
	    DUCKDB_V2_LOGICAL_TYPE_ID_TIME,
	    DUCKDB_V2_LOGICAL_TYPE_ID_TIME_TZ,
	    DUCKDB_V2_LOGICAL_TYPE_ID_TIME_NS,
	    DUCKDB_V2_LOGICAL_TYPE_ID_TIMESTAMP_SEC,
	    DUCKDB_V2_LOGICAL_TYPE_ID_TIMESTAMP_MS,
	    DUCKDB_V2_LOGICAL_TYPE_ID_TIMESTAMP,
	    DUCKDB_V2_LOGICAL_TYPE_ID_TIMESTAMP_NS,
	    DUCKDB_V2_LOGICAL_TYPE_ID_TIMESTAMP_TZ,
	    DUCKDB_V2_LOGICAL_TYPE_ID_TIMESTAMP_TZ_NS,
	    DUCKDB_V2_LOGICAL_TYPE_ID_INTERVAL,
	    DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR,
	    DUCKDB_V2_LOGICAL_TYPE_ID_BLOB,
	    DUCKDB_V2_LOGICAL_TYPE_ID_BIT,
	    DUCKDB_V2_LOGICAL_TYPE_ID_BIGNUM,
	    DUCKDB_V2_LOGICAL_TYPE_ID_UUID,
	};
	for (auto id : primitives) {
		duckdb_v2_logical_type_handle t = nullptr;
		REQUIRE(duckdb_v2_factory_create_type_from_id(f.factory, id, nullptr, nullptr, 0, &t, nullptr) ==
		        DUCKDB_V2_ERROR_NONE);
		require_round_trip(t);
		duckdb_v2_logical_type_destroy(&t);
	}

	// Composites (built via V1 fixtures), including nesting.
	auto dec = ConvertToV2(duckdb_create_decimal_type(18, 3));
	require_round_trip(dec);
	duckdb_v2_logical_type_destroy(&dec);

	auto int_v1 = duckdb_create_logical_type(DUCKDB_TYPE_INTEGER);
	auto list_v1 = duckdb_create_list_type(int_v1);
	auto list = ConvertToV2(duckdb_create_list_type(list_v1)); // INTEGER[][]
	duckdb_destroy_logical_type(&list_v1);
	require_round_trip(list);
	duckdb_v2_logical_type_destroy(&list);

	auto arr = ConvertToV2(duckdb_create_array_type(int_v1, 7));
	require_round_trip(arr);
	duckdb_v2_logical_type_destroy(&arr);

	auto varchar_v1 = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
	auto map = ConvertToV2(duckdb_create_map_type(varchar_v1, int_v1));
	require_round_trip(map);
	duckdb_v2_logical_type_destroy(&map);
	duckdb_destroy_logical_type(&int_v1);
	duckdb_destroy_logical_type(&varchar_v1);

	auto s = MakeV1Struct();
	require_round_trip(s);
	duckdb_v2_logical_type_destroy(&s);

	auto u = MakeV1Union();
	require_round_trip(u);
	duckdb_v2_logical_type_destroy(&u);

	const char *enum_names[] = {"sad", "ok", "happy"};
	auto e = MakeV1Enum(enum_names, 3);
	require_round_trip(e);
	duckdb_v2_logical_type_destroy(&e);

	// Kinds only reachable through from_text itself.
	auto type_t = V2TypeFromText(f.factory, "TYPE");
	require_round_trip(type_t);
	duckdb_v2_logical_type_destroy(&type_t);

	auto geom = V2TypeFromText(f.factory, "GEOMETRY");
	REQUIRE(V2TypeIdOf(geom) == DUCKDB_V2_LOGICAL_TYPE_ID_GEOMETRY);
	require_round_trip(geom);
	duckdb_v2_logical_type_destroy(&geom);
}

TEST_CASE("V2: logical_type create_from_text resolves a catalog type", "[capi_v2][logical_type][from_text]") {
	EnvFixture f;
	ExecSQL(f.conn, "CREATE TYPE mood AS ENUM('sad', 'ok', 'happy')");

	auto t = V2TypeFromText(f.factory, "mood");
	REQUIRE(V2TypeIdOf(t) == DUCKDB_V2_LOGICAL_TYPE_ID_ENUM);
	REQUIRE(V2ParamCount(t) == 3);
	REQUIRE(V2ParamVarchar(t, 2, nullptr) == "happy");

	// The bound type is structural: this path attaches no alias, so
	// get_name falls back to the canonical id name and to_text renders the
	// dictionary form, which round-trips.
	duckdb_v2_str name = {nullptr, 0};
	REQUIRE(duckdb_v2_logical_type_get_name(t, &name, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(name == "ENUM");
	auto text = V2TypeText(t);
	REQUIRE(text == "ENUM('sad', 'ok', 'happy')");
	auto again = V2TypeFromText(f.factory, text);
	REQUIRE(V2TypesEqual(t, again));
	duckdb_v2_logical_type_destroy(&again);
	duckdb_v2_logical_type_destroy(&t);
}

TEST_CASE("V2: logical_type create_from_text error paths", "[capi_v2][logical_type][from_text]") {
	EnvFixture f;
	duckdb_v2_logical_type_handle t = nullptr;
	duckdb_v2_error_info_handle err = nullptr;

	// Unresolvable type name: binder/catalog error surfaces from the call.
	auto text_str = Convert("definitely_not_a_type");
	REQUIRE(duckdb_v2_factory_create_type_from_text(f.factory, &text_str, &t, &err) != DUCKDB_V2_ERROR_NONE);
	REQUIRE(t == nullptr);
	REQUIRE(err != nullptr);
	duckdb_v2_error_info_destroy(&err);

	// Unparseable type expression.
	auto text_str2 = Convert("INTEGER[[");
	REQUIRE(duckdb_v2_factory_create_type_from_text(f.factory, &text_str2, &t, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(t == nullptr);
	REQUIRE(err != nullptr);
	duckdb_v2_error_info_destroy(&err);

	// Empty text ({NULL, 0} is a valid empty view; parsing it fails).
	auto text_str3 = duckdb_v2_str {nullptr, 0};
	REQUIRE(duckdb_v2_factory_create_type_from_text(f.factory, &text_str3, &t, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(t == nullptr);
	duckdb_v2_error_info_destroy(&err);
}

TEST_CASE("V2: logical_type create_from_text null-arg refusals", "[capi_v2][logical_type][from_text]") {
	EnvFixture f;
	duckdb_v2_logical_type_handle t = nullptr;

	// A null connection is refused.
	auto text_str = Convert("INTEGER");
	REQUIRE(duckdb_v2_factory_create_type_from_text(nullptr, &text_str, &t, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	REQUIRE(duckdb_v2_factory_create_type_from_text(f.factory, &text_str, nullptr, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	// Malformed view: null pointer with nonzero length.
	duckdb_v2_logical_type_handle out = nullptr;
	auto text_str2 = duckdb_v2_str {nullptr, 3};
	REQUIRE(duckdb_v2_factory_create_type_from_text(f.factory, &text_str2, &out, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(out == nullptr);
}

namespace {
// The conn-level dance pinned as a contract: resolve a type straight from the
// connection, then use the type afterward.
void BuildDecimalFromText(duckdb_v2_factory_handle factory, duckdb_v2_logical_type_handle *out) {
	auto text_str = Convert("DECIMAL(12,4)");
	duckdb_v2_factory_create_type_from_text(factory, &text_str, out, nullptr);
}
} // namespace

TEST_CASE("V2: get_from_text resolves a type that outlives the call", "[capi_v2][logical_type][from_text]") {
	EnvFixture f;
	duckdb_v2_logical_type_handle t = nullptr;
	BuildDecimalFromText(f.factory, &t);
	REQUIRE(t != nullptr);
	// Fully usable after the scope exits.
	REQUIRE(V2TypeIdOf(t) == DUCKDB_V2_LOGICAL_TYPE_ID_DECIMAL);
	REQUIRE(V2ParamU8(t, 0) == 12);
	REQUIRE(V2ParamU8(t, 1) == 4);
	REQUIRE(V2TypeText(t) == "DECIMAL(12,4)");
	duckdb_v2_logical_type_destroy(&t);
}

TEST_CASE("V2: type construction does not disturb a live streaming result",
          "[capi_v2][logical_type][from_text][create][streaming]") {
	EnvFixture f;
	ExecSQL(f.conn, "CREATE TYPE mood AS ENUM('sad', 'ok', 'happy')");

	QueryResult r;
	REQUIRE(Query(f.conn, "SELECT * FROM range(10000)", &r) == DUCKDB_V2_ERROR_NONE);
	auto first = StepChunk(r);
	REQUIRE(first != nullptr);
	idx_t seen = 0;
	duckdb_v2_data_chunk_get_size(first, &seen, nullptr);
	duckdb_v2_data_chunk_destroy(&first);

	// The get_from_text call runs on the live stream's transaction; parse-only,
	// catalog-lookup, and generic construction all run inside it without
	// cancelling the stream. Do not step the stream inside the scope.
	duckdb_v2_logical_type_handle list = nullptr;
	auto text_str = Convert("INTEGER[]");
	REQUIRE(duckdb_v2_factory_create_type_from_text(f.factory, &text_str, &list, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(V2TypeIdOf(list) == DUCKDB_V2_LOGICAL_TYPE_ID_LIST);
	duckdb_v2_logical_type_destroy(&list);

	duckdb_v2_logical_type_handle mood = nullptr;
	auto text_str2 = Convert("mood");
	REQUIRE(duckdb_v2_factory_create_type_from_text(f.factory, &text_str2, &mood, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(V2TypeIdOf(mood) == DUCKDB_V2_LOGICAL_TYPE_ID_ENUM);
	duckdb_v2_logical_type_destroy(&mood);

	duckdb_v2_value_handle elem = MakeTypeValue(f.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	const duckdb_v2_value_handle params[1] = {elem};
	duckdb_v2_logical_type_handle built = nullptr;
	auto rc = LTCreateTypeFromName(f.factory, Convert("list"), nullptr, params, 1, &built, nullptr);
	duckdb_v2_value_destroy(&elem);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
	REQUIRE(V2TypeIdOf(built) == DUCKDB_V2_LOGICAL_TYPE_ID_LIST);
	duckdb_v2_logical_type_destroy(&built);

	// The stream is still live and drains to completion.
	REQUIRE(seen + DrainRowCount(r) == 10000);
}

// ===========================================================================
// Generic parameter inspection: get_param_count / get_param
// ===========================================================================

TEST_CASE("V2: parameterless kinds report zero params", "[capi_v2][logical_type][param]") {
	EnvFixture f;
	DUCKDB_V2_LOGICAL_TYPE_ID ids[] = {
	    DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER,
	    DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR,
	    DUCKDB_V2_LOGICAL_TYPE_ID_TIMESTAMP,
	    DUCKDB_V2_LOGICAL_TYPE_ID_BLOB,
	};
	for (auto id : ids) {
		duckdb_v2_logical_type_handle t = nullptr;
		REQUIRE(duckdb_v2_factory_create_type_from_id(f.factory, id, nullptr, nullptr, 0, &t, nullptr) ==
		        DUCKDB_V2_ERROR_NONE);
		REQUIRE(V2ParamCount(t) == 0);
		duckdb_v2_logical_type_destroy(&t);
	}
	// TYPE and (CRS-less) GEOMETRY carry no parameters either.
	auto type_t = V2TypeFromText(f.factory, "TYPE");
	REQUIRE(V2ParamCount(type_t) == 0);
	duckdb_v2_logical_type_destroy(&type_t);
	auto geom = V2TypeFromText(f.factory, "GEOMETRY");
	REQUIRE(V2ParamCount(geom) == 0);
	duckdb_v2_logical_type_destroy(&geom);
}

TEST_CASE("V2: DECIMAL params are width and scale as UTINYINT", "[capi_v2][logical_type][param]") {
	EnvFixture fx;
	auto t = MakeType(fx.factory, "decimal", nullptr, {MakeInt32Value(fx.factory, 18), MakeInt32Value(fx.factory, 3)});
	REQUIRE(V2ParamCount(t) == 2);
	REQUIRE(V2ParamU8(t, 0) == 18);
	REQUIRE(V2ParamU8(t, 1) == 3);
	// The param values are UTINYINT typed.
	auto v = V2GetParam(t, 0, nullptr);
	duckdb_v2_logical_type_handle vt = nullptr;
	duckdb_v2_value_get_logical_type(v, &vt, nullptr);
	REQUIRE(V2TypeIdOf(vt) == DUCKDB_V2_LOGICAL_TYPE_ID_UTINYINT);
	duckdb_v2_logical_type_destroy(&vt);
	duckdb_v2_value_destroy(&v);
	duckdb_v2_logical_type_destroy(&t);
}

TEST_CASE("V2: LIST and ARRAY params carry the element type as a TYPE value", "[capi_v2][logical_type][param]") {
	EnvFixture fx;
	auto inner_list = MakeListType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	auto nested = MakeType(fx.factory, "list", nullptr, {MakeTypeValue(fx.factory, inner_list)}); // INTEGER[][]

	REQUIRE(V2ParamCount(nested) == 1);
	auto inner = V2ParamType(nested, 0, nullptr);
	REQUIRE(V2TypeIdOf(inner) == DUCKDB_V2_LOGICAL_TYPE_ID_LIST);
	auto leaf = V2ParamType(inner, 0, nullptr);
	REQUIRE(V2TypeIdOf(leaf) == DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_logical_type_destroy(&leaf);
	duckdb_v2_logical_type_destroy(&inner);
	duckdb_v2_logical_type_destroy(&nested);

	auto arr = MakeType(fx.factory, "array", nullptr,
	                    {MakeTypeValue(fx.factory, inner_list), MakeInt32Value(fx.factory, 7)}); // INTEGER[][7]
	duckdb_v2_logical_type_destroy(&inner_list);
	REQUIRE(V2ParamCount(arr) == 2);
	auto elem = V2ParamType(arr, 0, nullptr);
	REQUIRE(V2TypeIdOf(elem) == DUCKDB_V2_LOGICAL_TYPE_ID_LIST);
	duckdb_v2_logical_type_destroy(&elem);
	REQUIRE(V2ParamI64(arr, 1) == 7);
	duckdb_v2_logical_type_destroy(&arr);
}

TEST_CASE("V2: MAP params are key and value types", "[capi_v2][logical_type][param]") {
	EnvFixture fx;
	auto map = MakeMapType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);

	REQUIRE(V2ParamCount(map) == 2);
	auto key = V2ParamType(map, 0, nullptr);
	REQUIRE(V2TypeIdOf(key) == DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR);
	duckdb_v2_logical_type_destroy(&key);
	auto val = V2ParamType(map, 1, nullptr);
	REQUIRE(V2TypeIdOf(val) == DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_logical_type_destroy(&val);
	duckdb_v2_logical_type_destroy(&map);
}

TEST_CASE("V2: STRUCT and UNION params are named TYPE values", "[capi_v2][logical_type][param]") {
	EnvFixture fx;
	auto s = MakeStruct(fx.factory); // STRUCT(id INTEGER, name VARCHAR)
	REQUIRE(V2ParamCount(s) == 2);
	auto id_field = V2ParamType(s, 0, "id");
	REQUIRE(V2TypeIdOf(id_field) == DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_logical_type_destroy(&id_field);
	auto name_field = V2ParamType(s, 1, "name");
	REQUIRE(V2TypeIdOf(name_field) == DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR);
	duckdb_v2_logical_type_destroy(&name_field);
	duckdb_v2_logical_type_destroy(&s);

	auto u = MakeUnion(fx.factory); // UNION(i INTEGER, s VARCHAR)
	REQUIRE(V2ParamCount(u) == 2);
	auto member = V2ParamType(u, 1, "s");
	REQUIRE(V2TypeIdOf(member) == DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR);
	duckdb_v2_logical_type_destroy(&member);
	duckdb_v2_logical_type_destroy(&u);
}

TEST_CASE("V2: ENUM params are the dictionary entries as VARCHAR", "[capi_v2][logical_type][param]") {
	EnvFixture fx;
	const char *names[] = {"a", "bb", "ccc"};
	auto e = MakeEnum(fx.factory, names, 3);
	REQUIRE(V2ParamCount(e) == 3);
	REQUIRE(V2ParamVarchar(e, 0, nullptr) == "a");
	REQUIRE(V2ParamVarchar(e, 1, nullptr) == "bb");
	REQUIRE(V2ParamVarchar(e, 2, nullptr) == "ccc");
	duckdb_v2_logical_type_destroy(&e);

	// Interop pin: a zero-entry enum is invalid in SQL (the enum bind
	// requires at least one argument) and V2 correctly refuses to build it;
	// only V1 can construct the degenerate form. V2 inspection handles the
	// V1-built handle gracefully: zero params, get_param refuses.
	const char *none[] = {nullptr};
	auto empty = MakeV1Enum(none, 0);
	REQUIRE(V2ParamCount(empty) == 0);
	duckdb_v2_str name = {nullptr, 0};
	duckdb_v2_value_handle v = nullptr;
	REQUIRE(duckdb_v2_logical_type_get_param(empty, 0, &name, &v, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(v == nullptr);
	duckdb_v2_logical_type_destroy(&empty);
}

TEST_CASE("V2: get_param out-of-range and null-arg refusals", "[capi_v2][logical_type][param]") {
	EnvFixture fx;
	auto t = MakeType(fx.factory, "decimal", nullptr, {MakeInt32Value(fx.factory, 10), MakeInt32Value(fx.factory, 2)});

	duckdb_v2_str name = {nullptr, 0};
	auto v = reinterpret_cast<duckdb_v2_value_handle>(0x1);
	duckdb_v2_error_info_handle err = nullptr;
	REQUIRE(duckdb_v2_logical_type_get_param(t, 2, &name, &v, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(v == nullptr);
	REQUIRE(err != nullptr);
	duckdb_v2_error_info_destroy(&err);

	idx_t count = 0;
	REQUIRE(duckdb_v2_logical_type_get_param_count(nullptr, &count, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_logical_type_get_param_count(t, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_logical_type_get_param(nullptr, 0, &name, &v, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_logical_type_get_param(t, 0, nullptr, &v, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_logical_type_get_param(t, 0, &name, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	duckdb_v2_logical_type_destroy(&t);
}

// ===========================================================================
// logical_type_create
// ===========================================================================

TEST_CASE("V2: logical_type_create builds the built-in parameterized kinds", "[capi_v2][logical_type][create]") {
	EnvFixture f;

	// decimal(width, scale): numeric positional params.
	auto dec = MakeType(f.factory, "decimal", nullptr, {MakeInt32Value(f.factory, 18), MakeInt32Value(f.factory, 3)});
	auto dec_expected = ConvertToV2(duckdb_create_decimal_type(18, 3));
	REQUIRE(V2TypesEqual(dec, dec_expected));
	duckdb_v2_logical_type_destroy(&dec_expected);
	duckdb_v2_logical_type_destroy(&dec);

	// list(T): the element type crosses as a TYPE value.
	auto list = MakeType(f.factory, "list", nullptr, {MakeTypeValue(f.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER)});
	REQUIRE(V2TypeText(list) == "INTEGER[]");
	duckdb_v2_logical_type_destroy(&list);

	// array(T, size).
	auto arr = MakeType(f.factory, "array", nullptr,
	                    {MakeTypeValue(f.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER), MakeInt32Value(f.factory, 3)});
	REQUIRE(V2TypeText(arr) == "INTEGER[3]");
	duckdb_v2_logical_type_destroy(&arr);

	// map(K, V).
	auto map = MakeType(f.factory, "map", nullptr,
	                    {MakeTypeValue(f.factory, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR),
	                     MakeTypeValue(f.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER)});
	REQUIRE(V2TypeText(map) == "MAP(VARCHAR, INTEGER)");
	duckdb_v2_logical_type_destroy(&map);

	// struct: named fields.
	std::vector<const char *> field_names = {"a", "b"};
	auto s = MakeType(f.factory, "struct", &field_names,
	                  {MakeTypeValue(f.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER),
	                   MakeTypeValue(f.factory, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR)});
	REQUIRE(V2TypeText(s) == "STRUCT(a INTEGER, b VARCHAR)");
	duckdb_v2_logical_type_destroy(&s);

	// struct requires named fields; the anonymous form is the tuple type.
	REQUIRE(MakeTypeErr(f.factory, "struct", nullptr,
	                    {MakeTypeValue(f.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER),
	                     MakeTypeValue(f.factory, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR)}) == DUCKDB_V2_ERROR_QUERY_BINDER);

	// tuple: all positional; params come back positional.
	auto tup = MakeType(f.factory, "tuple", nullptr,
	                    {MakeTypeValue(f.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER),
	                     MakeTypeValue(f.factory, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR)});
	REQUIRE(V2TypeIdOf(tup) == DUCKDB_V2_LOGICAL_TYPE_ID_TUPLE);
	REQUIRE(V2ParamCount(tup) == 2);
	auto tup_field = V2ParamType(tup, 0, nullptr);
	REQUIRE(V2TypeIdOf(tup_field) == DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_logical_type_destroy(&tup_field);
	duckdb_v2_logical_type_destroy(&tup);

	// union: named members.
	std::vector<const char *> member_names = {"i", "s"};
	auto u = MakeType(f.factory, "union", &member_names,
	                  {MakeTypeValue(f.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER),
	                   MakeTypeValue(f.factory, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR)});
	REQUIRE(V2TypeText(u) == "UNION(i INTEGER, s VARCHAR)");
	duckdb_v2_logical_type_destroy(&u);

	// enum: positional VARCHAR entries. Name resolution is case-insensitive.
	auto e = MakeType(f.factory, "ENUM", nullptr, {MakeVarcharValue(f.factory, "x"), MakeVarcharValue(f.factory, "y")});
	REQUIRE(V2TypeText(e) == "ENUM('x', 'y')");
	duckdb_v2_logical_type_destroy(&e);
}

TEST_CASE("V2: logical_type_create nests through TYPE values", "[capi_v2][logical_type][create]") {
	EnvFixture f;
	// list(struct(a integer[], m map(varchar, decimal(10,2)))), composed
	// bottom-up, equals the from_text form.
	auto int_list = MakeType(f.factory, "list", nullptr, {MakeTypeValue(f.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER)});
	auto dec = MakeType(f.factory, "decimal", nullptr, {MakeInt32Value(f.factory, 10), MakeInt32Value(f.factory, 2)});
	auto map = MakeType(f.factory, "map", nullptr,
	                    {MakeTypeValue(f.factory, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR), MakeTypeValue(f.factory, dec)});
	std::vector<const char *> field_names = {"a", "m"};
	auto s = MakeType(f.factory, "struct", &field_names,
	                  {MakeTypeValue(f.factory, int_list), MakeTypeValue(f.factory, map)});
	auto deep = MakeType(f.factory, "list", nullptr, {MakeTypeValue(f.factory, s)});
	duckdb_v2_logical_type_destroy(&int_list);
	duckdb_v2_logical_type_destroy(&dec);
	duckdb_v2_logical_type_destroy(&map);
	duckdb_v2_logical_type_destroy(&s);

	auto expected = V2TypeFromText(f.factory, "STRUCT(a INTEGER[], m MAP(VARCHAR, DECIMAL(10,2)))[]");
	REQUIRE(V2TypesEqual(deep, expected));
	duckdb_v2_logical_type_destroy(&expected);
	duckdb_v2_logical_type_destroy(&deep);
}

TEST_CASE("V2: logical_type_create builds a collation VARCHAR", "[capi_v2][logical_type][create][param]") {
	EnvFixture f;
	std::vector<const char *> names = {"collation"};
	auto t = MakeType(f.factory, "varchar", &names, {MakeVarcharValue(f.factory, "nocase")});
	REQUIRE(V2TypeIdOf(t) == DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR);
	// Inspection keeps what the bound type carries: the collation, as one
	// named param (to_text renders plain VARCHAR).
	REQUIRE(V2ParamCount(t) == 1);
	REQUIRE(V2ParamVarchar(t, 0, "collation") == "nocase");
	RequireParamRoundTrip(f.factory, t);
	duckdb_v2_logical_type_destroy(&t);
}

TEST_CASE("V2: logical_type_create error paths", "[capi_v2][logical_type][create]") {
	EnvFixture f;

	// Unknown name: catalog error.
	REQUIRE(MakeTypeErr(f.factory, "definitely_not_a_type", nullptr, {}) == DUCKDB_V2_ERROR_DATABASE_CATALOG);
	// Wrong parameter counts and types: binder errors from the bind function.
	REQUIRE(MakeTypeErr(f.factory, "list", nullptr, {}) == DUCKDB_V2_ERROR_QUERY_BINDER);
	REQUIRE(MakeTypeErr(f.factory, "list", nullptr, {MakeInt32Value(f.factory, 1)}) == DUCKDB_V2_ERROR_QUERY_BINDER);
	REQUIRE(MakeTypeErr(f.factory, "decimal", nullptr, {MakeInt32Value(f.factory, 0)}) == DUCKDB_V2_ERROR_QUERY_BINDER);
	REQUIRE(MakeTypeErr(f.factory, "array", nullptr,
	                    {MakeTypeValue(f.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER), MakeInt32Value(f.factory, 0)}) ==
	        DUCKDB_V2_ERROR_QUERY_BINDER);
	// UNION members must be named; ENUM entries must be non-NULL VARCHAR.
	REQUIRE(MakeTypeErr(f.factory, "union", nullptr, {MakeTypeValue(f.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER)}) ==
	        DUCKDB_V2_ERROR_QUERY_BINDER);
	REQUIRE(MakeTypeErr(f.factory, "enum", nullptr, {MakeInt32Value(f.factory, 1)}) == DUCKDB_V2_ERROR_QUERY_BINDER);
	duckdb_v2_logical_type_handle varchar_type = nullptr;
	duckdb_v2_factory_create_type_from_id(f.factory, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR, nullptr, nullptr, 0,
	                                      &varchar_type, nullptr);
	duckdb_v2_value_handle null_entry = nullptr;
	duckdb_v2_value_create_null(f.factory, varchar_type, &null_entry, nullptr);
	duckdb_v2_logical_type_destroy(&varchar_type);
	REQUIRE(MakeTypeErr(f.factory, "enum", nullptr, {null_entry}) == DUCKDB_V2_ERROR_QUERY_BINDER);
	// STRUCT fields must be all named or all positional.
	std::vector<const char *> mixed = {"a", nullptr};
	REQUIRE(MakeTypeErr(f.factory, "struct", &mixed,
	                    {MakeTypeValue(f.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER),
	                     MakeTypeValue(f.factory, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR)}) == DUCKDB_V2_ERROR_QUERY_BINDER);
	// A parameterless built-in refuses params.
	REQUIRE(MakeTypeErr(f.factory, "integer", nullptr, {MakeInt32Value(f.factory, 1)}) == DUCKDB_V2_ERROR_QUERY_BINDER);
}

TEST_CASE("V2: logical_type_create is unqualified-only; from_text resolves qualified names",
          "[capi_v2][logical_type][create][from_text]") {
	// The documented contract, pinned in both directions so an
	// identifier-splitting "fix" cannot change behavior unnoticed.
	EnvFixture f;
	ExecSQL(f.conn, "CREATE TYPE mood AS ENUM('sad', 'ok', 'happy')");

	// A qualified name is one literal identifier to the generic
	// constructor: no catalog entry matches, so the lookup fails.
	REQUIRE(MakeTypeErr(f.factory, "main.mood", nullptr, {}) == DUCKDB_V2_ERROR_DATABASE_CATALOG);

	// The text path runs the real binder and resolves it.
	auto t = V2TypeFromText(f.factory, "main.mood");
	REQUIRE(V2TypeIdOf(t) == DUCKDB_V2_LOGICAL_TYPE_ID_ENUM);
	duckdb_v2_logical_type_destroy(&t);
}

TEST_CASE("V2: logical_type_create null-arg refusals", "[capi_v2][logical_type][create]") {
	EnvFixture f;
	duckdb_v2_logical_type_handle t = nullptr;

	// A null connection is refused.
	REQUIRE(LTCreateTypeFromName(nullptr, Convert("integer"), nullptr, nullptr, 0, &t, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);

	duckdb_v2_value_handle value = MakeInt32Value(f.factory, 1);
	const duckdb_v2_value_handle values[1] = {value};
	duckdb_v2_logical_type_handle out = nullptr;
	// Null out_type.
	REQUIRE(LTCreateTypeFromName(f.factory, Convert("integer"), nullptr, nullptr, 0, nullptr, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	// A null name handle.
	REQUIRE(duckdb_v2_factory_create_type_from_name(f.factory, nullptr, nullptr, nullptr, 0, &out, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(out == nullptr);
	// param_count > 0 with a null values array.
	REQUIRE(LTCreateTypeFromName(f.factory, Convert("list"), nullptr, nullptr, 1, &out, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(out == nullptr);
	// A null value handle inside the array.
	const duckdb_v2_value_handle holed[1] = {nullptr};
	REQUIRE(LTCreateTypeFromName(f.factory, Convert("list"), nullptr, holed, 1, &out, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(out == nullptr);
	// A malformed name view inside the names array.
	const duckdb_v2_str bad_names[1] = {{nullptr, 3}};
	REQUIRE(LTCreateTypeFromName(f.factory, Convert("list"), bad_names, values, 1, &out, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(out == nullptr);
	duckdb_v2_value_destroy(&value);
}

// Resolve types straight from a connection: get_from_text / get_from_args run
// the parse/bind in their own transaction on the connection's context.
TEST_CASE("V2: logical_type get_from_text / get_from_args", "[capi_v2][logical_type][create]") {
	EnvFixture f;

	// get_from_text: parse a parameterized kind straight from the connection.
	duckdb_v2_logical_type_handle dec = nullptr;
	auto text_str = Convert("DECIMAL(18,3)");
	REQUIRE(duckdb_v2_factory_create_type_from_text(f.factory, &text_str, &dec, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(dec != nullptr);
	DUCKDB_V2_LOGICAL_TYPE_ID dec_id = DUCKDB_V2_LOGICAL_TYPE_ID_INVALID;
	REQUIRE(duckdb_v2_logical_type_get_id(dec, &dec_id, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(dec_id == DUCKDB_V2_LOGICAL_TYPE_ID_DECIMAL);
	duckdb_v2_logical_type_destroy(&dec);

	// get_from_args: resolve a name plus a TYPE parameter (list(INTEGER)).
	duckdb_v2_logical_type_handle child = nullptr;
	REQUIRE(duckdb_v2_factory_create_type_from_id(f.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER, nullptr, nullptr, 0,
	                                              &child, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_value_handle child_type_value = nullptr;
	REQUIRE(duckdb_v2_value_create_type(f.factory, child, &child_type_value, nullptr) == DUCKDB_V2_ERROR_NONE);
	const duckdb_v2_value_handle params[1] = {child_type_value};
	duckdb_v2_logical_type_handle list = nullptr;
	REQUIRE(LTCreateTypeFromName(f.factory, Convert("list"), nullptr, params, 1, &list, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(list != nullptr);
	DUCKDB_V2_LOGICAL_TYPE_ID list_id = DUCKDB_V2_LOGICAL_TYPE_ID_INVALID;
	REQUIRE(duckdb_v2_logical_type_get_id(list, &list_id, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(list_id == DUCKDB_V2_LOGICAL_TYPE_ID_LIST);
	duckdb_v2_logical_type_destroy(&list);
	duckdb_v2_value_destroy(&child_type_value);
	duckdb_v2_logical_type_destroy(&child);

	// A null connection is refused on both.
	duckdb_v2_logical_type_handle out = nullptr;
	auto text_str2 = Convert("INTEGER");
	REQUIRE(duckdb_v2_factory_create_type_from_text(nullptr, &text_str2, &out, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(LTCreateTypeFromName(nullptr, Convert("integer"), nullptr, nullptr, 0, &out, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
}

TEST_CASE("V2: GEOMETRY with a coordinate system constructs and inspects",
          "[capi_v2][logical_type][create][param][geometry]") {
	EnvFixture f;

	// Shorthand: the in-tree default catalog ships OGC:CRS84 (and CRS83) as
	// COORDINATE_SYSTEM_ENTRY defaults; binding identifies the CRS and stores
	// its shortest form.
	auto geo = MakeType(f.factory, "geometry", nullptr, {MakeVarcharValue(f.factory, "OGC:CRS84")});
	REQUIRE(V2TypeIdOf(geo) == DUCKDB_V2_LOGICAL_TYPE_ID_GEOMETRY);
	REQUIRE(V2ParamCount(geo) == 1);
	REQUIRE(V2ParamVarchar(geo, 0, nullptr) == "OGC:CRS84");
	RequireParamRoundTrip(f.factory, geo);

	// to_text renders the CRS; from_text re-binds the same type.
	auto text = V2TypeText(geo);
	REQUIRE(text == "GEOMETRY('OGC:CRS84')");
	auto parsed = V2TypeFromText(f.factory, text);
	REQUIRE(V2TypesEqual(geo, parsed));
	duckdb_v2_logical_type_destroy(&parsed);
	duckdb_v2_logical_type_destroy(&geo);

	// A complete definition (here PROJJSON without an id) needs no resolver
	// at all: the identify fall-through keeps it verbatim.
	const char *projjson = "{\"type\":\"GeographicCRS\",\"name\":\"Test CRS\"}";
	auto complete = MakeType(f.factory, "geometry", nullptr, {MakeVarcharValue(f.factory, projjson)});
	REQUIRE(V2TypeIdOf(complete) == DUCKDB_V2_LOGICAL_TYPE_ID_GEOMETRY);
	REQUIRE(V2ParamCount(complete) == 1);
	REQUIRE(V2ParamVarchar(complete, 0, nullptr) == projjson);
	RequireParamRoundTrip(f.factory, complete);
	auto complete_parsed = V2TypeFromText(f.factory, V2TypeText(complete));
	REQUIRE(V2TypesEqual(complete, complete_parsed));
	duckdb_v2_logical_type_destroy(&complete_parsed);
	duckdb_v2_logical_type_destroy(&complete);
}

TEST_CASE("V2: GEOMETRY with an unknown coordinate system", "[capi_v2][logical_type][create][geometry]") {
	EnvFixture f;

	// A well-formed shorthand outside the default entries fails the bind.
	REQUIRE(MakeTypeErr(f.factory, "geometry", nullptr, {MakeVarcharValue(f.factory, "EPSG:4326")}) ==
	        DUCKDB_V2_ERROR_QUERY_BINDER);

	// ignore_unknown_crs degrades to the generic GEOMETRY type instead.
	ExecSQL(f.conn, "SET ignore_unknown_crs = true");
	auto geo = MakeType(f.factory, "geometry", nullptr, {MakeVarcharValue(f.factory, "EPSG:4326")});
	REQUIRE(V2TypeIdOf(geo) == DUCKDB_V2_LOGICAL_TYPE_ID_GEOMETRY);
	REQUIRE(V2ParamCount(geo) == 0);
	duckdb_v2_logical_type_destroy(&geo);
}

// ===========================================================================
// Storage-tier contract pins
//
// The DECIMAL and ENUM physical storage tiers are committed contract text
// in the vector module's view docstring, derivable from width / dictionary
// size; there are no getters. These pins assert the documented tables
// against the engine's own InternalType(), so the text cannot silently
// drift if core ever changes tiers.
// ===========================================================================

TEST_CASE("V2: DECIMAL storage tier by width matches the documented table", "[capi_v2][logical_type][storage_tier]") {
	EnvFixture f;
	struct {
		int width;
		duckdb::PhysicalType expected;
	} cases[] = {
	    {4, duckdb::PhysicalType::INT16},   {5, duckdb::PhysicalType::INT32},  {9, duckdb::PhysicalType::INT32},
	    {10, duckdb::PhysicalType::INT64},  {18, duckdb::PhysicalType::INT64}, {19, duckdb::PhysicalType::INT128},
	    {38, duckdb::PhysicalType::INT128},
	};
	for (auto &c : cases) {
		auto t = V2TypeFromText(f.factory, "DECIMAL(" + std::to_string(c.width) + ",2)");
		REQUIRE(V2LogicalType(t).InternalType() == c.expected);
		duckdb_v2_logical_type_destroy(&t);
	}
}

TEST_CASE("V2: ENUM storage tier by dictionary size matches the documented table",
          "[capi_v2][logical_type][storage_tier]") {
	EnvFixture f;
	struct {
		idx_t entries;
		duckdb::PhysicalType expected;
	} cases[] = {
	    {1, duckdb::PhysicalType::UINT8},      {255, duckdb::PhysicalType::UINT8},
	    {256, duckdb::PhysicalType::UINT16},   {65535, duckdb::PhysicalType::UINT16},
	    {65536, duckdb::PhysicalType::UINT32},
	};
	for (auto &c : cases) {
		std::vector<duckdb_v2_value_handle> values;
		values.reserve(c.entries);
		for (idx_t i = 0; i < c.entries; i++) {
			values.push_back(MakeVarcharValue(f.factory, ("v" + std::to_string(i)).c_str()));
		}
		auto t = MakeType(f.factory, "enum", nullptr, std::move(values));
		REQUIRE(V2LogicalType(t).InternalType() == c.expected);
		REQUIRE(V2ParamCount(t) == c.entries);
		duckdb_v2_logical_type_destroy(&t);
	}

	// Interop pin: the V1-only zero-entry enum (SQL forbids it) still
	// reports the uint8 tier through V2 inspection.
	const char *none[] = {nullptr};
	auto empty = MakeV1Enum(none, 0);
	REQUIRE(V2LogicalType(empty).InternalType() == duckdb::PhysicalType::UINT8);
	duckdb_v2_logical_type_destroy(&empty);
}

// Deliberate cross-version pin: the composite fixtures are V1-built and the
// V2 reconstruction is compared against them via logical_type_is_equal.
TEST_CASE("V2: create(name, params(t)) round-trips every constructible kind",
          "[capi_v2][logical_type][create][param]") {
	EnvFixture f;
	ExecSQL(f.conn, "CREATE TYPE mood AS ENUM('sad', 'ok', 'happy')");

	// Primitives: zero params, the rendered text is the name.
	DUCKDB_V2_LOGICAL_TYPE_ID primitives[] = {
	    DUCKDB_V2_LOGICAL_TYPE_ID_BOOLEAN,
	    DUCKDB_V2_LOGICAL_TYPE_ID_TINYINT,
	    DUCKDB_V2_LOGICAL_TYPE_ID_SMALLINT,
	    DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER,
	    DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT,
	    DUCKDB_V2_LOGICAL_TYPE_ID_UTINYINT,
	    DUCKDB_V2_LOGICAL_TYPE_ID_USMALLINT,
	    DUCKDB_V2_LOGICAL_TYPE_ID_UINTEGER,
	    DUCKDB_V2_LOGICAL_TYPE_ID_UBIGINT,
	    DUCKDB_V2_LOGICAL_TYPE_ID_HUGEINT,
	    DUCKDB_V2_LOGICAL_TYPE_ID_UHUGEINT,
	    DUCKDB_V2_LOGICAL_TYPE_ID_FLOAT,
	    DUCKDB_V2_LOGICAL_TYPE_ID_DOUBLE,
	    DUCKDB_V2_LOGICAL_TYPE_ID_DATE,
	    DUCKDB_V2_LOGICAL_TYPE_ID_TIME,
	    DUCKDB_V2_LOGICAL_TYPE_ID_TIME_TZ,
	    DUCKDB_V2_LOGICAL_TYPE_ID_TIME_NS,
	    DUCKDB_V2_LOGICAL_TYPE_ID_TIMESTAMP_SEC,
	    DUCKDB_V2_LOGICAL_TYPE_ID_TIMESTAMP_MS,
	    DUCKDB_V2_LOGICAL_TYPE_ID_TIMESTAMP,
	    DUCKDB_V2_LOGICAL_TYPE_ID_TIMESTAMP_NS,
	    DUCKDB_V2_LOGICAL_TYPE_ID_TIMESTAMP_TZ,
	    DUCKDB_V2_LOGICAL_TYPE_ID_TIMESTAMP_TZ_NS,
	    DUCKDB_V2_LOGICAL_TYPE_ID_INTERVAL,
	    DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR,
	    DUCKDB_V2_LOGICAL_TYPE_ID_BLOB,
	    DUCKDB_V2_LOGICAL_TYPE_ID_BIT,
	    DUCKDB_V2_LOGICAL_TYPE_ID_BIGNUM,
	    DUCKDB_V2_LOGICAL_TYPE_ID_UUID,
	};
	for (auto id : primitives) {
		duckdb_v2_logical_type_handle t = nullptr;
		REQUIRE(duckdb_v2_factory_create_type_from_id(f.factory, id, nullptr, nullptr, 0, &t, nullptr) ==
		        DUCKDB_V2_ERROR_NONE);
		RequireParamRoundTrip(f.factory, t);
		duckdb_v2_logical_type_destroy(&t);
	}

	// Composites, nesting included.
	auto dec = ConvertToV2(duckdb_create_decimal_type(38, 10));
	RequireParamRoundTrip(f.factory, dec);
	duckdb_v2_logical_type_destroy(&dec);

	auto int_v1 = duckdb_create_logical_type(DUCKDB_TYPE_INTEGER);
	auto list_v1 = duckdb_create_list_type(int_v1);
	auto nested = ConvertToV2(duckdb_create_list_type(list_v1));
	RequireParamRoundTrip(f.factory, nested);
	duckdb_v2_logical_type_destroy(&nested);

	auto arr = ConvertToV2(duckdb_create_array_type(list_v1, 7));
	duckdb_destroy_logical_type(&list_v1);
	RequireParamRoundTrip(f.factory, arr);
	duckdb_v2_logical_type_destroy(&arr);

	auto varchar_v1 = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
	auto map = ConvertToV2(duckdb_create_map_type(varchar_v1, int_v1));
	duckdb_destroy_logical_type(&int_v1);
	duckdb_destroy_logical_type(&varchar_v1);
	RequireParamRoundTrip(f.factory, map);
	duckdb_v2_logical_type_destroy(&map);

	auto s = MakeV1Struct();
	RequireParamRoundTrip(f.factory, s);
	duckdb_v2_logical_type_destroy(&s);

	auto u = MakeV1Union();
	RequireParamRoundTrip(f.factory, u);
	duckdb_v2_logical_type_destroy(&u);

	const char *enum_names[] = {"sad", "ok", "happy"};
	auto e = MakeV1Enum(enum_names, 3);
	RequireParamRoundTrip(f.factory, e);
	duckdb_v2_logical_type_destroy(&e);

	// Kinds reached through the other constructors.
	auto type_t = V2TypeFromText(f.factory, "TYPE");
	RequireParamRoundTrip(f.factory, type_t);
	duckdb_v2_logical_type_destroy(&type_t);

	auto geom = V2TypeFromText(f.factory, "GEOMETRY");
	RequireParamRoundTrip(f.factory, geom);
	duckdb_v2_logical_type_destroy(&geom);

	auto mood = V2TypeFromText(f.factory, "mood");
	RequireParamRoundTrip(f.factory, mood);
	duckdb_v2_logical_type_destroy(&mood);
}

// ===========================================================================
// Volume measurement (hidden): run explicitly via "[capi_v2_bench]"
// ===========================================================================

TEST_CASE("V2 bench: 100k-entry enum inspection cost", "[.][capi_v2_bench]") {
	EnvFixture f;
	constexpr idx_t N = 100000;
	std::vector<duckdb_v2_value_handle> entries;
	entries.reserve(N);
	for (idx_t i = 0; i < N; i++) {
		entries.push_back(MakeVarcharValue(f.factory, ("v" + std::to_string(i)).c_str()));
	}
	auto t = MakeType(f.factory, "enum", nullptr, std::move(entries));
	REQUIRE(V2ParamCount(t) == N);

	using bench_clock = std::chrono::steady_clock;

	// Generic path: one owned value per entry (3 crossings each). Failure
	// counters instead of per-iteration REQUIREs keep assertion overhead out
	// of the measurement.
	uint64_t failures = 0;
	size_t generic_bytes = 0;
	auto start = bench_clock::now();
	for (idx_t i = 0; i < N; i++) {
		duckdb_v2_str name = {nullptr, 0};
		duckdb_v2_value_handle v = nullptr;
		failures += (duckdb_v2_logical_type_get_param(t, i, &name, &v, nullptr) != DUCKDB_V2_ERROR_NONE);
		duckdb_v2_str bytes = {nullptr, 0};
		failures += (duckdb_v2_value_get_varchar(v, &bytes, nullptr) != DUCKDB_V2_ERROR_NONE);
		generic_bytes += bytes.len;
		duckdb_v2_value_destroy(&v);
	}
	auto generic_us = std::chrono::duration_cast<std::chrono::microseconds>(bench_clock::now() - start).count();
	REQUIRE(failures == 0);

	// Borrowed floor: direct engine dictionary access, what the removed
	// borrowed enum getter handed out per entry.
	size_t borrowed_bytes = 0;
	start = bench_clock::now();
	auto lt = V2LogicalType(t);
	auto &dict = duckdb::EnumType::GetValuesInsertOrder(lt);
	auto *data = duckdb::FlatVector::GetData<duckdb::string_t>(dict);
	for (idx_t i = 0; i < N; i++) {
		borrowed_bytes += data[i].GetSize();
	}
	auto borrowed_us = std::chrono::duration_cast<std::chrono::microseconds>(bench_clock::now() - start).count();
	REQUIRE(generic_bytes == borrowed_bytes);

	WARN("enum inspection, 100k entries: generic get_param " << generic_us << " us total (" << (generic_us * 1000.0 / N)
	                                                         << " ns/entry); borrowed engine floor " << borrowed_us
	                                                         << " us");
	duckdb_v2_logical_type_destroy(&t);
}

TEST_CASE("V2: create_type_from_name resolves a qualified name", "[capi_v2][logical_type]") {
	EnvFixture f;
	ExecSQL(f.conn, "CREATE SCHEMA s");
	ExecSQL(f.conn, "CREATE TYPE s.scoped AS INTEGER");

	auto resolve = [&](const std::vector<const char *> &parts, duckdb_v2_logical_type_handle *out) {
		std::vector<duckdb_v2_identifier_t> views;
		for (auto *part : parts) {
			views.push_back(Convert(part));
		}
		duckdb_v2_qname_handle qname = nullptr;
		REQUIRE(duckdb_v2_qname_create(views.data(), views.size(), &qname, nullptr) == DUCKDB_V2_ERROR_NONE);
		auto rc = duckdb_v2_factory_create_type_from_name(f.factory, qname, nullptr, nullptr, 0, out, nullptr);
		duckdb_v2_qname_destroy(&qname);
		return rc;
	};

	// The qualified name reaches a type the search path does not cover.
	duckdb_v2_logical_type_handle scoped = nullptr;
	REQUIRE(resolve({"s", "scoped"}, &scoped) == DUCKDB_V2_ERROR_NONE);
	DUCKDB_V2_LOGICAL_TYPE_ID id = DUCKDB_V2_LOGICAL_TYPE_ID_INVALID;
	REQUIRE(duckdb_v2_logical_type_get_id(scoped, &id, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(id == DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_logical_type_destroy(&scoped);

	// Unqualified, the same name is not on the search path.
	duckdb_v2_logical_type_handle missing = nullptr;
	REQUIRE(resolve({"scoped"}, &missing) != DUCKDB_V2_ERROR_NONE);
	REQUIRE(missing == nullptr);

	// A qualified name is resolved exactly: naming the wrong schema fails rather than falling back.
	REQUIRE(resolve({"main", "scoped"}, &missing) != DUCKDB_V2_ERROR_NONE);
	REQUIRE(missing == nullptr);

	// Built-in kinds still resolve unqualified, through the system catalog.
	duckdb_v2_logical_type_handle builtin = nullptr;
	REQUIRE(resolve({"integer"}, &builtin) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_logical_type_destroy(&builtin);
}

} // namespace test_capi_v2
