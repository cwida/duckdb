#include "duckdb/function/table/system_functions.hpp"

#include "duckdb/function/function_set.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

namespace duckdb {

struct DuckDBSecretsData : public GlobalTableFunctionState {
	DuckDBSecretsData() : offset(0) {
	}
	idx_t offset;
	duckdb::vector<duckdb::SecretEntry> secrets;
};

struct DuckDBSecretsBindData : public FunctionData {
public:
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<DuckDBSecretsBindData>();
	};

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<DuckDBSecretsBindData>();
		return redact == other.redact;
	}
	SecretDisplayType redact = SecretDisplayType::REDACTED;
};

static unique_ptr<FunctionData> DuckDBSecretsBind(ClientContext &context, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<DuckDBSecretsBindData>();

	auto entry = input.named_parameters.find("redact");
	if (entry != input.named_parameters.end()) {
		if (entry->second.IsNull()) {
			throw InvalidInputException("Cannot use NULL as argument for redact");
		}
		if (BooleanValue::Get(entry->second)) {
			result->redact = SecretDisplayType::REDACTED;
		} else {
			result->redact = SecretDisplayType::UNREDACTED;
		}
	}

	if (!Settings::Get<AllowUnredactedSecretsSetting>(context) && result->redact == SecretDisplayType::UNREDACTED) {
		throw InvalidInputException("Displaying unredacted secrets is disabled");
	}

	names.emplace_back("name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("type");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("provider");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("persistent");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("storage");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("scope");
	return_types.emplace_back(LogicalType::LIST(LogicalType::VARCHAR));

	names.emplace_back("secret_string");
	return_types.emplace_back(LogicalType::VARCHAR);

	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> DuckDBSecretsInit(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<DuckDBSecretsData>();
	return std::move(result);
}

void DuckDBSecretsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<DuckDBSecretsData>();
	auto &bind_data = data_p.bind_data->Cast<DuckDBSecretsBindData>();

	auto &secret_manager = SecretManager::Get(context);

	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);

	if (data.secrets.empty()) {
		data.secrets = secret_manager.AllSecrets(transaction);
	}
	auto &secrets = data.secrets;
	if (data.offset >= secrets.size()) {
		// finished returning values
		return;
	}
	// start returning values
	// either fill up the chunk or return all the remaining columns
	idx_t count = 0;

	// name
	auto &name = output.data[0];
	// type
	auto &type = output.data[1];
	// provider
	auto &provider = output.data[2];
	// persistent
	auto &persistent = output.data[3];
	// storage
	auto &storage = output.data[4];
	// scope
	auto &scope = output.data[5];
	// secret_string
	auto &secret_string = output.data[6];

	while (data.offset < secrets.size() && count < STANDARD_VECTOR_SIZE) {
		auto &secret_entry = secrets[data.offset];

		vector<Value> scope_value;
		for (const auto &scope_entry : secret_entry.secret->GetScope()) {
			scope_value.push_back(scope_entry);
		}

		const auto &secret = *secret_entry.secret;

		name.Append(Value(secret.GetName()));
		type.Append(Value(secret.GetType()));
		provider.Append(Value(secret.GetProvider()));
		persistent.Append(Value::BOOLEAN(secret_entry.persist_type == SecretPersistType::PERSISTENT));
		storage.Append(Value(secret_entry.storage_mode));
		scope.Append(Value::LIST(LogicalType::VARCHAR, scope_value));
		secret_string.Append(Value(secret.ToString(bind_data.redact)));

		data.offset++;
		count++;
	}
}

void DuckDBSecretsFun::RegisterFunction(BuiltinFunctions &set) {
	TableFunctionSet functions("duckdb_secrets");
	auto fun = TableFunction({}, DuckDBSecretsFunction, DuckDBSecretsBind, DuckDBSecretsInit);
	fun.GetSignature().WithTypedKwargs("options",
	                                   [](TypedKwargs &options) { options.Add("redact", LogicalType::BOOLEAN); });
	functions.AddFunction(fun);
	set.AddFunction(functions);
}

} // namespace duckdb
