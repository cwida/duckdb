#include "duckdb/common/local_file_system.hpp"

#include "duckdb/common/checksum.hpp"
#include "duckdb/common/enums/file_sync_mode.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/memory_mapped_file.hpp"
#include "duckdb/common/process_util.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/thread.hpp"
#include "duckdb/common/windows.hpp"
#include "duckdb/function/scalar/string_common.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/logging/file_system_logger.hpp"
#include "duckdb/logging/log_manager.hpp"
#include "duckdb/common/multi_file/multi_file_list.hpp"

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <sys/stat.h>
#include <type_traits>

#ifndef _WIN32
#include <dirent.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#else
#include "duckdb/common/windows_util.hpp"

#include <io.h>
#include <string>

#ifdef __MINGW32__
// need to manually define this for mingw
extern "C" WINBASEAPI BOOL WINAPI GetPhysicallyInstalledSystemMemory(PULONGLONG);
extern "C" WINBASEAPI BOOL QueryFullProcessImageNameW(HANDLE, DWORD, LPWSTR, PDWORD);
#endif

#undef FILE_CREATE // woo mingw
#endif

#if defined(__linux__)
// See https://man7.org/linux/man-pages/man2/fallocate.2.html
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* See feature_test_macros(7) */
#endif
#include <fcntl.h>
#elif defined(_WIN32)
// for giving a better error message on lock conflicts
#include <restartmanager.h>
#endif

namespace duckdb {

#ifndef _WIN32
bool LocalFileSystem::FileExists(const string &filename, optional_ptr<FileOpener> opener) {
	if (!filename.empty()) {
		auto normalized_file = ExpandPath(filename, opener);
		struct stat status;
		if (stat(normalized_file.c_str(), &status) == 0 && S_ISREG(status.st_mode)) {
			return true;
		}
	}
	// if any condition fails
	return false;
}

bool LocalFileSystem::IsPipe(const string &filename, optional_ptr<FileOpener> opener) {
	if (!filename.empty()) {
		auto normalized_file = ExpandPath(filename, opener);
		struct stat status;
		if (stat(normalized_file.c_str(), &status) == 0 && (S_ISFIFO(status.st_mode) || S_ISCHR(status.st_mode))) {
			return true;
		}
	}
	// if any condition fails
	return false;
}

#else

// Maximum length of a path that does not require a long path prefix or
// LongPathsEnabled setting along with longPathAware manifest.
// For files the limit is 260 - 1 characters.
// Directories additionally must be able to create 8+3 files inside them.
// The limit applies to fully resolved absolute paths.
static const size_t WINDOWS_MAX_SHORT_PATH = 247;
static const size_t WINDOWS_MAX_LONG_PATH = 32000;
static const std::wstring WINDOWS_LOCAL_LONG_PATH_PREFIX = L"\\\\?\\";
static const std::wstring WINDOWS_UNC_LONG_PATH_PREFIX = L"\\\\?\\UNC\\";

static std::wstring ConvertPathToUnicode(const string &path) {
	return WindowsUtil::UTF8ToUnicode(path.c_str());
}

static std::wstring ConvertPathToNormalizedAbsolute(const std::wstring &path) {
	// len includes NULL-terminator
	DWORD len = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
	if (len == 0) {
		string error = LocalFileSystem::GetLastErrorAsString();
		string utf8_path = WindowsUtil::UnicodeToUTF8(path.c_str());
		throw IOException("Failed to get length for normalized path \"%s\": %s", utf8_path, error);
	}

	std::vector<wchar_t> buf;
	buf.resize(len);

	// written does NOT include NULL-terminator
	DWORD written = GetFullPathNameW(path.c_str(), len, buf.data(), nullptr);
	if (written == 0) {
		string error = LocalFileSystem::GetLastErrorAsString();
		string utf8_path = WindowsUtil::UnicodeToUTF8(path.c_str());
		throw IOException("Failed to normalize path \"%s\", length: %lu: %s", utf8_path, len, error);
	}

	return std::wstring(buf.data(), written);
}

static std::wstring NormalizePathAndConvertToUnicode(FileSystem &fs, const string &path,
                                                     optional_ptr<FileOpener> opener) {
	string normalized_path = fs.ExpandPath(path, opener);
	std::wstring unicode_path = ConvertPathToUnicode(normalized_path);

	// We need to get absolute path to check the length. Normalizing it (removing "." and "..",
	// flipping forward slashes) is only required if the path is long.
	// We are doing it for all paths to not perform current working dir resolving twice.
	std::wstring abs_path = ConvertPathToNormalizedAbsolute(unicode_path);

	if (abs_path.length() <= WINDOWS_MAX_SHORT_PATH || abs_path.find(WINDOWS_LOCAL_LONG_PATH_PREFIX) == 0 ||
	    abs_path.find(WINDOWS_UNC_LONG_PATH_PREFIX) == 0) {
		return abs_path;
	}

	if (abs_path.length() > WINDOWS_MAX_LONG_PATH) {
		string resolved_path = WindowsUtil::UnicodeToUTF8(abs_path.c_str());
		throw IOException("Path is longer than Windows MAX_PATH, length: %zu, beginning: %s[...]", abs_path.length(),
		                  resolved_path.substr(0, 100));
	}

	if (abs_path.find(L"\\\\") == 0) {
		// Extended UNC paths use "\\?\UNC\server\share", so remove the original leading "\\".
		// See https://learn.microsoft.com/en-us/windows/win32/fileio/naming-a-file#namespaces
		return WINDOWS_UNC_LONG_PATH_PREFIX + abs_path.substr(2);
	}

	return WINDOWS_LOCAL_LONG_PATH_PREFIX + abs_path;
}

bool LocalFileSystem::FileExists(const string &filename, optional_ptr<FileOpener> opener) {
	auto unicode_path = NormalizePathAndConvertToUnicode(*this, filename, opener);
	const wchar_t *wpath = unicode_path.c_str();
	struct _stati64 status; // typos:ignore
	if (_wstati64(wpath, &status) == 0 && (status.st_mode & S_IFREG)) {
		return true;
	}
	return false;
}
bool LocalFileSystem::IsPipe(const string &filename, optional_ptr<FileOpener> opener) {
	auto unicode_path = NormalizePathAndConvertToUnicode(*this, filename, opener);
	const wchar_t *wpath = unicode_path.c_str();
	struct _stati64 status; // typos:ignore
	if (_wstati64(wpath, &status) == 0 && (status.st_mode & _S_IFCHR)) {
		return true;
	}
	return false;
}
#endif

#ifndef _WIN32
// somehow sometimes this is missing
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

// Solaris
#ifndef O_DIRECT
#define O_DIRECT 0
#endif

static idx_t GetLocalFileSystemDelay(optional_ptr<DatabaseInstance> db) {
	if (!db) {
		return 0;
	}
	return Settings::Get<DebugLocalFileSystemDelayMsSetting>(*db);
}

static void ApplyLocalFileSystemDelay(optional_ptr<DatabaseInstance> db) {
#ifndef DUCKDB_NO_THREADS
	auto delay_ms = GetLocalFileSystemDelay(db);
	if (delay_ms > 0) {
		ThreadUtil::SleepMs(delay_ms);
	}
#endif
}

static void ApplyLocalFileSystemDelay(optional_ptr<FileOpener> opener) {
	ApplyLocalFileSystemDelay(FileOpener::TryGetDatabase(opener));
}

struct UnixFileHandle : public FileHandle {
public:
	UnixFileHandle(FileSystem &file_system, string path, int fd, FileOpenFlags flags, optional_ptr<DatabaseInstance> db)
	    : FileHandle(file_system, std::move(path), std::move(flags)), fd(fd), db(db) {
	}
	~UnixFileHandle() override {
		UnixFileHandle::Close();
	}

	int fd;
	optional_ptr<DatabaseInstance> db;

