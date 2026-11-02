#include "duckdb/storage/checkpoint_manager.hpp"

#include "duckdb/catalog/catalog_entry/duck_index_entry.hpp"
#include "duckdb/catalog/catalog_entry/duck_schema_entry.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/index_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/scalar_macro_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/sequence_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/type_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/trigger_catalog_entry.hpp"
#include "duckdb/parser/parsed_data/create_trigger_info.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/catalog/dependency_manager.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/common/assert.hpp"
#include "duckdb/common/enums/checkpoint_abort.hpp"
#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/thread.hpp"
#include "duckdb/common/vector_size.hpp"
#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/execution/index/unbound_index.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/storage/block_manager.hpp"
#include "duckdb/storage/checkpoint/table_data_reader.hpp"
#include "duckdb/storage/checkpoint/table_data_writer.hpp"
#include "duckdb/storage/metadata/metadata_reader.hpp"
#include "duckdb/storage/table/data_table_info.hpp"
#include "duckdb/storage/table/index_entry.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/transaction_manager.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"

namespace duckdb {

ActiveCheckpointWrapper::ActiveCheckpointWrapper(optional_ptr<ClientContext> context, AttachedDatabase &db_p,
                                                 DuckTransactionManager &transaction_manager_p)
    : db(db_p), transaction_manager(transaction_manager_p) {
	if (!context) {
		return;
	}
	checkpoint_connection = make_uniq<Connection>(Connection::CreateInternal(db.GetDatabase()));
	checkpoint_context = checkpoint_connection->context.get();
}

ActiveCheckpointWrapper::~ActiveCheckpointWrapper() {
	// This happens on failure before we commit the transaction.
	if (active) {
		transaction_manager.ResetActiveCheckpoint();
	}
	if (checkpoint_transaction) {
		transaction_manager.RollbackTransaction(*checkpoint_transaction);
		checkpoint_transaction = nullptr;
	}
	if (checkpoint_context) {
		checkpoint_context->transaction.ClearTransaction();
	}
}

void ActiveCheckpointWrapper::Begin(CheckpointOptions &options) {
	if (checkpoint_context) {
		// the start time of the checkpoint transaction defines the visibility for checkpointing
		checkpoint_context->transaction.BeginTransaction();
		checkpoint_context->transaction.SetReadOnly();
		auto &transaction = DuckTransaction::Get(*checkpoint_context, db);
		transaction.SetIsCheckpointTransaction();
		checkpoint_transaction = &transaction;
		// the checkpoint sees every commit before it started
		D_ASSERT(transaction.view.visibility_bound == VisibilityBound::Before(transaction.start_time));
		options.visibility_bound = transaction.view.visibility_bound;
	} else {
		options.visibility_bound = VisibilityBound::Through(transaction_manager.GetLastCommit());
	}
	options.checkpoint_id = transaction_manager.NextCheckpointId();
	transaction_manager.SetActiveCheckpoint(options.checkpoint_id.GetIndex());
	active = true;
}

void ActiveCheckpointWrapper::Commit() {
	transaction_manager.ResetActiveCheckpoint();
	active = false;
	if (!checkpoint_transaction) {
		return;
	}
	checkpoint_context->transaction.Commit();
	checkpoint_transaction = nullptr;
}

optional_ptr<ClientContext> ActiveCheckpointWrapper::GetCheckpointContext() const {
	return checkpoint_context;
}

void ReorderTableEntries(catalog_entry_vector_t &tables);

SingleFileCheckpointWriter::SingleFileCheckpointWriter(QueryContext context, AttachedDatabase &db,
                                                       BlockManager &block_manager, CheckpointOptions options_p)
    : CheckpointWriter(db), context(context.GetClientContext()),
      partial_block_manager(context, block_manager, PartialBlockType::FULL_CHECKPOINT),
      index_partial_block_manager(context, block_manager, PartialBlockType::FULL_CHECKPOINT), options(options_p) {
}

BlockManager &SingleFileCheckpointWriter::GetBlockManager() {
	auto &storage_manager = db.GetStorageManager().Cast<SingleFileStorageManager>();
	return *storage_manager.block_manager;
}

PartialBlockManager SingleFileCheckpointWriter::CreatePerIndexPartialBlockManager() {
	return PartialBlockManager(QueryContext(context), GetBlockManager(), PartialBlockType::FULL_CHECKPOINT);
}

MetadataWriter &SingleFileCheckpointWriter::GetMetadataWriter() {
	return *metadata_writer;
}

MetadataManager &SingleFileCheckpointWriter::GetMetadataManager() {
	return GetBlockManager().GetMetadataManager();
}

unique_ptr<TableDataWriter> SingleFileCheckpointWriter::GetTableDataWriter(TableCatalogEntry &table) {
	return make_uniq<SingleFileTableDataWriter>(*this, table, *table_metadata_writer);
}

static catalog_entry_vector_t GetCatalogEntries(vector<reference<DuckSchemaEntry>> &schemas, VisibilityBound bound) {
	catalog_entry_vector_t entries;
	for (auto &schema_p : schemas) {
		auto &schema = schema_p.get();
		entries.push_back(schema);
		schema.Scan(CatalogType::TYPE_ENTRY, bound, [&](CatalogEntry &entry) {
			if (entry.internal) {
				return;
			}
			entries.push_back(entry);
		});

		schema.Scan(CatalogType::SEQUENCE_ENTRY, bound, [&](CatalogEntry &entry) {
			if (entry.internal) {
				return;
			}
			entries.push_back(entry);
		});

		catalog_entry_vector_t tables;
		vector<reference<ViewCatalogEntry>> views;
		schema.Scan(CatalogType::TABLE_ENTRY, bound, [&](CatalogEntry &entry) {
			if (entry.internal) {
				return;
			}
			if (entry.type == CatalogType::TABLE_ENTRY) {
				tables.push_back(entry.Cast<TableCatalogEntry>());
			} else if (entry.type == CatalogType::VIEW_ENTRY) {
				views.push_back(entry.Cast<ViewCatalogEntry>());
			} else {
				throw NotImplementedException("Catalog type for entries");
			}
		});
		// Reorder tables because of foreign key constraint
		ReorderTableEntries(tables);
		for (auto &table : tables) {
			entries.push_back(table.get());
		}
		for (auto &view : views) {
			entries.push_back(view.get());
		}

		// Scan triggers from each table directly (triggers are nested under their table)
		for (auto &table_entry : tables) {
			auto &table = table_entry.get().Cast<TableCatalogEntry>();
			if (!table.IsDuckTable()) {
				continue;
			}
			auto &duck_table = table.Cast<DuckTableEntry>();
			duck_table.ScanTriggers(bound, [&](CatalogEntry &entry) {
				if (!entry.internal) {
					entries.push_back(entry);
				}
			});
		}

		schema.Scan(CatalogType::SCALAR_FUNCTION_ENTRY, bound, [&](CatalogEntry &entry) {
			if (entry.internal) {
				return;
			}
			if (entry.type == CatalogType::MACRO_ENTRY) {
				entries.push_back(entry);
			}
		});

		schema.Scan(CatalogType::TABLE_FUNCTION_ENTRY, bound, [&](CatalogEntry &entry) {
			if (entry.internal) {
				return;
			}
			if (entry.type == CatalogType::TABLE_MACRO_ENTRY) {
				entries.push_back(entry);
			}
		});

		schema.Scan(CatalogType::INDEX_ENTRY, bound, [&](CatalogEntry &entry) {
			D_ASSERT(!entry.internal);
			entries.push_back(entry);
		});
	}
	return entries;
}

static bool HasBufferedIndexReplays(AttachedDatabase &db) {
	bool has_buffered_replays = false;
	auto &catalog = Catalog::GetCatalog(db).Cast<DuckCatalog>();
	catalog.ScanSchemas(VisibilityBound::AllCommitted(), [&](DuckSchemaEntry &schema) {
		if (has_buffered_replays) {
			return;
		}
		schema.Scan(CatalogType::TABLE_ENTRY, VisibilityBound::AllCommitted(), [&](CatalogEntry &entry) {
			if (has_buffered_replays || entry.type != CatalogType::TABLE_ENTRY) {
				return;
			}
			auto &table = entry.Cast<DuckTableEntry>();
			auto &indexes = table.GetStorage().GetDataTableInfo()->GetIndexes();
			has_buffered_replays = indexes.HasBufferedReplays();
		});
	});
	return has_buffered_replays;
}

void SingleFileCheckpointWriter::CreateCheckpoint() {
	auto &storage_manager = db.GetStorageManager().Cast<SingleFileStorageManager>();
	if (storage_manager.InMemory()) {
		return;
	}
	if (ValidChecker::IsInvalidated(db.GetDatabase())) {
		// don't checkpoint invalidated databases
		return;
	}
	// A context-free checkpoint cannot persist buffered operations on an unbound index. Keep the WAL instead.
	if (!context && HasBufferedIndexReplays(db)) {
		return;
	}
	// assert that the checkpoint manager hasn't been used before
	D_ASSERT(!metadata_writer);

	auto &block_manager = GetBlockManager();
	auto &metadata_manager = GetMetadataManager();

	//! Set up the writers for the checkpoints
	metadata_writer = make_uniq<MetadataWriter>(metadata_manager);
	table_metadata_writer = make_uniq<MetadataWriter>(metadata_manager);

	// get the id of the first meta block
	auto meta_block = metadata_writer->GetMetaBlockPointer();

	// write a checkpoint flag to the WAL
	// in case a crash happens during the checkpoint, we know a checkpoint was instantiated
	// we write the root meta block of the planned checkpoint to the WAL
	// during recovery we use this:
	// * if the root meta block matches the checkpoint entry, we know the checkpoint was completed
	// * if the root meta block does not match the checkpoint entry, we know the checkpoint was not completed
	// if the checkpoint was completed we don't need to replay the WAL - otherwise we need to replay the WAL
	// we also know if a checkpoint was running that we need to check for the checkpoint WAL (`.checkpoint.wal`)
	// to replay any concurrent commits that have succeeded and ensure these are not lost
	auto &transaction_manager = db.GetTransactionManager().Cast<DuckTransactionManager>();

	// If there is a context (non shutdown path): this will create a new connection for the checkpoint, then in
	// WALStartCheckpoint we will create a transaction for the checkpoint.
	ActiveCheckpointWrapper active_checkpoint(context, db, transaction_manager);
	auto has_wal = storage_manager.WALStartCheckpoint(meta_block, options, active_checkpoint);
	checkpoint_context = active_checkpoint.GetCheckpointContext();

	catalog_entry_vector_t catalog_entries;

	auto checkpoint_sleep_ms = Settings::Get<DebugCheckpointSleepMsSetting>(db.GetDatabase());
	if (checkpoint_sleep_ms > 0) {
		ThreadUtil::SleepMs(checkpoint_sleep_ms);
	}

	vector<reference<DuckSchemaEntry>> schemas;
	// write the catalog as of the bound: later commits are in the WAL that outlives this checkpoint
	auto &catalog = Catalog::GetCatalog(db).Cast<DuckCatalog>();
	catalog.ScanSchemas(options.visibility_bound, [&](DuckSchemaEntry &entry) { schemas.push_back(entry); });

	D_ASSERT(catalog.IsDuckCatalog());

	auto &dependency_manager = *catalog.GetDependencyManager();
	catalog_entries = GetCatalogEntries(schemas, options.visibility_bound);
	dependency_manager.ReorderEntries(catalog_entries, options.visibility_bound);

	// write the actual data into the database

	// Create a serializer to write the checkpoint data
	// The serialized format is roughly:
	/*
	    {
	        schemas: [
	            {
	                schema: <schema_info>,
	                custom_types: [ { type: <type_info> }, ... ],
	                sequences: [ { sequence: <sequence_info> }, ... ],
	                tables: [ { table: <table_info> }, ... ],
	                views: [ { view: <view_info> }, ... ],
	                macros: [ { macro: <macro_info> }, ... ],
	                table_macros: [ { table_macro: <table_macro_info> }, ... ],
	                indexes: [ { index: <index_info>, root_offset <block_ptr> }, ... ]
	            }
	        ]
	    }
	 */
	BinarySerializer serializer(*metadata_writer, SerializationOptions(db));
	serializer.Begin();
	serializer.WriteList(100, "catalog_entries", catalog_entries.size(), [&](Serializer::List &list, idx_t i) {
		auto &entry = catalog_entries[i];
		list.WriteObject([&](Serializer &obj) { WriteEntry(entry.get(), obj); });
	});
	serializer.End();

	metadata_writer->Flush();
	table_metadata_writer->Flush();

	auto debug_checkpoint_abort = Settings::Get<DebugCheckpointAbortSetting>(db.GetDatabase());
	if (debug_checkpoint_abort == CheckpointAbort::DEBUG_ABORT_BEFORE_HEADER) {
		throw FatalException("Checkpoint aborted before header write because of PRAGMA checkpoint_abort flag");
	}
	if (debug_checkpoint_abort == CheckpointAbort::DEBUG_ABORT_BEFORE_HEADER_NON_FATAL) {
		throw IOException("Checkpoint aborted before header write (non-fatal) because of PRAGMA checkpoint_abort flag");
	}

	// finally write the updated header
	DatabaseHeader header;
	header.meta_block = meta_block.block_pointer;
	header.block_alloc_size = block_manager.GetBlockAllocSize();
	header.vector_size = STANDARD_VECTOR_SIZE;
	block_manager.WriteHeader(context, header);

	auto debug_verify_blocks = Settings::Get<DebugVerifyBlocksSetting>(db.GetDatabase());
	if (debug_verify_blocks) {
		// extend verify_block_usage_count
		auto metadata_info = storage_manager.GetMetadataInfo();
		for (auto &info : metadata_info) {
			verify_block_usage_count[info.block_id]++;
		}
		for (auto &entry_ref : catalog_entries) {
			auto &entry = entry_ref.get();
			if (entry.type != CatalogType::TABLE_ENTRY) {
				continue;
			}
			auto &table = entry.Cast<DuckTableEntry>();
			auto &storage = table.GetStorage();
			auto segment_info = storage.GetColumnSegmentInfo(context);
			for (auto &segment : segment_info) {
				verify_block_usage_count[segment.block_id]++;
				if (StringUtil::Contains(segment.segment_info, "Overflow String Block Ids: ")) {
					auto overflow_blocks = StringUtil::Replace(segment.segment_info, "Overflow String Block Ids: ", "");
					auto splits = StringUtil::Split(overflow_blocks, ", ");
					for (auto &split : splits) {
						auto overflow_block_id = std::stoll(split);
						verify_block_usage_count[overflow_block_id]++;
					}
				}
			}
		}
		block_manager.VerifyBlocks(verify_block_usage_count);
	}

	if (debug_checkpoint_abort == CheckpointAbort::DEBUG_ABORT_BEFORE_TRUNCATE) {
		throw FatalException("Checkpoint aborted before truncate because of PRAGMA checkpoint_abort flag");
	}

	// truncate the file
	block_manager.Truncate();

	if (debug_checkpoint_abort == CheckpointAbort::DEBUG_ABORT_BEFORE_WAL_FINISH) {
		throw FatalException("Checkpoint aborted before truncate because of PRAGMA checkpoint_abort flag");
	}

	// truncate the WAL
	if (has_wal) {
		unique_lock<mutex> owned_commit_lock;
		optional_ptr<unique_lock<mutex>> commit_lock;
		if (!options.commit_lock) {
			// not holding the commit lock yet - grab it
			owned_commit_lock = storage_manager.GetCommitLock();
			commit_lock = owned_commit_lock;
		} else {
			// we already have the commit lock - just refer to it
			commit_lock = options.commit_lock;
		}
		storage_manager.WALFinishCheckpoint(*commit_lock);
	}

	// for any indexes that were appended to while checkpointing, merge the delta back into the main index
	// FIXME: we only clean up appends made to tables that are part of this checkpoint
	// Currently, that is correct, since we don't allow creating tables DURING a checkpoint
	// In the future, we will allow this
	// When that happens, we should ensure the delta indexes are NOT used for tables created DURING a checkpoint
	// this is also not necessary - if we are not checkpointing a table, we are not checkpointing its indexes
	// ergo we don't need the delta indexes
	for (auto &entry_ref : catalog_entries) {
		auto &entry = entry_ref.get();
		if (entry.type != CatalogType::TABLE_ENTRY) {
			continue;
		}
		auto &table = entry.Cast<DuckTableEntry>();
		auto &storage = table.GetStorage();
		auto &table_info = storage.GetDataTableInfo();
		auto &index_list = table_info->GetIndexes();
		index_list.MergeCheckpointDeltas(options.checkpoint_id);
	}
	checkpoint_context = nullptr;
	active_checkpoint.Commit();
}

void CheckpointReader::LoadCheckpoint(CatalogTransaction transaction, MetadataReader &reader) {
	BinaryDeserializer deserializer(reader);
	deserializer.Set<Catalog &>(catalog);
	deserializer.Begin();
	deserializer.ReadList(100, "catalog_entries", [&](Deserializer::List &list, idx_t i) {
		return list.ReadObject([&](Deserializer &obj) { ReadEntry(transaction, obj); });
	});
	deserializer.End();
	deserializer.Unset<Catalog>();
}

MetadataManager &SingleFileCheckpointReader::GetMetadataManager() {
	return storage.block_manager->GetMetadataManager();
}

void SingleFileCheckpointReader::LoadFromStorage() {
	auto &block_manager = *storage.block_manager;
	auto &metadata_manager = GetMetadataManager();
	MetaBlockPointer meta_block(block_manager.GetMetaBlock(), 0);
	if (!meta_block.IsValid()) {
		// storage is empty
		return;
	}

	if (block_manager.Prefetch()) {
		auto metadata_blocks = metadata_manager.GetBlocks();
		auto &buffer_manager = BufferManager::GetBufferManager(storage.GetDatabase());
		// Database load happens before any query is running, so there is no query context to attribute to.
		buffer_manager.Prefetch(QueryContext(), metadata_blocks);
	}

	// create the MetadataReader to read from the storage
	MetadataReader reader(metadata_manager, meta_block);
	auto transaction = CatalogTransaction::GetSystemTransaction(catalog.GetDatabase());
	LoadCheckpoint(transaction, reader);
}

void CheckpointWriter::WriteEntry(CatalogEntry &entry, Serializer &serializer) {
	serializer.WriteProperty(99, "catalog_type", entry.type);

	switch (entry.type) {
	case CatalogType::SCHEMA_ENTRY: {
		auto &schema = entry.Cast<SchemaCatalogEntry>();
		WriteSchema(schema, serializer);
		break;
	}
	case CatalogType::TYPE_ENTRY: {
		auto &custom_type = entry.Cast<TypeCatalogEntry>();
		WriteType(custom_type, serializer);
		break;
	}
	case CatalogType::SEQUENCE_ENTRY: {
		auto &seq = entry.Cast<SequenceCatalogEntry>();
		WriteSequence(seq, serializer);
		break;
	}
	case CatalogType::TABLE_ENTRY: {
		auto &table = entry.Cast<TableCatalogEntry>();
		WriteTable(table, serializer);
		break;
	}
	case CatalogType::VIEW_ENTRY: {
		auto &view = entry.Cast<ViewCatalogEntry>();
		WriteView(view, serializer);
		break;
	}
	case CatalogType::MACRO_ENTRY: {
		auto &macro = entry.Cast<ScalarMacroCatalogEntry>();
		WriteMacro(macro, serializer);
		break;
	}
	case CatalogType::TABLE_MACRO_ENTRY: {
		auto &macro = entry.Cast<TableMacroCatalogEntry>();
		WriteTableMacro(macro, serializer);
		break;
	}
	case CatalogType::INDEX_ENTRY: {
		auto &index = entry.Cast<IndexCatalogEntry>();
		WriteIndex(index, serializer);
		break;
	}
	case CatalogType::TRIGGER_ENTRY: {
		auto &trigger = entry.Cast<TriggerCatalogEntry>();
		WriteTrigger(trigger, serializer);
		break;
	}
	default:
		throw InternalException("Unrecognized catalog type in CheckpointWriter::WriteEntry");
	}
}

//===--------------------------------------------------------------------===//
// Schema
//===--------------------------------------------------------------------===//
void CheckpointWriter::WriteSchema(SchemaCatalogEntry &schema, Serializer &serializer) {
	// write the schema data
	serializer.WriteProperty(100, "schema", &schema);
}

static unique_ptr<CreateInfo> ReadCreateInfo(Deserializer &deserializer, CatalogType expected_type,
                                             const char *entry_name) {
	// 100 is the serialization field ID for the catalog entry's CreateInfo payload
	auto info = deserializer.ReadProperty<unique_ptr<CreateInfo>>(100, entry_name);
	if (!info) {
		throw DataCorruptionException("corrupt database file - %s entry without create info", entry_name);
	}
	if (info->type != expected_type) {
		throw DataCorruptionException("corrupt database file - catalog entry type mismatch for %s", entry_name);
	}
	return info;
}

void CheckpointReader::ReadEntry(CatalogTransaction transaction, Deserializer &deserializer) {
	auto type = deserializer.ReadProperty<CatalogType>(99, "type");

	switch (type) {
	case CatalogType::SCHEMA_ENTRY: {
		ReadSchema(transaction, deserializer);
		break;
	}
	case CatalogType::TYPE_ENTRY: {
		ReadType(transaction, deserializer);
		break;
	}
	case CatalogType::SEQUENCE_ENTRY: {
		ReadSequence(transaction, deserializer);
		break;
	}
	case CatalogType::TABLE_ENTRY: {
		ReadTable(transaction, deserializer);
		break;
	}
	case CatalogType::VIEW_ENTRY: {
		ReadView(transaction, deserializer);
		break;
	}
	case CatalogType::MACRO_ENTRY: {
		ReadMacro(transaction, deserializer);
		break;
	}
	case CatalogType::TABLE_MACRO_ENTRY: {
		ReadTableMacro(transaction, deserializer);
		break;
	}
	case CatalogType::INDEX_ENTRY: {
		ReadIndex(transaction, deserializer);
		break;
	}
	case CatalogType::TRIGGER_ENTRY: {
		ReadTrigger(transaction, deserializer);
		break;
	}
	default:
		throw DataCorruptionException("corrupt database file - unrecognized catalog type in checkpoint");
	}
}

void CheckpointReader::ReadSchema(CatalogTransaction transaction, Deserializer &deserializer) {
	// Read the schema and create it in the catalog
	auto info = ReadCreateInfo(deserializer, CatalogType::SCHEMA_ENTRY, "schema");
	auto &schema_info = info->Cast<CreateSchemaInfo>();

	// we set create conflict to IGNORE_ON_CONFLICT, so that we can ignore a failure when recreating the main schema
	schema_info.on_conflict = OnCreateConflict::IGNORE_ON_CONFLICT;
	catalog.CreateSchema(transaction, schema_info);
}

//===--------------------------------------------------------------------===//
// Views
//===--------------------------------------------------------------------===//
void CheckpointWriter::WriteView(ViewCatalogEntry &view, Serializer &serializer) {
	serializer.WriteProperty(100, "view", &view);
}

void CheckpointReader::ReadView(CatalogTransaction transaction, Deserializer &deserializer) {
	auto info = ReadCreateInfo(deserializer, CatalogType::VIEW_ENTRY, "view");
	auto &view_info = info->Cast<CreateViewInfo>();
	catalog.CreateView(transaction, view_info);
}

//===--------------------------------------------------------------------===//
// Triggers
//===--------------------------------------------------------------------===//
void CheckpointWriter::WriteTrigger(TriggerCatalogEntry &trigger, Serializer &serializer) {
	serializer.WriteProperty(100, "trigger", &trigger);
}

void CheckpointReader::ReadTrigger(CatalogTransaction transaction, Deserializer &deserializer) {
	auto info = ReadCreateInfo(deserializer, CatalogType::TRIGGER_ENTRY, "trigger");
	auto &trigger_info = info->Cast<CreateTriggerInfo>();
	trigger_info.on_conflict = OnCreateConflict::IGNORE_ON_CONFLICT;
	auto &schema = catalog.GetEntrySchema(transaction, trigger_info.GetQualifiedName());
	auto table_entry = schema.GetEntry(transaction, CatalogType::TABLE_ENTRY, trigger_info.base_table->Table());
	if (!table_entry) {
		throw DataCorruptionException("corrupt database file - trigger entry without table entry");
	}
	auto &duck_table = table_entry->Cast<DuckTableEntry>();
	duck_table.CreateTrigger(transaction, trigger_info);
}

//===--------------------------------------------------------------------===//
// Sequences
//===--------------------------------------------------------------------===//
void CheckpointWriter::WriteSequence(SequenceCatalogEntry &seq, Serializer &serializer) {
	auto info = seq.GetInfo();
	serializer.WriteProperty(100, "sequence", info.get());
}

void CheckpointReader::ReadSequence(CatalogTransaction transaction, Deserializer &deserializer) {
	auto info = ReadCreateInfo(deserializer, CatalogType::SEQUENCE_ENTRY, "sequence");
	auto &sequence_info = info->Cast<CreateSequenceInfo>();
	auto entry = catalog.CreateSequence(transaction, sequence_info);
	auto &storage_manager = catalog.GetAttached().GetStorageManager();
	if (entry && storage_manager.HasStorageVersion() &&
	    StorageManager::IsPriorToVersion(StorageVersion::V2_0_0, storage_manager.GetStorageVersion())) {
		entry->Cast<SequenceCatalogEntry>().DeriveLastValue();
	}
}

//===--------------------------------------------------------------------===//
// Indexes
//===--------------------------------------------------------------------===//
void CheckpointWriter::WriteIndex(IndexCatalogEntry &index_catalog_entry, Serializer &serializer) {
	// The index data is written as part of WriteTableData
	// Here, we serialize the index catalog entry

	// we need to keep the tag "index", even though it is slightly misleading
	serializer.WriteProperty(100, "index", &index_catalog_entry);
}

void CheckpointReader::ReadIndex(CatalogTransaction transaction, Deserializer &deserializer) {
	// we need to keep the tag "index", even though it is slightly misleading.
	auto create_info = ReadCreateInfo(deserializer, CatalogType::INDEX_ENTRY, "index");
	auto &info = create_info->Cast<CreateIndexInfo>();

	// also, we have to read the root_block_pointer, which will not be valid for newer storage versions.
	// This leads to different code paths in this function.
	auto root_block_pointer =
	    deserializer.ReadPropertyWithExplicitDefault<BlockPointer>(101, "root_block_pointer", BlockPointer());

	// create the index in the catalog

	// look for the table in the catalog
	auto &schema = catalog.GetEntrySchema(transaction, create_info->GetQualifiedName());
	auto catalog_table = schema.GetEntry(transaction, CatalogType::TABLE_ENTRY, info.table);
	if (!catalog_table) {
		// See internal issue 3663.
		throw DataCorruptionException("corrupt database file - index entry without table entry");
	}
	auto &table = catalog_table->Cast<DuckTableEntry>();

	// we also need to make sure the index type is loaded
	// backwards compatibility:
	// if the index type is not specified, we default to ART
	if (info.index_type.empty()) {
		info.index_type = ART::TYPE_NAME;
	}

	// now we can look for the index in the catalog and assign the table info
	auto &index = schema.CreateIndex(transaction, info, table)->Cast<DuckIndexEntry>();
	auto &data_table = table.GetStorage();
	auto &table_info = data_table.GetDataTableInfo();

	IndexStorageInfo index_storage_info;
	if (root_block_pointer.IsValid()) {
		// Read older duckdb files.
		index_storage_info.name = index.name;
		index_storage_info.root_block_ptr = root_block_pointer;
	} else {
		// Extract the matching index storage info (moves it out of the stored collection).
		index_storage_info = table_info->ExtractIndexStorageInfo(index.name);
	}

	D_ASSERT(index_storage_info.IsValid());
	D_ASSERT(!index_storage_info.name.empty());

	// Create an unbound index and add it to the table.
	auto unbound_index = make_uniq<UnboundIndex>(std::move(create_info), std::move(index_storage_info),
	                                             TableIOManager::Get(data_table), data_table.db);
	table_info->GetIndexes().AddIndex(std::move(unbound_index), index.oid, ConstraintCheckMode::DEFAULT);
}

//===--------------------------------------------------------------------===//
// Custom Types
//===--------------------------------------------------------------------===//
void CheckpointWriter::WriteType(TypeCatalogEntry &type, Serializer &serializer) {
	serializer.WriteProperty(100, "type", &type);
}

void CheckpointReader::ReadType(CatalogTransaction transaction, Deserializer &deserializer) {
	auto info = ReadCreateInfo(deserializer, CatalogType::TYPE_ENTRY, "type");
	auto &type_info = info->Cast<CreateTypeInfo>();
	catalog.CreateType(transaction, type_info);
}

//===--------------------------------------------------------------------===//
// Macro's
//===--------------------------------------------------------------------===//
void CheckpointWriter::WriteMacro(ScalarMacroCatalogEntry &macro, Serializer &serializer) {
	serializer.WriteProperty(100, "macro", &macro);
}

void CheckpointReader::ReadMacro(CatalogTransaction transaction, Deserializer &deserializer) {
	auto info = ReadCreateInfo(deserializer, CatalogType::MACRO_ENTRY, "macro");
	auto &macro_info = info->Cast<CreateMacroInfo>();
	catalog.CreateFunction(transaction, macro_info);
}

void CheckpointWriter::WriteTableMacro(TableMacroCatalogEntry &macro, Serializer &serializer) {
	serializer.WriteProperty(100, "table_macro", &macro);
}

void CheckpointReader::ReadTableMacro(CatalogTransaction transaction, Deserializer &deserializer) {
	auto info = ReadCreateInfo(deserializer, CatalogType::TABLE_MACRO_ENTRY, "table_macro");
	auto &macro_info = info->Cast<CreateMacroInfo>();
	catalog.CreateFunction(transaction, macro_info);
}

//===--------------------------------------------------------------------===//
// Table Metadata
//===--------------------------------------------------------------------===//
void SingleFileCheckpointWriter::WriteTable(TableCatalogEntry &table, Serializer &serializer) {
	// Write the table metadata
	serializer.WriteProperty(100, "table", &table);

	// Explicit checkpoints bind indexes before serialization, so that buffered index operations are replayed
	// and not lost on a restart. During a commit-time checkpoint the caller has no active transaction.
	if (context && context->transaction.HasActiveTransaction() && checkpoint_context) {
		D_ASSERT(checkpoint_context->transaction.HasActiveTransaction());
		// Bind indexes with checkpoint transaction, which is already running and read-only, so any transaction the
		// binder still starts skips start_transaction_lock.
		D_ASSERT(MetaTransaction::Get(*checkpoint_context).IsReadOnly());
		auto &info = table.GetStorage().GetDataTableInfo();
		info->BindIndexes(*checkpoint_context);
	}
	// FIXME: If we do not have a context, however, the unbound indexes have to be serialized to disk.

	// Write the table data
	auto writer = GetTableDataWriter(table);
	if (writer) {
		writer->WriteTableData(serializer);
	}
}

void CheckpointReader::ReadTable(CatalogTransaction transaction, Deserializer &deserializer) {
	// deserialize the table meta data
	auto info = ReadCreateInfo(deserializer, CatalogType::TABLE_ENTRY, "table");
	// the qualified name is [catalog, schema_path..., name] - navigate the (possibly nested) schema path
	auto &path = info->GetQualifiedName().Path();
	vector<Identifier> schema_path(path.begin() + 1, path.end() - 1);
	auto &schema = *catalog.GetSchema(transaction, schema_path, OnEntryNotFound::THROW_EXCEPTION);
	auto bound_info = Binder::BindCreateTableCheckpoint(std::move(info), schema);

	// now read the actual table data and place it into the CreateTableInfo
	ReadTableData(transaction, deserializer, *bound_info);

	// finally create the table in the catalog
	catalog.CreateTable(transaction, *bound_info);
}

void CheckpointReader::ReadTableData(CatalogTransaction transaction, Deserializer &deserializer,
                                     BoundCreateTableInfo &bound_info) {
	// written in "SingleFileTableDataWriter::FinalizeTable"
	auto table_pointer = deserializer.ReadProperty<MetaBlockPointer>(101, "table_pointer");
	auto total_rows = deserializer.ReadProperty<idx_t>(102, "total_rows");

	// Cover reading old storage files.
	auto index_pointers = deserializer.ReadPropertyWithExplicitDefault<vector<BlockPointer>>(103, "index_pointers", {});
	// Cover reading new storage files.
	auto index_storage_infos =
	    deserializer.ReadPropertyWithExplicitDefault<vector<IndexStorageInfo>>(104, "index_storage_infos", {});
	// Read next_row_id as total_rows for backwards compatibility. Older storage versions do not allow for gaps in
	// row_id numbering, in which case next_row_id = total_rows.
	auto next_row_id = deserializer.ReadPropertyWithExplicitDefault<idx_t>(105, "next_row_id", total_rows);
	D_ASSERT(next_row_id >= total_rows);

	if (!index_storage_infos.empty()) {
		bound_info.indexes = std::move(index_storage_infos);

	} else {
		// This is an old duckdb file containing index pointers and deprecated storage.
		for (idx_t i = 0; i < index_pointers.size(); i++) {
			// Deprecated storage is always true for old duckdb files.
			IndexStorageInfo index_storage_info;
			index_storage_info.root_block_ptr = index_pointers[i];
			bound_info.indexes.push_back(std::move(index_storage_info));
		}
	}

	// FIXME: icky downcast to get the underlying MetadataReader
	DynamicCastCheck<BinaryDeserializer>(&deserializer);
	auto &binary_deserializer = static_cast<BinaryDeserializer &>(deserializer);
	DynamicCastCheck<MetadataReader>(&binary_deserializer.GetStream());
	auto &reader = static_cast<MetadataReader &>(binary_deserializer.GetStream());

	vector<MetaBlockPointer> read_pointers;
	MetadataReader table_data_reader(reader.GetMetadataManager(), table_pointer, read_pointers);
	TableDataReader data_reader(table_data_reader, bound_info, table_pointer);
	data_reader.ReadTableData();

	bound_info.data->total_rows = total_rows;
	bound_info.data->next_row_id = next_row_id;
	bound_info.data->read_metadata_pointers = read_pointers;
}

} // namespace duckdb
