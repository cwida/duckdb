#include "duckdb/storage/storage_manager.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/enums/checkpoint_abort.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/valid_checker.hpp"
#include "duckdb/storage/checkpoint_manager.hpp"
#include "duckdb/storage/in_memory_block_manager.hpp"
#include "duckdb/storage/single_file_block_manager.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/storage/table/column_data.hpp"
#include "duckdb/storage/table/in_memory_checkpoint.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/duck_schema_entry.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"
#include "duckdb/storage/data_table.hpp"
#include "mbedtls_wrapper.hpp"
#include "duckdb/common/path.hpp"

namespace duckdb {
using SHA256State = duckdb_mbedtls::MbedTlsWrapper::SHA256State;

void StorageOptions::SetEncryptionVersion(string &storage_version_user_provided) {
	// storage version < v1.4.0
	if (storage_version == StorageVersion::INVALID ||
	    StorageManager::IsPriorToVersion(StorageVersion::V1_4_0, storage_version)) {
		if (!storage_version_user_provided.empty()) {
			throw InvalidInputException("Explicit provided STORAGE_VERSION (\"%s\") and ENCRYPTION_KEY (storage >= "
			                            "v1.4.0) are not compatible",
			                            storage_version_user_provided);
		}
	}

	auto target_encryption_version = encryption_version;

	if (target_encryption_version == EncryptionTypes::NONE) {
		target_encryption_version = EncryptionTypes::V0_1;
	}

	switch (target_encryption_version) {
	case EncryptionTypes::V0_1:
		// without an explicit storage version, new files get the default on creation and existing files keep theirs
		if (storage_version == StorageVersion::INVALID && storage_version_user_provided.empty()) {
			break;
		}
		// storage version set, but v1.4.0 =< storage < v1.5.0
		if (StorageManager::IsPriorToVersion(StorageVersion::V1_5_0, storage_version)) {
			if (!storage_version_user_provided.empty()) {
				if (encryption_version == target_encryption_version) {
					// encryption version is explicitly given, but not compatible with < v1.5.0
					throw InvalidInputException("Explicit provided STORAGE_VERSION (\"%s\") is not compatible with "
					                            "'debug_encryption_version = v1' (storage >= "
					                            "v1.5.0)",
					                            storage_version_user_provided);
				}
			} else {
				// encryption version needs to be lowered, because storage version < v1.5.0
				target_encryption_version = EncryptionTypes::V0_0;
				break;
			}
		}

		break;

	case EncryptionTypes::V0_0:
		// without an explicit storage version, new files get the default on creation and existing files keep theirs
		break;
	default:
		throw InvalidConfigurationException("Encryption version is not set");
	}

	encryption_version = target_encryption_version;
}

void StorageOptions::Initialize(unordered_map<string, Value> &options) {
	string storage_version_user_provided = "";
	for (auto &entry : options) {
		if (entry.first == "block_size") {
			// Extract the block allocation size. This is NOT the actual memory available on a block (block_size),
			// even though the corresponding option we expose to the user is called "block_size".
			block_alloc_size = entry.second.GetValue<uint64_t>();
		} else if (entry.first == "encryption_key") {
			// check the type of the key
			auto type = entry.second.type();
			if (type.id() != LogicalTypeId::VARCHAR) {
				throw BinderException("\"%s\" is not a valid key. A key must be of type VARCHAR",
				                      entry.second.ToString());
			} else if (entry.second.GetValue<string>().empty()) {
				throw BinderException("Not a valid key. A key cannot be empty");
			}
			user_key = make_shared_ptr<string>(StringValue::Get(entry.second.DefaultCastAs(LogicalType::BLOB)));
			block_header_size = DEFAULT_ENCRYPTION_BLOCK_HEADER_SIZE;
			encryption = true;
		} else if (entry.first == "encryption_cipher") {
			auto parsed_cipher = EncryptionTypes::StringToCipher(entry.second.ToString());
			if (parsed_cipher != EncryptionTypes::CipherType::GCM &&
			    parsed_cipher != EncryptionTypes::CipherType::CTR) {
				throw BinderException("\"%s\" is not a valid cipher. Try 'GCM' or 'CTR'.", entry.second.ToString());
			}
			encryption_cipher = parsed_cipher;
		} else if (entry.first == "row_group_size") {
			row_group_size = entry.second.GetValue<uint64_t>();
		} else if (entry.first == "storage_version") {
			storage_version_user_provided = entry.second.ToString();
			storage_version = StorageCompatibility::FromString(storage_version_user_provided).storage_version;
		} else if (entry.first == "compress") {
			if (entry.second.DefaultCastAs(LogicalType::BOOLEAN).GetValue<bool>()) {
				compress_in_memory = CompressInMemory::COMPRESS;
			} else {
				compress_in_memory = CompressInMemory::DO_NOT_COMPRESS;
			}
		} else if (entry.first == "debug_encryption_version") {
			encryption_version = EncryptionTypes::StringToVersion(entry.second.ToString());
		} else if (entry.first == "io_mode") {
			auto io_mode_str = StringUtil::Upper(entry.second.ToString());
			if (io_mode_str == "BUFFERED_IO") {
				io_mode = FileIOMode::BUFFERED_IO;
			} else if (io_mode_str == "MMAP") {
				io_mode = FileIOMode::MMAP;
			} else if (io_mode_str == "DIRECT_IO") {
				io_mode = FileIOMode::DIRECT_IO;
			} else {
				throw BinderException(
				    "Unrecognized IO_MODE \"%s\". Valid values are 'BUFFERED_IO', 'MMAP', or 'DIRECT_IO'.",
				    entry.second.ToString());
			}
		} else if (entry.first == "mmap_reserve_size") {
			const auto reserve_size = DBConfig::ParseMemoryLimitOrMaximum(entry.second.ToString());
			if (reserve_size > NumericCast<idx_t>(NumericLimits<int64_t>::Maximum())) {
				throw BinderException("MMAP_RESERVE_SIZE \"%s\" is out of range", entry.second.ToString());
			}
			mmap_reserve_size = reserve_size;
		} else {
			throw BinderException("Unrecognized option for attach \"%s\"", entry.first);
		}
	}
	// erase encryption settings
	options.erase("encryption_key");
	options.erase("encryption_cipher");
	if (encryption) {
		SetEncryptionVersion(storage_version_user_provided);
	}
}

StorageManager::StorageManager(AttachedDatabase &db, string path_p, AttachOptions &options)
    : db(db), path(std::move(path_p)), read_only(options.access_mode == AccessMode::READ_ONLY), wal_size(0),
      prefetched_file(std::move(options.prefetched)) {
	if (path.empty()) {
		path = IN_MEMORY_PATH;
		return;
	}
	auto &fs = FileSystem::Get(db);
	path = fs.ExpandPath(path);

	storage_options.Initialize(options.options);
}

StorageManager::~StorageManager() {
}

StorageManager &StorageManager::Get(AttachedDatabase &db) {
	return db.GetStorageManager();
}
StorageManager &StorageManager::Get(Catalog &catalog) {
	return StorageManager::Get(catalog.GetAttached());
}

DatabaseInstance &StorageManager::GetDatabase() {
	return db.GetDatabase();
}

idx_t StorageManager::GetWALSize() {
	return wal_size;
}

void StorageManager::AddWALSize(idx_t size) {
	wal_size += size;
}

void StorageManager::SetWALSize(idx_t size) {
	wal_size = size;
}

idx_t StorageManager::GetWALEntriesCount() const {
	return wal_entries_count;
}

void StorageManager::ResetWALEntriesCount() {
	wal_entries_count = 0;
}

void StorageManager::IncrementWALEntriesCount() {
	wal_entries_count++;
}

optional_ptr<WriteAheadLog> StorageManager::GetWAL() {
	if (InMemory() || read_only || !load_complete) {
		return nullptr;
	}
	return wal.get();
}

// comparison used to see whether the storage version is compatible
bool StorageManager::TargetAtLeastVersion(StorageVersion target_version, idx_t storage_version) {
	if (storage_version < static_cast<idx_t>(target_version)) {
		// storage version is lower then required storage version
		return false;
	}
	return true;
}

// comparison used to see whether the storage version is compatible
bool StorageManager::IsPriorToVersion(StorageVersion target_version, StorageVersion storage_version) {
	if (storage_version < target_version) {
		// storage version is lower then required storage version
		return true;
	}
	return false;
}

bool StorageManager::TargetAtLeastVersion(StorageVersion target_version, StorageVersion storage_version) {
	if (storage_version < target_version) {
		// storage version is lower then required storage version
		return false;
	}
	return true;
}

bool StorageManager::HasWAL() const {
	if (InMemory() || read_only || !load_complete) {
		return false;
	}
	return true;
}

bool StorageManager::WALStartCheckpoint(MetaBlockPointer meta_block, CheckpointOptions &options,
                                        ActiveCheckpointWrapper &active_checkpoint) {
	unique_lock<mutex> guard;
	// Lock ordering: commit lock -> transaction lock (in ActiveCheckpointWrapper::Begin)
	if (!options.commit_lock) {
		// not holding the commit lock yet - grab it
		guard = GetCommitLock();
	}
	// drain under the commit lock before the checkpoint transaction starts: its snapshot would
	// otherwise be bounded below commits whose WAL this checkpoint deletes
	auto &transaction_manager = DuckTransactionManager::Get(db);
	transaction_manager.WaitForDurability();
	if (options.type == CheckpointType::FULL_CHECKPOINT &&
	    transaction_manager.GetLastCommit() >= transaction_manager.LowestVisibilityBound()) {
		// an active bounded snapshot still needs state a full checkpoint would vacuum away
		options.type = CheckpointType::CONCURRENT_CHECKPOINT;
	}

	// While holding the commit lock: any new commits are written to the next WAL
	active_checkpoint.Begin(options);

	D_ASSERT(options.checkpoint_id.IsValid());
	DUCKDB_LOG(db.GetDatabase(), TransactionLogType, db, "Start Checkpoint", options.checkpoint_id.GetIndex());
	if (!wal) {
		return false;
	}
	// start the checkpoint process around the WAL
	if (GetWALSize() == 0) {
		// no WAL - we don't need to do anything here
		return false;
	}
	// verify the main WAL is the active WAL currently
	if (wal->GetPath() != wal_path) {
		throw InternalException("Current WAL path \"%s\" does not match base WAL path \"%s\" in WALStartCheckpoint",
		                        wal->GetPath(), wal_path);
	}
	// write to the main WAL that we have initiated a checkpoint
	wal->WriteCheckpoint(meta_block);
	wal->Flush();

	// close the main WAL
	wal.reset();

	// replace the WAL with a new WAL (.checkpoint.wal) that transactions can write to while the checkpoint is happening
	// we don't eagerly write to this WAL - we just instantiate it here so it can be written to
	// if a checkpoint WAL already exists - delete it before proceeding
	auto checkpoint_wal_path = GetCheckpointWALPath();
	auto &fs = FileSystem::Get(db);
	fs.TryRemoveFile(checkpoint_wal_path);

	// the checkpoint WAL belongs to the NEXT checkpoint - when we are done we will overwrite the current WAL with it
	// as such override the checkpoint iteration number to the next one
	auto &single_file_block_manager = GetBlockManager().Cast<SingleFileBlockManager>();
	auto next_checkpoint_iteration = single_file_block_manager.GetCheckpointIteration() + 1;
	wal = make_uniq<WriteAheadLog>(*this, checkpoint_wal_path, 0ULL, WALInitState::NO_WAL, next_checkpoint_iteration);
	return true;
}

void StorageManager::WALFinishCheckpoint(unique_lock<mutex> &) {
	D_ASSERT(wal.get());

	// "wal" points to the checkpoint WAL
	// first check if the checkpoint WAL has been written to
	auto &fs = FileSystem::Get(db);
	if (!wal->Initialized()) {
		// the checkpoint WAL has not been written to
		// this is the common scenario if there are no concurrent writes happening while checkpointing
		// in this case we can just remove the main WAL and re-instantiate it to empty
		fs.TryRemoveFile(wal_path);
		ResetWALEntriesCount();
		wal = make_uniq<WriteAheadLog>(*this, wal_path);
		return;
	}

	// we have had writes to the checkpoint WAL - we need to override our WAL with the checkpoint WAL
	// commits to the checkpoint WAL may still be syncing: drain before destroying the WAL object
	DuckTransactionManager::Get(db).WaitForDurability();
	// first close the WAL writer
	auto checkpoint_wal_path = wal->GetPath();
	wal.reset();

	// move the secondary WAL over the main WAL
	fs.MoveFile(checkpoint_wal_path, wal_path);

	// open what is now the main WAL again
	wal = make_uniq<WriteAheadLog>(*this, wal_path);
	wal->Initialize();

	DUCKDB_LOG(db.GetDatabase(), TransactionLogType, db, "Finish Checkpoint");
}

unique_lock<mutex> StorageManager::GetCommitLock() {
	return unique_lock<mutex>(commit_lock);
}

string StorageManager::GetWALPath(const string &suffix) {
	return Path::AddSuffixToPath(path, suffix);
}

string StorageManager::GetCheckpointWALPath() {
	return GetWALPath(".wal.checkpoint");
}

string StorageManager::GetRecoveryWALPath() {
	return GetWALPath(".wal.recovery");
}

bool StorageManager::InMemory() const {
	D_ASSERT(!path.empty());
	return path == IN_MEMORY_PATH;
}

void StorageManager::Destroy() {
}

inline void ClearUserKey(shared_ptr<string> const &encryption_key) {
	if (encryption_key && !encryption_key->empty()) {
		duckdb_mbedtls::MbedTlsWrapper::AESStateMBEDTLS::SecureClearData(data_ptr_cast(&(*encryption_key)[0]),
		                                                                 encryption_key->size());
		encryption_key->clear();
	}
}

void StorageManager::Initialize(QueryContext context) {
	bool in_memory = InMemory();
	if (in_memory && read_only) {
		throw CatalogException("Cannot launch in-memory database in read-only mode!");
	}

	// Create or load the database from disk, if not in-memory mode.
	LoadDatabase(context);

	if (storage_options.encryption) {
		ClearUserKey(storage_options.user_key);
	}
}

class SingleFileTableIOManager : public TableIOManager {
public:
	explicit SingleFileTableIOManager(BlockManager &block_manager, idx_t row_group_size)
	    : block_manager(block_manager), row_group_size(row_group_size) {
	}

