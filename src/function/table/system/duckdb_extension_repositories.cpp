#include "duckdb/function/table/system_functions.hpp"

#include "duckdb/common/enum_util.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension_repository_manager.hpp"

namespace duckdb {

struct DuckDBExtensionRepositoriesData : public GlobalTableFunctionState {
	DuckDBExtensionRepositoriesData() : offset(0) {
	}

	vector<ExtensionRepository> entries;
	idx_t offset;
};

static unique_ptr<FunctionData> DuckDBExtensionRepositoriesBind(ClientContext &context, TableFunctionBindInput &input,
                                                                vector<LogicalType> &return_types,
                                                                vector<Identifier> &names) {
	names.emplace_back("repository_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("prefix");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("type");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("key_fingerprints");
	return_types.emplace_back(LogicalType::LIST(LogicalType::VARCHAR));

	names.emplace_back("public_keys");
	return_types.emplace_back(LogicalType::LIST(LogicalType::VARCHAR));

	names.emplace_back("host");
	return_types.emplace_back(LogicalType::VARCHAR);

	return nullptr;
}

unique_ptr<GlobalTableFunctionState> DuckDBExtensionRepositoriesInit(ClientContext &context,
                                                                     TableFunctionInitInput &input) {
	auto result = make_uniq<DuckDBExtensionRepositoriesData>();

	auto &db = DatabaseInstance::GetDatabase(context);
	// the built-in repositories, followed by the trusted repositories that were added by the user
	for (auto &name : ExtensionRepository::GetKnownRepositoryNames()) {
		ExtensionRepository repository;
		ExtensionRepository::TryGetKnownRepository(name, repository);
		result->entries.push_back(std::move(repository));
	}
	for (auto &repository : ExtensionRepositoryManager::GetRepositories(db, FileSystem::GetLocal(db))) {
		result->entries.push_back(repository);
	}
	return std::move(result);
}

void DuckDBExtensionRepositoriesFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<DuckDBExtensionRepositoriesData>();
	if (data.offset >= data.entries.size()) {
		// finished returning values
		return;
	}
	idx_t count = 0;

	// repository_name, VARCHAR
	auto &name = output.data[0];
	// prefix, VARCHAR
	auto &prefix = output.data[1];
	// type, VARCHAR
	auto &type = output.data[2];
	// key_fingerprints, LIST(VARCHAR)
	auto &key_fingerprints = output.data[3];
	// public_keys, LIST(VARCHAR)
	auto &public_keys = output.data[4];
	// host, VARCHAR: the host a repository was resolved from, NULL for one added by prefix or built in
	auto &host = output.data[5];

	while (data.offset < data.entries.size() && count < STANDARD_VECTOR_SIZE) {
		auto &entry = data.entries[data.offset++];
		name.Append(Value(entry.name));
		prefix.Append(Value(entry.path));
		type.Append(Value(EnumUtil::ToString(entry.type)));
		host.Append(entry.host.empty() ? Value(LogicalType::VARCHAR) : Value(entry.host));
		// only user provided repositories have keys of their own - the core and community keys are built in
		if (entry.public_keys.empty()) {
			key_fingerprints.Append(Value(LogicalType::LIST(LogicalType::VARCHAR)));
			public_keys.Append(Value(LogicalType::LIST(LogicalType::VARCHAR)));
		} else {
			vector<Value> fingerprint_values;
			vector<Value> key_values;
			for (auto &public_key : entry.public_keys) {
				fingerprint_values.push_back(Value(ExtensionRepositoryManager::GetPublicKeyFingerprint(public_key)));
				key_values.push_back(Value(public_key));
			}
			key_fingerprints.Append(Value::LIST(LogicalType::VARCHAR, std::move(fingerprint_values)));
			public_keys.Append(Value::LIST(LogicalType::VARCHAR, std::move(key_values)));
		}
		count++;
	}
}

void DuckDBExtensionRepositoriesFun::RegisterFunction(BuiltinFunctions &set) {
	set.AddFunction(TableFunction("duckdb_extension_repositories", {}, DuckDBExtensionRepositoriesFunction,
	                              DuckDBExtensionRepositoriesBind, DuckDBExtensionRepositoriesInit));
}

} // namespace duckdb
