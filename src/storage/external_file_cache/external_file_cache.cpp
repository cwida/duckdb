#include "duckdb/storage/external_file_cache/external_file_cache.hpp"

#include "duckdb/common/checksum.hpp"
#include "duckdb/common/enums/memory_tag.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/map.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/common/operator/subtract.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/buffer/block_handle.hpp"
#include "duckdb/storage/object_cache.hpp"

namespace duckdb {

namespace {

bool IsDroppedBlock(CacheBlock &block) {
	const annotated_lock_guard<annotated_mutex> block_guard(block.mtx);
	if (block.state != CacheBlockState::LOADED || !block.block_handle) {
		return false;
	}
	auto &memory = block.block_handle->GetMemory();
	return memory.IsUnloaded() && !memory.MustWriteToTemporaryFile();
}

bool IsLoadedBlock(CacheBlock &block) {
	const annotated_lock_guard<annotated_mutex> block_guard(block.mtx);
	return block.state == CacheBlockState::LOADED;
}

idx_t GapBlockCount(idx_t nr_bytes, idx_t max_block_size) {
	return (nr_bytes + max_block_size - 1) / max_block_size;
}

//! Whether the last modified timestamp is usable as a cache validator
bool HasUsableLastModified(timestamp_t last_modified) {
	return last_modified.IsFinite() && last_modified != timestamp_t(0);
}

} // namespace

bool CacheValidationInfo::IsCacheReuseProhibited() const {
	return cache_valid_until && *cache_valid_until == timestamp_t::ninfinity();
}

bool CacheValidationInfo::IsExpired() const {
	return cache_valid_until && Timestamp::GetCurrentTimestamp() >= *cache_valid_until;
}

class ExternalFileCache::ExternalFileCacheObjectCacheEntry : public ObjectCacheEntry {
public:
	ExternalFileCacheObjectCacheEntry(ExternalFileCache &cache_p, string path_p, idx_t generation_p)
	    : cache(cache_p), cached_file(make_shared_ptr<CachedFile>(std::move(path_p), generation_p,
	                                                              cache_p.GetBufferManager().GetBufferPool())) {
		cache.InsertCachedFileKey(cached_file->path);
	}

	~ExternalFileCacheObjectCacheEntry() override {
		cache.EraseCachedFileKey(cached_file->path);
	}

	static string ObjectType() {
		return "external_file_cache";
	}

	string GetObjectType() override {
		return ObjectType();
	}

	optional_idx GetEstimatedCacheMemory() const override {
		// Filepath is stored at two places: in the object cache key and in the cached file object.
		// Block metadata is reserved separately by the cached file as blocks are added and dropped.
		return cached_file->path.size() * 2 + sizeof(CachedFile);
	}

