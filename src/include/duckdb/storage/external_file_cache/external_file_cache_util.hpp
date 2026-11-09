//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/external_file_cache/external_file_cache_util.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/cache_validation_mode.hpp"
#include "duckdb/common/optional_ptr.hpp"

namespace duckdb {

struct OpenFileInfo;
class DatabaseInstance;
class Context;

struct ExternalFileCacheUtil {
	//! Get the cache validation mode in order of file open info, execution context (for client-local settings), or
	//! database config.
	DUCKDB_API static CacheValidationMode GetCacheValidationMode(const OpenFileInfo &info,
	                                                             optional_ptr<Context> context, DatabaseInstance &db);
};

} // namespace duckdb
