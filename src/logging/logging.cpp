#include "duckdb/logging/log_sink.hpp"
#include "duckdb/logging/logger.hpp"

namespace duckdb {

LogConfig::LogConfig()
    : enabled(false), mode(LogMode::LEVEL_ONLY), level(DEFAULT_LOG_LEVEL), storage(DEFAULT_LOG_STORAGE) {
}

bool LogConfig::IsConsistent() const {
	if (mode == LogMode::LEVEL_ONLY) {
		return enabled_log_types.empty() && disabled_log_types.empty();
	}
	if (mode == LogMode::DISABLE_SELECTED) {
		return enabled_log_types.empty() && !disabled_log_types.empty();
	}
	if (mode == LogMode::ENABLE_SELECTED) {
		return !enabled_log_types.empty() && disabled_log_types.empty();
	}
	return false;
}

LogConfig::LogConfig(bool enabled, LogLevel level_p, LogMode mode_p,
                     optional_ptr<unordered_set<string>> enabled_log_types_p,
                     optional_ptr<unordered_set<string>> disabled_log_types_p)
    : enabled(enabled), mode(mode_p), level(level_p), enabled_log_types(enabled_log_types_p),
      disabled_log_types(disabled_log_types_p) {
	storage = LogConfig::IN_MEMORY_STORAGE_NAME;
}

} // namespace duckdb
