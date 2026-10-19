#include "duckdb/storage/temporary_file_manager.hpp"

#include "duckdb/common/exception.hpp"

#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/process_util.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/storage/buffer/temporary_file_information.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/common/encryption_functions.hpp"
#include "zstd.h"

namespace duckdb {

//===--------------------------------------------------------------------===//
// TemporaryBufferSize
//===--------------------------------------------------------------------===//
bool TemporaryBufferSizeIsValid(const TemporaryBufferSize size) {
	switch (size) {
	case TemporaryBufferSize::S32K:
	case TemporaryBufferSize::S64K:
	case TemporaryBufferSize::S96K:
	case TemporaryBufferSize::S128K:
	case TemporaryBufferSize::S160K:
	case TemporaryBufferSize::S192K:
	case TemporaryBufferSize::S224K:
	case TemporaryBufferSize::DEFAULT:
		return true;
	default:
		return false;
	}
}

static TemporaryBufferSize SizeToTemporaryBufferSize(const idx_t size) {
	D_ASSERT(size != 0 && size % TEMPORARY_BUFFER_SIZE_GRANULARITY == 0);
	const auto res = static_cast<TemporaryBufferSize>(size);
	D_ASSERT(TemporaryBufferSizeIsValid(res));
	return res;
}

static idx_t TemporaryBufferSizeToSize(const TemporaryBufferSize size) {
	D_ASSERT(TemporaryBufferSizeIsValid(size));
	return static_cast<idx_t>(size);
}

static TemporaryBufferSize RoundUpSizeToTemporaryBufferSize(const idx_t size) {
	return SizeToTemporaryBufferSize(AlignValue<idx_t, TEMPORARY_BUFFER_SIZE_GRANULARITY>(size));
}

static const vector<TemporaryBufferSize> TemporaryBufferSizes() {
	return {TemporaryBufferSize::S32K,  TemporaryBufferSize::S64K,   TemporaryBufferSize::S96K,
	        TemporaryBufferSize::S128K, TemporaryBufferSize::S160K,  TemporaryBufferSize::S192K,
	        TemporaryBufferSize::S224K, TemporaryBufferSize::DEFAULT};
}

static TemporaryBufferSize MinimumCompressedTemporaryBufferSize() {
	return TemporaryBufferSize::S32K;
}

static TemporaryBufferSize MaximumCompressedTemporaryBufferSize() {
	return TemporaryBufferSize::S224K;
}

//===--------------------------------------------------------------------===//
// TemporaryFileIdentifier/TemporaryFileIndex
//===--------------------------------------------------------------------===//
TemporaryFileIdentifier::TemporaryFileIdentifier() : size(TemporaryBufferSize::INVALID) {
}

TemporaryFileIdentifier::TemporaryFileIdentifier(TemporaryBufferSize size_p, idx_t file_index_p)
    : size(size_p), file_index(file_index_p) {
}

TemporaryFileIdentifier::TemporaryFileIdentifier(DatabaseInstance &db, TemporaryBufferSize size_p, idx_t file_index_p,
                                                 bool encrypted_p)
    : size(size_p), file_index(file_index_p), encrypted(encrypted_p) {
	if (encrypted) {
		// generate a random encryption key ID and corresponding key
		EncryptionEngine::AddTempKeyToCache(db);
	}
}

bool TemporaryFileIdentifier::IsValid() const {
	return size != TemporaryBufferSize::INVALID && file_index.IsValid();
}

TemporaryFileIndex::TemporaryFileIndex() {
}

TemporaryFileIndex::TemporaryFileIndex(TemporaryFileIdentifier identifier_p, idx_t block_index_p,
                                       idx_t block_header_size_p, FileBufferType buffer_type_p)
    : identifier(identifier_p), block_index(block_index_p), block_header_size(block_header_size_p),
      buffer_type(buffer_type_p) {
}

bool TemporaryFileIndex::IsValid() const {
	return identifier.IsValid() && block_index.IsValid() && block_header_size.IsValid();
}

//===--------------------------------------------------------------------===//
// BlockIndexManager
//===--------------------------------------------------------------------===//
BlockIndexManager::BlockIndexManager() : max_index(0), manager(nullptr), encrypted(false) {
}

BlockIndexManager::BlockIndexManager(TemporaryFileManager &manager, bool encrypted)
    : max_index(0), manager(&manager), encrypted(encrypted) {
}

//! Physical stride of one block on disk: payload + (per-block encryption header, if encrypted).
static idx_t PhysicalBlockSize(const TemporaryBufferSize size, const bool encrypted) {
	const auto base = size == TemporaryBufferSize::DEFAULT ? DEFAULT_BLOCK_ALLOC_SIZE : TemporaryBufferSizeToSize(size);
	const auto header_size = encrypted ? DEFAULT_ENCRYPTED_BUFFER_HEADER_SIZE : 0;
	return base + header_size;
}

idx_t BlockIndexManager::GetNewBlockIndex(const TemporaryBufferSize size) {
	auto index = GetNewBlockIndexInternal(size);
	indexes_in_use.insert(index);
	return index;
}

bool BlockIndexManager::RemoveIndex(idx_t index, const TemporaryBufferSize size) {
	// remove this block from the set of blocks
	auto entry = indexes_in_use.find(index);
	if (entry == indexes_in_use.end()) {
		throw InternalException("RemoveIndex - index %llu not found in indexes_in_use", index);
	}
	indexes_in_use.erase(entry);
	free_indexes.insert(index);
	// check if we can truncate the file

	// get the max_index in use right now
	auto max_index_in_use = indexes_in_use.empty() ? 0 : *indexes_in_use.rbegin() + 1;
	if (max_index_in_use < max_index) {
		// max index in use is lower than the max_index
		// reduce the max_index
		SetMaxIndex(max_index_in_use, size);
		// we can remove any free_indexes that are larger than the current max_index
		while (HasFreeBlocks()) {
			auto max_entry = *free_indexes.rbegin();
			if (max_entry < max_index) {
				break;
			}
			free_indexes.erase(max_entry);
		}
		return true;
	}
	return false;
}

idx_t BlockIndexManager::GetMaxIndex() const {
	return max_index;
}

idx_t BlockIndexManager::GetUsedBlockCount() const {
	return indexes_in_use.size();
}

bool BlockIndexManager::HasFreeBlocks() const {
	return !free_indexes.empty();
}

idx_t BlockIndexManager::GetNewBlockIndexInternal(const TemporaryBufferSize size) {
	if (!HasFreeBlocks()) {
		auto new_index = max_index;
		SetMaxIndex(max_index + 1, size);
		return new_index;
	}
	auto entry = free_indexes.begin();
	auto index = *entry;
	free_indexes.erase(entry);
	return index;
}

void BlockIndexManager::SetMaxIndex(const idx_t new_index, const TemporaryBufferSize size) {
	const auto temp_file_block_size = PhysicalBlockSize(size, encrypted);
	if (!manager) {
		max_index = new_index;
	} else {
		auto old = max_index;
		if (new_index < old) {
			max_index = new_index;
			const auto difference = old - new_index;
			const auto size_on_disk = difference * temp_file_block_size;
			manager->DecreaseSizeOnDisk(size_on_disk);
		} else if (new_index > old) {
			const auto difference = new_index - old;
			const auto size_on_disk = difference * temp_file_block_size;
			manager->IncreaseSizeOnDisk(size_on_disk);
			// Increase can throw, so this is only updated after it was successfully updated
			max_index = new_index;
		}
	}
}

//===--------------------------------------------------------------------===//
// TemporaryFileHandle
//===--------------------------------------------------------------------===//
TemporaryFileHandle::TemporaryFileHandle(TemporaryFileManager &manager, TemporaryFileIdentifier identifier_p,
                                         idx_t temp_file_count)
    : db(manager.db), identifier(identifier_p), max_allowed_index((1 << temp_file_count) * MAX_ALLOWED_INDEX_BASE),
      path(manager.CreateTemporaryFileName(identifier)), index_manager(manager, identifier.encrypted) {
}

TemporaryFileHandle::~TemporaryFileHandle() {
}

TemporaryFileHandle::TemporaryFileLock::TemporaryFileLock(mutex &mutex) : lock(mutex) {
}

TemporaryFileIndex TemporaryFileHandle::TryGetBlockIndex(idx_t block_header_size, FileBufferType buffer_type) {
	TemporaryFileLock lock(file_lock);
	if (index_manager.GetMaxIndex() >= max_allowed_index && !index_manager.HasFreeBlocks()) {
		// file is at capacity
		return TemporaryFileIndex();
	}
	// open the file handle if it does not yet exist
	CreateFileIfNotExists(lock);
	// fetch a new block index to write to
	auto block_index = index_manager.GetNewBlockIndex(identifier.size);
	return TemporaryFileIndex(identifier, block_index, block_header_size, buffer_type);
}

FileHandle &TemporaryFileHandle::GetIOHandle(data_ptr_t buffer, const idx_t size, const idx_t offset) {
	if (!handle->GetFlags().DirectIO() || (AlignPointer<Storage::SECTOR_SIZE>(buffer) == buffer &&
	                                       size % Storage::SECTOR_SIZE == 0 && offset % Storage::SECTOR_SIZE == 0)) {
		return *handle;
	}

	TemporaryFileLock lock(file_lock);
	if (!buffered_handle) {
		auto &fs = FileSystem::GetFileSystem(db);
		buffered_handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ | FileFlags::FILE_FLAGS_WRITE);
	}
	return *buffered_handle;
}

