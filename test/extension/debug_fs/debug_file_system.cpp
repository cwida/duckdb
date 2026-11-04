#include "duckdb/common/compressed_file_system.hpp"
#include "duckdb/common/memory_mapped_file.hpp"
#include "duckdb/common/multi_file/multi_file_list.hpp"
#include "debug_file_system.hpp"

#include "duckdb/common/helper.hpp"
#include "duckdb/common/thread.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/database.hpp"
#include "io_latency_model.hpp"

namespace duckdb {

DebugFileHandle::DebugFileHandle(DebugFileSystem &fs, unique_ptr<FileHandle> inner_p)
    : FileHandle(fs, inner_p->path, inner_p->flags), inner(std::move(inner_p)) {
}

void DebugFileHandle::Close() {
	inner->Close();
}

bool DebugFileHandle::CanSeek() {
	return inner->CanSeek();
}

idx_t DebugFileHandle::GetProgress() {
	return inner->GetProgress();
}

FileCompressionType DebugFileHandle::GetFileCompressionType() {
	return inner->GetFileCompressionType();
}

DebugFileSystem::DebugFileSystem(unique_ptr<FileSystem> inner_fs, DatabaseInstance &db)
    : inner_fs(std::move(inner_fs)), db(db) {
}

void DebugFileSystem::SetDelayMeanMs(double v) {
	const annotated_lock_guard<annotated_mutex> guard(random_engine_lock);
	delay_mean_ms = v;
	ALWAYS_ASSERT(delay_mean_ms >= 0.0);
}

void DebugFileSystem::SetDelayStddevMs(double v) {
	const annotated_lock_guard<annotated_mutex> guard(random_engine_lock);
	delay_stddev_ms = v;
	ALWAYS_ASSERT(delay_stddev_ms >= 0.0);
}

void DebugFileSystem::SetRandomSeed(optional_idx seed) {
	const annotated_lock_guard<annotated_mutex> guard(random_engine_lock);
	if (random_engine && seed.IsValid()) {
		throw InvalidInputException("Cannot change debug_fs_random_seed after the random engine has been initialized");
	}
	random_seed = seed;
}

void DebugFileSystem::EnsureRandomEngineInitialized() {
	const annotated_lock_guard<annotated_mutex> guard(random_engine_lock);
	if (random_engine) {
		return;
	}
	uint64_t seed = 0;
	if (random_seed.IsValid()) {
		seed = random_seed.GetIndex();
	} else {
		seed = NumericCast<uint64_t>(Timestamp::GetCurrentTimestamp().value);
	}
	random_engine = make_uniq<RandomEngine>();
	random_engine->SetSeed(seed);
	// Log the random seed for reproduction.
	DUCKDB_LOG_INFO(db, "DebugFileSystem initialized with random seed: %llu", seed);
}

void DebugFileSystem::ApplyDelay() {
#ifndef DUCKDB_NO_THREADS
	double mean_ms = 0;
	double stddev_ms = 0;
	{
		const annotated_lock_guard<annotated_mutex> guard(random_engine_lock);
		mean_ms = delay_mean_ms;
		stddev_ms = delay_stddev_ms;
	}

	if (mean_ms == 0.0) {
		return;
	}

	// Lazy initialize the random engine on first IO operation, so user-set random seed could be applied.
	EnsureRandomEngineInitialized();

	double delay_ms = 0;
	if (stddev_ms == 0.0) {
		delay_ms = mean_ms;
	} else {
		const annotated_lock_guard<annotated_mutex> guard(random_engine_lock);
		delay_ms = IoLatencyModel(mean_ms, stddev_ms).SampleLatency(*random_engine);
	}
	if (delay_ms > 0.0) {
		ThreadUtil::SleepMs(LossyNumericCast<idx_t>(delay_ms));
	}
#endif
}

unique_ptr<FileHandle> DebugFileSystem::OpenFileExtended(const OpenFileInfo &file, FileOpenFlags flags,
                                                         optional_ptr<FileOpener> opener) {
	ApplyDelay();
	auto inner_handle = inner_fs->OpenFile(file, flags, opener);
	if (!inner_handle) {
		return nullptr;
	}
	return make_uniq<DebugFileHandle>(*this, std::move(inner_handle));
}

void DebugFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	ApplyDelay();
	auto &inner = *handle.Cast<DebugFileHandle>().inner;
	inner.file_system.Read(inner, buffer, nr_bytes, location);
}

int64_t DebugFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	ApplyDelay();
	auto &inner = *handle.Cast<DebugFileHandle>().inner;
	return inner.file_system.Read(inner, buffer, nr_bytes);
}

void DebugFileSystem::Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	ApplyDelay();
	auto &inner = *handle.Cast<DebugFileHandle>().inner;
	inner.file_system.Write(inner, buffer, nr_bytes, location);
}

