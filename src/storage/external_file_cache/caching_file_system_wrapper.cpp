#include "duckdb/storage/external_file_cache/caching_file_system_wrapper.hpp"

#include "duckdb/common/compressed_file_system.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

namespace {
// Get database instance from file opener, throw InvalidInput exception.
DatabaseInstance &GetDatabaseInstance(optional_ptr<FileOpener> file_opener) {
	if (file_opener == nullptr) {
		throw InvalidInputException("Cannot get database instance out of file opener, because file opener is nullptr.");
	}
	auto database_ptr = file_opener->TryGetDatabase();
	if (database_ptr == nullptr) {
		throw InvalidInputException("Cannot extract database instance out of file opener.");
	}
	return *database_ptr;
}
} // namespace

//===----------------------------------------------------------------------===//
// CachingFileHandleWrapper implementation
//===----------------------------------------------------------------------===//
CachingFileHandleWrapper::CachingFileHandleWrapper(shared_ptr<CachingFileSystemWrapper> file_system,
                                                   unique_ptr<CachingFileHandle> handle, const FileOpenFlags &flags)
    : FileHandle(*file_system, handle->GetPath(), flags), caching_file_system(std::move(file_system)),
      caching_handle(std::move(handle)) {
	D_ASSERT(!flags.OpenForWriting());
	D_ASSERT(!flags.OpenForAppending());
}

CachingFileHandleWrapper::~CachingFileHandleWrapper() {
}

void CachingFileHandleWrapper::Close() {
	if (caching_handle) {
		caching_handle.reset();
	}
}

//===----------------------------------------------------------------------===//
// CachingFileSystemWrapper implementation
//===----------------------------------------------------------------------===//
CachingFileSystemWrapper::CachingFileSystemWrapper(FileSystem &file_system, DatabaseInstance &db, CachingMode mode)
    : caching_file_system(file_system, db), underlying_file_system(file_system), caching_mode(mode) {
}

CachingFileSystemWrapper::CachingFileSystemWrapper(FileSystem &file_system, optional_ptr<FileOpener> file_opener,
                                                   CachingMode mode)
    : caching_file_system(file_system, GetDatabaseInstance(file_opener)), underlying_file_system(file_system),
      caching_mode(mode) {
}

bool CachingFileSystemWrapper::ShouldUseCache(const string &path) const {
	if (caching_mode == CachingMode::ALWAYS_CACHE) {
		return true;
	}
	if (caching_mode == CachingMode::NO_CACHING) {
		return false;
	}
	D_ASSERT(caching_mode == CachingMode::CACHE_REMOTE_ONLY);
	return FileSystem::IsRemoteFile(path);
}

CachingFileHandle *CachingFileSystemWrapper::GetCachingHandleIfPossible(FileHandle &handle) {
	const auto &filepath = handle.GetPath();
	if (!ShouldUseCache(filepath)) {
		return nullptr;
	}
	auto &wrapper = handle.Cast<CachingFileHandleWrapper>();
	return wrapper.caching_handle.get();
}

CachingFileSystemWrapper::~CachingFileSystemWrapper() {
}

std::string CachingFileSystemWrapper::GetName() const {
	return "CachingFileSystemWrapper";
}

//===----------------------------------------------------------------------===//
// Write Operations (Not Supported - Read-Only Filesystem)
//===----------------------------------------------------------------------===//
void CachingFileSystemWrapper::Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	throw NotImplementedException("CachingFileSystemWrapper: Write operations are not supported. "
	                              "CachingFileSystemWrapper is a read-only caching filesystem.");
}

int64_t CachingFileSystemWrapper::Write(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	throw NotImplementedException("CachingFileSystemWrapper: Write operations are not supported. "
	                              "CachingFileSystemWrapper is a read-only caching filesystem.");
}

bool CachingFileSystemWrapper::Trim(FileHandle &handle, idx_t offset_bytes, idx_t length_bytes) {
	throw NotImplementedException("CachingFileSystemWrapper: Trim operations are not supported. "
	                              "CachingFileSystemWrapper is a read-only caching filesystem.");
}