	// Kept for logging purposes
	idx_t current_pos = 0;

public:
	void Close() override {
		if (fd != -1) {
			close(fd);
			fd = -1;
			DUCKDB_LOG_FILE_SYSTEM_CLOSE((*this));
		}
	};
};

static void CloseFileAndAppendError(int fd, string &extended_error) {
	if (close(fd) == -1) {
		extended_error += ". Also, failed closing file: ";
		extended_error += strerror(errno);
	}
}

static FileMetadata StatsFromStruct(struct stat s) {
	FileMetadata file_metadata;
	file_metadata.file_size = s.st_size;
	file_metadata.last_modification_time = Timestamp::FromEpochSeconds(s.st_mtime);
	file_metadata.device_id = static_cast<idx_t>(s.st_dev);
	file_metadata.file_id = static_cast<idx_t>(s.st_ino);

	switch (s.st_mode & S_IFMT) {
	case S_IFBLK:
		file_metadata.file_type = FileType::FILE_TYPE_BLOCKDEV;
		break;
	case S_IFCHR:
		file_metadata.file_type = FileType::FILE_TYPE_CHARDEV;
		break;
	case S_IFIFO:
		file_metadata.file_type = FileType::FILE_TYPE_FIFO;
		break;
	case S_IFDIR:
		file_metadata.file_type = FileType::FILE_TYPE_DIR;
		break;
	case S_IFLNK:
		file_metadata.file_type = FileType::FILE_TYPE_LINK;
		break;
	case S_IFREG:
		file_metadata.file_type = FileType::FILE_TYPE_REGULAR;
		break;
	case S_IFSOCK:
		file_metadata.file_type = FileType::FILE_TYPE_SOCKET;
		break;
	default:
		file_metadata.file_type = FileType::FILE_TYPE_INVALID;
		break;
	}

	return file_metadata;
}

static FileMetadata StatsInternal(int fd, const string &path) {
	struct stat s;
	if (fstat(fd, &s) == -1) {
		throw IOException({{"errno", std::to_string(errno)}}, "Failed to get stats for file \"%s\": %s", path,
		                  strerror(errno));
	}
	return StatsFromStruct(s);
}

// Apply a fcntl advisory lock per flags.Lock(); throws (and closes fd) on failure. Shared
// by OpenFile and MemoryMapFile.
static void TryAcquireFileLock(FileSystem &fs, int fd, const string &path, FileOpenFlags flags) {
	if (flags.Lock() == FileLockType::NO_LOCK) {
		return;
	}
	// only attempt locking on regular files (not FIFOs/sockets)
	auto file_type = StatsInternal(fd, path).file_type;
	if (file_type == FileType::FILE_TYPE_FIFO || file_type == FileType::FILE_TYPE_SOCKET) {
		return;
	}
	struct flock fl;
	memset(&fl, 0, sizeof fl);
	fl.l_type = flags.Lock() == FileLockType::READ_LOCK ? F_RDLCK : F_WRLCK;
	fl.l_whence = SEEK_SET;
	fl.l_start = 0;
	fl.l_len = 0;
	int rc = fcntl(fd, F_SETLK, &fl);
	int retained_errno = errno;
	bool has_error = rc == -1;
	string extended_error;
	if (has_error) {
		if (retained_errno == ENOTSUP) {
			if (flags.Lock() == FileLockType::READ_LOCK) {
				// file lock not supported here; ignore for read-only
				return;
			}
			extended_error = "File locks are not supported for this file system, cannot open the file in "
			                 "read-write mode. Try opening the file in read-only mode";
		}
	}
	if (!has_error) {
		return;
	}
	if (extended_error.empty()) {
		// who is holding the lock?
		rc = fcntl(fd, F_GETLK, &fl);
		if (rc == -1) {
			extended_error = strerror(errno);
		} else if (fl.l_pid == ProcessUtil::CurrentProcessId()) {
			extended_error = "Lock is already held in current process, likely another DuckDB instance";
		} else {
			auto process = ProcessUtil::GetProcessDescription(fs, fl.l_pid);
			if (!process.empty()) {
				extended_error = "Conflicting lock is held in " + process;
			}
		}
		if (flags.Lock() == FileLockType::WRITE_LOCK) {
			// could we get a read lock?
			fl.l_type = F_RDLCK;
			rc = fcntl(fd, F_SETLK, &fl);
			if (rc != -1) {
				extended_error += ". However, you would be able to open this database in read-only mode, e.g. by "
				                  "using the -readonly parameter in the CLI";
			}
		}
	}
	CloseFileAndAppendError(fd, extended_error);
	extended_error += ". See also https://duckdb.org/docs/current/connect/concurrency";
	throw IOException({{"errno", std::to_string(retained_errno)}}, "Could not set lock on file \"%s\": %s", path,
	                  extended_error);
}

bool LocalFileSystem::IsPrivateFile(const string &path_p, FileOpener *opener) {
	auto path = FileSystem::ExpandPath(path_p, opener);

	struct stat st;

	if (lstat(path.c_str(), &st) != 0) {
		throw IOException(
		    "Failed to stat '%s' when checking file permissions, file may be missing or have incorrect permissions",
		    path_p.c_str());
	}

	// If group or other have any permission, the file is not private
	if (st.st_mode & (S_IRGRP | S_IWGRP | S_IXGRP | S_IROTH | S_IWOTH | S_IXOTH)) {
		return false;
	}

	return true;
}

unique_ptr<FileHandle> LocalFileSystem::OpenFile(const string &path_p, FileOpenFlags flags,
                                                 optional_ptr<FileOpener> opener) {
	auto path = ExpandPath(path_p, opener);
	if (flags.Compression() != FileCompressionType::UNCOMPRESSED) {
		throw NotImplementedException("Unsupported compression type for default file system");
	}

	flags.Verify();

	int open_flags = 0;
	bool open_read = flags.OpenForReading();
	bool open_write = flags.OpenForWriting();
	if (open_read && open_write) {
		open_flags = O_RDWR;
	} else if (open_read) {
		open_flags = O_RDONLY;
	} else if (open_write) {
		open_flags = O_WRONLY;
	} else {
		throw InternalException("READ, WRITE or both should be specified when opening a file");
	}
	if (open_write) {
		// need Read or Write
		D_ASSERT(flags.OpenForWriting());
		open_flags |= O_CLOEXEC;
		if (flags.CreateFileIfNotExists() || flags.OverwriteExistingFile()) {
			open_flags |= O_CREAT;
		}
		if (flags.OpenForAppending()) {
			open_flags |= O_APPEND;
		}
	}
	if (flags.DirectIO()) {
#if defined(__sun) && defined(__SVR4)
		throw InvalidInputException("DIRECT_IO not supported on Solaris");
#endif
#if defined(__DARWIN__) || defined(__APPLE__) || defined(__OpenBSD__)
		// OSX does not have O_DIRECT, instead we need to use fcntl afterwards to support direct IO
#else
		open_flags |= O_DIRECT;
#endif
	}

	// Determine permissions
	mode_t filesec;
	if (flags.CreatePrivateFile()) {
		open_flags |= O_EXCL; // Ensure we error on existing files or the permissions may not set
		filesec = 0600;
	} else {
		filesec = 0666;
	}

	if (flags.ExclusiveCreate()) {
		open_flags |= O_EXCL;
	}

	// Open the file
	ApplyLocalFileSystemDelay(opener);
	int fd = open(path.c_str(), open_flags, filesec);

	if (fd == -1) {
		if (flags.ReturnNullIfNotExists() && errno == ENOENT) {
			return nullptr;
		}
		if (flags.ReturnNullIfExists() && errno == EEXIST) {
			return nullptr;
		}
		throw IOException({{"errno", std::to_string(errno)}}, "Cannot open file \"%s\": %s", path, strerror(errno));
	}

#if defined(__DARWIN__) || defined(__APPLE__)
	if (flags.DirectIO()) {
		// OSX requires fcntl for Direct IO
		int rc = fcntl(fd, F_NOCACHE, 1);
		if (rc == -1) {
			int retained_errno = errno;
			string extended_error = strerror(retained_errno);
			CloseFileAndAppendError(fd, extended_error);
			throw IOException({{"errno", std::to_string(retained_errno)}},
			                  "Could not enable direct IO for file \"%s\": %s", path, extended_error);
		}
	}
#endif

	TryAcquireFileLock(*this, fd, path, flags);

	auto file_handle = make_uniq<UnixFileHandle>(*this, path, fd, flags, FileOpener::TryGetDatabase(opener));
	if (flags.OverwriteExistingFile() && StatsInternal(fd, path).file_type == FileType::FILE_TYPE_REGULAR) {
		Truncate(*file_handle, 0);
	}
	if (opener) {
		file_handle->TryAddLogger(*opener);
		DUCKDB_LOG_FILE_SYSTEM_OPEN((*file_handle));
	}
	return std::move(file_handle);
}

void LocalFileSystem::SetFilePointer(FileHandle &handle, idx_t location) {
	int fd = handle.Cast<UnixFileHandle>().fd;
	off_t offset = lseek(fd, UnsafeNumericCast<off_t>(location), SEEK_SET);
	if (offset == (off_t)-1) {
		throw IOException({{"errno", std::to_string(errno)}}, "Could not seek to location %lld for file \"%s\": %s",
		                  location, handle.path, strerror(errno));
	}
}

idx_t LocalFileSystem::GetFilePointer(FileHandle &handle) {
	int fd = handle.Cast<UnixFileHandle>().fd;
	off_t position = lseek(fd, 0, SEEK_CUR);
	if (position == (off_t)-1) {
		throw IOException({{"errno", std::to_string(errno)}}, "Could not get file position file \"%s\": %s",
		                  handle.path, strerror(errno));
	}
	return UnsafeNumericCast<idx_t>(position);
}

void LocalFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	auto bytes_to_read = nr_bytes;
	auto &unix_handle = handle.Cast<UnixFileHandle>();
	ApplyLocalFileSystemDelay(unix_handle.db);
	int fd = unix_handle.fd;
	auto read_buffer = char_ptr_cast(buffer);
	while (nr_bytes > 0) {
		// macOS rejects a single read above INT_MAX bytes with EINVAL and Linux silently caps it, so read in chunks
		auto chunk = MinValue<int64_t>(nr_bytes, int64_t(1) << 30);
		int64_t bytes_read =
		    pread(fd, read_buffer, UnsafeNumericCast<size_t>(chunk), UnsafeNumericCast<off_t>(location));
		if (bytes_read == -1) {
			throw IOException({{"errno", std::to_string(errno)}}, "Could not read from file \"%s\": %s", handle.path,
			                  strerror(errno));
		}
		if (bytes_read == 0) {
			throw IOException(
			    "Could not read enough bytes from file \"%s\": attempted to read %llu bytes from location %llu",
			    handle.path, nr_bytes, location);
		}
		read_buffer += bytes_read;
		nr_bytes -= bytes_read;
		location += UnsafeNumericCast<idx_t>(bytes_read);
	}

	DUCKDB_LOG_FILE_SYSTEM_READ(handle, bytes_to_read, location - UnsafeNumericCast<idx_t>(bytes_to_read));
}

int64_t LocalFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	auto &unix_handle = handle.Cast<UnixFileHandle>();
	ApplyLocalFileSystemDelay(unix_handle.db);
	int fd = unix_handle.fd;
	int64_t bytes_read = read(fd, buffer, UnsafeNumericCast<size_t>(nr_bytes));
	if (bytes_read == -1) {
		throw IOException({{"errno", std::to_string(errno)}}, "Could not read from file \"%s\": %s", handle.path,
		                  strerror(errno));
	}

	DUCKDB_LOG_FILE_SYSTEM_READ(handle, bytes_read, unix_handle.current_pos);
	unix_handle.current_pos += UnsafeNumericCast<idx_t>(bytes_read);

	return bytes_read;
}

void LocalFileSystem::Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	auto &unix_handle = handle.Cast<UnixFileHandle>();
	ApplyLocalFileSystemDelay(unix_handle.db);
	int fd = unix_handle.fd;
	auto write_buffer = char_ptr_cast(buffer);

	auto bytes_to_write = nr_bytes;
	auto current_location = location;

	while (bytes_to_write > 0) {
		int64_t bytes_written = pwrite(fd, write_buffer, UnsafeNumericCast<size_t>(bytes_to_write),
		                               UnsafeNumericCast<off_t>(current_location));
		if (bytes_written < 0 || bytes_written > bytes_to_write) {
			throw IOException({{"errno", std::to_string(errno)}}, "Could not write file \"%s\": %s", handle.path,
			                  strerror(errno));
		}
		if (bytes_written == 0) {
			throw IOException({{"errno", std::to_string(errno)}},
			                  "Could not write to file \"%s\" - attempted to write 0 bytes: %s", handle.path,
			                  strerror(errno));
		}
		write_buffer += bytes_written;
		bytes_to_write -= bytes_written;
		current_location += UnsafeNumericCast<idx_t>(bytes_written);
	}

	DUCKDB_LOG_FILE_SYSTEM_WRITE(handle, nr_bytes, location);
}

int64_t LocalFileSystem::Write(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	auto &unix_handle = handle.Cast<UnixFileHandle>();
	ApplyLocalFileSystemDelay(unix_handle.db);
	int fd = unix_handle.fd;

	auto bytes_to_write = nr_bytes;
	while (bytes_to_write > 0) {
		auto bytes_to_write_this_call =
		    MinValue<idx_t>(idx_t(NumericLimits<int32_t>::Maximum()), idx_t(bytes_to_write));
		int64_t current_bytes_written = write(fd, buffer, bytes_to_write_this_call);
		if (current_bytes_written <= 0 || idx_t(current_bytes_written) > bytes_to_write_this_call) {
			throw IOException({{"errno", std::to_string(errno)}}, "Could not write file \"%s\": %s", handle.path,
			                  strerror(errno));
		}
		buffer = (void *)(data_ptr_cast(buffer) + current_bytes_written);
		bytes_to_write -= current_bytes_written;
	}

	DUCKDB_LOG_FILE_SYSTEM_WRITE(handle, nr_bytes, unix_handle.current_pos);
	unix_handle.current_pos += UnsafeNumericCast<idx_t>(nr_bytes);

	return nr_bytes;
}

bool LocalFileSystem::Trim(FileHandle &handle, idx_t offset_bytes, idx_t length_bytes) {
	bool trimmed = false;
#if defined(DUCKDB_RUN_SLOW_VERIFIERS) || defined(DUCKDB_ALTERNATIVE_VERIFY)
	std::vector<char> zeros(length_bytes, '\0');
	Write(handle, zeros.data(), length_bytes, offset_bytes);
	trimmed = true;
#endif
#if defined(__linux__)
	// FALLOC_FL_PUNCH_HOLE requires glibc 2.18 or up
#if __GLIBC__ < 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ < 18)
	// Nothing
#else
	int fd = handle.Cast<UnixFileHandle>().fd;
	int res = fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, UnsafeNumericCast<int64_t>(offset_bytes),
	                    UnsafeNumericCast<int64_t>(length_bytes));
	trimmed = (res == 0);
#endif
#endif
	return trimmed;
}

int64_t LocalFileSystem::GetFileSize(FileHandle &handle) {
	const auto file_metadata = Stats(handle);
	return file_metadata.file_size;
}

timestamp_t LocalFileSystem::GetLastModifiedTime(FileHandle &handle) {
	const auto file_metadata = Stats(handle);
	return file_metadata.last_modification_time;
}

FileType LocalFileSystem::GetFileType(FileHandle &handle) {
	const auto file_metadata = Stats(handle);
	return file_metadata.file_type;
}

FileMetadata LocalFileSystem::Stats(FileHandle &handle) {
	int fd = handle.Cast<UnixFileHandle>().fd;
	auto file_metadata = StatsInternal(fd, handle.GetPath());
	file_metadata.version_tag = VersionTagFromMetadata(file_metadata);
	return file_metadata;
}

