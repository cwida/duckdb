//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/file_system.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/file_compression_type.hpp"
#include "duckdb/common/enums/file_glob_options.hpp"
#include "duckdb/common/enums/file_write_mode.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/file_buffer.hpp"
#include "duckdb/common/file_open_flags.hpp"
#include "duckdb/common/open_file_info.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/path.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/vector.hpp"

#include <functional>

#undef CreateDirectory
#undef MoveFile
#undef RemoveDirectory

namespace duckdb {

class AttachedDatabase;
class DatabaseInstance;
class FileOpener;
class FileSystem;
class Logger;
class ClientContext;
class CompressedFileSystem;
class QueryContext;
class MemoryMappedFile;
struct MMapOptions;
class MultiFileList;

enum class CreateDirectoryMode : uint8_t {
	//! Create the target directory only; its parent directory must already exist.
	SINGLE,
	//! Create the target directory and any missing parent directories.
	RECURSIVE
};

enum class RemoveDirectoryMode : uint8_t {
	//! Remove the target only; return false if it is missing or non-empty and never remove its contents.
	SINGLE,
	//! Remove the target directory and all files and directories contained in it.
	RECURSIVE
};

struct CreateDirectoryOptions {
	CreateDirectoryMode mode = CreateDirectoryMode::SINGLE;
};

struct RemoveDirectoryOptions {
	RemoveDirectoryMode mode = RemoveDirectoryMode::SINGLE;
};

enum class FileType {
	//! Regular file
	FILE_TYPE_REGULAR,
	//! Directory
	FILE_TYPE_DIR,
	//! FIFO named pipe
	FILE_TYPE_FIFO,
	//! Socket
	FILE_TYPE_SOCKET,
	//! Symbolic link
	FILE_TYPE_LINK,
	//! Block device
	FILE_TYPE_BLOCKDEV,
	//! Character device
	FILE_TYPE_CHARDEV,
	//! Unknown or invalid file handle
	FILE_TYPE_INVALID,
};

struct FileMetadata {
	int64_t file_size = -1;
	timestamp_t last_modification_time = timestamp_t::ninfinity();
	FileType file_type = FileType::FILE_TYPE_INVALID;
	optional_idx device_id;
	optional_idx file_id;
	//! Optional: tag that uniquely identifies the version of the file (e.g., HTTP ETag).
	//! Empty if the storage backend does not provide one.
	string version_tag;
	//! Time until which (inclusive) cached data may be served without revalidation.
	//! Unset means the backend provides no freshness information; infinities mean always valid/invalid.
	optional<timestamp_t> cache_valid_until;

	// A key-value pair of the extended file metadata, which could store any attributes.
	unordered_map<string, Value> extended_file_info;
};

//! Measured network throughput for a (remote) file handle. Used to size prefetch coalescing gaps.
struct NetworkThroughputEstimate {
	//! Round-trip latency + request setup, in seconds
	double latency_seconds = 0;
	//! Single-stream throughput, in bytes per second
	double bandwidth_bytes_per_s = 0;
};

struct FileHandle {
public:
	DUCKDB_API FileHandle(FileSystem &file_system, string path, FileOpenFlags flags);
	FileHandle(const FileHandle &) = delete;
	DUCKDB_API virtual ~FileHandle();

	// Read at [nr_bytes] bytes into [buffer], and return the bytes actually read.
	// File offset will be changed, which advances for number of bytes read.
	DUCKDB_API int64_t Read(void *buffer, idx_t nr_bytes);
	DUCKDB_API int64_t Read(QueryContext context, void *buffer, idx_t nr_bytes);
	DUCKDB_API int64_t Write(void *buffer, idx_t nr_bytes);
	DUCKDB_API int64_t Write(QueryContext context, void *buffer, idx_t nr_bytes);
	// Read at [nr_bytes] bytes into [buffer].
	// File offset will not be changed.
	DUCKDB_API void Read(void *buffer, idx_t nr_bytes, idx_t location);
	DUCKDB_API void Read(QueryContext context, void *buffer, idx_t nr_bytes, idx_t location);
	DUCKDB_API void Write(QueryContext context, void *buffer, idx_t nr_bytes, idx_t location);
	DUCKDB_API void Seek(idx_t location);
	DUCKDB_API void Reset();
	DUCKDB_API idx_t SeekPosition();
	DUCKDB_API void Sync();
	DUCKDB_API void Truncate(int64_t new_size);
	//! Abandon an incomplete write without publishing any buffered or staged data.
	DUCKDB_API void AbortWrite();
	DUCKDB_API string ReadLine();
	DUCKDB_API string ReadLine(QueryContext context);
	DUCKDB_API bool Trim(idx_t offset_bytes, idx_t length_bytes);
	DUCKDB_API virtual idx_t GetProgress();
	DUCKDB_API virtual FileCompressionType GetFileCompressionType();

