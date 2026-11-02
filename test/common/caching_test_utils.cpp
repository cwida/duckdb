#include "caching_test_utils.hpp"

#include "duckdb/common/algorithm.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

CachingTestFileGuard::CachingTestFileGuard(const string &filename, const string &content)
    : file_path(TestCreatePath(filename)) {
	auto local_fs = FileSystem::CreateLocal();
	auto handle = local_fs->OpenFile(file_path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE);
	handle->Write(QueryContext(), const_cast<char *>(content.data()), content.size(), 0);
	handle->Sync();
}

CachingTestFileGuard::~CachingTestFileGuard() {
	auto local_fs = FileSystem::CreateLocal();
	local_fs->TryRemoveFile(file_path);
}

const string &CachingTestFileGuard::GetPath() const {
	return file_path;
}

DuckDB MakeCacheLocalFilesDB() {
	DBConfig config;
	config.SetOptionByName("cache_local_files", true);
	return DuckDB(":memory:", &config);
}

string SimpleTrackingFileSystem::GetName() const {
	return "TrackingFileSystem";
}

bool SimpleTrackingFileSystem::CanHandleFile(const string &path) {
	return StringUtil::StartsWith(path, TestDirectoryPath());
}

bool SimpleTrackingFileSystem::CanSeek() {
	return true;
}

FileMetadata SimpleTrackingFileSystem::Stats(FileHandle &handle) {
	auto metadata = LocalFileSystem::Stats(handle);
	metadata.version_tag = StringUtil::Format("%lld:%lld", metadata.file_size, metadata.last_modification_time.value);
	return metadata;
}

unique_ptr<FileHandle> SimpleTrackingFileSystem::OpenFile(const string &path, FileOpenFlags flags,
                                                          optional_ptr<FileOpener> opener) {
	if (!StringUtil::StartsWith(path, CACHING_TEST_REMOTE_PREFIX)) {
		return LocalFileSystem::OpenFile(path, flags, opener);
	}
	auto handle = LocalFileSystem::OpenFile(path.substr(CACHING_TEST_REMOTE_PREFIX.size()), flags, opener);
	handle->path = path;
	return handle;
}

void SimpleTrackingFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	{
		annotated_lock_guard<annotated_mutex> guard(reads_lock);
		reads.emplace_back(location, NumericCast<idx_t>(nr_bytes));
	}
	LocalFileSystem::Read(handle, buffer, nr_bytes, location);
}

vector<pair<idx_t, idx_t>> SimpleTrackingFileSystem::TakeReads() {
	annotated_lock_guard<annotated_mutex> guard(reads_lock);
	auto result = std::move(reads);
	reads.clear();
	std::sort(result.begin(), result.end());
	return result;
}

string NoValidationMetadataFileSystem::GetName() const {
	return "NoValidationMetadataFileSystem";
}

bool NoValidationMetadataFileSystem::CanHandleFile(const string &path) {
	return StringUtil::StartsWith(path, TestDirectoryPath());
}

bool NoValidationMetadataFileSystem::CanSeek() {
	return true;
}

FileMetadata NoValidationMetadataFileSystem::Stats(FileHandle &handle) {
	auto metadata = LocalFileSystem::Stats(handle);
	metadata.last_modification_time = timestamp_t(0);
	metadata.version_tag.clear();
	return metadata;
}

string FreshnessOnlyFileSystem::GetName() const {
	return "FreshnessOnlyFileSystem";
}

FileMetadata FreshnessOnlyFileSystem::Stats(FileHandle &handle) {
	auto metadata = NoValidationMetadataFileSystem::Stats(handle);
	metadata.cache_valid_until = timestamp_t(Timestamp::GetCurrentTimestamp().value + max_age_micros);
	return metadata;
}

} // namespace duckdb
