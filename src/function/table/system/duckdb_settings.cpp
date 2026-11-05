#include "duckdb/function/table/system_functions.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/common/enum_util.hpp"

namespace duckdb {

struct DuckDBSettingValue {
	Identifier name;
	Value value;
	string description;
	string input_type;
	string scope;
	vector<Value> aliases;

	inline bool operator<(const DuckDBSettingValue &rhs) const {
		return name < rhs.name;
	};
};

struct DuckDBSettingsData : public GlobalTableFunctionState {
	DuckDBSettingsData() : offset(0) {
	}

	vector<DuckDBSettingValue> settings;
	idx_t offset;
};

struct DuckDBSettingsBindData : public TableFunctionData {
	bool debug = false;
	bool deprecated = false;
	//! Ignore the debug and deprecated filters
	bool include_all = false;

	bool Includes(bool is_debug, bool is_deprecated) const {
		return include_all || (debug == is_debug && deprecated == is_deprecated);
	}
};

static unique_ptr<FunctionData> DuckDBSettingsBind(ClientContext &context, TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types, vector<Identifier> &names) {
	names.emplace_back("name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("value");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("description");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("input_type");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("scope");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("aliases");
	return_types.emplace_back(LogicalType::LIST(LogicalType::VARCHAR));

	names.emplace_back("typed_value");
	return_types.emplace_back(LogicalType::VARIANT());

	auto result = make_uniq<DuckDBSettingsBindData>();
	for (auto name : {"debug", "deprecated", "include_all"}) {
		auto it = input.named_parameters.find(name);
		if (it != input.named_parameters.end() && it->second.IsNull()) {
			throw InvalidInputException("Cannot use NULL as argument for %s", name);
		}
	}
	if (auto it = input.named_parameters.find("debug"); it != input.named_parameters.end()) {
		result->debug = it->second.GetValue<bool>();
	}

	if (auto it = input.named_parameters.find("deprecated"); it != input.named_parameters.end()) {
		result->deprecated = it->second.GetValue<bool>();
	}

	if (auto it = input.named_parameters.find("include_all"); it != input.named_parameters.end()) {
		result->include_all = it->second.GetValue<bool>();
	}
	auto &params = input.named_parameters;
	if (result->include_all && (params.find("debug") != params.end() || params.find("deprecated") != params.end())) {
		throw InvalidInputException("Cannot combine include_all := true with the debug or deprecated filters");
	}

	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> DuckDBSettingsInit(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<DuckDBSettingsData>();
	auto &bind_data = input.bind_data->Cast<DuckDBSettingsBindData>();

	identifier_map_t<vector<Value>> aliases;
	for (idx_t i = 0; i < DBConfig::GetAliasCount(); i++) {
		auto alias = DBConfig::GetAliasByIndex(i);
		aliases[alias->setting_name].emplace_back(alias->alias);
	}

	auto &config = DBConfig::GetConfig(context);
	auto options_count = DBConfig::GetOptionCount();
	for (idx_t i = 0; i < options_count; i++) {
		auto option = DBConfig::GetOptionByIndex(i);
		D_ASSERT(option);
		if (!bind_data.Includes(option->is_debug, option->is_deprecated)) {
			continue;
		}
		DuckDBSettingValue value;
		auto scope = option->set_global ? SettingScope::GLOBAL : SettingScope::LOCAL;
		value.name = option->name;
		if (option->get_setting) {
			value.value = option->get_setting(context);
		} else {
			auto lookup_result = context.TryGetCurrentSetting(value.name, value.value);
			if (lookup_result) {
				scope = lookup_result.GetScope();
			} else {
				value.value = option->default_value;
			}
		}
		value.description = option->description;
		value.input_type = option->parameter_type;
		value.scope = EnumUtil::ToString(scope);
		auto entry = aliases.find(option->name);
		if (entry != aliases.end()) {
			value.aliases = std::move(entry->second);
		}
		for (auto &alias : value.aliases) {
			DuckDBSettingValue alias_value = value;
			alias_value.name = alias.GetValue<Identifier>();
			alias_value.aliases.clear();
			result->settings.push_back(std::move(alias_value));
		}
		result->settings.push_back(std::move(value));
	}
	for (auto &ext_param : config.GetExtensionSettings()) {
		if (!bind_data.Includes(ext_param.second.is_debug, ext_param.second.is_deprecated)) {
			continue;
		}
		Value setting_val;
		auto scope = SettingScope::GLOBAL;
		auto lookup_result = context.TryGetCurrentSetting(ext_param.first, setting_val);
		if (lookup_result) {
			scope = lookup_result.GetScope();
		}
		DuckDBSettingValue value;
		value.name = ext_param.first;
		value.value = std::move(setting_val);
		value.description = ext_param.second.description;
		value.input_type = ext_param.second.type.ToString();
		value.scope = EnumUtil::ToString(scope);

		result->settings.push_back(std::move(value));
	}
	std::sort(result->settings.begin(), result->settings.end());
	return std::move(result);
}

void DuckDBSettingsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<DuckDBSettingsData>();
	if (data.offset >= data.settings.size()) {
		// finished returning values
		return;
	}
	// start returning values
	// either fill up the chunk or return all the remaining columns
	idx_t count = 0;

	// name, LogicalType::VARCHAR
	auto &name = output.data[0];
	// value, LogicalType::VARCHAR
	auto &value = output.data[1];
	// description, LogicalType::VARCHAR
	auto &description = output.data[2];
	// input_type, LogicalType::VARCHAR
	auto &input_type = output.data[3];
	// scope, LogicalType::VARCHAR
	auto &scope = output.data[4];
	// aliases, LogicalType::VARCHAR[]
	auto &aliases = output.data[5];
	// value, LogicalType::VARIANT
	auto &typed_value = output.data[6];

	while (data.offset < data.settings.size() && count < STANDARD_VECTOR_SIZE) {
		auto &entry = data.settings[data.offset++];

		name.Append(Value(entry.name));
		value.Append(entry.value.CastAs(context, LogicalType::VARCHAR));
		description.Append(Value(entry.description));
		input_type.Append(Value(entry.input_type));
		scope.Append(Value(entry.scope));
		aliases.Append(Value::LIST(LogicalType::VARCHAR, std::move(entry.aliases)));
		typed_value.Append(entry.value.CastAs(context, LogicalType::VARIANT()));
		count++;
	}
}

void DuckDBSettingsFun::RegisterFunction(BuiltinFunctions &set) {
	TableFunction settings_fun("duckdb_settings", {}, DuckDBSettingsFunction, DuckDBSettingsBind, DuckDBSettingsInit);
	settings_fun.GetSignature().WithTypedKwargs("options", [](TypedKwargs &options) {
		options.Add("debug", LogicalType::BOOLEAN)
		    .Add("deprecated", LogicalType::BOOLEAN)
		    .Add("include_all", LogicalType::BOOLEAN);
	});
	set.AddFunction(settings_fun);
}

} // namespace duckdb