	DUCKDB_API virtual bool CanSeek();
	DUCKDB_API FileWriteMode GetWriteMode();
	DUCKDB_API bool IsPipe();
	DUCKDB_API bool OnDiskFile();
	//! Try to obtain a network throughput estimate (Local files return false).
	DUCKDB_API bool TryGetNetworkThroughput(NetworkThroughputEstimate &result);
	DUCKDB_API idx_t GetFileSize();
	DUCKDB_API FileType GetType();
	DUCKDB_API FileMetadata Stats();

	DUCKDB_API void TryAddLogger(FileOpener &opener);

	//! Closes the file handle.
	DUCKDB_API virtual void Close() = 0;

	string GetPath() const {
		return path;
	}

	FileOpenFlags GetFlags() const {
		return flags;
	}

	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}

public:
	FileSystem &file_system;
	string path;
	FileOpenFlags flags;

	shared_ptr<Logger> logger;
	//! Whether reads/writes through this handle are counted in the query's I/O metrics. Set to false for
	//! wrapper handles (e.g. compressed files) that delegate the real on-disk I/O to a child handle, so that
	//! the bytes are attributed to the child handle (the actual disk I/O) and not double-counted.
	bool track_io = true;
};

class FileSystem {
public:
	DUCKDB_API virtual ~FileSystem();

public:
	DUCKDB_API static FileSystem &GetFileSystem(ClientContext &context);
	DUCKDB_API static FileSystem &GetFileSystem(DatabaseInstance &db);
	DUCKDB_API static FileSystem &GetLocal(DatabaseInstance &db);
	DUCKDB_API static FileSystem &Get(AttachedDatabase &db);
	//! The client's file system if there is one, so database files use the settings and logger of the session
	DUCKDB_API static FileSystem &Get(QueryContext context, AttachedDatabase &db);