unique_ptr<FileBuffer> TemporaryFileHandle::ReadTemporaryBuffer(QueryContext context,
                                                                const TemporaryFileIndex &index_in_file,
                                                                unique_ptr<FileBuffer> buffer) {
	auto block_index = index_in_file.block_index.GetIndex();
	D_ASSERT(buffer->GetHeaderSize() == index_in_file.block_header_size.GetIndex());
	AllocatedData compressed_buffer;
	data_ptr_t read_buffer;
	idx_t read_size;
	bool is_uncompressed = identifier.size == TemporaryBufferSize::DEFAULT;
	if (is_uncompressed) {
		// not compressed - read directly into buffer
		read_buffer = buffer->InternalBuffer();
		read_size = buffer->AllocSize();
	} else {
		// compressed - read into separate compression buffer first
		read_size = TemporaryBufferSizeToSize(identifier.size);
		compressed_buffer = Allocator::Get(db).Allocate(read_size + Storage::SECTOR_SIZE - 1);
		read_buffer = AlignPointer<Storage::SECTOR_SIZE>(compressed_buffer.get());
	}

	idx_t read_position = GetPositionInFile(block_index);
	if (IsEncrypted()) {
		uint8_t encryption_metadata[DEFAULT_ENCRYPTED_BUFFER_HEADER_SIZE];
		//! Read nonce and tag.
		handle->Read(context, encryption_metadata, DEFAULT_ENCRYPTED_BUFFER_HEADER_SIZE, read_position);
		//! Read the encrypted compressed buffer.
		handle->Read(context, read_buffer, read_size, read_position + DEFAULT_ENCRYPTED_BUFFER_HEADER_SIZE);
		//! Decrypt the compressed buffer.
		EncryptionEngine::DecryptTemporaryBuffer(db, read_buffer, read_size, encryption_metadata);
	} else {
		GetIOHandle(read_buffer, read_size, read_position).Read(context, read_buffer, read_size, read_position);
	}

	if (is_uncompressed) {
		return buffer;
	}

	// Decompress into buffer.
	// The leading length word and the compressed payload both come off disk, so validate them before
	// handing them to zstd: an oversized length would make ZSTD_decompress read past compressed_buffer.
	const auto compressed_size = Load<idx_t>(read_buffer);
	if (compressed_size > read_size - sizeof(idx_t)) {
		throw IOException("Corrupt temporary file: compressed block claims %llu bytes but only %llu are available",
		                  compressed_size, read_size - sizeof(idx_t));
	}
	const auto decompressed_size = duckdb_zstd::ZSTD_decompress(buffer->InternalBuffer(), buffer->AllocSize(),
	                                                            read_buffer + sizeof(idx_t), compressed_size);
	if (duckdb_zstd::ZSTD_isError(decompressed_size)) {
		throw IOException("Corrupt temporary file: failed to decompress block (%s)",
		                  duckdb_zstd::ZSTD_getErrorName(decompressed_size));
	}
	if (decompressed_size != buffer->AllocSize()) {
		throw IOException("Corrupt temporary file: decompressed block is %llu bytes but expected %llu",
		                  decompressed_size, buffer->AllocSize());
	}
	return buffer;
}

