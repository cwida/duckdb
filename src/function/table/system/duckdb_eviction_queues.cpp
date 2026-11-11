#include "duckdb/function/table/system_functions.hpp"
#include "duckdb/storage/buffer/buffer_pool.hpp"
#include "duckdb/storage/buffer_manager.hpp"

namespace duckdb {

struct DuckDBEvictionQueuesData : public GlobalTableFunctionState {
	DuckDBEvictionQueuesData() : offset(0) {
	}

	vector<EvictionQueueInformation> entries;
	idx_t offset;
};

static unique_ptr<FunctionData> DuckDBEvictionQueuesBind(ClientContext &context, TableFunctionBindInput &input,
                                                         vector<LogicalType> &return_types, vector<Identifier> &names) {
	names.emplace_back("queue_index");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("queue_type");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("approximate_size");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("dead_nodes");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("total_insertions");
	return_types.emplace_back(LogicalType::BIGINT);

	return nullptr;
}

unique_ptr<GlobalTableFunctionState> DuckDBEvictionQueuesInit(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<DuckDBEvictionQueuesData>();
	result->entries = BufferManager::GetBufferManager(context).GetBufferPool().GetEvictionQueueInfo();
	return std::move(result);
}

void DuckDBEvictionQueuesFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<DuckDBEvictionQueuesData>();
	if (data.offset >= data.entries.size()) {
		return;
	}
	idx_t count = 0;

	// queue_index, BIGINT
	auto &queue_index = output.data[0];
	// queue_type, VARCHAR
	auto &queue_type = output.data[1];
	// approximate_size, BIGINT
	auto &approximate_size = output.data[2];
	// dead_nodes, BIGINT
	auto &dead_nodes = output.data[3];
	// total_insertions, BIGINT
	auto &total_insertions = output.data[4];

	while (data.offset < data.entries.size() && count < STANDARD_VECTOR_SIZE) {
		auto &entry = data.entries[data.offset++];
		queue_index.Append(Value::BIGINT(UnsafeNumericCast<int64_t>(entry.queue_index)));
		queue_type.Append(Value(entry.queue_type));
		approximate_size.Append(Value::BIGINT(UnsafeNumericCast<int64_t>(entry.approximate_size)));
		dead_nodes.Append(Value::BIGINT(UnsafeNumericCast<int64_t>(entry.dead_nodes)));
		total_insertions.Append(Value::BIGINT(UnsafeNumericCast<int64_t>(entry.total_insertions)));
		count++;
	}
}

void DuckDBEvictionQueuesFun::RegisterFunction(BuiltinFunctions &set) {
	TableFunction eviction_queues_fun("duckdb_eviction_queues", {}, DuckDBEvictionQueuesFunction,
	                                  DuckDBEvictionQueuesBind, DuckDBEvictionQueuesInit);
	// reads live buffer manager state
	eviction_queues_fun.stability = FunctionStability::VOLATILE;
	set.AddFunction(std::move(eviction_queues_fun));
}

} // namespace duckdb
