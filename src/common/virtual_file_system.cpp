#include "duckdb/common/virtual_file_system.hpp"

#include "duckdb/common/compressed_file_system.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/memory_mapped_file.hpp"
#include "duckdb/common/gzip_file_system.hpp"
#include "duckdb/common/pipe_file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/storage/external_file_cache/caching_file_system_wrapper.hpp"
#include "duckdb/common/multi_file/multi_file_list.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension_helper.hpp"

namespace duckdb {

struct FileSystemHandle {
	explicit FileSystemHandle(unique_ptr<FileSystem> fs) : file_system(std::move(fs)) {
	}

	unique_ptr<FileSystem> file_system;
};

//! The FileSystemRegistry holds the set of file systems that are registered.
//! Note that it should be treated as read-only.
//! When a change is made (e.g. by registering new file system) a copy of the registry is made
struct FileSystemRegistry {
	explicit FileSystemRegistry(unique_ptr<FileSystem> fs)
	    : default_fs(make_shared_ptr<FileSystemHandle>(std::move(fs))) {
	}

	vector<shared_ptr<FileSystemHandle>> sub_systems;
	const shared_ptr<FileSystemHandle> default_fs;
	unordered_set<string> disabled_file_systems;
	//! Registered compression filesystems (both built-in and provided by extensions), keyed by compression type
	map<string, shared_ptr<FileSystemHandle>> compressed_fs;

public:
	shared_ptr<FileSystemRegistry> RegisterSubSystem(unique_ptr<FileSystem> fs) const;
	shared_ptr<FileSystemRegistry> RegisterCompressionFilesystem(unique_ptr<CompressedFileSystem> fs) const;
	shared_ptr<FileSystemRegistry> SetDisabledFileSystems(const vector<string> &names) const;
	shared_ptr<FileSystemRegistry> ExtractSubSystem(const string &name, unique_ptr<FileSystem> &result) const;
};

shared_ptr<FileSystemRegistry> FileSystemRegistry::RegisterSubSystem(unique_ptr<FileSystem> fs) const {
	auto new_registry = make_shared_ptr<FileSystemRegistry>(*this);
	const auto &name = fs->GetName();
	for (auto &sub_system : new_registry->sub_systems) {
		if (sub_system->file_system->GetName() == name) {
			throw InvalidInputException("Filesystem with name %s has already been registered, cannot re-register!",
			                            name);
		}
	}
	new_registry->sub_systems.push_back(make_shared_ptr<FileSystemHandle>(std::move(fs)));
	return new_registry;
}

shared_ptr<FileSystemRegistry>
FileSystemRegistry::RegisterCompressionFilesystem(unique_ptr<CompressedFileSystem> fs) const {
	auto new_registry = make_shared_ptr<FileSystemRegistry>(*this);
	auto compression_type = fs->GetCompressionType().ToString();
	new_registry->compressed_fs[compression_type] = make_shared_ptr<FileSystemHandle>(std::move(fs));
	return new_registry;
}

shared_ptr<FileSystemRegistry> FileSystemRegistry::SetDisabledFileSystems(const vector<string> &names) const {
	auto new_registry = make_shared_ptr<FileSystemRegistry>(*this);
	unordered_set<string> new_disabled_file_systems;
	for (auto &name : names) {
		if (name.empty()) {
			continue;
		}
		if (new_disabled_file_systems.find(name) != new_disabled_file_systems.end()) {
			throw InvalidInputException("Duplicate disabled file system \"%s\"", name);
		}
		new_disabled_file_systems.insert(name);
	}
	for (auto &disabled_fs : disabled_file_systems) {
		if (new_disabled_file_systems.find(disabled_fs) == new_disabled_file_systems.end()) {
			throw InvalidInputException("File system \"%s\" has been disabled previously, it cannot be re-enabled",
			                            disabled_fs);
		}
	}
	new_registry->disabled_file_systems = std::move(new_disabled_file_systems);
	return new_registry;
}

shared_ptr<FileSystemRegistry> FileSystemRegistry::ExtractSubSystem(const string &name,
                                                                    unique_ptr<FileSystem> &result) const {
	auto new_registry = make_shared_ptr<FileSystemRegistry>(*this);
	// If the subsystem has been disabled, we don't allow extraction and return nullptr here.
	if (disabled_file_systems.find(name) != disabled_file_systems.end()) {
		return nullptr;
	}

	for (auto iter = new_registry->sub_systems.begin(); iter != new_registry->sub_systems.end(); ++iter) {
		auto &cur_filesystem = (*iter)->file_system;
		if (cur_filesystem->GetName() == name) {
			result = std::move(cur_filesystem);
			new_registry->sub_systems.erase(iter);
			return new_registry;
		}
	}

	// Requested subfilesystem is not registered.
	return nullptr;
}

VirtualFileSystem::VirtualFileSystem() : VirtualFileSystem(FileSystem::CreateLocal()) {
}

VirtualFileSystem::VirtualFileSystem(unique_ptr<FileSystem> &&inner)
    : file_system_registry(make_shared_ptr<FileSystemRegistry>(std::move(inner))) {
	VirtualFileSystem::RegisterCompressionFilesystem(make_uniq<GZipFileSystem>());
}

VirtualFileSystem::~VirtualFileSystem() {
}

unique_ptr<MemoryMappedFile> VirtualFileSystem::MemoryMapFile(const OpenFileInfo &path, FileOpenFlags flags,
                                                              const MMapOptions &options,
                                                              optional_ptr<FileOpener> opener) {
	auto registry = file_system_registry.atomic_load();
	auto &internal_filesystem = FindFileSystem(registry, path.path, opener);
	return internal_filesystem.MemoryMapFile(path, flags, options, opener);
}

FileSystem &VirtualFileSystem::GetDefaultFileSystem() {
	auto &fs = *file_system_registry->default_fs->file_system;
	if (SubSystemIsDisabled(fs.GetName())) {
		throw PermissionException("File system %s has been disabled by configuration", fs.GetName());
	}
	return fs;
}

//! The part of a path whose extension names its compression: a URL's path without its host, query or fragment, or a
//! local path up to a '?' (as IsFileCompressed treats it), except in a Windows long path prefix
static string CompressionSuffixPath(const string &lower_path) {
	auto scheme_end = lower_path.find("://");
	if (scheme_end == string::npos) {
		return StringUtil::StartsWith(lower_path, "\\\\?\\") ? lower_path : lower_path.substr(0, lower_path.find('?'));
	}
	// the host ends at the first '/', '?' or '#'; only a '/' starts a path
	auto path_start = lower_path.find_first_of("/?#", scheme_end + 3);
	if (path_start == string::npos || lower_path[path_start] != '/') {
		return string();
	}
	auto path_end = lower_path.find_first_of("?#", path_start);
	return lower_path.substr(path_start, path_end == string::npos ? string::npos : path_end - path_start);
}

//! The compression a file extension indicates, whether or not a filesystem for it is registered
static FileCompressionType CompressionFromExtension(const string &lower_path) {
	static const pair<const char *, const char *> compression_extensions[] = {
	    {".zst", "zstd"},  {".bz2", "bzip2"}, {".xz", "xz"},  {".lz4", "lz4"},
	    {".br", "brotli"}, {".lzma", "lzma"}, {".zip", "zip"}};
	auto path = CompressionSuffixPath(lower_path);
	for (auto &entry : compression_extensions) {
		if (StringUtil::EndsWith(path, entry.first)) {
			return FileCompressionType(entry.second);
		}
	}
	return FileCompressionType::UNCOMPRESSED;
}

optional_ptr<FileSystem> VirtualFileSystem::FindCompressionFileSystem(FileSystemRegistry &registry,
                                                                      const FileCompressionType &compression,
                                                                      const string &filepath, bool for_writing) {
	auto resolved = compression;
	// For auto-detection, check whether any registered compression filesystem can handle this file.
	if (resolved.IsAutoDetect()) {
		auto lower_path = StringUtil::Lower(filepath);
		if (StringUtil::EndsWith(lower_path, ".tmp")) {
			// strip .tmp
			lower_path = lower_path.substr(0, lower_path.length() - 4);
		}
		for (auto &entry : registry.compressed_fs) {
			if (entry.second->file_system->CanHandleFile(lower_path)) {
				return entry.second->file_system.get();
			}
		}
		resolved = CompressionFromExtension(lower_path);
		// a file written under such a name is written as is (e.g. a blob that is already compressed), except zstd,
		// which has always required its filesystem
		if (for_writing && resolved != FileCompressionType::ZSTD) {
			return nullptr;
		}
		if (resolved.IsUncompressed()) {
			// no applicable compression filesystem was found - consider the file uncompressed
			return nullptr;
		}
		// the file looks compressed but its compression is not registered - fall through to raise an error below
	}
	if (resolved.IsUncompressed()) {
		return nullptr;
	}

	// An explicit compression type was requested - look it up in the registry.
	auto iter = registry.compressed_fs.find(resolved.ToString());
	if (iter != registry.compressed_fs.end()) {
		return iter->second->file_system.get();
	}
	// zstd support is provided by the parquet extension - hint at loading it
	string hint;
	if (resolved == FileCompressionType::ZSTD) {
		hint = "\nConsider explicitly \"INSTALL parquet; LOAD parquet;\" to support this compression scheme";
	}
	if (compression.IsAutoDetect()) {
		hint +=
		    StringUtil::Format("\nThe compression was detected from the name of \"%s\"; set compression = 'none' to "
		                       "treat it as uncompressed",
		                       filepath);
	}
	throw NotImplementedException(
	    "Attempting to open a compressed file, but the compression type is not supported (compression type \"%s\")%s",
	    resolved.ToString(), hint);
}

unique_ptr<FileHandle> VirtualFileSystem::OpenFileExtended(const OpenFileInfo &file, FileOpenFlags flags,
                                                           optional_ptr<FileOpener> opener) {
	const FileCompressionType compression = flags.Compression();
	// open the base file handle in UNCOMPRESSED mode
	flags.SetCompression(FileCompressionType::UNCOMPRESSED);

	auto registry = file_system_registry.atomic_load();
	auto &internal_filesystem = FindFileSystem(registry, file.path, opener);

	// File handle gets created.
	unique_ptr<FileHandle> file_handle = nullptr;

	// Handle caching logic.
	if (flags.GetCachingMode() != CachingMode::NO_CACHING) {
		auto caching_filesystem =
		    make_shared_ptr<CachingFileSystemWrapper>(internal_filesystem, opener, flags.GetCachingMode());
		// caching filesystem's lifecycle is extended inside of caching file handle.
		file_handle = caching_filesystem->OpenFile(file, flags, opener);
	} else {
		file_handle = internal_filesystem.OpenFile(file, flags, opener);
	}
	if (!file_handle) {
		return nullptr;
	}

	try {
		// Evaluate and apply compression option then.
		const auto context = !flags.MultiClientAccess() ? FileOpener::TryGetClientContext(opener) : QueryContext();
		if (file_handle->GetType() == FileType::FILE_TYPE_FIFO) {
			file_handle = PipeFileSystem::OpenPipe(context, std::move(file_handle));
		} else {
			auto compression_filesystem =
			    FindCompressionFileSystem(*registry, compression, file.path, flags.OpenForWriting());
			if (compression_filesystem) {
				file_handle =
				    compression_filesystem->OpenCompressedFile(context, std::move(file_handle), flags.OpenForWriting());
			}
		}
	} catch (...) {
		auto error = std::current_exception();
		if (flags.OpenForWriting() && file_handle) {
			try {
				file_handle->AbortWrite();
			} catch (...) { // NOLINT
			}
		}
		std::rethrow_exception(error);
	}
	return file_handle;
}

void VirtualFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	handle.file_system.Read(handle, buffer, nr_bytes, location);
}

