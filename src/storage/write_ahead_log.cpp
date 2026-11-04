#include "duckdb/storage/write_ahead_log.hpp"

#include "duckdb/catalog/catalog_entry/duck_index_entry.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/trigger_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/scalar_macro_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/type_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/common/checksum.hpp"
#include "duckdb/common/thread.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/common/encryption_functions.hpp"
#include "duckdb/common/encryption_key_manager.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/storage/single_file_block_manager.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/storage/table/column_data.hpp"
#include "duckdb/storage/table/data_table_info.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/wal_entry.hpp"
#include "duckdb/main/attached_database.hpp"

namespace duckdb {

constexpr uint64_t WAL_VERSION_NUMBER = 2;
constexpr uint64_t WAL_ENCRYPTED_VERSION_NUMBER = 3;

WriteAheadLog::WriteAheadLog(StorageManager &storage_manager, const string &wal_path, idx_t wal_size,
                             WALInitState init_state, optional_idx checkpoint_iteration)
    : storage_manager(storage_manager), wal_path(wal_path), init_state(init_state),
      checkpoint_iteration(checkpoint_iteration) {
	storage_manager.SetWALSize(wal_size);
	storage_manager.ResetWALEntriesCount();
}

WriteAheadLog::~WriteAheadLog() {
}

AttachedDatabase &WriteAheadLog::GetDatabase() {
	return storage_manager.GetAttached();
}

StorageManager &WriteAheadLog::GetStorageManager() {
	return storage_manager;
}

BufferedFileWriter &WriteAheadLog::Initialize() {
	if (Initialized()) {
		return *writer;
	}
	lock_guard<mutex> lock(wal_lock);
	if (!writer) {
		writer =
		    make_uniq<BufferedFileWriter>(FileSystem::Get(GetDatabase()), wal_path,
		                                  FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE |
		                                      FileFlags::FILE_FLAGS_APPEND | FileFlags::FILE_FLAGS_MULTI_CLIENT_ACCESS);
		if (init_state == WALInitState::UNINITIALIZED_REQUIRES_TRUNCATE) {
			writer->Truncate(storage_manager.GetWALSize());
		} else {
			storage_manager.SetWALSize(writer->GetFileSize());
		}
		init_state = WALInitState::INITIALIZED;
	}
	return *writer;
}

idx_t WriteAheadLog::GetTotalWritten() const {
	if (!Initialized()) {
		return 0;
	}
	return writer->GetTotalWritten();
}

void WriteAheadLog::Truncate(idx_t size) {
	if (init_state == WALInitState::NO_WAL) {
		// no WAL to truncate
		return;
	}
	if (!Initialized()) {
		init_state = WALInitState::UNINITIALIZED_REQUIRES_TRUNCATE;
		storage_manager.SetWALSize(size);
		return;
	}
	writer->Truncate(size);
	storage_manager.SetWALSize(writer->GetFileSize());
}

bool WriteAheadLog::Initialized() const {
	return init_state == WALInitState::INITIALIZED;
}

//===--------------------------------------------------------------------===//
// Serializer
//===--------------------------------------------------------------------===//
class ChecksumWriter : public WriteStream {
public:
	explicit ChecksumWriter(WriteAheadLog &wal) : wal(wal), memory_stream(BufferAllocator::Get(wal.GetDatabase())) {
	}

	void WriteData(const_data_ptr_t buffer, idx_t write_size) override {
		// buffer data into the memory stream
		memory_stream.WriteData(buffer, write_size);
	}

	void Flush() {
		if (!stream) {
			stream = wal.Initialize();
		}

		// if the config.encrypt WAL is true
		// and if the attached database is encrypted
		// then encrypt WAL before flushing
		auto &catalog = wal.GetDatabase().GetCatalog().Cast<DuckCatalog>();

		if (catalog.GetIsEncrypted()) {
			return FlushEncrypted();
		}

		auto data = memory_stream.GetData();
		auto size = memory_stream.GetPosition();
		// compute the checksum over the entry
		auto checksum = Checksum(data, size);
		// write the checksum and the length of the entry
		stream->Write<uint64_t>(size);
		stream->Write<uint64_t>(checksum);
		// write data to the underlying stream
		stream->WriteData(memory_stream.GetData(), memory_stream.GetPosition());
		// rewind the buffer
		memory_stream.Rewind();
	}