void TemporaryFileHandle::WriteTemporaryBuffer(QueryContext context, FileBuffer &buffer, const idx_t block_index,
                                               AllocatedData &compressed_buffer) {
	// We group DEFAULT_BLOCK_ALLOC_SIZE blocks into the same file.
	D_ASSERT(buffer.AllocSize() == BufferManager::GetBufferManager(db).GetBlockAllocSize());
	bool is_uncompressed = identifier.size == TemporaryBufferSize::DEFAULT;
	data_ptr_t write_buffer;
	idx_t write_size;
	if (is_uncompressed) {
		// write the uncompressed buffer to the file directly
		write_buffer = buffer.InternalBuffer();
		write_size = buffer.AllocSize();
	} else {
		// write the compressed buffer to the file
		write_buffer = AlignPointer<Storage::SECTOR_SIZE>(compressed_buffer.get());
		write_size = TemporaryBufferSizeToSize(identifier.size);
	}
	idx_t write_position = GetPositionInFile(block_index);
	if (IsEncrypted()) {
		uint8_t encryption_metadata[DEFAULT_ENCRYPTED_BUFFER_HEADER_SIZE];
		EncryptionEngine::EncryptTemporaryBuffer(db, write_buffer, write_size, encryption_metadata);

		handle->Write(context, encryption_metadata, DEFAULT_ENCRYPTED_BUFFER_HEADER_SIZE, write_position);
		handle->Write(context, write_buffer, write_size, write_position + DEFAULT_ENCRYPTED_BUFFER_HEADER_SIZE);
	} else {
		GetIOHandle(write_buffer, write_size, write_position).Write(context, write_buffer, write_size, write_position);
	}
}

void TemporaryFileHandle::EraseBlockIndex(block_id_t block_index) {
	// remove the block (and potentially truncate the temp file)
	TemporaryFileLock lock(file_lock);
	D_ASSERT(handle);
	RemoveTempBlockIndex(lock, NumericCast<idx_t>(block_index));
}

bool TemporaryFileHandle::DeleteIfEmpty() {
	TemporaryFileLock lock(file_lock);
	if (index_manager.GetMaxIndex() > 0) {
		// there are still blocks in this file
		return false;
	}
	// the file is empty: delete it
	buffered_handle.reset();
	handle.reset();
	auto &fs = FileSystem::GetFileSystem(db);
	fs.RemoveFile(path);
	return true;
}

bool TemporaryFileHandle::IsEncrypted() const {
	return identifier.encrypted;
}

TemporaryFileInformation TemporaryFileHandle::GetTemporaryFile() {
	TemporaryFileLock lock(file_lock);
	TemporaryFileInformation info;
	info.path = path;
	info.size = GetPositionInFile(index_manager.GetMaxIndex());
	return info;
}

void TemporaryFileHandle::CreateFileIfNotExists(TemporaryFileLock &) {
	if (handle) {
		return;
	}
	auto &fs = FileSystem::GetFileSystem(db);
	auto open_flags = FileFlags::FILE_FLAGS_READ | FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE;
// Experimental direct I/O requires an explicit compiler definition.
#if defined(DUCKDB_TEMPORARY_FILE_DIRECT_IO) && (!defined(__sun) || !defined(__SVR4))
	if (!IsEncrypted()) {
		try {
			handle = fs.OpenFile(path, open_flags | FileFlags::FILE_FLAGS_DIRECT_IO);
			return;
		} catch (const IOException &) {
			// Some filesystems cannot open files with direct I/O.
		}
	}
#endif
	handle = fs.OpenFile(path, open_flags);
}