	BlockManager &block_manager;
	idx_t row_group_size;

public:
	BlockManager &GetIndexBlockManager() override {
		return block_manager;
	}
	BlockManager &GetBlockManagerForRowData() override {
		return block_manager;
	}
	MetadataManager &GetMetadataManager() override {
		return block_manager.GetMetadataManager();
	}
	idx_t GetRowGroupSize() const override {
		return row_group_size;
	}
};

SingleFileStorageManager::SingleFileStorageManager(AttachedDatabase &db, string path, AttachOptions &options)
    : StorageManager(db, std::move(path), options) {
}

void SingleFileStorageManager::LoadDatabase(QueryContext context) {
	if (InMemory()) {
		block_manager = make_uniq<InMemoryBlockManager>(BufferManager::GetBufferManager(db), DEFAULT_BLOCK_ALLOC_SIZE,
		                                                DEFAULT_BLOCK_HEADER_STORAGE_SIZE);
		table_io_manager = make_uniq<SingleFileTableIOManager>(*block_manager, DEFAULT_ROW_GROUP_SIZE);
		// in-memory databases can always use the latest storage version
		storage_version = GetLatestStorageVersion();
		load_complete = true;
		return;
	}
	if (storage_options.compress_in_memory != CompressInMemory::AUTOMATIC) {
		throw InvalidInputException("COMPRESS can only be set for in-memory databases");
	}

	auto &fs = FileSystem::Get(db);
	auto &config = DBConfig::Get(db);

	StorageManagerOptions options;
	options.read_only = read_only;
	// MMAP + encryption would corrupt the file (in-place decryption); demote to BUFFERED_IO.
	auto resolved_io_mode = storage_options.io_mode ? *storage_options.io_mode : FileIOMode::BUFFERED_IO;
	if (storage_options.encryption && resolved_io_mode == FileIOMode::MMAP) {
		DUCKDB_LOG_WARNING(db.GetDatabase(),
		                   "MMAP IO_MODE is incompatible with encryption; falling back to BUFFERED_IO for \"%s\"",
		                   path);
		resolved_io_mode = FileIOMode::BUFFERED_IO;
	}
	options.io_mode = resolved_io_mode;
	options.mmap_reserve_size = storage_options.mmap_reserve_size;
	options.debug_initialize = config.options.debug_initialize;
	options.storage_version = storage_options.storage_version;

	if (storage_options.encryption) {
		// key is given upon ATTACH
		D_ASSERT(storage_options.block_header_size == DEFAULT_ENCRYPTION_BLOCK_HEADER_SIZE);
		options.encryption_options.encryption_enabled = true;
		options.encryption_options.user_key = std::move(storage_options.user_key);
		options.encryption_options.encryption_version = storage_options.encryption_version;
	}

	idx_t row_group_size = DEFAULT_ROW_GROUP_SIZE;
	if (storage_options.row_group_size.IsValid()) {
		row_group_size = storage_options.row_group_size.GetIndex();
		if (row_group_size == 0) {
			throw NotImplementedException("Invalid row group size: %llu - row group size must be bigger than 0",
			                              row_group_size);
		}
		if (row_group_size % STANDARD_VECTOR_SIZE != 0) {
			throw NotImplementedException(
			    "Invalid row group size: %llu - row group size must be divisible by the vector size (%llu)",
			    row_group_size, STANDARD_VECTOR_SIZE);
		}
	}
	// Check if the database file already exists.
	// Note: a file can also exist if there was a ROLLBACK on a previous transaction creating that file.
	if (!read_only && !fs.FileExists(path)) {
		// file does not exist and we are in read-write mode
		// create a new file

		wal_path = GetWALPath();
		// try to remove the WAL file if it exists
		fs.TryRemoveFile(wal_path);

		// Set the block allocation size for the new database file.
		if (storage_options.block_alloc_size.IsValid()) {
			// Use the option provided by the user.
			Storage::VerifyBlockAllocSize(storage_options.block_alloc_size.GetIndex());
			options.block_alloc_size = storage_options.block_alloc_size;
		} else {
			// No explicit option provided: use the default option.
			options.block_alloc_size = Settings::Get<DefaultBlockSizeSetting>(config);
		}
		//! set the block header size for the encrypted database files
		//! set the database to encrypted
		//! update the storage version to 1.4.0
		if (storage_options.block_header_size.IsValid()) {
			// Use the header size for the corresponding encryption algorithm.
			Storage::VerifyBlockHeaderSize(storage_options.block_header_size.GetIndex());
			options.block_header_size = storage_options.block_header_size;
			options.storage_version = storage_options.storage_version;
		} else {
			// No encryption; use the default option.
			options.block_header_size = config.options.default_block_header_size;
		}

		if (options.storage_version == StorageVersion::INVALID) {
			// when creating a new database we default to the serialization version specified in the config
			options.storage_version = config.options.storage_compatibility.storage_version;
			if (storage_options.encryption &&
			    StorageManager::IsPriorToVersion(StorageVersion::V1_5_0, options.storage_version)) {
				// the default encryption format needs at least v1.5.0
				options.storage_version = StorageVersion::V1_5_0;
			}
		}

		// Initialize the block manager before creating a new database.
		auto sf_block_manager = make_uniq<SingleFileBlockManager>(db, path, options);
		sf_block_manager->CreateNewDatabase(context);
		block_manager = std::move(sf_block_manager);
		table_io_manager = make_uniq<SingleFileTableIOManager>(*block_manager, row_group_size);
		wal = make_uniq<WriteAheadLog>(*this, wal_path);

	} else {
		// Either the file exists, or we are in read-only mode, so we
		// try to read the existing file on disk.

		// set the block header size for the encrypted database files
		// (also if they already exist)
		if (storage_options.encryption) {
			options.encryption_options.encryption_enabled = true;
			D_ASSERT(storage_options.block_header_size == DEFAULT_ENCRYPTION_BLOCK_HEADER_SIZE);
		}
		if (storage_options.block_header_size.IsValid()) {
			Storage::VerifyBlockHeaderSize(storage_options.block_header_size.GetIndex());
			options.block_header_size = storage_options.block_header_size;
			options.storage_version = storage_options.storage_version;
		} else {
			// No explicit option provided: use the default option.
			options.block_header_size = config.options.default_block_header_size;
		}

		// Carry the prefetched header into the block manager options so the initial header reads hit memory.
		options.prefetched = std::move(prefetched_file);

		// Initialize the block manager while loading the database file.
		// We'll construct the SingleFileBlockManager with the default block allocation size,
		// and later adjust it when reading the file header.
		auto sf_block_manager = make_uniq<SingleFileBlockManager>(db, path, options);
		sf_block_manager->LoadExistingDatabase(context);
		block_manager = std::move(sf_block_manager);
		table_io_manager = make_uniq<SingleFileTableIOManager>(*block_manager, row_group_size);

		if (storage_options.block_alloc_size.IsValid()) {
			// user-provided block alloc size
			idx_t block_alloc_size = storage_options.block_alloc_size.GetIndex();
			if (block_alloc_size != block_manager->GetBlockAllocSize()) {
				throw InvalidInputException(
				    "block size parameter does not match the file's block size, got %llu, expected %llu",
				    storage_options.block_alloc_size.GetIndex(), block_manager->GetBlockAllocSize());
			}
		}

		if (storage_options.block_header_size.IsValid()) {
			// block header size for encrypted database files
			idx_t block_header_size = storage_options.block_header_size.GetIndex();
			if (block_header_size != block_manager->GetBlockHeaderSize()) {
				throw InvalidInputException(
				    "block header size does not match the file's block header size, got %llu, expected %llu",
				    storage_options.block_header_size.GetIndex(), block_manager->GetBlockHeaderSize());
			}
		}

		unique_ptr<MetricsTimer> timer = nullptr;

		// Start timing the storage load step.
		auto client_context = context.GetClientContext();
		if (client_context) {
			auto profiler = client_context->client_data->profiler;
			timer = make_uniq<MetricsTimer>(profiler->StartTimer<MetricStorageAttachLoadStorageLatency>());
		}

		// Load the checkpoint from storage.
		auto checkpoint_reader = SingleFileCheckpointReader(*this);
		checkpoint_reader.LoadFromStorage();

		// Reset the timer (also ends it).
		if (timer) {
			timer = nullptr;
		}

		// Start timing the WAL replay step.
		if (client_context) {
			auto profiler = client_context->client_data->profiler;
			timer = make_uniq<MetricsTimer>(profiler->StartTimer<MetricStorageAttachReplayWALLatency>());
		}

		// Replay the WAL.
		wal_path = GetWALPath();
		wal = WriteAheadLog::Replay(context, *this, wal_path);

		// Timer will go out of scope here, if set.
	}

	//
	if (row_group_size > 122880ULL && IsPriorToVersion(StorageVersion::V1_2_0, GetStorageVersion())) {
		throw InvalidInputException("Unsupported row group size %llu - row group sizes >= 122_880 are only supported "
		                            "with STORAGE_VERSION '1.2.0' or above.\nExplicitly specify a newer storage "
		                            "version when creating the database to enable larger row groups",
		                            row_group_size);
	}

	load_complete = true;
}

///////////////////////////////////////////////////////////////////////////////

enum class WALCommitState { IN_PROGRESS, FLUSHED, TRUNCATED };

struct OptimisticallyWrittenRowGroupData {
	OptimisticallyWrittenRowGroupData(idx_t start, idx_t count, unique_ptr<PersistentCollectionData> row_group_data_p)
	    : start(start), count(count), row_group_data(std::move(row_group_data_p)) {
	}

