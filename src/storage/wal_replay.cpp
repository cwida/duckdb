#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/index_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/common/assert.hpp"
#include "duckdb/common/checksum.hpp"
#include "duckdb/common/encryption_functions.hpp"
#include "duckdb/common/encryption_key_manager.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/buffered_file_reader.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/common/vector_size.hpp"
#include "duckdb/common/enums/checkpoint_abort.hpp"
#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/execution/index/index_type_set.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_trigger_info.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression_binder/index_binder.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/storage/single_file_block_manager.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/storage/table/column_data.hpp"
#include "duckdb/storage/table/data_table_info.hpp"
#include "duckdb/storage/table/delete_state.hpp"
#include "duckdb/storage/wal_entry.hpp"
#include "duckdb/storage/write_ahead_log.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/row_group_collection.hpp"
#include "duckdb/main/profiler/metrics.hpp"
#include "duckdb/main/query_profiler.hpp"

namespace duckdb {
enum class WALReplayState { MAIN_WAL, CHECKPOINT_WAL };

class ReplayState {
public:
	ReplayState(AttachedDatabase &db, ClientContext &context, WALReplayState replay_state_p)
	    : db(db), context(context), catalog(db.GetCatalog()), replay_state(replay_state_p) {
	}

	AttachedDatabase &db;
	ClientContext &context;
	Catalog &catalog;
	optional_ptr<DuckTableEntry> current_table;
	MetaBlockPointer checkpoint_id;
	idx_t wal_version = 1;
	optional_idx current_position;
	optional_idx checkpoint_position;
	optional_idx checkpoint_end_position;
	optional_idx expected_checkpoint_id;
	WALReplayState replay_state;
	//! Blocks referenced by ROW_GROUP_DATA entries, collected during the deserialize-only scan. They are marked as used
	//! only once we have decided to replay the WAL, so if log replay is not needed, these blocks won't be
	//! double-referenced.
	vector<block_id_t> row_group_blocks;

	struct ReplayIndexInfo {
		ReplayIndexInfo(TableIndexList &index_list, unique_ptr<Index> index, idx_t table_oid, idx_t index_oid,
		                ConstraintCheckMode check_mode)
		    : index_list(index_list), index(std::move(index)), table_oid(table_oid), index_oid(index_oid),
		      check_mode(check_mode) {
		}

		reference<TableIndexList> index_list;
		unique_ptr<Index> index;
		//! The oid of the table, used to uniquely identify the table (even after a rename).
		idx_t table_oid;
		//! The oid of the index, used to match a DROP INDEX in the same replayed transaction.
		idx_t index_oid;
		//! The check mode of the constraint enforced by the index.
		ConstraintCheckMode check_mode;
	};
	vector<ReplayIndexInfo> replay_index_infos;
};

class WriteAheadLogDeserializer {
public:
	WriteAheadLogDeserializer(ReplayState &state_p, BufferedFileReader &stream_p, bool deserialize_only = false)
	    : state(state_p), db(state.db), context(state.context), catalog(state.catalog), stream(nullptr, 0),
	      deserializer(stream_p), deserialize_only(deserialize_only) {
		deserializer.Set<Catalog &>(catalog);
	}
	WriteAheadLogDeserializer(ReplayState &state_p, AllocatedData data_p, idx_t size, bool deserialize_only = false)
	    : state(state_p), db(state.db), context(state.context), catalog(state.catalog), data(std::move(data_p)),
	      stream(data.get(), size), deserializer(stream), deserialize_only(deserialize_only) {
		deserializer.Set<Catalog &>(catalog);
	}

	//! WAL entries are read into memory in full, so the buffer is tracked by the buffer manager
	static AllocatedData AllocateEntryBuffer(ReplayState &state_p, idx_t size) {
		if (size == 0) {
			throw SerializationException("Corrupt WAL file: found an empty entry");
		}
		return BufferAllocator::Get(state_p.db).Allocate(size);
	}

	static WriteAheadLogDeserializer GetEntryDeserializer(ReplayState &state_p, BufferedFileReader &stream,
	                                                      bool deserialize_only = false) {
		if (state_p.wal_version == 1) {
			// old WAL versions do not have checksums
			return WriteAheadLogDeserializer(state_p, stream, deserialize_only);
		}

		if (state_p.wal_version == 2) {
			// read the size and checksum
			auto size = stream.Read<uint64_t>();
			auto stored_checksum = stream.Read<uint64_t>();
			auto offset = stream.CurrentOffset();
			auto file_size = stream.FileSize();

			if (offset + size > file_size) {
				throw SerializationException(
				    "Corrupt WAL file: entry size exceeded remaining data in file at byte position %llu "
				    "(found entry with size %llu bytes, file size %llu bytes)",
				    offset, size, file_size);
			}

			// allocate a buffer and read data into the buffer
			auto buffer = AllocateEntryBuffer(state_p, size);
			stream.ReadData(buffer.get(), size);

			// compute and verify the checksum
			auto computed_checksum = Checksum(buffer.get(), size);
			if (stored_checksum != computed_checksum) {
				throw DataCorruptionException(
				    "Corrupt WAL file: entry at byte position %llu computed checksum %llu does not match "
				    "stored checksum %llu",
				    offset, computed_checksum, stored_checksum);
			}

			return WriteAheadLogDeserializer(state_p, std::move(buffer), size, deserialize_only);
		}

		if (state_p.wal_version == 3) {
			auto &database = state_p.db.GetDatabase();
			//! Version 3 means that the WAL is encrypted
			//! For encryption, the length field remains plaintext
			//! After the length field, we store a 12-byte nonce (for GCM)
			//! After the nonce, we store the checksum, followed by the actual entry
			//! After the stored entry, we store a 16-byte nonce (for GCM).

			// read the size (this is excluding the nonce, checksum and tag)
			auto size = stream.Read<uint64_t>();
			// the ciphertext size is including the checksum
			const auto ciphertext_size = size + sizeof(uint64_t);

			auto offset = stream.CurrentOffset();
			auto file_size = stream.FileSize();

			EncryptionNonce nonce(state_p.db.GetStorageManager().GetCipher(),
			                      state_p.db.GetStorageManager().GetEncryptionVersion());
			EncryptionTag tag;

			stream.ReadData(nonce.data(), nonce.size());

			auto &keys = EncryptionKeyManager::Get(state_p.db.GetDatabase());
			auto &catalog = state_p.db.GetCatalog().Cast<DuckCatalog>();
			auto derived_key = keys.GetKey(catalog.GetEncryptionKeyId());
			auto metadata = make_uniq<EncryptionStateMetadata>(state_p.db.GetStorageManager().GetCipher(),
			                                                   MainHeader::DEFAULT_ENCRYPTION_KEY_LENGTH,
			                                                   state_p.db.GetStorageManager().GetEncryptionVersion());
			//! initialize the decryption
			auto encryption_state =
			    database.GetEncryptionUtil(state_p.db.IsReadOnly())->CreateEncryptionState(std::move(metadata));
			encryption_state->InitializeDecryption(nonce, derived_key);

			if (encryption_state->GetCipher() == EncryptionTypes::CipherType::CTR) {
				tag.SetSize(0);
			}

			if (offset + nonce.size() + ciphertext_size + tag.size() > file_size) {
				throw SerializationException(
				    "Corrupt Encrypted WAL file: entry size exceeded remaining data in file at byte position %llu "
				    "(found entry with size %llu bytes, file size %llu bytes)",
				    offset, size, file_size);
			}

			//! Allocate a decryption buffer
			auto buffer = AllocateEntryBuffer(state_p, ciphertext_size);
			auto out_buffer = AllocateEntryBuffer(state_p, size);

			stream.ReadData(buffer.get(), ciphertext_size);
			encryption_state->Process(buffer.get(), ciphertext_size, buffer.get(), ciphertext_size);

			if (encryption_state->GetCipher() == EncryptionTypes::CipherType::GCM) {
				//! read and verify the stored tag
				stream.ReadData(tag.data(), tag.size());
				D_ASSERT(!tag.IsAllZeros());
				encryption_state->Finalize(buffer.get(), ciphertext_size, tag.data(), tag.size());
			} else {
				encryption_state->Finalize(buffer.get(), ciphertext_size, nullptr, 0);
			}

			//! read the stored checksum
			auto stored_checksum = Load<uint64_t>(buffer.get());

			//! copy the decrypted data to the output buffer
			memcpy(out_buffer.get(), buffer.get() + sizeof(stored_checksum), size);

			// compute and verify the checksum
			auto computed_checksum = Checksum(out_buffer.get(), size);
			if (stored_checksum != computed_checksum) {
				throw DataCorruptionException(
				    "Corrupt WAL file: entry at byte position %llu computed checksum %llu does not match "
				    "stored checksum %llu",
				    offset, computed_checksum, stored_checksum);
			}

			return WriteAheadLogDeserializer(state_p, std::move(out_buffer), size, deserialize_only);
		}

		throw IOException("Failed to read WAL of version %llu - can only read version 1, 2 and 3 (encrypted)",
		                  state_p.wal_version);
	}