void VirtualFileSystem::Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	handle.file_system.Write(handle, buffer, nr_bytes, location);
}

int64_t VirtualFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	return handle.file_system.Read(handle, buffer, nr_bytes);
}

int64_t VirtualFileSystem::Write(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	return handle.file_system.Write(handle, buffer, nr_bytes);
}

int64_t VirtualFileSystem::GetFileSize(FileHandle &handle) {
	return handle.file_system.GetFileSize(handle);
}
timestamp_t VirtualFileSystem::GetLastModifiedTime(FileHandle &handle) {
	return handle.file_system.GetLastModifiedTime(handle);
}
string VirtualFileSystem::GetVersionTag(FileHandle &handle) {
	return handle.file_system.GetVersionTag(handle);
}
FileType VirtualFileSystem::GetFileType(FileHandle &handle) {
	return handle.file_system.GetFileType(handle);
}
FileMetadata VirtualFileSystem::Stats(FileHandle &handle) {
	return handle.file_system.Stats(handle);
}

optional<FileMetadata> VirtualFileSystem::GetStatsIfExists(const OpenFileInfo &file, optional_ptr<FileOpener> opener) {
	return FindFileSystem(file.path, opener).GetStatsIfExists(file, opener);
}

void VirtualFileSystem::Truncate(FileHandle &handle, int64_t new_size) {
	handle.file_system.Truncate(handle, new_size);
}

