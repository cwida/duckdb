//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/logging/file_system_logger.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

namespace duckdb {

#define DUCKDB_LOG_FILE_SYSTEM_BYTES(HANDLE, OP, BYTES, POS)                                                           \
	{                                                                                                                  \
		if (HANDLE.logger) {                                                                                           \
			DUCKDB_LOG(HANDLE.logger, FileSystemLogType, HANDLE, OP, BYTES, POS)                                       \
		}                                                                                                              \
	}
#define DUCKDB_LOG_FILE_SYSTEM(HANDLE, OP)                                                                             \
	{                                                                                                                  \
		if (HANDLE.logger) {                                                                                           \
			DUCKDB_LOG(HANDLE.logger, FileSystemLogType, HANDLE, OP)                                                   \
		}                                                                                                              \
	}

// Macros for logging to file handles
#define DUCKDB_LOG_FILE_SYSTEM_READ(HANDLE, BYTES, POS)  DUCKDB_LOG_FILE_SYSTEM_BYTES(HANDLE, "READ", BYTES, POS);
#define DUCKDB_LOG_FILE_SYSTEM_WRITE(HANDLE, BYTES, POS) DUCKDB_LOG_FILE_SYSTEM_BYTES(HANDLE, "WRITE", BYTES, POS);
#define DUCKDB_LOG_FILE_SYSTEM_OPEN(HANDLE)              DUCKDB_LOG_FILE_SYSTEM(HANDLE, "OPEN");
#define DUCKDB_LOG_FILE_SYSTEM_CLOSE(HANDLE)             DUCKDB_LOG_FILE_SYSTEM(HANDLE, "CLOSE");
#define DUCKDB_LOG_FILE_SYSTEM_SYNC(HANDLE)              DUCKDB_LOG_FILE_SYSTEM(HANDLE, "SYNC");

} // namespace duckdb
