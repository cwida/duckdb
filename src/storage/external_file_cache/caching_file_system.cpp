#include "duckdb/storage/external_file_cache/caching_file_system.hpp"

#include "duckdb/common/checksum.hpp"
#include "duckdb/common/chrono.hpp"
#include "duckdb/common/enums/cache_validation_mode.hpp"
#include "duckdb/common/enums/destroy_buffer_upon.hpp"
#include "duckdb/common/enums/memory_tag.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parallel/task_executor.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/storage/buffer/block_handle.hpp"
#include "duckdb/storage/buffer/buffer_pool.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/external_file_cache/external_file_cache.hpp"
#include "duckdb/storage/external_file_cache/external_file_cache_util.hpp"

namespace duckdb {

// Forward declaration.
class DatabaseInstance;

namespace {

// Destroy the uncached buffer and its metadata on unpin; its allocation can still be reused.
BufferHandle AllocateUncachedReadBuffer(BufferManager &buffer_manager, idx_t size) {
	BufferHandle buffer;
	const auto header_size = Storage::DEFAULT_BLOCK_HEADER_SIZE;
	if (size >= 256ULL * 1024 && size <= 64ULL * 1024 * 1024 - header_size) {
		const auto capacity = NextPowerOfTwo(size + header_size);
		const auto limit = buffer_manager.GetMaxMemory();
		const auto used = buffer_manager.GetBufferPool().GetUsedMemory(false);
		const auto available = limit - MinValue(limit, used);
		auto &scheduler = TaskScheduler::GetScheduler(buffer_manager.GetDatabase());
		const auto workers = MaxValue<idx_t>(1, scheduler.NumberOfThreads() + scheduler.NumberOfAsyncThreads());
		// Leave room for a second read per worker before rounding pinned buffers.
		// Changes to rounding should be tested with ClickBench on Parquet on a 192-core machine.
		if (capacity <= available / workers / 2) {
			try {
				buffer = buffer_manager.Allocate(MemoryTag::EXTERNAL_FILE_CACHE, capacity - header_size);
			} catch (const OutOfMemoryException &) {
				// Concurrent allocations may have consumed the headroom.
			}
		}
	}
	if (!buffer.IsValid()) {
		buffer = buffer_manager.Allocate(MemoryTag::EXTERNAL_FILE_CACHE, size);
	}
	buffer.GetBlockHandle()->GetMemory().SetDestroyBufferUpon(DestroyBufferUpon::UNPIN);
	return buffer;
}

// IO operation is performed to stat the file.
CacheValidationInfo GetValidationInfo(FileSystem &file_system, FileHandle &handle) {
	auto stats = file_system.Stats(handle);
	CacheValidationInfo result;
	result.file_size = NumericCast<idx_t>(stats.file_size);
	result.last_modified = stats.last_modification_time;
	result.cache_valid_until = stats.cache_valid_until;
	result.version_tag = std::move(stats.version_tag);
	return result;
}

void TightenCacheDeadline(optional<timestamp_t> &cached, const optional<timestamp_t> &current) {
	if (current && (!cached || *current < *cached)) {
		cached = current;
	}
}

//===----------------------------------------------------------------------===//
// FetchBlockTask
//===----------------------------------------------------------------------===//

class FetchBlockTask : public BaseExecutorTask {
public:
	FetchBlockTask(CachingFileHandle &caching_file_handle_p, TaskExecutor &executor, QueryContext context_p,
	               BufferManager &buffer_manager_p, shared_ptr<CacheBlock> block_p, BufferHandle &result_pin_p)
	    : BaseExecutorTask(executor), caching_file_handle(caching_file_handle_p), context(context_p),
	      buffer_manager(buffer_manager_p), block(std::move(block_p)), result_pin(result_pin_p) {
	}