	DUCKDB_API virtual unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags,
	                                                   optional_ptr<FileOpener> opener = nullptr);
	DUCKDB_API unique_ptr<FileHandle> OpenFile(const OpenFileInfo &path, const FileOpenFlags &flags,
	                                           optional_ptr<FileOpener> opener = nullptr);

	//! Open a memory-mapped view of [path]. Throws if not supported by this filesystem.
	DUCKDB_API virtual unique_ptr<MemoryMappedFile> MemoryMapFile(const OpenFileInfo &path, FileOpenFlags flags,
	                                                              const MMapOptions &options,
	                                                              optional_ptr<FileOpener> opener = nullptr);

	//! Read exactly nr_bytes from the specified location in the file. Fails if nr_bytes could not be read. This is
	//! equivalent to calling SetFilePointer(location) followed by calling Read().
	DUCKDB_API virtual void Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location);
	//! Write exactly nr_bytes to the specified location in the file. Fails if nr_bytes could not be written. This is
	//! equivalent to calling SetFilePointer(location) followed by calling Write().
	DUCKDB_API virtual void Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location);
	//! Read nr_bytes from the specified file into the buffer, moving the file pointer forward by nr_bytes. Returns the
	//! amount of bytes read.
	DUCKDB_API virtual int64_t Read(FileHandle &handle, void *buffer, int64_t nr_bytes);
	//! Write nr_bytes from the buffer into the file, moving the file pointer forward by nr_bytes.
	DUCKDB_API virtual int64_t Write(FileHandle &handle, void *buffer, int64_t nr_bytes);
	//! Excise a range of the file. The OS can drop pages from the page-cache, and the file-system is free to deallocate
	//! this range (sparse file support). Reads to the range will succeed but will return undefined data.
	DUCKDB_API virtual bool Trim(FileHandle &handle, idx_t offset_bytes, idx_t length_bytes);

	//! Returns the file size of a file handle, returns -1 on error
	DUCKDB_API virtual int64_t GetFileSize(FileHandle &handle);
	//! Returns the file last modified time of a file handle, returns timespec with zero on all attributes on error
	DUCKDB_API virtual timestamp_t GetLastModifiedTime(FileHandle &handle);
	//! Returns a tag that uniquely identifies the version of the file,
	//! used for checking cache invalidation for CachingFileSystem httpfs files
	DUCKDB_API virtual string GetVersionTag(FileHandle &handle);
	//! Returns the current cache freshness deadline, if provided by the file system
	DUCKDB_API virtual optional<timestamp_t> GetCacheValidUntil(FileHandle &handle);
	//! Returns the file type of the attached handle
	DUCKDB_API virtual FileType GetFileType(FileHandle &handle);
	//! Returns the file stats of the attached handle.
	DUCKDB_API virtual FileMetadata Stats(FileHandle &handle);
	//! Returns the stats of a file, or nullopt if the file does not exist.
	DUCKDB_API virtual optional<FileMetadata> GetStatsIfExists(const OpenFileInfo &file,
	                                                           optional_ptr<FileOpener> opener = nullptr);
	//! Truncate a file to a maximum size of new_size, new_size should be smaller than or equal to the current size of
	//! the file
	DUCKDB_API virtual void Truncate(FileHandle &handle, int64_t new_size);

	//! Check if a directory exists
	DUCKDB_API virtual bool DirectoryExists(const string &directory, optional_ptr<FileOpener> opener = nullptr);
	//! Create a directory if it does not exist
	DUCKDB_API virtual void CreateDirectory(const string &directory, optional_ptr<FileOpener> opener = nullptr);
	//! Create one directory or a directory tree. Returns whether this call is known to have created the target.
	DUCKDB_API virtual bool CreateDirectoryExtended(const string &directory, const CreateDirectoryOptions &options,
	                                                optional_ptr<FileOpener> opener = nullptr);
	//! Helper function that uses DirectoryExists and CreateDirectory to ensure all directories in path are created
	DUCKDB_API virtual void CreateDirectoriesRecursive(const string &path, optional_ptr<FileOpener> opener = nullptr);
	//! Recursively remove a directory and all files in it
	DUCKDB_API virtual void RemoveDirectory(const string &directory, optional_ptr<FileOpener> opener = nullptr);
	//! Remove an empty directory or a directory tree. Returns whether the target was removed.
	DUCKDB_API virtual bool RemoveDirectoryExtended(const string &directory, const RemoveDirectoryOptions &options,
	                                                optional_ptr<FileOpener> opener = nullptr);

	//! List files in a directory, invoking the callback method for each one with (filename, is_dir)
	DUCKDB_API virtual bool ListFiles(const string &directory,
	                                  const std::function<void(const string &, bool)> &callback,
	                                  FileOpener *opener = nullptr);
	DUCKDB_API bool ListFiles(const string &directory, const std::function<void(OpenFileInfo &info)> &callback,
	                          optional_ptr<FileOpener> opener = nullptr);

	//! Move a file from source path to the target, StorageManager relies on this being an atomic action for ACID
	//! properties
	DUCKDB_API virtual void MoveFile(const string &source, const string &target,
	                                 optional_ptr<FileOpener> opener = nullptr);
	//! Check if a file exists
	DUCKDB_API virtual bool FileExists(const string &filename, optional_ptr<FileOpener> opener = nullptr);
	//! Check if path is a pipe or character device
	DUCKDB_API virtual bool IsPipe(const string &filename, optional_ptr<FileOpener> opener = nullptr);
	//! Remove a file from disk
	DUCKDB_API virtual void RemoveFile(const string &filename, optional_ptr<FileOpener> opener = nullptr);
	//! Remove a file from disk if it exists - if it does not exist, return false
	DUCKDB_API virtual bool TryRemoveFile(const string &filename, optional_ptr<FileOpener> opener = nullptr);
	//! Remove multiple files from disk - does not error if any file does not exist
	DUCKDB_API virtual void RemoveFiles(const vector<string> &filenames, optional_ptr<FileOpener> opener = nullptr);
	//! Sync a file handle to disk
	DUCKDB_API virtual void FileSync(FileHandle &handle);
	//! Abandon an incomplete write represented by this file handle.
	DUCKDB_API virtual void AbortFileWrite(FileHandle &handle);
	//! Sets the working directory
	DUCKDB_API static void SetWorkingDirectory(const string &path);
	//! Gets the working directory
	DUCKDB_API static string GetWorkingDirectory();
	//! Gets the users home directory
	DUCKDB_API static string GetHomeDirectory(optional_ptr<FileOpener> opener);
	//! Gets the users home directory
	DUCKDB_API virtual string GetHomeDirectory();
	//! Expands a given path, including e.g. expanding the home directory of the user
	DUCKDB_API static string ExpandPath(const string &path, optional_ptr<FileOpener> opener);
	//! Expands a given path, including e.g. expanding the home directory of the user
	DUCKDB_API virtual string ExpandPath(const string &path);
	//! Returns the system-available memory in bytes. Returns DConstants::INVALID_INDEX if the system function fails.
	DUCKDB_API static optional_idx GetAvailableMemory();
	//! Returns the space available on the disk. Returns DConstants::INVALID_INDEX if the information was not available.
	DUCKDB_API static optional_idx GetAvailableDiskSpace(const string &path);
	//! Returns the soft limit on open file descriptors, or an invalid index when it is unlimited or unknown.
	DUCKDB_API static optional_idx GetFileDescriptorLimit();
	//! Returns the share of the file descriptor limit that one consumer of descriptors may hold open.
	DUCKDB_API static idx_t GetFileDescriptorShare(idx_t file_descriptor_limit);
	//! Path separator for path
	DUCKDB_API virtual string PathSeparator(const string &path);
	//! Checks if path is is an absolute path
	DUCKDB_API virtual bool IsPathAbsolute(const string &path);
	//! Join two paths together
	DUCKDB_API string JoinPath(const string &a, const string &path);
	// Join N paths together
	template <typename... ARGS>
	string JoinPath(const string &a, const string &b, ARGS... args) {
		return JoinPath(JoinPath(a, b), args...);
	}
	//! Convert separators in a path to the local separators (e.g. convert "/" into \\ on windows)
	DUCKDB_API string ConvertSeparators(const string &path);
	//! Extract the base name of a file (e.g. if the input is lib/example.dll the base name is 'example')
	DUCKDB_API string ExtractBaseName(const string &path);
	//! Extract the extension of a file (e.g. if the input is lib/example.dll the extension is 'dll')
	DUCKDB_API string ExtractExtension(const string &path);
	//! Extract the name of a file (e.g if the input is lib/example.dll the name is 'example.dll')
	DUCKDB_API string ExtractName(const string &path);

	//! Returns the value of an environment variable - or the empty string if it is not set
	DUCKDB_API static string GetEnvVariable(const string &name);

	//! Whether there is a glob in the string
	DUCKDB_API static bool HasGlob(const string &str);
	//! Runs a glob on the file system, returning a list of matching files
	DUCKDB_API virtual vector<OpenFileInfo> Glob(const string &path, FileOpener *opener = nullptr);
	DUCKDB_API unique_ptr<MultiFileList> Glob(const string &path, const FileGlobInput &input,
	                                          optional_ptr<FileOpener> opener);
	DUCKDB_API unique_ptr<MultiFileList> GlobFileList(const string &path,
	                                                  const FileGlobInput &input = FileGlobOptions::DISALLOW_EMPTY);
	DUCKDB_API vector<OpenFileInfo> GlobFiles(const string &pattern,
	                                          const FileGlobInput &input = FileGlobOptions::DISALLOW_EMPTY);

	//! registers a sub-file system to handle certain file name prefixes, e.g. http:// etc.
	DUCKDB_API virtual void RegisterSubSystem(unique_ptr<FileSystem> sub_fs);
	//! Register a compression filesystem (both built-in and provided by extensions), keyed by its compression type
	DUCKDB_API virtual void RegisterCompressionFilesystem(unique_ptr<CompressedFileSystem> fs);

	//! Unregister a sub-filesystem by name
	DUCKDB_API virtual void UnregisterSubSystem(const string &name);

	// !Extract a sub-filesystem by name, with ownership transferred, return nullptr if not registered or the subsystem
	// has been disabled.
	DUCKDB_API virtual unique_ptr<FileSystem> ExtractSubSystem(const string &name);

	//! List registered sub-filesystems, including builtin ones
	DUCKDB_API virtual vector<string> ListSubSystems();

	//! Whether or not a sub-system can handle a specific file path
	DUCKDB_API virtual bool CanHandleFile(const string &fpath);

	//! Set the file pointer of a file handle to a specified location. Reads and writes will happen from this location
	DUCKDB_API virtual void Seek(FileHandle &handle, idx_t location);
	//! Reset a file to the beginning (equivalent to Seek(handle, 0) for simple files)
	DUCKDB_API virtual void Reset(FileHandle &handle);
	DUCKDB_API virtual idx_t SeekPosition(FileHandle &handle);

	//! If FS was manually set by the user
	DUCKDB_API virtual bool IsManuallySet();
	//! Return the write ordering contract for this handle.
	DUCKDB_API virtual FileWriteMode GetWriteMode(FileHandle &handle);
	//! Whether or not we can seek into the file
	DUCKDB_API virtual bool CanSeek();
	//! Whether or not the FS handles plain files on disk. This is relevant for certain optimizations, as random reads
	//! in a file on-disk are much cheaper than e.g. random reads in a file over the network
	DUCKDB_API virtual bool OnDiskFile(FileHandle &handle);
	//! Try to obtain a measured network throughput estimate. Default: not supported (returns false).
	//! Used for file systems
	DUCKDB_API virtual bool TryGetNetworkThroughput(FileHandle &handle, NetworkThroughputEstimate &result);

	DUCKDB_API virtual unique_ptr<FileHandle> OpenCompressedFile(QueryContext context, unique_ptr<FileHandle> handle,
	                                                             bool write);

	//! Create a LocalFileSystem.
	DUCKDB_API static unique_ptr<FileSystem> CreateLocal();

	//! Whether this is a LocalFileSystem instance.
	DUCKDB_API virtual bool IsLocalFileSystem() const;

	//! Return the name of the filesystem. Used for forming diagnosis messages.
	DUCKDB_API virtual std::string GetName() const = 0;

	//! Whether or not a file is remote or local, based only on file path
	DUCKDB_API static bool IsRemoteFile(const string &path);
	DUCKDB_API static bool IsRemoteFile(const string &path, string &extension);

	DUCKDB_API virtual void SetDisabledFileSystems(const vector<string> &names);
	DUCKDB_API virtual bool SubSystemIsDisabled(const string &name);
	//! Check if the filesystem that would handle this path is disabled
	DUCKDB_API virtual bool IsDisabledForPath(const string &path);

	DUCKDB_API static bool IsDirectory(const OpenFileInfo &info);

	//! Canonicalize a path
	DUCKDB_API virtual string CanonicalizePath(const string &path, optional_ptr<FileOpener> opener = nullptr);

protected:
	DUCKDB_API virtual unique_ptr<FileHandle> OpenFileExtended(const OpenFileInfo &path, FileOpenFlags flags,
	                                                           optional_ptr<FileOpener> opener);
	DUCKDB_API virtual bool SupportsOpenFileExtended() const;

	DUCKDB_API virtual bool ListFilesExtended(const string &directory,
	                                          const std::function<void(OpenFileInfo &info)> &callback,
	                                          optional_ptr<FileOpener> opener);
	DUCKDB_API virtual bool SupportsListFilesExtended() const;

	DUCKDB_API virtual unique_ptr<MultiFileList> GlobFilesExtended(const string &path, const FileGlobInput &input,
	                                                               optional_ptr<FileOpener> opener = nullptr);
	DUCKDB_API virtual bool SupportsGlobExtended() const;

public:
	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}

	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}
};

} // namespace duckdb
