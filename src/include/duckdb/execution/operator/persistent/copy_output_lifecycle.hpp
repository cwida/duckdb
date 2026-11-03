//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/persistent/copy_output_lifecycle.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/mutex.hpp"
#include "duckdb/common/set.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

class ClientContext;

enum class CopyOutputOwnership : uint8_t {
	//! Preserve the path because this query cannot prove that it created it.
	PRESERVE,
	//! Remove the path if the query fails.
	REMOVE_ON_FAILURE
};

class CopyOutputLifecycle {
public:
	explicit CopyOutputLifecycle(ClientContext &context, bool fsync = false);
	~CopyOutputLifecycle();

public:
	void RegisterFile(string path) DUCKDB_EXCLUDES(lock);
	void RegisterCreatedDirectory(string path) DUCKDB_EXCLUDES(lock);
	//! With FSYNC, syncs a finished local file to stable storage
	void SyncFile(const string &path) DUCKDB_EXCLUDES(lock);
	//! With FSYNC, syncs the directories holding the synced files and the created directories
	void SyncDirectories() DUCKDB_EXCLUDES(lock);
	void MarkSuccessful() noexcept DUCKDB_EXCLUDES(lock);

private:
	struct FileEntry {
		string path;
		CopyOutputOwnership ownership = CopyOutputOwnership::PRESERVE;
	};

private:
	CopyOutputOwnership GetOwnership(const string &path) const DUCKDB_EXCLUDES(lock);
	void Cleanup() DUCKDB_EXCLUDES(lock);

private:
	ClientContext &context;
	//! Whether the output is synced to stable storage before the COPY completes
	const bool fsync;
	annotated_mutex lock;
	vector<FileEntry> files DUCKDB_GUARDED_BY(lock);
	vector<string> created_directories DUCKDB_GUARDED_BY(lock);
	set<string> synced_file_directories DUCKDB_GUARDED_BY(lock);
	bool successful DUCKDB_GUARDED_BY(lock) = false;
};

} // namespace duckdb