void VirtualFileSystem::FileSync(FileHandle &handle) {
	handle.file_system.FileSync(handle);
}

// need to look up correct fs for this
bool VirtualFileSystem::DirectoryExists(const string &directory, optional_ptr<FileOpener> opener) {
	return FindFileSystem(directory, opener).DirectoryExists(directory, opener);
}
void VirtualFileSystem::CreateDirectory(const string &directory, optional_ptr<FileOpener> opener) {
	CreateDirectoryExtended(directory, {CreateDirectoryMode::SINGLE}, opener);
}

bool VirtualFileSystem::CreateDirectoryExtended(const string &directory, const CreateDirectoryOptions &options,
                                                optional_ptr<FileOpener> opener) {
	return FindFileSystem(directory, opener).CreateDirectoryExtended(directory, options, opener);
}

void VirtualFileSystem::CreateDirectoriesRecursive(const string &path, optional_ptr<FileOpener> opener) {
	CreateDirectoryExtended(path, {CreateDirectoryMode::RECURSIVE}, opener);
}

void VirtualFileSystem::RemoveDirectory(const string &directory, optional_ptr<FileOpener> opener) {
	RemoveDirectoryExtended(directory, {RemoveDirectoryMode::RECURSIVE}, opener);
}

bool VirtualFileSystem::RemoveDirectoryExtended(const string &directory, const RemoveDirectoryOptions &options,
                                                optional_ptr<FileOpener> opener) {
	return FindFileSystem(directory, opener).RemoveDirectoryExtended(directory, options, opener);
}

