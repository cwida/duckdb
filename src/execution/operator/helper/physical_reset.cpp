#include "duckdb/execution/operator/helper/physical_reset.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/execution/operator/helper/physical_set.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

void PhysicalReset::ResetExtensionVariable(ExecutionContext &context, DBConfig &config,
                                           ExtensionOption &extension_option) const {
	auto effective_scope = extension_option.ResolveScope(Identifier(name.ToStdString()), scope);
	if (extension_option.set_function) {
		extension_option.set_function(context.client, effective_scope, extension_option.default_value);
	}
	if (extension_option.info) {
		extension_option.info->OnSet(context.client, effective_scope, extension_option.default_value, true);
	}
	if (effective_scope == SetScope::GLOBAL) {
		config.ResetOption(extension_option);
	} else {
		auto &client_config = ClientConfig::GetConfig(context.client);
		auto setting_index = extension_option.setting_index.GetIndex();
		client_config.user_settings.SetUserSetting(setting_index, extension_option.default_value);
	}
}

SourceResultType PhysicalReset::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                OperatorSourceInput &input) const {
	if (scope == SetScope::VARIABLE) {
		auto &client_config = ClientConfig::GetConfig(context.client);
		client_config.ResetUserVariable(name);
		return SourceResultType::FINISHED;
	}
	auto &config = DBConfig::GetConfig(context.client);
	Identifier option_name(name.ToStdString());
	config.CheckLock(option_name);
	auto option = DBConfig::GetOptionByName(option_name);
	if (!option) {
		// check if this is an extra extension variable
		ExtensionOption extension_option;
		if (!config.TryGetExtensionOption(option_name, extension_option)) {
			auto extension_name = Catalog::AutoloadExtensionByConfigName(context.client, option_name);
			if (!config.TryGetExtensionOption(option_name, extension_option)) {
				throw InvalidInputException("Extension parameter %s was not found after autoloading", name);
			}
		}
		ResetExtensionVariable(context, config, extension_option);
		return SourceResultType::FINISHED;
	}

	// Transform scope
	SetScope variable_scope = PhysicalSet::GetSettingScope(*option, scope);

	if (option->default_value) {
		if (option->set_callback) {
			SettingCallbackInfo info(context.client, variable_scope);
			info.is_reset = true;
			auto parameter_type = DBConfig::ParseLogicalType(option->parameter_type);
			Value reset_val = Value(option->default_value).CastAs(context.client, parameter_type);
			option->set_callback(info, reset_val);
		}
		auto setting_index = option->setting_idx.GetIndex();
		if (variable_scope == SetScope::SESSION) {
			auto &client_config = ClientConfig::GetConfig(context.client);
			client_config.user_settings.ClearSetting(setting_index);
		} else {
			config.ResetGenericOption(setting_index);
		}
		return SourceResultType::FINISHED;
	}
	switch (variable_scope) {
	case SetScope::GLOBAL: {
		if (!option->set_global) {
			throw CatalogException("option \"%s\" cannot be reset globally", name.ToStdString());
		}
		auto &db = DatabaseInstance::GetDatabase(context.client);
		config.ResetOption(&db, *option);
		break;
	}
	case SetScope::SESSION:
		if (!option->reset_local) {
			throw CatalogException("option \"%s\" cannot be reset locally", name.ToStdString());
		}
		option->reset_local(context.client);
		break;
	default:
		throw InternalException("Unsupported SetScope for variable");
	}

	return SourceResultType::FINISHED;
}

} // namespace duckdb