	void ExecuteTask() override {
		annotated_unique_lock<annotated_mutex> lk(block->mtx);

		while (true) {
			switch (block->state) {
			case CacheBlockState::LOADED: {
				auto pin = buffer_manager.Pin(block->block_handle);
				if (pin.IsValid()) {
#ifdef DEBUG
					D_ASSERT(Checksum(pin.Ptr(), block->nr_bytes) == block->checksum);
#endif
					result_pin = std::move(pin);
					return;
				}
				// Evicted by buffer manager, need to re-fetch
				block->state = CacheBlockState::EMPTY;
				continue;
			}
			case CacheBlockState::EMPTY: {
				block->state = CacheBlockState::LOADING;
				lk.unlock();

				try {
					const idx_t file_size = caching_file_handle.GetFileSize();
					const idx_t offset = block->location;
					if (offset >= file_size) {
						lk.lock();
						// If there're other workers waiting for this block, we need to reset the block to empty state
						// for another attempt.
						block->state = CacheBlockState::EMPTY;
						block->cv.notify_all();
						return;
					}
					const idx_t to_read = MinValue(block->size, file_size - offset);
					auto buf =
					    ExternalFileCache::AllocateCacheBuffer(buffer_manager, caching_file_handle.GetPath(), to_read);
					caching_file_handle.ReadAndRecord(context, buf.GetDataMutable(), to_read, offset);
					const bool share_block = !caching_file_handle.IsCacheReuseProhibited();

					lk.lock();
					if (!share_block) {
						block->nr_bytes = to_read;
						block->state = CacheBlockState::EMPTY;
						result_pin = std::move(buf);
						block->cv.notify_all();
						return;
					}
					block->block_handle = buf.GetBlockHandle();
					block->nr_bytes = to_read;
					block->state = CacheBlockState::LOADED;
#ifdef DEBUG
					block->checksum = Checksum(buf.Ptr(), to_read);
#endif
					result_pin = std::move(buf);
					block->cv.notify_all();
				} catch (std::exception &e) {
					lk.lock();
					block->state = CacheBlockState::IO_ERROR;
					block->cv.notify_all();
					throw;
				}
				return;
			}
			case CacheBlockState::LOADING: {
				block->cv.wait(lk,
				               [&]() DUCKDB_REQUIRES(block->mtx) { return block->state != CacheBlockState::LOADING; });
				continue;
			}
			case CacheBlockState::IO_ERROR: {
				// IO operation failed, reset the block to empty state for another attempt.
				block->state = CacheBlockState::EMPTY;
				continue;
			}
			}
		}
	}

private:
	CachingFileHandle &caching_file_handle;
	QueryContext context;
	BufferManager &buffer_manager;
	shared_ptr<CacheBlock> block;
	BufferHandle &result_pin;
};

//===----------------------------------------------------------------------===//
// ReadRangeTask
//===----------------------------------------------------------------------===//

class ReadRangeTask : public BaseExecutorTask {
public:
	ReadRangeTask(CachingFileHandle &caching_file_handle_p, TaskExecutor &executor, QueryContext context_p,
	              data_ptr_t buffer_p, idx_t nr_bytes_p, idx_t location_p)
	    : BaseExecutorTask(executor), caching_file_handle(caching_file_handle_p), context(context_p), buffer(buffer_p),
	      nr_bytes(nr_bytes_p), location(location_p) {
	}

