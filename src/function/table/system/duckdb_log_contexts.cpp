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

struct DuckDBLogContextBindData : public TableFunctionData {
	explicit DuckDBLogContextBindData(string sink_name_p) : sink_name(std::move(sink_name_p)) {
	}

	//! The sink to scan, or empty for the log sink
	string sink_name;
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

	return make_uniq<DuckDBLogContextBindData>(DuckDBLogFun::GetSinkName(input));
}

unique_ptr<GlobalTableFunctionState> DuckDBLogContextInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<DuckDBLogContextBindData>();
	auto log_sink = LogManager::Get(context).GetScannableLogSink(bind_data.sink_name, LoggingTargetTable::LOG_CONTEXTS);
	return make_uniq<DuckDBLogContextData>(std::move(log_sink));
}

void DuckDBLogContextFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<DuckDBLogContextData>();
	data.log_sink->Scan(*data.scan_state, output);
}

static unique_ptr<TableRef> DuckDBLogContextsBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	auto log_sink = LogManager::Get(context).GetLogSink(DuckDBLogFun::GetSinkName(input));

	// Attempt to let the sink BindReplace the scan function
	return log_sink->BindReplace(context, input, LoggingTargetTable::LOG_CONTEXTS);
}

void DuckDBLogContextFun::RegisterFunction(BuiltinFunctions &set) {
	auto fun =
	    TableFunction("duckdb_log_contexts", {}, DuckDBLogContextFunction, DuckDBLogContextBind, DuckDBLogContextInit);
	fun.bind_replace = DuckDBLogContextsBindReplace;
	// the log sink grows while a query executes
	fun.stability = FunctionStability::VOLATILE;
	fun.GetSignature().WithTypedKwargs("options",
	                                   [](TypedKwargs &options) { options.Add("sink", LogicalType::VARCHAR); });
	set.AddFunction(fun);
}

} // namespace duckdb
