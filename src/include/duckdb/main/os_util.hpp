//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/os_util.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/string.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/winapi.hpp"

namespace duckdb {
class DatabaseInstance;

//! Reads process-level state, such as environment variables, on behalf of a database
class OSUtil {
public:
	//! Created when the database is configured, with the embedder's configuration variables
	OSUtil(DatabaseInstance &db, unordered_set<string> configuration_env);

	DUCKDB_API static OSUtil &Get(DatabaseInstance &db);

public:
	//! Reads an environment variable for the user; throws if enable_external_access is disabled
	DUCKDB_API bool GetEnv(const string &name, string &value);
	//! As above, returning an empty string if the variable is unset
	DUCKDB_API string GetEnv(const string &name);

	//! Reads a known configuration variable (see IsConfigurationEnv and configuration_env), regardless of settings
	DUCKDB_API bool GetEnvUnrestricted(const string &name, string &value);
	//! As above, returning an empty string if the variable is unset
	DUCKDB_API string GetEnvUnrestricted(const string &name);

	//! Whether the engine itself configures from this variable
	DUCKDB_API static bool IsConfigurationEnv(const string &name);
	//! Reads one of the engine's own configuration variables when no database exists yet
	DUCKDB_API static bool GetConfigurationEnv(const string &name, string &value);
	//! As above, returning an empty string if the variable is unset
	DUCKDB_API static string GetConfigurationEnv(const string &name);

private:
	//! Reads a variable from the process environment, without any checks
	static bool ReadEnv(const string &name, string &value);

private:
	DatabaseInstance &db;
	//! Variables the embedding application declared as its configuration
	const unordered_set<string> configuration_env;
};

} // namespace duckdb
