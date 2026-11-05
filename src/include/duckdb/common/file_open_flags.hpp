//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/file_open_flags.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/file_compression_type.hpp"
#include "duckdb/storage/caching_mode.hpp"

namespace duckdb {

enum class FileLockType : uint8_t { NO_LOCK = 0, READ_LOCK = 1, WRITE_LOCK = 2 };

class FileOpenFlags {
public:
	static constexpr idx_t FILE_FLAGS_READ = idx_t(1 << 0);
	static constexpr idx_t FILE_FLAGS_WRITE = idx_t(1 << 1);
	static constexpr idx_t FILE_FLAGS_DIRECT_IO = idx_t(1 << 2);
	static constexpr idx_t FILE_FLAGS_FILE_CREATE = idx_t(1 << 3);
	static constexpr idx_t FILE_FLAGS_FILE_CREATE_NEW = idx_t(1 << 4);
	static constexpr idx_t FILE_FLAGS_APPEND = idx_t(1 << 5);
	static constexpr idx_t FILE_FLAGS_PRIVATE = idx_t(1 << 6);
	static constexpr idx_t FILE_FLAGS_NULL_IF_NOT_EXISTS = idx_t(1 << 7);
	static constexpr idx_t FILE_FLAGS_PARALLEL_ACCESS = idx_t(1 << 8);
	static constexpr idx_t FILE_FLAGS_EXCLUSIVE_CREATE = idx_t(1 << 9);
	static constexpr idx_t FILE_FLAGS_NULL_IF_EXISTS = idx_t(1 << 10);
	static constexpr idx_t FILE_FLAGS_MULTI_CLIENT_ACCESS = idx_t(1 << 11);
	static constexpr idx_t FILE_FLAGS_DISABLE_LOGGING = idx_t(1 << 12);
	static constexpr idx_t FILE_FLAGS_ENABLE_EXTENSION_INSTALL = idx_t(1 << 13);

public:
	FileOpenFlags() = default;
	FileOpenFlags(idx_t flags) : flags(flags) { // NOLINT: allow implicit conversion
	}
	FileOpenFlags(FileLockType lock) : lock(lock) { // NOLINT: allow implicit conversion
	}
	FileOpenFlags(FileCompressionType compression) // NOLINT: allow implicit conversion
	    : compression(std::move(compression)) {
	}
	FileOpenFlags(idx_t flags, FileLockType lock, FileCompressionType compression)
	    : flags(flags), lock(lock), compression(std::move(compression)) {
	}

	static constexpr FileLockType MergeLock(FileLockType a, FileLockType b) {
		return a == FileLockType::NO_LOCK ? b : a;
	}

	//! An uncompressed flag acts as the identity - any other compression flag wins the merge
	static FileCompressionType MergeCompression(const FileCompressionType &a, const FileCompressionType &b) {
		return a.IsUncompressed() ? b : a;
	}

	static constexpr CachingMode MergeCachingMode(CachingMode a, CachingMode b) {
		return a == CachingMode::NO_CACHING ? b : a;
	}

	static constexpr RequestSizing MergeRequestSizing(RequestSizing a, RequestSizing b) {
		return a == RequestSizing::BY_CACHE ? b : a;
	}

	inline FileOpenFlags operator|(const FileOpenFlags &b) const {
		FileOpenFlags result(flags | b.flags, MergeLock(lock, b.lock), MergeCompression(compression, b.compression));
		result.request_sizing = MergeRequestSizing(request_sizing, b.request_sizing);
		return result;
	}
	inline FileOpenFlags &operator|=(const FileOpenFlags &b) {
		flags |= b.flags;
		lock = MergeLock(lock, b.lock);
		compression = MergeCompression(compression, b.compression);
		caching_mode = MergeCachingMode(caching_mode, b.caching_mode);
		request_sizing = MergeRequestSizing(request_sizing, b.request_sizing);
		return *this;
	}

	FileLockType Lock() {
		return lock;
	}

	const FileCompressionType &Compression() const {
		return compression;
	}

	void SetCompression(FileCompressionType new_compression) {
		compression = std::move(new_compression);
	}

	CachingMode GetCachingMode() {
		return caching_mode;
	}
	void SetCachingMode(CachingMode new_caching_mode) {
		caching_mode = new_caching_mode;
	}

	RequestSizing GetRequestSizing() const {
		return request_sizing;
	}
	void SetRequestSizing(RequestSizing new_request_sizing) {
		request_sizing = new_request_sizing;
	}

