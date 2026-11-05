#include "duckdb/function/table/system_functions.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/main/extension_callback_manager.hpp"
#include "duckdb/parser/peg/dialect_extension.hpp"

namespace duckdb {

struct DuckDBDialectsData : public GlobalTableFunctionState {
	vector<string> dialects;
	idx_t offset = 0;
};

static unique_ptr<FunctionData> DuckDBDialectsBind(ClientContext &context, TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types, vector<Identifier> &names) {
	names.emplace_back("dialect_name");
	return_types.emplace_back(LogicalType::VARCHAR);
	return nullptr;
}

static unique_ptr<GlobalTableFunctionState> DuckDBDialectsInit(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<DuckDBDialectsData>();
	for (auto &dialect : ExtensionCallbackManager::Get(context).DialectExtensions()) {
		result->dialects.push_back(dialect->Name());
	}
	return std::move(result);
}

static void DuckDBDialectsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<DuckDBDialectsData>();
	auto &dialect_name = output.data[0];
	idx_t count = 0;
	while (data.offset < data.dialects.size() && count < STANDARD_VECTOR_SIZE) {
		dialect_name.Append(Value(data.dialects[data.offset++]));
		count++;
	}
}

void DuckDBDialectsFun::RegisterFunction(BuiltinFunctions &set) {
	set.AddFunction(
	    TableFunction("duckdb_dialects", {}, DuckDBDialectsFunction, DuckDBDialectsBind, DuckDBDialectsInit));
}

} // namespace duckdb
