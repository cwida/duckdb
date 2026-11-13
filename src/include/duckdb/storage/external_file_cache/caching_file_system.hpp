//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/external_file_cache/caching_file_system.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/cache_validation_mode.hpp"
#include "duckdb/common/file_open_flags.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/open_file_info.hpp"
#include "duckdb/common/query_context.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/winapi.hpp"
#include "duckdb/storage/external_file_cache/external_file_cache.hpp"
#include "duckdb/storage/external_file_cache/file_buffer_handle_group.hpp"

namespace duckdb {

class Allocator;
class BufferHandle;
class ClientContext;
class DatabaseInstance;
class FileOpenFlags;
class FileSystem;
struct FileHandle;
struct NetworkThroughputEstimate;
class QueryContext;
class CachingFileSystem;

struct ReadThroughputEstimator {
	//! Record a timed read.
	void AddSample(double seconds, idx_t bytes);
	//! Try to produce an estimate. Needs reads of at least two different sizes to separate latency from bandwidth.
	bool TryEstimate(NetworkThroughputEstimate &result) const;

private:
	mutable mutex lock;
	idx_t sample_count = 0;
	double sum_bytes = 0;
	double sum_seconds = 0;
	//! sum of bytes squared
	double sum_bytes_sq = 0;
	//! sum of bytes times seconds
	double sum_bytes_seconds = 0;
};

struct CachingFileHandle {
public:
	using CachedFile = ExternalFileCache::CachedFile;

public:
	DUCKDB_API CachingFileHandle(QueryContext context, CachingFileSystem &caching_file_system, const OpenFileInfo &path,
	                             FileOpenFlags flags, optional_ptr<FileOpener> opener);
	DUCKDB_API ~CachingFileHandle();

public:
	//! Get shared ownership of the underlying FileHandle
	DUCKDB_API shared_ptr<FileHandle> GetFileHandle();
	//! Get the buffer-manager-backed Allocator.
	DUCKDB_API Allocator &GetBufferAllocator() const;
	//! Read [nr_bytes] bytes at the requested [location].
	//! Returns a buffer handle group that keeps the data pinned in memory.
	DUCKDB_API FileBufferHandleGroup Read(idx_t nr_bytes, idx_t location);
	//! Read [nr_bytes] bytes at [location]; within absorb_ranges ([begin, end) pairs), smaller cached blocks are
	//! re-fetched with the gaps around them when that saves a request (see ExternalFileCache::AcquireBlocks)
	DUCKDB_API FileBufferHandleGroup Read(idx_t nr_bytes, idx_t location,
	                                      const vector<pair<idx_t, idx_t>> &absorb_ranges);
	//! Read [nr_bytes] bytes and sets [nr_bytes] to the actually read bytes.
	DUCKDB_API FileBufferHandleGroup Read(idx_t &nr_bytes);
	//! Read and record time
	DUCKDB_API void ReadAndRecord(QueryContext context, data_ptr_t buffer, idx_t nr_bytes, idx_t location);
	//! Whether responses observed by this handle prohibit sharing cached data (e.g., HTTP no-store/Vary)
	DUCKDB_API bool IsCacheReuseProhibited();
	//! Get some properties of the file
	DUCKDB_API string GetPath() const;
	DUCKDB_API idx_t GetFileSize();
	DUCKDB_API timestamp_t GetLastModifiedTime();
	DUCKDB_API string GetVersionTag();
	DUCKDB_API bool Validate() const;
	DUCKDB_API bool CanSeek();
	DUCKDB_API bool IsRemoteFile() const;
	DUCKDB_API bool OnDiskFile();
	//! Whether reads through this handle go through the external file cache (enabled, file cacheable, metadata valid)
	DUCKDB_API bool ReadsAreCached();
	DUCKDB_API bool TryGetNetworkThroughput(NetworkThroughputEstimate &result);
	//! The number of small cached blocks of this file whose data is still available (resident or spilled) that
	//! overlap [location, location + nr_bytes); 0 without a cache
	DUCKDB_API idx_t CountCachedBlocks(idx_t location, idx_t nr_bytes);
	//! Whether the cache can serve all of [location, location + nr_bytes) without a read; false without a cache
	DUCKDB_API bool IsCached(idx_t location, idx_t nr_bytes);
	//! The memory limit of the buffer manager that holds the cached blocks
	DUCKDB_API idx_t GetMaxMemory() const;
	DUCKDB_API idx_t SeekPosition();
	DUCKDB_API void Seek(idx_t location);

private:
	//! Remove the 'force_full_download' option from the file handle if present, and return whether it was present
	bool StripForceFullDownloadIfPresent();
	//! Refresh the cached file if the global cache state has changed.
	shared_ptr<CachedFile> EnsureCachedFileCurrent();
	//! Whether validation metadata permits using cached blocks.
	bool CanUseCache();
	//! Read without caching, splitting large reads of cacheable files into parallel reads of at most a block.
	void ReadUncached(data_ptr_t buffer, idx_t nr_bytes, idx_t location);
	//! The size of the blocks a read of [nr_bytes] is split into, [max_block_size], or for remote files a multiple of
	//! it sized from the measured latency and bandwidth of their requests.
	idx_t GetSplitBlockSize(idx_t nr_bytes, idx_t max_block_size);
	//! Reconcile cached blocks with validation metadata observed while reading them.
	void ReconcileCacheAfterRead(CachedFile &cached_file);
	//! Record a timed read of a local file into the throughput estimate
	void RecordReadThroughput(double total_seconds, idx_t bytes);
	//! Keep the throughput estimate of the host of a remote file current after a read
	void PublishHostNetworkThroughput(FileHandle &handle);

private:
	QueryContext context;