	bool ReplayEntry() {
		deserializer.Begin();
		auto wal_type = deserializer.ReadProperty<WALType>(100, "wal_type");
		if (wal_type == WALType::WAL_FLUSH) {
			deserializer.End();
			return true;
		}
		if (CanSkipPayload(wal_type)) {
			// Framed entries have already been read and integrity-checked, so jump directly to the root terminator.
			D_ASSERT(stream.GetCapacity() >= sizeof(field_id_t));
			stream.SetPosition(stream.GetCapacity() - sizeof(field_id_t));
		} else {
			ReplayEntry(wal_type);
		}
		deserializer.End();
		return false;
	}

	bool DeserializeOnly() const {
		return deserialize_only;
	}

	static void ThrowVersionError(idx_t checkpoint_iteration, idx_t expected_checkpoint_iteration);

protected:
	void ReplayEntry(WALType wal_type);
	bool CanSkipPayload(WALType wal_type) const {
		// Unframed WAL v1 entries must still be deserialized; Replay* guards suppress their side effects.
		if (!deserialize_only || !data.get()) {
			return false;
		}
		D_ASSERT(state.wal_version == 2 || state.wal_version == 3);
		switch (wal_type) {
		case WALType::CREATE_TABLE:
		case WALType::DROP_TABLE:
		case WALType::CREATE_SCHEMA:
		case WALType::DROP_SCHEMA:
		case WALType::CREATE_VIEW:
		case WALType::DROP_VIEW:
		case WALType::CREATE_SEQUENCE:
		case WALType::DROP_SEQUENCE:
		case WALType::SEQUENCE_VALUE:
		case WALType::CREATE_MACRO:
		case WALType::DROP_MACRO:
		case WALType::CREATE_TYPE:
		case WALType::DROP_TYPE:
		case WALType::ALTER_INFO:
		case WALType::CREATE_TABLE_MACRO:
		case WALType::DROP_TABLE_MACRO:
		case WALType::CREATE_INDEX:
		case WALType::DROP_INDEX:
		case WALType::USE_TABLE:
		case WALType::INSERT_TUPLE:
		case WALType::DELETE_TUPLE:
		case WALType::UPDATE_TUPLE:
		case WALType::CREATE_TRIGGER:
		case WALType::DROP_TRIGGER:
			return true;
		default:
			return false;
		}
	}

	void ReplayVersion();

	void ReplayCreateTable();
	void ReplayDropTable();
	void ReplayAlter();

	void ReplayCreateView();
	void ReplayDropView();

	void ReplayCreateSchema();
	void ReplayDropSchema();

	void ReplayCreateType();
	void ReplayDropType();

	void ReplayCreateSequence();
	void ReplayDropSequence();
	void ReplaySequenceValue();

	void ReplayCreateMacro();
	void ReplayDropMacro();

	void ReplayCreateTableMacro();
	void ReplayDropTableMacro();

	void ReplayCreateIndex();
	void ReplayDropIndex();

	void ReplayCreateTrigger();
	void ReplayDropTrigger();

	void ReplayUseTable();
	void ReplayInsert();
	void ReplayRowGroupData();
	void ReplayDelete();
	void ReplayUpdate();
	void ReplayCheckpoint();

private:
	void ReplayIndexData(IndexStorageInfo &info);

private:
	ReplayState &state;
	AttachedDatabase &db;
	ClientContext &context;
	Catalog &catalog;
	AllocatedData data;
	MemoryStream stream;
	BinaryDeserializer deserializer;
	bool deserialize_only;
	optional_idx expected_checkpoint_id;
};

//===--------------------------------------------------------------------===//
// Replay
//===--------------------------------------------------------------------===//
struct WriteAheadLogReplayer {
public:
	WriteAheadLogReplayer(QueryContext context, StorageManager &storage_manager, const string &wal_path);

