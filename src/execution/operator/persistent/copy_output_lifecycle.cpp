#include "duckdb/execution/operator/persistent/copy_output_lifecycle.hpp"

#include "duckdb/common/file_system.hpp"
#include "duckdb/main/client_context.hpp"

#include <algorithm>

namespace duckdb {

CopyOutputLifecycle::CopyOutputLifecycle(ClientContext &context_p) : context(context_p) {
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

void CopyOutputLifecycle::RegisterFile(string path) {
	auto ownership = GetOwnership(path);
	annotated_lock_guard<annotated_mutex> guard(lock);
	files.push_back({std::move(path), ownership});
}

void CopyOutputLifecycle::RegisterCreatedDirectory(string path) {
	annotated_lock_guard<annotated_mutex> guard(lock);
	created_directories.push_back(std::move(path));
}

void CopyOutputLifecycle::MarkSuccessful() noexcept {
	annotated_lock_guard<annotated_mutex> guard(lock);
	successful = true;
}

} // namespace duckdb