	//! The client file system, held directly because the caching file system can be destroyed before this handle
	FileSystem &file_system;
	//! The DatabaseInstance
	DatabaseInstance &db;
	//! The DB external file cache
	ExternalFileCache &external_file_cache;
	//! For opening the file (possibly with extra info)
	OpenFileInfo path;
	//! Flags used to open the file
	FileOpenFlags flags;
	//! File opener, which contains file open context.
	optional_ptr<FileOpener> opener;
	//! Cache validation mode for this file
	CacheValidationMode validate;
	//! Associated cached file.
	shared_ptr<CachedFile> cached_file;

	//! Used to ensure file handle and cached file metadata is only initialized once.
	annotated_mutex file_handle_mutex;
	//! File handle for the internal filesystem.
	shared_ptr<FileHandle> file_handle;
	//! Metadata snapshot taken with a single Stats call when the file handle is opened.
	CacheValidationInfo validation_info;

	//! Current position (if non-seeking reads)
	idx_t position;

	//! Throughput fitted from this handle's own reads, used for local files (remote files measure their own).
	ReadThroughputEstimator throughput_estimator;
};

//! CachingFileSystem is a read-only file system that closely resembles the FileSystem API.
//! Instead of reading into a designated buffer, it caches reads using the BufferManager,
//! it returns a BufferHandle and sets a pointer into it
class CachingFileSystem {
private:
	friend struct CachingFileHandle;

public:
	// Notice, the provided [file_system] should be a raw, non-caching filesystem.
	DUCKDB_API CachingFileSystem(FileSystem &file_system, DatabaseInstance &db);
	DUCKDB_API ~CachingFileSystem();

public:
	DUCKDB_API static CachingFileSystem Get(ClientContext &context);

	DUCKDB_API unique_ptr<CachingFileHandle> OpenFile(const OpenFileInfo &path, const FileOpenFlags &flags,
	                                                  optional_ptr<FileOpener> opener = nullptr);
	DUCKDB_API unique_ptr<CachingFileHandle> OpenFile(QueryContext context, const OpenFileInfo &path,
	                                                  const FileOpenFlags &flags,
	                                                  optional_ptr<FileOpener> opener = nullptr);

private:
	//! The Client FileSystem (needs to be client-specific so we can do, e.g., HTTPFS profiling)
	FileSystem &file_system;
	//! The DatabaseInstance.
	DatabaseInstance &db;
	//! The External File Cache that caches the files
	ExternalFileCache &external_file_cache;
};

} // namespace duckdb