optional<FileMetadata> LocalFileSystem::GetStatsIfExists(const OpenFileInfo &file, optional_ptr<FileOpener> opener) {
	const auto &path_p = file.path;
	if (path_p.empty()) {
		return nullopt;
	}
	auto path = ExpandPath(path_p, opener);
	struct stat status;
	if (stat(path.c_str(), &status) != 0) {
		auto retained_errno = errno;
		if (retained_errno == ENOENT || retained_errno == ENOTDIR) {
			return nullopt;
		}
		throw IOException({{"errno", std::to_string(retained_errno)}}, "Failed to get stats for path \"%s\": %s",
		                  path_p, strerror(retained_errno));
	}
	auto file_metadata = StatsFromStruct(status);
	file_metadata.version_tag = VersionTagFromMetadata(file_metadata);
	return file_metadata;
}

void LocalFileSystem::Truncate(FileHandle &handle, int64_t new_size) {
	int fd = handle.Cast<UnixFileHandle>().fd;
	if (ftruncate(fd, new_size) != 0) {
		throw IOException({{"errno", std::to_string(errno)}}, "Could not truncate file \"%s\": %s", handle.path,
		                  strerror(errno));
	}
}

bool LocalFileSystem::DirectoryExists(const string &directory, optional_ptr<FileOpener> opener) {
	ApplyLocalFileSystemDelay(opener);

	if (!directory.empty()) {
		auto normalized_dir = ExpandPath(directory, opener);
		struct stat status;
		if (stat(normalized_dir.c_str(), &status) == 0 && S_ISDIR(status.st_mode)) {
			return true;
		}
	}
	// if any condition fails
	return false;
}

void LocalFileSystem::CreateDirectory(const string &directory, optional_ptr<FileOpener> opener) {
	CreateDirectoryExtended(directory, {CreateDirectoryMode::SINGLE}, opener);
}

void LocalFileSystem::CreateDirectoriesRecursive(const string &path, optional_ptr<FileOpener> opener) {
	CreateDirectoryExtended(path, {CreateDirectoryMode::RECURSIVE}, opener);
}

bool LocalFileSystem::CreateDirectoryExtended(const string &directory, const CreateDirectoryOptions &options,
                                              optional_ptr<FileOpener> opener) {
	if (options.mode == CreateDirectoryMode::RECURSIVE) {
		return FileSystem::CreateDirectoryExtended(directory, options, opener);
	}
	if (options.mode != CreateDirectoryMode::SINGLE) {
		throw InternalException("Unknown CreateDirectoryMode");
	}
	ApplyLocalFileSystemDelay(opener);
	auto normalized_dir = ExpandPath(directory, opener);
	if (mkdir(normalized_dir.c_str(), 0755) == 0) {
		return true;
	}
	auto error = errno;
	if (error == EEXIST) {
		struct stat st;
		if (stat(normalized_dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
			return false;
		}
	}
	throw IOException({{"errno", std::to_string(error)}}, "Failed to create directory \"%s\": %s", directory,
	                  strerror(error));
}

int RemoveDirectoryRecursive(const char *path) {
	DIR *d = opendir(path);
	idx_t path_len = (idx_t)strlen(path);
	int r = -1;

	if (d) {
		struct dirent *p;
		r = 0;
		while (!r && (p = readdir(d))) {
			int r2 = -1;
			char *buf;
			idx_t len;
			/* Skip the names "." and ".." as we don't want to recurse on them. */
			if (!strcmp(p->d_name, ".") || !strcmp(p->d_name, "..")) {
				continue;
			}
			len = path_len + (idx_t)strlen(p->d_name) + 2;
			buf = new (std::nothrow) char[len];
			if (buf) {
				struct stat statbuf;
				snprintf(buf, len, "%s/%s", path, p->d_name);
				if (!stat(buf, &statbuf)) {
					if (S_ISDIR(statbuf.st_mode)) {
						r2 = RemoveDirectoryRecursive(buf);
					} else {
						r2 = unlink(buf);
					}
				}
				delete[] buf;
			}
			r = r2;
		}
		closedir(d);
	}
	if (!r) {
		r = rmdir(path);
	}
	return r;
}

void LocalFileSystem::RemoveDirectory(const string &directory, optional_ptr<FileOpener> opener) {
	RemoveDirectoryExtended(directory, {RemoveDirectoryMode::RECURSIVE}, opener);
}

bool LocalFileSystem::RemoveDirectoryExtended(const string &directory, const RemoveDirectoryOptions &options,
                                              optional_ptr<FileOpener> opener) {
	if (options.mode == RemoveDirectoryMode::RECURSIVE) {
		auto normalized_dir = ExpandPath(directory, opener);
		return RemoveDirectoryRecursive(normalized_dir.c_str()) == 0;
	}
	if (options.mode != RemoveDirectoryMode::SINGLE) {
		throw InternalException("Unknown RemoveDirectoryMode");
	}
	auto normalized_dir = ExpandPath(directory, opener);
	if (rmdir(normalized_dir.c_str()) == 0) {
		return true;
	}
	if (errno == ENOENT || errno == ENOTEMPTY || errno == EEXIST) {
		return false;
	}
	throw IOException({{"errno", std::to_string(errno)}}, "Failed to remove empty directory \"%s\": %s", directory,
	                  strerror(errno));
}

void LocalFileSystem::RemoveFile(const string &filename, optional_ptr<FileOpener> opener) {
	auto normalized_file = ExpandPath(filename, opener);
	if (std::remove(normalized_file.c_str()) != 0) {
		throw IOException({{"errno", std::to_string(errno)}}, "Could not remove file \"%s\": %s", filename,
		                  strerror(errno));
	}
}

bool LocalFileSystem::ListFilesExtended(const string &directory,
                                        const std::function<void(OpenFileInfo &info)> &callback,
                                        optional_ptr<FileOpener> opener) {
	auto normalized_dir = ExpandPath(directory, opener);
	auto dir = opendir(normalized_dir.c_str());
	if (!dir) {
		return false;
	}

	// RAII wrapper around DIR to automatically free on exceptions in callback
	duckdb::unique_ptr<DIR, std::function<void(DIR *)>> dir_unique_ptr(dir, [](DIR *d) { closedir(d); });

	struct dirent *ent;
	// loop over all files in the directory
	while ((ent = readdir(dir)) != nullptr) {
		OpenFileInfo info(ent->d_name);
		auto &name = info.path;
		// skip . .. and empty files
		if (name.empty() || name == "." || name == "..") {
			continue;
		}
		// now stat the file to figure out if it is a regular file or directory
		string full_path = JoinPath(normalized_dir, name);
		struct stat status;
		auto res = stat(full_path.c_str(), &status);
		if (res != 0) {
			continue;
		}
		if (!S_ISREG(status.st_mode) && !S_ISDIR(status.st_mode)) {
			// not a file or directory: skip
			continue;
		}
		// create extended info
		info.extended_info = make_shared_ptr<ExtendedOpenFileInfo>();
		auto &options = info.extended_info->options;
		// file type
		Value file_type(S_ISDIR(status.st_mode) ? "directory" : "file");
		auto file_metadata = StatsFromStruct(status);
		options.emplace("type", std::move(file_type));

		FillFileOptions(file_metadata, options);

		// invoke callback
		callback(info);
	}
	return true;
}

void LocalFileSystem::FileSync(FileHandle &handle) {
	auto &unix_handle = handle.Cast<UnixFileHandle>();
	auto fsync_mode = unix_handle.db ? Settings::Get<FsyncModeSetting>(*unix_handle.db) : FileSyncMode::STANDARD;
	if (fsync_mode == FileSyncMode::NONE) {
		return;
	}
	int fd = unix_handle.fd;

#ifdef F_FULLFSYNC
	// On macOS and iOS, fsync() doesn't guarantee durability past power failures. fcntl(F_FULLFSYNC) is required for
	// that purpose. Some filesystems don't support fcntl(F_FULLFSYNC), and require a fallback to fsync().
	if (fsync_mode == FileSyncMode::FULL && ::fcntl(fd, F_FULLFSYNC) == 0) {
		return;
	}
#endif // F_FULLFSYNC

#ifdef DUCKDB_HAVE_FDATASYNC
	bool sync_success = ::fdatasync(fd) == 0;
#else
	bool sync_success = ::fsync(fd) == 0;
#endif // DUCKDB_HAVE_FDATASYNC

	if (sync_success) {
		return;
	}

	// Use fatal exception to handle fsyncgate issue: `fsync` only reports EIO for once, which makes it unretriable and
	// data loss unrecoverable.
	if (errno == EIO) {
		throw FatalException("fsync failed!");
	}

	// For other types of errors, throw normal IO exception.
	throw IOException("Could not fsync file \"%s\": %s", handle.GetPath(), strerror(errno));
}

void LocalFileSystem::MoveFile(const string &source, const string &target, optional_ptr<FileOpener> opener) {
	auto normalized_source = ExpandPath(source, opener);
	auto normalized_target = ExpandPath(target, opener);
	//! FIXME: rename does not guarantee atomicity or overwriting target file if it exists
	if (rename(normalized_source.c_str(), normalized_target.c_str()) != 0) {
		throw IOException({{"errno", to_string(errno)}}, "Could not rename file \"%s\" to \"%s\": %s", source, target,
		                  strerror(errno));
	}
}

std::string LocalFileSystem::GetLastErrorAsString() {
	return string();
}

bool LocalFileSystem::TryCanonicalizeExistingPath(string &input) {
	char resolved[PATH_MAX];
	if (realpath(input.c_str(), resolved)) {
		input = resolved;
		return true;
	}
	// the path might be a dangling symlink - follow the chain and canonicalize the parent of the final target
	static constexpr idx_t MAX_SYMLINK_DEPTH = 40;
	string current = input;
	for (idx_t depth = 0; depth < MAX_SYMLINK_DEPTH; depth++) {
		char link_target[PATH_MAX];
		auto link_length = readlink(current.c_str(), link_target, sizeof(link_target) - 1);
		if (link_length < 0) {
			return false;
		}
		string target(link_target, NumericCast<idx_t>(link_length));
		auto sep = current.rfind('/');
		if (target.empty() || sep == string::npos) {
			return false;
		}
		if (target[0] != '/') {
			target = current.substr(0, sep + 1) + target;
		}
		if (realpath(target.c_str(), resolved)) {
			input = resolved;
			return true;
		}
		auto target_sep = target.rfind('/');
		if (target_sep == string::npos) {
			return false;
		}
		struct stat target_stat;
		if (lstat(target.c_str(), &target_stat) == 0 && S_ISLNK(target_stat.st_mode)) {
			current = target;
			continue;
		}
		// final target does not exist - canonicalize its parent directory
		auto parent = target.substr(0, target_sep + 1);
		if (!realpath(parent.c_str(), resolved)) {
			return false;
		}
		string result = resolved;
		if (result.empty() || result.back() != '/') {
			result += '/';
		}
		input = result + target.substr(target_sep + 1);
		return true;
	}
	return false;
}

bool LocalFileSystem::PathStartsWithDrive(const string &path) {
	return false;
}

bool LocalFileSystem::IsPathAbsolute(const string &path) {
	return FileSystem::IsPathAbsolute(path);
}

string LocalFileSystem::MakePathAbsolute(const string &path_p, optional_ptr<FileOpener> opener) {
	auto parsed = Path::FromString(ExpandPath(path_p, opener));
	return (parsed.IsAbsolute() ? parsed : Path::FromString(GetWorkingDirectory()).Join(parsed)).ToString();
}

//===----------------------------------------------------------------------===//
// Memory-mapped files (Unix)
//===----------------------------------------------------------------------===//
class UnixMemoryMappedFile : public MemoryMappedFile {
public:
	UnixMemoryMappedFile(string path_p, FileOpenFlags flags_p, int fd_p, data_ptr_t data_p, idx_t size_p);
	~UnixMemoryMappedFile() override;

	void Sync() override;
	bool Trim(idx_t offset, idx_t length) override;
	void Close() override;

private:
	int fd;
};

UnixMemoryMappedFile::UnixMemoryMappedFile(string path_p, FileOpenFlags flags_p, int fd_p, data_ptr_t data_p,
                                           idx_t size_p)
    : MemoryMappedFile(std::move(path_p), std::move(flags_p), data_p, size_p), fd(fd_p) {
}

UnixMemoryMappedFile::~UnixMemoryMappedFile() {
	try {
		UnixMemoryMappedFile::Close();
	} catch (...) {
		// not allowed to throw in destructor
	}
}

void UnixMemoryMappedFile::Sync() {
	if (data == nullptr || size == 0) {
		return;
	}
	if (msync(data, size, MS_SYNC) != 0) {
		throw IOException({{"errno", std::to_string(errno)}}, "Could not msync file \"%s\": %s", path, strerror(errno));
	}
}

bool UnixMemoryMappedFile::Trim(idx_t offset, idx_t length) {
#if defined(__linux__) && !(defined(__GLIBC__) && (__GLIBC__ < 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ < 18)))
	int rc = fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, NumericCast<off_t>(offset),
	                   NumericCast<off_t>(length));
	return rc == 0;
#else
	(void)offset;
	(void)length;
	return false;
#endif
}

