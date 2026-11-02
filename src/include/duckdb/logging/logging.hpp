//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/logging/log_storage.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/typedefs.hpp"

namespace duckdb {

// Note: `LOG_` prefix is to avoid problems with DEBUG and ERROR macros, these are ToString-ed without the prefix
enum class LogLevel : uint8_t {
	LOG_TRACE = 10,
	LOG_DEBUG = 20,
	LOG_INFO = 30,
	LOG_WARNING = 40,
	LOG_ERROR = 50,
	LOG_FATAL = 60
};

enum class LogContextScope : uint8_t { DATABASE = 10, CONNECTION = 20, THREAD = 30 };

enum class LogMode : uint8_t { LEVEL_ONLY = 0, DISABLE_SELECTED = 1, ENABLE_SELECTED = 2 };

struct LogConfig {
	constexpr static const char *IN_MEMORY_STORAGE_NAME = "memory";
	constexpr static const char *STDOUT_STORAGE_NAME = "stdout";
	constexpr static const char *FILE_STORAGE_NAME = "file";

	constexpr static LogLevel DEFAULT_LOG_LEVEL = LogLevel::LOG_INFO;
	constexpr static const char *DEFAULT_LOG_STORAGE = IN_MEMORY_STORAGE_NAME;

	LogConfig();

	DUCKDB_API bool IsConsistent() const;

	bool enabled;
	LogMode mode;
	LogLevel level;
	string storage;

	unordered_set<string> enabled_log_types;
	unordered_set<string> disabled_log_types;

protected:
	LogConfig(bool enabled, LogLevel level, LogMode mode, optional_ptr<unordered_set<string>> enabled_log_types,
	          optional_ptr<unordered_set<string>> disable_loggers);
};

struct LoggingContext {
	explicit LoggingContext(LogContextScope scope_p) : scope(scope_p) {
	}

	LogContextScope scope;

	optional_idx thread_id;
	optional_idx connection_id;
	optional_idx transaction_id;
	optional_idx query_id;
};

struct RegisteredLoggingContext {
	idx_t context_id;
	LoggingContext context;
};

} // namespace duckdb
