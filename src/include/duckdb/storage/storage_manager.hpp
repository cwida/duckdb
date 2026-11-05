//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/storage_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/prefetched_file_data.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/storage/table_io_manager.hpp"
#include "duckdb/storage/write_ahead_log.hpp"
#include "duckdb/storage/database_size.hpp"
#include "duckdb/storage/checkpoint/checkpoint_options.hpp"
#include "duckdb/storage/storage_options.hpp"

namespace duckdb {
struct AttachOptions;
class ActiveCheckpointWrapper;
class BlockManager;
class Catalog;
class CheckpointWriter;
class DataTable;
class DatabaseInstance;
class TransactionManager;
class TableCatalogEntry;
struct PersistentCollectionData;

class StorageCommitState {
public:
	// Destruction of this object, without prior call to FlushCommit,
	// will roll back the committed changes.
	virtual ~StorageCommitState() {
	}

	//! Revert the commit
	virtual void RevertCommit() = 0;
	//! Write the commit's WAL flush marker. With sync_now the WAL is synced up to it before returning;
	//! otherwise the marker is only pushed to the OS and the WAL offset to sync up to is returned (0 if
	//! nothing was written, or it was synced here)
	virtual idx_t FlushCommit(bool sync_now) = 0;

	virtual void AddRowGroupData(DataTable &table, idx_t start_index, idx_t count,
	                             unique_ptr<PersistentCollectionData> row_group_data) = 0;
	virtual optional_ptr<PersistentCollectionData> GetRowGroupData(DataTable &table, idx_t start_index,
	                                                               idx_t &count) = 0;
	virtual bool HasRowGroupData() {
		return false;
	}
};

//! StorageManager is responsible for managing the physical storage of a persistent database.
class StorageManager {
public:
	StorageManager(AttachedDatabase &db, string path, AttachOptions &options);
	virtual ~StorageManager();

public:
	static StorageManager &Get(AttachedDatabase &db);
	static StorageManager &Get(Catalog &catalog);

	//! Initialize a database or load an existing database from the database file path. The block_alloc_size is
	//! either set, or invalid. If invalid, then DuckDB defaults to the default_block_alloc_size (DBConfig),
	//! or the file's block allocation size, if it is an existing database.
	void Initialize(QueryContext context);

	static bool TargetAtLeastVersion(StorageVersion version, idx_t target_version);
	static bool TargetAtLeastVersion(StorageVersion version, StorageVersion target_version);
	static bool IsPriorToVersion(StorageVersion version, StorageVersion target_version);

	DatabaseInstance &GetDatabase();
	AttachedDatabase &GetAttached() const {
		return db;
	}

	//! Gets the size of the WAL, or zero, if there is no WAL.
	idx_t GetWALSize();
	bool HasWAL() const;
	void AddWALSize(idx_t size);
	void SetWALSize(idx_t size);
	//! Gets the number of WAL entries since last checkpoint
	idx_t GetWALEntriesCount() const;
	void ResetWALEntriesCount();
	void IncrementWALEntriesCount();
	//! Gets the WAL of the StorageManager, or nullptr, if there is no WAL.
	optional_ptr<WriteAheadLog> GetWAL();
	//! Write that we started a checkpoint to the WAL if there is one - returns whether or not there is a WAL
	bool WALStartCheckpoint(MetaBlockPointer meta_block, CheckpointOptions &options,
	                        ActiveCheckpointWrapper &active_checkpoint);
	//! Finishes a checkpoint
	void WALFinishCheckpoint(unique_lock<mutex> &commit_lock);
	//! Acquires commit_lock
	unique_lock<mutex> GetCommitLock();

	//! Returns the database file path
	string GetDBPath() const {
		return path;
	}
	bool IsLoaded() const {
		return load_complete;
	}
	//! The path to the WAL, derived from the database file path
	string GetWALPath(const string &suffix = ".wal");
	//! The path to the WAL that is used while a checkpoint is running
	string GetCheckpointWALPath();
	//! The path to the WAL that is used while recovering from a crash involving the checkpoint WAL
	string GetRecoveryWALPath();
	bool InMemory() const;

