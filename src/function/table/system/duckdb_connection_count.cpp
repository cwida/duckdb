#include "duckdb/function/table/system_functions.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/connection_manager.hpp"

namespace duckdb {

struct DuckDBConnectionCountData : public GlobalTableFunctionState {
	DuckDBConnectionCountData() : count(0), finished(false) {
	}
	idx_t count;
	bool finished;
};

static unique_ptr<FunctionData> DuckDBConnectionCountBind(ClientContext &context, TableFunctionBindInput &input,
                                                          vector<LogicalType> &return_types,
                                                          vector<Identifier> &names) {
	names.emplace_back("count");
	return_types.emplace_back(LogicalType::UBIGINT);
	return nullptr;
}

unique_ptr<GlobalTableFunctionState> DuckDBConnectionCountInit(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<DuckDBConnectionCountData>();
	auto &conn_manager = context.db->GetConnectionManager();
	result->count = conn_manager.GetConnectionCount();
	return std::move(result);
}

void DuckDBConnectionCountFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<DuckDBConnectionCountData>();
	if (data.finished) {
		return;
	}
	output.data[0].Append(Value::UBIGINT(data.count));
	data.finished = true;
}

void DuckDBConnectionCountFun::RegisterFunction(BuiltinFunctions &set) {
	TableFunction connection_count_fun("duckdb_connection_count", {}, DuckDBConnectionCountFunction,
	                                   DuckDBConnectionCountBind, DuckDBConnectionCountInit);
	// reads the live connection registry
	connection_count_fun.stability = FunctionStability::VOLATILE;
	set.AddFunction(std::move(connection_count_fun));
}

} // namespace duckdb
