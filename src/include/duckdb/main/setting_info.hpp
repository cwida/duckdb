//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/setting_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/enums/set_scope.hpp"

namespace duckdb {
class ClientContext;
class DatabaseInstance;
struct DBConfig;

enum class SettingScope : uint8_t {
	//! Setting is from the global Setting scope
	GLOBAL,
	//! Setting is from the local Setting scope
	LOCAL,
	//! Setting was not fetched from settings, but it was fetched from a secret instead
	SECRET,
	//! The setting was not found or invalid in some other way
	INVALID
};

enum class SettingScopeTarget {
	INVALID,
	//! Setting can be set in global scope only
	GLOBAL_ONLY,
	//! Setting can be set in local scope only
	LOCAL_ONLY,
	//! Setting can be set in both scopes - but defaults to global
	GLOBAL_DEFAULT,
	//! Setting can be set in both scopes - but defaults to local
	LOCAL_DEFAULT
};

struct SettingLookupResult {
public:
	SettingLookupResult() : scope(SettingScope::INVALID) {
	}
	explicit SettingLookupResult(SettingScope scope) : scope(scope) {
		D_ASSERT(scope != SettingScope::INVALID);
	}

public:
	operator bool() { // NOLINT: allow implicit conversion to bool
		return scope != SettingScope::INVALID;
	}

public:
	SettingScope GetScope() {
		D_ASSERT(scope != SettingScope::INVALID);
		return scope;
	}

private:
	SettingScope scope = SettingScope::INVALID;
};

struct SettingCallbackInfo {
	explicit SettingCallbackInfo(ClientContext &context, SetScope scope);
	explicit SettingCallbackInfo(DBConfig &config, optional_ptr<DatabaseInstance> db);

	DBConfig &config;
	optional_ptr<DatabaseInstance> db;
	optional_ptr<ClientContext> context;
	SetScope scope;
	//! Whether the callback fires for a RESET rather than a SET
	bool is_reset = false;

public:
	//! Logs that the setting is deprecated; resetting it back to its default does not
	DUCKDB_API void WarnDeprecated(const string &name) const;
};

typedef void (*set_callback_t)(SettingCallbackInfo &info, Value &parameter);
typedef void (*set_global_function_t)(DatabaseInstance *db, DBConfig &config, const Value &parameter);
typedef void (*set_local_function_t)(ClientContext &context, const Value &parameter);
typedef void (*reset_global_function_t)(DatabaseInstance *db, DBConfig &config);
typedef void (*reset_local_function_t)(ClientContext &context);
typedef Value (*get_setting_function_t)(const ClientContext &context);

struct ConfigurationOption {
	const char *name;
	const char *description;
	const char *parameter_type;
	set_global_function_t set_global;
	set_local_function_t set_local;
	reset_global_function_t reset_global;
	reset_local_function_t reset_local;
	get_setting_function_t get_setting;
	SettingScopeTarget scope;
	const char *default_value;
	set_callback_t set_callback;
	optional_idx setting_idx;
	bool is_debug = false;
	bool is_deprecated = false;
};

struct ConfigurationAlias {
	const char *alias;
	//! The name of the setting this alias refers to
	const char *setting_name;
};

typedef void (*set_option_callback_t)(ClientContext &context, SetScope scope, Value &parameter);

//! Extra state and behavior of an extension option
struct ExtensionOptionInfo {
	DUCKDB_API virtual ~ExtensionOptionInfo();

	//! Called when the option is set or reset, with the scope resolved to SESSION or GLOBAL, before the value is
	//! stored. May replace the value, or throw to refuse it
	virtual void OnSet(ClientContext &context, SetScope scope, Value &parameter, bool is_reset) const = 0;

	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}
};

struct ExtensionOption {
	ExtensionOption() : set_function(nullptr), default_scope(SetScope::AUTOMATIC) {
	}
	// NOLINTNEXTLINE: work around bug in clang-tidy
	ExtensionOption(string description_p, LogicalType type_p, set_option_callback_t set_function_p,
	                Value default_value_p, SetScope default_scope_p, bool is_debug_p, bool is_deprecated_p)
	    : description(std::move(description_p)), type(std::move(type_p)), set_function(set_function_p),
	      default_value(std::move(default_value_p)), default_scope(default_scope_p), is_debug(is_debug_p),
	      is_deprecated(is_deprecated_p) {
	}

	string description;
	LogicalType type;
	set_option_callback_t set_function;
	Value default_value;
	SetScope default_scope;
	optional_idx setting_index;
	bool is_debug = false;
	bool is_deprecated = false;
	//! Whether the option can be set globally, and per session
	bool supports_global = true;
	bool supports_session = true;
	//! Optional extra state and behavior, invoked after set_function
	shared_ptr<ExtensionOptionInfo> info;

public:
	//! Resolves AUTOMATIC to the default scope, and throws when the option cannot be set at the resulting scope
	DUCKDB_API SetScope ResolveScope(const Identifier &name, SetScope scope) const;
};

} // namespace duckdb
