#include "duckdb/function/table/system_functions.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension_callback_manager.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parser/grammar_extension.hpp"

namespace duckdb {

static bool IsActiveGrammarExtension(const ClientContext &context, const string &name) {
	for (auto &active : ClientConfig::GetConfig(context).active_grammar_extensions) {
		if (StringUtil::CIEquals(active, name)) {
			return true;
		}
	}
	return false;
}

struct GrammarExtensionData {
	string name;
	string description;
	bool active;
};

struct DuckDBGrammarExtensionsData : public GlobalTableFunctionState {
	vector<GrammarExtensionData> extensions;
	idx_t offset = 0;
};

static unique_ptr<FunctionData> DuckDBGrammarExtensionsBind(ClientContext &context, TableFunctionBindInput &input,
                                                            vector<LogicalType> &return_types,
                                                            vector<Identifier> &names) {
	names.emplace_back("name");
	names.emplace_back("description");
	names.emplace_back("active");
	return_types.emplace_back(LogicalType::VARCHAR);
	return_types.emplace_back(LogicalType::VARCHAR);
	return_types.emplace_back(LogicalType::BOOLEAN);
	return nullptr;
}

static unique_ptr<GlobalTableFunctionState> DuckDBGrammarExtensionsInit(ClientContext &context,
                                                                        TableFunctionInitInput &input) {
	auto result = make_uniq<DuckDBGrammarExtensionsData>();
	auto &callback_manager = ExtensionCallbackManager::Get(context);
	for (auto &[name, extension] : callback_manager.GrammarExtensions()) {
		GrammarExtensionData data;
		data.name = name;
		data.description = extension->Description();
		data.active = IsActiveGrammarExtension(context, name);
		result->extensions.push_back(data);
	}
	return std::move(result);
}

static void DuckDBGrammarExtensionsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<DuckDBGrammarExtensionsData>();
	auto &name = output.data[0];
	auto &description = output.data[1];
	auto &active = output.data[2];
	idx_t count = 0;
	while (data.offset < data.extensions.size() && count < STANDARD_VECTOR_SIZE) {
		auto &extension_data = data.extensions[data.offset++];
		name.Append(Value(extension_data.name));
		description.Append(Value(extension_data.description));
		active.Append(Value::BOOLEAN(extension_data.active));
		count++;
	}
}

void DuckDBGrammarExtensionsFun::RegisterFunction(BuiltinFunctions &set) {
	set.AddFunction(TableFunction("duckdb_grammar_extensions", {}, DuckDBGrammarExtensionsFunction,
	                              DuckDBGrammarExtensionsBind, DuckDBGrammarExtensionsInit));
}

//===--------------------------------------------------------------------===//
// add_grammar_extension / remove_grammar_extension
//===--------------------------------------------------------------------===//
struct GrammarExtensionNameData : public TableFunctionData {
	explicit GrammarExtensionNameData(string name_p) : name(std::move(name_p)) {
	}

	//! The registered name of the grammar extension
	string name;
};

static unique_ptr<FunctionData> BindGrammarExtensionName(ClientContext &context, TableFunctionBindInput &input,
                                                         vector<LogicalType> &return_types, vector<Identifier> &names) {
	DBConfig::GetConfig(context).CheckLock(ActiveGrammarExtensionsSetting::Name);
	if (input.inputs[0].IsNull()) {
		throw InvalidInputException("The name of the grammar extension cannot be NULL");
	}
	auto name = input.inputs[0].GetValue<string>();
	auto &callback_manager = ExtensionCallbackManager::Get(context);
	auto extension = callback_manager.FindGrammarExtension(name);
	if (!extension) {
		vector<string> registered_names;
		for (auto &entry : callback_manager.GrammarExtensions()) {
			registered_names.push_back(entry.first);
		}
		auto candidates =
		    StringUtil::CandidatesMessage(StringUtil::TopNJaroWinkler(registered_names, name), "Did you mean");
		throw InvalidInputException("Grammar extension \"%s\" does not exist%s", name, candidates);
	}
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("Success");
	return make_uniq<GrammarExtensionNameData>(extension->Name());
}

static void SetActiveGrammarExtensions(ClientContext &context, const vector<string> &extensions) {
	vector<Value> values;
	for (auto &extension : extensions) {
		values.emplace_back(extension);
	}
	ActiveGrammarExtensionsSetting::SetLocal(context, Value::LIST(LogicalType::VARCHAR, std::move(values)));
}

//! Appends the grammar extension to the active grammar extensions, keeping the ones that are already active
static void AddGrammarExtension(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &name = data.bind_data->Cast<GrammarExtensionNameData>().name;
	if (IsActiveGrammarExtension(context, name)) {
		return;
	}
	auto extensions = ClientConfig::GetConfig(context).active_grammar_extensions;
	extensions.push_back(name);
	SetActiveGrammarExtensions(context, extensions);
}

//! Removes the grammar extension from the active grammar extensions, keeping the other ones
static void RemoveGrammarExtension(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &name = data.bind_data->Cast<GrammarExtensionNameData>().name;
	if (!IsActiveGrammarExtension(context, name)) {
		return;
	}
	vector<string> extensions;
	for (auto &active : ClientConfig::GetConfig(context).active_grammar_extensions) {
		if (!StringUtil::CIEquals(active, name)) {
			extensions.push_back(active);
		}
	}
	SetActiveGrammarExtensions(context, extensions);
}

void ActiveGrammarExtensionFun::RegisterFunction(BuiltinFunctions &set) {
	set.AddFunction(
	    TableFunction("add_grammar_extension", {LogicalType::VARCHAR}, AddGrammarExtension, BindGrammarExtensionName));
	set.AddFunction(TableFunction("remove_grammar_extension", {LogicalType::VARCHAR}, RemoveGrammarExtension,
	                              BindGrammarExtensionName));
}

} // namespace duckdb