void UnixMemoryMappedFile::Close() {
	if (data != nullptr) {
		munmap(data, size);
		data = nullptr;
		size = 0;
	}
	if (fd != -1) {
		close(fd);
		fd = -1;
	}
}

unique_ptr<MemoryMappedFile> LocalFileSystem::MemoryMapFile(const OpenFileInfo &path_info, FileOpenFlags flags,
                                                            const MMapOptions &options,
                                                            optional_ptr<FileOpener> opener) {
	auto path = ExpandPath(path_info.path, opener);
	flags.Verify();

	bool open_read = flags.OpenForReading();
	bool open_write = flags.OpenForWriting();
	int open_flags;
	if (open_read && open_write) {
		open_flags = O_RDWR;
	} else if (open_read) {
		open_flags = O_RDONLY;
	} else {
		throw InternalException("READ or READ+WRITE must be specified when memory-mapping a file");
	}
	if (open_write && flags.CreateFileIfNotExists()) {
		open_flags |= O_CREAT;
	}

	int fd = open(path.c_str(), open_flags, 0666);
	if (fd == -1) {
		if (flags.ReturnNullIfNotExists() && errno == ENOENT) {
			return nullptr;
		}
		throw IOException({{"errno", std::to_string(errno)}}, "Cannot open file \"%s\" for memory mapping: %s", path,
		                  strerror(errno));
	}

	TryAcquireFileLock(*this, fd, path, flags);

	struct stat st;
	if (fstat(fd, &st) != 0) {
		int saved_errno = errno;
		close(fd);
		throw IOException({{"errno", std::to_string(saved_errno)}}, "Could not stat file \"%s\": %s", path,
		                  strerror(saved_errno));
	}
	auto initial_file_size = NumericCast<idx_t>(st.st_size);

	// Writable mappings sparsely extend the file to the reserve size so the region never
	// needs to be remapped; read-only mappings just span the existing file.
	idx_t mapping_size;
	if (open_write) {
		mapping_size = MaxValue<idx_t>(initial_file_size, options.reserve_size);
		if (initial_file_size < mapping_size) {
			if (ftruncate(fd, NumericCast<off_t>(mapping_size)) != 0) {
				int saved_errno = errno;
				close(fd);
				throw IOException({{"errno", std::to_string(saved_errno)}},
				                  "Could not ftruncate file \"%s\" to reserve size %llu: %s", path, mapping_size,
				                  strerror(saved_errno));
			}
		}
	} else {
		mapping_size = initial_file_size;
	}

	data_ptr_t data = nullptr;
	if (mapping_size > 0) {
		int prot = PROT_READ | (open_write ? PROT_WRITE : 0);
		void *mapped = mmap(nullptr, mapping_size, prot, MAP_SHARED, fd, 0);
		if (mapped == MAP_FAILED) {
			int saved_errno = errno;
			close(fd);
			throw IOException({{"errno", std::to_string(saved_errno)}}, "Could not mmap file \"%s\" (size=%llu): %s",
			                  path, mapping_size, strerror(saved_errno));
		}
		data = data_ptr_cast(mapped);
	}

	return make_uniq<UnixMemoryMappedFile>(path, flags, fd, data, mapping_size);
}

#else

constexpr char PIPE_PREFIX[] = "\\\\.\\pipe\\";

// Returns the last Win32 error, in string format. Returns an empty string if there is no error.
std::string LocalFileSystem::GetLastErrorAsString() {
	// Get the error message, if any.
	DWORD errorMessageID = GetLastError();
	if (errorMessageID == 0)
		return std::string(); // No error message has been recorded

	LPWSTR messageBuffer = nullptr;
	idx_t size = FormatMessageW(
	    FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
	    errorMessageID, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)&messageBuffer, 0, NULL);

	if (size == 0) {
		return std::string();
	}

	// Convert wide string to UTF-8
	std::wstring wideMessage(messageBuffer, size);
	std::string message = WindowsUtil::UnicodeToUTF8(wideMessage.c_str());

	// Free the buffer.
	LocalFree(messageBuffer);

	return message;
}

static timestamp_t FiletimeToTimeStamp(FILETIME file_time) {
	// https://stackoverflow.com/questions/29266743/what-is-dwlowdatetime-and-dwhighdatetime
	ULARGE_INTEGER ul;
	ul.LowPart = file_time.dwLowDateTime;
	ul.HighPart = file_time.dwHighDateTime;
	int64_t fileTime64 = ul.QuadPart;

	// fileTime64 contains a 64-bit value representing the number of
	// 100-nanosecond intervals since January 1, 1601 (UTC).
	// https://docs.microsoft.com/en-us/windows/win32/api/minwinbase/ns-minwinbase-filetime

	// Adapted from: https://stackoverflow.com/questions/6161776/convert-windows-filetime-to-second-in-unix-linux
	const auto WINDOWS_TICK = 10000000;
	const auto SEC_TO_UNIX_EPOCH = 11644473600LL;
	return Timestamp::FromEpochSeconds(fileTime64 / WINDOWS_TICK - SEC_TO_UNIX_EPOCH);
}

static FileMetadata StatsInternal(HANDLE hFile, const string &path) {
	FileMetadata file_metadata;

	DWORD handle_type = GetFileType(hFile);
	if (handle_type == FILE_TYPE_CHAR) {
		file_metadata.file_type = FileType::FILE_TYPE_CHARDEV;
		file_metadata.file_size = 0;
		file_metadata.last_modification_time = Timestamp::FromEpochSeconds(0);
		return file_metadata;
	}
	if (handle_type == FILE_TYPE_PIPE) {
		file_metadata.file_type = FileType::FILE_TYPE_FIFO;
		file_metadata.file_size = 0;
		file_metadata.last_modification_time = Timestamp::FromEpochSeconds(0);
		return file_metadata;
	}

	BY_HANDLE_FILE_INFORMATION file_info;
	if (!GetFileInformationByHandle(hFile, &file_info)) {
		auto error = LocalFileSystem::GetLastErrorAsString();
		throw IOException("Failed to get stats for file \"%s\": %s", path, error);
	}

	// Get file size from high and low parts.
	file_metadata.file_size =
	    (static_cast<int64_t>(file_info.nFileSizeHigh) << 32) | static_cast<int64_t>(file_info.nFileSizeLow);
	file_metadata.device_id = static_cast<uint64_t>(file_info.dwVolumeSerialNumber);
	file_metadata.file_id =
	    (static_cast<uint64_t>(file_info.nFileIndexHigh) << 32) | static_cast<uint64_t>(file_info.nFileIndexLow);

	// Get last modification time
	file_metadata.last_modification_time = FiletimeToTimeStamp(file_info.ftLastWriteTime);

	// Get file type from attributes
	if (strncmp(path.c_str(), PIPE_PREFIX, strlen(PIPE_PREFIX)) == 0) {
		// pipes in windows are just files in '\\.\pipe\' folder
		file_metadata.file_type = FileType::FILE_TYPE_FIFO;
	} else if (file_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
		file_metadata.file_type = FileType::FILE_TYPE_DIR;
	} else if (file_info.dwFileAttributes & FILE_ATTRIBUTE_DEVICE) {
		file_metadata.file_type = FileType::FILE_TYPE_CHARDEV;
	} else if (file_info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
		file_metadata.file_type = FileType::FILE_TYPE_LINK;
	} else if (file_info.dwFileAttributes != INVALID_FILE_ATTRIBUTES) {
		file_metadata.file_type = FileType::FILE_TYPE_REGULAR;
	} else {
		file_metadata.file_type = FileType::FILE_TYPE_INVALID;
	}

	return file_metadata;
}

static FileMetadata StatsFromDirInfo(const FILE_ID_BOTH_DIR_INFO &entry) {
	FileMetadata result;
	result.file_size = static_cast<idx_t>(entry.EndOfFile.QuadPart);

	FILETIME ft;
	ft.dwLowDateTime = entry.LastWriteTime.LowPart;
	ft.dwHighDateTime = entry.LastWriteTime.HighPart;
	result.last_modification_time = FiletimeToTimeStamp(ft);

	result.file_id = entry.FileId.QuadPart;
	return result;
}

struct WindowsFileHandle : public FileHandle {
public:
	WindowsFileHandle(FileSystem &file_system, string path, HANDLE fd, FileOpenFlags flags,
	                  optional_ptr<DatabaseInstance> db)
	    : FileHandle(file_system, path, flags), position(0), fd(fd), db(db) {
	}
	~WindowsFileHandle() override {
		Close();
	}

	idx_t position;
	HANDLE fd;
	optional_ptr<DatabaseInstance> db;

public:
	void Close() override {
		if (!fd) {
			return;
		}
		CloseHandle(fd);
		fd = nullptr;
		DUCKDB_LOG_FILE_SYSTEM_CLOSE((*this));
	};
};

static string AdditionalLockInfo(const std::wstring path) {
	// try to find out if another process is holding the lock

	// init of the somewhat obscure "Windows Restart Manager"
	// see also https://devblogs.microsoft.com/oldnewthing/20120217-00/?p=8283

	DWORD session, status, reason;
	WCHAR session_key[CCH_RM_SESSION_KEY + 1] = {0};

	status = RmStartSession(&session, 0, session_key);
	if (status != ERROR_SUCCESS) {
		return string();
	}

	PCWSTR path_ptr = path.c_str();
	status = RmRegisterResources(session, 1, &path_ptr, 0, NULL, 0, NULL);
	if (status != ERROR_SUCCESS) {
		return string();
	}
	UINT process_info_size_needed, process_info_size;

	// we first call with nProcInfo = 0 to find out how much to allocate
	process_info_size = 0;
	status = RmGetList(session, &process_info_size_needed, &process_info_size, NULL, &reason);
	if (status != ERROR_MORE_DATA || process_info_size_needed == 0) {
		return string();
	}

	// allocate
	auto process_info_buffer = duckdb::unique_ptr<RM_PROCESS_INFO[]>(new RM_PROCESS_INFO[process_info_size_needed]);
	auto process_info = process_info_buffer.get();

	// now call again to get actual data
	process_info_size = process_info_size_needed;
	status = RmGetList(session, &process_info_size_needed, &process_info_size, process_info, &reason);
	if (status != ERROR_SUCCESS || process_info_size == 0) {
		return "";
	}

	string conflict_string;
	for (UINT process_idx = 0; process_idx < process_info_size; process_idx++) {
		string process_name = WindowsUtil::UnicodeToUTF8(process_info[process_idx].strAppName);
		auto pid = process_info[process_idx].Process.dwProcessId;

		// find out full path if possible
		HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		if (process) {
			WCHAR full_path[MAX_PATH];
			DWORD full_path_size = MAX_PATH;
			if (QueryFullProcessImageNameW(process, 0, full_path, &full_path_size) && full_path_size <= MAX_PATH) {
				process_name = WindowsUtil::UnicodeToUTF8(full_path);
			}
			CloseHandle(process);
		}
		conflict_string += StringUtil::Format("\n%s (PID %d)", process_name, pid);
	}

	RmEndSession(session);
	if (conflict_string.empty()) {
		return string();
	}
	return "File is already open in " + conflict_string;
}