void TemporaryFileHandle::RemoveTempBlockIndex(TemporaryFileLock &, idx_t index) {
	// remove the block index from the index manager
	if (index_manager.RemoveIndex(index, identifier.size)) {
		// the max_index that is currently in use has decreased
		// as a result we can truncate the file
#ifndef WIN32 // this ended up causing issues when sorting
		auto max_index = index_manager.GetMaxIndex();
		auto &fs = FileSystem::GetFileSystem(db);
		fs.Truncate(*handle, NumericCast<int64_t>(GetPositionInFile(max_index)));
#endif
	}
}

idx_t TemporaryFileHandle::GetPositionInFile(const idx_t index) const {
	idx_t metadata_size = 0;
	if (IsEncrypted()) {
		// if the file is encrypted, we need to account for the header size
		metadata_size = DEFAULT_ENCRYPTED_BUFFER_HEADER_SIZE;
	}
	return index * (static_cast<idx_t>(identifier.size) + metadata_size);
}

//===--------------------------------------------------------------------===//
// TemporaryFileMap
//===--------------------------------------------------------------------===//
TemporaryFileMap::TemporaryFileMap(TemporaryFileManager &manager_p) : manager(manager_p) {
}

void TemporaryFileMap::Clear() {
	files.clear();
}

TemporaryFileMap::temporary_file_map_t &TemporaryFileMap::GetMapForSize(const TemporaryBufferSize size) {
	D_ASSERT(TemporaryBufferSizeIsValid(size));
	return files[size];
}

optional_ptr<TemporaryFileHandle> TemporaryFileMap::GetFile(const TemporaryFileIdentifier &identifier) {
	D_ASSERT(identifier.IsValid());
	auto &map = GetMapForSize(identifier.size);
	const auto it = map.find(identifier.file_index.GetIndex());
	return it == map.end() ? nullptr : it->second.get();
}

TemporaryFileHandle &TemporaryFileMap::CreateFile(const TemporaryFileIdentifier &identifier) {
	D_ASSERT(identifier.IsValid());
	D_ASSERT(!GetFile(identifier));
	auto &map = GetMapForSize(identifier.size);
	const auto res =
	    map.emplace(identifier.file_index.GetIndex(), make_uniq<TemporaryFileHandle>(manager, identifier, map.size()));
	D_ASSERT(res.second);
	return *res.first->second;
}

void TemporaryFileMap::EraseFile(const TemporaryFileIdentifier &identifier) {
	D_ASSERT(identifier.IsValid());
	D_ASSERT(GetFile(identifier));
	GetMapForSize(identifier.size).erase(identifier.file_index.GetIndex());
}

//===--------------------------------------------------------------------===//
// TemporaryFileCompressionLevel/TemporaryFileCompressionAdaptivity
//===--------------------------------------------------------------------===//
TemporaryFileCompressionAdaptivity::TemporaryFileCompressionAdaptivity() : last_uncompressed_write_ns(INITIAL_NS) {
	for (idx_t i = 0; i < LEVELS; i++) {
		last_compressed_writes_ns[i] = INITIAL_NS;
	}
}

TemporaryCompressionLevel TemporaryFileCompressionAdaptivity::IndexToLevel(const idx_t index) {
	return static_cast<TemporaryCompressionLevel>(NumericCast<int>(index) * 2 - 5);
}

idx_t TemporaryFileCompressionAdaptivity::LevelToIndex(const TemporaryCompressionLevel level) {
	return NumericCast<idx_t>((static_cast<int>(level) + 5) / 2);
}

TemporaryCompressionLevel TemporaryFileCompressionAdaptivity::MinimumCompressionLevel() {
	return IndexToLevel(0);
}

TemporaryCompressionLevel TemporaryFileCompressionAdaptivity::MaximumCompressionLevel() {
	return IndexToLevel(LEVELS - 1);
}

TemporaryCompressionLevel TemporaryFileCompressionAdaptivity::GetCompressionLevel() {
	idx_t min_compression_idx = 0;
	TemporaryCompressionLevel level;

	double ratio;
	bool should_compress;

	bool should_deviate;
	bool deviate_uncompressed;
	{
		lock_guard<mutex> guard(random_engine.lock);

		auto min_compressed_time = last_compressed_writes_ns[min_compression_idx];
		for (idx_t compression_idx = 1; compression_idx < LEVELS; compression_idx++) {
			const auto time = last_compressed_writes_ns[compression_idx];
			if (time < min_compressed_time) {
				min_compression_idx = compression_idx;
				min_compressed_time = time;
			}
		}
		level = IndexToLevel(min_compression_idx);

		ratio = static_cast<double>(min_compressed_time) / static_cast<double>(last_uncompressed_write_ns);
		should_compress = ratio < DURATION_RATIO_THRESHOLD;

		should_deviate = random_engine.NextRandom() < COMPRESSION_DEVIATION;
		deviate_uncompressed = random_engine.NextRandom() < 0.5; // Coin flip to deviate with just uncompressed
	}

	TemporaryCompressionLevel result;
	if (!should_deviate) {
		result = should_compress ? level : TemporaryCompressionLevel::UNCOMPRESSED; // Don't deviate
	} else if (!should_compress) {
		result = MinimumCompressionLevel(); // Deviate from uncompressed -> go to fastest level
	} else if (deviate_uncompressed) {
		result = TemporaryCompressionLevel::UNCOMPRESSED;
	} else if (level == MaximumCompressionLevel()) {
		result = IndexToLevel(min_compression_idx - 1); // At highest level, go down one
	} else if (ratio < 1.0) { // Compressed writes are faster, try increasing the compression level
		result = IndexToLevel(min_compression_idx + 1);
	} else { // Compressed writes are slower, try decreasing the compression level
		result = level == MinimumCompressionLevel()
		             ? TemporaryCompressionLevel::UNCOMPRESSED // Already lowest level, go to uncompressed
		             : IndexToLevel(min_compression_idx - 1);
	}
	return result;
}