bool VirtualFileSystem::ListFilesExtended(const string &directory,
                                          const std::function<void(OpenFileInfo &info)> &callback,
                                          optional_ptr<FileOpener> opener) {
	return FindFileSystem(directory, opener).ListFiles(directory, callback, opener);
}

void VirtualFileSystem::MoveFile(const string &source, const string &target, optional_ptr<FileOpener> opener) {
	FindFileSystem(source, opener).MoveFile(source, target, opener);
}

bool VirtualFileSystem::FileExists(const string &filename, optional_ptr<FileOpener> opener) {
	return FindFileSystem(filename, opener).FileExists(filename, opener);
}

bool VirtualFileSystem::IsPipe(const string &filename, optional_ptr<FileOpener> opener) {
	return FindFileSystem(filename, opener).IsPipe(filename, opener);
}

void VirtualFileSystem::RemoveFile(const string &filename, optional_ptr<FileOpener> opener) {
	FindFileSystem(filename, opener).RemoveFile(filename, opener);
}

bool VirtualFileSystem::TryRemoveFile(const string &filename, optional_ptr<FileOpener> opener) {
	return FindFileSystem(filename, opener).TryRemoveFile(filename, opener);
}

void VirtualFileSystem::RemoveFiles(const vector<string> &filenames, optional_ptr<FileOpener> opener) {
	reference_map_t<FileSystem, vector<string>> files_by_fs;
	for (const auto &filename : filenames) {
		auto &fs = FindFileSystem(filename, opener);
		files_by_fs[fs].push_back(filename);
	}
	for (auto &entry : files_by_fs) {
		entry.first.get().RemoveFiles(entry.second, opener);
	}
}

string VirtualFileSystem::PathSeparator(const string &path) {
	return FindFileSystem(path, nullptr).PathSeparator(path);
}

string VirtualFileSystem::CanonicalizePath(const string &path_p, optional_ptr<FileOpener> opener) {
	return FindFileSystem(path_p, opener).CanonicalizePath(path_p, opener);
}

unique_ptr<MultiFileList> VirtualFileSystem::GlobFilesExtended(const string &path, const FileGlobInput &input,
                                                               optional_ptr<FileOpener> opener) {
	return FindFileSystem(path, opener).Glob(path, input, opener);
}