bool LocalFileSystem::IsPrivateFile(const string &path_p, FileOpener *opener) {
	// TODO: detect if file is shared in windows
	return true;
}

unique_ptr<FileHandle> LocalFileSystem::OpenFile(const string &path_p, FileOpenFlags flags,
                                                 optional_ptr<FileOpener> opener) {
	auto path = FileSystem::ExpandPath(path_p, opener);
	auto unicode_path = NormalizePathAndConvertToUnicode(*this, path, opener);
	if (flags.Compression() != FileCompressionType::UNCOMPRESSED) {
		throw NotImplementedException("Unsupported compression type for default file system");
	}
	flags.Verify();

	DWORD desired_access;
	DWORD share_mode;
	DWORD creation_disposition = OPEN_EXISTING;
	DWORD flags_and_attributes = FILE_ATTRIBUTE_NORMAL;
	bool open_read = flags.OpenForReading();
	bool open_write = flags.OpenForWriting();
	if (open_read && open_write) {
		desired_access = GENERIC_READ | GENERIC_WRITE;
	} else if (open_read) {
		desired_access = GENERIC_READ;
	} else if (open_write) {
		desired_access = GENERIC_WRITE;
	} else {
		throw InternalException("READ, WRITE or both should be specified when opening a file");
	}
	switch (flags.Lock()) {
	case FileLockType::NO_LOCK:
		share_mode = FILE_SHARE_READ | FILE_SHARE_WRITE;
		break;
	case FileLockType::READ_LOCK:
		share_mode = FILE_SHARE_READ;
		break;
	case FileLockType::WRITE_LOCK:
		share_mode = 0;
		break;
	default:
		throw InternalException("Unknown FileLockType");
	}
	// For windows platform, by default deletion fails when the file is accessed by other thread/process.
	// To keep deletion behavior compatible with unix platform, which physically deletes a file when reference count
	// drops to 0 without interfering with already opened file handles, open files with [`FILE_SHARE_DELETE`].
	share_mode |= FILE_SHARE_DELETE;

	if (open_write) {
		if (flags.ExclusiveCreate()) {
			creation_disposition = CREATE_NEW;
		} else if (flags.CreateFileIfNotExists()) {
			creation_disposition = OPEN_ALWAYS;
		} else if (flags.OverwriteExistingFile()) {
			creation_disposition = CREATE_ALWAYS;
		}
	}
	if (flags.DirectIO()) {
		flags_and_attributes |= FILE_FLAG_NO_BUFFERING;
	}
	HANDLE hFile = CreateFileW(unicode_path.c_str(), desired_access, share_mode, NULL, creation_disposition,
	                           flags_and_attributes, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		auto last_error = GetLastError();
		if (flags.ReturnNullIfNotExists() && last_error == ERROR_FILE_NOT_FOUND) {
			return nullptr;
		}
		// Match POSIX EEXIST handling for exclusive creation.
		if (flags.ReturnNullIfExists() && (last_error == ERROR_FILE_EXISTS || last_error == ERROR_ALREADY_EXISTS)) {
			return nullptr;
		}
		auto error = LocalFileSystem::GetLastErrorAsString();

		auto extended_error = AdditionalLockInfo(unicode_path);
		if (!extended_error.empty()) {
			extended_error = "\n" + extended_error;
		}
		auto abs_path = WindowsUtil::UnicodeToUTF8(unicode_path.c_str());
		throw IOException("Cannot open file \"%s\": %s%s", abs_path, error, extended_error);
	}
	auto handle = make_uniq<WindowsFileHandle>(*this, path.c_str(), hFile, flags, FileOpener::TryGetDatabase(opener));
	if (flags.OpenForAppending()) {
		auto file_size = GetFileSize(*handle);
		SetFilePointer(*handle, file_size);
	}
	if (opener) {
		handle->TryAddLogger(*opener);
		DUCKDB_LOG_FILE_SYSTEM_OPEN((*handle));
	}
	return std::move(handle);
}

void LocalFileSystem::SetFilePointer(FileHandle &handle, idx_t location) {
	auto &whandle = handle.Cast<WindowsFileHandle>();
	whandle.position = location;
	LARGE_INTEGER wlocation;
	wlocation.QuadPart = location;
	SetFilePointerEx(whandle.fd, wlocation, NULL, FILE_BEGIN);
}

idx_t LocalFileSystem::GetFilePointer(FileHandle &handle) {
	return handle.Cast<WindowsFileHandle>().position;
}

static DWORD FSInternalRead(FileHandle &handle, HANDLE hFile, void *buffer, int64_t nr_bytes, idx_t location) {
	DWORD bytes_read = 0;
	OVERLAPPED ov = {};
	ov.Internal = 0;
	ov.InternalHigh = 0;
	ov.Offset = location & 0xFFFFFFFF;
	ov.OffsetHigh = location >> 32;
	ov.hEvent = 0;
	auto rc = ReadFile(hFile, buffer, (DWORD)nr_bytes, &bytes_read, &ov);
	if (!rc) {
		auto error = LocalFileSystem::GetLastErrorAsString();
		throw IOException("Could not read file \"%s\" (error in ReadFile(location: %llu, nr_bytes: %lld)): %s",
		                  handle.path, location, nr_bytes, error);
	}
	return bytes_read;
}

void LocalFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	HANDLE hFile = ((WindowsFileHandle &)handle).fd;
	auto bytes_read = FSInternalRead(handle, hFile, buffer, nr_bytes, location);
	if (bytes_read != nr_bytes) {
		throw IOException("Could not read all bytes from file \"%s\": wanted=%lld read=%lld", handle.path, nr_bytes,
		                  bytes_read);
	}
	DUCKDB_LOG_FILE_SYSTEM_READ(handle, bytes_read, location);
}

int64_t LocalFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	HANDLE hFile = handle.Cast<WindowsFileHandle>().fd;
	auto &pos = handle.Cast<WindowsFileHandle>().position;
	auto n = std::min<idx_t>(std::max<idx_t>(GetFileSize(handle), pos) - pos, nr_bytes);
	auto bytes_read = FSInternalRead(handle, hFile, buffer, n, pos);
	pos += bytes_read;

	DUCKDB_LOG_FILE_SYSTEM_READ(handle, bytes_read, pos - bytes_read);
	return bytes_read;
}

static DWORD FSInternalWrite(FileHandle &handle, HANDLE hFile, void *buffer, int64_t nr_bytes, idx_t location) {
	DWORD bytes_written = 0;
	OVERLAPPED ov = {};
	ov.Internal = 0;
	ov.InternalHigh = 0;
	ov.Offset = location & 0xFFFFFFFF;
	ov.OffsetHigh = location >> 32;
	ov.hEvent = 0;
	auto rc = WriteFile(hFile, buffer, (DWORD)nr_bytes, &bytes_written, &ov);
	if (!rc) {
		auto error = LocalFileSystem::GetLastErrorAsString();
		throw IOException("Could not write file \"%s\" (error in WriteFile): %s", handle.path, error);
	}
	return bytes_written;
}

static int64_t FSWrite(FileHandle &handle, HANDLE hFile, void *buffer, int64_t nr_bytes, idx_t location) {
	int64_t bytes_written = 0;
	while (nr_bytes > 0) {
		auto bytes_to_write = MinValue<idx_t>(idx_t(NumericLimits<int32_t>::Maximum()), idx_t(nr_bytes));
		DWORD current_bytes_written = FSInternalWrite(handle, hFile, buffer, bytes_to_write, location);
		if (current_bytes_written <= 0 || current_bytes_written > bytes_to_write) {
			throw IOException({{"errno", std::to_string(errno)}}, "Could not write file \"%s\": %s", handle.path,
			                  strerror(errno));
		}
		bytes_written += current_bytes_written;
		buffer = (void *)(data_ptr_cast(buffer) + current_bytes_written);
		location += current_bytes_written;
		nr_bytes -= current_bytes_written;
	}
	return bytes_written;
}

void LocalFileSystem::Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	HANDLE hFile = handle.Cast<WindowsFileHandle>().fd;
	auto bytes_written = FSWrite(handle, hFile, buffer, nr_bytes, location);
	if (bytes_written != nr_bytes) {
		throw IOException("Could not write all bytes from file \"%s\": wanted=%lld wrote=%lld", handle.path, nr_bytes,
		                  bytes_written);
	}
	DUCKDB_LOG_FILE_SYSTEM_WRITE(handle, bytes_written, location);
}

int64_t LocalFileSystem::Write(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	HANDLE hFile = handle.Cast<WindowsFileHandle>().fd;
	auto &pos = handle.Cast<WindowsFileHandle>().position;
	auto bytes_written = FSWrite(handle, hFile, buffer, nr_bytes, pos);
	pos += bytes_written;
	DUCKDB_LOG_FILE_SYSTEM_WRITE(handle, bytes_written, pos - bytes_written);
	return bytes_written;
}

bool LocalFileSystem::Trim(FileHandle &handle, idx_t offset_bytes, idx_t length_bytes) {
	// TODO: Not yet implemented on windows.
	return false;
}

int64_t LocalFileSystem::GetFileSize(FileHandle &handle) {
	const auto file_metadata = Stats(handle);
	return file_metadata.file_size;
}

timestamp_t LocalFileSystem::GetLastModifiedTime(FileHandle &handle) {
	const auto file_metadata = Stats(handle);
	return file_metadata.last_modification_time;
}

void LocalFileSystem::Truncate(FileHandle &handle, int64_t new_size) {
	HANDLE hFile = handle.Cast<WindowsFileHandle>().fd;
	// seek to the location
	SetFilePointer(handle, new_size);
	// now set the end of file position
	if (!SetEndOfFile(hFile)) {
		auto error = LocalFileSystem::GetLastErrorAsString();
		throw IOException("Failure in SetEndOfFile call on file \"%s\": %s", handle.path, error);
	}
}

static DWORD WindowsGetFileAttributes(LocalFileSystem &fs, const string &filename, optional_ptr<FileOpener> opener) {
	auto unicode_path = NormalizePathAndConvertToUnicode(fs, filename, opener);
	return GetFileAttributesW(unicode_path.c_str());
}

static DWORD WindowsGetFileAttributes(const std::wstring &filename) {
	return GetFileAttributesW(filename.c_str());
}

bool LocalFileSystem::DirectoryExists(const string &directory, optional_ptr<FileOpener> opener) {
	DWORD attrs = WindowsGetFileAttributes(*this, directory, opener);
	return (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY));
}

void LocalFileSystem::CreateDirectory(const string &directory, optional_ptr<FileOpener> opener) {
	CreateDirectoryExtended(directory, {CreateDirectoryMode::SINGLE}, opener);
}

void LocalFileSystem::CreateDirectoriesRecursive(const string &path, optional_ptr<FileOpener> opener) {
	CreateDirectoryExtended(path, {CreateDirectoryMode::RECURSIVE}, opener);
}

bool LocalFileSystem::CreateDirectoryExtended(const string &directory, const CreateDirectoryOptions &options,
                                              optional_ptr<FileOpener> opener) {
	if (options.mode == CreateDirectoryMode::RECURSIVE) {
		return FileSystem::CreateDirectoryExtended(directory, options, opener);
	}
	if (options.mode != CreateDirectoryMode::SINGLE) {
		throw InternalException("Unknown CreateDirectoryMode");
	}
	auto unicode_path = NormalizePathAndConvertToUnicode(*this, directory, opener);
	if (!directory.empty() && CreateDirectoryW(unicode_path.c_str(), NULL)) {
		return true;
	}
	auto error_code = GetLastError();
	if (error_code == ERROR_ALREADY_EXISTS) {
		auto attributes = GetFileAttributesW(unicode_path.c_str());
		if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
			return false;
		}
	}
	SetLastError(error_code);
	auto error = LocalFileSystem::GetLastErrorAsString();
	auto abs_path = WindowsUtil::UnicodeToUTF8(unicode_path.c_str());
	throw IOException("Failed to create directory \"%s\": %s", abs_path, error);
}

