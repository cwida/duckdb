#include "duckdb/main/database_file_path_manager.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/common/enums/on_entry_not_found.hpp"

namespace duckdb {

DatabasePathInfo::DatabasePathInfo(DatabaseManager &manager, const Identifier &name_p, AccessMode access_mode)
    : name(name_p.GetIdentifierName()), access_mode(access_mode) {
	attached_databases.insert(manager);
}

idx_t DatabaseFilePathManager::ApproxDatabaseCount() const {
	lock_guard<mutex> path_lock(db_paths_lock);
	return db_paths.size();
}

InsertDatabasePathResult DatabaseFilePathManager::InsertDatabasePath(DatabaseManager &manager, const string &path,
                                                                     const Identifier &name,
                                                                     OnCreateConflict on_conflict,
                                                                     AttachOptions &options) {
	if (path.empty() || path == IN_MEMORY_PATH) {
		throw InternalException("DatabaseFilePathManager::InsertDatabasePath - cannot insert in-memory database");
	}
	lock_guard<mutex> path_lock(db_paths_lock);
	auto entry = db_paths.emplace(path, DatabasePathInfo(manager, name, options.access_mode));
	if (!entry.second) {
		auto &existing = entry.first->second;
		bool already_exists = false;
		bool attached_in_this_system = false;
		if (on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT && existing.name == name) {
			already_exists = true;
			attached_in_this_system = existing.attached_databases.find(manager) != existing.attached_databases.end();
		}
		if (options.access_mode == AccessMode::READ_ONLY && existing.access_mode == AccessMode::READ_ONLY) {
			if (attached_in_this_system) {
				return InsertDatabasePathResult::ALREADY_EXISTS;
			}
			// all attaches are in read-only mode - there is no conflict, just increase the reference count
			existing.attached_databases.insert(manager);
			existing.reference_count++;
		} else {
			if (already_exists) {
				if (attached_in_this_system) {
					return InsertDatabasePathResult::ALREADY_EXISTS;
				}
				throw ResourceInUseException(
				    "Unique file handle conflict: Cannot attach %s - the database file \"%s\" is in "
				    "the process of being detached",
				    name, path);
			}
			throw ResourceInUseException(
			    "Unique file handle conflict: Cannot attach %s - the database file \"%s\" is already "
			    "attached by database \"%s\"",
			    name, path, existing.name);
		}
	}
	options.stored_database_path = make_uniq<StoredDatabasePath>(manager, *this, path, name);
	return InsertDatabasePathResult::SUCCESS;
}

bool DatabaseFilePathManager::IsDatabasePath(const string &path) const {
	lock_guard<mutex> path_lock(db_paths_lock);
	return db_paths.find(path) != db_paths.end();
}

void DatabaseFilePathManager::EraseDatabasePath(const string &path) {
	if (path.empty() || path == IN_MEMORY_PATH) {
		return;
	}
	lock_guard<mutex> path_lock(db_paths_lock);
	auto entry = db_paths.find(path);
	if (entry != db_paths.end()) {
		if (entry->second.reference_count <= 1) {
			db_paths.erase(entry);
		} else {
			entry->second.reference_count--;
		}
	}
}

void DatabaseFilePathManager::DetachDatabase(DatabaseManager &manager, const string &path) {
	if (path.empty() || path == IN_MEMORY_PATH) {
		return;
	}
	lock_guard<mutex> path_lock(db_paths_lock);
	auto entry = db_paths.find(path);
	if (entry != db_paths.end()) {
		entry->second.attached_databases.erase(manager);
	}
}

} // namespace duckdb