void TemporaryFileCompressionAdaptivity::Update(const TemporaryCompressionLevel level, const TimePoint &time_before) {
	const auto duration = time_before.ElapsedNanos();
	auto &last_write_ns = level == TemporaryCompressionLevel::UNCOMPRESSED
	                          ? last_uncompressed_write_ns
	                          : last_compressed_writes_ns[LevelToIndex(level)];
	lock_guard<mutex> guard(random_engine.lock);
	last_write_ns = (last_write_ns * (WEIGHT - 1) + duration) / WEIGHT;
}

//===--------------------------------------------------------------------===//
// TemporaryFileManager
//===--------------------------------------------------------------------===//
TemporaryFileManager::TemporaryFileManager(DatabaseInstance &db, const string &temp_directory_p,
                                           atomic<idx_t> &size_on_disk_p, string file_prefix_p)
    : file_prefix(std::move(file_prefix_p)), db(db), temp_directory(temp_directory_p), files(*this),
      size_on_disk(size_on_disk_p), max_swap_space(0) {
}

TemporaryFileManager::~TemporaryFileManager() {
	files.Clear();
}

TemporaryFileManager::TemporaryFileManagerLock::TemporaryFileManagerLock(mutex &mutex) : lock(mutex) {
}

idx_t TemporaryFileManager::WriteTemporaryBuffer(QueryContext context, block_id_t block_id, FileBuffer &buffer) {
	// We group DEFAULT_BLOCK_ALLOC_SIZE blocks into the same file.
	D_ASSERT(buffer.AllocSize() == BufferManager::GetBufferManager(db).GetBlockAllocSize());

	auto header_size = buffer.GetHeaderSize();
	auto buffer_type = buffer.GetBufferType();
	const auto adaptivity_idx = TaskScheduler::GetEstimatedCPUId() % COMPRESSION_ADAPTIVITIES;
	auto &compression_adaptivity = compression_adaptivities[adaptivity_idx];

	const auto time_before = TimePoint::Tick();
	AllocatedData compressed_buffer;
	const auto compression_result = CompressBuffer(compression_adaptivity, buffer, compressed_buffer);

	TemporaryFileIndex index;
	optional_ptr<TemporaryFileHandle> handle;
	{
		TemporaryFileManagerLock lock(manager_lock);
		// first check if we can write to an open existing file
		for (auto &entry : files.GetMapForSize(compression_result.size)) {
			auto &temp_file = entry.second;
			index = temp_file->TryGetBlockIndex(header_size, buffer_type);
			if (index.IsValid()) {
				handle = entry.second.get();
				break;
			}
		}
		if (!handle) {
			// no existing handle to write to; we need to create & open a new file
			auto &size = compression_result.size;
			const TemporaryFileIdentifier identifier(db, size, index_managers[size].GetNewBlockIndex(size),
			                                         IsEncrypted());
			auto &new_file = files.CreateFile(identifier);
			index = new_file.TryGetBlockIndex(header_size, buffer_type);
			handle = &new_file;
		}
		D_ASSERT(used_blocks.find(block_id) == used_blocks.end());
		used_blocks[block_id] = index;
	}
	D_ASSERT(handle);
	D_ASSERT(index.IsValid());

	try {
		handle->WriteTemporaryBuffer(context, buffer, index.block_index.GetIndex(), compressed_buffer);
	} catch (...) {
		// the block was registered under the lock above - unregister it again on write failure
		TemporaryFileManagerLock lock(manager_lock);
		EraseUsedBlock(lock, block_id, *handle, index);
		throw;
	}

	compression_adaptivity.Update(compression_result.level, time_before);
	return PhysicalBlockSize(compression_result.size, handle->IsEncrypted());
}

TemporaryFileManager::CompressionResult
TemporaryFileManager::CompressBuffer(TemporaryFileCompressionAdaptivity &compression_adaptivity, FileBuffer &buffer,
                                     AllocatedData &compressed_buffer) {
	if (buffer.AllocSize() <= TemporaryBufferSizeToSize(MinimumCompressedTemporaryBufferSize())) {
		// Buffer size is less or equal to the minimum compressed size - no point compressing
		return {TemporaryBufferSize::DEFAULT, TemporaryCompressionLevel::UNCOMPRESSED};
	}

	const auto level = compression_adaptivity.GetCompressionLevel();
	if (level == TemporaryCompressionLevel::UNCOMPRESSED) {
		return {TemporaryBufferSize::DEFAULT, TemporaryCompressionLevel::UNCOMPRESSED};
	}

	const auto compression_level = static_cast<int>(level);
	D_ASSERT(compression_level >= duckdb_zstd::ZSTD_minCLevel() && compression_level <= duckdb_zstd::ZSTD_maxCLevel());
	const auto zstd_bound = duckdb_zstd::ZSTD_compressBound(buffer.AllocSize());
	compressed_buffer = Allocator::Get(db).Allocate(sizeof(idx_t) + zstd_bound + Storage::SECTOR_SIZE - 1);
	const auto compressed_ptr = AlignPointer<Storage::SECTOR_SIZE>(compressed_buffer.get());
	const auto zstd_size = duckdb_zstd::ZSTD_compress(compressed_ptr + sizeof(idx_t), zstd_bound,
	                                                  buffer.InternalBuffer(), buffer.AllocSize(), compression_level);
	D_ASSERT(!duckdb_zstd::ZSTD_isError(zstd_size));
	Store<idx_t>(zstd_size, compressed_ptr);
	const auto compressed_size = sizeof(idx_t) + zstd_size;

	if (compressed_size > TemporaryBufferSizeToSize(MaximumCompressedTemporaryBufferSize())) {
		return {TemporaryBufferSize::DEFAULT, level}; // Use default size if compression ratio is bad
	}

	return {RoundUpSizeToTemporaryBufferSize(compressed_size), level};
}

