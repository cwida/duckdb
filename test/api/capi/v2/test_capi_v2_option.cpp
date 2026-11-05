#include "test_capi_v2.hpp"

// ---------------------------------------------------------------------------
// V2 options: values written and read by name on an instance, connection or
// context, and described by a descriptor handle. Scope enforcement reuses
// PhysicalSet::GetSettingScope so error messages are DuckDB's own.
// ---------------------------------------------------------------------------

namespace test_capi_v2 {

namespace {

// A VARCHAR value holding SQL `SET` text.
struct TextValue {
	TextValue(duckdb_v2_factory_handle factory, const duckdb_v2_str *text) {
		REQUIRE(duckdb_v2_value_create_varchar(factory, text, &value, nullptr) == DUCKDB_V2_ERROR_NONE);
	}
	~TextValue() {
		duckdb_v2_value_destroy(&value);
	}
	duckdb_v2_value_handle value = nullptr;
};

// The value of a setting in `config`, rendered as text, plus the scope it was read from.
std::string Setting(duckdb_v2_config_handle config, const char *name, DUCKDB_V2_SETTING_SCOPE *out_scope = nullptr) {
	auto name_str = Convert(name);
	duckdb_v2_value_handle value = nullptr;
	DUCKDB_V2_SETTING_SCOPE scope = DUCKDB_V2_SETTING_SCOPE_DEFAULT;
	REQUIRE(duckdb_v2_config_get_option_value(config, &name_str, &value, &scope, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto result = Render(value);
	duckdb_v2_value_destroy(&value);
	if (out_scope) {
		*out_scope = scope;
	}
	return result;
}

// What the exec callback read through the context.
struct OptionProbe {
	std::string setting;
	DUCKDB_V2_SETTING_SCOPE scope = DUCKDB_V2_SETTING_SCOPE_DEFAULT;
	idx_t count = 0;
	DUCKDB_V2_ERROR unknown_rc = DUCKDB_V2_ERROR_NONE;
	DUCKDB_V2_ERROR by_index_rc = DUCKDB_V2_ERROR_NONE;
} option_probe;

void OptionProbeExec(duckdb_v2_scalar_function_exec_info_handle info, duckdb_v2_context_handle context,
                     duckdb_v2_error_info_handle *err) {
	duckdb_v2_config_handle config = nullptr;
	if (duckdb_v2_context_get_config(context, &config, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_option_handle opt = nullptr;
	auto name_str = Convert("max_execution_time");
	duckdb_v2_value_handle value = nullptr;
	DUCKDB_V2_SETTING_SCOPE scope = DUCKDB_V2_SETTING_SCOPE_DEFAULT;
	if (duckdb_v2_config_get_option_value(config, &name_str, &value, &scope, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	option_probe.setting = Render(value);
	option_probe.scope = scope;
	duckdb_v2_value_destroy(&value);

	if (duckdb_v2_config_get_option_count(config, &option_probe.count, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto name_str2 = Convert("no_such_option");
	option_probe.unknown_rc = duckdb_v2_config_get_option_by_name(config, &name_str2, &opt, nullptr);
	option_probe.by_index_rc = duckdb_v2_config_get_option_by_index(config, 0, &opt, nullptr);
	duckdb_v2_option_destroy(&opt);

	duckdb_v2_vector_handle out = nullptr;
	void *raw = nullptr;
	if (duckdb_v2_scalar_function_exec_get_result(info, &out, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_vector_get_data_mutable(out, &raw, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	static_cast<int32_t *>(raw)[0] = 1;
}

void RegisterOptionProbe(EnvFixture &fx) {
	auto integer = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_scalar_function_handle function = nullptr;
	REQUIRE(duckdb_v2_scalar_function_create(fx.factory, &function, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto name = Convert("probe_option");
	REQUIRE(duckdb_v2_scalar_function_set_name(function, &name, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_function_signature_handle sig = nullptr;
	REQUIRE(duckdb_v2_scalar_function_get_signature(function, &sig, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_function_signature_set_return_type(sig, integer, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_scalar_function_set_exec_callback(function, OptionProbeExec, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_scalar_function(fx.conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_scalar_function_destroy(&function) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_logical_type_destroy(&integer);
}

} // namespace

TEST_CASE("V2 db option: set + get round-trip", "[capi_v2][db][option]") {
	EnvFixture fx;

	// Read the default before mutating so we can compare against it.
	auto default_value = Setting(fx.instance_config, "memory_limit");
	auto name_str = Convert("memory_limit");
	auto setting_str = Convert("1GB");
	REQUIRE(duckdb_v2_config_set_option_value(fx.instance_config, &name_str, TextValue(fx.factory, &setting_str).value,
	                                          DUCKDB_V2_SETTING_SCOPE_DEFAULT, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto after = Setting(fx.instance_config, "memory_limit");
	REQUIRE(!after.empty());
	REQUIRE(after != default_value); // mutation visible
	// ... and visible to a connection, as a GLOBAL write.
	REQUIRE(Setting(fx.config, "memory_limit") == after);
}

TEST_CASE("V2 db option: get populates description and aliases", "[capi_v2][db][option]") {
	EnvFixture fx;

	duckdb_v2_option_handle opt = nullptr;
	auto name_str = Convert("memory_limit");
	REQUIRE(duckdb_v2_config_get_option_by_name(fx.instance_config, &name_str, &opt, nullptr) == DUCKDB_V2_ERROR_NONE);

	// "memory_limit" is an alias; the canonical name is "max_memory", and the alias list carries "memory_limit".
	duckdb_v2_str name = {nullptr, 0};
	duckdb_v2_option_get_name(opt, &name, nullptr);
	REQUIRE(name == "max_memory");
	idx_t alias_count = 0;
	duckdb_v2_option_get_alias_count(opt, &alias_count, nullptr);
	bool has_memory_limit = false;
	for (idx_t i = 0; i < alias_count; i++) {
		duckdb_v2_str alias = {nullptr, 0};
		duckdb_v2_option_get_alias(opt, i, &alias, nullptr);
		if (alias == "memory_limit") {
			has_memory_limit = true;
			break;
		}
	}
	REQUIRE(has_memory_limit);

	duckdb_v2_str desc = {nullptr, 0};
	duckdb_v2_option_get_description(opt, &desc, nullptr);
	REQUIRE(desc.ptr != nullptr);
	REQUIRE(desc.len != 0);

	duckdb_v2_option_destroy(&opt);

	// The supported scopes are reported; allow_community_extensions can only be written GLOBAL.
	auto name_str2 = Convert("allow_community_extensions");
	REQUIRE(duckdb_v2_config_get_option_by_name(fx.instance_config, &name_str2, &opt, nullptr) == DUCKDB_V2_ERROR_NONE);
	bool supported = false;
	REQUIRE(duckdb_v2_option_supports_scope(opt, DUCKDB_V2_SETTING_SCOPE_GLOBAL, &supported, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(supported);
	REQUIRE(duckdb_v2_option_supports_scope(opt, DUCKDB_V2_SETTING_SCOPE_SESSION, &supported, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE_FALSE(supported);
	DUCKDB_V2_SETTING_SCOPE scope = DUCKDB_V2_SETTING_SCOPE_DEFAULT;
	REQUIRE(duckdb_v2_option_get_default_scope(opt, &scope, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(scope == DUCKDB_V2_SETTING_SCOPE_GLOBAL);
	duckdb_v2_option_destroy(&opt);

	// max_execution_time can be written at either scope, and a bare SET writes the session.
	auto name_str3 = Convert("max_execution_time");
	REQUIRE(duckdb_v2_config_get_option_by_name(fx.instance_config, &name_str3, &opt, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_option_supports_scope(opt, DUCKDB_V2_SETTING_SCOPE_GLOBAL, &supported, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(supported);
	REQUIRE(duckdb_v2_option_supports_scope(opt, DUCKDB_V2_SETTING_SCOPE_SESSION, &supported, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(supported);
	REQUIRE(duckdb_v2_option_get_default_scope(opt, &scope, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(scope == DUCKDB_V2_SETTING_SCOPE_SESSION);
	duckdb_v2_option_destroy(&opt);
}

TEST_CASE("V2 db option: get unknown name errors", "[capi_v2][db][option]") {
	EnvFixture fx;
	duckdb_v2_option_handle out = nullptr;
	duckdb_v2_error_info_handle err = nullptr;
	auto name_str = Convert("this_option_does_not_exist");
	REQUIRE(duckdb_v2_config_get_option_by_name(fx.instance_config, &name_str, &out, &err) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(out == nullptr);
	REQUIRE(err != nullptr);
	duckdb_v2_error_info_destroy(&err);
}

TEST_CASE("V2 db option: get_option_count and get_option_by_index", "[capi_v2][db][option]") {
	EnvFixture fx;
	idx_t count = 0;
	REQUIRE(duckdb_v2_config_get_option_count(fx.instance_config, &count, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(count > 0);

	// Walk the first few entries — each should produce a populated handle.
	idx_t to_check = count < 5 ? count : 5;
	for (idx_t i = 0; i < to_check; i++) {
		duckdb_v2_option_handle opt = nullptr;
		REQUIRE(duckdb_v2_config_get_option_by_index(fx.instance_config, i, &opt, nullptr) == DUCKDB_V2_ERROR_NONE);
		duckdb_v2_str name = {nullptr, 0};
		duckdb_v2_option_get_name(opt, &name, nullptr);
		REQUIRE(name.ptr != nullptr);
		REQUIRE(name.len != 0);
		duckdb_v2_option_destroy(&opt);
	}

	duckdb_v2_option_handle out_of_range = nullptr;
	duckdb_v2_error_info_handle err = nullptr;
	REQUIRE(duckdb_v2_config_get_option_by_index(fx.instance_config, count + 100, &out_of_range, &err) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_error_info_destroy(&err);
}

TEST_CASE("V2 db option: set rejects an unparseable setting and a null value", "[capi_v2][db][option]") {
	EnvFixture fx;
	duckdb_v2_error_info_handle err = nullptr;
	auto name_str = Convert("threads");
	auto setting_str = Convert("lots");
	REQUIRE(duckdb_v2_config_set_option_value(fx.instance_config, &name_str, TextValue(fx.factory, &setting_str).value,
	                                          DUCKDB_V2_SETTING_SCOPE_DEFAULT, &err) != DUCKDB_V2_ERROR_NONE);
	REQUIRE(err != nullptr);
	duckdb_v2_error_info_destroy(&err);
	// A null value is caught up front.
	REQUIRE(duckdb_v2_config_set_option_value(fx.instance_config, &name_str, nullptr, DUCKDB_V2_SETTING_SCOPE_DEFAULT,
	                                          nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
}

TEST_CASE("V2 conn option: set SESSION is invisible to other connections", "[capi_v2][conn][option]") {
	EnvFixture fx;
	duckdb_v2_connection_handle other = nullptr;
	duckdb_v2_connection_create(fx.instance, &other, nullptr);
	duckdb_v2_config_handle other_config = nullptr;
	duckdb_v2_connection_get_config(other, &other_config, nullptr);

	// max_execution_time is LOCAL_DEFAULT, so a LOCAL-scope write stays
	// session-local — perfect for this test.
	auto name_str = Convert("max_execution_time");
	auto setting_str = Convert("5000");
	REQUIRE(duckdb_v2_config_set_option_value(fx.config, &name_str, TextValue(fx.factory, &setting_str).value,
	                                          DUCKDB_V2_SETTING_SCOPE_SESSION, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(Setting(fx.config, "max_execution_time") == "5000");
	// The other connection sees the static default ("0"), not "5000".
	REQUIRE(Setting(other_config, "max_execution_time") != "5000");

	duckdb_v2_connection_destroy(&other);
}

TEST_CASE("V2 conn option: set GLOBAL is visible everywhere", "[capi_v2][conn][option]") {
	EnvFixture fx;
	duckdb_v2_connection_handle other = nullptr;
	duckdb_v2_connection_create(fx.instance, &other, nullptr);
	duckdb_v2_config_handle other_config = nullptr;
	duckdb_v2_connection_get_config(other, &other_config, nullptr);

	auto name_str = Convert("memory_limit");
	auto setting_str = Convert("2GB");
	REQUIRE(duckdb_v2_config_set_option_value(fx.config, &name_str, TextValue(fx.factory, &setting_str).value,
	                                          DUCKDB_V2_SETTING_SCOPE_GLOBAL, nullptr) == DUCKDB_V2_ERROR_NONE);

	auto fx_setting = Setting(fx.config, "memory_limit");
	REQUIRE(!fx_setting.empty());
	REQUIRE(fx_setting == Setting(other_config, "memory_limit"));       // GLOBAL write seen identically by both
	REQUIRE(fx_setting == Setting(fx.instance_config, "memory_limit")); // ... and by the database itself

	duckdb_v2_connection_destroy(&other);
}

TEST_CASE("V2 conn option: scope enforcement matches SQL", "[capi_v2][conn][option]") {
	EnvFixture fx;
	duckdb_v2_error_info_handle err = nullptr;

	// GLOBAL_ONLY × LOCAL: rejected. allow_community_extensions is GLOBAL_ONLY.
	auto name_str = Convert("allow_community_extensions");
	auto setting_str = Convert("false");
	REQUIRE(duckdb_v2_config_set_option_value(fx.config, &name_str, TextValue(fx.factory, &setting_str).value,
	                                          DUCKDB_V2_SETTING_SCOPE_SESSION, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_error_info_destroy(&err);
}

TEST_CASE("V2 conn option: DEFAULT scope mirrors bare SQL `SET`", "[capi_v2][conn][option]") {
	EnvFixture fx;
	// max_execution_time is LOCAL_DEFAULT → AUTOMATIC resolves to SESSION → write succeeds.
	auto name_str = Convert("max_execution_time");
	auto setting_str = Convert("5000");
	REQUIRE(duckdb_v2_config_set_option_value(fx.config, &name_str, TextValue(fx.factory, &setting_str).value,
	                                          DUCKDB_V2_SETTING_SCOPE_DEFAULT, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(Setting(fx.config, "max_execution_time") == "5000");
}

TEST_CASE("V2 conn option: unknown name errors", "[capi_v2][conn][option]") {
	EnvFixture fx;
	duckdb_v2_error_info_handle err = nullptr;
	auto name_str = Convert("no_such_option_xyz");
	auto setting_str = Convert("1");
	REQUIRE(duckdb_v2_config_set_option_value(fx.config, &name_str, TextValue(fx.factory, &setting_str).value,
	                                          DUCKDB_V2_SETTING_SCOPE_DEFAULT, &err) != DUCKDB_V2_ERROR_NONE);
	REQUIRE(err != nullptr);
	duckdb_v2_error_info_destroy(&err);
	duckdb_v2_option_handle opt = nullptr;
	REQUIRE(duckdb_v2_config_get_option_by_name(fx.config, &name_str, &opt, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(opt == nullptr);
	duckdb_v2_error_info_destroy(&err);
}

TEST_CASE("V2 db option: startup options are applied when the instance is created", "[capi_v2][db][option]") {
	duckdb_v2_environment_handle env = nullptr;
	duckdb_v2_environment_create(&env, nullptr);

	SECTION("a startup option is applied, under its canonical name or an alias") {
		duckdb_v2_instance_handle plain = nullptr;
		REQUIRE(duckdb_v2_instance_create(env, &plain, nullptr) == DUCKDB_V2_ERROR_NONE);
		duckdb_v2_config_handle plain_config = nullptr;
		duckdb_v2_instance_get_config(plain, &plain_config, nullptr);
		auto default_value = Setting(plain_config, "memory_limit");
		duckdb_v2_instance_destroy(&plain);

		duckdb_v2_str names[] = {Convert("max_memory"), Convert("threads")};
		duckdb_v2_str values[] = {Convert("2GB"), Convert("3")};
		duckdb_v2_instance_handle instance = nullptr;
		REQUIRE(duckdb_v2_instance_create_with_options(env, names, values, 2, &instance, nullptr) ==
		        DUCKDB_V2_ERROR_NONE);
		duckdb_v2_config_handle instance_config = nullptr;
		duckdb_v2_instance_get_config(instance, &instance_config, nullptr);
		auto applied = Setting(instance_config, "memory_limit");
		REQUIRE(applied != default_value);
		REQUIRE(Setting(instance_config, "threads") == "3");

		duckdb_v2_connection_handle conn = nullptr;
		duckdb_v2_connection_create(instance, &conn, nullptr);
		duckdb_v2_config_handle conn_config = nullptr;
		duckdb_v2_connection_get_config(conn, &conn_config, nullptr);
		REQUIRE(Setting(conn_config, "memory_limit") == applied);
		duckdb_v2_connection_destroy(&conn);
		duckdb_v2_instance_destroy(&instance);
	}

	SECTION("a construction-time option is accepted at creation and rejected after") {
		auto path = duckdb::TestCreatePath("v2_option_readonly.db");
		duckdb::DeleteDatabase(path);
		{
			duckdb_v2_instance_handle instance = nullptr;
			OpenInstance(env, Convert(path), &instance, nullptr);
			duckdb_v2_connection_handle conn = nullptr;
			duckdb_v2_connection_create(instance, &conn, nullptr);
			ExecSQL(conn, "CREATE TABLE t(i INTEGER)");
			duckdb_v2_connection_destroy(&conn);
			duckdb_v2_instance_destroy(&instance);
		}

		auto name_str = Convert("access_mode");
		auto value_str = Convert("READ_ONLY");
		duckdb_v2_instance_handle instance = nullptr;
		REQUIRE(duckdb_v2_instance_create_with_options(env, &name_str, &value_str, 1, &instance, nullptr) ==
		        DUCKDB_V2_ERROR_NONE);
		auto path_str = Convert(path);
		REQUIRE(duckdb_v2_instance_attach(instance, nullptr, &path_str, nullptr, true, nullptr) ==
		        DUCKDB_V2_ERROR_NONE);
		duckdb_v2_connection_handle conn = nullptr;
		duckdb_v2_connection_create(instance, &conn, nullptr);

		duckdb_v2_result_handle r = nullptr;
		duckdb_v2_error_info_handle err = nullptr;
		REQUIRE(Query(conn, "INSERT INTO t VALUES (1)", &r, &err) != DUCKDB_V2_ERROR_NONE);
		duckdb_v2_error_info_destroy(&err);

		// Once the instance exists, access_mode can no longer change: the same error SET GLOBAL gives.
		duckdb_v2_config_handle instance_config = nullptr;
		duckdb_v2_instance_get_config(instance, &instance_config, nullptr);
		auto read_write = Convert("READ_WRITE");
		REQUIRE(
		    duckdb_v2_config_set_option_value(instance_config, &name_str, TextValue(FactoryOf(conn), &read_write).value,
		                                      DUCKDB_V2_SETTING_SCOPE_DEFAULT, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
		REQUIRE(err != nullptr);
		duckdb_v2_error_info_destroy(&err);

		duckdb_v2_connection_destroy(&conn);
		duckdb_v2_instance_destroy(&instance);
		duckdb::DeleteDatabase(path);
	}

	SECTION("an option no one claims fails creation") {
		auto name_str = Convert("no_such_option_xyz");
		auto value_str = Convert("1");
		duckdb_v2_instance_handle instance = nullptr;
		duckdb_v2_error_info_handle err = nullptr;
		REQUIRE(duckdb_v2_instance_create_with_options(env, &name_str, &value_str, 1, &instance, &err) !=
		        DUCKDB_V2_ERROR_NONE);
		REQUIRE(instance == nullptr);
		REQUIRE(err != nullptr);
		duckdb_v2_str msg = {nullptr, 0};
		duckdb_v2_error_info_get_text(err, &msg);
		REQUIRE(Convert(msg).find("no_such_option_xyz") != std::string::npos);
		duckdb_v2_error_info_destroy(&err);
	}

	SECTION("an option that needs a running instance is written through the config instead") {
		auto name_str = Convert("allowed_directories");
		auto value_str = Convert("[]");
		duckdb_v2_instance_handle instance = nullptr;
		REQUIRE(duckdb_v2_instance_create_with_options(env, &name_str, &value_str, 1, &instance, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		REQUIRE(instance == nullptr);
	}

	SECTION("malformed option arrays are rejected") {
		duckdb_v2_instance_handle instance = nullptr;
		auto name_str = Convert("threads");
		auto value_str = Convert("2");
		REQUIRE(duckdb_v2_instance_create_with_options(env, nullptr, &value_str, 1, &instance, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		REQUIRE(duckdb_v2_instance_create_with_options(env, &name_str, nullptr, 1, &instance, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		REQUIRE(instance == nullptr);
		// No options at all is the same as instance_create.
		REQUIRE(duckdb_v2_instance_create_with_options(env, nullptr, nullptr, 0, &instance, nullptr) ==
		        DUCKDB_V2_ERROR_NONE);
		duckdb_v2_instance_destroy(&instance);
	}

	duckdb_v2_environment_destroy(&env);
}

TEST_CASE("V2 context option: read through a context inside a callback", "[capi_v2][context][option]") {
	EnvFixture fx;
	option_probe = OptionProbe {};
	RegisterOptionProbe(fx);
	auto name_str = Convert("max_execution_time");
	auto setting_str = Convert("4242");
	REQUIRE(duckdb_v2_config_set_option_value(fx.config, &name_str, TextValue(fx.factory, &setting_str).value,
	                                          DUCKDB_V2_SETTING_SCOPE_SESSION, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_result_handle result = nullptr;
	REQUIRE(Query(fx.conn, "SELECT probe_option()", &result) == DUCKDB_V2_ERROR_NONE);
	auto chunk = StepChunk(result);
	REQUIRE(chunk != nullptr);
	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_result_destroy(&result);

	// The context sees the connection's LOCAL override, and the same option space.
	REQUIRE(option_probe.setting == "4242");
	REQUIRE(option_probe.scope == DUCKDB_V2_SETTING_SCOPE_SESSION);
	idx_t count = 0;
	duckdb_v2_config_get_option_count(fx.config, &count, nullptr);
	REQUIRE(option_probe.count == count);
	REQUIRE(option_probe.unknown_rc == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(option_probe.by_index_rc == DUCKDB_V2_ERROR_NONE);
}

TEST_CASE("V2 option: descriptor accessors", "[capi_v2][option]") {
	EnvFixture fx;
	duckdb_v2_option_handle opt = nullptr;
	auto name_str = Convert("threads");
	REQUIRE(duckdb_v2_config_get_option_by_name(fx.instance_config, &name_str, &opt, nullptr) == DUCKDB_V2_ERROR_NONE);

	SECTION("get_default_value is populated") {
		duckdb_v2_value_handle def = nullptr;
		REQUIRE(duckdb_v2_option_get_default_value(opt, &def, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(def != nullptr);
		duckdb_v2_value_destroy(&def);
	}

	SECTION("borrowed pointers stay stable across reads") {
		duckdb_v2_str first_name = {nullptr, 0};
		duckdb_v2_str second_name = {nullptr, 0};
		duckdb_v2_option_get_name(opt, &first_name, nullptr);
		duckdb_v2_option_get_name(opt, &second_name, nullptr);
		REQUIRE(first_name.ptr == second_name.ptr);
		REQUIRE(first_name == "threads");
	}

	SECTION("get_alias out-of-range surfaces a descriptive error") {
		duckdb_v2_str alias = {nullptr, 0};
		duckdb_v2_error_info_handle err = nullptr;
		REQUIRE(duckdb_v2_option_get_alias(opt, 99, &alias, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
		REQUIRE(alias.ptr == nullptr);
		REQUIRE(err != nullptr);
		duckdb_v2_str msg = {nullptr, 0};
		duckdb_v2_error_info_get_text(err, &msg);
		REQUIRE(Convert(msg).find("out of range") != std::string::npos);
		duckdb_v2_error_info_destroy(&err);
		// err == nullptr is tolerated on the failure path.
		REQUIRE(duckdb_v2_option_get_alias(opt, 99, &alias, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	}

	SECTION("destroy nulls the slot and is safe to repeat") {
		REQUIRE(duckdb_v2_option_destroy(&opt) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(opt == nullptr);
		REQUIRE(duckdb_v2_option_destroy(&opt) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_option_destroy(nullptr) == DUCKDB_V2_ERROR_NONE);
	}

	SECTION("handles are independent") {
		duckdb_v2_option_handle other = nullptr;
		auto name_str2 = Convert("memory_limit");
		REQUIRE(duckdb_v2_config_get_option_by_name(fx.instance_config, &name_str2, &other, nullptr) ==
		        DUCKDB_V2_ERROR_NONE);
		duckdb_v2_option_destroy(&other);
		duckdb_v2_str still = {nullptr, 0};
		REQUIRE(duckdb_v2_option_get_name(opt, &still, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(still == "threads");
	}

	duckdb_v2_option_destroy(&opt);
}

TEST_CASE("V2 option: accessor null-arg validation", "[capi_v2][option]") {
	EnvFixture fx;
	duckdb_v2_option_handle opt = nullptr;
	auto name_str = Convert("threads");
	REQUIRE(duckdb_v2_config_get_option_by_name(fx.instance_config, &name_str, &opt, nullptr) == DUCKDB_V2_ERROR_NONE);

	SECTION("get_name rejects null option") {
		duckdb_v2_str out = {nullptr, 0};
		REQUIRE(duckdb_v2_option_get_name(nullptr, &out, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	}
	SECTION("get_name rejects null out_name") {
		REQUIRE(duckdb_v2_option_get_name(opt, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	}
	SECTION("get_default_value rejects null option") {
		duckdb_v2_value_handle out = nullptr;
		REQUIRE(duckdb_v2_option_get_default_value(nullptr, &out, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	}
	SECTION("get_default_value rejects null out_value") {
		REQUIRE(duckdb_v2_option_get_default_value(opt, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	}
	SECTION("get_description rejects null option") {
		duckdb_v2_str out = {nullptr, 0};
		REQUIRE(duckdb_v2_option_get_description(nullptr, &out, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	}
	SECTION("supports_scope and get_default_scope reject null arguments") {
		bool supported = false;
		DUCKDB_V2_SETTING_SCOPE s = DUCKDB_V2_SETTING_SCOPE_DEFAULT;
		REQUIRE(duckdb_v2_option_supports_scope(nullptr, DUCKDB_V2_SETTING_SCOPE_GLOBAL, &supported, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		REQUIRE(duckdb_v2_option_supports_scope(opt, DUCKDB_V2_SETTING_SCOPE_GLOBAL, nullptr, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		REQUIRE(duckdb_v2_option_get_default_scope(nullptr, &s, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
		REQUIRE(duckdb_v2_option_get_default_scope(opt, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	}
	SECTION("get_alias_count rejects null option") {
		idx_t c;
		REQUIRE(duckdb_v2_option_get_alias_count(nullptr, &c, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	}
	SECTION("get_alias rejects null option") {
		duckdb_v2_str out = {nullptr, 0};
		REQUIRE(duckdb_v2_option_get_alias(nullptr, 0, &out, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	}
	SECTION("set_option / get_option reject null and malformed arguments") {
		auto setting_str = Convert("1");
		REQUIRE(duckdb_v2_config_set_option_value(nullptr, &name_str, TextValue(fx.factory, &setting_str).value,
		                                          DUCKDB_V2_SETTING_SCOPE_DEFAULT,
		                                          nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
		auto name_str2 = duckdb_v2_str {nullptr, 1};
		REQUIRE(duckdb_v2_config_set_option_value(
		            fx.instance_config, &name_str2, TextValue(fx.factory, &setting_str).value,
		            DUCKDB_V2_SETTING_SCOPE_DEFAULT, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
		duckdb_v2_option_handle out = nullptr;
		REQUIRE(duckdb_v2_config_get_option_by_name(fx.instance_config, &name_str, nullptr, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		REQUIRE(duckdb_v2_config_set_option_value(nullptr, &name_str, TextValue(fx.factory, &setting_str).value,
		                                          DUCKDB_V2_SETTING_SCOPE_DEFAULT,
		                                          nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
		REQUIRE(duckdb_v2_config_get_option_by_name(fx.config, &name_str, nullptr, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		REQUIRE(duckdb_v2_config_get_option_by_name(nullptr, &name_str, &out, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
	}

	duckdb_v2_option_destroy(&opt);
}

TEST_CASE("V2 option values report the scope they were read from", "[capi_v2][option]") {
	EnvFixture fx;
	DUCKDB_V2_SETTING_SCOPE scope = DUCKDB_V2_SETTING_SCOPE_DEFAULT;

	// An option nobody set reads as GLOBAL: its default.
	Setting(fx.config, "max_execution_time", &scope);
	REQUIRE(scope == DUCKDB_V2_SETTING_SCOPE_GLOBAL);

	// A session write is read back as SESSION by the connection, and leaves the instance's value alone.
	auto name_str = Convert("max_execution_time");
	auto session = Convert("5000");
	REQUIRE(duckdb_v2_config_set_option_value(fx.config, &name_str, TextValue(fx.factory, &session).value,
	                                          DUCKDB_V2_SETTING_SCOPE_SESSION, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(Setting(fx.config, "max_execution_time", &scope) == "5000");
	REQUIRE(scope == DUCKDB_V2_SETTING_SCOPE_SESSION);
	REQUIRE(Setting(fx.instance_config, "max_execution_time", &scope) != "5000");
	REQUIRE(scope == DUCKDB_V2_SETTING_SCOPE_GLOBAL);

	// A GLOBAL write through a context reaches every connection.
	auto global = Convert("4242");
	REQUIRE(duckdb_v2_config_set_option_value(fx.config, &name_str, TextValue(fx.factory, &global).value,
	                                          DUCKDB_V2_SETTING_SCOPE_GLOBAL, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_connection_handle other = nullptr;
	REQUIRE(duckdb_v2_connection_create(fx.instance, &other, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_config_handle other_config = nullptr;
	REQUIRE(duckdb_v2_connection_get_config(other, &other_config, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(Setting(other_config, "max_execution_time", &scope) == "4242");
	REQUIRE(scope == DUCKDB_V2_SETTING_SCOPE_GLOBAL);
	duckdb_v2_connection_destroy(&other);

	// An instance has no SESSION scope.
	REQUIRE(duckdb_v2_config_set_option_value(fx.instance_config, &name_str, TextValue(fx.factory, &global).value,
	                                          DUCKDB_V2_SETTING_SCOPE_SESSION,
	                                          nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	// The text setter is the one-call form, and casts like SQL `SET`.
	auto text = Convert("1234");
	REQUIRE(duckdb_v2_config_set_option_text(fx.config, &name_str, &text, DUCKDB_V2_SETTING_SCOPE_SESSION, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(Setting(fx.config, "max_execution_time", &scope) == "1234");
	REQUIRE(scope == DUCKDB_V2_SETTING_SCOPE_SESSION);
	auto bad = Convert("not-a-number");
	REQUIRE(duckdb_v2_config_set_option_text(fx.config, &name_str, &bad, DUCKDB_V2_SETTING_SCOPE_SESSION, nullptr) !=
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_config_set_option_text(nullptr, &name_str, &text, DUCKDB_V2_SETTING_SCOPE_SESSION, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_config_set_option_text(fx.config, &name_str, nullptr, DUCKDB_V2_SETTING_SCOPE_SESSION, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);

	// Unknown names fail on every read.
	duckdb_v2_value_handle value = nullptr;
	auto unknown = Convert("no_such_option");
	REQUIRE(duckdb_v2_config_get_option_value(fx.config, &unknown, &value, &scope, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_config_get_option_value(fx.instance_config, &unknown, &value, &scope, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(value == nullptr);
}

} // namespace test_capi_v2