int64_t DebugFileSystem::Write(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	ApplyDelay();
	auto &inner = *handle.Cast<DebugFileHandle>().inner;
	return inner.file_system.Write(inner, buffer, nr_bytes);
}

string DebugFileSystem::GetName() const {
	return inner_fs->GetName();
}

bool DebugFileSystem::IsLocalFileSystem() const {
	return inner_fs->IsLocalFileSystem();
}

int64_t DebugFileSystem::GetFileSize(FileHandle &handle) {
	auto &inner = *handle.Cast<DebugFileHandle>().inner;
	return inner.file_system.GetFileSize(inner);
}

timestamp_t DebugFileSystem::GetLastModifiedTime(FileHandle &handle) {
	auto &inner = *handle.Cast<DebugFileHandle>().inner;
	return inner.file_system.GetLastModifiedTime(inner);
}

string DebugFileSystem::GetVersionTag(FileHandle &handle) {
	auto &inner = *handle.Cast<DebugFileHandle>().inner;
	return inner.file_system.GetVersionTag(inner);
}

FileType DebugFileSystem::GetFileType(FileHandle &handle) {
	auto &inner = *handle.Cast<DebugFileHandle>().inner;
	return inner.file_system.GetFileType(inner);
}

FileMetadata DebugFileSystem::Stats(FileHandle &handle) {
	auto &inner = *handle.Cast<DebugFileHandle>().inner;
	return inner.file_system.Stats(inner);
}

optional<FileMetadata> DebugFileSystem::GetStatsIfExists(const OpenFileInfo &file, optional_ptr<FileOpener> opener) {
	return inner_fs->GetStatsIfExists(file, opener);
}

void DebugFileSystem::Truncate(FileHandle &handle, int64_t new_size) {
	auto &inner = *handle.Cast<DebugFileHandle>().inner;
	inner.file_system.Truncate(inner, new_size);
}

void DebugFileSystem::FileSync(FileHandle &handle) {
	auto &inner = *handle.Cast<DebugFileHandle>().inner;
	inner.file_system.FileSync(inner);
}

void DebugFileSystem::AbortFileWrite(FileHandle &handle) {
	handle.Cast<DebugFileHandle>().inner->AbortWrite();
}

void DebugFileSystem::Seek(FileHandle &handle, idx_t location) {
	auto &inner = *handle.Cast<DebugFileHandle>().inner;
	inner.file_system.Seek(inner, location);
}

void DebugFileSystem::Reset(FileHandle &handle) {
	auto &inner = *handle.Cast<DebugFileHandle>().inner;
	inner.file_system.Reset(inner);
}

idx_t DebugFileSystem::SeekPosition(FileHandle &handle) {
	auto &inner = *handle.Cast<DebugFileHandle>().inner;
	return inner.file_system.SeekPosition(inner);
}

FileWriteMode DebugFileSystem::GetWriteMode(FileHandle &handle) {
	auto &inner = *handle.Cast<DebugFileHandle>().inner;
	return inner.file_system.GetWriteMode(inner);
}

bool DebugFileSystem::OnDiskFile(FileHandle &handle) {
	auto &inner = *handle.Cast<DebugFileHandle>().inner;
	return inner.file_system.OnDiskFile(inner);
}

bool DebugFileSystem::Trim(FileHandle &handle, idx_t offset_bytes, idx_t length_bytes) {
	auto &inner = *handle.Cast<DebugFileHandle>().inner;
	return inner.file_system.Trim(inner, offset_bytes, length_bytes);
}

bool DebugFileSystem::TryGetNetworkThroughput(FileHandle &handle, NetworkThroughputEstimate &result) {
	auto &inner = *handle.Cast<DebugFileHandle>().inner;
	return inner.file_system.TryGetNetworkThroughput(inner, result);
}

unique_ptr<FileHandle> DebugFileSystem::OpenCompressedFile(QueryContext context, unique_ptr<FileHandle> handle,
                                                           bool write) {
	auto &debug_handle = handle->Cast<DebugFileHandle>();
	auto &inner_fs_ref = debug_handle.inner->file_system;
	auto compressed = inner_fs_ref.OpenCompressedFile(context, std::move(debug_handle.inner), write);
	return make_uniq<DebugFileHandle>(*this, std::move(compressed));
}

bool DebugFileSystem::DirectoryExists(const string &directory, optional_ptr<FileOpener> opener) {
	return inner_fs->DirectoryExists(directory, opener);
}

void DebugFileSystem::CreateDirectory(const string &directory, optional_ptr<FileOpener> opener) {
	CreateDirectoryExtended(directory, {CreateDirectoryMode::SINGLE}, opener);
}

