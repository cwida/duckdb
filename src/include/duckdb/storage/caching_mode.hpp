//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/cache_mode.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>

namespace duckdb {

//! Caching mode for CachingFileSystemWrapper.
//! By default only remote files will be cached, but it's also allowed to cache local for direct IO use case.
enum class CachingMode : uint8_t {
	// Cache all files.
	ALWAYS_CACHE = 0,
	// Only cache remote files, bypass cache for local files.
	CACHE_REMOTE_ONLY = 1,
	// Doesn't perform caching.
	NO_CACHING = 2,
};

enum class RequestSizing : uint8_t {
	BY_CACHE = 0,
	BY_READER = 1,
};

//! How reads of cached files are sized, overriding the RequestSizing of their readers.
enum class ExternalFileCacheRequestSizing : uint8_t {
	// Each reader chooses through its RequestSizing.
	AUTO = 0,
	// Every read covers the aligned blocks of the cache block size around it.
	GRID = 1,
	// Every read covers exactly the bytes it requests.
	EXACT = 2,
};

} // namespace duckdb