void VirtualFileSystem::RegisterSubSystem(unique_ptr<FileSystem> fs) {
	lock_guard<mutex> guard(registry_lock);
	auto new_registry = file_system_registry->RegisterSubSystem(std::move(fs));
	file_system_registry.atomic_store(new_registry);
}

void VirtualFileSystem::RegisterCompressionFilesystem(unique_ptr<CompressedFileSystem> fs) {
	const lock_guard<mutex> guard(registry_lock);
	auto new_registry = file_system_registry->RegisterCompressionFilesystem(std::move(fs));
	file_system_registry.atomic_store(new_registry);
}

void VirtualFileSystem::UnregisterSubSystem(const string &name) {
	auto sub_system = ExtractSubSystem(name);

	lock_guard<mutex> guard(registry_lock);
	unregistered_file_systems.push_back(std::move(sub_system));
}

void VirtualFileSystem::SetDisabledFileSystems(const vector<string> &names) {
	lock_guard<mutex> guard(registry_lock);
	auto new_registry = file_system_registry->SetDisabledFileSystems(names);
	file_system_registry.atomic_store(new_registry);
}

unique_ptr<FileSystem> VirtualFileSystem::ExtractSubSystem(const string &name) {
	lock_guard<mutex> guard(registry_lock);
	unique_ptr<FileSystem> result;
	auto new_registry = file_system_registry->ExtractSubSystem(name, result);
	if (new_registry) {
		file_system_registry.atomic_store(new_registry);
	}
	return result;
}

vector<string> VirtualFileSystem::ListSubSystems() {
	auto registry = file_system_registry.atomic_load();
	auto &sub_systems = registry->sub_systems;
	vector<string> names;
	for (auto &sub_system : sub_systems) {
		names.push_back(sub_system->file_system->GetName());
	}
	return names;
}

std::string VirtualFileSystem::GetName() const {
	return "VirtualFileSystem";
}

bool VirtualFileSystem::SubSystemIsDisabled(const string &name) {
	auto registry = file_system_registry.atomic_load();
	auto &disabled_file_systems = registry->disabled_file_systems;
	return disabled_file_systems.find(name) != disabled_file_systems.end();
}

bool VirtualFileSystem::IsDisabledForPath(const string &path) {
	auto registry = file_system_registry.atomic_load();
	auto &disabled_file_systems = registry->disabled_file_systems;
	if (disabled_file_systems.empty()) {
		return false;
	}
	auto fs = FindFileSystemInternal(*registry, path);
	if (!fs) {
		fs = registry->default_fs->file_system;
	}
	return disabled_file_systems.find(fs->GetName()) != disabled_file_systems.end();
}

FileSystem &VirtualFileSystem::FindFileSystem(const string &path, optional_ptr<FileOpener> opener) {
	auto registry = file_system_registry.atomic_load();
	return FindFileSystem(registry, path, opener);
}

FileSystem &VirtualFileSystem::FindFileSystem(shared_ptr<FileSystemRegistry> &registry, const string &path,
                                              optional_ptr<FileOpener> opener) {
	auto db_instance = FileOpener::TryGetDatabase(opener);
	auto fs = FindFileSystemInternal(*registry, path);

	if (!fs && db_instance) {
		ExtensionHelper::AutoLoadExtensionForPath(*db_instance, path);
		// refresh the registry after loading extensions
		registry = file_system_registry.atomic_load();

		// Retry after having autoloaded
		fs = FindFileSystemInternal(*registry, path);
	}

	if (!fs) {
		fs = registry->default_fs->file_system;
	}
	auto &disabled_file_systems = registry->disabled_file_systems;
	if (!disabled_file_systems.empty() && disabled_file_systems.find(fs->GetName()) != disabled_file_systems.end()) {
		throw PermissionException("File system %s has been disabled by configuration", fs->GetName());
	}
	return *fs;
}

optional_ptr<FileSystem> VirtualFileSystem::FindFileSystemInternal(FileSystemRegistry &registry, const string &path) {
	optional_ptr<FileSystem> fs;
	for (auto &sub_system : registry.sub_systems) {
		auto &sub_fs = *sub_system->file_system;
		if (sub_fs.CanHandleFile(path)) {
			if (sub_fs.IsManuallySet()) {
				return sub_fs;
			}
			fs = sub_fs;
		}
	}
	return fs;
}

} // namespace duckdb