bool TemporaryFileManager::HasTemporaryBuffer(block_id_t block_id) {
	lock_guard<mutex> lock(manager_lock);
	return used_blocks.find(block_id) != used_blocks.end();
}

idx_t TemporaryFileManager::GetTotalUsedSpaceInBytes() const {
	return size_on_disk.load(std::memory_order_relaxed);
}

optional_idx TemporaryFileManager::GetMaxSwapSpace() const {
	return max_swap_space;
}

static idx_t GetDefaultMax(const string &path) {
	D_ASSERT(!path.empty());
	auto disk_space = FileSystem::GetAvailableDiskSpace(path);
	// Use the available disk space
	// We have made sure that the file exists before we call this, it shouldn't fail
	if (!disk_space.IsValid()) {
		// But if it does (i.e because the system call is not implemented)
		// we don't cap the available swap space
		return DConstants::INVALID_INDEX - 1;
	}
	// Only use 90% of the available disk space
	return static_cast<idx_t>(static_cast<double>(disk_space.GetIndex()) * 0.9);
}

void TemporaryFileManager::SetMaxSwapSpace(optional_idx limit) {
	idx_t new_limit;
	if (limit.IsValid()) {
		new_limit = limit.GetIndex();
	} else {
		new_limit = GetDefaultMax(temp_directory);
	}

	auto current_size_on_disk = GetTotalUsedSpaceInBytes();
	if (current_size_on_disk > new_limit) {
		auto used = StringUtil::BytesToHumanReadableString(current_size_on_disk);
		auto max = StringUtil::BytesToHumanReadableString(new_limit);
		throw OutOfMemoryException(
		    R"(failed to adjust the 'max_temp_directory_size', currently used space (%s) exceeds the new limit (%s)
Please increase the limit or destroy the buffers stored in the temp directory by e.g removing temporary tables.
To get usage information of the temp_directory, use 'CALL duckdb_temporary_files();'
		)",
		    used, max);
	}
	max_swap_space = new_limit;
}

void TemporaryFileManager::IncreaseSizeOnDisk(idx_t bytes) {
	auto current_size_on_disk = size_on_disk.load();
	do {
		if (current_size_on_disk + bytes > max_swap_space) {
			auto used = StringUtil::BytesToHumanReadableString(current_size_on_disk);
			auto max = StringUtil::BytesToHumanReadableString(max_swap_space);
			auto data_size = StringUtil::BytesToHumanReadableString(bytes);
			throw OutOfMemoryException(R"(failed to offload data block of size %s (%s/%s used).
This limit was set by the 'max_temp_directory_size' setting.
By default, this setting utilizes the available disk space on the drive where the 'temp_directory' is located.
You can adjust this setting, by using (for example) PRAGMA max_temp_directory_size='10GiB')",
			                           data_size, used, max);
		}
	} while (!size_on_disk.compare_exchange_weak(current_size_on_disk, current_size_on_disk + bytes));
}

void TemporaryFileManager::DecreaseSizeOnDisk(idx_t bytes) {
	size_on_disk -= bytes;
}

bool TemporaryFileManager::IsEncrypted() const {
	return Settings::Get<TempFileEncryptionSetting>(db);
}

unique_ptr<FileBuffer> TemporaryFileManager::ReadTemporaryBuffer(QueryContext context, block_id_t id,
                                                                 unique_ptr<FileBuffer> reusable_buffer,
                                                                 idx_t *eviction_size) {
	TemporaryFileIndex index;
	optional_ptr<TemporaryFileHandle> handle;
	{
		TemporaryFileManagerLock lock(manager_lock);
		index = GetTempBlockIndex(lock, id);
		handle = GetFileHandle(lock, index.identifier);
	}

	// If eviction size requested, set it to the size of the block (compressed size if applicable).
	if (eviction_size) {
		*eviction_size = PhysicalBlockSize(index.identifier.size, index.identifier.encrypted);
	}

	auto &buffer_manager = BufferManager::GetBufferManager(db);
	auto block_header_size = index.block_header_size.GetIndex();
	auto buffer =
	    buffer_manager.ConstructManagedBuffer(buffer_manager.GetBlockAllocSize() - block_header_size, block_header_size,
	                                          std::move(reusable_buffer), index.buffer_type);
	buffer = handle->ReadTemporaryBuffer(context, index, std::move(buffer));
	{
		// remove the block (and potentially erase the temp file)
		TemporaryFileManagerLock lock(manager_lock);
		EraseUsedBlock(lock, id, *handle, index);
	}
	return buffer;
}

