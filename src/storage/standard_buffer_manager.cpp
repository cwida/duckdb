#include "duckdb/storage/standard_buffer_manager.hpp"

#include "duckdb/common/allocator.hpp"
#include "duckdb/common/enums/memory_tag.hpp"
#include "duckdb/common/enums/storage_block_prefetch.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/set.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/storage/buffer/buffer_pool.hpp"
#include "duckdb/storage/in_memory_block_manager.hpp"
#include "duckdb/storage/temporary_file_manager.hpp"
#include "duckdb/storage/block_allocator.hpp"
#include "duckdb/common/encryption_functions.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parallel/callback_async_task.hpp"
#include "duckdb/storage/metadata/metadata_manager.hpp"

namespace duckdb {

#ifdef DUCKDB_DEBUG_DESTROY_BLOCKS
static void WriteGarbageIntoBuffer(BlockLock &lock, BlockHandle &block) {
	auto &buffer = block.GetMemory().GetBuffer(lock);
	if (!buffer->OwnsInternalBuffer()) {
		// don't write garbage into mmap buffers
		// this would directly be written back into the file
		return;
	}
	memset(buffer->GetDataMutable(), 0xa5, buffer->Size()); // 0xa5 is default memory in debug mode
}

static void WriteGarbageIntoBuffer(BlockHandle &block) {
	auto lock = block.GetMemory().GetLock();
	WriteGarbageIntoBuffer(lock, block);
}
#endif

struct BufferAllocatorData : PrivateAllocatorData {
	explicit BufferAllocatorData(StandardBufferManager &manager) : manager(manager) {
	}

	StandardBufferManager &manager;

