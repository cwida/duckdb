#include "duckdb/main/capi_v2/capi_v2_internal.hpp"

#include "duckdb/logging/log_type.hpp"
#include "duckdb/logging/logger.hpp"

namespace duckdb {
namespace capiv2 {

static LogLevel ConvertLogLevel(DUCKDB_V2_LOG_LEVEL level) {
	switch (level) {
	case DUCKDB_V2_LOG_LEVEL_TRACE:
		return LogLevel::LOG_TRACE;
	case DUCKDB_V2_LOG_LEVEL_DEBUG:
		return LogLevel::LOG_DEBUG;
	case DUCKDB_V2_LOG_LEVEL_INFO:
		return LogLevel::LOG_INFO;
	case DUCKDB_V2_LOG_LEVEL_WARNING:
		return LogLevel::LOG_WARNING;
	case DUCKDB_V2_LOG_LEVEL_ERROR:
		return LogLevel::LOG_ERROR;
	case DUCKDB_V2_LOG_LEVEL_FATAL:
		return LogLevel::LOG_FATAL;
	default:
		throw InvalidInputException("'%d' is not a log level", static_cast<int>(level));
	}
}

static void WriteLog(Logger &logger, DUCKDB_V2_LOG_LEVEL level, const duckdb_v2_str *log_type,
                     const duckdb_v2_str *message) {
	const auto log_level = ConvertLogLevel(level);
	// ShouldLog/WriteLog take a C string, so the borrowed view has to be materialized.
	const string type(Convert(log_type));
	const auto *type_name = type.empty() ? DefaultLogType::NAME : type.c_str();
	if (logger.ShouldLog(type_name, log_level)) {
		logger.WriteLog(type_name, log_level, string(Convert(message)));
	}
}

} // namespace capiv2
} // namespace duckdb

//----------------------------------------------------------------------------------------------------------------------
// Public API
//----------------------------------------------------------------------------------------------------------------------

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_context_log(duckdb_v2_context_handle ctx, DUCKDB_V2_LOG_LEVEL level,
                                      const duckdb_v2_str *log_type, const duckdb_v2_str *message,
                                      duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(ctx);
	return WithErrorHandler(err,
	                        [&]() { WriteLog(duckdb::Logger::Get(Convert(ctx)->context), level, log_type, message); });
}

DUCKDB_V2_ERROR duckdb_v2_instance_log(duckdb_v2_instance_handle instance, DUCKDB_V2_LOG_LEVEL level,
                                       const duckdb_v2_str *log_type, const duckdb_v2_str *message,
                                       duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(instance);
	return WithErrorHandler(err, [&]() {
		WriteLog(duckdb::Logger::Get(*Convert(instance)->GetDatabase().instance), level, log_type, message);
	});
}

DUCKDB_V2_ERROR duckdb_v2_connection_log(duckdb_v2_connection_handle conn, DUCKDB_V2_LOG_LEVEL level,
                                         const duckdb_v2_str *log_type, const duckdb_v2_str *message,
                                         duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(conn);
	return WithErrorHandler(
	    err, [&]() { WriteLog(duckdb::Logger::Get(*Convert(conn)->context), level, log_type, message); });
}
