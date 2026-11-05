//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/external_file_cache/caching_file_system_wrapper.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enable_shared_from_this_ipp.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/winapi.hpp"
#include "duckdb/storage/external_file_cache/caching_file_system.hpp"
#include "duckdb/storage/caching_mode.hpp"

namespace duckdb {

// Forward declaration.
class DatabaseInstance;
class CachingFileSystemWrapper;
struct CachingFileHandle;

//! CachingFileHandleWrapper wraps CachingFileHandle to conform to FileHandle API.
class CachingFileHandleWrapper : public FileHandle {
	friend class CachingFileSystemWrapper;

public:
	DUCKDB_API CachingFileHandleWrapper(shared_ptr<CachingFileSystemWrapper> file_system,
	                                    unique_ptr<CachingFileHandle> handle, const FileOpenFlags &flags);

	DUCKDB_API ~CachingFileHandleWrapper() override;

	DUCKDB_API void Close() override;

private:
	// CachingFileSystem is not kept within VFS as other filesystems, so sometimes it's necessary to pin it inside of
	// file handle and ensure it's valid.
	shared_ptr<CachingFileSystemWrapper> caching_file_system;

	unique_ptr<CachingFileHandle> caching_handle;
};

//! [CachingFileSystemWrapper] is an adapter class, which wraps [CachingFileSystem] to conform to FileSystem API.
//! Different from [CachingFileSystem], which owns cache content and returns a [BufferHandle] to achieve zero-copy on
//! read, the wrapper class always copies requested bytes into the provided address.
//!
//! NOTICE: Currently only read and seek operations are supported, write operations are disabled.
class CachingFileSystemWrapper : public FileSystem, public enable_shared_from_this<CachingFileSystemWrapper> {
public:
	DUCKDB_API CachingFileSystemWrapper(FileSystem &file_system, DatabaseInstance &db,
	                                    CachingMode mode = CachingMode::CACHE_REMOTE_ONLY);
	DUCKDB_API CachingFileSystemWrapper(FileSystem &file_system, optional_ptr<FileOpener> file_opener,
	                                    CachingMode mode = CachingMode::CACHE_REMOTE_ONLY);
	DUCKDB_API ~CachingFileSystemWrapper() override;

	DUCKDB_API std::string GetName() const override;

	DUCKDB_API unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags,
	                                           optional_ptr<FileOpener> opener = nullptr) override;
	DUCKDB_API unique_ptr<FileHandle> OpenFile(const OpenFileInfo &path, FileOpenFlags flags,
	                                           optional_ptr<FileOpener> opener = nullptr);

	DUCKDB_API void Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override;
	DUCKDB_API void Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override;
	DUCKDB_API int64_t Read(FileHandle &handle, void *buffer, int64_t nr_bytes) override;
	DUCKDB_API int64_t Write(FileHandle &handle, void *buffer, int64_t nr_bytes) override;
	DUCKDB_API bool Trim(FileHandle &handle, idx_t offset_bytes, idx_t length_bytes) override;

	DUCKDB_API int64_t GetFileSize(FileHandle &handle) override;
	DUCKDB_API timestamp_t GetLastModifiedTime(FileHandle &handle) override;
	DUCKDB_API string GetVersionTag(FileHandle &handle) override;
	DUCKDB_API FileType GetFileType(FileHandle &handle) override;
	DUCKDB_API FileMetadata Stats(FileHandle &handle) override;
	DUCKDB_API optional<FileMetadata> GetStatsIfExists(const OpenFileInfo &file,
	                                                   optional_ptr<FileOpener> opener = nullptr) override;
	DUCKDB_API void Truncate(FileHandle &handle, int64_t new_size) override;
	DUCKDB_API void FileSync(FileHandle &handle) override;