	TempBufferPoolReservation Reserve(idx_t size);
	void Release(idx_t size);
	data_ptr_t Allocate(idx_t size);
	void Free(data_ptr_t pointer, idx_t size);
	data_ptr_t Reallocate(data_ptr_t pointer, idx_t old_size, idx_t size);
};

unique_ptr<FileBuffer> StandardBufferManager::ConstructManagedBuffer(idx_t size, idx_t block_header_size,
                                                                     unique_ptr<FileBuffer> &&source,
                                                                     FileBufferType type) {
	unique_ptr<FileBuffer> result;
	if (type == FileBufferType::BLOCK) {
		throw InternalException("ConstructManagedBuffer cannot be used to construct blocks");
	}
	if (source && source->OwnsInternalBuffer()) {
		auto tmp = std::move(source);
		D_ASSERT(tmp->AllocSize() == BufferManager::GetAllocSize(size + block_header_size));
		result = make_uniq<FileBuffer>(*tmp, type, block_header_size);
	} else {
		// non re-usable buffer (or mmap-backed, which we cannot rewrite): allocate a new buffer
		result = make_uniq<FileBuffer>(BlockAllocator::Get(db), type, size, block_header_size);
	}
	result->Initialize(DBConfig::GetConfig(db).options.debug_initialize);
	return result;
}

void StandardBufferManager::SetTemporaryDirectory(const string &new_dir) {
	lock_guard<mutex> guard(temporary_directory.lock);
	if (temporary_directory.handle) {
		throw NotImplementedException("Cannot switch temporary directory after the current one has been used");
	}
	temporary_directory.path = new_dir;
}

StandardBufferManager::StandardBufferManager(DatabaseInstance &db, string tmp)
    : BufferManager(), db(db), buffer_pool(db.GetBufferPool()), temporary_id(MAXIMUM_BLOCK), buffer_allocator(*this) {
	temp_block_manager =
	    make_uniq<InMemoryBlockManager>(*this, DEFAULT_BLOCK_ALLOC_SIZE, DEFAULT_BLOCK_HEADER_STORAGE_SIZE);
	temporary_directory.path = std::move(tmp);
	for (idx_t i = 0; i < MEMORY_TAG_COUNT; i++) {
		evicted_data_per_tag[i] = 0;
	}
}

StandardBufferManager::~StandardBufferManager() {
}

BufferPool &StandardBufferManager::GetBufferPool() const {
	return buffer_pool;
}

TemporaryMemoryManager &StandardBufferManager::GetTemporaryMemoryManager() {
	return buffer_pool.GetTemporaryMemoryManager();
}

idx_t StandardBufferManager::GetUsedMemory() const {
	return buffer_pool.GetUsedMemory();
}
idx_t StandardBufferManager::GetMaxMemory() const {
	return buffer_pool.GetMaxMemory();
}

idx_t StandardBufferManager::GetUsedSwap() const {
	return temporary_directory.size_on_disk.load(std::memory_order_relaxed);
}

optional_idx StandardBufferManager::GetMaxSwap() const {
	lock_guard<mutex> guard(temporary_directory.lock);
	if (!temporary_directory.handle) {
		return optional_idx();
	}
	return temporary_directory.handle->GetTempFile().GetMaxSwapSpace();
}

idx_t StandardBufferManager::GetBlockAllocSize() const {
	return temp_block_manager->GetBlockAllocSize();
}

idx_t StandardBufferManager::GetBlockSize() const {
	return temp_block_manager->GetBlockSize();
}

idx_t StandardBufferManager::GetOperatorMemoryLimit() const {
	return GetBufferPool().GetOperatorMemoryLimit();
}

template <typename... ARGS>
TempBufferPoolReservation StandardBufferManager::EvictBlocksOrThrow(QueryContext context, MemoryTag tag,
                                                                    idx_t memory_delta, unique_ptr<FileBuffer> *buffer,
                                                                    const ARGS &...args) {
	auto r = buffer_pool.EvictBlocks(context, tag, memory_delta, buffer_pool.maximum_memory, buffer);
	if (!r.success) {
		string extra_text = StringUtil::Format(" (%s/%s used)", StringUtil::BytesToHumanReadableString(GetUsedMemory()),
		                                       StringUtil::BytesToHumanReadableString(GetMaxMemory()));
		extra_text += InMemoryWarning();
		throw OutOfMemoryException(args..., extra_text);
	}
	return std::move(r.reservation);
}

shared_ptr<BlockHandle> StandardBufferManager::RegisterTransientMemory(const idx_t size, BlockManager &block_manager) {
	D_ASSERT(size <= block_manager.GetBlockSize());

	// This comparison is the reason behind passing block_size through transient memory creation.
	// Otherwise, any non-default block size would register as small memory, causing problems when
	// trying to convert that memory to consistent blocks later on.
	if (size < block_manager.GetBlockSize()) {
		return RegisterSmallMemory(MemoryTag::IN_MEMORY_TABLE, size);
	}

	auto buffer_handle = Allocate(MemoryTag::IN_MEMORY_TABLE, &block_manager, false);
	return buffer_handle.GetBlockHandle();
}

shared_ptr<BlockHandle> StandardBufferManager::RegisterSmallMemory(MemoryTag tag, const idx_t size) {
	D_ASSERT(size < GetBlockSize());
	auto reservation = EvictBlocksOrThrow(QueryContext(), tag, size, nullptr, "could not allocate block of size %s%s",
	                                      StringUtil::BytesToHumanReadableString(size));

	auto buffer = ConstructManagedBuffer(size, DEFAULT_BLOCK_HEADER_STORAGE_SIZE, nullptr, FileBufferType::TINY_BUFFER);

	// Create a new block pointer for this block.
	auto result = make_shared_ptr<BlockHandle>(*temp_block_manager, ++temporary_id, tag, std::move(buffer),
	                                           DestroyBufferUpon::BLOCK, size, std::move(reservation));
#ifdef DUCKDB_DEBUG_DESTROY_BLOCKS
	// Initialize the memory with garbage data
	WriteGarbageIntoBuffer(*result);
#endif
	return result;
}

shared_ptr<BlockHandle> StandardBufferManager::RegisterMemory(MemoryTag tag, idx_t block_size, idx_t block_header_size,
                                                              bool can_destroy, QueryContext context) {
	auto alloc_size = GetAllocSize(block_size + block_header_size);

	// Evict blocks until there is enough memory to store the buffer.
	unique_ptr<FileBuffer> reusable_buffer;
	auto res = EvictBlocksOrThrow(context, tag, alloc_size, &reusable_buffer, "could not allocate block of size %s%s",
	                              StringUtil::BytesToHumanReadableString(alloc_size));

	// Create a new buffer and a block to hold the buffer.
	const auto file_buffer_type =
	    tag == MemoryTag::EXTERNAL_FILE_CACHE ? FileBufferType::EXTERNAL_FILE : FileBufferType::MANAGED_BUFFER;
	auto buffer = ConstructManagedBuffer(block_size, block_header_size, std::move(reusable_buffer), file_buffer_type);
	const auto destroy_buffer_upon = can_destroy ? DestroyBufferUpon::EVICTION : DestroyBufferUpon::BLOCK;
	return make_shared_ptr<BlockHandle>(*temp_block_manager, ++temporary_id, tag, std::move(buffer),
	                                    destroy_buffer_upon, alloc_size, std::move(res));
}

shared_ptr<BlockHandle> StandardBufferManager::AllocateTemporaryMemory(MemoryTag tag, idx_t block_size,
                                                                       bool can_destroy) {
	return RegisterMemory(tag, block_size, Storage::DEFAULT_BLOCK_HEADER_SIZE, can_destroy);
}

shared_ptr<BlockHandle> StandardBufferManager::AllocateMemory(MemoryTag tag, BlockManager *block_manager,
                                                              bool can_destroy) {
	return RegisterMemory(tag, block_manager->GetBlockSize(), block_manager->GetBlockHeaderSize(), can_destroy);
}

BufferHandle StandardBufferManager::Allocate(MemoryTag tag, BlockManager *block_manager, bool can_destroy) {
	auto block = AllocateMemory(tag, block_manager, can_destroy);

#ifdef DUCKDB_DEBUG_DESTROY_BLOCKS
	// Initialize the memory with garbage data
	WriteGarbageIntoBuffer(*block);
#endif
	return Pin(block);
}

BufferHandle StandardBufferManager::Allocate(MemoryTag tag, idx_t block_size, bool can_destroy) {
	auto block = AllocateTemporaryMemory(tag, block_size, can_destroy);

#ifdef DUCKDB_DEBUG_DESTROY_BLOCKS
	// Initialize the memory with garbage data
	WriteGarbageIntoBuffer(*block);
#endif
	return Pin(block);
}

BufferHandle StandardBufferManager::Allocate(QueryContext context, MemoryTag tag, idx_t block_size, bool can_destroy) {
	auto block = RegisterMemory(tag, block_size, Storage::DEFAULT_BLOCK_HEADER_SIZE, can_destroy, context);

#ifdef DUCKDB_DEBUG_DESTROY_BLOCKS
	// Initialize the memory with garbage data
	WriteGarbageIntoBuffer(*block);
#endif
	return Pin(context, block);
}

BufferHandle StandardBufferManager::Allocate(QueryContext context, MemoryTag tag, BlockManager *block_manager,
                                             bool can_destroy) {
	auto block =
	    RegisterMemory(tag, block_manager->GetBlockSize(), block_manager->GetBlockHeaderSize(), can_destroy, context);

#ifdef DUCKDB_DEBUG_DESTROY_BLOCKS
	// Initialize the memory with garbage data
	WriteGarbageIntoBuffer(*block);
#endif
	return Pin(context, block);
}

void StandardBufferManager::BatchRead(QueryContext context, PrefetchRun &run) {
	idx_t block_count = run.handles.size();

	// Allocate a buffer to hold the data of all blocks.
	auto block_alloc_size = run.handles[0]->GetBlockAllocSize();
	auto total_block_size = block_count * block_alloc_size;
	// prefetching is best effort, on memory pressure the run is left for the scan to pin its blocks on demand
	unique_ptr<FileBuffer> staging_reuse;
	auto staging_result = buffer_pool.EvictBlocks(context, MemoryTag::BASE_TABLE, GetAllocSize(total_block_size),
	                                              buffer_pool.maximum_memory, &staging_reuse);
	if (!staging_result.success) {
		return;
	}
	// the reservation is held for the staging buffer's lifetime, released when this function returns
	auto intermediate_buffer =
	    ConstructManagedBuffer(total_block_size, 0, std::move(staging_reuse), FileBufferType::MANAGED_BUFFER);

	// perform a batch read of the blocks into the buffer
	auto &block_manager = run.handles[0]->GetBlockManager();
	block_manager.ReadBlocks(context, *intermediate_buffer, run.first_block, block_count);

	// the blocks are read, now assign them to the individual block handles
	for (idx_t block_idx = 0; block_idx < block_count; block_idx++) {
		auto &handle = run.handles[block_idx];
		D_ASSERT(handle->BlockId() == run.first_block + NumericCast<block_id_t>(block_idx));

		// reserve memory for the block
		auto &block_memory = handle->GetMemory();
		idx_t required_memory = block_memory.GetMemoryUsage();
		unique_ptr<FileBuffer> reusable_buffer;
		auto block_result = buffer_pool.EvictBlocks(context, block_memory.GetMemoryTag(), required_memory,
		                                            buffer_pool.maximum_memory, &reusable_buffer);
		if (!block_result.success) {
			// the remaining blocks do not fit next to what the pool holds, the scan pins them on demand
			return;
		}
		// load the block, the handle is not kept, the scan pins it again before it can be evicted
		BufferHandle buf;
		{
			auto lock = block_memory.GetLock();
			if (block_memory.GetState() == BlockState::BLOCK_LOADED) {
				// the block is loaded already by another thread, free up the reservation and continue
				block_result.reservation.Resize(0);
				continue;
			}
			auto block_ptr = intermediate_buffer->InternalBuffer() + block_idx * block_alloc_size;
			buf = handle->LoadFromBuffer(lock, block_ptr, std::move(reusable_buffer),
			                             std::move(block_result.reservation));
		}
	}
}

vector<StandardBufferManager::PrefetchRun>
StandardBufferManager::RegisterPrefetch(vector<shared_ptr<BlockHandle>> &handles) {
	// figure out which set of blocks we should load
	map<block_id_t, shared_ptr<BlockHandle>> to_be_loaded;
	for (auto &handle : handles) {
		if (handle->GetMemory().GetState() != BlockState::BLOCK_LOADED) {
			to_be_loaded.insert(make_pair(handle->BlockId(), handle));
		}
	}
	vector<PrefetchRun> plan;
	if (to_be_loaded.empty()) {
		return plan;
	}
	// each run is read with a single pread into one staging buffer, cap the read size
	static constexpr idx_t MAX_PREFETCH_RUN_SIZE = 32ULL * 1024 * 1024;
	const idx_t block_size = to_be_loaded.begin()->second->GetBlockAllocSize();
	for (auto it = to_be_loaded.begin(); it != to_be_loaded.end(); ++it) {
		auto &entry = *it;
		bool new_run = plan.empty() ||
		               plan.back().first_block + NumericCast<block_id_t>(plan.back().handles.size()) != entry.first;
		if (!new_run) {
			const auto run_size = plan.back().handles.size() * block_size;
			auto next = std::next(it);
			// a lone trailing block is not worth a run of its own, let a run within the cap grow by it instead
			const bool last_of_region = next == to_be_loaded.end() || next->first != entry.first + 1;
			new_run =
			    run_size + block_size > MAX_PREFETCH_RUN_SIZE && !(last_of_region && run_size <= MAX_PREFETCH_RUN_SIZE);
		}
		if (new_run) {
			// the block is not adjacent to the previous block or the run is full, start a new run
			plan.push_back(PrefetchRun {entry.first, {}});
		}
		plan.back().handles.push_back(std::move(entry.second));
	}
	return plan;
}

void StandardBufferManager::ExecutePrefetch(QueryContext context, vector<PrefetchRun> &plan) {
	for (auto &run : plan) {
		if (run.handles.size() == 1 &&
		    Settings::Get<StorageBlockPrefetchSetting>(db) != StorageBlockPrefetch::DEBUG_FORCE_ALWAYS) {
			// skip runs of a single block unless debug_force_always is set, a single read cannot be batched
			continue;
		}
		BatchRead(context, run);
	}
}

void StandardBufferManager::Prefetch(QueryContext context, vector<shared_ptr<BlockHandle>> &handles) {
	auto plan = RegisterPrefetch(handles);
	ExecutePrefetch(context, plan);
}

vector<unique_ptr<AsyncTask>> StandardBufferManager::CreatePrefetchTasks(QueryContext context,
                                                                         vector<shared_ptr<BlockHandle>> &handles) {
	auto plan = RegisterPrefetch(handles);
	vector<unique_ptr<AsyncTask>> tasks;
	tasks.reserve(plan.size());
	for (auto &run : plan) {
		// the io size is the number of bytes this run will read
		auto io_size = run.handles.size() * run.handles[0]->GetBlockAllocSize();
		tasks.push_back(make_uniq<CallbackAsyncTask>(
		    [this, context, run = std::move(run)]() mutable { BatchRead(context, run); }, io_size));
	}
	return tasks;
}

BufferHandle StandardBufferManager::Pin(shared_ptr<BlockHandle> &handle) {
	return Pin(QueryContext(), handle);
}

BufferHandle StandardBufferManager::Pin(const QueryContext &context, shared_ptr<BlockHandle> &handle) {
	// we need to be careful not to return the BufferHandle to this block while holding the BlockHandle's lock
	// as exiting this function's scope may cause the destructor of the BufferHandle to be called while holding the lock
	// the destructor calls Unpin, which grabs the BlockHandle's lock again, causing a deadlock
	BufferHandle buf;

	idx_t required_memory;
	auto &block_memory = handle->GetMemory();
	if (block_memory.TryIncrementPinnedReaders()) {
		return BufferHandle(handle, block_memory.GetBuffer());
	}
	{
		// lock the block
		auto lock = block_memory.GetLock();
		// check if the block is already loaded
		if (block_memory.GetState() == BlockState::BLOCK_LOADED) {
			// the block is loaded, increment the reader count and set the BufferHandle
			buf = handle->Load(lock, context);
		}
		required_memory = block_memory.GetMemoryUsage();
	}

	if (buf.IsValid()) {
		return buf; // the block was already loaded, return it without holding the BlockHandle's lock
	}

	// evict blocks until we have space for the current block
	unique_ptr<FileBuffer> reusable_buffer;
	auto reservation =
	    EvictBlocksOrThrow(context, block_memory.GetMemoryTag(), required_memory, &reusable_buffer,
	                       "failed to pin block of size %s%s", StringUtil::BytesToHumanReadableString(required_memory));

	// lock the handle again and repeat the check (in case anybody loaded in the meantime)
	auto lock = block_memory.GetLock();
	// check if the block is already loaded
	if (block_memory.GetState() == BlockState::BLOCK_LOADED) {
		// the block is loaded, increment the reader count and return a pointer to the handle
		reservation.Resize(0);
		buf = handle->Load(lock, context);
	} else {
		// now we can actually load the current block
		D_ASSERT(block_memory.GetReaders() == 0);
		buf = handle->Load(lock, context, std::move(reusable_buffer));
		if (!buf.IsValid()) {
			reservation.Resize(0);
			return buf; // Buffer was destroyed (e.g., due to DestroyBufferUpon::Eviction)
		}
		auto &memory_charge = block_memory.GetMemoryCharge(lock);
		memory_charge = std::move(reservation);
		// in the case of a variable sized block, the buffer may be smaller than a full block.
		int64_t delta = NumericCast<int64_t>(block_memory.GetBuffer(lock)->AllocSize()) -
		                NumericCast<int64_t>(block_memory.GetMemoryUsage());
		if (delta) {
			block_memory.ChangeMemoryUsage(lock, delta);
		}
		D_ASSERT(block_memory.GetMemoryUsage() == block_memory.GetBuffer(lock)->AllocSize());
	}

	// we should have a valid BufferHandle by now, either because the block was already loaded, or because we loaded it
	// return it without holding the BlockHandle's lock
	D_ASSERT(buf.IsValid());
	return buf;
}

void StandardBufferManager::PurgeQueue(const BlockHandle &handle) {
	buffer_pool.PurgeQueue(handle);
}

void StandardBufferManager::AddToEvictionQueue(shared_ptr<BlockHandle> &handle) {
	auto lock = handle->GetMemory().GetLock();
	buffer_pool.AddToEvictionQueue(lock, handle);
}

void StandardBufferManager::VerifyZeroReaders(BlockLock &lock, shared_ptr<BlockHandle> &handle) {
#ifdef DUCKDB_DEBUG_DESTROY_BLOCKS
	unique_ptr<FileBuffer> replacement_buffer;
	auto &block_allocator = BlockAllocator::Get(db);
	auto &buffer = handle->GetMemory().GetBuffer(lock);
	auto block_header_size = buffer->GetHeaderSize();
	auto alloc_size = buffer->AllocSize() - block_header_size;
	if (handle->GetMemory().GetBufferType() == FileBufferType::BLOCK) {
		auto block = reinterpret_cast<Block *>(buffer.get());
		replacement_buffer = make_uniq<Block>(block_allocator, block->id, alloc_size, block_header_size);
	} else {
		replacement_buffer =
		    make_uniq<FileBuffer>(block_allocator, buffer->GetBufferType(), alloc_size, block_header_size);
	}
	memcpy(replacement_buffer->GetDataMutable(), buffer->GetData(), buffer->Size());
	WriteGarbageIntoBuffer(lock, *handle);
	buffer = std::move(replacement_buffer);
#endif
}

void StandardBufferManager::Unpin(shared_ptr<BlockHandle> &handle) {
	bool purge = false;
	auto &block_memory = handle->GetMemory();
	if (block_memory.TryDecrementPinnedReaders()) {
		return;
	}
	{
		auto lock = block_memory.GetLock();
		D_ASSERT(block_memory.GetReaders() > 0);
		auto new_readers = block_memory.DecrementReaders(lock);
		// Tiny buffers are never evicted, and a block converted to a persistent block has no buffer left
		if (new_readers == 0 && block_memory.GetBuffer(lock) &&
		    block_memory.GetBufferType() != FileBufferType::TINY_BUFFER) {
			VerifyZeroReaders(lock, handle);
			if (block_memory.MustAddToEvictionQueue()) {
				purge = buffer_pool.AddToEvictionQueue(lock, handle);
			} else {
				block_memory.Unload(lock);
			}
		}
	}

	// We do not have to keep the handle locked while purging.
	if (purge) {
		PurgeQueue(*handle);
	}
}

void StandardBufferManager::SetMemoryLimit(idx_t limit) {
	buffer_pool.SetLimit(limit, InMemoryWarning());
}

void StandardBufferManager::SetSwapLimit(optional_idx limit) {
	lock_guard<mutex> guard(temporary_directory.lock);
	if (temporary_directory.handle) {
		temporary_directory.handle->GetTempFile().SetMaxSwapSpace(limit);
	} else {
		temporary_directory.maximum_swap_space = limit;
	}
}

vector<MemoryInformation> StandardBufferManager::GetMemoryUsageInfo() const {
	vector<MemoryInformation> result;
	for (idx_t k = 0; k < MEMORY_TAG_COUNT; k++) {
		MemoryInformation info;
		info.tag = MemoryTag(k);
		info.size = buffer_pool.memory_usage.GetUsedMemory(MemoryTag(k), BufferPool::MemoryUsageCaches::FLUSH);
		info.evicted_data = evicted_data_per_tag[k].load();
		result.push_back(info);
	}
	return result;
}

string StandardBufferManager::GetTemporaryPath(block_id_t id) {
	auto &fs = FileSystem::GetFileSystem(db);
	return fs.JoinPath(temporary_directory.path,
	                   temporary_directory.handle->GetFilePrefix() + "block-" + to_string(id) + ".block");
}

void StandardBufferManager::RequireTemporaryDirectory() {
	if (temporary_directory.initialized.load(std::memory_order_acquire)) {
		return;
	}
	lock_guard<mutex> guard(temporary_directory.lock);
	if (temporary_directory.handle) {
		return;
	}
	if (temporary_directory.path.empty()) {
		throw InvalidInputException(
		    "Out-of-memory: cannot write buffer because no temporary directory is specified!\nTo enable "
		    "temporary buffer eviction set a temporary directory using PRAGMA temp_directory='/path/to/tmp.tmp'");
	}
	temporary_directory.handle = make_uniq<TemporaryDirectoryHandle>(
	    db, temporary_directory.path, temporary_directory.size_on_disk, temporary_directory.maximum_swap_space);
	temporary_directory.initialized.store(true, std::memory_order_release);
}

bool StandardBufferManager::EncryptTemporaryFiles() {
	return Settings::Get<TempFileEncryptionSetting>(db);
}

void StandardBufferManager::WriteTemporaryBuffer(QueryContext context, MemoryTag tag, block_id_t block_id,
                                                 FileBuffer &buffer) {
	// WriteTemporaryBuffer assumes that we never write a buffer below DEFAULT_BLOCK_ALLOC_SIZE.
	RequireTemporaryDirectory();

	// Append to a few grouped files.
	if (buffer.AllocSize() == GetBlockAllocSize()) {
		idx_t eviction_size = temporary_directory.handle->GetTempFile().WriteTemporaryBuffer(context, block_id, buffer);
		evicted_data_per_tag[uint8_t(tag)] += eviction_size;
		if (context.GetClientContext()) {
			QueryProfiler::Get(*context.GetClientContext()).TrackBytesSpilled(eviction_size);
		}
		return;
	}

	// Get the path to write to.
	// These files are .block, and variable sized (larger then 256kb), as opposed to .tmp
	auto path = GetTemporaryPath(block_id);

	idx_t header_size = sizeof(idx_t) * 2;
	if (EncryptTemporaryFiles()) {
		header_size += DEFAULT_ENCRYPTED_BUFFER_HEADER_SIZE;
	}

	// Create the file and write the size followed by the buffer contents.
	auto &fs = FileSystem::GetFileSystem(db);
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE);
	bool size_on_disk_increased = false;
	try {
		temporary_directory.handle->GetTempFile().IncreaseSizeOnDisk(buffer.AllocSize() + header_size);
		size_on_disk_increased = true;
		//! for very large buffers, we store the size of the buffer in plaintext.
		idx_t block_header_size = buffer.GetHeaderSize();
		auto user_size = buffer.Size();
		handle->Write(context, &user_size, sizeof(idx_t), 0);
		handle->Write(context, &block_header_size, sizeof(idx_t), sizeof(idx_t));

		idx_t offset = sizeof(idx_t) * 2;

		if (EncryptTemporaryFiles()) {
			uint8_t encryption_metadata[DEFAULT_ENCRYPTED_BUFFER_HEADER_SIZE];
			EncryptionEngine::EncryptTemporaryBuffer(db, buffer.InternalBuffer(), buffer.AllocSize(),
			                                         encryption_metadata);
			handle->Write(context, encryption_metadata, DEFAULT_ENCRYPTED_BUFFER_HEADER_SIZE, offset);
			offset += DEFAULT_ENCRYPTED_BUFFER_HEADER_SIZE;
		}

		buffer.Write(context, *handle, offset);
	} catch (...) {
		// roll back the accounting and remove the partially written file
		if (size_on_disk_increased) {
			temporary_directory.handle->GetTempFile().DecreaseSizeOnDisk(buffer.AllocSize() + header_size);
		}
		handle.reset();
		fs.RemoveFile(path);
		throw;
	}
	evicted_data_per_tag[uint8_t(tag)] += buffer.AllocSize();
	if (context.GetClientContext()) {
		QueryProfiler::Get(*context.GetClientContext()).TrackBytesSpilled(buffer.AllocSize() + header_size);
	}
}