	void FlushEncrypted() {
		auto &catalog = wal.GetDatabase().GetCatalog().Cast<DuckCatalog>();
		auto encryption_key_id = catalog.GetEncryptionKeyId();

		auto data = memory_stream.GetData();
		auto size = memory_stream.GetPosition();

		// compute the checksum over the entry
		auto checksum = Checksum(data, size);

		auto &db = wal.GetDatabase();
		auto &keys = EncryptionKeyManager::Get(db.GetDatabase());
		auto metadata = make_uniq<EncryptionStateMetadata>(db.GetStorageManager().GetCipher(),
		                                                   MainHeader::DEFAULT_ENCRYPTION_KEY_LENGTH,
		                                                   EncryptionTypes::EncryptionVersion::V0_1);
		auto encryption_state =
		    db.GetDatabase().GetEncryptionUtil(db.IsReadOnly())->CreateEncryptionState(std::move(metadata));

		// temp buffer
		const idx_t ciphertext_size = size + sizeof(uint64_t);
		auto temp_buf = BufferAllocator::Get(db).Allocate(ciphertext_size);

		EncryptionNonce nonce(db.GetStorageManager().GetCipher(), db.GetStorageManager().GetEncryptionVersion());
		EncryptionTag tag;

		// generate nonce
		encryption_state->GenerateRandomData(nonce.data(), nonce.size());

		stream->Write<uint64_t>(size);
		stream->WriteData(nonce.data(), nonce.size());

		//! store the checksum in the temp buffer
		memcpy(temp_buf.get(), &checksum, sizeof(checksum));
		//! checksum + entry in the temp buf
		memcpy(temp_buf.get() + sizeof(checksum), memory_stream.GetData(), memory_stream.GetPosition());

		//! encrypt the temp buf
		encryption_state->InitializeEncryption(nonce, keys.GetKey(encryption_key_id));
		encryption_state->Process(temp_buf.get(), ciphertext_size, temp_buf.get(), ciphertext_size);

		//! calculate the tag (for GCM)
		encryption_state->Finalize(temp_buf.get(), ciphertext_size, tag.data(), tag.size());

		// write data to the underlying stream
		stream->WriteData(temp_buf.get(), ciphertext_size);

		// Write the tag to the stream
		if (encryption_state->GetCipher() == EncryptionTypes::CipherType::GCM) {
			D_ASSERT(!tag.IsAllZeros());
			stream->WriteData(tag.data(), tag.size());
		}

		// rewind the buffer
		memory_stream.Rewind();
	}

	WriteAheadLog &GetWAL() {
		return wal;
	}

private:
	WriteAheadLog &wal;
	optional_ptr<WriteStream> stream;
	MemoryStream memory_stream;
};

class WriteAheadLogSerializer {
public:
	WriteAheadLogSerializer(WriteAheadLog &wal, WALType wal_type)
	    : checksum_writer(wal), serializer(checksum_writer, SerializationOptions(wal.GetDatabase())) {
		if (!wal.Initialized()) {
			wal.Initialize();
		}
		// Write a header, if none has been written yet.
		wal.WriteHeader();
		serializer.Begin();
		serializer.WriteProperty(100, "wal_type", wal_type);
	}

	void End() {
		serializer.End();
		checksum_writer.Flush();
		checksum_writer.GetWAL().IncrementWALEntriesCount();
	}

	template <class T>
	void WriteProperty(const field_id_t field_id, const char *tag, const T &value) {
		serializer.WriteProperty(field_id, tag, value);
	}

	//! Serialize a generated WAL entry struct (its fields follow the WALType marker written in the constructor)
	template <class T>
	void WriteEntry(const T &entry) {
		entry.Serialize(serializer);
	}

