//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/database_file_path_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/enums/on_create_conflict.hpp"
#include "duckdb/common/enums/access_mode.hpp"
#include "duckdb/common/reference_map.hpp"

namespace duckdb {
struct AttachInfo;
struct AttachOptions;
class DatabaseManager;

enum class InsertDatabasePathResult { SUCCESS, ALREADY_EXISTS };

struct DatabasePathInfo {
	DatabasePathInfo(DatabaseManager &manager, const Identifier &name_p, AccessMode access_mode);

	string name;
	AccessMode access_mode;
	reference_set_t<DatabaseManager> attached_databases;
	idx_t reference_count = 1;
};

//! The DatabaseFilePathManager is used to ensure we only ever open a single database file once
class DatabaseFilePathManager {
public:
	idx_t ApproxDatabaseCount() const;
	InsertDatabasePathResult InsertDatabasePath(DatabaseManager &manager, const string &path, const Identifier &name,
	                                            OnCreateConflict on_conflict, AttachOptions &options);
	//! Erase a database path - indicating we are done with using it
	void EraseDatabasePath(const string &path);
	//! Called when a database is detached, but before it is fully finished being used
	void DetachDatabase(DatabaseManager &manager, const string &path);
	//! Whether the (canonicalized) path is in use by an attached database
	bool IsDatabasePath(const string &path) const;

private:
	//! The lock to add entries to the db_paths map
	mutable mutex db_paths_lock;
	//! A set containing all attached database path
	//! This allows to attach many databases efficiently, and to avoid attaching the
	//! same file path twice
#if defined(_WIN32) || defined(__APPLE__)
	case_insensitive_map_t<DatabasePathInfo> db_paths;
#else  // !(_WIN32 or __APPLE__)
	unordered_map<string, DatabasePathInfo> db_paths;
#endif // _WIN32 or __APPLE__
};

} // namespace duckdb
