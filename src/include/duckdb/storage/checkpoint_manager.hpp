//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/checkpoint_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/storage/metadata/metadata_writer.hpp"
#include "duckdb/storage/partial_block_manager.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/storage/checkpoint/table_index_writer.hpp"

namespace duckdb {

// Forward declaration.
class AttachedDatabase;
class BlockManager;
class CatalogEntry;
struct CatalogTransaction;
class ClientContext;
class ColumnSegment;
class DatabaseInstance;
class Deserializer;
class DuckTransaction;
class DuckTransactionManager;
class IndexCatalogEntry;

class MetadataManager;
class MetadataReader;
class SchemaCatalogEntry;
class SequenceCatalogEntry;
class Serializer;
class ScalarMacroCatalogEntry;
class TableMacroCatalogEntry;
class TableCatalogEntry;
class TriggerCatalogEntry;
class ViewCatalogEntry;
class TableDataWriter;
class TypeCatalogEntry;
class PendingPublication;
struct BoundCreateTableInfo;
struct CheckpointOptions;

//! Wrapper to manage the lifetime of a checkpoint transaction.
class ActiveCheckpointWrapper {
public:
	//! Without a checkpoint context, we are on shutdown and a checkpoint transaction is not created.
	ActiveCheckpointWrapper(optional_ptr<ClientContext> checkpoint_context, AttachedDatabase &db,
	                        DuckTransactionManager &transaction_manager);

	~ActiveCheckpointWrapper();

	//! Registers the checkpoint (id, bound, active checkpoint), through a checkpoint transaction if there is a context
	void Begin(CheckpointOptions &options);
	void Commit();

private:
	AttachedDatabase &db;
	DuckTransactionManager &transaction_manager;
	optional_ptr<ClientContext> checkpoint_context;
	optional_ptr<DuckTransaction> checkpoint_transaction;
	//! Whether Begin registered an active checkpoint that Commit has not reset yet
	bool active = false;
};

class CheckpointWriter {
public:
	CheckpointWriter(AttachedDatabase &db, QueryContext context,
	                 optional_ptr<ClientContext> checkpoint_context = nullptr);
	virtual ~CheckpointWriter();

	//! The database
	AttachedDatabase &db;

	//! The services of the query that triggered the checkpoint, or database-wide services if there is none
	Context &GetContext() const {
		return context;
	}
	//! The context the checkpoint transaction runs in (null on shutdown and DETACH)
	optional_ptr<ClientContext> GetCheckpointContext() const {
		return checkpoint_context;
	}

	virtual void CreateCheckpoint() = 0;
	virtual MetadataManager &GetMetadataManager() = 0;
	virtual MetadataWriter &GetMetadataWriter() = 0;
	virtual unique_ptr<TableDataWriter> GetTableDataWriter(TableCatalogEntry &table) = 0;

private:
	shared_ptr<Context> owned_context;
	Context &context;
	optional_ptr<ClientContext> checkpoint_context;

protected:
	virtual void WriteEntry(CatalogEntry &entry, Serializer &serializer);
	virtual void WriteSchema(SchemaCatalogEntry &schema, Serializer &serializer);
	virtual void WriteTable(TableCatalogEntry &table, Serializer &serializer) = 0;
	virtual void WriteView(ViewCatalogEntry &table, Serializer &serializer);
	virtual void WriteSequence(SequenceCatalogEntry &table, Serializer &serializer);
	virtual void WriteMacro(ScalarMacroCatalogEntry &table, Serializer &serializer);
	virtual void WriteTableMacro(TableMacroCatalogEntry &table, Serializer &serializer);
	virtual void WriteIndex(IndexCatalogEntry &index_catalog_entry, Serializer &serializer);
	virtual void WriteType(TypeCatalogEntry &type, Serializer &serializer);
	virtual void WriteTrigger(TriggerCatalogEntry &trigger, Serializer &serializer);
};

class CheckpointReader {
public:
	explicit CheckpointReader(Catalog &catalog) : catalog(catalog) {
	}
	virtual ~CheckpointReader() {
	}

protected:
	Catalog &catalog;

protected:
	virtual void LoadCheckpoint(CatalogTransaction transaction, MetadataReader &reader);
	virtual void ReadEntry(CatalogTransaction transaction, Deserializer &deserializer);
	virtual void ReadSchema(CatalogTransaction transaction, Deserializer &deserializer);
	virtual void ReadTable(CatalogTransaction transaction, Deserializer &deserializer);
	virtual void ReadView(CatalogTransaction transaction, Deserializer &deserializer);
	virtual void ReadSequence(CatalogTransaction transaction, Deserializer &deserializer);
	virtual void ReadMacro(CatalogTransaction transaction, Deserializer &deserializer);
	virtual void ReadTableMacro(CatalogTransaction transaction, Deserializer &deserializer);
	virtual void ReadIndex(CatalogTransaction transaction, Deserializer &deserializer);
	virtual void ReadType(CatalogTransaction transaction, Deserializer &deserializer);
	virtual void ReadTrigger(CatalogTransaction transaction, Deserializer &deserializer);

	virtual void ReadTableData(CatalogTransaction transaction, Deserializer &deserializer,
	                           BoundCreateTableInfo &bound_info);
};

class SingleFileCheckpointReader final : public CheckpointReader {
public:
	explicit SingleFileCheckpointReader(SingleFileStorageManager &storage)
	    : CheckpointReader(Catalog::GetCatalog(storage.GetAttached())), storage(storage) {
	}

	void LoadFromStorage();
	MetadataManager &GetMetadataManager();

	//! The database
	SingleFileStorageManager &storage;
};

class SingleFileCheckpointWriter final : public CheckpointWriter {
	friend class SingleFileTableIndexWriter;
	friend class SingleFileRowGroupWriter;
	friend class SingleFileTableDataWriter;

public:
	SingleFileCheckpointWriter(QueryContext context, AttachedDatabase &db, BlockManager &block_manager,
	                           CheckpointOptions options, optional_ptr<ClientContext> checkpoint_context = nullptr);
	~SingleFileCheckpointWriter() override;

	void CreateCheckpoint() override;

	MetadataWriter &GetMetadataWriter() override;
	MetadataManager &GetMetadataManager() override;
	unique_ptr<TableDataWriter> GetTableDataWriter(TableCatalogEntry &table) override;

	BlockManager &GetBlockManager();
	//! Make a partial block scope for an index that persists immediately.
	PartialBlockManager CreatePerIndexPartialBlockManager();
	CheckpointOptions GetCheckpointOptions() const {
		return options;
	}

public:
	void WriteTable(TableCatalogEntry &table, Serializer &serializer) override;

private:
	//! Retain prepared state until the shared blocks are flushed and publication can run.
	void AddPendingPublication(unique_ptr<PendingPublication> publication);

private:
	//! The metadata writer is responsible for writing schema information
	unique_ptr<MetadataWriter> metadata_writer;
	//! The table data writer is responsible for writing the DataPointers used by the table chunks
	unique_ptr<MetadataWriter> table_metadata_writer;
	//! Because this is single-file storage, we can share partial blocks across
	//! an entire checkpoint.
	PartialBlockManager partial_block_manager;
	//! Checkpoint type
	CheckpointOptions options;
	//! Block usage count for verification purposes
	unordered_map<block_id_t, idx_t> verify_block_usage_count;
	//! Owns prepared state in publication order.
	vector<unique_ptr<PendingPublication>> pending_publications;
};

} // namespace duckdb
