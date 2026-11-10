#include "duckdb/main/settings.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/context.hpp"

namespace duckdb {

vector<string> Settings::SplitListDefault(const char *default_value) {
	string input(default_value);
	StringUtil::Trim(input);
	if (input.size() < 2 || input.front() != '[' || input.back() != ']') {
		throw InternalException("Default value \"%s\" of a list setting is not a list literal", default_value);
	}
	vector<string> result;
	string entry;
	bool in_quotes = false;
	bool has_entry = false;
	for (idx_t i = 1; i + 1 < input.size(); i++) {
		auto c = input[i];
		if (in_quotes) {
			if (c != '\'') {
				entry += c;
			} else if (i + 2 < input.size() && input[i + 1] == '\'') {
				// an escaped quote
				entry += c;
				i++;
			} else {
				in_quotes = false;
			}
		} else if (c == '\'') {
			in_quotes = true;
			has_entry = true;
		} else if (c == ',') {
			result.push_back(std::move(entry));
			entry.clear();
			has_entry = false;
		} else if (!StringUtil::CharacterIsSpace(c)) {
			entry += c;
			has_entry = true;
		}
	}
	if (in_quotes) {
		throw InternalException("Default value \"%s\" of a list setting has an unterminated quote", default_value);
	}
	if (has_entry || !result.empty()) {
		result.push_back(std::move(entry));
	}
	return result;
}

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