idx_t TemporaryFileManager::DeleteTemporaryBuffer(block_id_t id) {
	TemporaryFileManagerLock lock(manager_lock);
	auto index = GetTempBlockIndex(lock, id);
	auto handle = GetFileHandle(lock, index.identifier);
	EraseUsedBlock(lock, id, *handle, index);
	return PhysicalBlockSize(index.identifier.size, index.identifier.encrypted);
}

vector<TemporaryFileInformation> TemporaryFileManager::GetTemporaryFiles() {
	lock_guard<mutex> lock(manager_lock);
	vector<TemporaryFileInformation> result;
	for (auto &size : TemporaryBufferSizes()) {
		for (const auto &file : files.GetMapForSize(size)) {
			result.push_back(file.second->GetTemporaryFile());
		}
	}
	return result;
}

void TemporaryFileManager::EraseUsedBlock(TemporaryFileManagerLock &lock, block_id_t id, TemporaryFileHandle &handle,
                                          TemporaryFileIndex index) {
	auto entry = used_blocks.find(id);
	if (entry == used_blocks.end()) {
		throw InternalException("EraseUsedBlock - Block %llu not found in used blocks", id);
	}
	used_blocks.erase(entry);
	handle.EraseBlockIndex(NumericCast<block_id_t>(index.block_index.GetIndex()));
	if (handle.DeleteIfEmpty()) {
		EraseFileHandle(lock, index.identifier);
	}
}

//! an array rather than a pointer, so the length below is derived from it and cannot drift
static constexpr const char TEMPORARY_FILE_PREFIX[] = "duckdb_temp_";
static constexpr idx_t TEMPORARY_FILE_PREFIX_LENGTH = sizeof(TEMPORARY_FILE_PREFIX) - 1;

string TemporaryFilePrefix(const TemporaryFileOwner &owner) {
	return TEMPORARY_FILE_PREFIX + to_string(owner.pid) + "_" + to_string(owner.instance) + "_";
}

string TemporaryOwnerMarkerName(const TemporaryFileOwner &owner) {
	return TemporaryFilePrefix(owner) + "owner";
}

//! Only unsigned decimal, so nothing a version naming its files differently wrote can parse
static bool TryParseDecimal(const string &text, uint64_t &result) {
	if (text.empty() || text.size() > 19) {
		return false;
	}
	uint64_t value = 0;
	for (const auto c : text) {
		if (c < '0' || c > '9') {
			return false;
		}
		value = value * 10 + static_cast<uint64_t>(c - '0');
	}
	result = value;
	return true;
}

bool TryParseTemporaryFileOwner(const string &file_name, TemporaryFileOwner &owner) {
	if (!StringUtil::StartsWith(file_name, TEMPORARY_FILE_PREFIX)) {
		return false;
	}
	auto pid_end = file_name.find('_', TEMPORARY_FILE_PREFIX_LENGTH);
	if (pid_end == string::npos) {
		return false;
	}
	auto instance_end = file_name.find('_', pid_end + 1);
	if (instance_end == string::npos) {
		return false;
	}
	uint64_t pid, instance;
	if (!TryParseDecimal(file_name.substr(TEMPORARY_FILE_PREFIX_LENGTH, pid_end - TEMPORARY_FILE_PREFIX_LENGTH), pid) ||
	    !TryParseDecimal(file_name.substr(pid_end + 1, instance_end - pid_end - 1), instance)) {
		// written by a version that did not name its files after their owner - a UUID carries '-',
		// which is not a decimal digit, so those land here too
		return false;
	}
	owner.pid = NumericCast<int64_t>(pid);
	owner.instance = NumericCast<idx_t>(instance);
	return true;
}

//! Handed out in order, so instances in this process never take the same id twice
static idx_t NextInstanceId() {
	static atomic<idx_t> next_id(0);
	return next_id++;
}

//! Whether this process still owes a directory its sweep. Leaked, mutex included: a DatabaseInstance
//! can be created during static destruction, and locking a destroyed mutex throws.
static bool ClaimDirectorySweep(const string &temp_directory) {
	struct SweptDirectories {
		mutex lock;
		unordered_set<string> directories;
	};
	static auto &swept = *new SweptDirectories();
	lock_guard<mutex> guard(swept.lock);
	return swept.directories.insert(temp_directory).second;
}

string TemporaryFileManager::CreateTemporaryFileName(const TemporaryFileIdentifier &identifier) const {
	return FileSystem::GetFileSystem(db).JoinPath(
	    temp_directory, StringUtil::Format("%sstorage_%s-%llu.tmp", file_prefix, EnumUtil::ToString(identifier.size),
	                                       identifier.file_index.GetIndex()));
}

optional_ptr<TemporaryFileHandle> TemporaryFileManager::GetFileHandle(TemporaryFileManagerLock &,
                                                                      const TemporaryFileIdentifier &identifier) {
	D_ASSERT(identifier.IsValid());
	return files.GetFile(identifier);
}

TemporaryFileIndex TemporaryFileManager::GetTempBlockIndex(TemporaryFileManagerLock &, block_id_t id) {
	D_ASSERT(used_blocks.find(id) != used_blocks.end());
	return used_blocks[id];
}

void TemporaryFileManager::EraseFileHandle(TemporaryFileManagerLock &, const TemporaryFileIdentifier &identifier) {
	D_ASSERT(identifier.IsValid());
	files.EraseFile(identifier);
	index_managers[identifier.size].RemoveIndex(identifier.file_index.GetIndex(), identifier.size);
}

