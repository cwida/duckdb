#include "duckdb/function/table/system_functions.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/storage/external_file_cache/external_file_cache.hpp"

namespace duckdb {

struct DuckDBExternalFileCacheData : public GlobalTableFunctionState {
	DuckDBExternalFileCacheData() : offset(0) {
	}

	vector<CachedFileInformation> entries;
	idx_t offset;
	//! The offset, published once per chunk (for progress)
	atomic<idx_t> progress_offset {0};
};

static unique_ptr<FunctionData> DuckDBExternalFileCacheBind(ClientContext &context, TableFunctionBindInput &input,
                                                            vector<LogicalType> &return_types,
                                                            vector<Identifier> &names) {
	names.emplace_back("path");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("nr_bytes");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("location");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("loaded");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("spilled");
	return_types.emplace_back(LogicalType::BOOLEAN);

	return nullptr;
}

unique_ptr<GlobalTableFunctionState> DuckDBExternalFileCacheInit(ClientContext &context,
                                                                 TableFunctionInitInput &input) {
	auto result = make_uniq<DuckDBExternalFileCacheData>();
	result->entries = ExternalFileCache::Get(context).GetCachedFileInformation();
	return std::move(result);
}

void DuckDBExternalFileCacheFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<DuckDBExternalFileCacheData>();
	if (data.offset >= data.entries.size()) {
		// finished returning values
		return;
	}
	// start returning values
	// either fill up the chunk or return all the remaining columns
	idx_t count = 0;

	// path, VARCHAR
	auto &path = output.data[0];
	// nr_bytes, BIGINT
	auto &nr_bytes = output.data[1];
	// location, BIGINT
	auto &location = output.data[2];
	// loaded, BOOLEAN
	auto &loaded = output.data[3];
	// spilled, BOOLEAN
	auto &spilled = output.data[4];

	while (data.offset < data.entries.size() && count < STANDARD_VECTOR_SIZE) {
		auto &entry = data.entries[data.offset++];
		path.Append(Value(entry.path));
		nr_bytes.Append(Value::BIGINT(NumericCast<int64_t>(entry.nr_bytes)));
		location.Append(Value::BIGINT(NumericCast<int64_t>(entry.location)));
		loaded.Append(Value::BOOLEAN(entry.loaded));
		spilled.Append(Value::BOOLEAN(entry.spilled));
		count++;
	}
	data.progress_offset.store(data.offset, std::memory_order_relaxed);
}

static double DuckDBExternalFileCacheProgress(ClientContext &context, const FunctionData *bind_data,
                                              const GlobalTableFunctionState *global_state) {
	auto &data = global_state->Cast<DuckDBExternalFileCacheData>();
	if (data.entries.empty()) {
		return 100.0;
	}
	auto offset = data.progress_offset.load(std::memory_order_relaxed);
	return 100.0 * static_cast<double>(offset) / static_cast<double>(data.entries.size());
}

void DuckDBExternalFileCacheFun::RegisterFunction(BuiltinFunctions &set) {
	TableFunction function("duckdb_external_file_cache", {}, DuckDBExternalFileCacheFunction,
	                       DuckDBExternalFileCacheBind, DuckDBExternalFileCacheInit);
	function.table_scan_progress = DuckDBExternalFileCacheProgress;
	set.AddFunction(function);
}

} // namespace duckdb
