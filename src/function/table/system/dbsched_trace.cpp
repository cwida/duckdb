#include "duckdb/function/table/system_functions.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parallel/pipeline_hook.hpp"
#include "duckdb/parallel/pipeline_tracer.hpp"
#include "duckdb/parallel/task_scheduler.hpp"

namespace duckdb {

//! The dbsched_trace_* table functions read the pipeline tracer's records for the progress simulator
namespace {

enum class TraceTable : uint8_t { INTERVALS, TICKS, PIPELINES, QUERIES, RESET, STATE, SET_POLICY, TASKS, ATTACHMENTS };

struct TraceColumn {
	const char *name;
	LogicalType type;
};

vector<TraceColumn> GetTraceColumns(TraceTable table) {
	auto id_list = LogicalType::LIST(LogicalType::UBIGINT);
	switch (table) {
	case TraceTable::INTERVALS:
		return {{"tick", LogicalType::UBIGINT},
		        {"t_ns", LogicalType::BIGINT},
		        {"query_id", LogicalType::UBIGINT},
		        {"pipeline_id", LogicalType::UBIGINT},
		        {"status", LogicalType::VARCHAR},
		        {"threads", LogicalType::UBIGINT},
		        {"workers", id_list},
		        {"running", LogicalType::UBIGINT},
		        {"assigned", LogicalType::UBIGINT},
		        {"source_chunks", LogicalType::UBIGINT},
		        {"source_rows", LogicalType::UBIGINT},
		        {"sink_rows", LogicalType::UBIGINT},
		        {"thread_ns", LogicalType::UBIGINT},
		        {"finalize_thread_ns", LogicalType::UBIGINT}};
	case TraceTable::TICKS:
		return {{"tick", LogicalType::UBIGINT},           {"t_ns", LogicalType::BIGINT},
		        {"busy_workers", LogicalType::UBIGINT},   {"busy_external", LogicalType::UBIGINT},
		        {"active_queries", LogicalType::UBIGINT}, {"active_pipelines", LogicalType::UBIGINT},
		        {"free_workers", LogicalType::UBIGINT},   {"untraced_tasks", LogicalType::UBIGINT},
		        {"busy_ns", LogicalType::UBIGINT},        {"attached_idle_ns", LogicalType::UBIGINT},
		        {"free_ns", LogicalType::UBIGINT},        {"tick_work_ns", LogicalType::BIGINT}};
	case TraceTable::PIPELINES:
		return {{"query_id", LogicalType::UBIGINT},
		        {"pipeline_id", LogicalType::UBIGINT},
		        {"operator_ids", id_list},
		        {"operator_names", LogicalType::LIST(LogicalType::VARCHAR)},
		        {"dependencies", id_list},
		        {"source_estimated_cardinality", LogicalType::UBIGINT},
		        {"task_count", LogicalType::BIGINT},
		        {"status", LogicalType::VARCHAR},
		        {"scheduled_ns", LogicalType::BIGINT},
		        {"first_task_ns", LogicalType::BIGINT},
		        {"last_source_ns", LogicalType::BIGINT},
		        {"execute_end_ns", LogicalType::BIGINT},
		        {"finalize_start_ns", LogicalType::BIGINT},
		        {"finalize_end_ns", LogicalType::BIGINT},
		        {"source_chunks", LogicalType::UBIGINT},
		        {"source_rows", LogicalType::UBIGINT},
		        {"sink_rows", LogicalType::UBIGINT},
		        {"thread_ns", LogicalType::UBIGINT},
		        {"finalize_thread_ns", LogicalType::UBIGINT},
		        {"finalize_tasks", LogicalType::UBIGINT},
		        {"initialize_thread_ns", LogicalType::UBIGINT},
		        {"eligible_ns", LogicalType::BIGINT},
		        {"task_wait_ns", LogicalType::UBIGINT},
		        {"tasks_started", LogicalType::UBIGINT},
		        {"complete_ns", LogicalType::BIGINT},
		        {"initialize_start_ns", LogicalType::BIGINT},
		        {"initialize_end_ns", LogicalType::BIGINT},
		        {"initialize_on_worker", LogicalType::BOOLEAN}};
	case TraceTable::QUERIES:
		return {{"query_id", LogicalType::UBIGINT},      {"tag", LogicalType::VARCHAR},
		        {"query", LogicalType::VARCHAR},         {"submitted_ns", LogicalType::BIGINT},
		        {"finished_ns", LogicalType::BIGINT},    {"error", LogicalType::BOOLEAN},
		        {"pipeline_count", LogicalType::UBIGINT}};
	case TraceTable::RESET:
	case TraceTable::SET_POLICY:
		return {{"success", LogicalType::BOOLEAN}};
	case TraceTable::TASKS:
		return {{"query_id", LogicalType::UBIGINT},   {"pipeline_id", LogicalType::UBIGINT},
		        {"task_type", LogicalType::VARCHAR},  {"worker", LogicalType::UBIGINT},
		        {"enqueued_ns", LogicalType::BIGINT}, {"start_ns", LogicalType::BIGINT},
		        {"end_ns", LogicalType::BIGINT}};
	case TraceTable::ATTACHMENTS:
		return {{"worker", LogicalType::UBIGINT},
		        {"query_id", LogicalType::UBIGINT},
		        {"pipeline_id", LogicalType::UBIGINT},
		        {"event", LogicalType::VARCHAR},
		        {"t_ns", LogicalType::BIGINT}};
	case TraceTable::STATE:
		return {{"query_id", LogicalType::UBIGINT},      {"pipeline_id", LogicalType::UBIGINT},
		        {"attached", LogicalType::UBIGINT},      {"busy", LogicalType::UBIGINT},
		        {"pending_tasks", LogicalType::UBIGINT}, {"current_step_tasks", LogicalType::UBIGINT},
		        {"eligible_ns", LogicalType::BIGINT},    {"source_chunks", LogicalType::UBIGINT},
		        {"source_rows", LogicalType::UBIGINT}};
	}
	throw InternalException("Unknown trace table");
}

struct TraceBindData : public TableFunctionData {
	explicit TraceBindData(TraceTable table_p) : table(table_p) {
	}
	TraceTable table;
	vector<PipelineHookPolicyEntry> policy;
};

struct TraceState : public GlobalTableFunctionState {
	vector<vector<Value>> rows;
	idx_t offset = 0;
	//! The long tables are read in slices from the tracer or the hook: their rows when the scan started
	bool sliced = false;
	TraceTable table = TraceTable::INTERVALS;
	idx_t total = 0;
};

//! Up to `count` rows of a sliced table, from `offset`
vector<vector<Value>> ReadSlice(ClientContext &context, TraceTable table, idx_t offset, idx_t count) {
	auto &scheduler = TaskScheduler::GetScheduler(context);
	switch (table) {
	case TraceTable::INTERVALS:
		return scheduler.GetTracer().GetIntervalRows(offset, count);
	case TraceTable::TICKS:
		return scheduler.GetTracer().GetTickRows(offset, count);
	case TraceTable::TASKS:
		return scheduler.GetHook().GetTaskRows(offset, count);
	case TraceTable::ATTACHMENTS:
		return scheduler.GetHook().GetAttachmentRows(offset, count);
	default:
		throw InternalException("Trace table is not read in slices");
	}
}

template <TraceTable TABLE>
unique_ptr<FunctionData> TraceBind(ClientContext &context, TableFunctionBindInput &input,
                                   vector<LogicalType> &return_types, vector<Identifier> &names) {
	for (auto &column : GetTraceColumns(TABLE)) {
		names.emplace_back(column.name);
		return_types.push_back(column.type);
	}
	return make_uniq<TraceBindData>(TABLE);
}

vector<idx_t> ListArgument(const Value &value, const char *name) {
	if (value.IsNull()) {
		throw InvalidInputException("dbsched_set_policy: %s must not be NULL", name);
	}
	vector<idx_t> result;
	for (auto &child : ListValue::GetChildren(value)) {
		if (child.IsNull()) {
			throw InvalidInputException("dbsched_set_policy: %s must not contain NULL", name);
		}
		result.push_back(child.GetValue<uint64_t>());
	}
	return result;
}

unique_ptr<FunctionData> SetPolicyBind(ClientContext &context, TableFunctionBindInput &input,
                                       vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<TraceBindData>(TraceTable::SET_POLICY);
	for (auto &column : GetTraceColumns(TraceTable::SET_POLICY)) {
		names.emplace_back(column.name);
		return_types.push_back(column.type);
	}
	auto query_ids = ListArgument(input.inputs[0], "query_ids");
	auto pipeline_ids = ListArgument(input.inputs[1], "pipeline_ids");
	auto caps = ListArgument(input.inputs[2], "caps");
	if (query_ids.size() != pipeline_ids.size() || query_ids.size() != caps.size()) {
		throw InvalidInputException("dbsched_set_policy: query_ids, pipeline_ids, and caps must have the same length");
	}
	for (idx_t i = 0; i < query_ids.size(); i++) {
		result->policy.push_back({query_ids[i], pipeline_ids[i], caps[i]});
	}
	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> TraceInit(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<TraceState>();
	auto &tracer = TaskScheduler::GetScheduler(context).GetTracer();
	result->table = input.bind_data->Cast<TraceBindData>().table;
	result->sliced = result->table == TraceTable::INTERVALS || result->table == TraceTable::TICKS ||
	                 result->table == TraceTable::TASKS || result->table == TraceTable::ATTACHMENTS;
	switch (result->table) {
	case TraceTable::INTERVALS:
		result->total = tracer.GetIntervalRowCount();
		break;
	case TraceTable::TICKS:
		result->total = tracer.GetTickRowCount();
		break;
	case TraceTable::PIPELINES:
		result->rows = tracer.GetPipelineRows();
		break;
	case TraceTable::QUERIES:
		result->rows = tracer.GetQueryRows();
		break;
	case TraceTable::RESET:
		tracer.Reset();
		result->rows.push_back({Value::BOOLEAN(true)});
		break;
	case TraceTable::STATE:
		result->rows = TaskScheduler::GetScheduler(context).GetHook().GetStateRows();
		break;
	case TraceTable::TASKS:
		result->total = TaskScheduler::GetScheduler(context).GetHook().GetTaskRowCount();
		break;
	case TraceTable::ATTACHMENTS:
		result->total = TaskScheduler::GetScheduler(context).GetHook().GetAttachmentRowCount();
		break;
	case TraceTable::SET_POLICY:
		TaskScheduler::GetScheduler(context).GetHook().SetPolicy(input.bind_data->Cast<TraceBindData>().policy);
		result->rows.push_back({Value::BOOLEAN(true)});
		break;
	}
	return std::move(result);
}

void TraceFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<TraceState>();
	if (state.sliced) {
		// read the next slice only: the whole table can be far larger than memory as Values
		auto rows = ReadSlice(context, state.table, state.offset,
		                      MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.total - MinValue(state.offset, state.total)));
		for (idx_t i = 0; i < rows.size(); i++) {
			for (idx_t col = 0; col < rows[i].size(); col++) {
				output.SetValue(col, i, rows[i][col]);
			}
		}
		state.offset += rows.size();
		output.SetCardinality(rows.size());
		return;
	}
	idx_t count = 0;
	while (state.offset < state.rows.size() && count < STANDARD_VECTOR_SIZE) {
		auto &row = state.rows[state.offset++];
		for (idx_t col = 0; col < row.size(); col++) {
			output.SetValue(col, count, row[col]);
		}
		count++;
	}
	output.SetCardinality(count);
}

template <TraceTable TABLE>
void RegisterTraceFunction(BuiltinFunctions &set, const char *name) {
	set.AddFunction(TableFunction(name, {}, TraceFunction, TraceBind<TABLE>, TraceInit));
}

} // namespace

void DbschedTraceFun::RegisterFunction(BuiltinFunctions &set) {
	RegisterTraceFunction<TraceTable::INTERVALS>(set, "dbsched_trace_intervals");
	RegisterTraceFunction<TraceTable::TICKS>(set, "dbsched_trace_ticks");
	RegisterTraceFunction<TraceTable::PIPELINES>(set, "dbsched_trace_pipelines");
	RegisterTraceFunction<TraceTable::QUERIES>(set, "dbsched_trace_queries");
	RegisterTraceFunction<TraceTable::RESET>(set, "dbsched_trace_reset");
	RegisterTraceFunction<TraceTable::STATE>(set, "dbsched_scheduler_state");
	RegisterTraceFunction<TraceTable::TASKS>(set, "dbsched_trace_tasks");
	RegisterTraceFunction<TraceTable::ATTACHMENTS>(set, "dbsched_trace_attachments");
	auto id_list = LogicalType::LIST(LogicalType::UBIGINT);
	set.AddFunction(
	    TableFunction("dbsched_set_policy", {id_list, id_list, id_list}, TraceFunction, SetPolicyBind, TraceInit));
}

} // namespace duckdb