unique_ptr<FileBuffer> StandardBufferManager::ReadTemporaryBuffer(QueryContext context, MemoryTag tag,
                                                                  BlockHandle &block,
                                                                  unique_ptr<FileBuffer> reusable_buffer) {
	D_ASSERT(!temporary_directory.path.empty());
	auto id = block.BlockId();
	if (!temporary_directory.handle) {
		throw InternalException("ReadTemporaryBuffer called but temporary directory has not been instantiated yet");
	}
	if (temporary_directory.handle->GetTempFile().HasTemporaryBuffer(id)) {
		// This is a block that was offloaded to a regular .tmp file, the file contains blocks of a fixed size

		idx_t eviction_size = 0;
		auto buffer = temporary_directory.handle->GetTempFile().ReadTemporaryBuffer(
		    context, id, std::move(reusable_buffer), &eviction_size);

		// Decrement evicted size.
		evicted_data_per_tag[uint8_t(tag)] -= eviction_size;

		return buffer;
	}

	// This block contains data of variable size so we need to open it and read it to get its size.
	idx_t block_size;
	idx_t block_header_size;
	auto path = GetTemporaryPath(id);
	auto &fs = FileSystem::GetFileSystem(db);
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	handle->Read(context, &block_size, sizeof(idx_t), 0);
	handle->Read(context, &block_header_size, sizeof(idx_t), sizeof(idx_t));

	idx_t offset = sizeof(idx_t) * 2;
	if (EncryptTemporaryFiles()) {
		offset += DEFAULT_ENCRYPTED_BUFFER_HEADER_SIZE;
	}

	// block_size and block_header_size are read off disk: validate them against the physical file before
	// using them to size an allocation or to offset into the buffer. WriteTemporaryBuffer lays out the file
	// as [size][header_size][optional nonce/tag][payload], where the payload is GetAllocSize(header + size).
	auto file_size = handle->GetFileSize();
	if (block_header_size > file_size || block_size > file_size) {
		throw IOException(
		    "Corrupt temporary file \"%s\": block header size (%llu) or block size (%llu) exceeds file size %llu", path,
		    block_header_size, block_size, file_size);
	}
	if (offset + GetAllocSize(block_header_size + block_size) != file_size) {
		throw IOException("Corrupt temporary file \"%s\": header describes %llu bytes but file is %llu", path,
		                  offset + GetAllocSize(block_header_size + block_size), file_size);
	}

	// Allocate a buffer of the file's size and read the data into that buffer.
	auto buffer = ConstructManagedBuffer(block_size, block_header_size, std::move(reusable_buffer),
	                                     block.GetMemory().GetBufferType());

	if (EncryptTemporaryFiles()) {
		// encrypted: the nonce/tag sit between the two size words and the payload (which starts at offset)
		//! Read nonce and tag from file.
		uint8_t encryption_metadata[DEFAULT_ENCRYPTED_BUFFER_HEADER_SIZE];
		handle->Read(context, encryption_metadata, DEFAULT_ENCRYPTED_BUFFER_HEADER_SIZE, sizeof(idx_t) * 2);

		//! Read and decrypt the buffer.
		buffer->Read(context, *handle, offset);
		EncryptionEngine::DecryptTemporaryBuffer(GetDatabase(), buffer->InternalBuffer(), buffer->AllocSize(),
		                                         encryption_metadata);
	} else {
		// unencrypted: read the data directly
		buffer->Read(context, *handle, offset);
	}

	handle.reset();

	// Delete the file and return the buffer.
	// DeleteTemporaryFile already decrements evicted_data_per_tag for the .block path; do not
	// decrement again here or the counter underflows on every read-back.
	DeleteTemporaryFile(block.GetMemory());

	return buffer;
}

