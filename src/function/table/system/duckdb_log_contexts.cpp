#include "duckdb/function/table/system_functions.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/logging/log_manager.hpp"
#include "duckdb/logging/log_sink.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/parser/tableref.hpp"

namespace duckdb {

struct DuckDBLogContextData : public GlobalTableFunctionState {
	explicit DuckDBLogContextData(shared_ptr<LogSink> log_sink_p) : log_sink(std::move(log_sink_p)) {
		scan_state = log_sink->CreateScanState(LoggingTargetTable::LOG_CONTEXTS);
		log_sink->InitializeScan(*scan_state);
	}

	//! The log sink we are scanning
	shared_ptr<LogSink> log_sink;
	unique_ptr<LogSinkScanState> scan_state;
};

static unique_ptr<FunctionData> DuckDBLogContextBind(ClientContext &context, TableFunctionBindInput &input,
                                                     vector<LogicalType> &return_types, vector<Identifier> &names) {
	names.emplace_back("context_id");
	return_types.emplace_back(LogicalType::UBIGINT);

	names.emplace_back("scope");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("connection_id");
	return_types.emplace_back(LogicalType::UBIGINT);

	names.emplace_back("transaction_id");
	return_types.emplace_back(LogicalType::UBIGINT);

	names.emplace_back("query_id");
	return_types.emplace_back(LogicalType::UBIGINT);

	names.emplace_back("thread_id");
	return_types.emplace_back(LogicalType::UBIGINT);

	return nullptr;
}

unique_ptr<GlobalTableFunctionState> DuckDBLogContextInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &log_manager = LogManager::Get(context);
	if (!log_manager.CanScan(LoggingTargetTable::LOG_CONTEXTS)) {
		throw InvalidConfigurationException(
		    "Log sink '%s' does not support this query. Select a queryable sink, such as 'memory' or 'file', before "
		    "generating logs.",
		    log_manager.GetConfig().storage);
	}
	return make_uniq<DuckDBLogContextData>(log_manager.GetLogSink());
}

void DuckDBLogContextFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<DuckDBLogContextData>();
	data.log_sink->Scan(*data.scan_state, output);
}

static unique_ptr<TableRef> DuckDBLogContextsBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	auto log_sink = LogManager::Get(context).GetLogSink();

	// Attempt to let the sink BindReplace the scan function
	return log_sink->BindReplace(context, input, LoggingTargetTable::LOG_CONTEXTS);
}

void DuckDBLogContextFun::RegisterFunction(BuiltinFunctions &set) {
	auto fun =
	    TableFunction("duckdb_log_contexts", {}, DuckDBLogContextFunction, DuckDBLogContextBind, DuckDBLogContextInit);
	fun.bind_replace = DuckDBLogContextsBindReplace;
	set.AddFunction(fun);
}

} // namespace duckdb