	unique_ptr<WriteAheadLog> Replay();

private:
	unique_ptr<WriteAheadLog> ReplayLog(unique_ptr<FileHandle> handle,
	                                    WALReplayState replay_state = WALReplayState::MAIN_WAL);
	void CopyOverWAL(BufferedFileReader &reader, FileHandle &target, data_ptr_t buffer, idx_t buffer_size,
	                 idx_t copy_end);
	void MergeIntoRecoveryWAL(Connection &con, const ReplayState &checkpoint_state, BufferedFileReader &main_wal_reader,
	                          const string &recovery_path, unique_ptr<FileHandle> checkpoint_handle);

private:
	QueryContext context;
	StorageManager &storage_manager;
	AttachedDatabase &database;
	const string &main_wal_path;
	FileSystem &fs;
};

unique_ptr<WriteAheadLog> WriteAheadLog::Replay(QueryContext context, StorageManager &storage_manager,
                                                const string &main_wal_path) {
	WriteAheadLogReplayer wal_replay(context, storage_manager, main_wal_path);
	return wal_replay.Replay();
}

WriteAheadLogReplayer::WriteAheadLogReplayer(QueryContext context, StorageManager &storage_manager,
                                             const string &main_wal_path)
    : context(context), storage_manager(storage_manager), database(storage_manager.GetAttached()),
      main_wal_path(main_wal_path), fs(FileSystem::Get(storage_manager.GetAttached())) {
}

unique_ptr<WriteAheadLog> WriteAheadLogReplayer::Replay() {
	auto handle = fs.OpenFile(main_wal_path, FileFlags::FILE_FLAGS_READ | FileFlags::FILE_FLAGS_NULL_IF_NOT_EXISTS);
	if (!handle) {
		// WAL does not exist - instantiate an empty WAL
		return make_uniq<WriteAheadLog>(storage_manager, main_wal_path);
	}

	// context is passed for metric collection purposes only!!
	auto wal_handle = ReplayLog(std::move(handle));
	if (wal_handle) {
		return wal_handle;
	}
	// replay returning NULL indicates we can nuke the WAL entirely - but only if this is not a read-only connection
	if (!storage_manager.GetAttached().IsReadOnly()) {
		fs.TryRemoveFile(main_wal_path);
	}
	return make_uniq<WriteAheadLog>(storage_manager, main_wal_path);
}

void WriteAheadLogReplayer::CopyOverWAL(BufferedFileReader &reader, FileHandle &target, data_ptr_t buffer,
                                        idx_t buffer_size, idx_t copy_end) {
	while (!reader.Finished()) {
		idx_t read_count = MinValue<idx_t>(buffer_size, copy_end - reader.CurrentOffset());
		if (read_count == 0) {
			break;
		}
		reader.ReadData(context, buffer, read_count);

		target.Write(buffer, read_count);
	}
}

void WriteAheadLogReplayer::MergeIntoRecoveryWAL(Connection &con, const ReplayState &checkpoint_state,
                                                 BufferedFileReader &main_wal_reader, const string &recovery_path,
                                                 unique_ptr<FileHandle> checkpoint_handle) {
	auto recovery_handle =
	    fs.OpenFile(recovery_path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW);

	static constexpr idx_t BATCH_SIZE = Storage::DEFAULT_BLOCK_SIZE;
	auto buffer = make_uniq_array<data_t>(BATCH_SIZE);

	// first copy over the main WAL contents
	auto copy_end = checkpoint_state.checkpoint_position.GetIndex();
	main_wal_reader.Reset();
	CopyOverWAL(main_wal_reader, *recovery_handle, buffer.get(), BATCH_SIZE, copy_end);

	auto checkpoint_wal_path = checkpoint_handle->GetPath();
	// now copy over the checkpoint WAL
	{
		BufferedFileReader checkpoint_reader(fs, std::move(checkpoint_handle));

		if (checkpoint_reader.FileSize() != 0) {
			// skip over the version entry
			ReplayState checkpoint_replay_state(database, *con.context, WALReplayState::CHECKPOINT_WAL);

			auto deserializer =
			    WriteAheadLogDeserializer::GetEntryDeserializer(checkpoint_replay_state, checkpoint_reader, true);
			deserializer.ReplayEntry();

			if (checkpoint_replay_state.wal_version != checkpoint_state.wal_version) {
				throw InvalidInputException("Failure while replaying checkpoint WAL file \"%s\": checkpoint "
				                            "WAL version is different from main WAL version",
				                            checkpoint_wal_path);
			}

			CopyOverWAL(checkpoint_reader, *recovery_handle, buffer.get(), BATCH_SIZE, checkpoint_reader.FileSize());
		}
	}

	auto debug_checkpoint_abort = Settings::Get<DebugCheckpointAbortSetting>(storage_manager.GetDatabase());

	// move over the recovery WAL over the main WAL
	recovery_handle->Sync();
	recovery_handle.reset();
	// the main WAL is the target of the move - Windows refuses to replace a file that is still open
	main_wal_reader.handle.reset();

	if (debug_checkpoint_abort == CheckpointAbort::DEBUG_ABORT_BEFORE_MOVING_RECOVERY) {
		throw FatalException("Checkpoint aborted before moving recovery file because of PRAGMA checkpoint_abort flag");
	}

	fs.MoveFile(recovery_path, main_wal_path);

	if (debug_checkpoint_abort == CheckpointAbort::DEBUG_ABORT_BEFORE_DELETING_CHECKPOINT_WAL) {
		throw FatalException(
		    "Checkpoint aborted before deleting checkpoint file because of PRAGMA checkpoint_abort flag");
	}

	// delete the checkpoint WAL
	fs.RemoveFile(checkpoint_wal_path);
}

unique_ptr<WriteAheadLog> WriteAheadLogReplayer::ReplayLog(unique_ptr<FileHandle> handle, WALReplayState replay_state) {
	auto &database = storage_manager.GetAttached();
	Connection con(database.GetDatabase());
	auto wal_path = handle->GetPath();
	BufferedFileReader reader(FileSystem::Get(database), std::move(handle));
	if (reader.Finished()) {
		// WAL file exists, but it is empty - we can delete the file
		return nullptr;
	}

	con.BeginTransaction();
	MetaTransaction::Get(*con.context).ModifyDatabase(database, DatabaseModificationType());

	auto &config = DBConfig::GetConfig(database.GetDatabase());
	// first deserialize the WAL to look for a checkpoint flag
	// if there is a checkpoint flag, we might have already flushed the contents of the WAL to disk
	ReplayState checkpoint_state(database, *con.context, replay_state);
	idx_t last_wal_flush_end = 0;
	idx_t checkpoint_truncate_offset = 0;
	idx_t last_wal_flush_row_group_blocks = 0;
	try {
		idx_t replay_entry_count = 0;
		while (true) {
			replay_entry_count++;
			// read the current entry (deserialize only)
			checkpoint_state.current_position = reader.CurrentOffset();
			auto deserializer = WriteAheadLogDeserializer::GetEntryDeserializer(checkpoint_state, reader, true);
			auto is_wal_flush = deserializer.ReplayEntry();
			if (checkpoint_state.checkpoint_position.IsValid() && !checkpoint_state.checkpoint_end_position.IsValid()) {
				checkpoint_state.checkpoint_end_position = reader.CurrentOffset();
				checkpoint_truncate_offset = last_wal_flush_end;
			}
			if (is_wal_flush) {
				last_wal_flush_end = reader.CurrentOffset();
				last_wal_flush_row_group_blocks = checkpoint_state.row_group_blocks.size();
				// check if the file is exhausted
				if (reader.Finished()) {
					// we finished reading the file: break
					break;
				}
			}
		}
		auto client_context = context.GetClientContext();
		if (client_context) {
			auto &profiler = *client_context->client_data->profiler;
			profiler.AddToMetricCounter(MetricStorageWALReplayEntryCount::Name, replay_entry_count);
		}
	} catch (std::exception &ex) { // LCOV_EXCL_START
		ErrorData error(ex);
		// ignore serialization exceptions - they signal a torn WAL
		if (config.options.abort_on_wal_failure || error.Type() != ExceptionType::SERIALIZATION) {
			con.Rollback();
			error.Throw("Failure while replaying WAL file \"" + wal_path + "\": ");
		}
	} // LCOV_EXCL_STOP

	// Discard row group blocks from uncommitted transactions.
	// Notice, this must happen before apply any WAL entries to block manager.
	checkpoint_state.row_group_blocks.resize(last_wal_flush_row_group_blocks);

	unique_ptr<FileHandle> checkpoint_handle;
	bool truncate_failed_checkpoint_marker = false;
	// A serialization error can leave a partially deserialized checkpoint marker in the replay state. Only reconcile
	// checkpoints after the complete marker entry has been consumed.
	if (checkpoint_state.checkpoint_id.IsValid() && checkpoint_state.checkpoint_end_position.IsValid()) {
		if (replay_state == WALReplayState::CHECKPOINT_WAL) {
			throw InvalidInputException(
			    "Failure while replaying checkpoint WAL file \"%s\": checkpoint WAL cannot contain a checkpoint marker",
			    wal_path);
		}
		auto checkpoint_end = checkpoint_state.checkpoint_end_position.GetIndex();
		// The scan stops at either the final WAL_FLUSH or a torn tail. In both cases, current_position is the start of
		// the final entry attempt. It must directly follow the checkpoint marker.
		bool checkpoint_marker_at_end = checkpoint_state.current_position.GetIndex() == checkpoint_end;
		if (!checkpoint_marker_at_end) {
			throw DataCorruptionException("WAL checkpoint marker must be at the end of the WAL");
		}
		// there is a checkpoint flag
		// this means a checkpoint was on-going when we crashed
		// we need to reconcile this with what is in the data file
		// first check if there is a checkpoint WAL
		auto &manager = database.GetStorageManager();
		auto checkpoint_wal = manager.GetCheckpointWALPath();
		checkpoint_handle =
		    fs.OpenFile(checkpoint_wal, FileFlags::FILE_FLAGS_READ | FileFlags::FILE_FLAGS_NULL_IF_NOT_EXISTS);
		bool checkpoint_was_successful = manager.IsCheckpointClean(checkpoint_state.checkpoint_id);
		if (!checkpoint_handle) {
			// no checkpoint WAL - either we just need to replay this WAL, or we are done
			if (checkpoint_was_successful) {
				// the contents of the WAL have already been checkpointed and there is no checkpoint WAL - we are done
				return nullptr;
			}
			if (!storage_manager.GetAttached().IsReadOnly()) {
				truncate_failed_checkpoint_marker = true;
			}
		} else {
			// we have a checkpoint WAL
			if (checkpoint_was_successful) {
				// the checkpoint was successful
				// the main WAL is no longer needed, we only need to replay the checkpoint WAL
				// if this is a read-only connection then replay the checkpoint WAL directly
				if (storage_manager.GetAttached().IsReadOnly()) {
					return ReplayLog(std::move(checkpoint_handle), WALReplayState::CHECKPOINT_WAL);
				}
				// if this is not a read-only connection we need to finish the checkpoint
				// overwrite the current WAL with the checkpoint WAL
				// both files must be closed - Windows refuses to move a file that is still open, and refuses to
				// replace a target that is still open
				checkpoint_handle.reset();
				reader.handle.reset();

				fs.MoveFile(checkpoint_wal, wal_path);

				// now open the handle again and replay the checkpoint WAL
				checkpoint_handle =
				    fs.OpenFile(wal_path, FileFlags::FILE_FLAGS_READ | FileFlags::FILE_FLAGS_NULL_IF_NOT_EXISTS);
				return ReplayLog(std::move(checkpoint_handle), WALReplayState::CHECKPOINT_WAL);
			}
			// the checkpoint was unsuccessful
			// this means we need to replay both this WAL and the checkpoint WAL
			// if this is a read-only connection - replay both WAL files
			if (!storage_manager.GetAttached().IsReadOnly()) {
				// if this is not a read-only connection, then merge the two WALs and replay the merged WAL
				// we merge into the recovery WAL path
				auto recovery_path = manager.GetRecoveryWALPath();
				MergeIntoRecoveryWAL(con, checkpoint_state, reader, recovery_path, std::move(checkpoint_handle));

				// replay the (combined) recovery WAL
				auto main_handle = fs.OpenFile(wal_path, FileFlags::FILE_FLAGS_READ);
				return ReplayLog(std::move(main_handle), WALReplayState::CHECKPOINT_WAL);
			}
		}
	}
	if (checkpoint_state.expected_checkpoint_id.IsValid()) {
		// we expected a checkpoint id - but no checkpoint has happened - abort!
		auto expected_id = checkpoint_state.expected_checkpoint_id.GetIndex();
		WriteAheadLogDeserializer::ThrowVersionError(expected_id - 1, expected_id);
	}
	unique_ptr<BufferedFileReader> truncated_wal_reader;
	if (truncate_failed_checkpoint_marker) {
		// The failed checkpoint marker is the logical end of the WAL and must not become writable again. Truncate to
		// the preceding commit boundary, which is zero if the WAL only contained its header before the marker.
		reader.handle.reset();
		auto truncate_handle = fs.OpenFile(wal_path, FileFlags::FILE_FLAGS_WRITE);
		fs.Truncate(*truncate_handle, NumericCast<int64_t>(checkpoint_truncate_offset));
		truncate_handle->Sync();
		truncate_handle.reset();

		if (checkpoint_truncate_offset == 0) {
			return make_uniq<WriteAheadLog>(storage_manager, wal_path);
		}
		auto main_handle = fs.OpenFile(wal_path, FileFlags::FILE_FLAGS_READ);
		truncated_wal_reader = make_uniq<BufferedFileReader>(fs, std::move(main_handle));
	}

	// Now we have decided to replay this WAL, mark the blocks referenced by ROW_GROUP_DATA entries as used.
	// Notice, this must happen before replay, because replaying earlier entries can allocate blocks; without the marks,
	// those allocations could hand out blocks that later entries reference.
	auto &block_manager = storage_manager.GetBlockManager();
	for (auto &block_id : checkpoint_state.row_group_blocks) {
		block_manager.MarkBlockAsUsed(block_id);
	}

	// If there are no committed transactions in the WAL, rollback and truncate.
	if (last_wal_flush_end == 0) {
		con.Rollback();
		return make_uniq<WriteAheadLog>(storage_manager, wal_path, idx_t(0),
		                                WALInitState::UNINITIALIZED_REQUIRES_TRUNCATE);
	}

	// we need to recover from the WAL: actually set up the replay state
	ReplayState state(database, *con.context, replay_state);

	// reset the reader - we are going to read the WAL from the beginning again
	auto &wal_reader = truncated_wal_reader ? *truncated_wal_reader : reader;
	wal_reader.Reset();

	// replay the WAL
	// note that everything is wrapped inside a try/catch block here
	// there can be errors in WAL replay because of a corrupt WAL file
	idx_t successful_offset = 0;
	bool all_succeeded = false;
	try {
		while (wal_reader.CurrentOffset() < last_wal_flush_end) {
			// read the current entry
			auto deserializer = WriteAheadLogDeserializer::GetEntryDeserializer(state, wal_reader);
			if (deserializer.ReplayEntry()) {
				con.Commit();

				// Commit any outstanding indexes.
				for (auto &info : state.replay_index_infos) {
					info.index_list.get().AddIndex(std::move(info.index), info.index_oid, info.check_mode);
				}
				state.replay_index_infos.clear();

				successful_offset = wal_reader.CurrentOffset();
				// check if the file is exhausted or all committed entries were replayed
				if (wal_reader.Finished() || wal_reader.CurrentOffset() >= last_wal_flush_end) {
					// we finished reading the file: break
					all_succeeded = wal_reader.Finished();
					break;
				}
				con.BeginTransaction();
				MetaTransaction::Get(*con.context).ModifyDatabase(database, DatabaseModificationType());
			}
		}
	} catch (std::exception &ex) { // LCOV_EXCL_START
		// exception thrown in WAL replay: rollback
		con.Query("ROLLBACK");
		ErrorData error(ex);
		// serialization failure means a truncated WAL
		// these failures are ignored unless abort_on_wal_failure is true
		// other failures always result in an error
		if (config.options.abort_on_wal_failure || error.Type() != ExceptionType::SERIALIZATION) {
			error.Throw("Failure while replaying WAL file \"" + wal_path + "\": ");
		}
	} catch (...) {
		// exception thrown in WAL replay: rollback
		con.Query("ROLLBACK");
		throw;
	} // LCOV_EXCL_STOP
	if (all_succeeded && checkpoint_handle) {
		// we have successfully replayed the main WAL - but there is still a checkpoint WAL remaining
		// this can only happen in read-only mode
		// replay the checkpoint WAL and return
		return ReplayLog(std::move(checkpoint_handle), WALReplayState::CHECKPOINT_WAL);
	}
	auto init_state = all_succeeded ? WALInitState::UNINITIALIZED : WALInitState::UNINITIALIZED_REQUIRES_TRUNCATE;
	return make_uniq<WriteAheadLog>(storage_manager, wal_path, successful_offset, init_state);
}

//===--------------------------------------------------------------------===//
// Replay Entries
//===--------------------------------------------------------------------===//
void WriteAheadLogDeserializer::ReplayEntry(WALType entry_type) {
	switch (entry_type) {
	case WALType::WAL_VERSION:
		ReplayVersion();
		break;
	case WALType::CREATE_TABLE:
		ReplayCreateTable();
		break;
	case WALType::DROP_TABLE:
		ReplayDropTable();
		break;
	case WALType::ALTER_INFO:
		ReplayAlter();
		break;
	case WALType::CREATE_VIEW:
		ReplayCreateView();
		break;
	case WALType::DROP_VIEW:
		ReplayDropView();
		break;
	case WALType::CREATE_SCHEMA:
		ReplayCreateSchema();
		break;
	case WALType::DROP_SCHEMA:
		ReplayDropSchema();
		break;
	case WALType::CREATE_SEQUENCE:
		ReplayCreateSequence();
		break;
	case WALType::DROP_SEQUENCE:
		ReplayDropSequence();
		break;
	case WALType::SEQUENCE_VALUE:
		ReplaySequenceValue();
		break;
	case WALType::CREATE_MACRO:
		ReplayCreateMacro();
		break;
	case WALType::DROP_MACRO:
		ReplayDropMacro();
		break;
	case WALType::CREATE_TABLE_MACRO:
		ReplayCreateTableMacro();
		break;
	case WALType::DROP_TABLE_MACRO:
		ReplayDropTableMacro();
		break;
	case WALType::CREATE_INDEX:
		ReplayCreateIndex();
		break;
	case WALType::DROP_INDEX:
		ReplayDropIndex();
		break;
	case WALType::USE_TABLE:
		ReplayUseTable();
		break;
	case WALType::INSERT_TUPLE:
		ReplayInsert();
		break;
	case WALType::ROW_GROUP_DATA:
		ReplayRowGroupData();
		break;
	case WALType::DELETE_TUPLE:
		ReplayDelete();
		break;
	case WALType::UPDATE_TUPLE:
		ReplayUpdate();
		break;
	case WALType::CHECKPOINT:
		ReplayCheckpoint();
		break;
	case WALType::CREATE_TYPE:
		ReplayCreateType();
		break;
	case WALType::DROP_TYPE:
		ReplayDropType();
		break;
	case WALType::CREATE_TRIGGER:
		ReplayCreateTrigger();
		break;
	case WALType::DROP_TRIGGER:
		ReplayDropTrigger();
		break;
	default:
		throw InternalException("Invalid WAL entry type!");
	}
}

//===--------------------------------------------------------------------===//
// Replay Version
//===--------------------------------------------------------------------===//
void WriteAheadLogDeserializer::ThrowVersionError(idx_t checkpoint_iteration, idx_t expected_checkpoint_iteration) {
	string relation = checkpoint_iteration < expected_checkpoint_iteration ? "an older" : "a newer";
	throw IOException("This WAL was created for this database file, but the WAL checkpoint iteration does not "
	                  "match the database file. "
	                  "That means the WAL was created for %s version of this database. File checkpoint "
	                  "iteration: %d, WAL checkpoint iteration: %d",
	                  relation, expected_checkpoint_iteration, checkpoint_iteration);
}

void WriteAheadLogDeserializer::ReplayVersion() {
	state.wal_version = deserializer.ReadProperty<idx_t>(101, "version");

	auto &single_file_block_manager = db.GetStorageManager().GetBlockManager().Cast<SingleFileBlockManager>();
	data_t db_identifier[MainHeader::DB_IDENTIFIER_LEN];
	bool is_set = false;
	deserializer.ReadOptionalList(102, "db_identifier", [&](Deserializer::List &list, idx_t i) {
		if (i >= MainHeader::DB_IDENTIFIER_LEN) {
			throw DataCorruptionException("Corrupt file - database identifier in header out of range");
		}
		db_identifier[i] = list.ReadElement<uint8_t>();
		is_set = true;
	});
	auto checkpoint_iteration = deserializer.ReadPropertyWithDefault<optional_idx>(103, "checkpoint_iteration");
	if (!is_set || !checkpoint_iteration.IsValid()) {
		return;
	}
	auto expected_db_identifier = single_file_block_manager.GetDBIdentifier();
	if (!MainHeader::CompareDBIdentifiers(db_identifier, expected_db_identifier)) {
		throw IOException("WAL does not match database file.");
	}

	auto wal_checkpoint_iteration = checkpoint_iteration.GetIndex();
	auto expected_checkpoint_iteration = single_file_block_manager.GetCheckpointIteration();
	if (expected_checkpoint_iteration != wal_checkpoint_iteration) {
		if (wal_checkpoint_iteration + 1 == expected_checkpoint_iteration) {
			// this iteration is exactly one lower than the expected iteration
			// this can happen if we aborted AFTER checkpointing the file, but BEFORE truncating the WAL
			// expect this situation to occur - we will throw an error if it does not later on
			state.expected_checkpoint_id = expected_checkpoint_iteration;
			return;
		}
		if (state.replay_state == WALReplayState::CHECKPOINT_WAL &&
		    wal_checkpoint_iteration == expected_checkpoint_iteration + 1) {
			// if we are recovering from a checkpoint WAL, the checkpoint iteration is possibly one higher
			// (depending on when the crash happened)
			return;
		}
		ThrowVersionError(wal_checkpoint_iteration, expected_checkpoint_iteration);
	}
}

//! WAL names carry schema paths without a catalog component.
static QualifiedName ReplayEntryName(Catalog &catalog, const QualifiedName &entry_name) {
	return QualifiedName::FromCatalogSchema(catalog.GetName(), entry_name.Parent().Path(), entry_name.Name());
}

//! Re-qualify a serialized [catalog, schema_path..., name] entry name (as carried by a CreateInfo) for the catalog it
//! is replayed into: the (possibly nested) schema path is kept, the serialized catalog component is replaced
static QualifiedName ReplayQualifiedName(Catalog &catalog, const QualifiedName &entry_name, const Identifier &name) {
	return entry_name.WithCatalog(catalog.GetName()).WithName(name);
}

//===--------------------------------------------------------------------===//
// Replay Table
//===--------------------------------------------------------------------===//
void WriteAheadLogDeserializer::ReplayCreateTable() {
	auto entry = WALCreateTable::Deserialize(deserializer);
	auto info = std::move(entry.table);
	if (DeserializeOnly()) {
		return;
	}
	// bind the constraints to the table again
	auto binder = Binder::CreateBinder(context);
	// the qualified name is [catalog, schema_path..., name] - navigate the (possibly nested) schema path
	auto &path = info->GetQualifiedName().Path();
	vector<Identifier> schema_path(path.begin() + 1, path.end() - 1);
	auto &schema =
	    *catalog.GetSchema(catalog.GetCatalogTransaction(context), schema_path, OnEntryNotFound::THROW_EXCEPTION);
	auto bound_info = Binder::BindCreateTableCheckpoint(std::move(info), schema);

	catalog.CreateTable(context, *bound_info);
}

void WriteAheadLogDeserializer::ReplayDropTable() {
	auto entry = WALDropTable::Deserialize(deserializer);
	DropInfo info;

	info.type = CatalogType::TABLE_ENTRY;
	info.SetQualifiedName(ReplayEntryName(catalog, entry.qualified_name));
	if (DeserializeOnly()) {
		return;
	}

	// Remove any replay indexes of this table.
	auto &table_entry = catalog.GetEntry<TableCatalogEntry>(context, info.GetQualifiedName());
	state.replay_index_infos.erase(std::remove_if(state.replay_index_infos.begin(), state.replay_index_infos.end(),
	                                              [&table_entry](const ReplayState::ReplayIndexInfo &replay_info) {
		                                              return replay_info.table_oid == table_entry.oid;
	                                              }),
	                               state.replay_index_infos.end());

	catalog.DropEntry(context, info);
}

void ReplayWithoutIndex(ClientContext &context, Catalog &catalog, AlterInfo &info, const bool only_deserialize) {
	if (only_deserialize) {
		return;
	}
	// the WAL carries the catalog name the entry had when it was written - re-root it in the catalog we replay into
	// (the database can be attached under a different name)
	info.SetQualifiedName(info.GetQualifiedName().WithCatalog(catalog.GetName()));
	catalog.Alter(context, info);
}

void WriteAheadLogDeserializer::ReplayIndexData(IndexStorageInfo &info) {
	D_ASSERT(info.IsValid() && !info.name.empty());

	auto &single_file_sm = db.GetStorageManager().Cast<SingleFileStorageManager>();
	auto &block_manager = single_file_sm.block_manager;
	auto &buffer_manager = block_manager->buffer_manager;

	deserializer.ReadList(103, "index_storage", [&](Deserializer::List &list, idx_t i) {
		auto &data_info = info.allocator_infos[i];

		// Read the data into buffer handles and convert them to blocks on disk.
		for (idx_t j = 0; j < data_info.allocation_sizes.size(); j++) {
			if (data_info.allocation_sizes[j] > block_manager->GetBlockSize()) {
				throw DataCorruptionException("Corrupt WAL: index allocation size (%llu) exceeds the block size (%llu)",
				                              data_info.allocation_sizes[j], block_manager->GetBlockSize());
			}
			// Read the data into a buffer handle.
			auto buffer_handle = buffer_manager.Allocate(MemoryTag::ART_INDEX, block_manager.get(), false);
			auto block_handle = buffer_handle.GetBlockHandle();
			auto data_ptr = buffer_handle.GetDataMutable();

			list.ReadElement<bool>(data_ptr, data_info.allocation_sizes[j]);

			// For read-only mode, retain the transient block handle and release the pin held by the buffer handle. The
			// buffer can then be evicted to temporary storage until the index is bound.
			if (db.IsReadOnly()) {
				if (!data_info.transient_block_handles) {
					data_info.transient_block_handles = make_shared_ptr<vector<shared_ptr<BlockHandle>>>();
				}
				data_info.transient_block_handles->push_back(std::move(block_handle));
				continue;
			}

			// Convert the buffer handle to a persistent block and store the block id.
			if (!deserialize_only) {
				auto block_id = block_manager->GetFreeBlockIdForCheckpoint();
				block_manager->ConvertToPersistent(context, block_id, std::move(block_handle),
				                                   std::move(buffer_handle));
				data_info.block_pointers[j].block_id = block_id;
			}
		}
	});
}

void WriteAheadLogDeserializer::ReplayAlter() {
	auto info = deserializer.ReadProperty<unique_ptr<ParseInfo>>(101, "info");
	auto &alter_info = info->Cast<AlterInfo>();
	alter_info.bind_mode = AlterBindMode::SKIP_BINDING;
	if (!alter_info.IsAddUniqueConstraint()) {
		return ReplayWithoutIndex(context, catalog, alter_info, DeserializeOnly());
	}

	auto index_storage_info = deserializer.ReadProperty<IndexStorageInfo>(102, "index_storage_info");
	ReplayIndexData(index_storage_info);
	if (DeserializeOnly()) {
		return;
	}

	auto &table_info = alter_info.Cast<AlterTableInfo>();
	auto &constraint_info = table_info.Cast<AddConstraintInfo>();
	auto &unique_info = constraint_info.constraint->Cast<UniqueConstraint>();

	auto table_name = ReplayQualifiedName(catalog, table_info.GetQualifiedName(), table_info.GetQualifiedName().Name());
	auto &table = catalog.GetEntry<TableCatalogEntry>(context, table_name).Cast<DuckTableEntry>();
	auto &column_list = table.GetColumns();

	// Add the table to the bind context to bind the parsed expressions.
	auto binder = Binder::CreateBinder(context);
	vector<LogicalType> column_types;
	vector<string> column_names;
	for (auto &col : column_list.Logical()) {
		column_types.push_back(col.Type());
		column_names.emplace_back(col.Name());
	}

	// Create a binder to bind the parsed expressions.
	vector<ColumnIndex> column_indexes;
	binder->bind_context.AddBaseTable(TableIndex(0), Identifier(), StringsToIdentifiers(column_names), column_types,
	                                  column_indexes, table);
	IndexBinder idx_binder(*binder, context);

	// Bind the parsed expressions to create unbound expressions.
	vector<unique_ptr<Expression>> unbound_expressions;
	auto logical_indexes = unique_info.GetLogicalIndexes(column_list);
	for (const auto &logical_index : logical_indexes) {
		auto &col = column_list.GetColumn(logical_index);
		unique_ptr<ParsedExpression> parsed =
		    make_uniq<ColumnRefExpression>(col.GetName(), table_info.GetQualifiedName().Name());
		unbound_expressions.push_back(idx_binder.Bind(parsed));
	}

	vector<column_t> column_ids;
	column_ids.reserve(logical_indexes.size());
	for (const auto &logical_index : logical_indexes) {
		column_ids.push_back(column_list.LogicalToPhysical(logical_index).index);
	}

	auto &storage = table.GetStorage();
	CreateIndexInput input(context, TableIOManager::Get(storage), storage.db, unique_info.GetIndexConstraintType(),
	                       index_storage_info.name, column_ids, unbound_expressions, index_storage_info,
	                       index_storage_info.options);

	auto index_type = context.db->config.GetIndexTypes().FindByName(ART::TYPE_NAME);
	auto index_instance = index_type->create_instance(input);

	auto &table_index_list = storage.GetDataTableInfo()->GetIndexes();
	auto index_oid = DatabaseManager::Get(context).NextOid();
	unique_info.SetBackingIndexOid(index_oid);
	catalog.Alter(context, alter_info);
	state.replay_index_infos.emplace_back(table_index_list, std::move(index_instance), table.oid, index_oid,
	                                      unique_info.check_mode);
}

//===--------------------------------------------------------------------===//
// Replay View
//===--------------------------------------------------------------------===//
void WriteAheadLogDeserializer::ReplayCreateView() {
	auto wal_entry = WALCreateView::Deserialize(deserializer);
	auto &entry = wal_entry.view;
	if (DeserializeOnly()) {
		return;
	}
	catalog.CreateView(context, entry->Cast<CreateViewInfo>());
}

void WriteAheadLogDeserializer::ReplayDropView() {
	auto entry = WALDropView::Deserialize(deserializer);
	DropInfo info;
	info.type = CatalogType::VIEW_ENTRY;
	info.SetQualifiedName(ReplayEntryName(catalog, entry.qualified_name));
	if (DeserializeOnly()) {
		return;
	}
	catalog.DropEntry(context, info);
}

//===--------------------------------------------------------------------===//
// Replay Schema
//===--------------------------------------------------------------------===//
void WriteAheadLogDeserializer::ReplayCreateSchema() {
	auto entry = WALCreateSchema::Deserialize(deserializer);
	CreateSchemaInfo info;
	auto schema_path = entry.qualified_name.Path();
	if (schema_path.empty()) {
		// Legacy WALs only store a top-level schema name.
		schema_path.push_back(std::move(entry.schema));
	}
	info.SetQualifiedName(QualifiedName::FromCatalogSchema(catalog.GetName(), std::move(schema_path), Identifier()));
	if (DeserializeOnly()) {
		return;
	}

	catalog.CreateSchema(context, info);
}

void WriteAheadLogDeserializer::ReplayDropSchema() {
	auto entry = WALDropSchema::Deserialize(deserializer);
	DropInfo info;
	info.type = CatalogType::SCHEMA_ENTRY;
	auto schema_name =
	    entry.qualified_name.Path().empty() ? QualifiedName(std::move(entry.schema)) : std::move(entry.qualified_name);
	auto path = schema_name.Path();
	path.insert(path.begin(), catalog.GetName());
	info.SetQualifiedName(QualifiedName::FromPath(std::move(path)));
	if (DeserializeOnly()) {
		return;
	}

	catalog.DropEntry(context, info);
}

//===--------------------------------------------------------------------===//
// Replay Custom Type
//===--------------------------------------------------------------------===//
void WriteAheadLogDeserializer::ReplayCreateType() {
	auto wal_entry = WALCreateType::Deserialize(deserializer);
	auto &info = wal_entry.type;
	info->on_conflict = OnCreateConflict::IGNORE_ON_CONFLICT;
	if (DeserializeOnly()) {
		return;
	}
	catalog.CreateType(context, info->Cast<CreateTypeInfo>());
}

void WriteAheadLogDeserializer::ReplayDropType() {
	auto entry = WALDropType::Deserialize(deserializer);
	DropInfo info;

	info.type = CatalogType::TYPE_ENTRY;
	info.SetQualifiedName(ReplayEntryName(catalog, entry.qualified_name));
	if (DeserializeOnly()) {
		return;
	}

	catalog.DropEntry(context, info);
}

//===--------------------------------------------------------------------===//
// Replay Trigger
//===--------------------------------------------------------------------===//
void WriteAheadLogDeserializer::ReplayCreateTrigger() {
	auto wal_entry = WALCreateTrigger::Deserialize(deserializer);
	auto &info = wal_entry.trigger;
	info->on_conflict = OnCreateConflict::IGNORE_ON_CONFLICT;
	if (DeserializeOnly()) {
		return;
	}
	auto &trigger_info = info->Cast<CreateTriggerInfo>();
	// the trigger lives in the same (possibly nested) schema as its base table
	auto &table = Catalog::GetEntry<TableCatalogEntry>(
	    context, ReplayQualifiedName(catalog, trigger_info.GetQualifiedName(), trigger_info.base_table->Table()));
	auto &duck_table = table.Cast<DuckTableEntry>();
	auto transaction = catalog.GetCatalogTransaction(context);
	duck_table.CreateTrigger(transaction, trigger_info);
}

void WriteAheadLogDeserializer::ReplayDropTrigger() {
	auto entry = WALDropTrigger::Deserialize(deserializer);
	DropInfo info;
	info.type = CatalogType::TRIGGER_ENTRY;
	auto table_name = std::move(entry.table);
	info.SetQualifiedName(ReplayEntryName(catalog, entry.qualified_name));
	if (DeserializeOnly()) {
		return;
	}
	if (table_name.empty()) {
		throw InternalException("WAL replay: DROP TRIGGER entry has an empty table name for trigger %s",
		                        info.GetQualifiedName().Name());
	}
	// the trigger lives in the same (possibly nested) schema as its base table
	auto &table =
	    Catalog::GetEntry<TableCatalogEntry>(context, info.GetQualifiedName().WithName(std::move(table_name)));
	auto &duck_table = table.Cast<DuckTableEntry>();
	auto transaction = catalog.GetCatalogTransaction(context);
	duck_table.DropTrigger(transaction, info.GetQualifiedName().Name(), info.cascade);
}

//===--------------------------------------------------------------------===//
// Replay Sequence
//===--------------------------------------------------------------------===//
void WriteAheadLogDeserializer::ReplayCreateSequence() {
	auto wal_entry = WALCreateSequence::Deserialize(deserializer);
	auto &entry = wal_entry.sequence;
	if (DeserializeOnly()) {
		return;
	}

	catalog.CreateSequence(context, entry->Cast<CreateSequenceInfo>());
}

void WriteAheadLogDeserializer::ReplayDropSequence() {
	auto entry = WALDropSequence::Deserialize(deserializer);
	DropInfo info;
	info.type = CatalogType::SEQUENCE_ENTRY;
	info.SetQualifiedName(ReplayEntryName(catalog, entry.qualified_name));
	if (DeserializeOnly()) {
		return;
	}

	catalog.DropEntry(context, info);
}

void WriteAheadLogDeserializer::ReplaySequenceValue() {
	auto entry = WALSequenceValue::Deserialize(deserializer);

	if (DeserializeOnly()) {
		return;
	}

	// fetch the sequence from the catalog
	auto &seq = catalog.GetEntry<SequenceCatalogEntry>(context, ReplayEntryName(catalog, entry.qualified_name));
	seq.ReplayValue(entry.usage_count, entry.counter, entry.last_value);
	auto &storage_manager = db.GetStorageManager();
	if (storage_manager.HasStorageVersion() &&
	    StorageManager::IsPriorToVersion(StorageVersion::V2_0_0, storage_manager.GetStorageVersion())) {
		seq.DeriveLastValue();
	}
}

//===--------------------------------------------------------------------===//
// Replay Macro
//===--------------------------------------------------------------------===//
void WriteAheadLogDeserializer::ReplayCreateMacro() {
	auto wal_entry = WALCreateMacro::Deserialize(deserializer);
	auto &entry = wal_entry.macro;
	if (DeserializeOnly()) {
		return;
	}

	catalog.CreateFunction(context, entry->Cast<CreateMacroInfo>());
}

void WriteAheadLogDeserializer::ReplayDropMacro() {
	auto entry = WALDropMacro::Deserialize(deserializer);
	DropInfo info;
	info.type = CatalogType::MACRO_ENTRY;
	info.SetQualifiedName(ReplayEntryName(catalog, entry.qualified_name));
	if (DeserializeOnly()) {
		return;
	}

	catalog.DropEntry(context, info);
}

//===--------------------------------------------------------------------===//
// Replay Table Macro
//===--------------------------------------------------------------------===//
void WriteAheadLogDeserializer::ReplayCreateTableMacro() {
	auto wal_entry = WALCreateTableMacro::Deserialize(deserializer);
	auto &entry = wal_entry.table_macro;
	if (DeserializeOnly()) {
		return;
	}
	catalog.CreateFunction(context, entry->Cast<CreateMacroInfo>());
}

void WriteAheadLogDeserializer::ReplayDropTableMacro() {
	auto entry = WALDropTableMacro::Deserialize(deserializer);
	DropInfo info;
	info.type = CatalogType::TABLE_MACRO_ENTRY;
	info.SetQualifiedName(ReplayEntryName(catalog, entry.qualified_name));
	if (DeserializeOnly()) {
		return;
	}

	catalog.DropEntry(context, info);
}

//===--------------------------------------------------------------------===//
// Replay Index
//===--------------------------------------------------------------------===//
void WriteAheadLogDeserializer::ReplayCreateIndex() {
	auto create_info = deserializer.ReadProperty<unique_ptr<CreateInfo>>(101, "index_catalog_entry");
	auto index_info = deserializer.ReadProperty<IndexStorageInfo>(102, "index_storage_info");

	ReplayIndexData(index_info);
	if (DeserializeOnly()) {
		return;
	}
	auto &info = create_info->Cast<CreateIndexInfo>();

	// Ensure that the index type exists.
	if (info.index_type.empty()) {
		info.index_type = ART::TYPE_NAME;
	}

	// the table lives in the same (possibly nested) schema as the index
	auto table_name = ReplayQualifiedName(catalog, create_info->GetQualifiedName(), info.table);
	auto &entry = catalog.GetEntry<TableCatalogEntry>(context, table_name);
	auto &table = entry.Cast<DuckTableEntry>();
	auto &storage = table.GetStorage();
	auto &io_manager = TableIOManager::Get(storage);

	// Create the index in the catalog.
	auto index_entry = table.schema.CreateIndex(context, info, table);

	// add the index to the storage
	auto unbound_index = make_uniq<UnboundIndex>(std::move(create_info), std::move(index_info), io_manager, db);

	auto &table_index_list = storage.GetDataTableInfo()->GetIndexes();
	state.replay_index_infos.emplace_back(table_index_list, std::move(unbound_index), table.oid, index_entry->oid,
	                                      ConstraintCheckMode::DEFAULT);
}

void WriteAheadLogDeserializer::ReplayDropIndex() {
	auto entry = WALDropIndex::Deserialize(deserializer);
	DropInfo info;
	info.type = CatalogType::INDEX_ENTRY;
	info.SetQualifiedName(ReplayEntryName(catalog, entry.qualified_name));
	if (DeserializeOnly()) {
		return;
	}

	// Remove the replay index, if any. Match on the index entry's oid.
	auto &index_entry = catalog.GetEntry<IndexCatalogEntry>(context, info.GetQualifiedName());
	state.replay_index_infos.erase(std::remove_if(state.replay_index_infos.begin(), state.replay_index_infos.end(),
	                                              [&index_entry](const ReplayState::ReplayIndexInfo &replay_info) {
		                                              return replay_info.index_oid == index_entry.oid;
	                                              }),
	                               state.replay_index_infos.end());

	catalog.DropEntry(context, info);
}

//===--------------------------------------------------------------------===//
// Replay Data
//===--------------------------------------------------------------------===//
void WriteAheadLogDeserializer::ReplayUseTable() {
	auto entry = WALUseTable::Deserialize(deserializer);
	if (DeserializeOnly()) {
		return;
	}
	state.current_table = &catalog.GetEntry<DuckTableEntry>(context, ReplayEntryName(catalog, entry.qualified_name));
}

void WriteAheadLogDeserializer::ReplayInsert() {
	DataChunk chunk;
	deserializer.ReadObject(101, "chunk", [&](Deserializer &object) { chunk.Deserialize(object); });
	if (DeserializeOnly()) {
		return;
	}
	if (!state.current_table) {
		throw DataCorruptionException("Corrupt WAL: insert without table");
	}

	// Append to the current table without constraint verification.
	vector<unique_ptr<BoundConstraint>> bound_constraints;
	auto &storage = state.current_table->GetStorage();
	storage.LocalWALAppend(*state.current_table, context, chunk, bound_constraints);
}

void WriteAheadLogDeserializer::ReplayRowGroupData() {
	auto &block_manager = db.GetStorageManager().GetBlockManager();
	PersistentCollectionData data;
	deserializer.Set<DatabaseInstance &>(db.GetDatabase());
	CompressionInfo compression_info(block_manager);
	deserializer.Set<const CompressionInfo &>(compression_info);
	deserializer.ReadProperty(101, "row_group_data", data);
	deserializer.Unset<const CompressionInfo>();
	deserializer.Unset<DatabaseInstance>();
	if (DeserializeOnly()) {
		for (auto &block_id : data.GetBlockIds()) {
			state.row_group_blocks.push_back(block_id);
		}
		return;
	}
	if (!state.current_table) {
		throw DataCorruptionException("Corrupt WAL: insert without table");
	}
	auto &storage = state.current_table->GetStorage();
	auto &table_info = storage.GetDataTableInfo();
	auto base_row = storage.GetNextRowId();
	RowGroupCollection new_row_groups(table_info, table_info->GetIOManager(), storage.GetTypes(), base_row);
	new_row_groups.Initialize(data);

	// if we have any indexes - scan the row groups and add data to the indexes
	auto &indexes = table_info->GetIndexes();
	if (!indexes.Empty()) {
		auto &transaction = DuckTransaction::Get(context, db);
		// we have indexes - append
		vector<StorageIndex> column_ids;
		for (auto &col : state.current_table->GetColumns().Physical()) {
			column_ids.emplace_back(col.StorageOid());
		}
		auto current_row_id = storage.GetNextRowId();
		for (auto &chunk : new_row_groups.Chunks(transaction, column_ids)) {
			// Deleted index entries are removed when the replay transaction commits. Duplicates can temporarily
			// exist, similar to tuple WAL replay.
			auto error = indexes.Append(nullptr, chunk, NumericCast<row_t>(current_row_id),
			                            IndexAppendMode::INSERT_DUPLICATES, optional_idx());
			if (error.HasError()) {
				throw InternalException("Failed to append to index during ROW_GROUP_DATA WAL replay: %s",
				                        error.Message());
			}
			current_row_id += chunk.size();
		}
	}
	storage.MergeStorage(new_row_groups, nullptr);
}

void WriteAheadLogDeserializer::ReplayDelete() {
	DataChunk chunk;
	deserializer.ReadObject(101, "chunk", [&](Deserializer &object) { chunk.Deserialize(object); });
	if (DeserializeOnly()) {
		return;
	}
	if (!state.current_table) {
		throw DataCorruptionException("Corrupt WAL: delete without table");
	}

	D_ASSERT(chunk.ColumnCount() == 1 && chunk.data[0].GetType() == LogicalType::ROW_TYPE);
	auto &row_identifiers = chunk.data[0];
	row_identifiers.Flatten();
	auto source_ids = FlatVector::GetData<row_t>(row_identifiers);

	// Delete the row IDs from the current table.
	auto &storage = state.current_table->GetStorage();
	auto next_row_id = storage.GetNextRowId();
	for (idx_t i = 0; i < chunk.size(); i++) {
		if (source_ids[i] >= UnsafeNumericCast<row_t>(next_row_id)) {
			throw DataCorruptionException("Corrupt WAL: row ID for delete out of bounds");
		}
	}
	TableDeleteState delete_state;
	storage.Delete(delete_state, context, *state.current_table, row_identifiers, chunk.size());
}

void WriteAheadLogDeserializer::ReplayUpdate() {
	auto column_path = deserializer.ReadProperty<vector<column_t>>(101, "column_indexes");

	DataChunk chunk;
	deserializer.ReadObject(102, "chunk", [&](Deserializer &object) { chunk.Deserialize(object); });

	if (DeserializeOnly()) {
		return;
	}
	if (!state.current_table) {
		throw DataCorruptionException("Corrupt WAL: update without table");
	}

	if (column_path.empty() || column_path[0] >= state.current_table->GetColumns().PhysicalColumnCount()) {
		throw DataCorruptionException("Corrupt WAL: column index for update out of bounds");
	}
	if (chunk.ColumnCount() != 2 || chunk.data[1].GetType() != LogicalType::ROW_TYPE) {
		throw DataCorruptionException("Corrupt WAL: update must contain the updated values and the row ids");
	}
	auto &column_type = state.current_table->GetColumns().GetColumn(PhysicalIndex(column_path[0])).Type();
	if (column_path.size() == 1 && chunk.data[0].GetType() != column_type) {
		throw DataCorruptionException("Corrupt WAL: update type does not match the column type");
	}

	// remove the row id vector from the chunk
	auto row_ids = std::move(chunk.data.back());
	chunk.data.pop_back();

	// now perform the update
	state.current_table->GetStorage().UpdateColumn(*state.current_table, context, row_ids, column_path, chunk);
}

void WriteAheadLogDeserializer::ReplayCheckpoint() {
	if (state.checkpoint_position.IsValid()) {
		throw DataCorruptionException("WAL cannot contain more than one checkpoint marker");
	}
	auto entry = WALCheckpoint::Deserialize(deserializer);
	state.checkpoint_id = entry.meta_block;
	state.checkpoint_position = state.current_position;
}

} // namespace duckdb