void StandardBufferManager::DeleteTemporaryFile(BlockMemory &memory) {
	// Without spill storage, there are no temporary buffers to delete.
	if (!memory.IsUnloaded() || GetUsedSwap() == 0) {
		return;
	}
	auto id = memory.BlockId();
	if (temporary_directory.path.empty()) {
		// no temporary directory specified: nothing to delete
		return;
	}
	{
		lock_guard<mutex> guard(temporary_directory.lock);
		if (!temporary_directory.handle) {
			// temporary directory was not initialized yet: nothing to delete
			return;
		}
	}

	// check if we should delete the file from the shared pool of files, or from the general file system
	if (temporary_directory.handle->GetTempFile().HasTemporaryBuffer(id)) {
		idx_t eviction_size = temporary_directory.handle->GetTempFile().DeleteTemporaryBuffer(id);
		evicted_data_per_tag[uint8_t(memory.GetMemoryTag())] -= eviction_size;
		return;
	}

	// The file is not in the shared pool of files.
	auto &fs = FileSystem::GetFileSystem(db);
	auto path = GetTemporaryPath(id);
	auto metadata = fs.GetStatsIfExists(path);
	if (metadata) {
		evicted_data_per_tag[uint8_t(memory.GetMemoryTag())] -= memory.GetMemoryUsage();
		fs.RemoveFile(path);
		temporary_directory.handle->GetTempFile().DecreaseSizeOnDisk(NumericCast<idx_t>(metadata->file_size));
	}
}

