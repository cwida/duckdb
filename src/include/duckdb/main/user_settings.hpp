//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/user_settings.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/setting_info.hpp"
#include "duckdb/common/atomic.hpp"

namespace duckdb {

struct GenericSetting {
	GenericSetting() : is_set(false) {
	}

	bool is_set;
	Value value;
};

struct UserSettingsMap {
public:
	void SetUserSetting(idx_t setting_index, Value target_value);
	void ClearSetting(idx_t setting_index);
	bool IsSet(idx_t setting_index) const;
	bool TryGetSetting(idx_t setting_index, Value &result_value) const;

private:
	vector<GenericSetting> settings;
};

#ifndef __MINGW32__
struct GlobalUserSettings;

struct CachedGlobalSettings {
	CachedGlobalSettings();
	CachedGlobalSettings(const GlobalUserSettings &global_user_settings, idx_t version, UserSettingsMap settings,
	                     const identifier_map_t<ExtensionOption> &extension_parameters);

	optional_ptr<const GlobalUserSettings> global_user_settings;
	idx_t version;
	UserSettingsMap settings;
	identifier_map_t<idx_t> extension_setting_indexes;
	hugeint_t uuid;
};
#endif

struct GlobalUserSettings {
public:
	GlobalUserSettings();
	// enable copy constructors
	GlobalUserSettings(const GlobalUserSettings &other);
	GlobalUserSettings &operator=(const GlobalUserSettings &);

	void SetUserSetting(idx_t setting_index, Value target_value);
	void ClearSetting(idx_t setting_index);
	bool IsSet(idx_t setting_index) const;
	SettingLookupResult TryGetSetting(idx_t setting_index, Value &result_value) const;
	bool HasExtensionOption(const Identifier &name) const;
	idx_t AddExtensionOption(const Identifier &name, ExtensionOption extension_option);
	identifier_map_t<ExtensionOption> GetExtensionSettings() const;
	bool TryGetExtensionOption(const Identifier &name, ExtensionOption &result) const;
	optional_idx TryGetExtensionSettingIndex(const Identifier &name) const;
	hugeint_t GetUUID() const;

#ifndef __MINGW32__
	CachedGlobalSettings &GetSettings() const;
#endif

private:
	mutable mutex lock;
	//! Database-global settings
	UserSettingsMap settings_map;
	//! Extra parameters that can be SET for loaded extensions
	identifier_map_t<ExtensionOption> extension_parameters;
	//! Current version of the settings - incremented when settings are modified
	atomic<idx_t> settings_version;
	//! Settings uuid
	hugeint_t uuid;
};

struct LocalUserSettings {
public:
	~LocalUserSettings();

	void SetUserSetting(idx_t setting_index, Value target_value);
	void ClearSetting(idx_t setting_index);
	bool IsSet(idx_t setting_index) const;
	SettingLookupResult TryGetSetting(const GlobalUserSettings &global_settings, idx_t setting_index,
	                                  Value &result_value) const;

private:
	//! Client-local settings
	UserSettingsMap settings_map;
};
} // namespace duckdb
