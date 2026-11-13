#include "duckdb/execution/operator/persistent/copy_output_lifecycle.hpp"

#include "duckdb/common/enums/file_sync_mode.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/opener_file_system.hpp"
#include "duckdb/common/set.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/settings.hpp"

#include <algorithm>

namespace duckdb {

CopyOutputLifecycle::CopyOutputLifecycle(ClientContext &context_p, bool fsync_p)
    : context(context_p), fsync(fsync_p && Settings::Get<FsyncModeSetting>(context_p) != FileSyncMode::NONE) {
}

CopyOutputLifecycle::~CopyOutputLifecycle() {
	try {
		Cleanup();
	} catch (...) { // NOLINT
	}
}

CopyOutputOwnership CopyOutputLifecycle::GetOwnership(const string &path) const {
	auto &fs = FileSystem::GetFileSystem(context);
	// checking a remote path costs a request for every output file, so a remote output is kept like an existing one
	if (fs.IsRemoteFile(path) || fs.FileExists(path) || fs.DirectoryExists(path) || fs.IsPipe(path)) {
		return CopyOutputOwnership::PRESERVE;
	}
	return CopyOutputOwnership::REMOVE_ON_FAILURE;
}

void CopyOutputLifecycle::Cleanup() {
	vector<string> files_to_remove;
	vector<string> directories_to_remove;
	{
		annotated_lock_guard<annotated_mutex> guard(lock);
		if (successful) {
			return;
		}
		for (auto &file : files) {
			if (file.ownership == CopyOutputOwnership::REMOVE_ON_FAILURE) {
				files_to_remove.push_back(std::move(file.path));
			}
		}
		directories_to_remove = std::move(created_directories);
	}

	auto &fs = FileSystem::GetFileSystem(context);
	if (!files_to_remove.empty()) {
		try {
			fs.RemoveFiles(files_to_remove);
		} catch (...) { // NOLINT
			for (auto &file : files_to_remove) {
				try {
					fs.TryRemoveFile(file);
				} catch (...) { // NOLINT
				}
			}
		}
	}

	std::sort(directories_to_remove.begin(), directories_to_remove.end(), [](const string &left, const string &right) {
		if (left.size() != right.size()) {
			return left.size() > right.size();
		}
		return left < right;
	});
	directories_to_remove.erase(std::unique(directories_to_remove.begin(), directories_to_remove.end()),
	                            directories_to_remove.end());
	for (auto &directory : directories_to_remove) {
		try {
			fs.RemoveDirectoryExtended(directory, {RemoveDirectoryMode::SINGLE});
		} catch (...) { // NOLINT
		}
	}
}

bool CopyOutputLifecycle::RegisterFile(string path) {
	auto &fs = FileSystem::GetFileSystem(context);
	auto ownership = GetOwnership(path);
	// remote files and pipes are not synced
	auto synced = fsync && !fs.IsRemoteFile(path) && !fs.IsPipe(path);
	annotated_lock_guard<annotated_mutex> guard(lock);
	files.push_back({std::move(path), ownership, synced});
	return synced;
}

void CopyOutputLifecycle::RegisterCreatedDirectory(string path) {
	annotated_lock_guard<annotated_mutex> guard(lock);
	created_directories.push_back(std::move(path));
}

static string ParentDirectory(FileSystem &fs, const string &path) {
	auto separator = fs.PathSeparator(path);
	auto end = path.find_last_not_of(separator);
	if (end == string::npos) {
		return path;
	}
	auto pos = path.find_last_of(separator, end);
	if (pos == string::npos) {
		return ".";
	}
	return pos == 0 ? separator : path.substr(0, pos);
}

unique_ptr<FileHandle> CopyOutputLifecycle::OpenDirectory(const string &directory) const {
	auto &client_fs = FileSystem::GetFileSystem(context).Cast<OpenerFileSystem>();
	// the client file system checks an opened path as a file, so check the directory as one before opening it
	client_fs.VerifyCanAccessDirectory(directory);
	return client_fs.GetFileSystem().OpenFile(directory, FileFlags::FILE_FLAGS_READ, client_fs.GetOpener());
}

bool CopyOutputLifecycle::IsSynced(const string &path) {
	annotated_lock_guard<annotated_mutex> guard(lock);
	for (auto &file : files) {
		if (file.path == path) {
			return file.synced;
		}
	}
	return false;
}

void CopyOutputLifecycle::MoveFile(const string &path, const string &new_path) {
	auto &fs = FileSystem::GetFileSystem(context);
#ifndef _WIN32
	if (IsSynced(path)) {
		// a directory that cannot be opened for its sync fails before the rename publishes the file
		OpenDirectory(ParentDirectory(fs, new_path));
	}
#endif
	// the replacement of an existing file is kept on failure, as the replaced file is gone
	auto replaced = GetOwnership(new_path) == CopyOutputOwnership::PRESERVE;
	fs.MoveFile(path, new_path);
	annotated_lock_guard<annotated_mutex> guard(lock);
	for (auto &file : files) {
		if (file.path == path) {
			file.path = new_path;
			if (replaced) {
				file.ownership = CopyOutputOwnership::PRESERVE;
			}
		}
	}
}

void CopyOutputLifecycle::SyncDirectories() {
#ifndef _WIN32
	if (!fsync) {
		return;
	}
	auto &fs = FileSystem::GetFileSystem(context);
	set<string> directories;
	{
		annotated_lock_guard<annotated_mutex> guard(lock);
		for (auto &file : files) {
			if (file.synced) {
				directories.insert(ParentDirectory(fs, file.path));
			}
		}
		for (auto &directory : created_directories) {
			if (!FileSystem::IsRemoteFile(directory)) {
				directories.insert(ParentDirectory(fs, directory));
			}
		}
	}
	for (auto &directory : directories) {
		OpenDirectory(directory)->Sync();
	}
#endif
}

void CopyOutputLifecycle::MarkSuccessful() noexcept {
	annotated_lock_guard<annotated_mutex> guard(lock);
	successful = true;
}

} // namespace duckdb