bool StandardBufferManager::HasTemporaryDirectory() const {
	return !temporary_directory.path.empty();
}

bool StandardBufferManager::HasFilesInTemporaryDirectory() const {
	if (temporary_directory.path.empty()) {
		return false;
	}

	auto &fs = FileSystem::GetFileSystem(db);
	bool found = false;
	fs.ListFiles(temporary_directory.path, [&](const string &name, bool is_dir) {
		if (is_dir) {
			return;
		}
		if (StringUtil::EndsWith(name, ".block") || StringUtil::EndsWith(name, ".tmp")) {
			found = true;
		}
	});
	return found;
}

BlockManager &StandardBufferManager::GetTemporaryBlockManager() {
	return *temp_block_manager;
}

vector<TemporaryFileInformation> StandardBufferManager::GetTemporaryFiles() {
	vector<TemporaryFileInformation> result;
	if (temporary_directory.path.empty()) {
		return result;
	}
	{
		lock_guard<mutex> temp_handle_guard(temporary_directory.lock);
		if (temporary_directory.handle) {
			result = temporary_directory.handle->GetTempFile().GetTemporaryFiles();
		}
	}
	auto &fs = FileSystem::GetFileSystem(db);
	fs.ListFiles(temporary_directory.path, [&](const string &name, bool is_dir) {
		if (is_dir) {
			return;
		}
		if (!StringUtil::EndsWith(name, ".block")) {
			return;
		}

		// Another process or thread can delete the file before we can get its file size.
		auto path = fs.JoinPath(temporary_directory.path, name);
		auto metadata = fs.GetStatsIfExists(path);
		if (!metadata) {
			return;
		}

		TemporaryFileInformation info;
		info.path = std::move(path);
		info.size = NumericCast<idx_t>(metadata->file_size);
		result.push_back(info);
	});
	return result;
}

