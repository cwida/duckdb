#include "test_capi_v2.hpp"

#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// V2 custom option tests: register options of our own, then read and write
// them through a config and from SQL, and observe them through the option
// descriptors.
//
// Callbacks avoid Catch assertions: a REQUIRE would throw through the C
// callback boundary into the engine. On failure they populate the provided
// error slot and return; the failure then surfaces as a statement error the
// test asserts on.
// ---------------------------------------------------------------------------

namespace test_capi_v2 {

namespace {

duckdb_v2_identifier_t OptionIdent(const char *s) {
	return duckdb_v2_identifier_t {s, std::strlen(s)};
}

// What the update callback saw, carried in the option's user data.
struct UpdateLog {
	idx_t calls = 0;
	DUCKDB_V2_SETTING_SCOPE last_scope = DUCKDB_V2_SETTING_SCOPE_DEFAULT;
	bool last_reset = false;
	int64_t last_value = -1;
};

// Refuses negative values and clamps values above 100, recording every update.
void ClampUpdate(duckdb_v2_custom_option_update_info_handle info, duckdb_v2_context_handle context,
                 duckdb_v2_error_info_handle *err) {
	void *user_data = nullptr;
	duckdb_v2_value_handle value = nullptr;
	if (duckdb_v2_custom_option_update_get_user_data(info, &user_data, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_custom_option_update_get_value(info, &value, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto &log = *static_cast<UpdateLog *>(user_data);
	log.calls++;
	int64_t number = 0;
	auto rc = duckdb_v2_value_get_bigint(value, &number, err);
	duckdb_v2_value_destroy(&value);
	if (rc != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_custom_option_update_get_scope(info, &log.last_scope, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_custom_option_update_is_reset(info, &log.last_reset, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	if (number < 0) {
		duckdb_v2_error_info_set_code(*err, DUCKDB_V2_ERROR_INPUT_INVALID);
		auto text = Convert("cpu_budget cannot be negative");
		duckdb_v2_error_info_set_text(*err, &text);
		return;
	}
	if (number > 100) {
		duckdb_v2_factory_handle factory = nullptr;
		if (duckdb_v2_context_get_factory(context, &factory, err) != DUCKDB_V2_ERROR_NONE) {
			return;
		}
		duckdb_v2_value_handle clamped = nullptr;
		if (duckdb_v2_value_create_bigint(factory, 100, &clamped, err) != DUCKDB_V2_ERROR_NONE) {
			return;
		}
		duckdb_v2_custom_option_update_set_value(info, clamped, err);
		duckdb_v2_value_destroy(&clamped);
		number = 100;
	}
	log.last_value = number;
}

// Builds an option with a name and, optionally, a BIGINT type and default value.
duckdb_v2_custom_option_handle MakeOption(duckdb_v2_factory_handle factory, const char *name, bool typed,
                                          duckdb_v2_value_handle default_value) {
	duckdb_v2_custom_option_handle option = nullptr;
	REQUIRE(duckdb_v2_custom_option_create(factory, &option, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto name_str = OptionIdent(name);
	REQUIRE(duckdb_v2_custom_option_set_name(option, &name_str, nullptr) == DUCKDB_V2_ERROR_NONE);
	if (typed) {
		auto bigint = MakeType(factory, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT);
		REQUIRE(duckdb_v2_custom_option_set_type(option, bigint, nullptr) == DUCKDB_V2_ERROR_NONE);
		duckdb_v2_logical_type_destroy(&bigint);
	}
	if (default_value) {
		REQUIRE(duckdb_v2_custom_option_set_default_value(option, default_value, nullptr) == DUCKDB_V2_ERROR_NONE);
	}
	return option;
}

void RegisterOption(duckdb_v2_connection_handle conn, duckdb_v2_custom_option_handle option) {
	REQUIRE(duckdb_v2_connection_register_custom_option(conn, option, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_custom_option_destroy(&option);
}

DUCKDB_V2_ERROR RegisterOptionError(duckdb_v2_connection_handle conn, duckdb_v2_custom_option_handle option) {
	auto rc = duckdb_v2_connection_register_custom_option(conn, option, nullptr);
	duckdb_v2_custom_option_destroy(&option);
	return rc;
}

// Reads a BIGINT option through `config`, or -1 when it is NULL.
int64_t ReadOption(duckdb_v2_config_handle config, const char *name, DUCKDB_V2_SETTING_SCOPE *scope = nullptr) {
	auto name_str = OptionIdent(name);
	duckdb_v2_value_handle value = nullptr;
	DUCKDB_V2_SETTING_SCOPE read_scope = DUCKDB_V2_SETTING_SCOPE_DEFAULT;
	REQUIRE(duckdb_v2_config_get_option_value(config, &name_str, &value, &read_scope, nullptr) == DUCKDB_V2_ERROR_NONE);
	if (scope) {
		*scope = read_scope;
	}
	int64_t result = -1;
	duckdb_v2_value_get_bigint(value, &result, nullptr);
	duckdb_v2_value_destroy(&value);
	return result;
}

DUCKDB_V2_ERROR WriteOption(duckdb_v2_config_handle config, const char *name, duckdb_v2_value_handle value,
                            DUCKDB_V2_SETTING_SCOPE scope) {
	auto name_str = OptionIdent(name);
	return duckdb_v2_config_set_option_value(config, &name_str, value, scope, nullptr);
}

// The descriptor of an option, owned by the caller.
duckdb_v2_option_handle Describe(duckdb_v2_config_handle config, const char *name) {
	auto name_str = OptionIdent(name);
	duckdb_v2_option_handle option = nullptr;
	REQUIRE(duckdb_v2_config_get_option_by_name(config, &name_str, &option, nullptr) == DUCKDB_V2_ERROR_NONE);
	return option;
}

bool SupportsScope(duckdb_v2_option_handle option, DUCKDB_V2_SETTING_SCOPE scope) {
	bool supported = false;
	REQUIRE(duckdb_v2_option_supports_scope(option, scope, &supported, nullptr) == DUCKDB_V2_ERROR_NONE);
	return supported;
}

} // namespace

TEST_CASE("V2 custom option: registered options are read and written like any other", "[capi_v2][custom_option]") {
	EnvFixture fx;
	auto default_value = MakeInt64Value(fx.factory, 5);
	auto option = MakeOption(fx.factory, "my_budget", true, default_value);
	duckdb_v2_value_destroy(&default_value);
	auto description = Convert("The budget");
	REQUIRE(duckdb_v2_custom_option_set_description(option, &description, nullptr) == DUCKDB_V2_ERROR_NONE);
	RegisterOption(fx.conn, option);

	// The default value, until it is set.
	REQUIRE(ReadOption(fx.config, "my_budget") == 5);

	// From SQL, at the default scope: SESSION.
	REQUIRE(RunSql(fx.conn, "SET my_budget = 7") == DUCKDB_V2_ERROR_NONE);
	DUCKDB_V2_SETTING_SCOPE scope = DUCKDB_V2_SETTING_SCOPE_DEFAULT;
	REQUIRE(ReadOption(fx.config, "my_budget", &scope) == 7);
	REQUIRE(scope == DUCKDB_V2_SETTING_SCOPE_SESSION);
	// The value is cast to the option's type.
	REQUIRE(RunSql(fx.conn, "SET my_budget = '8'") == DUCKDB_V2_ERROR_NONE);
	REQUIRE(ReadOption(fx.config, "my_budget") == 8);

	// Through the config, globally: visible to a new connection.
	auto nine = MakeInt64Value(fx.factory, 9);
	REQUIRE(WriteOption(fx.instance_config, "my_budget", nine, DUCKDB_V2_SETTING_SCOPE_GLOBAL) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_value_destroy(&nine);
	duckdb_v2_connection_handle other = nullptr;
	REQUIRE(duckdb_v2_connection_create(fx.instance, &other, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_config_handle other_config = nullptr;
	REQUIRE(duckdb_v2_connection_get_config(other, &other_config, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(ReadOption(other_config, "my_budget", &scope) == 9);
	REQUIRE(scope == DUCKDB_V2_SETTING_SCOPE_GLOBAL);
	duckdb_v2_connection_destroy(&other);

	// A reset at the default scope, SESSION, writes the default value into the session.
	REQUIRE(RunSql(fx.conn, "RESET my_budget") == DUCKDB_V2_ERROR_NONE);
	REQUIRE(ReadOption(fx.config, "my_budget") == 5);

	// The descriptor reports what was registered.
	auto descriptor = Describe(fx.config, "my_budget");
	duckdb_v2_str text = {nullptr, 0};
	REQUIRE(duckdb_v2_option_get_description(descriptor, &text, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(Convert(text) == "The budget");
	REQUIRE(SupportsScope(descriptor, DUCKDB_V2_SETTING_SCOPE_GLOBAL));
	REQUIRE(SupportsScope(descriptor, DUCKDB_V2_SETTING_SCOPE_SESSION));
	duckdb_v2_option_destroy(&descriptor);
}

TEST_CASE("V2 custom option: the type can come from the default value", "[capi_v2][custom_option]") {
	EnvFixture fx;
	auto default_value = MakeInt64Value(fx.factory, 3);
	RegisterOption(fx.conn, MakeOption(fx.factory, "inferred", false, default_value));
	duckdb_v2_value_destroy(&default_value);

	REQUIRE(ReadOption(fx.config, "inferred") == 3);
	REQUIRE(RunSql(fx.conn, "SET inferred = '4'") == DUCKDB_V2_ERROR_NONE);
	REQUIRE(ReadOption(fx.config, "inferred") == 4);
	REQUIRE(RunSql(fx.conn, "SET inferred = 'not a number'") != DUCKDB_V2_ERROR_NONE);

	// Without a default value, the option reads as NULL until it is set.
	RegisterOption(fx.conn, MakeOption(fx.factory, "unset", true, nullptr));
	REQUIRE(ReadOption(fx.config, "unset") == -1);
}

TEST_CASE("V2 custom option: supported scopes are enforced", "[capi_v2][custom_option]") {
	EnvFixture fx;
	auto option = MakeOption(fx.factory, "global_only", true, nullptr);
	REQUIRE(duckdb_v2_custom_option_set_supports_scope(option, DUCKDB_V2_SETTING_SCOPE_SESSION, false, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	RegisterOption(fx.conn, option);

	// The default scope follows from the supported ones.
	auto descriptor = Describe(fx.config, "global_only");
	DUCKDB_V2_SETTING_SCOPE default_scope = DUCKDB_V2_SETTING_SCOPE_DEFAULT;
	REQUIRE(duckdb_v2_option_get_default_scope(descriptor, &default_scope, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(default_scope == DUCKDB_V2_SETTING_SCOPE_GLOBAL);
	REQUIRE(SupportsScope(descriptor, DUCKDB_V2_SETTING_SCOPE_GLOBAL));
	REQUIRE_FALSE(SupportsScope(descriptor, DUCKDB_V2_SETTING_SCOPE_SESSION));
	duckdb_v2_option_destroy(&descriptor);

	REQUIRE(RunSql(fx.conn, "SET global_only = 1") == DUCKDB_V2_ERROR_NONE);
	REQUIRE(RunSql(fx.conn, "SET GLOBAL global_only = 2") == DUCKDB_V2_ERROR_NONE);
	REQUIRE(RunSql(fx.conn, "SET SESSION global_only = 3") != DUCKDB_V2_ERROR_NONE);
	REQUIRE(RunSql(fx.conn, "RESET SESSION global_only") != DUCKDB_V2_ERROR_NONE);
	auto four = MakeInt64Value(fx.factory, 4);
	REQUIRE(WriteOption(fx.config, "global_only", four, DUCKDB_V2_SETTING_SCOPE_SESSION) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_value_destroy(&four);
	REQUIRE(ReadOption(fx.config, "global_only") == 2);
}

TEST_CASE("V2 custom option: the update callback validates and normalizes", "[capi_v2][custom_option]") {
	EnvFixture fx;
	auto log = new UpdateLog();
	auto default_value = MakeInt64Value(fx.factory, 10);
	auto option = MakeOption(fx.factory, "cpu_budget", true, default_value);
	duckdb_v2_value_destroy(&default_value);
	duckdb_v2_opaque user_data = {log, [](void *ptr) { delete static_cast<UpdateLog *>(ptr); }, nullptr};
	REQUIRE(duckdb_v2_custom_option_set_user_data(option, &user_data, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_custom_option_set_update_callback(option, ClampUpdate, nullptr) == DUCKDB_V2_ERROR_NONE);
	RegisterOption(fx.conn, option);

	// Not called for the default value.
	REQUIRE(log->calls == 0);

	REQUIRE(RunSql(fx.conn, "SET cpu_budget = 50") == DUCKDB_V2_ERROR_NONE);
	REQUIRE(log->calls == 1);
	REQUIRE(log->last_scope == DUCKDB_V2_SETTING_SCOPE_SESSION);
	REQUIRE_FALSE(log->last_reset);
	REQUIRE(ReadOption(fx.config, "cpu_budget") == 50);

	// A replaced value is what is stored.
	REQUIRE(RunSql(fx.conn, "SET GLOBAL cpu_budget = 500") == DUCKDB_V2_ERROR_NONE);
	REQUIRE(log->last_scope == DUCKDB_V2_SETTING_SCOPE_GLOBAL);
	REQUIRE(ReadOption(fx.instance_config, "cpu_budget") == 100);

	// A refused value is not stored, and its error surfaces.
	REQUIRE(RunSql(fx.conn, "SET cpu_budget = -1") == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(ReadOption(fx.config, "cpu_budget") == 50);
	auto negative = MakeInt64Value(fx.factory, -2);
	REQUIRE(WriteOption(fx.config, "cpu_budget", negative, DUCKDB_V2_SETTING_SCOPE_DEFAULT) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_value_destroy(&negative);

	// A reset passes the default value.
	REQUIRE(RunSql(fx.conn, "RESET cpu_budget") == DUCKDB_V2_ERROR_NONE);
	REQUIRE(log->last_reset);
	REQUIRE(log->last_value == 10);
}

TEST_CASE("V2 custom option: registration validates the option", "[capi_v2][custom_option]") {
	EnvFixture fx;

	SECTION("a name is required") {
		duckdb_v2_custom_option_handle option = nullptr;
		REQUIRE(duckdb_v2_custom_option_create(fx.factory, &option, nullptr) == DUCKDB_V2_ERROR_NONE);
		auto bigint = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT);
		REQUIRE(duckdb_v2_custom_option_set_type(option, bigint, nullptr) == DUCKDB_V2_ERROR_NONE);
		duckdb_v2_logical_type_destroy(&bigint);
		REQUIRE(RegisterOptionError(fx.conn, option) == DUCKDB_V2_ERROR_INPUT_INVALID);
	}
	SECTION("a type or a default value is required") {
		REQUIRE(RegisterOptionError(fx.conn, MakeOption(fx.factory, "untyped", false, nullptr)) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
	}
	SECTION("the default value must cast to the type") {
		auto text = MakeVarcharValue(fx.factory, "not a number");
		REQUIRE(RegisterOptionError(fx.conn, MakeOption(fx.factory, "bad_default", true, text)) !=
		        DUCKDB_V2_ERROR_NONE);
		duckdb_v2_value_destroy(&text);
	}
	SECTION("at least one scope must be supported") {
		auto option = MakeOption(fx.factory, "no_scope", true, nullptr);
		REQUIRE(duckdb_v2_custom_option_set_supports_scope(option, DUCKDB_V2_SETTING_SCOPE_GLOBAL, false, nullptr) ==
		        DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_custom_option_set_supports_scope(option, DUCKDB_V2_SETTING_SCOPE_SESSION, false, nullptr) ==
		        DUCKDB_V2_ERROR_NONE);
		REQUIRE(RegisterOptionError(fx.conn, option) == DUCKDB_V2_ERROR_INPUT_INVALID);
	}
	SECTION("the default scope must be supported") {
		auto option = MakeOption(fx.factory, "bad_scope", true, nullptr);
		REQUIRE(duckdb_v2_custom_option_set_supports_scope(option, DUCKDB_V2_SETTING_SCOPE_SESSION, false, nullptr) ==
		        DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_custom_option_set_default_scope(option, DUCKDB_V2_SETTING_SCOPE_SESSION, nullptr) ==
		        DUCKDB_V2_ERROR_NONE);
		REQUIRE(RegisterOptionError(fx.conn, option) == DUCKDB_V2_ERROR_INPUT_INVALID);
	}
	SECTION("DEFAULT is not a scope to configure") {
		auto option = MakeOption(fx.factory, "default_scope", true, nullptr);
		REQUIRE(duckdb_v2_custom_option_set_default_scope(option, DUCKDB_V2_SETTING_SCOPE_DEFAULT, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		REQUIRE(duckdb_v2_custom_option_set_supports_scope(option, DUCKDB_V2_SETTING_SCOPE_DEFAULT, false, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		duckdb_v2_custom_option_destroy(&option);
	}
	SECTION("a name can only be registered once") {
		RegisterOption(fx.conn, MakeOption(fx.factory, "taken", true, nullptr));
		REQUIRE(RegisterOptionError(fx.conn, MakeOption(fx.factory, "taken", true, nullptr)) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		// Nor can it take over a built-in option, or one of its aliases.
		REQUIRE(RegisterOptionError(fx.conn, MakeOption(fx.factory, "threads", true, nullptr)) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		REQUIRE(RegisterOptionError(fx.conn, MakeOption(fx.factory, "memory_limit", true, nullptr)) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
	}
	SECTION("null handles are rejected, and destroy is null-safe") {
		REQUIRE(duckdb_v2_connection_register_custom_option(fx.conn, nullptr, nullptr) != DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_custom_option_destroy(nullptr) == DUCKDB_V2_ERROR_NONE);
		duckdb_v2_custom_option_handle option = nullptr;
		REQUIRE(duckdb_v2_custom_option_destroy(&option) == DUCKDB_V2_ERROR_NONE);
	}
}

TEST_CASE("V2 custom option: registration is not transactional", "[capi_v2][custom_option]") {
	EnvFixture fx;
	REQUIRE(RunSql(fx.conn, "BEGIN") == DUCKDB_V2_ERROR_NONE);
	RegisterOption(fx.conn, MakeOption(fx.factory, "kept", true, nullptr));
	REQUIRE(RunSql(fx.conn, "ROLLBACK") == DUCKDB_V2_ERROR_NONE);
	REQUIRE(RunSql(fx.conn, "SET kept = 1") == DUCKDB_V2_ERROR_NONE);
	REQUIRE(ReadOption(fx.config, "kept") == 1);
}

} // namespace test_capi_v2
