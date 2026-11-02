#include "duckdb/function/table/system_functions.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/common/map.hpp"
#include "duckdb/main/connection_manager.hpp"

namespace duckdb {

struct DuckDBLiveQueryMetricsData : public GlobalTableFunctionState {
	DuckDBLiveQueryMetricsData() : offset(0) {
	}

	struct Entry {
		connection_t connection_id;
		string metric_name;
		Value metric_value;
	};
	vector<Entry> entries;
	idx_t offset;
};

static unique_ptr<FunctionData> DuckDBLiveQueryMetricsBind(ClientContext &context, TableFunctionBindInput &input,
                                                           vector<LogicalType> &return_types,
                                                           vector<Identifier> &names) {
	names.emplace_back("connection_id");
	return_types.emplace_back(LogicalType::UBIGINT);

	names.emplace_back("metric_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("metric_value");
	return_types.emplace_back(LogicalType::VARIANT());

	return nullptr;
}

static unique_ptr<GlobalTableFunctionState> DuckDBLiveQueryMetricsInit(ClientContext &context,
                                                                       TableFunctionInitInput &input) {
	auto result = make_uniq<DuckDBLiveQueryMetricsData>();
	// the counters of each connection's current query, or of its most recent one if it is idle
	for (auto &connection : ConnectionManager::Get(context).GetConnectionList()) {
		auto metrics = connection->GetLiveQueryMetrics();
		map<string, Value> sorted(metrics.begin(), metrics.end());
		for (auto &metric : sorted) {
			result->entries.push_back(
			    {connection->GetConnectionId(), metric.first, metric.second.CastAs(context, LogicalType::VARIANT())});
		}
	}
	return std::move(result);
}

static void DuckDBLiveQueryMetricsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<DuckDBLiveQueryMetricsData>();
	idx_t count = 0;
	while (data.offset < data.entries.size() && count < STANDARD_VECTOR_SIZE) {
		auto &entry = data.entries[data.offset++];
		output.data[0].Append(Value::UBIGINT(entry.connection_id));
		output.data[1].Append(Value(entry.metric_name));
		output.data[2].Append(entry.metric_value);
		count++;
	}
}

void DuckDBLiveQueryMetricsFun::RegisterFunction(BuiltinFunctions &set) {
	set.AddFunction(TableFunction("duckdb_live_query_metrics", {}, DuckDBLiveQueryMetricsFunction,
	                              DuckDBLiveQueryMetricsBind, DuckDBLiveQueryMetricsInit));
}

} // namespace duckdb