const char *StandardBufferManager::InMemoryWarning() {
	if (!temporary_directory.path.empty()) {
		return "";
	}
	return "\nDatabase is launched in in-memory mode and no temporary directory is specified."
	       "\nUnused blocks cannot be offloaded to disk."
	       "\n\nLaunch the database with a persistent storage back-end"
	       "\nOr set SET temp_directory='/path/to/tmp.tmp'";
}

void StandardBufferManager::ReserveMemory(idx_t size) {
	if (size == 0) {
		return;
	}
	auto reservation =
	    EvictBlocksOrThrow(QueryContext(), MemoryTag::EXTENSION, size, nullptr,
	                       "failed to reserve memory data of size %s%s", StringUtil::BytesToHumanReadableString(size));
	reservation.size = 0;
}

void StandardBufferManager::FreeReservedMemory(idx_t size) {
	if (size == 0) {
		return;
	}
	buffer_pool.memory_usage.UpdateUsedMemory(MemoryTag::EXTENSION, -(int64_t)size);
}

//===--------------------------------------------------------------------===//
// Buffer Allocator
//===--------------------------------------------------------------------===//
TempBufferPoolReservation BufferAllocatorData::Reserve(idx_t size) {
	return manager.EvictBlocksOrThrow(QueryContext(), MemoryTag::ALLOCATOR, size, nullptr,
	                                  "failed to allocate data of size %s%s",
	                                  StringUtil::BytesToHumanReadableString(size));
}