	void Verify();

	inline bool OpenForReading() const {
		return flags & FILE_FLAGS_READ;
	}
	inline bool OpenForWriting() const {
		return flags & FILE_FLAGS_WRITE;
	}
	inline bool DirectIO() const {
		return flags & FILE_FLAGS_DIRECT_IO;
	}
	inline bool CreateFileIfNotExists() const {
		return flags & FILE_FLAGS_FILE_CREATE;
	}
	inline bool OverwriteExistingFile() const {
		return flags & FILE_FLAGS_FILE_CREATE_NEW;
	}
	inline bool OpenForAppending() const {
		return flags & FILE_FLAGS_APPEND;
	}
	inline bool CreatePrivateFile() const {
		return flags & FILE_FLAGS_PRIVATE;
	}
	inline bool ReturnNullIfNotExists() const {
		return flags & FILE_FLAGS_NULL_IF_NOT_EXISTS;
	}
	inline bool RequireParallelAccess() const {
		return flags & FILE_FLAGS_PARALLEL_ACCESS;
	}
	inline bool ExclusiveCreate() const {
		return flags & FILE_FLAGS_EXCLUSIVE_CREATE;
	}
	inline bool ReturnNullIfExists() const {
		return flags & FILE_FLAGS_NULL_IF_EXISTS;
	}
	inline bool MultiClientAccess() const {
		return flags & FILE_FLAGS_MULTI_CLIENT_ACCESS;
	}
	inline bool DisableLogging() const {
		return flags & FILE_FLAGS_DISABLE_LOGGING;
	}
	inline bool EnableExtensionInstall() const {
		return flags & FILE_FLAGS_ENABLE_EXTENSION_INSTALL;
	}
	inline idx_t GetFlagsInternal() const {
		return flags;
	}

private:
	idx_t flags = 0;
	FileLockType lock = FileLockType::NO_LOCK;
	CachingMode caching_mode = CachingMode::NO_CACHING;
	RequestSizing request_sizing = RequestSizing::BY_CACHE;
	//! Default-constructed FileCompressionType is UNCOMPRESSED
	FileCompressionType compression;
};

class FileFlags {
public:
	//! Open file with read access
	DUCKDB_API static const FileOpenFlags FILE_FLAGS_READ;
	//! Open file with write access
	DUCKDB_API static const FileOpenFlags FILE_FLAGS_WRITE;
	//! Use direct IO when reading/writing to the file
	DUCKDB_API static const FileOpenFlags FILE_FLAGS_DIRECT_IO;
	//! Create file if not exists, can only be used together with WRITE
	DUCKDB_API static const FileOpenFlags FILE_FLAGS_FILE_CREATE;
	//! Always create a new file. If a file exists, the file is truncated. Cannot be used together with CREATE.
	DUCKDB_API static const FileOpenFlags FILE_FLAGS_FILE_CREATE_NEW;
	//! Open file in append mode
	DUCKDB_API static const FileOpenFlags FILE_FLAGS_APPEND;
	//! Open file with restrictive permissions (600 on linux/mac) can only be used when creating, throws if file exists
	DUCKDB_API static const FileOpenFlags FILE_FLAGS_PRIVATE;
	//! Return NULL if the file does not exist instead of throwing an error
	DUCKDB_API static const FileOpenFlags FILE_FLAGS_NULL_IF_NOT_EXISTS;
	//! Multiple threads may perform reads and writes in parallel
	DUCKDB_API static const FileOpenFlags FILE_FLAGS_PARALLEL_ACCESS;
	//! Ensure that this call creates the file, throw is file exists
	DUCKDB_API static const FileOpenFlags FILE_FLAGS_EXCLUSIVE_CREATE;
	//!  Return NULL if the file exist instead of throwing an error
	DUCKDB_API static const FileOpenFlags FILE_FLAGS_NULL_IF_EXISTS;
	//! Multiple clients may access the file at the same time
	DUCKDB_API static const FileOpenFlags FILE_FLAGS_MULTI_CLIENT_ACCESS;
	//! Disables logging to avoid infinite loops when using FileHandle-backed log storage
	DUCKDB_API static const FileOpenFlags FILE_FLAGS_DISABLE_LOGGING;
	//! Opened file is allowed to be a duckdb_extension
	DUCKDB_API static const FileOpenFlags FILE_FLAGS_ENABLE_EXTENSION_INSTALL;
};

} // namespace duckdb
