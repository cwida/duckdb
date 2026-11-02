//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/single_file_block_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/storage/block_manager.hpp"
#include "duckdb/storage/block.hpp"
#include "duckdb/storage/storage_options.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/prefetched_file_data.hpp"
#include "duckdb/common/memory_mapped_file.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/set.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/common/encryption_functions.hpp"
#include "duckdb/storage/database_handle.hpp"

namespace duckdb {

class DatabaseInstance;
struct MetadataHandle;
enum class FreeBlockType { NEWLY_USED_BLOCK, CHECKPOINTED_BLOCK };

struct EncryptionOptions {
	//! indicates whether the db is encrypted
	bool encryption_enabled = false;
	//! Whether Additional Authenticated Data is used
	bool additional_authenticated_data = false;
	//! derived encryption key id
	string derived_key_id;
	// //! Cipher used for encryption
	// EncryptionTypes::CipherType cipher = EncryptionTypes::CipherType::INVALID;
	//! key derivation function (kdf) used
	EncryptionTypes::KeyDerivationFunction kdf = EncryptionTypes::KeyDerivationFunction::SHA256;
	//! Key Length
	uint32_t key_length = MainHeader::DEFAULT_ENCRYPTION_KEY_LENGTH;
	//! User key pointer (to StorageOptions)
	shared_ptr<string> user_key;
	//! Version of duckdb-encryption
	EncryptionTypes::EncryptionVersion encryption_version = EncryptionTypes::NONE;
};

struct StorageManagerOptions {
	bool read_only = false;
	FileIOMode io_mode = FileIOMode::BUFFERED_IO;
	//! Reserve size for MMAP mode; empty uses the built-in default.
	optional_idx mmap_reserve_size;
	DebugInitialize debug_initialize = DebugInitialize::NO_INITIALIZE;
	optional_idx block_alloc_size;
	StorageVersion storage_version = StorageVersion::INVALID;
	StorageVersion version_number = StorageVersion::INVALID;
	optional_idx block_header_size;
	//! Unique database identifier and optional encryption salt.
	data_t db_identifier[MainHeader::DB_IDENTIFIER_LEN];
	EncryptionOptions encryption_options;
	//! Header prefetched during file-type detection; DatabaseHandle::Open reuses it. Empty unless opened via ATTACH.
	PrefetchedFileData prefetched;
};

//! SingleFileBlockManager is an implementation for a BlockManager which manages blocks in a single file
class SingleFileBlockManager : public BlockManager {
public:
	//! The location in the file where the block writing starts
	static constexpr uint64_t BLOCK_START = Storage::FILE_HEADER_SIZE * 3;

public:
	SingleFileBlockManager(AttachedDatabase &db_p, const string &path_p, const StorageManagerOptions &options_p);
	~SingleFileBlockManager() override;

	//! Creates a new database.
	void CreateNewDatabase(QueryContext context);
	//! Loads an existing database. We pass the provided block allocation size as a parameter
	//! to detect inconsistencies with the file header.
	void LoadExistingDatabase(QueryContext context);

	//! Creates a new Block using the specified block_id and returns a pointer
	unique_ptr<Block> ConvertBlock(block_id_t block_id, FileBuffer &source_buffer) override;
	unique_ptr<Block> CreateBlock(block_id_t block_id, FileBuffer *source_buffer) override;
	//! Return the next free block id
	block_id_t GetFreeBlockId() override;
	//! Return the next free block id
	block_id_t GetFreeBlockIdForCheckpoint() override;
	//! Check the next free block id - but do not assign or allocate it
	block_id_t PeekFreeBlockId() override;
	//! Returns whether or not a specified block is the root block
	bool IsRootBlock(MetaBlockPointer root) override;
	//! Mark a block as included in a checkpoint
	void MarkBlockAsCheckpointed(block_id_t block_id) override;
	//! Mark a block as used (no longer re-writeable)
	void MarkBlockAsUsed(block_id_t block_id) override;
	//! Mark a block as modified (re-writeable after a checkpoint)
	void MarkBlockAsModified(block_id_t block_id) override;
	//! Increase the reference count of a block. The block should hold at least one reference
	void IncreaseBlockReferenceCount(block_id_t block_id) override;
	//! UnregisterBlock, only accepts non-temporary block ids
	void UnregisterBlock(block_id_t id) override;
	//! Unregister the block id if no live handle is registered for it
	bool UnregisterExpiredBlock(block_id_t id) override;
	//! Return the meta block id
	idx_t GetMetaBlock() override;
	//! Read the content of the block from disk
	void Read(QueryContext context, Block &block) override;

	//! Read individual blocks
	void ReadBlock(Block &block, bool skip_block_header = false) const;
	void ReadBlock(data_ptr_t internal_buffer, uint64_t block_size, bool skip_block_header = false) const;
	//! Read the content of a range of blocks into a buffer
	void ReadBlocks(QueryContext context, FileBuffer &buffer, block_id_t start_block, idx_t block_count) override;
	//! Write the block to disk. Use Write with client context instead.
	void Write(FileBuffer &buffer, block_id_t block_id) override;
	//! Write the block to disk.
	void Write(QueryContext context, FileBuffer &buffer, block_id_t block_id) override;
	//! Write the header to disk, this is the final step of the checkpointing process
	void WriteHeader(QueryContext context, DatabaseHeader header) override;
	//! Sync changes to the underlying file
	void FileSync() override;
	//! Truncate the underlying database file after a checkpoint
	void Truncate() override;