	virtual bool AutomaticCheckpoint(idx_t estimated_wal_bytes) = 0;
	virtual unique_ptr<StorageCommitState> GenStorageCommitState(WriteAheadLog &wal) = 0;
	virtual bool IsCheckpointClean(MetaBlockPointer checkpoint_id) = 0;
	virtual void CreateCheckpoint(QueryContext context, CheckpointOptions options = CheckpointOptions()) = 0;
	virtual DatabaseSize GetDatabaseSize() = 0;
	virtual vector<MetadataBlockInfo> GetMetadataInfo() = 0;
	virtual shared_ptr<TableIOManager> GetTableIOManager(BoundCreateTableInfo *info) = 0;
	virtual BlockManager &GetBlockManager() = 0;
	virtual void Destroy();

	void SetStorageVersion(const StorageVersion &version) {
		storage_version = version;
	}
	StorageVersion GetLatestStorageVersion() const {
		return StorageVersion::LATEST;
	}
	StorageVersion GetStorageVersion() const {
		D_ASSERT(HasStorageVersion());
		return storage_version;
	}
	bool HasStorageVersion() const {
		return storage_version != StorageVersion::INVALID;
	}
	string_t GetDuckDBVersionString() const {
		D_ASSERT(HasStorageVersion());
		return StorageVersionInfo::GetStorageVersionString(storage_version);
	}
	bool CompressionIsEnabled() const {
		return storage_options.compress_in_memory == CompressInMemory::COMPRESS;
	}
	EncryptionTypes::CipherType GetCipher() const {
		return storage_options.encryption_cipher;
	}
	void SetCipher(EncryptionTypes::CipherType cipher_p) {
		D_ASSERT(cipher_p != EncryptionTypes::INVALID);
		if (cipher_p == EncryptionTypes::CBC) {
			throw InvalidInputException("CBC cipher is disabled");
		}
		storage_options.encryption_cipher = cipher_p;
	}

	void SetEncryptionVersion(EncryptionTypes::EncryptionVersion version) {
		storage_options.encryption_version = version;
	}

	bool IsEncrypted() const {
		return storage_options.encryption;
	}

	EncryptionTypes::EncryptionVersion GetEncryptionVersion() const {
		return storage_options.encryption_version;
	}

protected:
	virtual void LoadDatabase(QueryContext context) = 0;

protected:
	//! The attached database managed by this storage manager.
	AttachedDatabase &db;
	//! The path of the database
	string path;
	//! The WAL path
	string wal_path;
	//! The WriteAheadLog of the storage manager
	unique_ptr<WriteAheadLog> wal;
	//! Held by every commit with changes from its append to its commit or revert, and by a checkpoint at start and end
	mutex commit_lock;
	//! Whether or not the database is opened in read-only mode
	bool read_only;
	//! When loading a database, we do not yet set the wal-field. Therefore, GetWriteAheadLog must
	//! return nullptr when loading a database
	bool load_complete = false;
	//! The storage compatibility version when reading and writing from this database
	StorageVersion storage_version;
	//! Estimated size of changes for determining automatic checkpointing on in-memory databases and databases without a
	//! WAL.
	atomic<idx_t> wal_size;
	atomic<idx_t> wal_entries_count;
	//! Storage options passed in through configuration
	StorageOptions storage_options;
	//! Header prefetched during file-type detection, consumed by LoadDatabase. Empty unless a DuckDB file via ATTACH.
	PrefetchedFileData prefetched_file;

public:
	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}
};

//! Stores the database in a single file.
class SingleFileStorageManager : public StorageManager {
public:
	SingleFileStorageManager() = delete;
	SingleFileStorageManager(AttachedDatabase &db, string path, AttachOptions &options);

	//! The BlockManager to read from and write to blocks, both for the metadata and the data itself.
	unique_ptr<BlockManager> block_manager;
	//! The table I/O manager.
	unique_ptr<TableIOManager> table_io_manager;

public:
	bool AutomaticCheckpoint(idx_t estimated_wal_bytes) override;
	unique_ptr<StorageCommitState> GenStorageCommitState(WriteAheadLog &wal) override;
	bool IsCheckpointClean(MetaBlockPointer checkpoint_id) override;
	void CreateCheckpoint(QueryContext context, CheckpointOptions options) override;
	DatabaseSize GetDatabaseSize() override;
	vector<MetadataBlockInfo> GetMetadataInfo() override;
	shared_ptr<TableIOManager> GetTableIOManager(BoundCreateTableInfo *info) override;
	BlockManager &GetBlockManager() override;
	void Destroy() override;

protected:
	void LoadDatabase(QueryContext context) override;

	unique_ptr<CheckpointWriter> CreateCheckpointWriter(QueryContext context, CheckpointOptions options);
};
} // namespace duckdb