//===--------------------------------------------------------------------===//
// TemporaryDirectoryHandle
//===--------------------------------------------------------------------===//
TemporaryDirectoryHandle::TemporaryDirectoryHandle(DatabaseInstance &db, string path_p, atomic<idx_t> &size_on_disk,
                                                   optional_idx max_swap_space)
    : db(db), temp_directory(std::move(path_p)) {
	auto &fs = FileSystem::GetFileSystem(db);
	D_ASSERT(!temp_directory.empty());
	if (!fs.DirectoryExists(temp_directory)) {
		fs.CreateDirectory(temp_directory);
		created_directory = true;
	}
	ClaimOwner();
	// reap whatever earlier instances left behind - a crash never reaches teardown. Once per
	// directory: repeating it per instance only multiplies the chances of misjudging a live process.
	if (ClaimDirectorySweep(temp_directory)) {
		SweepAbandonedInstances();
	}
	temp_file = make_uniq<TemporaryFileManager>(db, temp_directory, size_on_disk, file_prefix);
	temp_file->SetMaxSwapSpace(max_swap_space);
}

void TemporaryDirectoryHandle::ClaimOwner() {
	auto &fs = FileSystem::GetFileSystem(db);
	owner.pid = ProcessUtil::CurrentProcessId();
	// Creating the marker is what claims the id, because it is the only step that is atomic against
	// an instance that shares our pid but not our counter - two copies of duckdb in one process each
	// count from zero. It is not locked, and nothing ever opens anybody else's: existing is its job.
	for (;;) {
		owner.instance = NextInstanceId();
		auto marker_path = fs.JoinPath(temp_directory, TemporaryOwnerMarkerName(owner));
		unique_ptr<FileHandle> marker;
		try {
			marker = fs.OpenFile(marker_path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW);
		} catch (std::exception &) {
			// taken, by a live instance or by one that died holding it - either way not ours
			if (!fs.FileExists(marker_path)) {
				throw;
			}
			continue;
		}
		marker->Close();
		break;
	}
	file_prefix = TemporaryFilePrefix(owner);
}

vector<string> TemporaryDirectoryHandle::ListOwnFiles() {
	auto &fs = FileSystem::GetFileSystem(db);
	auto marker_name = TemporaryOwnerMarkerName(owner);
	vector<string> result;
	fs.ListFiles(temp_directory, [&](const string &name, bool is_dir) {
		TemporaryFileOwner found;
		if (is_dir || !TryParseTemporaryFileOwner(name, found) || !(found == owner)) {
			return;
		}
		if (name == marker_name) {
			// the claim itself, removed on its own once everything it reserves is gone
			return;
		}
		result.push_back(fs.JoinPath(temp_directory, name));
	});
	return result;
}

void TemporaryDirectoryHandle::SweepAbandonedInstances() {
	auto &fs = FileSystem::GetFileSystem(db);
	try {
		// the instance an id belongs to never affects the verdict, only the process it ran in does
		unordered_map<int64_t, vector<string>> by_pid;
		fs.ListFiles(temp_directory, [&](const string &name, bool is_dir) {
			TemporaryFileOwner found;
			if (!is_dir && TryParseTemporaryFileOwner(name, found)) {
				by_pid[found.pid].push_back(fs.JoinPath(temp_directory, name));
			}
		});
		for (auto &entry : by_pid) {
			// our own pid included: a live process is the one thing that can still be using these
			if (ProcessUtil::ProcessIsRunning(entry.first)) {
				continue;
			}
			fs.RemoveFiles(entry.second);
		}
	} catch (...) { // NOLINT
		            // reclaiming disk is not worth failing an open over - a later process tries again
	}
}

TemporaryDirectoryHandle::~TemporaryDirectoryHandle() {
	// first release any temporary files
	temp_file.reset();
	// Nothing here may throw - this is a destructor - but it reaches RemoveFile, which does: another
	// instance removing a file between TryRemoveFile's existence check and its removal raises ENOENT,
	// and Windows refuses to remove a file another instance holds open. A later instance reaps
	// whatever is left, so there is nothing to do but log.
	try {
		CleanupTemporaryDirectory();
	} catch (...) { // NOLINT
		            // ~DatabaseInstance resets the log manager before the buffer manager that owns us, so there is
		            // nowhere to report this; a later instance reaps whatever was left behind
	}
}

void TemporaryDirectoryHandle::CleanupTemporaryDirectory() {
	if (temp_directory.empty()) {
		return;
	}
	auto &fs = FileSystem::GetFileSystem(db);
	// only our own: we are their owner, so no process has to be judged to know they are free. What
	// others left behind is the first-spill sweep's business
	fs.RemoveFiles(ListOwnFiles());
	// the claim goes last - while it stands nobody can take our id and create a file under our name
	// that we would then delete out from under them
	fs.TryRemoveFile(fs.JoinPath(temp_directory, TemporaryOwnerMarkerName(owner)));
	if (created_directory) {
		// only if nothing else is in it: another instance may have joined us here, and removing the
		// directory is recursive - it would take their files with it
		fs.RemoveDirectoryExtended(temp_directory, {RemoveDirectoryMode::SINGLE});
	}
}

TemporaryFileManager &TemporaryDirectoryHandle::GetTempFile() const {
	return *temp_file;
}

} // namespace duckdb
