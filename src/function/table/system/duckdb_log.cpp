#include "duckdb/function/table/system_functions.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/parser_options.hpp"
#include "duckdb/logging/log_manager.hpp"
#include "duckdb/logging/log_sink.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"

namespace duckdb {

struct DuckDBLogData : public GlobalTableFunctionState {
	explicit DuckDBLogData(shared_ptr<LogSink> log_sink_p) : log_sink(std::move(log_sink_p)) {
		scan_state = log_sink->CreateScanState(LoggingTargetTable::LOG_ENTRIES);
		log_sink->InitializeScan(*scan_state);
		total_rows = log_sink->GetScanRowCount(LoggingTargetTable::LOG_ENTRIES);
	}

	//! The log sink we are scanning
	shared_ptr<LogSink> log_sink;
	unique_ptr<LogSinkScanState> scan_state;
	//! The number of log entries when the scan started, if known (for progress)
	optional_idx total_rows;
	atomic<idx_t> scanned_rows {0};
};

struct DuckDBLogBindData : public TableFunctionData {
	explicit DuckDBLogBindData(string sink_name_p) : sink_name(std::move(sink_name_p)) {
	}

	//! The sink to scan, or empty for the log sink
	string sink_name;
};

string DuckDBLogFun::GetSinkName(TableFunctionBindInput &input) {
	auto sink_setting = input.named_parameters.find("sink");
	if (sink_setting == input.named_parameters.end()) {
		return string();
	}
	if (sink_setting->second.IsNull()) {
		throw InvalidInputException("sink cannot be NULL");
	}
	return sink_setting->second.GetValue<string>();
}

static unique_ptr<FunctionData> DuckDBLogBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<Identifier> &names) {
	names.emplace_back("context_id");
	return_types.emplace_back(LogicalType::UBIGINT);

	names.emplace_back("timestamp");
	return_types.emplace_back(LogicalType::TIMESTAMP_TZ);

	names.emplace_back("type");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("log_level");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("message");
	return_types.emplace_back(LogicalType::VARCHAR);

	return make_uniq<DuckDBLogBindData>(DuckDBLogFun::GetSinkName(input));
}

unique_ptr<GlobalTableFunctionState> DuckDBLogInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<DuckDBLogBindData>();
	auto log_sink = LogManager::Get(context).GetScannableLogSink(bind_data.sink_name, LoggingTargetTable::LOG_ENTRIES);
	return make_uniq<DuckDBLogData>(std::move(log_sink));
}

void DuckDBLogFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<DuckDBLogData>();
	data.log_sink->Scan(*data.scan_state, output);
	data.scanned_rows.fetch_add(output.size(), std::memory_order_relaxed);
}

static double DuckDBLogProgress(ClientContext &context, const FunctionData *bind_data,
                                const GlobalTableFunctionState *global_state) {
	auto &data = global_state->Cast<DuckDBLogData>();
	if (!data.log_sink) {
		return 100.0;
	}
	if (!data.total_rows.IsValid()) {
		return -1;
	}
	auto total_rows = data.total_rows.GetIndex();
	if (total_rows == 0) {
		return 100.0;
	}
	// log entries that are added while scanning are not part of the total
	return MinValue<double>(100.0 * static_cast<double>(data.scanned_rows) / static_cast<double>(total_rows), 100.0);
}

unique_ptr<TableRef> DuckDBLogBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	auto sink_name = DuckDBLogFun::GetSinkName(input);
	auto log_sink = LogManager::Get(context).GetLogSink(sink_name);

	bool denormalized_table = false;
	auto denormalized_table_setting = input.named_parameters.find("denormalized_table");
	if (denormalized_table_setting != input.named_parameters.end()) {
		if (denormalized_table_setting->second.IsNull()) {
			throw InvalidInputException("denormalized_table cannot be NULL");
		}
		denormalized_table = denormalized_table_setting->second.GetValue<bool>();
	}

	// Without join contexts we simply scan the LOG_ENTRIES tables
	if (!denormalized_table) {
		auto res = log_sink->BindReplace(context, input, LoggingTargetTable::LOG_ENTRIES);
		return res;
	}

	// If the sink can bind replace for LoggingTargetTable::ALL_LOGS, we use that since that will be most efficient
	auto all_log_scan = log_sink->BindReplace(context, input, LoggingTargetTable::ALL_LOGS);
	if (all_log_scan) {
		return all_log_scan;
	}

	// We cannot scan ALL_LOGS but denormalized_table was requested: we need to inject the join between LOG_ENTRIES and
	// LOG_CONTEXTS
	string sink_parameter = sink_name.empty() ? string() : StringUtil::Format("sink=%s", SQLString(sink_name));
	string sub_query_string = "SELECT l.context_id, scope, connection_id, transaction_id, query_id, thread_id, "
	                          "timestamp, type, log_level, message"
	                          " FROM (SELECT row_number() OVER () AS rowid, * FROM duckdb_logs(" +
	                          sink_parameter + ")) as l JOIN duckdb_log_contexts(" + sink_parameter +
	                          ") as c ON l.context_id=c.context_id order by timestamp, l.rowid;";
	Parser parser(context);
	parser.ParseQuery(sub_query_string);
	auto select_stmt = unique_ptr_cast<SQLStatement, SelectStatement>(std::move(parser.statements[0]));

	return duckdb::make_uniq<SubqueryRef>(std::move(select_stmt));
}

void DuckDBLogFun::RegisterFunction(BuiltinFunctions &set) {
	TableFunction logs_fun("duckdb_logs", {}, DuckDBLogFunction, DuckDBLogBind, DuckDBLogInit);
	logs_fun.bind_replace = DuckDBLogBindReplace;
	// the log storage grows while a query executes
	logs_fun.stability = FunctionStability::VOLATILE;
	logs_fun.table_scan_progress = DuckDBLogProgress;
	logs_fun.GetSignature().WithTypedKwargs("options", [](TypedKwargs &options) {
		options.Add("denormalized_table", LogicalType::BOOLEAN).Add("sink", LogicalType::VARCHAR);
	});
	set.AddFunction(logs_fun);
}

} // namespace duckdb
