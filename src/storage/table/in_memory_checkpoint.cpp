#include "duckdb/storage/table/in_memory_checkpoint.hpp"
#include "duckdb/main/attached_database.hpp"

#include "duckdb/catalog/catalog_entry/duck_schema_entry.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/common/enums/checkpoint_abort.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/partial_block_manager.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/storage/table/column_segment.hpp"
#include "duckdb/storage/table/table_index_list.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// In-Memory Checkpoint Writer
//===--------------------------------------------------------------------===//
InMemoryCheckpointer::InMemoryCheckpointer(QueryContext context, AttachedDatabase &db, BlockManager &block_manager,
                                           StorageManager &storage_manager, CheckpointOptions options_p)
    : CheckpointWriter(db), context(context.GetClientContext()),
      partial_block_manager(context, block_manager, PartialBlockType::IN_MEMORY_CHECKPOINT),
      storage_manager(storage_manager), options(options_p) {
}

void InMemoryCheckpointer::CreateCheckpoint() {
	auto &transaction_manager = DuckTransactionManager::Get(db);
	// appends that run alongside the checkpoint start new row groups, index changes go to the checkpoint deltas
	ActiveCheckpointWrapper active_checkpoint(context, db, transaction_manager);
	{
		unique_lock<mutex> commit_lock;
		if (!options.commit_lock) {
			// a checkpoint instead of a WAL write already holds the commit lock
			commit_lock = storage_manager.GetCommitLock();
		}
		if (options.type == CheckpointType::FULL_CHECKPOINT &&
		    transaction_manager.GetLastCommit() >= transaction_manager.LowestVisibilityBound()) {
			// an active snapshot still needs state a full checkpoint would vacuum away
			options.type = CheckpointType::CONCURRENT_CHECKPOINT;
		}
		active_checkpoint.Begin(options);
	}

	vector<reference<DuckSchemaEntry>> schemas;
	// the schemas and tables committed before the checkpoint's bound
	auto &catalog = Catalog::GetCatalog(db).Cast<DuckCatalog>();
	catalog.ScanSchemas(options.visibility_bound, [&](DuckSchemaEntry &entry) { schemas.push_back(entry); });

	vector<reference<TableCatalogEntry>> tables;
	for (const auto &schema_ref : schemas) {
		auto &schema = schema_ref.get();
		schema.Scan(CatalogType::TABLE_ENTRY, options.visibility_bound, [&](CatalogEntry &entry) {
			if (entry.type == CatalogType::TABLE_ENTRY) {
				tables.push_back(entry.Cast<TableCatalogEntry>());
			}
		});
	}

	auto debug_checkpoint_abort = Settings::Get<DebugCheckpointAbortSetting>(db.GetDatabase());
	for (auto &table : tables) {
		MemoryStream write_stream;
		BinarySerializer serializer(write_stream);

		WriteTable(table, serializer);
		if (debug_checkpoint_abort == CheckpointAbort::DEBUG_ABORT_IN_MEMORY_CHECKPOINT) {
			throw IOException("In-memory checkpoint aborted because of PRAGMA debug_checkpoint_abort flag");
		}
	}
	for (auto &table : tables) {
		// index entries added or removed while the checkpoint ran went to delta indexes
		auto &index_list = table.get().GetStorage().GetDataTableInfo()->GetIndexes();
		index_list.MergeCheckpointDeltas(options.checkpoint_id);
	}
	storage_manager.SetWALSize(0);
	active_checkpoint.Commit();
}

MetadataWriter &InMemoryCheckpointer::GetMetadataWriter() {
	throw InternalException("Unsupported method GetMetadataWriter for InMemoryCheckpointer");
}
MetadataManager &InMemoryCheckpointer::GetMetadataManager() {
	throw InternalException("Unsupported method GetMetadataManager for InMemoryCheckpointer");
}
unique_ptr<TableDataWriter> InMemoryCheckpointer::GetTableDataWriter(TableCatalogEntry &table) {
	throw InternalException("Unsupported method GetTableDataWriter for InMemoryCheckpointer");
}

void InMemoryCheckpointer::WriteTable(TableCatalogEntry &table, Serializer &serializer) {
	InMemoryTableDataWriter data_writer(*this, table);

	// Write the table data
	table.GetStorage().Checkpoint(data_writer, serializer);
}

InMemoryRowGroupWriter::InMemoryRowGroupWriter(TableDataWriter &writer, TableCatalogEntry &table,
                                               PartialBlockManager &partial_block_manager,
                                               InMemoryCheckpointer &checkpoint_manager)
    : RowGroupWriter(writer, table, partial_block_manager), checkpoint_manager(checkpoint_manager) {
}

CheckpointOptions InMemoryRowGroupWriter::GetCheckpointOptions() const {
	return checkpoint_manager.GetCheckpointOptions();
}

WriteStream &InMemoryRowGroupWriter::GetPayloadWriter() {
	return metadata_writer;
}

MetaBlockPointer InMemoryRowGroupWriter::GetMetaBlockPointer() {
	return MetaBlockPointer();
}

optional_ptr<MetadataManager> InMemoryRowGroupWriter::GetMetadataManager() {
	return nullptr;
}

InMemoryTableDataWriter::InMemoryTableDataWriter(InMemoryCheckpointer &checkpoint_manager, TableCatalogEntry &table)
    : TableDataWriter(table, checkpoint_manager.GetClientContext()), checkpoint_manager(checkpoint_manager) {
}

void InMemoryTableDataWriter::WriteUnchangedTable(MetaBlockPointer pointer,
                                                  const vector<MetaBlockPointer> &metadata_pointers, idx_t total_rows,
                                                  idx_t next_row_id) {
}

void InMemoryTableDataWriter::FinalizeTable(const TableStatistics &global_stats, DataTableInfo &info,
                                            RowGroupCollection &collection,
                                            const vector<shared_ptr<const IndexStorageInfo>> &index_infos,
                                            Serializer &serializer) {
	// nop: no need to write anything
}

unique_ptr<RowGroupWriter> InMemoryTableDataWriter::GetRowGroupWriter(RowGroup &row_group) {
	return make_uniq<InMemoryRowGroupWriter>(*this, table, checkpoint_manager.GetPartialBlockManager(),
	                                         checkpoint_manager);
}

unique_ptr<TableIndexWriter> InMemoryTableDataWriter::GetTableIndexWriter(StorageVersion) {
	return nullptr;
};

void InMemoryTableDataWriter::FlushPartialBlocks() {
	// Finalize packed buffers before the checkpoint row groups become visible.
	checkpoint_manager.GetPartialBlockManager().FlushPartialBlocks();
}

CheckpointOptions InMemoryTableDataWriter::GetCheckpointOptions() const {
	return checkpoint_manager.GetCheckpointOptions();
}

MetadataManager &InMemoryTableDataWriter::GetMetadataManager() {
	return checkpoint_manager.GetMetadataManager();
}

} // namespace duckdb
