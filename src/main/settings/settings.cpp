#include "duckdb/main/settings.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/context.hpp"

namespace duckdb {

bool Settings::TryGetSettingInternal(const ClientContext &context, idx_t setting_index, Value &result) {
	return context.TryGetCurrentUserSetting(setting_index, result);
}

bool Settings::TryGetSettingInternal(const Context &context, idx_t setting_index, Value &result) {
	return context.TryGetCurrentUserSetting(setting_index, result);
}

bool Settings::TryGetSettingInternal(const DBConfig &config, idx_t setting_index, Value &result) {
	auto lookup_result = config.TryGetCurrentUserSetting(setting_index, result);
	return lookup_result;
}

bool Settings::TryGetSettingInternal(const DatabaseInstance &db, idx_t setting_index, Value &result) {
	return TryGetSettingInternal(DBConfig::GetConfig(db), setting_index, result);
}

void Settings::SetSettingInternal(DatabaseInstance &db, idx_t setting_index, SetScope scope, Value target_value) {
	SetSettingInternal(DBConfig::GetConfig(db), setting_index, scope, std::move(target_value));
}

void Settings::SetSettingInternal(DBConfig &config, idx_t setting_index, SetScope scope, Value target_value) {
	if (scope != SetScope::GLOBAL) {
		throw InternalException(
		    "Attempting to set setting with index %d with non-global scope, but no ClientContext was provided",
		    setting_index);
	}
	config.SetOption(setting_index, std::move(target_value));
}

void Settings::SetSettingInternal(ClientContext &context, idx_t setting_index, SetScope scope, Value target_value) {
	if (scope == SetScope::GLOBAL) {
		SetSettingInternal(DBConfig::GetConfig(context), setting_index, scope, std::move(target_value));
		return;
	}
	auto &client_config = ClientConfig::GetConfig(context);
	client_config.user_settings.SetUserSetting(setting_index, std::move(target_value));
}

} // namespace duckdb