static void DeleteDirectoryRecursive(FileSystem &fs, string directory, optional_ptr<FileOpener> opener) {
	fs.ListFiles(
	    directory,
	    [&](const string &fname, bool is_directory) {
		    if (is_directory) {
			    DeleteDirectoryRecursive(fs, fs.JoinPath(directory, fname), opener);
		    } else {
			    fs.RemoveFile(fs.JoinPath(directory, fname), opener);
		    }
	    },
	    opener.get());
	auto unicode_path = NormalizePathAndConvertToUnicode(fs, directory, opener);
	if (!RemoveDirectoryW(unicode_path.c_str())) {
		auto error = LocalFileSystem::GetLastErrorAsString();
		auto abs_path = WindowsUtil::UnicodeToUTF8(unicode_path.c_str());
		throw IOException("Failed to delete directory \"%s\": %s", abs_path, error);
	}
}

void LocalFileSystem::RemoveDirectory(const string &directory, optional_ptr<FileOpener> opener) {
	RemoveDirectoryExtended(directory, {RemoveDirectoryMode::RECURSIVE}, opener);
}

bool LocalFileSystem::RemoveDirectoryExtended(const string &directory, const RemoveDirectoryOptions &options,
                                              optional_ptr<FileOpener> opener) {
	if (options.mode == RemoveDirectoryMode::RECURSIVE) {
		if (FileExists(directory, opener)) {
			throw IOException("Attempting to delete directory \"%s\", but it is a file and not a directory!",
			                  directory);
		}
		if (!DirectoryExists(directory, opener)) {
			return false;
		}
		DeleteDirectoryRecursive(*this, directory, opener);
		return true;
	}
	if (options.mode != RemoveDirectoryMode::SINGLE) {
		throw InternalException("Unknown RemoveDirectoryMode");
	}
	auto unicode_path = NormalizePathAndConvertToUnicode(*this, directory, opener);
	if (RemoveDirectoryW(unicode_path.c_str())) {
		return true;
	}
	auto error = GetLastError();
	if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND || error == ERROR_DIR_NOT_EMPTY) {
		return false;
	}
	auto abs_path = WindowsUtil::UnicodeToUTF8(unicode_path.c_str());
	throw IOException("Failed to remove empty directory \"%s\": %s", abs_path, GetLastErrorAsString());
}

void LocalFileSystem::RemoveFile(const string &filename, optional_ptr<FileOpener> opener) {
	auto unicode_path = NormalizePathAndConvertToUnicode(*this, filename, opener);
	if (!DeleteFileW(unicode_path.c_str())) {
		auto error = LocalFileSystem::GetLastErrorAsString();
		auto abs_path = WindowsUtil::UnicodeToUTF8(unicode_path.c_str());
		throw IOException("Failed to delete file \"%s\": %s", abs_path, error);
	}
}

