//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/local_file_system.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/file_system.hpp"
#include "duckdb/common/windows_undefs.hpp"

namespace duckdb {

class LocalFileSystem : public FileSystem {
public:
	unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags,
	                                optional_ptr<FileOpener> opener = nullptr) override;

	unique_ptr<MemoryMappedFile> MemoryMapFile(const OpenFileInfo &path, FileOpenFlags flags,
	                                           const MMapOptions &options,
	                                           optional_ptr<FileOpener> opener = nullptr) override;

	//! Read exactly nr_bytes from the specified location in the file. Fails if nr_bytes could not be read. This is
	//! equivalent to calling SetFilePointer(location) followed by calling Read().
	void Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override;
	//! Write exactly nr_bytes to the specified location in the file. Fails if nr_bytes could not be written. This is
	//! equivalent to calling SetFilePointer(location) followed by calling Write().
	void Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override;
	//! Read nr_bytes from the specified file into the buffer, moving the file pointer forward by nr_bytes. Returns the
	//! amount of bytes read.
	int64_t Read(FileHandle &handle, void *buffer, int64_t nr_bytes) override;
	//! Write nr_bytes from the buffer into the file, moving the file pointer forward by nr_bytes.
	int64_t Write(FileHandle &handle, void *buffer, int64_t nr_bytes) override;
	FileWriteMode GetWriteMode(FileHandle &handle) override;
	//! Excise a range of the file. The file-system is free to deallocate this
	//! range (sparse file support). Reads to the range will succeed but will return
	//! undefined data.
	bool Trim(FileHandle &handle, idx_t offset_bytes, idx_t length_bytes) override;

	//! Returns the file size of a file handle, returns -1 on error
	int64_t GetFileSize(FileHandle &handle) override;
	//! Returns the file last modified time of a file handle, returns timespec with zero on all attributes on error
	timestamp_t GetLastModifiedTime(FileHandle &handle) override;
	//! Returns a tag that uniquely identifies the version of the file
	string GetVersionTag(FileHandle &handle) override;
	//! Returns the file last modified time of a file handle, returns timespec with zero on all attributes on error
	FileType GetFileType(FileHandle &handle) override;
	//! Returns the file stats of the attached handle.
	FileMetadata Stats(FileHandle &handle) override;
	optional<FileMetadata> GetStatsIfExists(const OpenFileInfo &file,
	                                        optional_ptr<FileOpener> opener = nullptr) override;
	//! Truncate a file to a maximum size of new_size, new_size should be smaller than or equal to the current size of
	//! the file
	void Truncate(FileHandle &handle, int64_t new_size) override;

	//! Check if a directory exists
	bool DirectoryExists(const string &directory, optional_ptr<FileOpener> opener = nullptr) override;
	//! Create a directory if it does not exist
	void CreateDirectory(const string &directory, optional_ptr<FileOpener> opener = nullptr) override;
	bool CreateDirectoryExtended(const string &directory, const CreateDirectoryOptions &options,
	                             optional_ptr<FileOpener> opener = nullptr) override;
	void CreateDirectoriesRecursive(const string &path, optional_ptr<FileOpener> opener = nullptr) override;
	//! Recursively remove a directory and all files in it
	void RemoveDirectory(const string &directory, optional_ptr<FileOpener> opener = nullptr) override;
	bool RemoveDirectoryExtended(const string &directory, const RemoveDirectoryOptions &options,
	                             optional_ptr<FileOpener> opener = nullptr) override;
	//! Move a file from source path to the target, StorageManager relies on this being an atomic action for ACID
	//! properties
	void MoveFile(const string &source, const string &target, optional_ptr<FileOpener> opener = nullptr) override;
	//! Check if a file exists
	bool FileExists(const string &filename, optional_ptr<FileOpener> opener = nullptr) override;

	//! Check if path is a pipe or character device
	bool IsPipe(const string &filename, optional_ptr<FileOpener> opener = nullptr) override;
	//! Remove a file from disk
	void RemoveFile(const string &filename, optional_ptr<FileOpener> opener = nullptr) override;
	//! Sync a file handle to disk
	void FileSync(FileHandle &handle) override;
	//! Close and remove an incomplete output file
	void AbortFileWrite(FileHandle &handle) override;

	//! Checks if path is is an absolute path
	bool IsPathAbsolute(const string &path) override;
	string MakePathAbsolute(const string &input, optional_ptr<FileOpener> opener);
	bool PathStartsWithDrive(const string &path);

	bool CanHandleFile(const string &fpath) override {
		//! Whether or not a sub-system can handle a specific file path
		return false;
	}

	//! Set the file pointer of a file handle to a specified location. Reads and writes will happen from this location
	void Seek(FileHandle &handle, idx_t location) override;
	//! Return the current seek position in the file.
	idx_t SeekPosition(FileHandle &handle) override;

	//! Whether or not we can seek into the file
	bool CanSeek() override;
	//! Whether or not the FS handles plain files on disk. This is relevant for certain optimizations, as random reads
	//! in a file on-disk are much cheaper than e.g. random reads in a file over the network
	bool OnDiskFile(FileHandle &handle) override;

	bool IsLocalFileSystem() const override {
		return true;
	}

	std::string GetName() const override {
		return "LocalFileSystem";
	}

	//! Returns the last Win32 error, in string format. Returns an empty string if there is no error, or on non-Windows
	//! systems.
	static std::string GetLastErrorAsString();

	//! Checks a file is private (checks for 600 on linux/macos, TODO: currently always returns true on windows)
	static bool IsPrivateFile(const string &path_p, FileOpener *opener);

	vector<OpenFileInfo> FetchFileWithoutGlob(const string &path, optional_ptr<FileOpener> opener, bool absolute_path);

	string CanonicalizePath(const string &path_p, optional_ptr<FileOpener> opener) override;

protected:
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

	bool TryCanonicalizeExistingPath(string &path_p);

	string VersionTagFromMetadata(const FileMetadata &file_metadata);
	void FillFileOptions(const FileMetadata &file_metadata, unordered_map<string, Value> &options);

private:
	//! Set the file pointer of a file handle to a specified location. Reads and writes will happen from this location
	void SetFilePointer(FileHandle &handle, idx_t location);
	idx_t GetFilePointer(FileHandle &handle);
};

} // namespace duckdb
