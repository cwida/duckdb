//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/sampling_pushdown_registry.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/string.hpp"
#include "duckdb/common/winapi.hpp"

namespace duckdb {

class ClientContext;
class DatabaseInstance;
struct FunctionData;
class SampleOptions;

//! Optional per-table-function callback for sampling pushdown eligibility.
//! Registered out-of-band to preserve TableFunction ABI with prebuilt extensions.
//! Callbacks live on DatabaseInstance so loadable extensions that statically link
//! duckdb still share state with the host optimizer.
typedef bool (*table_function_supports_sampling_pushdown_t)(const FunctionData &bind_data,
                                                            const SampleOptions &sample_options);

class SamplingPushdownRegistry {
public:
	DUCKDB_API static void Register(DatabaseInstance &db, const string &function_name,
	                                table_function_supports_sampling_pushdown_t callback);
	DUCKDB_API static table_function_supports_sampling_pushdown_t Lookup(ClientContext &context,
	                                                                     const string &function_name);
};

} // namespace duckdb