	shared_ptr<CachedFile> GetCachedFile() const {
		return cached_file;
	}

private:
	ExternalFileCache &cache;
	shared_ptr<CachedFile> cached_file;
};

idx_t ExternalFileCache::GetCacheMaxBlockSize(const string &path) const {
	auto &db = buffer_manager.GetDatabase();
	if (FileSystem::IsRemoteFile(path)) {
		return Settings::Get<ExternalFileCacheRemoteMaxBlockSizeSetting>(db);
	}
	return Settings::Get<ExternalFileCacheLocalMaxBlockSizeSetting>(db);
}

idx_t ExternalFileCache::GetCacheMinBlockSize(const string &path) const {
	if (!FileSystem::IsRemoteFile(path)) {
		return 1;
	}
	auto &db = buffer_manager.GetDatabase();
	return Settings::Get<ExternalFileCacheRemoteMinBlockSizeSetting>(db);
}

bool ExternalFileCache::ShouldCacheFile(const string &path) const {
	if (FileSystem::IsRemoteFile(path)) {
		return true;
	}
	// Local files are not cached: the OS page cache already serves repeated reads
	auto &db = buffer_manager.GetDatabase();
	return Settings::Get<CacheLocalFilesSetting>(db);
}

vector<shared_ptr<CacheBlock>> ExternalFileCache::AcquireBlocks(CachedFile &cached_file, idx_t location, idx_t nr_bytes,
                                                                idx_t max_block_size) {
	D_ASSERT(nr_bytes > 0);
	D_ASSERT(max_block_size > 0);
	const idx_t end = location + nr_bytes;
	// smaller cached blocks between two gaps are re-fetched with them when that saves a request
	const idx_t absorb_size = max_block_size / 8;

	const annotated_lock_guard<annotated_mutex> map_guard(cached_file.map_lock);
	auto &blocks = cached_file.blocks;
	// start at the block that covers `location`, if any
	auto it = blocks.upper_bound(location);
	if (it != blocks.begin()) {
		auto prev = std::prev(it);
		if (prev->first + prev->second->size > location) {
			it = prev;
		}
	}

	vector<shared_ptr<CacheBlock>> result;
	idx_t pos = location;
	while (pos < end) {
		if (it != blocks.end() && it->first <= pos) {
			if (!IsDroppedBlock(*it->second) || it->second.use_count() > 1) {
				result.push_back(it->second);
				pos = it->first + it->second->size;
				++it;
				continue;
			}
			// re-fetch a dropped block only as far as this read needs it
			it = blocks.erase(it);
		}
		// create blocks for the missing bytes up to the next cached block
		idx_t gap_end = it == blocks.end() ? end : MinValue(end, it->first);
		while (it != blocks.end() && it->first < end && it->second->size < absorb_size && IsLoadedBlock(*it->second)) {
			const idx_t block_end = it->first + it->second->size;
			const auto next = std::next(it);
			const idx_t next_gap_end = next == blocks.end() ? end : MinValue(end, next->first);
			if (block_end >= next_gap_end) {
				break;
			}
			const idx_t gap_before = gap_end - pos;
			const idx_t gap_after = next_gap_end - block_end;
			if (GapBlockCount(gap_before + it->second->size + gap_after, max_block_size) >=
			    GapBlockCount(gap_before, max_block_size) + GapBlockCount(gap_after, max_block_size)) {
				break;
			}
			it = blocks.erase(it);
			gap_end = next_gap_end;
		}
		while (pos < gap_end) {
			const idx_t size = MinValue(gap_end - pos, max_block_size);
			auto block = make_shared_ptr<CacheBlock>(pos, size);
			blocks.emplace_hint(it, pos, block);
			result.push_back(std::move(block));
			pos += size;
		}
	}
	cached_file.UpdateBlockReservation();
	return result;
}

void ExternalFileCache::DropBlocks(CachedFile &cached_file) {
	const annotated_lock_guard<annotated_mutex> map_guard(cached_file.map_lock);
	cached_file.blocks.clear();
	cached_file.content_generation++;
	cached_file.UpdateBlockReservation();
}

idx_t ExternalFileCache::GetContentGeneration(CachedFile &cached_file) {
	const annotated_lock_guard<annotated_mutex> map_guard(cached_file.map_lock);
	return cached_file.content_generation;
}

ExternalFileCache::CachedFile::CachedFile(string path_p, idx_t generation_p, BufferPool &buffer_pool)
    : path(std::move(path_p)), generation(generation_p), block_reservation(MemoryTag::OBJECT_CACHE, buffer_pool, 0) {
}

void ExternalFileCache::CachedFile::UpdateBlockReservation() {
	// The block handle is only present once a block is loaded, so this over-estimates in-flight blocks.
	static constexpr idx_t BLOCK_METADATA_SIZE = sizeof(CacheBlock) + sizeof(BlockHandle) + sizeof(BlockMemory) +
	                                             sizeof(std::pair<const idx_t, shared_ptr<CacheBlock>>);
	block_reservation.Resize(blocks.size() * BLOCK_METADATA_SIZE);
}

bool ExternalFileCache::IsValid(bool validate, const string &cached_version_tag, timestamp_t cached_last_modified,
                                const string &current_version_tag, timestamp_t current_last_modified) {
	if (!validate) {
		return true; // Assume valid
	}
	if (!current_version_tag.empty() || !cached_version_tag.empty()) {
		return cached_version_tag == current_version_tag; // Validity checked by version tag
	}
	if (cached_last_modified != current_last_modified) {
		return false; // The file has certainly been modified
	}

	// If the modified time is not assigned, we can't validate it.
	if (!current_last_modified.IsFinite() || !cached_last_modified.IsFinite()) {
		return false;
	}

	// The last modified time matches. However, we cannot blindly trust this,
	// because some file systems use a low resolution clock to set the last modified time.
	// So, we will require that the last modified time is more than 10 seconds ago.
	static constexpr int64_t LAST_MODIFIED_THRESHOLD = 10LL * 1000LL * 1000LL;
	const auto access_time = Timestamp::GetCurrentTimestamp();
	if (access_time < current_last_modified) {
		return false; // Last modified in the future?
	}
	int64_t last_modified_time;
	if (!TrySubtractOperator::Operation(access_time, current_last_modified, last_modified_time)) {
		// out of range
		return false;
	}
	return last_modified_time > LAST_MODIFIED_THRESHOLD;
}

bool ExternalFileCache::HasValidationMetadata(const CacheValidationInfo &info) {
	return !info.version_tag.empty() || HasUsableLastModified(info.last_modified);
}

bool ExternalFileCache::IsValid(bool validate, const CacheValidationInfo &cached, const CacheValidationInfo &current) {
	if (cached.IsCacheReuseProhibited() || current.IsCacheReuseProhibited()) {
		return false;
	}
	if (!validate) {
		return true; // Assume valid
	}
	if (HasValidationMetadata(cached) || HasValidationMetadata(current)) {
		return IsValid(validate, cached.version_tag, cached.last_modified, current.version_tag, current.last_modified);
	}
	// No validators at all: cached data may be served within the freshness deadline the storage backend granted
	// when the cache entry was created (e.g., HTTP Cache-Control), as long as the file size is unchanged.
	if (cached.file_size != current.file_size) {
		return false; // The file has certainly been modified
	}
	if (!cached.cache_valid_until) {
		return false; // The backend does not provide expiry information, so we cannot validate at all
	}
	return Timestamp::GetCurrentTimestamp() < *cached.cache_valid_until;
}

ExternalFileCache::ExternalFileCache(DatabaseInstance &db, bool enable_p)
    : buffer_manager(BufferManager::GetBufferManager(db)), enable(enable_p), generation(0) {
}

bool ExternalFileCache::IsEnabled() const {
	return enable;
}

void ExternalFileCache::SetEnabled(bool enable_p) {
	vector<string> keys_to_delete;
	{
		const annotated_lock_guard<annotated_mutex> guard(lock);
		if (enable == enable_p) {
			return;
		}
		enable = enable_p;
		generation++;
		if (!enable) {
			keys_to_delete.reserve(cached_file_keys.size());
			for (auto &key : cached_file_keys) {
				keys_to_delete.emplace_back(key.first);
			}
		}
	}
	DeleteObjectCacheEntries(keys_to_delete);
}

idx_t ExternalFileCache::GetGeneration() const {
	return generation;
}

vector<CachedFileInformation> ExternalFileCache::GetCachedFileInformation() const {
	vector<string> keys;
	{
		const annotated_lock_guard<annotated_mutex> files_guard(lock);
		keys.reserve(cached_file_keys.size());
		for (auto &key : cached_file_keys) {
			keys.emplace_back(key.first);
		}
	}

	auto &object_cache = buffer_manager.GetDatabase().GetObjectCache();
	vector<CachedFileInformation> result;
	for (const auto &key : keys) {
		auto entry = object_cache.GetWithTypePrefix<ExternalFileCacheObjectCacheEntry>(key);
		if (!entry) {
			continue;
		}
		auto file = entry->GetCachedFile();
		const annotated_lock_guard<annotated_mutex> map_guard(file->map_lock);
		for (const auto &block_entry : file->blocks) {
			const auto &block = *block_entry.second;

			annotated_lock_guard<annotated_mutex> block_guard(block.mtx);
			if (block.state != CacheBlockState::LOADED || !block.block_handle) {
				continue;
			}
			const idx_t location = block.location;
			const auto &memory = block.block_handle->GetMemory();
			const bool loaded = !memory.IsUnloaded();
			// An unloaded cache block is spilled if it still has a temporary file backing
			const bool spilled = !loaded && memory.MustWriteToTemporaryFile();
			result.push_back({file->path, block.nr_bytes, location, loaded, spilled});
		}
	}
	return result;
}

idx_t ExternalFileCache::GetCachedFileCount() const {
	const annotated_lock_guard<annotated_mutex> files_guard(lock);
	return cached_file_keys.size();
}

ExternalFileCache &ExternalFileCache::Get(DatabaseInstance &db) {
	return db.GetExternalFileCache();
}

ExternalFileCache &ExternalFileCache::Get(ClientContext &context) {
	return context.db->GetExternalFileCache();
}

BufferManager &ExternalFileCache::GetBufferManager() const {
	return buffer_manager;
}

BufferHandle ExternalFileCache::AllocateCacheBuffer(BufferManager &buffer_manager, const string &path, idx_t nr_bytes) {
	const bool spill = Settings::Get<ExternalFileCacheSpillSetting>(buffer_manager.GetDatabase()) &&
	                   FileSystem::IsRemoteFile(path) && buffer_manager.HasTemporaryDirectory() &&
	                   nr_bytes >= buffer_manager.GetBlockAllocSize();
	return buffer_manager.Allocate(MemoryTag::EXTERNAL_FILE_CACHE, nr_bytes, !spill);
}

void ExternalFileCache::DeleteObjectCacheEntries(const vector<string> &paths) {
	auto &object_cache = buffer_manager.GetDatabase().GetObjectCache();
	for (auto &path : paths) {
		object_cache.DeleteWithTypePrefix<ExternalFileCacheObjectCacheEntry>(path);
	}
}

shared_ptr<ExternalFileCache::CachedFile> ExternalFileCache::GetOrCreateCachedFile(const string &path) {
	auto &object_cache = buffer_manager.GetDatabase().GetObjectCache();
	while (true) {
		const auto current_generation = generation.load();
		if (!enable) {
			return make_shared_ptr<CachedFile>(path, current_generation, buffer_manager.GetBufferPool());
		}

		auto entry = object_cache.GetOrCreateWithTypePrefix<ExternalFileCacheObjectCacheEntry>(path, *this, path,
		                                                                                       current_generation);
		auto cached_file = entry->GetCachedFile();

		if (!enable) {
			object_cache.DeleteWithTypePrefix<ExternalFileCacheObjectCacheEntry>(path);
			return make_shared_ptr<CachedFile>(path, current_generation, buffer_manager.GetBufferPool());
		}
		if (cached_file->generation != current_generation) {
			object_cache.DeleteWithTypePrefix<ExternalFileCacheObjectCacheEntry>(path);
			continue;
		}
		return cached_file;
	}
}

void ExternalFileCache::InsertCachedFileKey(const string &path) {
	const annotated_lock_guard<annotated_mutex> guard(lock);
	cached_file_keys[path]++;
}

void ExternalFileCache::EraseCachedFileKey(const string &path) {
	const annotated_lock_guard<annotated_mutex> guard(lock);
	auto entry = cached_file_keys.find(path);
	ALWAYS_ASSERT(entry != cached_file_keys.end());
	D_ASSERT(entry->second > 0);
	if (--entry->second == 0) {
		cached_file_keys.erase(entry);
	}
}

} // namespace duckdb