void BufferAllocatorData::Release(idx_t size) {
	BufferPoolReservation r(MemoryTag::ALLOCATOR, manager.GetBufferPool());
	r.size = size;
	r.Resize(0);
}

data_ptr_t BufferAllocatorData::Allocate(idx_t size) {
	auto reservation = Reserve(size);
	auto pointer = BlockAllocator::Get(manager.db).AllocateData(size);
	// Commit the reservation only after backing allocation succeeds.
	reservation.size = 0;
	return pointer;
}

void BufferAllocatorData::Free(data_ptr_t pointer, idx_t size) {
	Release(size);
	BlockAllocator::Get(manager.db).FreeData(pointer, size);
}

data_ptr_t BufferAllocatorData::Reallocate(data_ptr_t pointer, idx_t old_size, idx_t size) {
	if (old_size == size) {
		return pointer;
	}
	auto result = BlockAllocator::Get(manager.db).ReallocateData(pointer, old_size, size);
	// Reallocation only adjusts accounting, and backing failure must leave the old reservation intact.
	BufferPoolReservation reservation(MemoryTag::ALLOCATOR, manager.GetBufferPool());
	reservation.size = old_size;
	reservation.Resize(size);
	reservation.size = 0;
	return result;
}

BufferAllocator::BufferAllocator(StandardBufferManager &manager)
    : Allocator([](PrivateAllocatorData *data, idx_t size) { return data->Cast<BufferAllocatorData>().Allocate(size); },
                [](PrivateAllocatorData *data, data_ptr_t pointer, idx_t size) {
	                data->Cast<BufferAllocatorData>().Free(pointer, size);
                },
                [](PrivateAllocatorData *data, data_ptr_t pointer, idx_t old_size, idx_t size) {
	                return data->Cast<BufferAllocatorData>().Reallocate(pointer, old_size, size);
                },
                make_uniq<BufferAllocatorData>(manager)) {
}

