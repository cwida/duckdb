#include "duckdb/function/table/system_functions.hpp"

#include "duckdb/common/file_system.hpp"
#include "duckdb/common/map.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension_helper.hpp"

#include "duckdb/common/serializer/buffered_file_reader.hpp"
#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/main/extension_install_info.hpp"

namespace duckdb {

struct ExtensionInformation {
	string name;
	bool loaded = false;
	bool installed = false;
	string file_path;
	ExtensionInstallMode install_mode;
	string installed_from;
	string description;
	vector<Value> aliases;
	string extension_version;
	string signature_key_fingerprint;
};

struct DuckDBExtensionsData : public GlobalTableFunctionState {
	DuckDBExtensionsData() : offset(0) {
	}

	vector<ExtensionInformation> entries;
	idx_t offset;
};

static unique_ptr<FunctionData> DuckDBExtensionsBind(ClientContext &context, TableFunctionBindInput &input,
                                                     vector<LogicalType> &return_types, vector<Identifier> &names) {
	names.emplace_back("extension_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("loaded");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("installed");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("install_path");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("description");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("aliases");
	return_types.emplace_back(LogicalType::LIST(LogicalType::VARCHAR));

	names.emplace_back("extension_version");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("install_mode");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("installed_from");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("signature_key_fingerprint");
	return_types.emplace_back(LogicalType::VARCHAR);

	return nullptr;
}

unique_ptr<GlobalTableFunctionState> DuckDBExtensionsInit(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<DuckDBExtensionsData>();

	auto &fs = FileSystem::GetFileSystem(context);
	auto &db = DatabaseInstance::GetDatabase(context);

	// Firstly, we go over all Default Extensions: duckdb_extensions always prints those, installed/loaded or not
	map<string, ExtensionInformation> installed_extensions;
	auto extension_count = ExtensionHelper::DefaultExtensionCount();
	auto alias_count = ExtensionHelper::ExtensionAliasCount();
	for (idx_t i = 0; i < extension_count; i++) {
		auto extension = ExtensionHelper::GetDefaultExtension(i);
		ExtensionInformation info;
		info.name = extension.name;
		info.installed = false;
		info.loaded = false;
		info.install_mode = ExtensionInstallMode::NOT_INSTALLED;
		info.description = extension.description;
		for (idx_t k = 0; k < alias_count; k++) {
			auto alias = ExtensionHelper::GetInternalExtensionAlias(k);
			if (info.name == alias.extension) {
				info.aliases.emplace_back(alias.alias);
			}
		}
		installed_extensions[info.name] = std::move(info);
	}

	// Extensions linked into this binary are built in, whether or not the default list knows them
	for (auto &linked : DBConfig::GetConfig(db).linked_extensions) {
		if (!linked.load) {
			continue;
		}
		auto &info = installed_extensions[linked.name];
		info.name = linked.name;
		info.installed = true;
		info.file_path = "(BUILT-IN)";
		info.install_mode = ExtensionInstallMode::STATICALLY_LINKED;
	}

	// Secondly we scan all installed extensions and their install info
#ifndef WASM_LOADABLE_EXTENSIONS
	auto ext_directories = ExtensionHelper::GetExtensionDirectoryPath(context);
	for (const auto &ext_directory : ext_directories) {
		fs.ListFiles(ext_directory, [&](const string &path, bool is_directory) {
			if (!ExtensionHelper::IsInstalledExtensionFile(path)) {
				return;
			}
			ExtensionInformation info;
			info.name = fs.ExtractBaseName(path);
			info.installed = true;
			info.loaded = false;
			info.file_path = fs.JoinPath(ext_directory, path);

			// Check the info file for its installation source
			auto info_file_path = fs.JoinPath(ext_directory, path + ".info");

			// Read the info file
			auto extension_install_info = ExtensionInstallInfo::TryReadInfoFile(fs, info_file_path, info.name);
			info.install_mode = extension_install_info->mode;
			info.extension_version = extension_install_info->version;
			info.signature_key_fingerprint = extension_install_info->signature_key_fingerprint;
			if (extension_install_info->mode == ExtensionInstallMode::REPOSITORY) {
				info.installed_from = ExtensionRepository::GetRepository(extension_install_info->repository_url);
			} else {
				info.installed_from = extension_install_info->full_path;
			}

			auto entry = installed_extensions.find(info.name);
			if (entry == installed_extensions.end()) {
				installed_extensions[info.name] = std::move(info);
			} else {
				if (entry->second.install_mode != ExtensionInstallMode::STATICALLY_LINKED) {
					entry->second.file_path = info.file_path;
					entry->second.install_mode = info.install_mode;
					entry->second.installed_from = info.installed_from;
					entry->second.install_mode = info.install_mode;
					entry->second.extension_version = info.extension_version;
					entry->second.signature_key_fingerprint = info.signature_key_fingerprint;
				}
				entry->second.installed = true;
			}
		});
	}
#endif

	// Finally, we check the list of currently loaded extensions
	auto &manager = ExtensionManager::Get(db);
	auto extensions = manager.GetExtensions();
	for (auto &ext_name : extensions) {
		auto ext_info = manager.GetExtensionInfo(ext_name);
		if (!ext_info) {
			continue;
		}
		lock_guard<mutex> guard(ext_info->lock);
		if (!ext_info->is_loaded) {
			continue;
		}
		auto &ext_data = *ext_info;
		if (auto &ext_install_info = ext_data.install_info) {
			auto entry = installed_extensions.find(ext_name);
			if (entry == installed_extensions.end() || !entry->second.installed) {
				ExtensionInformation &info = installed_extensions[ext_name];

				info.name = ext_name;
				info.loaded = true;
				info.extension_version = ext_install_info->version;
				info.installed = ext_install_info->mode == ExtensionInstallMode::STATICALLY_LINKED;
				info.install_mode = ext_install_info->mode;
				if (ext_data.install_info->mode == ExtensionInstallMode::STATICALLY_LINKED && info.file_path.empty()) {
					info.file_path = "(BUILT-IN)";
				}
			} else {
				entry->second.loaded = true;
				entry->second.extension_version = ext_install_info->version;
			}
		}
		if (auto &ext_load_info = ext_data.load_info) {
			auto entry = installed_extensions.find(ext_name);
			if (entry != installed_extensions.end()) {
				entry->second.description = ext_load_info->description;
			}
		}
	}

	result->entries.reserve(installed_extensions.size());
	for (auto &kv : installed_extensions) {
		result->entries.push_back(std::move(kv.second));
	}
	return std::move(result);
}

void DuckDBExtensionsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<DuckDBExtensionsData>();
	if (data.offset >= data.entries.size()) {
		// finished returning values
		return;
	}
	// start returning values
	// either fill up the chunk or return all the remaining columns
	idx_t count = 0;

	// extension_name LogicalType::VARCHAR
	auto &extension_name = output.data[0];
	// loaded LogicalType::BOOLEAN
	auto &loaded = output.data[1];
	// installed LogicalType::BOOLEAN
	auto &installed = output.data[2];
	// install_path LogicalType::VARCHAR
	auto &install_path = output.data[3];
	// description LogicalType::VARCHAR
	auto &description = output.data[4];
	// aliases     LogicalType::LIST(LogicalType::VARCHAR)
	auto &aliases = output.data[5];
	// extension_version LogicalType::VARCHAR
	auto &extension_version = output.data[6];
	// install_mode LogicalType::VARCHAR
	auto &install_mode = output.data[7];
	// installed_from LogicalType::VARCHAR
	auto &installed_from = output.data[8];
	// signature_key_fingerprint LogicalType::VARCHAR
	auto &signature_key_fingerprint = output.data[9];

	while (data.offset < data.entries.size() && count < STANDARD_VECTOR_SIZE) {
		auto &entry = data.entries[data.offset];

		extension_name.Append(Value(entry.name));
		loaded.Append(Value::BOOLEAN(entry.loaded));
		installed.Append(Value::BOOLEAN(entry.installed));
		install_path.Append(Value(entry.file_path));
		description.Append(Value(entry.description));
		aliases.Append(Value::LIST(LogicalType::VARCHAR, entry.aliases));
		extension_version.Append(Value(entry.extension_version));
		install_mode.Append(EnumUtil::ToString(entry.install_mode));
		installed_from.Append(Value(entry.installed_from));
		// unsigned or built-in extensions have no signing key: report NULL rather than an empty string
		signature_key_fingerprint.Append(entry.signature_key_fingerprint.empty()
		                                     ? Value(LogicalType::VARCHAR)
		                                     : Value(entry.signature_key_fingerprint));

		data.offset++;
		count++;
	}
}

void DuckDBExtensionsFun::RegisterFunction(BuiltinFunctions &set) {
	TableFunctionSet functions("duckdb_extensions");
	functions.AddFunction(TableFunction({}, DuckDBExtensionsFunction, DuckDBExtensionsBind, DuckDBExtensionsInit));
	set.AddFunction(functions);
}

} // namespace duckdb
