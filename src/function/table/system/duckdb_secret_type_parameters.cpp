
#include "duckdb/function/table/system_functions.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/main/secret/secret.hpp"

namespace duckdb {

struct SecretTypeParameterRow {
	string secret_type;
	string provider;
	string parameter_name;
	string parameter_type;
};

struct DuckDBSecretTypeParametersData : public GlobalTableFunctionState {
	DuckDBSecretTypeParametersData() : offset(0) {
	}

	vector<SecretTypeParameterRow> rows;
	idx_t offset;
};

static unique_ptr<FunctionData> DuckDBSecretTypeParametersBind(ClientContext &context, TableFunctionBindInput &input,
                                                               vector<LogicalType> &return_types,
                                                               vector<Identifier> &names) {
	names.emplace_back("type");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("provider");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("parameter_type");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("description");
	return_types.emplace_back(LogicalType::VARCHAR);

	return nullptr;
}

unique_ptr<GlobalTableFunctionState> DuckDBSecretTypeParametersInit(ClientContext &context,
                                                                    TableFunctionInitInput &input) {
	auto result = make_uniq<DuckDBSecretTypeParametersData>();

	auto &secret_manager = SecretManager::Get(context);
	auto all_functions = secret_manager.AllSecretFunctions();
	for (const auto &func : all_functions) {
		for (const auto &param_pairs : func.named_parameters) {
			SecretTypeParameterRow row;
			row.secret_type = func.secret_type;
			row.provider = func.provider.GetIdentifierName();
			row.parameter_name = param_pairs.first.GetIdentifierName();
			row.parameter_type = param_pairs.second.ToString();
			result->rows.push_back(row);
		}
	}

	return std::move(result);
}

void DuckDBSecretTypeParametersFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<DuckDBSecretTypeParametersData>();
	if (data.offset >= data.rows.size()) {
		// finished returning values
		return;
	}
	// start returning values
	// either fill up the chunk or return all the remaining columns
	idx_t count = 0;

	// type, VARCHAR
	auto &type = output.data[0];
	// provider, VARCHAR
	auto &provider = output.data[1];
	// name, VARCHAR
	auto &name = output.data[2];
	// parameter_type, VARCHAR
	auto &parameter_type = output.data[3];
	// description, VARCHAR
	auto &description = output.data[4];

	while (data.offset < data.rows.size() && count < STANDARD_VECTOR_SIZE) {
		auto &entry = data.rows[data.offset++];

		type.Append(Value(entry.secret_type));
		provider.Append(Value(entry.provider));
		name.Append(Value(entry.parameter_name));
		parameter_type.Append(Value(entry.parameter_type));
		description.Append(Value());

		count++;
	}
}

void DuckDBSecretTypeParametersFun::RegisterFunction(BuiltinFunctions &set) {
	set.AddFunction(TableFunction("duckdb_secret_type_parameters", {}, DuckDBSecretTypeParametersFunction,
	                              DuckDBSecretTypeParametersBind, DuckDBSecretTypeParametersInit));
}

} // namespace duckdb