	DUCKDB_API bool DirectoryExists(const string &directory, optional_ptr<FileOpener> opener = nullptr) override;
	DUCKDB_API void CreateDirectory(const string &directory, optional_ptr<FileOpener> opener = nullptr) override;
	DUCKDB_API bool CreateDirectoryExtended(const string &directory, const CreateDirectoryOptions &options,
	                                        optional_ptr<FileOpener> opener = nullptr) override;
	DUCKDB_API void CreateDirectoriesRecursive(const string &path, optional_ptr<FileOpener> opener = nullptr) override;
	DUCKDB_API void RemoveDirectory(const string &directory, optional_ptr<FileOpener> opener = nullptr) override;
	DUCKDB_API bool RemoveDirectoryExtended(const string &directory, const RemoveDirectoryOptions &options,
	                                        optional_ptr<FileOpener> opener = nullptr) override;

	DUCKDB_API bool ListFiles(const string &directory, const std::function<void(const string &, bool)> &callback,
	                          FileOpener *opener = nullptr) override;

	DUCKDB_API void MoveFile(const string &source, const string &target,
	                         optional_ptr<FileOpener> opener = nullptr) override;
	DUCKDB_API bool FileExists(const string &filename, optional_ptr<FileOpener> opener = nullptr) override;
	DUCKDB_API bool IsPipe(const string &filename, optional_ptr<FileOpener> opener = nullptr) override;
	DUCKDB_API void RemoveFile(const string &filename, optional_ptr<FileOpener> opener = nullptr) override;
	DUCKDB_API bool TryRemoveFile(const string &filename, optional_ptr<FileOpener> opener = nullptr) override;

	DUCKDB_API string GetHomeDirectory() override;
	DUCKDB_API string ExpandPath(const string &path) override;
	DUCKDB_API string PathSeparator(const string &path) override;

	DUCKDB_API vector<OpenFileInfo> Glob(const string &path, FileOpener *opener = nullptr) override;

	DUCKDB_API void RegisterSubSystem(unique_ptr<FileSystem> sub_fs) override;
	DUCKDB_API void RegisterCompressionFilesystem(unique_ptr<CompressedFileSystem> fs) override;
	DUCKDB_API void UnregisterSubSystem(const string &name) override;
	DUCKDB_API unique_ptr<FileSystem> ExtractSubSystem(const string &name) override;
	DUCKDB_API vector<string> ListSubSystems() override;
	DUCKDB_API bool CanHandleFile(const string &fpath) override;

	DUCKDB_API void Seek(FileHandle &handle, idx_t location) override;
	DUCKDB_API void Reset(FileHandle &handle) override;
	DUCKDB_API idx_t SeekPosition(FileHandle &handle) override;

	DUCKDB_API bool IsManuallySet() override;
	DUCKDB_API bool CanSeek() override;
	DUCKDB_API bool OnDiskFile(FileHandle &handle) override;

	DUCKDB_API unique_ptr<FileHandle> OpenCompressedFile(QueryContext context, unique_ptr<FileHandle> handle,
	                                                     bool write) override;

	DUCKDB_API void SetDisabledFileSystems(const vector<string> &names) override;
	DUCKDB_API bool SubSystemIsDisabled(const string &name) override;
	DUCKDB_API bool IsDisabledForPath(const string &path) override;

protected:
	DUCKDB_API unique_ptr<FileHandle> OpenFileExtended(const OpenFileInfo &path, FileOpenFlags flags,
	                                                   optional_ptr<FileOpener> opener) override;
	DUCKDB_API bool SupportsOpenFileExtended() const override;
	DUCKDB_API bool ListFilesExtended(const string &directory, const std::function<void(OpenFileInfo &info)> &callback,
	                                  optional_ptr<FileOpener> opener) override;
	DUCKDB_API bool SupportsListFilesExtended() const override;

private:
	bool ShouldUseCache(const string &path) const;

	// Return an optional caching file handle, if certain filepath is cached.
	CachingFileHandle *GetCachingHandleIfPossible(FileHandle &handle);

	CachingFileSystem caching_file_system;
	FileSystem &underlying_file_system;
	CachingMode caching_mode;
};

} // namespace duckdb