bool DebugFileSystem::CreateDirectoryExtended(const string &directory, const CreateDirectoryOptions &options,
                                              optional_ptr<FileOpener> opener) {
	return inner_fs->CreateDirectoryExtended(directory, options, opener);
}

void DebugFileSystem::CreateDirectoriesRecursive(const string &path, optional_ptr<FileOpener> opener) {
	CreateDirectoryExtended(path, {CreateDirectoryMode::RECURSIVE}, opener);
}

void DebugFileSystem::RemoveDirectory(const string &directory, optional_ptr<FileOpener> opener) {
	RemoveDirectoryExtended(directory, {RemoveDirectoryMode::RECURSIVE}, opener);
}

bool DebugFileSystem::RemoveDirectoryExtended(const string &directory, const RemoveDirectoryOptions &options,
                                              optional_ptr<FileOpener> opener) {
	return inner_fs->RemoveDirectoryExtended(directory, options, opener);
}

void DebugFileSystem::MoveFile(const string &source, const string &target, optional_ptr<FileOpener> opener) {
	inner_fs->MoveFile(source, target, opener);
}

bool DebugFileSystem::FileExists(const string &filename, optional_ptr<FileOpener> opener) {
	return inner_fs->FileExists(filename, opener);
}

bool DebugFileSystem::IsPipe(const string &filename, optional_ptr<FileOpener> opener) {
	return inner_fs->IsPipe(filename, opener);
}

void DebugFileSystem::RemoveFile(const string &filename, optional_ptr<FileOpener> opener) {
	inner_fs->RemoveFile(filename, opener);
}

bool DebugFileSystem::TryRemoveFile(const string &filename, optional_ptr<FileOpener> opener) {
	return inner_fs->TryRemoveFile(filename, opener);
}

void DebugFileSystem::RemoveFiles(const vector<string> &filenames, optional_ptr<FileOpener> opener) {
	inner_fs->RemoveFiles(filenames, opener);
}

string DebugFileSystem::PathSeparator(const string &path) {
	return inner_fs->PathSeparator(path);
}

vector<OpenFileInfo> DebugFileSystem::Glob(const string &path, FileOpener *opener) {
	return inner_fs->Glob(path, opener);
}

void DebugFileSystem::RegisterSubSystem(unique_ptr<FileSystem> sub_fs) {
	inner_fs->RegisterSubSystem(std::move(sub_fs));
}

void DebugFileSystem::RegisterCompressionFilesystem(unique_ptr<CompressedFileSystem> fs) {
	inner_fs->RegisterCompressionFilesystem(std::move(fs));
}

void DebugFileSystem::UnregisterSubSystem(const string &name) {
	inner_fs->UnregisterSubSystem(name);
}

unique_ptr<FileSystem> DebugFileSystem::ExtractSubSystem(const string &name) {
	return inner_fs->ExtractSubSystem(name);
}

void DebugFileSystem::SetDisabledFileSystems(const vector<string> &names) {
	inner_fs->SetDisabledFileSystems(names);
}

bool DebugFileSystem::SubSystemIsDisabled(const string &name) {
	return inner_fs->SubSystemIsDisabled(name);
}

bool DebugFileSystem::IsDisabledForPath(const string &path) {
	return inner_fs->IsDisabledForPath(path);
}

vector<string> DebugFileSystem::ListSubSystems() {
	return inner_fs->ListSubSystems();
}

string DebugFileSystem::GetHomeDirectory() {
	return inner_fs->GetHomeDirectory();
}

string DebugFileSystem::ExpandPath(const string &path) {
	return inner_fs->ExpandPath(path);
}

unique_ptr<MemoryMappedFile> DebugFileSystem::MemoryMapFile(const OpenFileInfo &path, FileOpenFlags flags,
                                                            const MMapOptions &options,
                                                            optional_ptr<FileOpener> opener) {
	return inner_fs->MemoryMapFile(path, flags, options, opener);
}

bool DebugFileSystem::SupportsOpenFileExtended() const {
	return true;
}

bool DebugFileSystem::ListFilesExtended(const string &directory,
                                        const std::function<void(OpenFileInfo &info)> &callback,
                                        optional_ptr<FileOpener> opener) {
	return inner_fs->ListFiles(directory, callback, opener);
}

bool DebugFileSystem::SupportsListFilesExtended() const {
	return true;
}

unique_ptr<MultiFileList> DebugFileSystem::GlobFilesExtended(const string &path, const FileGlobInput &input,
                                                             optional_ptr<FileOpener> opener) {
	return inner_fs->Glob(path, input, opener);
}

bool DebugFileSystem::SupportsGlobExtended() const {
	return true;
}

string DebugFileSystem::CanonicalizePath(const string &path_p, optional_ptr<FileOpener> opener) {
	return inner_fs->CanonicalizePath(path_p, opener);
}

} // namespace duckdb