	void ExecuteTask() override {
		caching_file_handle.ReadAndRecord(context, buffer, nr_bytes, location);
	}

private:
	CachingFileHandle &caching_file_handle;
	QueryContext context;
	data_ptr_t buffer;
	idx_t nr_bytes;
	idx_t location;
};

} // namespace

//===----------------------------------------------------------------------===//
// CachingFileSystem
//===----------------------------------------------------------------------===//

CachingFileSystem::CachingFileSystem(FileSystem &file_system_p, DatabaseInstance &db_p)
    : file_system(file_system_p), db(db_p), external_file_cache(ExternalFileCache::Get(db)) {
}

CachingFileSystem::~CachingFileSystem() {
}

CachingFileSystem CachingFileSystem::Get(ClientContext &context) {
	return CachingFileSystem(FileSystem::GetFileSystem(context), *context.db);
}

unique_ptr<CachingFileHandle> CachingFileSystem::OpenFile(const OpenFileInfo &path, const FileOpenFlags &flags,
                                                          optional_ptr<FileOpener> opener) {
	return make_uniq<CachingFileHandle>(QueryContext(), *this, path, flags, opener);
}

unique_ptr<CachingFileHandle> CachingFileSystem::OpenFile(QueryContext context, const OpenFileInfo &path,
                                                          const FileOpenFlags &flags, optional_ptr<FileOpener> opener) {
	return make_uniq<CachingFileHandle>(context, *this, path, flags, opener);
}

//===----------------------------------------------------------------------===//
// CachingFileHandle
//===----------------------------------------------------------------------===//

bool CachingFileHandle::StripForceFullDownloadIfPresent() {
	auto &extended_info_p = path.extended_info;
	if (!extended_info_p) {
		return false;
	}

	auto &extended_info = *extended_info_p;
	bool force_full_download;
	if (!extended_info.TryGetOption("force_full_download", force_full_download)) {
		return false;
	}

	//! We do have 'force_full_download' - strip it
	auto new_extended_info = make_shared_ptr<ExtendedOpenFileInfo>();
	*new_extended_info = extended_info;
	new_extended_info->options.erase("force_full_download");
	if (!new_extended_info->options.count("file_size")) {
		new_extended_info->options["file_size"] = Value::UBIGINT(GetFileSize());
	}
	path.extended_info = new_extended_info;
	return force_full_download;
}

shared_ptr<CachingFileHandle::CachedFile> CachingFileHandle::EnsureCachedFileCurrent() {
	bool needs_reopen = false;
	shared_ptr<CachedFile> current_cached_file;
	{
		annotated_lock_guard<annotated_mutex> guard(file_handle_mutex);
		if (cached_file && cached_file->generation == external_file_cache.GetGeneration()) {
			return cached_file;
		}
		needs_reopen = file_handle != nullptr;
		if (needs_reopen) {
			file_handle.reset();
		}
		cached_file = external_file_cache.GetOrCreateCachedFile(path.path);
		current_cached_file = cached_file;
	}

	if (needs_reopen) {
		GetFileHandle();
	}
	return current_cached_file;
}

bool CachingFileHandle::CanUseCache() {
	auto current_cached_file = EnsureCachedFileCurrent();
	if (!Validate()) {
		annotated_lock_guard<annotated_mutex> guard(current_cached_file->meta_lock);
		return !current_cached_file->validation_info.IsExpired();
	}
	bool has_validation_metadata;
	{
		const annotated_lock_guard<annotated_mutex> guard(file_handle_mutex);
		has_validation_metadata = ExternalFileCache::HasValidationMetadata(validation_info);
	}

	annotated_lock_guard<annotated_mutex> guard(current_cached_file->meta_lock);
	if (!current_cached_file->validation_info.cache_valid_until) {
		return has_validation_metadata;
	}
	return !current_cached_file->validation_info.IsExpired();
}

void CachingFileHandle::ReconcileCacheAfterRead(CachedFile &cached_file) {
	CacheValidationInfo current;
	{
		const annotated_lock_guard<annotated_mutex> guard(file_handle_mutex);
		current = validation_info;
	}

	const annotated_lock_guard<annotated_mutex> guard(cached_file.meta_lock);
	auto &cached = cached_file.validation_info;
	if (ExternalFileCache::IsValid(Validate(), cached, current)) {
		TightenCacheDeadline(cached.cache_valid_until, current.cache_valid_until);
		return;
	}

	cached = current;
	// Existing readers retain their pinned blocks while future reads fetch replacements.
	external_file_cache.DropBlocks(cached_file);
}

CachingFileHandle::CachingFileHandle(QueryContext context, CachingFileSystem &caching_file_system_p,
                                     const OpenFileInfo &path_p, FileOpenFlags flags_p,
                                     optional_ptr<FileOpener> opener_p)
    : context(context), file_system(caching_file_system_p.file_system), db(caching_file_system_p.db),
      external_file_cache(caching_file_system_p.external_file_cache), path(path_p), flags(std::move(flags_p)),
      opener(opener_p), validate(ExternalFileCacheUtil::GetCacheValidationMode(path_p, context.GetClientContext(),
                                                                               caching_file_system_p.db)),
      cached_file(nullptr), position(0) {
	cached_file = external_file_cache.GetOrCreateCachedFile(path_p.path);
	if (!external_file_cache.IsEnabled() || Validate()) {
		// If caching is disabled, or if we must validate cache entries, we always have to open the file
		GetFileHandle();
		return;
	}
	// If we don't have any cached blocks, we must also open the file.
	bool needs_open = false;
	{
		annotated_lock_guard<annotated_mutex> guard(cached_file->map_lock);
		needs_open = cached_file->blocks.empty();
	}
	if (needs_open) {
		GetFileHandle();
	}
	auto needs_full_download = StripForceFullDownloadIfPresent();
	if (needs_full_download) {
		Read(GetFileSize(), 0);
	}
}

CachingFileHandle::~CachingFileHandle() {
}

shared_ptr<FileHandle> CachingFileHandle::GetFileHandle() {
	annotated_lock_guard<annotated_mutex> guard(file_handle_mutex);
	if (file_handle) {
		return file_handle;
	}

	// The caching file handle can service parallel block fetch tasks against a single shared FileHandle.
	// Request parallel access on the underlying filesystem handle to avoid races in implementations that
	// require explicit opt-in for concurrent pread-style access (e.g., HTTPFS).
	auto internal_flags = flags | FileFlags::FILE_FLAGS_PARALLEL_ACCESS;
	file_handle = file_system.OpenFile(path, internal_flags, opener);
	// Snapshot the metadata with a single Stats call, avoiding repeated metadata lookups (e.g., fstat)
	validation_info = GetValidationInfo(file_system, *file_handle);

	{
		annotated_lock_guard<annotated_mutex> meta_guard(cached_file->meta_lock);
		const bool first_access = (cached_file->validation_info.file_size == 0);
		const bool refresh_expired_cache = !Validate() && cached_file->validation_info.IsExpired();
		if (first_access || Validate() || refresh_expired_cache) {
			const bool cache_is_valid =
			    !refresh_expired_cache &&
			    ExternalFileCache::IsValid(Validate(), cached_file->validation_info, validation_info);
			if (!cache_is_valid) {
				external_file_cache.DropBlocks(*cached_file);
			}
			// A successful validator check refreshes freshness. Without validators, preserve the original deadline.
			const bool revalidated = cache_is_valid && ExternalFileCache::HasValidationMetadata(validation_info);
			const auto preserved_valid_until = cached_file->validation_info.cache_valid_until;
			cached_file->validation_info = validation_info;
			if (cache_is_valid && !revalidated) {
				cached_file->validation_info.cache_valid_until = preserved_valid_until;
			}
			cached_file->can_seek = file_handle->CanSeek();
			cached_file->on_disk_file = file_handle->OnDiskFile();
		}
	}
	return file_handle;
}

Allocator &CachingFileHandle::GetBufferAllocator() const {
	return external_file_cache.GetBufferManager().GetBufferAllocator();
}

FileBufferHandleGroup CachingFileHandle::Read(const idx_t nr_bytes, const idx_t location) {
	if (nr_bytes == 0) {
		return FileBufferHandleGroup();
	}

	if (!external_file_cache.IsEnabled() || !external_file_cache.ShouldCacheFile(path.path) || !CanUseCache()) {
		auto buf = AllocateUncachedReadBuffer(external_file_cache.GetBufferManager(), nr_bytes);
		ReadUncached(buf.GetDataMutable(), nr_bytes, location);
		vector<FileBufferHandleGroup::MemoryHandle> mem_handles;
		mem_handles.push_back({std::move(buf), 0, nr_bytes});
		return FileBufferHandleGroup(std::move(mem_handles));
	}

	auto current_cached_file = EnsureCachedFileCurrent();
	const idx_t file_size = GetFileSize();
	if (location >= file_size) {
		return FileBufferHandleGroup();
	}
	const idx_t max_block_size = external_file_cache.GetCacheMaxBlockSize(current_cached_file->path);
	const idx_t min_block_size =
	    MinValue(external_file_cache.GetCacheMinBlockSize(current_cached_file->path), max_block_size);
	auto request_sizing = flags.GetRequestSizing();
	switch (Settings::Get<ExternalFileCacheRequestSizingSetting>(db)) {
	case ExternalFileCacheRequestSizing::GRID:
		request_sizing = RequestSizing::BY_CACHE;
		break;
	case ExternalFileCacheRequestSizing::EXACT:
		request_sizing = RequestSizing::BY_READER;
		break;
	case ExternalFileCacheRequestSizing::AUTO:
		break;
	}
	idx_t fetch_location = location;
	idx_t fetch_end = location + nr_bytes;
	if (file_size <= max_block_size) {
		// fetch a file that fits in one block whole, so later reads of it hit the cache
		fetch_location = 0;
		fetch_end = file_size;
	} else if (request_sizing == RequestSizing::BY_CACHE) {
		fetch_location = location - location % max_block_size;
		fetch_end = AlignValue(fetch_end, max_block_size);
	} else {
		fetch_location = location - location % min_block_size;
		fetch_end = AlignValue(fetch_end, min_block_size);
	}
	fetch_end = MinValue(fetch_end, file_size);
	auto blocks = external_file_cache.AcquireBlocks(*current_cached_file, fetch_location, fetch_end - fetch_location,
	                                                max_block_size);
	const idx_t num_blocks = blocks.size();

	// Schedule block fetch tasks for all blocks.
	vector<BufferHandle> pins(num_blocks);
	auto &scheduler = TaskScheduler::GetScheduler(db);
	TaskExecutor executor(scheduler, TaskSchedulerType::ASYNC);

	for (idx_t idx = 0; idx < num_blocks; idx++) {
		executor.ScheduleTask(make_uniq<FetchBlockTask>(
		    *this, executor, context, external_file_cache.GetBufferManager(), blocks[idx], pins[idx]));
	}
	executor.WorkOnTasks();

	// Build the handle group.
	vector<FileBufferHandleGroup::MemoryHandle> mem_handles;
	mem_handles.reserve(num_blocks);
	idx_t remaining = nr_bytes;
	for (idx_t idx = 0; idx < num_blocks && remaining > 0; idx++) {
		auto &block = *blocks[idx];
		if (block.location + block.size <= location) {
			// part of a small file fetched whole, before the requested range
			continue;
		}
		const idx_t offset_in_block = location > block.location ? location - block.location : 0;
		idx_t block_valid_bytes = 0;
		{
			annotated_lock_guard<annotated_mutex> block_guard(block.mtx);
			block_valid_bytes = block.nr_bytes;
		}
		const idx_t available_in_block =
		    (block_valid_bytes > offset_in_block) ? (block_valid_bytes - offset_in_block) : 0;
		const idx_t length = MinValue(available_in_block, remaining);
		mem_handles.push_back({std::move(pins[idx]), offset_in_block, length});
		remaining -= length;
	}

	ReconcileCacheAfterRead(*current_cached_file);

	return FileBufferHandleGroup(std::move(mem_handles));
}

void CachingFileHandle::ReadUncached(data_ptr_t buffer, idx_t nr_bytes, idx_t location) {
	const idx_t max_block_size = external_file_cache.GetCacheMaxBlockSize(path.path);
	auto &scheduler = TaskScheduler::GetScheduler(db);
	// idle worker threads also run async tasks, so the pieces are only sure to run serially without any other thread
	const bool single_threaded = scheduler.NumberOfThreads() <= 1 && scheduler.NumberOfAsyncThreads() == 0;
	if (nr_bytes <= max_block_size || !external_file_cache.ShouldCacheFile(path.path) || !CanSeek() ||
	    single_threaded) {
		ReadAndRecord(context, buffer, nr_bytes, location);
		return;
	}
	// split like cached reads, so the requests are the same whether or not the cache is enabled
	TaskExecutor executor(scheduler, TaskSchedulerType::ASYNC);
	for (idx_t offset = 0; offset < nr_bytes; offset += max_block_size) {
		executor.ScheduleTask(make_uniq<ReadRangeTask>(*this, executor, context, buffer + offset,
		                                               MinValue(max_block_size, nr_bytes - offset), location + offset));
	}
	executor.WorkOnTasks();
}

FileBufferHandleGroup CachingFileHandle::Read(idx_t &nr_bytes) {
	// If we can't seek, we can't use the cache for these calls,
	// because we won't be able to seek over any parts we skipped by reading from the cache
	if (!external_file_cache.IsEnabled() || !CanSeek() || !CanUseCache()) {
		auto buf = AllocateUncachedReadBuffer(external_file_cache.GetBufferManager(), nr_bytes);
		auto file_handle = GetFileHandle();
		// Do positional read if handle can seek, otherwise have to fallback to sequential read.
		if (file_handle->CanSeek()) {
			const auto file_size = file_handle->GetFileSize();
			nr_bytes = position >= file_size ? 0 : MinValue(nr_bytes, file_size - position);
			file_handle->Read(context, buf.GetDataMutable(), nr_bytes, position);
		} else {
			nr_bytes = NumericCast<idx_t>(file_handle->Read(context, buf.GetDataMutable(), nr_bytes));
		}
		vector<FileBufferHandleGroup::MemoryHandle> mem_handles;
		mem_handles.push_back({std::move(buf), 0, nr_bytes});
		position += nr_bytes;
		return FileBufferHandleGroup(std::move(mem_handles));
	}

	const idx_t file_size = GetFileSize();
	if (position >= file_size) {
		nr_bytes = 0;
		return {};
	}

	nr_bytes = MinValue(nr_bytes, file_size - position);
	auto group = Read(nr_bytes, position);
	position += nr_bytes;
	return group;
}

string CachingFileHandle::GetPath() const {
	return path.path;
}

idx_t CachingFileHandle::GetFileSize() {
	if (!Validate()) {
		auto current_cached_file = EnsureCachedFileCurrent();
		annotated_lock_guard<annotated_mutex> guard(current_cached_file->meta_lock);
		return current_cached_file->validation_info.file_size;
	}
	auto file_handle = GetFileHandle();
	annotated_lock_guard<annotated_mutex> guard(file_handle_mutex);
	return validation_info.file_size;
}

timestamp_t CachingFileHandle::GetLastModifiedTime() {
	if (!Validate()) {
		auto current_cached_file = EnsureCachedFileCurrent();
		annotated_lock_guard<annotated_mutex> guard(current_cached_file->meta_lock);
		return current_cached_file->validation_info.last_modified;
	}
	auto file_handle = GetFileHandle();
	annotated_lock_guard<annotated_mutex> guard(file_handle_mutex);
	return validation_info.last_modified;
}

string CachingFileHandle::GetVersionTag() {
	if (!Validate()) {
		auto current_cached_file = EnsureCachedFileCurrent();
		annotated_lock_guard<annotated_mutex> guard(current_cached_file->meta_lock);
		return current_cached_file->validation_info.version_tag;
	}
	auto file_handle = GetFileHandle();
	annotated_lock_guard<annotated_mutex> guard(file_handle_mutex);
	return validation_info.version_tag;
}

bool CachingFileHandle::Validate() const {
	if (!external_file_cache.ShouldCacheFile(path.path)) {
		// uncached files have no stale state to serve, so their metadata must always be read fresh
		return true;
	}
	switch (validate) {
	case CacheValidationMode::VALIDATE_ALL:
		return true;
	case CacheValidationMode::VALIDATE_REMOTE:
		return FileSystem::IsRemoteFile(path.path);
	case CacheValidationMode::NO_VALIDATION:
		return false;
	default:
		return true;
	}
}

bool CachingFileHandle::CanSeek() {
	if (!Validate()) {
		auto current_cached_file = EnsureCachedFileCurrent();
		annotated_lock_guard<annotated_mutex> guard(current_cached_file->meta_lock);
		return current_cached_file->can_seek;
	}
	return GetFileHandle()->CanSeek();
}

bool CachingFileHandle::IsRemoteFile() const {
	return FileSystem::IsRemoteFile(path.path);
}

bool CachingFileHandle::OnDiskFile() {
	if (!Validate()) {
		auto current_cached_file = EnsureCachedFileCurrent();
		annotated_lock_guard<annotated_mutex> guard(current_cached_file->meta_lock);
		return current_cached_file->on_disk_file;
	}
	return GetFileHandle()->OnDiskFile();
}

void ReadThroughputEstimator::AddSample(double seconds, idx_t bytes) {
	const double b = static_cast<double>(bytes);
	lock_guard<mutex> guard(lock);
	sample_count++;
	sum_bytes += b;
	sum_seconds += seconds;
	sum_bytes_sq += b * b;
	sum_bytes_seconds += b * seconds;
}

bool ReadThroughputEstimator::TryEstimate(NetworkThroughputEstimate &result) const {
	lock_guard<mutex> guard(lock);
	if (sample_count < 2) {
		return false;
	}
	const double n = static_cast<double>(sample_count);
	const double variance = sum_bytes_sq - sum_bytes * sum_bytes / n;
	if (!(variance > 0)) {
		// every read was the same size, so we cant really get the latency
		return false;
	}
	// least squares estimate of the line "total_time = intercept + slope * bytes".
	const double slope = (sum_bytes_seconds - sum_bytes * sum_seconds / n) / variance;
	const double intercept = (sum_seconds - slope * sum_bytes) / n;
	if (!(slope > 0) || !(intercept > 0)) {
		return false;
	}
	// intercept is the fixed start-up cost of a read, slope is seconds per byte so bandwidth is 1 / slope
	result.latency_seconds = intercept;
	result.bandwidth_bytes_per_s = 1.0 / slope;
	return true;
}

bool CachingFileHandle::TryGetNetworkThroughput(NetworkThroughputEstimate &result) {
	// Remote files measure throughput in their own file system; local files fit it from this handle's own reads.
	if (GetFileHandle()->TryGetNetworkThroughput(result)) {
		return true;
	}
	return throughput_estimator.TryEstimate(result);
}

void CachingFileHandle::ReadAndRecord(QueryContext context, data_ptr_t buffer, idx_t nr_bytes, idx_t location) {
	auto handle = GetFileHandle();
	if (handle->file_system.IsLocalFileSystem()) {
		// Pre-fault local read buffers before the kernel fills them.
		for (idx_t offset = 0; offset < nr_bytes; offset += 4096) {
			buffer[offset] = 0;
		}
	}
	const auto read_start = steady_clock::now();
	handle->Read(context, buffer, nr_bytes, location);
	auto cache_valid_until = handle->file_system.GetCacheValidUntil(*handle);
	if (cache_valid_until) {
		const annotated_lock_guard<annotated_mutex> guard(file_handle_mutex);
		if (file_handle == handle) {
			TightenCacheDeadline(validation_info.cache_valid_until, cache_valid_until);
		}
	}
	RecordReadThroughput(duration<double>(steady_clock::now() - read_start).count(), nr_bytes);
}

bool CachingFileHandle::IsCacheReuseProhibited() {
	const annotated_lock_guard<annotated_mutex> guard(file_handle_mutex);
	return validation_info.IsCacheReuseProhibited();
}

void CachingFileHandle::RecordReadThroughput(double total_seconds, idx_t bytes) {
	if (IsRemoteFile() || !(total_seconds > 0)) {
		return;
	}
	throughput_estimator.AddSample(total_seconds, bytes);
}

idx_t CachingFileHandle::SeekPosition() {
	return position;
}

void CachingFileHandle::Seek(idx_t location) {
	position = location;
	if (file_handle != nullptr) {
		file_handle->Seek(location);
	}
}

} // namespace duckdb
