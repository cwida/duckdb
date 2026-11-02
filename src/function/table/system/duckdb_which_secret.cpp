#include "duckdb/function/table/system_functions.hpp"

#include "duckdb/common/file_system.hpp"
#include "duckdb/common/map.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/multi_file/multi_file_reader.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

namespace duckdb {

struct DuckDBWhichSecretData : public GlobalTableFunctionState {
	DuckDBWhichSecretData() : finished(false) {
	}
	bool finished;
};

struct DuckDBWhichSecretBindData : public TableFunctionData {
	explicit DuckDBWhichSecretBindData(TableFunctionBindInput &tf_input) : inputs(tf_input.inputs) {};

	duckdb::vector<duckdb::Value> inputs;
};

static unique_ptr<FunctionData> DuckDBWhichSecretBind(ClientContext &context, TableFunctionBindInput &input,
                                                      vector<LogicalType> &return_types, vector<Identifier> &names) {
	names.emplace_back("name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("persistent");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("storage");
	return_types.emplace_back(LogicalType::VARCHAR);

	return make_uniq<DuckDBWhichSecretBindData>(input);
}

unique_ptr<GlobalTableFunctionState> DuckDBWhichSecretInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<DuckDBWhichSecretData>();
}

void DuckDBWhichSecretFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<DuckDBWhichSecretData>();
	if (data.finished) {
		// finished returning values
		return;
	}
	auto &bind_data = data_p.bind_data->Cast<DuckDBWhichSecretBindData>();

	auto &secret_manager = SecretManager::Get(context);
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);

	auto &inputs = bind_data.inputs;
	auto path = inputs[0].ToString();
	auto type = inputs[1].ToString();
	auto secret_match = secret_manager.LookupSecret(transaction, path, type);
	if (secret_match.HasMatch()) {
		auto &secret_entry = *secret_match.secret_entry;
		output.data[0].Append(Value(secret_entry.secret->GetName()));
		output.data[1].Append(Value(EnumUtil::ToString(secret_entry.persist_type)));
		output.data[2].Append(Value(secret_entry.storage_mode));
	}
	data.finished = true;
}

void DuckDBWhichSecretFun::RegisterFunction(BuiltinFunctions &set) {
	set.AddFunction(TableFunction("which_secret",
	                              FunctionSignature()
	                                  .AddPositionalOnly("path", LogicalType::VARCHAR)
	                                  .AddPositionalOnly("type", LogicalType::VARCHAR),
	                              DuckDBWhichSecretFunction, DuckDBWhichSecretBind, DuckDBWhichSecretInit));
}

} // namespace duckdb