bool LocalFileSystem::ListFilesExtended(const string &directory,
                                        const std::function<void(OpenFileInfo &info)> &callback,
                                        optional_ptr<FileOpener> opener) {
	auto unicode_path = NormalizePathAndConvertToUnicode(*this, directory, opener);

	// Open a handle to the directory itself so we can use GetFileInformationByHandleEx,
	// which returns FILE_ID_BOTH_DIR_INFO entries that include the per-file FileId
	// (equivalent to inode number) without requiring a separate handle per file.
	HANDLE hDir =
	    CreateFileW(unicode_path.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
	                NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if (hDir == INVALID_HANDLE_VALUE) {
		return false;
	}

	DWORD volume_serial_number = 0;
	if (!GetVolumeInformationByHandleW(hDir, NULL, 0, &volume_serial_number, NULL, NULL, NULL, 0)) {
		CloseHandle(hDir);
		throw IOException("Failed to get volume serial number from directory \"%s\": %s", directory,
		                  GetLastErrorAsString());
	}

	static constexpr DWORD BUFFER_SIZE = 65536;
	auto buffer = unique_ptr<char[]>(new char[BUFFER_SIZE]);

	FILE_INFO_BY_HANDLE_CLASS info_class = FileIdBothDirectoryRestartInfo;
	while (true) {
		if (!GetFileInformationByHandleEx(hDir, info_class, buffer.get(), BUFFER_SIZE)) {
			CloseHandle(hDir);
			if (GetLastError() == ERROR_NO_MORE_FILES) {
				// success - listed all files in the directory
				return true;
			}
			auto error = LocalFileSystem::GetLastErrorAsString();
			throw IOException("Failed to list directory \"%s\": %s", directory, error);
		}

		auto *entry = reinterpret_cast<FILE_ID_BOTH_DIR_INFO *>(buffer.get());
		while (true) {
			// FileName is not null-terminated; use FileNameLength (in bytes) to construct the string.
			std::wstring wname(entry->FileName, entry->FileNameLength / sizeof(WCHAR));
			string name = WindowsUtil::UnicodeToUTF8(wname.c_str());

			if (name != "." && name != "..") {
				OpenFileInfo info(name);
				info.extended_info = make_shared_ptr<ExtendedOpenFileInfo>();
				auto &options = info.extended_info->options;
				// file type
				Value file_type(entry->FileAttributes & FILE_ATTRIBUTE_DIRECTORY ? "directory" : "file");
				options.emplace("type", std::move(file_type));

				auto metadata = StatsFromDirInfo(*entry);
				metadata.device_id = volume_serial_number;
				FillFileOptions(metadata, options);

				callback(info);
			}

			if (entry->NextEntryOffset == 0) {
				break;
			}
			entry = reinterpret_cast<FILE_ID_BOTH_DIR_INFO *>(reinterpret_cast<char *>(entry) + entry->NextEntryOffset);
		}
		// next iteration -
		info_class = FileIdBothDirectoryInfo;
	}
}

void LocalFileSystem::FileSync(FileHandle &handle) {
	auto &windows_handle = handle.Cast<WindowsFileHandle>();
	auto fsync_mode = windows_handle.db ? Settings::Get<FsyncModeSetting>(*windows_handle.db) : FileSyncMode::STANDARD;
	if (fsync_mode == FileSyncMode::NONE) {
		return;
	}
	HANDLE hFile = windows_handle.fd;
	if (FlushFileBuffers(hFile) == 0) {
		throw IOException("Could not flush file handle to disk!");
	}
}

static bool TryMoveFileWithPosixSemantics(HANDLE source_handle, const std::wstring &target) {
	constexpr auto file_rename_info_ex = static_cast<FILE_INFO_BY_HANDLE_CLASS>(22); // FileRenameInfoEx
	const auto file_name_length = target.size() * sizeof(WCHAR);
	const auto rename_info_size = offsetof(FILE_RENAME_INFO, FileName) + file_name_length + sizeof(WCHAR);
	const auto rename_info_size_dw = NumericCast<DWORD>(rename_info_size);
	auto rename_info_buffer = make_uniq_array<data_t>(rename_info_size);
	auto rename_info = reinterpret_cast<FILE_RENAME_INFO *>(rename_info_buffer.get());
	rename_info->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
	rename_info->RootDirectory = nullptr;
	rename_info->FileNameLength = NumericCast<DWORD>(file_name_length);
	std::copy(target.c_str(), target.c_str() + target.size() + 1, rename_info->FileName);

	return SetFileInformationByHandle(source_handle, file_rename_info_ex, rename_info, rename_info_size_dw);
}

void LocalFileSystem::MoveFile(const string &source, const string &target, optional_ptr<FileOpener> opener) {
	auto source_unicode = NormalizePathAndConvertToUnicode(*this, source, opener);
	auto target_unicode = NormalizePathAndConvertToUnicode(*this, target, opener);

	// FileRenameInfoEx renames the file identified by a handle opened with DELETE access.
	// See https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_rename_info
	constexpr DWORD delete_access = 0x00010000L; // DELETE
	auto raw_source_handle =
	    CreateFileW(source_unicode.c_str(), delete_access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
	                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (raw_source_handle == INVALID_HANDLE_VALUE) {
		auto error = GetLastErrorAsString();
		throw IOException("Could not move file \"%s\" to \"%s\": failed to open source file: %s", source, target,
		                  error);
	}
	unique_ptr<void, decltype(&CloseHandle)> source_handle(raw_source_handle, CloseHandle);

	if (TryMoveFileWithPosixSemantics(source_handle.get(), target_unicode)) {
		return;
	}
	auto error_code = GetLastError();
	source_handle.reset();

	if (error_code != ERROR_INVALID_PARAMETER && error_code != ERROR_NOT_SUPPORTED &&
	    error_code != ERROR_INVALID_FUNCTION) {
		SetLastError(error_code);
		throw IOException("Could not move file \"%s\" to \"%s\": %s", source, target, GetLastErrorAsString());
	}

	DWORD flags = MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH;
	if (!MoveFileExW(source_unicode.c_str(), target_unicode.c_str(), flags)) {
		throw IOException("Could not move file \"%s\" to \"%s\": %s", source, target, GetLastErrorAsString());
	}
}

FileType LocalFileSystem::GetFileType(FileHandle &handle) {
	const auto file_metadata = Stats(handle);
	return file_metadata.file_type;
}

FileMetadata LocalFileSystem::Stats(FileHandle &handle) {
	HANDLE hFile = handle.Cast<WindowsFileHandle>().fd;
	auto file_metadata = StatsInternal(hFile, handle.GetPath());
	file_metadata.version_tag = VersionTagFromMetadata(file_metadata);
	return file_metadata;
}

optional<FileMetadata> LocalFileSystem::GetStatsIfExists(const OpenFileInfo &file, optional_ptr<FileOpener> opener) {
	const auto &path_p = file.path;
	if (path_p.empty()) {
		return nullopt;
	}
	auto unicode_path = NormalizePathAndConvertToUnicode(*this, path_p, opener);
	auto raw_handle = CreateFileW(unicode_path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	                              OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if (raw_handle == INVALID_HANDLE_VALUE) {
		auto error_code = GetLastError();
		if (error_code == ERROR_FILE_NOT_FOUND || error_code == ERROR_PATH_NOT_FOUND) {
			return nullopt;
		}
		SetLastError(error_code);
		throw IOException("Failed to get stats for path \"%s\": %s", path_p, GetLastErrorAsString());
	}
	unique_ptr<void, decltype(&CloseHandle)> handle(raw_handle, CloseHandle);

	auto file_metadata = StatsInternal(handle.get(), path_p);
	file_metadata.version_tag = VersionTagFromMetadata(file_metadata);
	return file_metadata;
}

bool LocalFileSystem::TryCanonicalizeExistingPath(string &input) {
	auto unicode_path = ConvertPathToUnicode(input);
	HANDLE handle = CreateFileW(unicode_path.c_str(),
	                            0, // No access needed, just query
	                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
	                            FILE_FLAG_BACKUP_SEMANTICS, // Required for directories
	                            NULL);

	if (handle == INVALID_HANDLE_VALUE) {
		return false;
	}
	wchar_t resolved[MAX_PATH];
	DWORD len = GetFinalPathNameByHandleW(handle, resolved, MAX_PATH, FILE_NAME_NORMALIZED);
	CloseHandle(handle);

	if (len == 0 || len >= MAX_PATH) {
		return false;
	}

	std::wstring resolved_wstr(resolved, static_cast<size_t>(len));

	if (resolved_wstr.find(WINDOWS_UNC_LONG_PATH_PREFIX) == 0) {
		resolved_wstr = L"\\\\" + resolved_wstr.substr(WINDOWS_UNC_LONG_PATH_PREFIX.length());
	} else if (resolved_wstr.find(WINDOWS_LOCAL_LONG_PATH_PREFIX) == 0) {
		resolved_wstr = resolved_wstr.substr(WINDOWS_LOCAL_LONG_PATH_PREFIX.length());
	}

	input = WindowsUtil::UnicodeToUTF8(resolved_wstr.c_str());
	return true;
}

bool LocalFileSystem::PathStartsWithDrive(const string &path) {
	return path.size() >= 2 && path[0] >= 'A' && path[0] <= 'Z' && path[1] == ':';
}

bool LocalFileSystem::IsPathAbsolute(const string &path) {
	if (FileSystem::IsPathAbsolute(path)) {
		return true;
	}
	// check if this is a drive letter (e.g. C:)
	if (PathStartsWithDrive(path)) {
		return true;
	}
	return false;
}

string LocalFileSystem::MakePathAbsolute(const string &path_p, optional_ptr<FileOpener> opener) {
	auto path = ExpandPath(path_p, opener);
	auto parsed = Path::FromString(path);
	if (parsed.IsAbsolute()) {
		// already absolute - nothing to do
		return parsed.ToString();
	}

	auto parsed_wd = Path::FromString(GetWorkingDirectory());
	if (parsed.HasDrive() && parsed_wd.GetDriveChar() != parsed.GetDriveChar()) {
		// this is relative path "C:", and this is not the drive we are on right now
		// (referencing D: while in $PWD in C:); In this case, default as if PWD is
		// root of drive, e.g. "C:\"
		// NOTE: should this error? pushdir("c:") && getcwd() && popdir()?
		parsed_wd = Path::FromString(parsed.GetBase() + parsed.GetSeparator());
	}
	return parsed_wd.Join(parsed.GetPath()).ToString();
}

//===----------------------------------------------------------------------===//
// Memory-mapped files (Windows)
//===----------------------------------------------------------------------===//
class WindowsMemoryMappedFile : public MemoryMappedFile {
public:
	WindowsMemoryMappedFile(string path_p, FileOpenFlags flags_p, HANDLE file_handle_p, HANDLE mapping_handle_p,
	                        data_ptr_t data_p, idx_t size_p);
	~WindowsMemoryMappedFile() override;

	void Sync() override;
	bool Trim(idx_t offset, idx_t length) override;
	void Close() override;

private:
	HANDLE file_handle;
	HANDLE mapping_handle;
};

WindowsMemoryMappedFile::WindowsMemoryMappedFile(string path_p, FileOpenFlags flags_p, HANDLE file_handle_p,
                                                 HANDLE mapping_handle_p, data_ptr_t data_p, idx_t size_p)
    : MemoryMappedFile(std::move(path_p), flags_p, data_p, size_p), file_handle(file_handle_p),
      mapping_handle(mapping_handle_p) {
}

WindowsMemoryMappedFile::~WindowsMemoryMappedFile() {
	try {
		WindowsMemoryMappedFile::Close();
	} catch (...) {
		// not allowed to throw in destructor
	}
}

void WindowsMemoryMappedFile::Sync() {
	if (data == nullptr || size == 0) {
		return;
	}
	if (!FlushViewOfFile(data, size)) {
		throw IOException("Could not FlushViewOfFile for \"%s\": %s", path, LocalFileSystem::GetLastErrorAsString());
	}
	if (!FlushFileBuffers(file_handle)) {
		throw IOException("Could not FlushFileBuffers for \"%s\": %s", path, LocalFileSystem::GetLastErrorAsString());
	}
}

bool WindowsMemoryMappedFile::Trim(idx_t offset, idx_t length) {
	// FSCTL_SET_ZERO_DATA on NTFS sparse files would work; not implemented yet.
	(void)offset;
	(void)length;
	return false;
}

void WindowsMemoryMappedFile::Close() {
	if (data != nullptr) {
		UnmapViewOfFile(data);
		data = nullptr;
		size = 0;
	}
	if (mapping_handle != nullptr) {
		CloseHandle(mapping_handle);
		mapping_handle = nullptr;
	}
	if (file_handle != nullptr && file_handle != INVALID_HANDLE_VALUE) {
		CloseHandle(file_handle);
		file_handle = nullptr;
	}
}

unique_ptr<MemoryMappedFile> LocalFileSystem::MemoryMapFile(const OpenFileInfo &path_info, FileOpenFlags flags,
                                                            const MMapOptions &options,
                                                            optional_ptr<FileOpener> opener) {
	auto path = ExpandPath(path_info.path, opener);
	flags.Verify();

	bool open_read = flags.OpenForReading();
	bool open_write = flags.OpenForWriting();
	DWORD desired_access;
	DWORD share_mode;
	DWORD creation_disposition = OPEN_EXISTING;

	if (open_read && open_write) {
		desired_access = GENERIC_READ | GENERIC_WRITE;
	} else if (open_read) {
		desired_access = GENERIC_READ;
	} else {
		throw InternalException("READ or READ+WRITE must be specified when memory-mapping a file");
	}
	// share_mode mirrors OpenFile's handling: write lock excludes, read lock blocks writers.
	switch (flags.Lock()) {
	case FileLockType::NO_LOCK:
		share_mode = FILE_SHARE_READ | FILE_SHARE_WRITE;
		break;
	case FileLockType::READ_LOCK:
		share_mode = FILE_SHARE_READ;
		break;
	case FileLockType::WRITE_LOCK:
		share_mode = 0;
		break;
	default:
		throw InternalException("Unknown FileLockType");
	}
	share_mode |= FILE_SHARE_DELETE;
	if (open_write && flags.CreateFileIfNotExists()) {
		creation_disposition = OPEN_ALWAYS;
	}

	auto unicode_path = WindowsUtil::UTF8ToUnicode(path.c_str());
	HANDLE file_handle = CreateFileW(unicode_path.c_str(), desired_access, share_mode, NULL, creation_disposition,
	                                 FILE_ATTRIBUTE_NORMAL, NULL);
	if (file_handle == INVALID_HANDLE_VALUE) {
		if (flags.ReturnNullIfNotExists() && GetLastError() == ERROR_FILE_NOT_FOUND) {
			return nullptr;
		}
		throw IOException("Cannot open file \"%s\" for memory mapping: %s", path,
		                  LocalFileSystem::GetLastErrorAsString());
	}

	LARGE_INTEGER size_li;
	if (!GetFileSizeEx(file_handle, &size_li)) {
		auto err = LocalFileSystem::GetLastErrorAsString();
		CloseHandle(file_handle);
		throw IOException("Could not GetFileSizeEx for \"%s\": %s", path, err);
	}
	auto initial_file_size = NumericCast<idx_t>(size_li.QuadPart);

	// Writable mappings sparsely extend the file to the reserve size so the region never
	// needs to be remapped; read-only mappings just span the existing file.
	idx_t mapping_size;
	if (open_write) {
		mapping_size = MaxValue<idx_t>(initial_file_size, options.reserve_size);
		if (initial_file_size < mapping_size) {
			// Sparse so the SetEndOfFile below doesn't allocate the untouched range.
			DWORD bytes_returned = 0;
			DeviceIoControl(file_handle, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &bytes_returned, NULL);
			LARGE_INTEGER li;
			li.QuadPart = static_cast<LONGLONG>(mapping_size);
			if (!SetFilePointerEx(file_handle, li, NULL, FILE_BEGIN)) {
				auto err = LocalFileSystem::GetLastErrorAsString();
				CloseHandle(file_handle);
				throw IOException("Could not SetFilePointerEx for \"%s\": %s", path, err);
			}
			if (!SetEndOfFile(file_handle)) {
				auto err = LocalFileSystem::GetLastErrorAsString();
				CloseHandle(file_handle);
				throw IOException("Could not SetEndOfFile for \"%s\" (reserve_size=%llu): %s", path, mapping_size, err);
			}
		}
	} else {
		mapping_size = initial_file_size;
	}

	HANDLE mapping_handle = nullptr;
	data_ptr_t data = nullptr;
	if (mapping_size > 0) {
		DWORD protect = open_write ? PAGE_READWRITE : PAGE_READONLY;
		DWORD desired_map_access = open_write ? FILE_MAP_WRITE : FILE_MAP_READ;
		DWORD high = static_cast<DWORD>((static_cast<uint64_t>(mapping_size) >> 32) & 0xFFFFFFFFULL);
		DWORD low = static_cast<DWORD>(static_cast<uint64_t>(mapping_size) & 0xFFFFFFFFULL);
		mapping_handle = CreateFileMappingW(file_handle, NULL, protect, high, low, NULL);
		if (mapping_handle == NULL) {
			auto err = LocalFileSystem::GetLastErrorAsString();
			CloseHandle(file_handle);
			throw IOException("Could not CreateFileMapping for \"%s\": %s", path, err);
		}
		void *mapped = MapViewOfFile(mapping_handle, desired_map_access, 0, 0, mapping_size);
		if (mapped == NULL) {
			auto err = LocalFileSystem::GetLastErrorAsString();
			CloseHandle(mapping_handle);
			CloseHandle(file_handle);
			throw IOException("Could not MapViewOfFile for \"%s\": %s", path, err);
		}
		data = data_ptr_cast(mapped);
	}

	return make_uniq<WindowsMemoryMappedFile>(path, flags, file_handle, mapping_handle, data, mapping_size);
}

#endif

void LocalFileSystem::AbortFileWrite(FileHandle &handle) {
	auto remove_path = handle.GetFlags().ExclusiveCreate();
	auto path = handle.GetPath();
	handle.Close();
	if (remove_path) {
		TryRemoveFile(path);
	}
}

bool LocalFileSystem::CanSeek() {
	return true;
}

FileWriteMode LocalFileSystem::GetWriteMode(FileHandle &handle) {
	auto type = GetFileType(handle);
	if (type == FileType::FILE_TYPE_FIFO || type == FileType::FILE_TYPE_CHARDEV) {
		return FileWriteMode::SEQUENTIAL;
	}
	return FileWriteMode::POSITIONAL;
}

bool LocalFileSystem::OnDiskFile(FileHandle &handle) {
	return true;
}

string LocalFileSystem::CanonicalizePath(const string &input, optional_ptr<FileOpener> opener) {
	auto path_sep = PathSeparator(input);
	if (path_sep.size() != 1) {
		throw InternalException("path separator can only be a single byte for local file systems");
	}
	// make the path absolute
	string path = MakePathAbsolute(input, opener);

	string current = path;
	string remainder;
	idx_t dot_dot_count = 0;
	bool is_drive = false;
	while (!current.empty()) {
		if (dot_dot_count == 0 && TryCanonicalizeExistingPath(current)) {
			// successfully canonicalized "current" - add remainder if we have any
			if (remainder.empty()) {
				return current;
			}
			if (StringUtil::EndsWith(current, path_sep)) {
				return current + remainder;
			}
			return current + path_sep + remainder;
		}
		if (is_drive) {
			// this is a drive only (e.g. C:\)
			// if we reach this, this is an unknown drive letter
			// use fallback canonicalize for the remainder
			return current + FileSystem::CanonicalizePath(remainder);
		}
		// move up one directory
		optional_idx sep_idx;
		for (idx_t i = current.size(); i > 0; i--) {
			// on windows we accept both separators (\ and /, and also :)
			if (current[i - 1] == path_sep[0] || current[i - 1] == '/') {
				sep_idx = i - 1;
				break;
			}
		}
		if (!sep_idx.IsValid()) {
			// exhausted the full path and nothing exists - break out
			current = string();
			break;
		}
		auto sep = sep_idx.GetIndex();
		auto component = current.substr(sep + 1);
		if (component == "..") {
			// dot dot - we need to move up a level
			// increment the count
			dot_dot_count++;
		} else if (!component.empty() && component != ".") {
			if (dot_dot_count > 0) {
				// just clear this directory
				dot_dot_count--;
			} else {
				// add component to remainder - unless it's dot or empty
				if (remainder.empty()) {
					remainder = component;
				} else {
					remainder = component + path_sep + remainder;
				}
			}
		}
		// continue with remainder
		current = current.substr(0, sep);
		if (current.size() == 2 && PathStartsWithDrive(current)) {
			// Windows only
			// C: and C:\\ mean different things (C: is relative, C:\\ is absolute)
			// we should have already normalized to C:\\ earlier on in this function
			// so turn this base drive letter into C:\\ for the final lookup
			is_drive = true;
			current += "\\";
		}
	}
	// failed to canonicalize path - fallback to generic canonicalization
	return FileSystem::CanonicalizePath(path);
}

string LocalFileSystem::VersionTagFromMetadata(const FileMetadata &stats) {
	uint64_t version_tag[4];
	Store(stats.device_id.IsValid() ? stats.device_id.GetIndex() : 0, data_ptr_cast(&version_tag[0]));
	Store(stats.file_id.IsValid() ? stats.file_id.GetIndex() : 0, data_ptr_cast(&version_tag[1]));
	Store(stats.file_size, data_ptr_cast(&version_tag[2]));
	Store(stats.last_modification_time.value, data_ptr_cast(&version_tag[3]));
	return string(char_ptr_cast(version_tag), sizeof(uint64_t) * 4);
}

void LocalFileSystem::FillFileOptions(const FileMetadata &file_metadata, unordered_map<string, Value> &options) {
	// file size
	options.emplace("file_size", Value::BIGINT(file_metadata.file_size));
	// last modified time
	options.emplace("last_modified", Value::TIMESTAMP(file_metadata.last_modification_time));
	// version tag
	options.emplace("etag", Value::BLOB_RAW(VersionTagFromMetadata(file_metadata)));
}

string LocalFileSystem::GetVersionTag(FileHandle &handle) {
	return handle.Stats().version_tag;
}

void LocalFileSystem::Seek(FileHandle &handle, idx_t location) {
	if (!CanSeek()) {
		throw IOException("Cannot seek in files of this type");
	}
	SetFilePointer(handle, location);
}

idx_t LocalFileSystem::SeekPosition(FileHandle &handle) {
	if (!CanSeek()) {
		throw IOException("Cannot seek in files of this type");
	}
	return GetFilePointer(handle);
}

static bool IsCrawl(const string &glob) {
	// glob must match exactly
	return glob == "**";
}

static bool IsSymbolicLink(const string &path) {
#ifndef _WIN32
	struct stat status;
	return (lstat(path.c_str(), &status) != -1 && S_ISLNK(status.st_mode));
#else
	auto unicode_path = ConvertPathToUnicode(path);
	auto attributes = WindowsGetFileAttributes(unicode_path);
	if (attributes == INVALID_FILE_ATTRIBUTES) {
		return false;
	}
	return attributes & FILE_ATTRIBUTE_REPARSE_POINT;
#endif
}

vector<OpenFileInfo> LocalFileSystem::FetchFileWithoutGlob(const string &path, optional_ptr<FileOpener> opener,
                                                           bool absolute_path) {
	vector<OpenFileInfo> result;
	if (FileExists(path, opener) || IsPipe(path, opener)) {
		result.emplace_back(path);
	} else if (!absolute_path) {
		Value value;
		if (opener && opener->TryGetCurrentSetting("file_search_path", value)) {
			auto search_paths_str = value.ToString();
			vector<std::string> search_paths = StringUtil::Split(search_paths_str, ',');
			for (const auto &search_path : search_paths) {
				auto joined_path = JoinPath(search_path, path);
				if (FileExists(joined_path, opener) || IsPipe(joined_path, opener)) {
					result.emplace_back(joined_path);
				}
			}
		}
	}
	return result;
}

struct PathSplit {
	PathSplit(LocalFileSystem &fs, string path_p) : path(std::move(path_p)), has_glob(fs.HasGlob(path)) {
	}

	string path;
	bool has_glob;
};

static bool HasMultipleCrawl(const vector<PathSplit> &splits) {
	idx_t crawl_count = 0;
	for (auto &split : splits) {
		if (split.path == "**") {
			crawl_count++;
		}
	}
	return crawl_count > 1;
}

struct ExpandDirectory {
	ExpandDirectory(string path_p, idx_t split_index, bool is_empty = false)
	    : path(std::move(path_p)), split_index(split_index), is_empty(is_empty) {
	}

	string path;
	idx_t split_index;
	bool is_empty = false;

	bool operator<(const ExpandDirectory &other) const {
		return path > other.path;
	}
};

static void CrawlDirectoryLevel(FileSystem &fs, const string &path, optional_ptr<vector<OpenFileInfo>> files,
                                std::priority_queue<ExpandDirectory> &directories, idx_t split_index) {
	fs.ListFiles(path, [&](OpenFileInfo &info) {
		info.path = fs.JoinPath(path, info.path);
		if (IsSymbolicLink(info.path)) {
			return;
		}
		bool is_directory = FileSystem::IsDirectory(info);
		if (is_directory) {
			directories.emplace(std::move(info.path), split_index);
		} else if (files) {
			files->push_back(std::move(info));
		}
	});
}

static void GlobFilesInternal(FileSystem &fs, const string &path, const string &glob, bool match_directory,
                              vector<OpenFileInfo> &result) {
	fs.ListFiles(path, [&](OpenFileInfo &info) {
		bool is_directory = FileSystem::IsDirectory(info);
		if (is_directory != match_directory) {
			return;
		}
		if (Glob(info.path.c_str(), info.path.size(), glob.c_str(), glob.size())) {
			info.path = fs.JoinPath(path, info.path);
			result.push_back(std::move(info));
		}
	});
}

struct LocalGlobResult : public LazyMultiFileList {
public:
	LocalGlobResult(LocalFileSystem &fs, const string &path, FileGlobOptions options, optional_ptr<FileOpener> opener);

protected:
	bool ExpandNextPath() const override;

private:
	LocalFileSystem &fs;
	string path;
	optional_ptr<FileOpener> opener;
	vector<PathSplit> splits;
	bool absolute_path = false;
	mutable std::priority_queue<ExpandDirectory> expand_directories;
	mutable bool finished = false;
};

LocalGlobResult::LocalGlobResult(LocalFileSystem &fs, const string &path_p, FileGlobOptions options_p,
                                 optional_ptr<FileOpener> opener)
    : LazyMultiFileList(FileOpener::TryGetClientContext(opener)), fs(fs), path(fs.ExpandPath(path_p, opener)),
      opener(opener) {
	if (path.empty()) {
		finished = true;
		return;
	}
	// Split path into base (= scheme+authority+anchor) + individual segments.
	auto parsed = Path::FromString(path);
	string base = parsed.GetBase();
	if (!base.empty()) {
		splits.emplace_back(fs, base);
	}
	for (auto &seg : parsed.GetPathSegments()) {
		splits.emplace_back(fs, seg);
	}
	absolute_path = parsed.IsAbsolute();
	// Preserve trailing separator semantics: glob("dir/*/") matches directories,
	// not files. Add empty trailing split to enforce this and make preceding splits
	// non-final.
	if (parsed.HasTrailingSeparator()) {
		splits.emplace_back(fs, string());
	}
	if (!splits.empty() && splits[0].path == "~") {
		// starts with home directory
		auto home_directory = fs.GetHomeDirectory(opener);
		if (!home_directory.empty()) {
			absolute_path = true;
			splits[0].path = home_directory;
			D_ASSERT(path[0] == '~');
			if (!fs.HasGlob(path)) {
				expanded_files = fs.FetchFileWithoutGlob(home_directory + path.substr(1), opener, absolute_path);
				finished = true;
				return;
			}
		}
	}
	// Check if the path has a glob at all
	if (!fs.HasGlob(path)) {
		// no glob: return only the file (if it exists or is a pipe)
		expanded_files = fs.FetchFileWithoutGlob(path, opener, absolute_path);
		finished = true;
		return;
	}
	if (absolute_path) {
		// for absolute paths, we don't start by scanning the current directory
		// FIXME: we don't support /[GLOB]/.. - i.e. globs in the first level of an absolute path
		if (splits.size() > 1) {
			expand_directories.emplace(splits[0].path, 1);
		}
	} else {
		// If file_search_path is set, use those paths as the first glob elements
		Value value;
		if (opener && opener->TryGetCurrentSetting("file_search_path", value)) {
			auto search_paths_str = value.ToString();
			auto search_paths = StringUtil::Split(search_paths_str, ',');
			for (const auto &search_path : search_paths) {
				expand_directories.emplace(search_path, 0);
			}
		}
		if (expand_directories.empty()) {
			expand_directories.emplace(".", 0, true);
		}
	}

	if (HasMultipleCrawl(splits)) {
		throw IOException("Cannot use multiple \'**\' in one path");
	}
}

bool LocalGlobResult::ExpandNextPath() const {
	if (finished) {
		return false;
	}
	if (expand_directories.empty()) {
		if (expanded_files.empty()) {
			// no result found that matches the glob
			// last ditch effort: search the path as a string literal
			expanded_files = fs.FetchFileWithoutGlob(path, opener, absolute_path);
		}
		finished = true;
		return false;
	}

	auto next_dir = expand_directories.top();
	auto is_empty = next_dir.is_empty;
	auto split_index = next_dir.split_index;
	auto &current_path = next_dir.path;
	expand_directories.pop();

	auto &next_split = splits[split_index];
	bool is_last_component = split_index + 1 == splits.size();
	auto &next_component = next_split.path;
	bool has_glob = next_split.has_glob;
	// if it's the last chunk we need to find files, otherwise we find directories
	// not the last chunk: gather a list of all directories that match the glob pattern
	if (!has_glob) {
		// no glob, just append as-is
		if (is_empty) {
			if (is_last_component) {
				throw InternalException("No glob in only component - but entire split has globs?");
			}
			// no path yet - just append
			expand_directories.emplace(next_component, split_index + 1);
		} else {
			if (is_last_component) {
				// last component - we are emitting a result here
				auto filename = fs.JoinPath(current_path, next_component);
				if (fs.FileExists(filename, opener) || fs.DirectoryExists(filename, opener)) {
					expanded_files.emplace_back(std::move(filename));
				}
			} else {
				// not the last component - add the next directory as "to-be-expanded"
				expand_directories.emplace(fs.JoinPath(current_path, next_component), split_index + 1);
			}
		}
	} else {
		// glob - need to resolve the glob
		if (IsCrawl(next_component)) {
			if (is_last_component) {
				// the crawl is the last component - we are looking for files in this directory
				// any directories we encounter are added to the expand directories
				CrawlDirectoryLevel(fs, current_path, expanded_files, expand_directories, split_index);
			} else {
				// not the last crawl
				// ** also matches the current directory (i.e. dir/**/file.parquet also matches dir/file.parquet)
				expand_directories.emplace(current_path, split_index + 1);
				// now crawl the contents of this directory - but don't add any files we find
				CrawlDirectoryLevel(fs, current_path, nullptr, expand_directories, split_index);
			}
		} else {
			// glob this directory according to the next component
			if (is_last_component) {
				// last component - match files and place them in the result
				GlobFilesInternal(fs, current_path, next_component, false, expanded_files);
			} else {
				// not the last component - match directories and add to expansion list
				vector<OpenFileInfo> child_directories;
				GlobFilesInternal(fs, current_path, next_component, true, child_directories);
				for (auto &file : child_directories) {
					expand_directories.emplace(std::move(file.path), split_index + 1);
				}
			}
		}
	}
	return true;
}

unique_ptr<MultiFileList> LocalFileSystem::GlobFilesExtended(const string &path, const FileGlobInput &input,
                                                             optional_ptr<FileOpener> opener) {
	return make_uniq<LocalGlobResult>(*this, path, FileGlobOptions::ALLOW_EMPTY, opener);
}

unique_ptr<FileSystem> FileSystem::CreateLocal() {
	return make_uniq<LocalFileSystem>();
}

} // namespace duckdb
