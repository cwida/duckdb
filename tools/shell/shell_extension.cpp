#include "shell_extension.hpp"
#include "shell_grammar_extension.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/os_util.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/planner/planner_extension.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "shell_state.hpp"
#include "duckdb/parser/tableref/column_data_ref.hpp"
#include "shell_renderer.hpp"
#ifdef HAVE_LINENOISE
#include "history.hpp"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <algorithm>

#ifndef DUCKDB_API_CLI
#define DUCKDB_API_CLI "cli"
#endif

namespace duckdb {

static void GetEnvFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &heap = StringVector::GetStringHeap(result);
	auto &os_util = OSUtil::Get(*state.GetContext().db);
	UnaryExecutor::Execute<string_t, string_t>(
	    args.data[0], result, [&](string_t input) { return heap.AddString(os_util.GetEnv(input.GetString())); });
}

static unique_ptr<FunctionData> GetEnvBind(BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	if (!Settings::Get<EnableExternalAccessSetting>(context)) {
		throw PermissionException("getenv is disabled through configuration");
	}
	return nullptr;
}

unique_ptr<TableRef> ShellScanLastResult(ClientContext &context, ReplacementScanInput &input,
                                         optional_ptr<ReplacementScanData> data) {
	auto &table_name = input.table_name;
	if (table_name != "_") {
		return nullptr;
	}
	auto &state = duckdb_shell::ShellState::Get();
	state.last_result_referenced = true;
	if (!state.last_result) {
		throw BinderException("Failed to query last result \"_\": no result available");
	}
	return make_uniq<ColumnDataRef>(state.last_result->Collection(), state.last_result->GetNames());
}

// Runs after the binder has finished. Releases the previous last_result early if it's
// safe — i.e. the new query doesn't statically reference `_` (the replacement scan
// would have fired)
void ShellPostBind(PlannerExtensionInput &input, BoundStatement &statement) {
	auto &state = duckdb_shell::ShellState::Get();
	if (!state.conn || !RefersToSameObject(*state.conn->context, input.context)) {
		return;
	}
	if (state.last_result_referenced) {
		return;
	}
	state.last_result.reset();
}

//===--------------------------------------------------------------------===//
// shell_history table function
//===--------------------------------------------------------------------===//
struct ShellHistoryData : public GlobalTableFunctionState {
	ShellHistoryData() : offset(0) {
#ifdef HAVE_LINENOISE
		count = History::GetLength();
#else
		count = 0;
#endif
	}

	idx_t count;
	idx_t offset;
};

static unique_ptr<FunctionData> ShellHistoryBind(ClientContext &context, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<Identifier> &names) {
	names.emplace_back("id");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("sql");
	return_types.emplace_back(LogicalType::VARCHAR);
	return nullptr;
}

static unique_ptr<GlobalTableFunctionState> ShellHistoryInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<ShellHistoryData>();
}

static void ShellHistoryFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<ShellHistoryData>();
	idx_t chunk_count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, data.count - data.offset);
	auto &id_vec = output.data[0];
	auto &sql_vec = output.data[1];
	for (idx_t i = 0; i < chunk_count; i++) {
		idx_t history_index = data.offset + i;
#ifdef HAVE_LINENOISE
		string entry = History::GetEntry(history_index);
		// multi-line history entries are stored with \r\n separators - normalize to \n
		entry = StringUtil::Replace(entry, "\r\n", "\n");
#else
		string entry;
#endif
		id_vec.Append(Value::BIGINT(NumericCast<int64_t>(history_index + 1)));
		sql_vec.Append(Value(std::move(entry)));
	}
	data.offset += chunk_count;
}

void ShellExtension::Load(ExtensionLoader &loader) {
	loader.SetDescription("Adds CLI-specific support and functionalities");
	ScalarFunction getenv_fun("getenv", {}, LogicalType::VARCHAR, GetEnvFunction, GetEnvBind);
	getenv_fun.GetSignature().AddParameter("name", LogicalType::VARCHAR);
	loader.RegisterFunction(getenv_fun);

	TableFunction shell_history("shell_history", {}, ShellHistoryFunction, ShellHistoryBind, ShellHistoryInit);
	loader.RegisterFunction(shell_history);

	auto &config = duckdb::DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.SetOptionByName("duckdb_api", DUCKDB_API_CLI);
	config.replacement_scans.push_back(duckdb::ReplacementScan(duckdb::ShellScanLastResult));

	PlannerExtension planner_ext;
	planner_ext.post_bind_function = duckdb::ShellPostBind;
	PlannerExtension::Register(config, planner_ext);

	// opt-in grammar: the CLI enables it via CALL add_grammar_extension('shell') when it opens a database
	GrammarExtension::Register(loader.GetDatabaseInstance(), make_shared_ptr<ShellGrammarExtension>());

	// highlight the CLI profiler "query_tree" output
	duckdb_shell::RegisterProfilerHighlighting(config);
}

std::string ShellExtension::Name() {
	return "shell";
}

std::string ShellExtension::Version() const {
	return DefaultVersion();
}

} // namespace duckdb