void CachingFileSystemWrapper::Truncate(FileHandle &handle, int64_t new_size) {
	throw NotImplementedException("CachingFileSystemWrapper: Truncate operations are not supported. "
	                              "CachingFileSystemWrapper is a read-only caching filesystem.");
}

void CachingFileSystemWrapper::FileSync(FileHandle &handle) {
	throw NotImplementedException("CachingFileSystemWrapper: FileSync operations are not supported. "
	                              "CachingFileSystemWrapper is a read-only caching filesystem.");
}

//===----------------------------------------------------------------------===//
// OpenFile Operations
//===----------------------------------------------------------------------===//
unique_ptr<FileHandle> CachingFileSystemWrapper::OpenFile(const string &path, FileOpenFlags flags,
                                                          optional_ptr<FileOpener> opener) {
	return OpenFile(OpenFileInfo(path), flags, opener);
}

unique_ptr<FileHandle> CachingFileSystemWrapper::OpenFile(const OpenFileInfo &path, FileOpenFlags flags,
                                                          optional_ptr<FileOpener> opener) {
	if (SupportsOpenFileExtended()) {
		return OpenFileExtended(path, std::move(flags), opener);
	}
	throw NotImplementedException("CachingFileSystemWrapper: OpenFile is not implemented!");
}

unique_ptr<FileHandle> CachingFileSystemWrapper::OpenFileExtended(const OpenFileInfo &path, FileOpenFlags flags,
                                                                  optional_ptr<FileOpener> opener) {
	if (flags.OpenForWriting()) {
		throw NotImplementedException("CachingFileSystemWrapper: Cannot open file for writing. "
		                              "CachingFileSystemWrapper is a read-only caching filesystem.");
	}
	if (!flags.OpenForReading()) {
		throw NotImplementedException("CachingFileSystemWrapper: File must be opened for reading. "
		                              "CachingFileSystemWrapper is a read-only caching filesystem.");
	}

	if (ShouldUseCache(path.path)) {
		auto caching_handle = caching_file_system.OpenFile(path, flags, opener);
		return make_uniq<CachingFileHandleWrapper>(shared_from_this(), std::move(caching_handle), flags);
	}
	// Bypass cache, use underlying file system directly.
	return underlying_file_system.OpenFile(path, flags, opener);
}

bool CachingFileSystemWrapper::SupportsOpenFileExtended() const {
	return true;
}

//===----------------------------------------------------------------------===//
// Read Operations
//===----------------------------------------------------------------------===//
void CachingFileSystemWrapper::Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	auto *caching_handle = GetCachingHandleIfPossible(handle);
	if (!caching_handle) {
		return underlying_file_system.Read(handle, buffer, nr_bytes, location);
	}

	auto group = caching_handle->Read(NumericCast<idx_t>(nr_bytes), location);
	group.CopyTo(static_cast<data_ptr_t>(buffer), NumericCast<idx_t>(nr_bytes));
}

int64_t CachingFileSystemWrapper::Read(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	const idx_t current_position = SeekPosition(handle);
	const idx_t max_read = NumericCast<idx_t>(GetFileSize(handle)) - current_position;
	nr_bytes = MinValue<int64_t>(NumericCast<int64_t>(max_read), nr_bytes);
	Read(handle, buffer, nr_bytes, current_position);
	Seek(handle, current_position + NumericCast<idx_t>(nr_bytes));
	return nr_bytes;
}

//===----------------------------------------------------------------------===//
// File Metadata Operations
//===----------------------------------------------------------------------===//
int64_t CachingFileSystemWrapper::GetFileSize(FileHandle &handle) {
	auto *caching_handle = GetCachingHandleIfPossible(handle);
	if (!caching_handle) {
		return underlying_file_system.GetFileSize(handle);
	}

	return NumericCast<int64_t>(caching_handle->GetFileSize());
}

timestamp_t CachingFileSystemWrapper::GetLastModifiedTime(FileHandle &handle) {
	auto *caching_handle = GetCachingHandleIfPossible(handle);
	if (!caching_handle) {
		return underlying_file_system.GetLastModifiedTime(handle);
	}

	return caching_handle->GetLastModifiedTime();
}