	template <class FUNC>
	void WriteList(const field_id_t field_id, const char *tag, idx_t count, FUNC func) {
		serializer.WriteList(field_id, tag, count, func);
	}

private:
	ChecksumWriter checksum_writer;
	SerializationOptions options;
	BinarySerializer serializer;
};

//===--------------------------------------------------------------------===//
// Write Entries
//===--------------------------------------------------------------------===//
void WriteAheadLog::WriteHeader() {
	D_ASSERT(writer);
	if (writer->GetFileSize() > 0) {
		// Already written - no need to write a header.
		return;
	}

	// Write the header containing
	// - the version marker,
	// - the header_id of the matching database file, and
	// - the checkpoint iteration of the matching database file.
	// Note that we explicitly do not checksum the header, as it contains the version entry.

	BinarySerializer serializer(*writer);
	serializer.Begin();
	serializer.WriteProperty(100, "wal_type", WALType::WAL_VERSION);

	auto &database = GetDatabase();
	auto &catalog = database.GetCatalog().Cast<DuckCatalog>();
	auto encryption_version_number =
	    catalog.GetIsEncrypted() ? idx_t(WAL_ENCRYPTED_VERSION_NUMBER) : idx_t(WAL_VERSION_NUMBER);
	serializer.WriteProperty(101, "version", encryption_version_number);

	auto &single_file_block_manager = database.GetStorageManager().GetBlockManager().Cast<SingleFileBlockManager>();
	auto file_version_number = single_file_block_manager.GetVersionNumber();
	// double check
	if (StorageManager::TargetAtLeastVersion(StorageVersion::V1_3_0, file_version_number)) {
		auto db_identifier = single_file_block_manager.GetDBIdentifier();
		serializer.WriteList(102, "db_identifier", MainHeader::DB_IDENTIFIER_LEN,
		                     [&](Serializer::List &list, idx_t i) { list.WriteElement(db_identifier[i]); });
		idx_t current_checkpoint_iteration;
		if (checkpoint_iteration.IsValid()) {
			current_checkpoint_iteration = checkpoint_iteration.GetIndex();
		} else {
			current_checkpoint_iteration = single_file_block_manager.GetCheckpointIteration();
		}
		serializer.WriteProperty(103, "checkpoint_iteration", current_checkpoint_iteration);
	}

	serializer.End();
}

void WriteAheadLog::WriteCheckpoint(MetaBlockPointer meta_block) {
	WriteAheadLogSerializer serializer(*this, WALType::CHECKPOINT);
	serializer.WriteEntry(WALCheckpoint {meta_block});
	serializer.End();
}

//===--------------------------------------------------------------------===//
// CREATE TABLE
//===--------------------------------------------------------------------===//
void WriteAheadLog::WriteCreateTable(const TableCatalogEntry &entry) {
	WriteAheadLogSerializer serializer(*this, WALType::CREATE_TABLE);
	serializer.WriteEntry(WALCreateTable {entry.GetInfo()});
	serializer.End();
}

//===--------------------------------------------------------------------===//
// DROP TABLE
//===--------------------------------------------------------------------===//
void WriteAheadLog::WriteDropTable(const TableCatalogEntry &entry) {
	WriteAheadLogSerializer serializer(*this, WALType::DROP_TABLE);
	// the qualified name carries the (possibly nested) containing schema path + the table name; the legacy immediate
	// schema name is derived from it when serializing for storage versions older than v2.0.0
	serializer.WriteEntry(WALDropTable(QualifiedName(entry.schema.GetSchemaPath(), entry.name)));
	serializer.End();
}

//===--------------------------------------------------------------------===//
// CREATE SCHEMA
//===--------------------------------------------------------------------===//
void WriteAheadLog::WriteCreateSchema(const SchemaCatalogEntry &entry) {
	WriteAheadLogSerializer serializer(*this, WALType::CREATE_SCHEMA);
	// serialize the schema as a QualifiedName: parent schemas form the path, the schema name is the name. For storage
	// versions older than v2.0.0 (which only support top-level schemas) the legacy "schema" name field is written.
	serializer.WriteEntry(WALCreateSchema {entry.name, QualifiedName::FromPath(entry.GetSchemaPath())});
	serializer.End();
}

//===--------------------------------------------------------------------===//
// SEQUENCES
//===--------------------------------------------------------------------===//
void WriteAheadLog::WriteCreateSequence(const SequenceCatalogEntry &entry) {
	WriteAheadLogSerializer serializer(*this, WALType::CREATE_SEQUENCE);
	serializer.WriteEntry(WALCreateSequence {entry.GetInfo()});
	serializer.End();
}

void WriteAheadLog::WriteDropSequence(const SequenceCatalogEntry &entry) {
	WriteAheadLogSerializer serializer(*this, WALType::DROP_SEQUENCE);
	serializer.WriteEntry(WALDropSequence(QualifiedName(entry.schema.GetSchemaPath(), entry.name)));
	serializer.End();
}

void WriteAheadLog::WriteSequenceValue(SequenceValue val) {
	auto &sequence = *val.entry;
	WriteAheadLogSerializer serializer(*this, WALType::SEQUENCE_VALUE);
	// last_value (id 105) is only serialized from storage version v2.0.0 onwards, and is omitted when unset
	serializer.WriteEntry(WALSequenceValue(QualifiedName(sequence.schema.GetSchemaPath(), sequence.name),
	                                       val.usage_count, val.counter,
	                                       val.has_last_value ? optional<int64_t>(val.last_value) : nullopt));
	serializer.End();
}

//===--------------------------------------------------------------------===//
// MACROS
//===--------------------------------------------------------------------===//
void WriteAheadLog::WriteCreateMacro(const ScalarMacroCatalogEntry &entry) {
	WriteAheadLogSerializer serializer(*this, WALType::CREATE_MACRO);
	serializer.WriteEntry(WALCreateMacro {entry.GetInfo()});
	serializer.End();
}

void WriteAheadLog::WriteDropMacro(const ScalarMacroCatalogEntry &entry) {
	WriteAheadLogSerializer serializer(*this, WALType::DROP_MACRO);
	serializer.WriteEntry(WALDropMacro(QualifiedName(entry.schema.GetSchemaPath(), entry.name)));
	serializer.End();
}

void WriteAheadLog::WriteCreateTableMacro(const TableMacroCatalogEntry &entry) {
	WriteAheadLogSerializer serializer(*this, WALType::CREATE_TABLE_MACRO);
	serializer.WriteEntry(WALCreateTableMacro {entry.GetInfo()});
	serializer.End();
}

void WriteAheadLog::WriteDropTableMacro(const TableMacroCatalogEntry &entry) {
	WriteAheadLogSerializer serializer(*this, WALType::DROP_TABLE_MACRO);
	serializer.WriteEntry(WALDropTableMacro(QualifiedName(entry.schema.GetSchemaPath(), entry.name)));
	serializer.End();
}

//===--------------------------------------------------------------------===//
// Indexes
//===--------------------------------------------------------------------===//

void SerializeIndex(AttachedDatabase &db, WriteAheadLogSerializer &serializer, TableIndexList &list, idx_t index_oid) {
	auto storage_version = db.GetStorageManager().GetStorageVersion();
	auto info = list.SerializeToWAL(index_oid, storage_version);
	if (!info) {
		return;
	}

	serializer.WriteProperty(102, "index_storage_info", *info);
	serializer.WriteList(103, "index_storage", info->buffers.size(), [&](Serializer::List &list, idx_t i) {
		auto &buffers = info->buffers[i];
		for (auto buffer : buffers) {
			list.WriteElement(buffer.buffer_ptr, buffer.allocation_size);
		}
	});
}

void WriteAheadLog::WriteCreateIndex(const IndexCatalogEntry &entry) {
	WriteAheadLogSerializer serializer(*this, WALType::CREATE_INDEX);
	serializer.WriteProperty(101, "index_catalog_entry", &entry);

	// Serialize the index data to the persistent storage and write the metadata.
	auto &index_entry = entry.Cast<DuckIndexEntry>();
	auto &list = index_entry.GetDataTableInfo().GetIndexes();
	auto &database = GetDatabase();
	SerializeIndex(database, serializer, list, index_entry.oid);
	serializer.End();
}

void WriteAheadLog::WriteDropIndex(const IndexCatalogEntry &entry) {
	WriteAheadLogSerializer serializer(*this, WALType::DROP_INDEX);
	serializer.WriteEntry(WALDropIndex(QualifiedName(entry.schema.GetSchemaPath(), entry.name)));
	serializer.End();
}

//===--------------------------------------------------------------------===//
// Custom Types
//===--------------------------------------------------------------------===//
void WriteAheadLog::WriteCreateType(const TypeCatalogEntry &entry) {
	WriteAheadLogSerializer serializer(*this, WALType::CREATE_TYPE);
	serializer.WriteEntry(WALCreateType {entry.GetInfo()});
	serializer.End();
}

void WriteAheadLog::WriteDropType(const TypeCatalogEntry &entry) {
	WriteAheadLogSerializer serializer(*this, WALType::DROP_TYPE);
	serializer.WriteEntry(WALDropType(QualifiedName(entry.schema.GetSchemaPath(), entry.name)));
	serializer.End();
}

//===--------------------------------------------------------------------===//
// TRIGGERS
//===--------------------------------------------------------------------===//
void WriteAheadLog::WriteCreateTrigger(const TriggerCatalogEntry &entry) {
	WriteAheadLogSerializer serializer(*this, WALType::CREATE_TRIGGER);
	serializer.WriteEntry(WALCreateTrigger {entry.GetInfo()});
	serializer.End();
}

void WriteAheadLog::WriteDropTrigger(const TriggerCatalogEntry &entry) {
	WriteAheadLogSerializer serializer(*this, WALType::DROP_TRIGGER);
	serializer.WriteEntry(
	    WALDropTrigger(QualifiedName(entry.schema.GetSchemaPath(), entry.name), entry.base_table->Table()));
	serializer.End();
}

//===--------------------------------------------------------------------===//
// VIEWS
//===--------------------------------------------------------------------===//
void WriteAheadLog::WriteCreateView(const ViewCatalogEntry &entry) {
	WriteAheadLogSerializer serializer(*this, WALType::CREATE_VIEW);
	serializer.WriteEntry(WALCreateView {entry.GetInfo()});
	serializer.End();
}

void WriteAheadLog::WriteDropView(const ViewCatalogEntry &entry) {
	WriteAheadLogSerializer serializer(*this, WALType::DROP_VIEW);
	serializer.WriteEntry(WALDropView(QualifiedName(entry.schema.GetSchemaPath(), entry.name)));
	serializer.End();
}

//===--------------------------------------------------------------------===//
// DROP SCHEMA
//===--------------------------------------------------------------------===//
void WriteAheadLog::WriteDropSchema(const SchemaCatalogEntry &entry) {
	WriteAheadLogSerializer serializer(*this, WALType::DROP_SCHEMA);
	// serialize the schema as a QualifiedName: parent schemas form the path, the schema name is the name. For storage
	// versions older than v2.0.0 (which only support top-level schemas) the legacy "schema" name field is written.
	serializer.WriteEntry(WALDropSchema {entry.name, QualifiedName::FromPath(entry.GetSchemaPath())});
	serializer.End();
}

//===--------------------------------------------------------------------===//
// DATA
//===--------------------------------------------------------------------===//
void WriteAheadLog::WriteSetTable(const QualifiedName &table) {
	WriteAheadLogSerializer serializer(*this, WALType::USE_TABLE);
	serializer.WriteEntry(WALUseTable(table));
	serializer.End();
}

void WriteAheadLog::WriteInsert(DataChunk &chunk) {
	D_ASSERT(chunk.size() > 0);
	chunk.Verify(GetDatabase().GetDatabase());

	WriteAheadLogSerializer serializer(*this, WALType::INSERT_TUPLE);
	serializer.WriteProperty(101, "chunk", chunk);
	serializer.End();
}

void WriteAheadLog::WriteRowGroupData(const PersistentCollectionData &data) {
	D_ASSERT(!data.row_group_data.empty());

	WriteAheadLogSerializer serializer(*this, WALType::ROW_GROUP_DATA);
	serializer.WriteProperty(101, "row_group_data", data);
	serializer.End();

	// mark written blocks as checkpointed
	auto &block_manager = GetDatabase().GetStorageManager().GetBlockManager();
	for (auto &block_id : data.GetBlockIds()) {
		block_manager.MarkBlockAsCheckpointed(block_id);
	}
}

void WriteAheadLog::WriteDelete(DataChunk &chunk) {
	D_ASSERT(chunk.size() > 0);
	D_ASSERT(chunk.ColumnCount() == 1 && chunk.data[0].GetType() == LogicalType::ROW_TYPE);
	chunk.Verify(GetDatabase().GetDatabase());

	WriteAheadLogSerializer serializer(*this, WALType::DELETE_TUPLE);
	serializer.WriteProperty(101, "chunk", chunk);
	serializer.End();
}

void WriteAheadLog::WriteUpdate(DataChunk &chunk, const vector<column_t> &column_indexes) {
	D_ASSERT(chunk.size() > 0);
	D_ASSERT(chunk.ColumnCount() == 2);
	D_ASSERT(chunk.data[1].GetType().id() == LogicalType::ROW_TYPE);
	chunk.Verify(GetDatabase().GetDatabase());

	WriteAheadLogSerializer serializer(*this, WALType::UPDATE_TUPLE);
	serializer.WriteProperty(101, "column_indexes", column_indexes);
	serializer.WriteProperty(102, "chunk", chunk);
	serializer.End();
}

//===--------------------------------------------------------------------===//
// Write ALTER Statement
//===--------------------------------------------------------------------===//
void WriteAheadLog::WriteAlter(CatalogEntry &entry, const AlterInfo &info) {
	WriteAheadLogSerializer serializer(*this, WALType::ALTER_INFO);
	serializer.WriteProperty(101, "info", &info);

	if (!info.IsAddUniqueConstraint()) {
		return serializer.End();
	}

	auto &table = entry.Cast<DuckTableEntry>();
	auto &parent = entry.Parent().Cast<DuckTableEntry>();
	auto added_oids = parent.GetAddedUniqueIndexOids(table);
	if (added_oids.size() != 1) {
		throw InternalException("WriteAlter: expected one added UNIQUE constraint on table %s, found %llu", parent.name,
		                        added_oids.size());
	}
	auto &list = parent.GetStorage().GetDataTableInfo()->GetIndexes();
	SerializeIndex(GetDatabase(), serializer, list, added_oids[0]);
	serializer.End();
}

//===--------------------------------------------------------------------===//
// FLUSH
//===--------------------------------------------------------------------===//
void WriteAheadLog::Flush() {
	if (!writer) {
		return;
	}
	SyncUpTo(FlushMarker());
}

idx_t WriteAheadLog::FlushMarker() {
	if (!writer) {
		// nothing was ever written to this WAL: there is nothing to make durable
		return 0;
	}

	// write an empty entry
	WriteAheadLogSerializer serializer(*this, WALType::WAL_FLUSH);
	serializer.End();

	// push to the OS without syncing: SyncUpTo does that, potentially batched with other commits
	writer->Flush();
	storage_manager.SetWALSize(writer->GetFileSize());
	auto marker_offset = writer->GetTotalWritten();
	{
		lock_guard<mutex> guard(sync_lock);
		if (marker_offset > requested_sync_offset) {
			requested_sync_offset = marker_offset;
		}
	}
	return marker_offset;
}

void WriteAheadLog::SyncUpTo(idx_t offset) {
	D_ASSERT(writer && offset > 0);
	auto &db_instance = GetDatabase().GetDatabase();
	auto fsync_sleep_ms = Settings::Get<DebugWalFsyncSleepMsSetting>(db_instance);
	auto force_fsync_failure = Settings::Get<DebugForceWalFsyncFailureSetting>(db_instance);
	unique_lock<mutex> guard(sync_lock);
	// durable_offset only advances on successful syncs, so an offset it covers stays durable
	while (durable_offset < offset) {
		if (sync_failed) {
			throw IOException("Cannot sync WAL \"%s\": a previous sync of this WAL has failed", wal_path);
		}
		if (sync_in_flight) {
			// one sync at a time: the next syncer covers every marker flushed meanwhile
			sync_cv.wait(guard);
			continue;
		}
		// sync everything flushed so far, on behalf of every waiter
		auto target = requested_sync_offset;
		sync_in_flight = true;
		guard.unlock();
		ErrorData error;
		try {
			if (fsync_sleep_ms > 0) {
				ThreadUtil::SleepMs(fsync_sleep_ms);
			}
			if (force_fsync_failure) {
				throw IOException("debug_force_wal_fsync_failure: injected WAL fsync failure");
			}
			writer->SyncHandle();
		} catch (std::exception &ex) {
			error = ErrorData(ex);
		}
		guard.lock();
		sync_in_flight = false;
		if (error.HasError()) {
			// the OS may have dropped the dirty pages: this WAL must never be synced again
			sync_failed = true;
		} else {
			durable_offset = target;
		}
		sync_cv.notify_all();
		if (error.HasError()) {
			error.Throw();
		}
	}
}

void WriteAheadLog::IncrementWALEntriesCount() {
	storage_manager.IncrementWALEntriesCount();
}

} // namespace duckdb
