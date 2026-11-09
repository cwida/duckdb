#include "duckdb/function/table/system_functions.hpp"
#include "duckdb/main/database_manager.hpp"

namespace duckdb {

struct DuckDBApproxDatabaseCountData : public GlobalTableFunctionState {
	DuckDBApproxDatabaseCountData() : count(0), finished(false) {
	}
	idx_t count;
	bool finished;
};

static unique_ptr<FunctionData> DuckDBApproxDatabaseCountBind(ClientContext &context, TableFunctionBindInput &input,
                                                              vector<LogicalType> &return_types,
                                                              vector<Identifier> &names) {
	names.emplace_back("approx_count");
	return_types.emplace_back(LogicalType::UBIGINT);
	return nullptr;
}

unique_ptr<GlobalTableFunctionState> DuckDBApproxDatabaseCountInit(ClientContext &context,
                                                                   TableFunctionInitInput &input) {
	auto result = make_uniq<DuckDBApproxDatabaseCountData>();
	result->count = DatabaseManager::Get(context).ApproxDatabaseCount();
	return std::move(result);
}

void DuckDBApproxDatabaseCountFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<DuckDBApproxDatabaseCountData>();
	if (data.finished) {
		return;
	}
	output.data[0].Append(Value::UBIGINT(data.count));
	data.finished = true;
}

void DuckDBApproxDatabaseCountFun::RegisterFunction(BuiltinFunctions &set) {
	TableFunction approx_count_fun("duckdb_approx_database_count", {}, DuckDBApproxDatabaseCountFunction,
	                               DuckDBApproxDatabaseCountBind, DuckDBApproxDatabaseCountInit);
	// reads an approximate live count of the attached databases
	approx_count_fun.stability = FunctionStability::VOLATILE;
	set.AddFunction(std::move(approx_count_fun));
}

} // namespace duckdb
