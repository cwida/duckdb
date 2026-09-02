#include "duckdb/optimizer/sampling_pushdown_registry.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"

namespace duckdb {

void SamplingPushdownRegistry::Register(DatabaseInstance &db, const string &function_name,
                                        table_function_supports_sampling_pushdown_t callback) {
	db.sampling_pushdown_callbacks[function_name] = callback;
}

table_function_supports_sampling_pushdown_t SamplingPushdownRegistry::Lookup(ClientContext &context,
                                                                             const string &function_name) {
	auto &db = DatabaseInstance::GetDatabase(context);
	auto entry = db.sampling_pushdown_callbacks.find(function_name);
	if (entry == db.sampling_pushdown_callbacks.end()) {
		return nullptr;
	}
	return entry->second;
}

} // namespace duckdb