HugeAllocation BufferAllocator::AllocateHuge(idx_t size) {
	D_ASSERT(size > 0);
	auto &data = GetPrivateData()->Cast<BufferAllocatorData>();
	auto reservation = data.Reserve(size);
	auto pointer = BlockAllocator::Get(data.manager.GetDatabase()).AllocateHugeData(size);
	reservation.size = 0;
	return HugeAllocation(*this, pointer, size);
}

void BufferAllocator::FreeHuge(data_ptr_t pointer, idx_t size) {
	auto &data = GetPrivateData()->Cast<BufferAllocatorData>();
	data.Release(size);
	BlockAllocator::Get(data.manager.GetDatabase()).FreeHugeData(pointer, size);
}

HugeAllocation::HugeAllocation() : allocator(nullptr), pointer(nullptr), allocated_size(0) {
}

HugeAllocation::HugeAllocation(BufferAllocator &allocator, data_ptr_t pointer, idx_t allocated_size)
    : allocator(allocator), pointer(pointer), allocated_size(allocated_size) {
	D_ASSERT(pointer);
}

HugeAllocation::~HugeAllocation() {
	Reset();
}

HugeAllocation::HugeAllocation(HugeAllocation &&other) noexcept
    : allocator(other.allocator), pointer(nullptr), allocated_size(0) {
	std::swap(pointer, other.pointer);
	std::swap(allocated_size, other.allocated_size);
}

HugeAllocation &HugeAllocation::operator=(HugeAllocation &&other) noexcept {
	std::swap(allocator, other.allocator);
	std::swap(pointer, other.pointer);
	std::swap(allocated_size, other.allocated_size);
	return *this;
}

void HugeAllocation::Reset() {
	if (!pointer) {
		return;
	}
	D_ASSERT(allocator);
	allocator->FreeHuge(pointer, allocated_size);
	pointer = nullptr;
	allocated_size = 0;
}

BufferAllocator &BufferAllocator::Get(ClientContext &context) {
	auto &manager = StandardBufferManager::GetBufferManager(context);
	return manager.GetBufferAllocator();
}

BufferAllocator &BufferAllocator::Get(DatabaseInstance &db) {
	return StandardBufferManager::GetBufferManager(db).GetBufferAllocator();
}

BufferAllocator &BufferAllocator::Get(AttachedDatabase &db) {
	return BufferAllocator::Get(db.GetDatabase());
}

BufferAllocator &StandardBufferManager::GetBufferAllocator() {
	return buffer_allocator;
}

} // namespace duckdb
