//===----------------------------------------------------------------------===//
//                         DuckDB
//
// Shared test utilities for external file cache tests.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/file_system.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/pair.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/vector.hpp"
#include "test_helpers.hpp"

namespace duckdb {

class CachingTestFileGuard {
public:
	CachingTestFileGuard(const string &filename, const string &content);
	~CachingTestFileGuard();

	const string &GetPath() const;

private:
	string file_path;
};

const string CACHING_TEST_REMOTE_PREFIX = "s3://efc-test/";

class SimpleTrackingFileSystem : public LocalFileSystem {
public:
	string GetName() const override;
	bool CanHandleFile(const string &path) override;
	bool CanSeek() override;
	FileMetadata Stats(FileHandle &handle) override;
	unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags,
	                                optional_ptr<FileOpener> opener = nullptr) override;
	void Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override;
	vector<pair<idx_t, idx_t>> TakeReads();

private:
	annotated_mutex reads_lock;
	vector<pair<idx_t, idx_t>> reads DUCKDB_GUARDED_BY(reads_lock);
};

//! A file system that returns no ETag and timestamp_t(0) for Last-Modified, simulating servers that do not
//! provide cache-validation headers.
class NoValidationMetadataFileSystem : public LocalFileSystem {
public:
	string GetName() const override;
	bool CanHandleFile(const string &path) override;
	bool CanSeek() override;
	FileMetadata Stats(FileHandle &handle) override;
};

//! A file system without validators that grants a freshness deadline, simulating servers that send
//! no ETag/Last-Modified but do send Cache-Control max-age.
class FreshnessOnlyFileSystem : public NoValidationMetadataFileSystem {
public:
	string GetName() const override;
	FileMetadata Stats(FileHandle &handle) override;

	//! Freshness lifetime granted on each stat; negative values produce an already-expired deadline.
	int64_t max_age_micros = 600 * 1000000LL;
};

//! In-memory DuckDB with the external file cache forced to also cache local files (off by default), so the external
//! file cache tests can exercise the cache machinery on local temp files.
DuckDB MakeCacheLocalFilesDB();

} // namespace duckdb