string CachingFileSystemWrapper::GetVersionTag(FileHandle &handle) {
	auto *caching_handle = GetCachingHandleIfPossible(handle);
	if (!caching_handle) {
		return underlying_file_system.GetVersionTag(handle);
	}

	return caching_handle->GetVersionTag();
}

FileType CachingFileSystemWrapper::GetFileType(FileHandle &handle) {
	auto *caching_handle = GetCachingHandleIfPossible(handle);
	if (!caching_handle) {
		return underlying_file_system.GetFileType(handle);
	}

	auto file_handle = caching_handle->GetFileHandle();
	return underlying_file_system.GetFileType(*file_handle);
}

FileMetadata CachingFileSystemWrapper::Stats(FileHandle &handle) {
	auto *caching_handle = GetCachingHandleIfPossible(handle);
	if (!caching_handle) {
		return underlying_file_system.Stats(handle);
	}

	auto file_handle = caching_handle->GetFileHandle();
	return underlying_file_system.Stats(*file_handle);
}

optional<FileMetadata> CachingFileSystemWrapper::GetStatsIfExists(const OpenFileInfo &file,
                                                                  optional_ptr<FileOpener> opener) {
	return underlying_file_system.GetStatsIfExists(file, opener);
}

//===----------------------------------------------------------------------===//
// Directory Operations
//===----------------------------------------------------------------------===//
bool CachingFileSystemWrapper::DirectoryExists(const string &directory, optional_ptr<FileOpener> opener) {
	return underlying_file_system.DirectoryExists(directory, opener);
}

void CachingFileSystemWrapper::CreateDirectory(const string &directory, optional_ptr<FileOpener> opener) {
	CreateDirectoryExtended(directory, {CreateDirectoryMode::SINGLE}, opener);
}

bool CachingFileSystemWrapper::CreateDirectoryExtended(const string &directory, const CreateDirectoryOptions &options,
                                                       optional_ptr<FileOpener> opener) {
	return underlying_file_system.CreateDirectoryExtended(directory, options, opener);
}

void CachingFileSystemWrapper::CreateDirectoriesRecursive(const string &path, optional_ptr<FileOpener> opener) {
	CreateDirectoryExtended(path, {CreateDirectoryMode::RECURSIVE}, opener);
}

void CachingFileSystemWrapper::RemoveDirectory(const string &directory, optional_ptr<FileOpener> opener) {
	RemoveDirectoryExtended(directory, {RemoveDirectoryMode::RECURSIVE}, opener);
}

bool CachingFileSystemWrapper::RemoveDirectoryExtended(const string &directory, const RemoveDirectoryOptions &options,
                                                       optional_ptr<FileOpener> opener) {
	return underlying_file_system.RemoveDirectoryExtended(directory, options, opener);
}

bool CachingFileSystemWrapper::ListFiles(const string &directory,
                                         const std::function<void(const string &, bool)> &callback,
                                         FileOpener *opener) {
	return underlying_file_system.ListFiles(directory, callback, opener);
}

bool CachingFileSystemWrapper::ListFilesExtended(const string &directory,
                                                 const std::function<void(OpenFileInfo &info)> &callback,
                                                 optional_ptr<FileOpener> opener) {
	// Use the public ListFiles API which will internally call `ListFilesExtended` if supported.
	return underlying_file_system.ListFiles(directory, callback, opener);
}

bool CachingFileSystemWrapper::SupportsListFilesExtended() const {
	// Cannot delegate to internal filesystem's invocation since it's `protected`.
	return true;
}

void CachingFileSystemWrapper::MoveFile(const string &source, const string &target, optional_ptr<FileOpener> opener) {
	underlying_file_system.MoveFile(source, target, opener);
}

bool CachingFileSystemWrapper::FileExists(const string &filename, optional_ptr<FileOpener> opener) {
	return underlying_file_system.FileExists(filename, opener);
}

bool CachingFileSystemWrapper::IsPipe(const string &filename, optional_ptr<FileOpener> opener) {
	return underlying_file_system.IsPipe(filename, opener);
}

void CachingFileSystemWrapper::RemoveFile(const string &filename, optional_ptr<FileOpener> opener) {
	underlying_file_system.RemoveFile(filename, opener);
}

