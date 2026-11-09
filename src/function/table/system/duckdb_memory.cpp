#include "duckdb/function/table/system_functions.hpp"
#include "duckdb/storage/buffer_manager.hpp"

namespace duckdb {

struct DuckDBMemoryData : public GlobalTableFunctionState {
	DuckDBMemoryData() : offset(0) {
	}

	vector<MemoryInformation> entries;
	idx_t offset;
};

static unique_ptr<FunctionData> DuckDBMemoryBind(ClientContext &context, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<Identifier> &names) {
	names.emplace_back("tag");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("memory_usage_bytes");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("temporary_storage_bytes");
	return_types.emplace_back(LogicalType::BIGINT);

	return nullptr;
}

unique_ptr<GlobalTableFunctionState> DuckDBMemoryInit(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<DuckDBMemoryData>();
	result->entries = BufferManager::GetBufferManager(context).GetMemoryUsageInfo();
	return std::move(result);
}

int64_t ClampReportedMemory(idx_t memory_usage) {
	if (memory_usage > static_cast<idx_t>(NumericLimits<int64_t>::Maximum())) {
		return 0;
	}
	return UnsafeNumericCast<int64_t>(memory_usage);
}

void DuckDBMemoryFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<DuckDBMemoryData>();
	if (data.offset >= data.entries.size()) {
		// finished returning values
		return;
	}
	// start returning values
	// either fill up the chunk or return all the remaining columns
	idx_t count = 0;

	// tag, VARCHAR
	auto &tag = output.data[0];
	// memory_usage_bytes, BIGINT
	auto &memory_usage_bytes = output.data[1];
	// temporary_storage_bytes, BIGINT
	auto &temporary_storage_bytes = output.data[2];

	while (data.offset < data.entries.size() && count < STANDARD_VECTOR_SIZE) {
		auto &entry = data.entries[data.offset++];
		tag.Append(Value(EnumUtil::ToString(entry.tag)));
		memory_usage_bytes.Append(Value::BIGINT(ClampReportedMemory(entry.size)));
		temporary_storage_bytes.Append(Value::BIGINT(ClampReportedMemory(entry.evicted_data)));
		count++;
	}
}

void DuckDBMemoryFun::RegisterFunction(BuiltinFunctions &set) {
	TableFunction memory_fun("duckdb_memory", {}, DuckDBMemoryFunction, DuckDBMemoryBind, DuckDBMemoryInit);
	// reads live buffer manager usage
	memory_fun.stability = FunctionStability::VOLATILE;
	set.AddFunction(std::move(memory_fun));
}

} // namespace duckdb
