#include "duckdb/execution/operator/helper/physical_set.hpp"
#include "duckdb/catalog/catalog.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/settings.hpp"

namespace duckdb {

void PhysicalSet::SetGenericVariable(ClientContext &context, idx_t setting_index, SetScope scope, Value target_value) {
	Settings::Set(context, setting_index, scope, std::move(target_value));
}

void PhysicalSet::SetExtensionVariable(ClientContext &context, ExtensionOption &extension_option, SetScope scope,
                                       const Value &value) {
	Identifier name;
	for (auto &entry : DBConfig::GetConfig(context).GetExtensionSettings()) {
		if (entry.second.setting_index.GetIndex() == extension_option.setting_index.GetIndex()) {
			name = entry.first;
			break;
		}
	}
	D_ASSERT(!name.GetIdentifierName().empty());
	SetExtensionVariable(context, name, extension_option, scope, value);
}

void PhysicalSet::SetExtensionVariable(ClientContext &context, const Identifier &name,
                                       ExtensionOption &extension_option, SetScope scope, const Value &value) {
	auto &target_type = extension_option.type;
	Value target_value = value.CastAs(context, target_type);
	auto resolved_scope = extension_option.ResolveScope(name, scope);
	if (extension_option.set_function) {
		extension_option.set_function(context, scope, target_value);
	}
	if (extension_option.info) {
		extension_option.info->OnSet(context, resolved_scope, target_value, false);
	}
	// after the callbacks, which may still refuse the value
	if (extension_option.is_deprecated) {
		SettingCallbackInfo(context, resolved_scope).WarnDeprecated(name.GetIdentifierName());
	}
	scope = resolved_scope;
	auto setting_index = extension_option.setting_index.GetIndex();
	SetGenericVariable(context, setting_index, scope, std::move(target_value));
}

SetScope PhysicalSet::GetSettingScope(const ConfigurationOption &option, SetScope variable_scope) {
	if (variable_scope == SetScope::AUTOMATIC) {
		if (option.set_local) {
			return SetScope::SESSION;
		}
		if (option.set_global) {
			return SetScope::GLOBAL;
		}
		// generic setting
		switch (option.scope) {
		case SettingScopeTarget::LOCAL_ONLY:
		case SettingScopeTarget::LOCAL_DEFAULT:
			return SetScope::SESSION;
		case SettingScopeTarget::GLOBAL_ONLY:
		case SettingScopeTarget::GLOBAL_DEFAULT:
			return SetScope::GLOBAL;
		default:
			throw InvalidInputException("Setting \"%s\" does not have a valid scope defined", option.name);
		}
	}
	if (variable_scope == SetScope::SESSION && option.scope == SettingScopeTarget::GLOBAL_ONLY) {
		throw InvalidInputException("Setting \"%s\" cannot be set as a session variable - it can only be set globally",
		                            option.name);
	}
	if (variable_scope == SetScope::GLOBAL && option.scope == SettingScopeTarget::LOCAL_ONLY) {
		throw InvalidInputException(
		    "Setting \"%s\" cannot be set as a global variable - it can only be set per session", option.name);
	}
	return variable_scope;
}

void PhysicalSet::SetVariable(ClientContext &context, const Identifier &name, SetScope scope, const Value &value) {
	auto &config = DBConfig::GetConfig(context);
	// check if we are allowed to change the configuration option
	config.CheckLock(name);
	auto option = DBConfig::GetOptionByName(name);
	if (!option) {
		ExtensionOption extension_option;
		// check if this is an extra extension variable
		if (!config.TryGetExtensionOption(name, extension_option)) {
			auto extension_name = Catalog::AutoloadExtensionByConfigName(context, name);
			if (!config.TryGetExtensionOption(name, extension_option)) {
				throw InvalidInputException("Extension parameter %s was not found after autoloading", name);
			}
		}
		SetExtensionVariable(context, name, extension_option, scope, value);
		return;
	}
	SetScope variable_scope = GetSettingScope(*option, scope);

	Value input_val = value.CastAs(context, DBConfig::ParseLogicalType(option->parameter_type));
	if (option->default_value) {
		SettingCallbackInfo info(context, variable_scope);
		if (option->set_callback) {
			option->set_callback(info, input_val);
		}
		// after the callback, which may still refuse the value
		if (option->is_deprecated) {
			info.WarnDeprecated(option->name);
		}
		auto setting_index = option->setting_idx.GetIndex();
		SetGenericVariable(context, setting_index, variable_scope, std::move(input_val));
		return;
	}
	switch (variable_scope) {
	case SetScope::GLOBAL: {
		if (!option->set_global) {
			throw CatalogException("option %s cannot be set globally", name);
		}
		auto &db = DatabaseInstance::GetDatabase(context);
		config.SetOption(&db, *option, input_val);
		break;
	}
	case SetScope::SESSION:
		if (!option->set_local) {
			throw CatalogException("option %s cannot be set locally", name);
		}
		option->set_local(context, input_val);
		break;
	default:
		throw InternalException("Unsupported SetScope for variable");
	}
}

SourceResultType PhysicalSet::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                              OperatorSourceInput &input) const {
	SetVariable(context.client, Identifier(std::string_view(name.data(), name.size())), scope, value);
	return SourceResultType::FINISHED;
}

} // namespace duckdb
