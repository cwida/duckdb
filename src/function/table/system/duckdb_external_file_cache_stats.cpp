#include "duckdb/function/table/system_functions.hpp"
#include "duckdb/storage/external_file_cache/external_file_cache.hpp"

namespace duckdb {

struct DuckDBExternalFileCacheStatsData : public GlobalTableFunctionState {
	DuckDBExternalFileCacheStatsData() : done(false) {
	}

	bool done;
};

static unique_ptr<FunctionData> DuckDBExternalFileCacheStatsBind(ClientContext &context, TableFunctionBindInput &input,
                                                                 vector<LogicalType> &return_types,
                                                                 vector<Identifier> &names) {
	names.emplace_back("read_request_bytes");
	return_types.emplace_back(LogicalType::UBIGINT);

	names.emplace_back("cache_hit_request_bytes");
	return_types.emplace_back(LogicalType::UBIGINT);

	names.emplace_back("actual_io_bytes");
	return_types.emplace_back(LogicalType::UBIGINT);

	names.emplace_back("block_hit_count");
	return_types.emplace_back(LogicalType::UBIGINT);

	names.emplace_back("block_miss_count");
	return_types.emplace_back(LogicalType::UBIGINT);

	names.emplace_back("eviction_refetch_count");
	return_types.emplace_back(LogicalType::UBIGINT);

	return nullptr;
}

unique_ptr<GlobalTableFunctionState> DuckDBExternalFileCacheStatsInit(ClientContext &context,
                                                                      TableFunctionInitInput &input) {
	return make_uniq<DuckDBExternalFileCacheStatsData>();
}

void DuckDBExternalFileCacheStatsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<DuckDBExternalFileCacheStatsData>();
	if (data.done) {
		return;
	}
	data.done = true;
	auto &stats = ExternalFileCache::Get(context).GetStats();
	output.data[0].Append(Value::UBIGINT(stats.read_request_bytes.load(std::memory_order_relaxed)));
	output.data[1].Append(Value::UBIGINT(stats.cache_hit_request_bytes.load(std::memory_order_relaxed)));
	output.data[2].Append(Value::UBIGINT(stats.actual_io_bytes.load(std::memory_order_relaxed)));
	output.data[3].Append(Value::UBIGINT(stats.block_hit_count.load(std::memory_order_relaxed)));
	output.data[4].Append(Value::UBIGINT(stats.block_miss_count.load(std::memory_order_relaxed)));
	output.data[5].Append(Value::UBIGINT(stats.eviction_refetch_count.load(std::memory_order_relaxed)));
}

void DuckDBExternalFileCacheStatsFun::RegisterFunction(BuiltinFunctions &set) {
	set.AddFunction(TableFunction("duckdb_external_file_cache_stats", {}, DuckDBExternalFileCacheStatsFunction,
	                              DuckDBExternalFileCacheStatsBind, DuckDBExternalFileCacheStatsInit));
}

} // namespace duckdb