	idx_t start;
	idx_t count;
	unique_ptr<PersistentCollectionData> row_group_data;
};

class SingleFileStorageCommitState : public StorageCommitState {
public:
	SingleFileStorageCommitState(StorageManager &storage, WriteAheadLog &log);
	~SingleFileStorageCommitState() override;

	//! Revert the commit
	void RevertCommit() override;
	// Make the commit persistent
	idx_t FlushCommit(bool sync_now) override;

	void AddRowGroupData(DataTable &table, idx_t start_index, idx_t count,
	                     unique_ptr<PersistentCollectionData> row_group_data) override;
	optional_ptr<PersistentCollectionData> GetRowGroupData(DataTable &table, idx_t start_index, idx_t &count) override;
	bool HasRowGroupData() override;

private:
	StorageManager &storage;
	idx_t initial_wal_size = 0;
	idx_t initial_written = 0;
	WriteAheadLog &wal;
	WALCommitState state;
	reference_map_t<DataTable, unordered_map<idx_t, OptimisticallyWrittenRowGroupData>> optimistically_written_data;
};

SingleFileStorageCommitState::SingleFileStorageCommitState(StorageManager &storage, WriteAheadLog &wal)
    : storage(storage), wal(wal), state(WALCommitState::IN_PROGRESS) {
	auto initial_size = storage.GetWALSize();
	initial_written = wal.GetTotalWritten();
	initial_wal_size = initial_size;
}

SingleFileStorageCommitState::~SingleFileStorageCommitState() {
	if (state != WALCommitState::IN_PROGRESS) {
		return;
	}
	try {
		// Truncate the WAL in case of a destructor.
		RevertCommit();
	} catch (std::exception &ex) {
		ErrorData data(ex);
		try {
			DUCKDB_LOG_ERROR(wal.GetDatabase().GetDatabase(),
			                 "SingleFileStorageCommitState::~SingleFileStorageCommitState()\t\t" + data.Message());
		} catch (...) { // NOLINT
		}
	} catch (...) { // NOLINT
	}
}

void SingleFileStorageCommitState::RevertCommit() {
	if (state != WALCommitState::IN_PROGRESS) {
		return;
	}
	if (wal.GetTotalWritten() > initial_written) {
		// remove any entries written into the WAL by truncating it
		wal.Truncate(initial_wal_size);
	}
	auto &block_manager = storage.GetBlockManager();
	for (auto &entry : optimistically_written_data) {
		for (auto &rg_entry : entry.second) {
			if (rg_entry.second.row_group_data) {
				for (auto &block_id : rg_entry.second.row_group_data->GetBlockIds()) {
					block_manager.MarkBlockAsModified(block_id);
				}
			}
		}
	}
	state = WALCommitState::TRUNCATED;
}

idx_t SingleFileStorageCommitState::FlushCommit(bool sync_now) {
	if (state != WALCommitState::IN_PROGRESS) {
		return 0;
	}

	// Move the blocks in this COMMIT into the WAL and mark them as "in use".
	auto abort_mode = Settings::Get<DebugCheckpointAbortSetting>(storage.GetDatabase());
	if (wal.Initialized() && abort_mode == CheckpointAbort::DEBUG_ABORT_BEFORE_WAL_FLUSH) {
		auto &writer = wal.Initialize();
		writer.Sync();
		storage.SetWALSize(writer.GetFileSize());
		ValidChecker::Invalidate(storage.GetDatabase(), "Simulated crash before WAL_FLUSH write");
		ValidChecker::Invalidate(storage.GetAttached(), "Simulated crash before WAL_FLUSH write");
		// Prevent `RevertCommit` from truncating the WAL so the torn records are kept for crash recovery
		state = WALCommitState::FLUSHED;
		throw FatalException("Simulated crash before WAL_FLUSH write");
	}

	idx_t wal_sync_offset = 0;
	if (sync_now) {
		wal.Flush();
	} else {
		// only the marker is written here: the sync happens once the locks are released
		wal_sync_offset = wal.FlushMarker();
	}
	state = WALCommitState::FLUSHED;
	return wal_sync_offset;
}

void SingleFileStorageCommitState::AddRowGroupData(DataTable &table, idx_t start_index, idx_t count,
                                                   unique_ptr<PersistentCollectionData> row_group_data) {
	if (row_group_data->HasUpdates()) {
		// cannot serialize optimistic block pointers if in-memory updates exist
		return;
	}
	auto &entries = optimistically_written_data[table];
	auto entry = entries.find(start_index);
	if (entry != entries.end()) {
		throw InternalException("FIXME: AddOptimisticallyWrittenRowGroup is writing a duplicate row group");
	}
	entries.insert(
	    make_pair(start_index, OptimisticallyWrittenRowGroupData(start_index, count, std::move(row_group_data))));
}

optional_ptr<PersistentCollectionData> SingleFileStorageCommitState::GetRowGroupData(DataTable &table,
                                                                                     idx_t start_index, idx_t &count) {
	auto entry = optimistically_written_data.find(table);
	if (entry == optimistically_written_data.end()) {
		// no data for this table
		return nullptr;
	}
	auto &row_groups = entry->second;
	auto start_entry = row_groups.find(start_index);
	if (start_entry == row_groups.end()) {
		// this row group was not optimistically written
		return nullptr;
	}
	count = start_entry->second.count;
	return start_entry->second.row_group_data.get();
}

bool SingleFileStorageCommitState::HasRowGroupData() {
	return !optimistically_written_data.empty();
}

unique_ptr<StorageCommitState> SingleFileStorageManager::GenStorageCommitState(WriteAheadLog &wal) {
	return make_uniq<SingleFileStorageCommitState>(*this, wal);
}

bool SingleFileStorageManager::IsCheckpointClean(MetaBlockPointer checkpoint_id) {
	return block_manager->IsRootBlock(checkpoint_id);
}

unique_ptr<CheckpointWriter> SingleFileStorageManager::CreateCheckpointWriter(QueryContext context,
                                                                              CheckpointOptions options) {
	if (InMemory()) {
		return make_uniq<InMemoryCheckpointer>(context, db, *block_manager, *this, options);
	}
	return make_uniq<SingleFileCheckpointWriter>(context, db, *block_manager, options);
}

void SingleFileStorageManager::CreateCheckpoint(QueryContext context, CheckpointOptions options) {
	if (read_only || !load_complete) {
		return;
	}
	unique_ptr<StorageLockKey> vacuum_lock;
	if (options.type != CheckpointType::CONCURRENT_CHECKPOINT) {
		auto &transaction_manager = GetAttached().GetTransactionManager().Cast<DuckTransactionManager>();
		vacuum_lock = transaction_manager.TryGetVacuumLock();
		if (!vacuum_lock) {
			if (options.type == CheckpointType::FULL_CHECKPOINT) {
				options.type = CheckpointType::CONCURRENT_CHECKPOINT;
			} else {
				// nothing to do
				return;
			}
		}
	}

	if (db.GetStorageExtension()) {
		db.GetStorageExtension()->OnCheckpointStart(db, options);
	}

	auto &config = DBConfig::Get(db);
	// We only need to checkpoint if there is anything in the WAL.
	auto wal_size = GetWALSize();
	if (wal_size > 0 || config.options.force_checkpoint || options.action == CheckpointAction::ALWAYS_CHECKPOINT) {
		try {
			// Start timing the checkpoint.
			auto client_context = context.GetClientContext();
			MetricsTimer timer;
			if (client_context) {
				timer = client_context->client_data->profiler->StartTimer<MetricStorageCheckpointLatency>();
			}

			// Write the checkpoint.
			auto checkpointer = CreateCheckpointWriter(context, options);
			checkpointer->CreateCheckpoint();
			timer.EndTimer();

		} catch (std::exception &ex) {
			ErrorData error(ex);
			if (db.IsInitialDatabase()) {
				ValidChecker::Invalidate(db.GetDatabase(), error.RawMessage());
				throw FatalException("Failed to create checkpoint because of error: %s", error.RawMessage());
			}
			// A non-initial database can be detached and reattached, so scope invalidation to this db.
			db.Invalidate(error.RawMessage());
			throw IOException("Checkpoint failed for database %s. The database has been invalidated. Original "
			                  "error: %s",
			                  db.GetName(), error.RawMessage());
		}
	}

	if (db.GetStorageExtension()) {
		db.GetStorageExtension()->OnCheckpointEnd(db, options);
	}
}

void SingleFileStorageManager::Destroy() {
	if (!load_complete) {
		return;
	}
	vector<reference<DuckSchemaEntry>> schemas;
	// we scan the set of committed schemas
	auto &catalog = Catalog::GetCatalog(db).Cast<DuckCatalog>();
	catalog.ScanSchemas(VisibilityBound::AllCommitted(), [&](DuckSchemaEntry &entry) { schemas.push_back(entry); });

	vector<reference<DuckTableEntry>> tables;
	for (auto &schema : schemas) {
		schema.get().Scan(CatalogType::TABLE_ENTRY, VisibilityBound::AllCommitted(), [&](CatalogEntry &entry) {
			if (entry.internal) {
				return;
			}
			if (entry.type == CatalogType::TABLE_ENTRY) {
				tables.push_back(entry.Cast<DuckTableEntry>());
			}
		});
	}

	for (auto &table : tables) {
		auto &data_table = table.get().GetStorage();
		data_table.Destroy();
	}
}

DatabaseSize SingleFileStorageManager::GetDatabaseSize() {
	// All members default to zero
	DatabaseSize ds;
	if (!InMemory()) {
		ds.total_blocks = block_manager->TotalBlocks();
		ds.block_size = block_manager->GetBlockAllocSize();
		ds.free_blocks = block_manager->FreeBlocks();
		ds.used_blocks = ds.total_blocks - ds.free_blocks;
		ds.bytes = (ds.total_blocks * ds.block_size);
		ds.wal_size = NumericCast<idx_t>(GetWALSize());
	}
	return ds;
}

vector<MetadataBlockInfo> SingleFileStorageManager::GetMetadataInfo() {
	auto &metadata_manager = block_manager->GetMetadataManager();
	return metadata_manager.GetMetadataInfo();
}

bool SingleFileStorageManager::AutomaticCheckpoint(idx_t estimated_wal_bytes) {
	auto &config = DBConfig::Get(db).options;

	// Check size-based threshold
	auto initial_size = NumericCast<idx_t>(GetWALSize());
	idx_t expected_wal_size = initial_size + estimated_wal_bytes;
	if (expected_wal_size > config.checkpoint_wal_size) {
		return true;
	}

	// Check entry-based threshold (if enabled)
	auto entry_limit = Settings::Get<WalAutocheckpointEntriesSetting>(DBConfig::Get(db));
	if (entry_limit > 0 && GetWALEntriesCount() >= entry_limit) {
		return true;
	}

	return false;
}

shared_ptr<TableIOManager> SingleFileStorageManager::GetTableIOManager(BoundCreateTableInfo *info /*info*/) {
	// This is an unmanaged reference. No ref/deref overhead. Lifetime of the
	// TableIoManager follows lifetime of the StorageManager (this).
	return shared_ptr<TableIOManager>(shared_ptr<char>(nullptr), table_io_manager.get());
}

BlockManager &SingleFileStorageManager::GetBlockManager() {
	return *block_manager;
}

} // namespace duckdb