bool CachingFileSystemWrapper::TryRemoveFile(const string &filename, optional_ptr<FileOpener> opener) {
	return underlying_file_system.TryRemoveFile(filename, opener);
}

//===----------------------------------------------------------------------===//
// Path Operations
//===----------------------------------------------------------------------===//
string CachingFileSystemWrapper::GetHomeDirectory() {
	return underlying_file_system.GetHomeDirectory();
}

string CachingFileSystemWrapper::ExpandPath(const string &path) {
	return underlying_file_system.ExpandPath(path);
}

string CachingFileSystemWrapper::PathSeparator(const string &path) {
	return underlying_file_system.PathSeparator(path);
}

vector<OpenFileInfo> CachingFileSystemWrapper::Glob(const string &path, FileOpener *opener) {
	return underlying_file_system.Glob(path, opener);
}

//===----------------------------------------------------------------------===//
// SubSystem Operations
//===----------------------------------------------------------------------===//
void CachingFileSystemWrapper::RegisterSubSystem(unique_ptr<FileSystem> sub_fs) {
	underlying_file_system.RegisterSubSystem(std::move(sub_fs));
}

void CachingFileSystemWrapper::RegisterCompressionFilesystem(unique_ptr<CompressedFileSystem> fs) {
	underlying_file_system.RegisterCompressionFilesystem(std::move(fs));
}

void CachingFileSystemWrapper::UnregisterSubSystem(const string &name) {
	underlying_file_system.UnregisterSubSystem(name);
}

unique_ptr<FileSystem> CachingFileSystemWrapper::ExtractSubSystem(const string &name) {
	return underlying_file_system.ExtractSubSystem(name);
}

vector<string> CachingFileSystemWrapper::ListSubSystems() {
	return underlying_file_system.ListSubSystems();
}

bool CachingFileSystemWrapper::CanHandleFile(const string &fpath) {
	return underlying_file_system.CanHandleFile(fpath);
}

//===----------------------------------------------------------------------===//
// File Handle Operations
//===----------------------------------------------------------------------===//
void CachingFileSystemWrapper::Seek(FileHandle &handle, idx_t location) {
	auto *caching_handle = GetCachingHandleIfPossible(handle);
	if (!caching_handle) {
		return underlying_file_system.Seek(handle, location);
	}

	caching_handle->Seek(location);
}

void CachingFileSystemWrapper::Reset(FileHandle &handle) {
	Seek(handle, 0);
}

idx_t CachingFileSystemWrapper::SeekPosition(FileHandle &handle) {
	auto *caching_handle = GetCachingHandleIfPossible(handle);
	if (!caching_handle) {
		return underlying_file_system.SeekPosition(handle);
	}

	return caching_handle->SeekPosition();
}

bool CachingFileSystemWrapper::IsManuallySet() {
	return underlying_file_system.IsManuallySet();
}

bool CachingFileSystemWrapper::CanSeek() {
	return underlying_file_system.CanSeek();
}

bool CachingFileSystemWrapper::OnDiskFile(FileHandle &handle) {
	auto *caching_handle = GetCachingHandleIfPossible(handle);
	if (!caching_handle) {
		return underlying_file_system.OnDiskFile(handle);
	}

	return caching_handle->OnDiskFile();
}

//===----------------------------------------------------------------------===//
// Other Operations
//===----------------------------------------------------------------------===//
unique_ptr<FileHandle> CachingFileSystemWrapper::OpenCompressedFile(QueryContext context, unique_ptr<FileHandle> handle,
                                                                    bool write) {
	return underlying_file_system.OpenCompressedFile(context, std::move(handle), write);
}

void CachingFileSystemWrapper::SetDisabledFileSystems(const vector<string> &names) {
	underlying_file_system.SetDisabledFileSystems(names);
}

bool CachingFileSystemWrapper::SubSystemIsDisabled(const string &name) {
	return underlying_file_system.SubSystemIsDisabled(name);
}

bool CachingFileSystemWrapper::IsDisabledForPath(const string &path) {
	return underlying_file_system.IsDisabledForPath(path);
}

} // namespace duckdb