	bool InMemory() override {
		return false;
	}
	//! Returns the number of total blocks
	idx_t TotalBlocks() override;
	//! Returns the number of free blocks
	idx_t FreeBlocks() override;
	//! Whether or not the attached database is a remote file
	bool IsRemote() override;
	//! Whether or not to prefetch
	bool Prefetch() override;

	//! Return the checkpoint iteration of the file.
	uint64_t GetCheckpointIteration() const {
		return iteration_count;
	}
	//! Return the version number of the file.
	StorageVersion GetVersionNumber() const;
	//! Return the database identifier.
	data_ptr_t GetDBIdentifier() {
		return options.db_identifier;
	}

private:
	//! Rewrites the main header
	//! Usage should be avoided
	//! Except for phasing-out the storage version
	void RewriteMainHeader(QueryContext &context,
	                       StorageVersion version_number = MainHeader::DEPRECATED_VERSION_NUMBER);
	//! Loads the free list of the file.
	void LoadFreeList(QueryContext context);

	//! Initializes the database header. We pass the provided block allocation size as a parameter
	//!	to detect inconsistencies with the file header.
	void Initialize(const DatabaseHeader &header, const optional_idx block_alloc_size);

	void CheckChecksum(FileBuffer &block, uint64_t location, uint64_t delta, bool skip_block_header = false) const;
	void CheckChecksum(data_ptr_t start_ptr, uint64_t delta, bool skip_block_header = false) const;

	void ReadAndChecksum(QueryContext context, FileBuffer &handle, uint64_t location,
	                     bool skip_block_header = false) const;
	void ChecksumAndWrite(QueryContext context, FileBuffer &handle, uint64_t location,
	                      bool skip_block_header = false) const;

	idx_t GetBlockLocation(block_id_t block_id) const;

	// Encrypt, Store, Decrypt the canary
	static void StoreEncryptedCanary(AttachedDatabase &db, MainHeader &main_header, const string &key_id);
	static void StoreDBIdentifier(MainHeader &main_header, const data_ptr_t db_identifier);
	void StoreEncryptionMetadata(MainHeader &main_header) const;
	template <typename T>
	static void WriteEncryptionData(MemoryStream &stream, const T &val);

	//! Check and adding Encryption Keys
	void CheckAndAddEncryptionKey(MainHeader &main_header, string &user_key);
	void CheckAndAddEncryptionKey(MainHeader &main_header);

	//! Return the blocks to which we will write the free list and modified blocks
	vector<MetadataHandle> GetFreeListBlocks();
	void TrimFreeBlocks(const set<block_id_t> &blocks);
	void TrimFreeBlockRange(block_id_t start, block_id_t end);

	void IncreaseBlockReferenceCountInternal(block_id_t block_id);

	//! Verify the block usage count
	void VerifyBlocks(const unordered_map<block_id_t, idx_t> &block_usage_count) override;

	void AddStorageVersionTag();

	block_id_t GetFreeBlockIdInternal(FreeBlockType type);
	//! Adds a free block to the free_list, returns true if it was added to the regular free_list
	bool AddFreeBlock(unique_lock<mutex> &lock, block_id_t block_id);
	//! Move the block from free_blocks_in_use to the free list, if present
	void ReleaseFreeBlockInUse(unique_lock<mutex> &lock, block_id_t id);
	//! Returns true, if the block is in the free list. Takes the lock (verification only).
	bool BlockIsFreeListed(block_id_t block_id);

private:
	AttachedDatabase &db;
	//! The active DatabaseHeader, either 0 (h1) or 1 (h2)
	uint8_t active_header;
	//! The path where the file is stored
	string path;
	//! The database handle
	unique_ptr<DatabaseHandle> handle;
	//! The buffer used to read/write to the headers
	FileBuffer header_buffer;
	//! The list of free blocks that can be written to currently
	set<block_id_t> free_list;
	//! The list of blocks that have been freed, but cannot yet be re-used because they are still in-use
	set<block_id_t> free_blocks_in_use;
	//! The list of blocks that are in-use, but haven't been written as part of a checkpoint yet
	set<block_id_t> newly_used_blocks;
	//! The list of multi-use blocks (i.e. blocks that have >1 reference in the file)
	//! When a multi-use block is marked as modified, the reference count is decreased by 1 instead of directly
	//! Appending the block to the modified_blocks list
	unordered_map<block_id_t, uint32_t> multi_use_blocks;
	//! The list of blocks that are no longer in-use, but cannot be re-used until the next checkpoint
	unordered_set<block_id_t> modified_blocks;
	//! The current meta block id
	idx_t meta_block;
	//! The current maximum block id, this id will be given away first after the free_list runs out
	block_id_t max_block;
	//! The block id where the free list can be found
	idx_t free_list_id;
	//! The current header iteration count.
	uint64_t iteration_count;
	//! The storage manager options
	StorageManagerOptions options;
	//! Lock for performing various operations in the single file block manager
	mutex single_file_block_lock;
};
} // namespace duckdb
