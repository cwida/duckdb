//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/virtual_file_system.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/file_system.hpp"
#include "duckdb/common/map.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/mutex.hpp"

namespace duckdb {
struct FileSystemRegistry;

// bunch of wrappers to allow registering protocol handlers
class VirtualFileSystem : public FileSystem {
public:
	VirtualFileSystem();
	explicit VirtualFileSystem(unique_ptr<FileSystem> &&inner_file_system);
	~VirtualFileSystem() override;

	void Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override;
	void Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override;
	int64_t Read(FileHandle &handle, void *buffer, int64_t nr_bytes) override;
	int64_t Write(FileHandle &handle, void *buffer, int64_t nr_bytes) override;

	unique_ptr<MemoryMappedFile> MemoryMapFile(const OpenFileInfo &path, FileOpenFlags flags,
	                                           const MMapOptions &options,
	                                           optional_ptr<FileOpener> opener = nullptr) override;

	int64_t GetFileSize(FileHandle &handle) override;
	timestamp_t GetLastModifiedTime(FileHandle &handle) override;
	string GetVersionTag(FileHandle &handle) override;
	FileType GetFileType(FileHandle &handle) override;
	FileMetadata Stats(FileHandle &handle) override;
	optional<FileMetadata> GetStatsIfExists(const OpenFileInfo &file,
	                                        optional_ptr<FileOpener> opener = nullptr) override;

	void Truncate(FileHandle &handle, int64_t new_size) override;

	void FileSync(FileHandle &handle) override;

	// need to look up correct fs for this
	bool DirectoryExists(const string &directory, optional_ptr<FileOpener> opener) override;
	void CreateDirectory(const string &directory, optional_ptr<FileOpener> opener) override;
	bool CreateDirectoryExtended(const string &directory, const CreateDirectoryOptions &options,
	                             optional_ptr<FileOpener> opener = nullptr) override;
	void CreateDirectoriesRecursive(const string &path, optional_ptr<FileOpener> opener = nullptr) override;

	void RemoveDirectory(const string &directory, optional_ptr<FileOpener> opener) override;
	bool RemoveDirectoryExtended(const string &directory, const RemoveDirectoryOptions &options,
	                             optional_ptr<FileOpener> opener = nullptr) override;

	void MoveFile(const string &source, const string &target, optional_ptr<FileOpener> opener) override;

	bool FileExists(const string &filename, optional_ptr<FileOpener> opener) override;

	bool IsPipe(const string &filename, optional_ptr<FileOpener> opener) override;
	void RemoveFile(const string &filename, optional_ptr<FileOpener> opener) override;
	bool TryRemoveFile(const string &filename, optional_ptr<FileOpener> opener) override;
	void RemoveFiles(const vector<string> &filenames, optional_ptr<FileOpener> opener) override;

	void RegisterSubSystem(unique_ptr<FileSystem> fs) override;
	void RegisterCompressionFilesystem(unique_ptr<CompressedFileSystem> fs) override;
	void UnregisterSubSystem(const string &name) override;
	unique_ptr<FileSystem> ExtractSubSystem(const string &name) override;

	vector<string> ListSubSystems() override;

	FileSystem &GetDefaultFileSystem();

	std::string GetName() const override;

	void SetDisabledFileSystems(const vector<string> &names) override;
	bool SubSystemIsDisabled(const string &name) override;
	bool IsDisabledForPath(const string &path) override;

	string PathSeparator(const string &path) override;

	string CanonicalizePath(const string &path_p, optional_ptr<FileOpener> opener) override;

protected:
	unique_ptr<FileHandle> OpenFileExtended(const OpenFileInfo &file, FileOpenFlags flags,
	                                        optional_ptr<FileOpener> opener) override;
	bool SupportsOpenFileExtended() const override {
		return true;
	}

	bool ListFilesExtended(const string &directory, const std::function<void(OpenFileInfo &info)> &callback,
	                       optional_ptr<FileOpener> opener) override;

	bool SupportsListFilesExtended() const override {
		return true;
	}

	unique_ptr<MultiFileList> GlobFilesExtended(const string &path, const FileGlobInput &input,
	                                            optional_ptr<FileOpener> opener) override;
	bool SupportsGlobExtended() const override {
		return true;
	}

private:
	FileSystem &FindFileSystem(const string &path, optional_ptr<FileOpener> file_opener);
	FileSystem &FindFileSystem(shared_ptr<FileSystemRegistry> &registry, const string &path,
	                           optional_ptr<FileOpener> file_opener);
	optional_ptr<FileSystem> FindFileSystemInternal(FileSystemRegistry &registry, const string &path);
	// Return nullptr if compression is not involved, throw exception if compression is requested but no usable
	// filesystem gets registered.
	optional_ptr<FileSystem> FindCompressionFileSystem(FileSystemRegistry &registry,
	                                                   const FileCompressionType &compression, const string &path,
	                                                   bool for_writing);

private:
	mutex registry_lock;
	shared_ptr<FileSystemRegistry> file_system_registry;
	vector<unique_ptr<FileSystem>> unregistered_file_systems;
};

} // namespace duckdb
