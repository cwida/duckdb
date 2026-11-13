#include "duckdb/storage/table/in_memory_checkpoint.hpp"
#include "duckdb/main/attached_database.hpp"

#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/common/enums/checkpoint_abort.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/column_segment.hpp"

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
	vector<reference<SchemaCatalogEntry>> schemas;
	// we scan the set of committed schemas
	auto &catalog = Catalog::GetCatalog(db).Cast<DuckCatalog>();
	catalog.ScanSchemas([&](SchemaCatalogEntry &entry) { schemas.push_back(entry); });

	vector<reference<TableCatalogEntry>> tables;
	for (const auto &schema_ref : schemas) {
		auto &schema = schema_ref.get();
		schema.Scan(CatalogType::TABLE_ENTRY, [&](CatalogEntry &entry) {
			if (entry.type == CatalogType::TABLE_ENTRY) {
				auto &table = entry.Cast<TableCatalogEntry>();
				if (table.IsDuckTable()) {
					tables.push_back(table);
				}
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
	storage_manager.SetWALSize(0);
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
	auto table_lock = table.GetStorage().GetCheckpointLock();
	table.GetStorage().Checkpoint(data_writer, serializer);
	// flush any partial blocks BEFORE releasing the table lock
	// flushing partial blocks updates where data lives and is not thread-safe
	partial_block_manager.FlushPartialBlocks();
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
                                            RowGroupCollection &collection, Serializer &serializer) {
	// nop: no need to write anything
}

unique_ptr<RowGroupWriter> InMemoryTableDataWriter::GetRowGroupWriter(RowGroup &row_group) {
	return make_uniq<InMemoryRowGroupWriter>(*this, table, checkpoint_manager.GetPartialBlockManager(),
	                                         checkpoint_manager);
}

void InMemoryTableDataWriter::FlushPartialBlocks() {
}

CheckpointOptions InMemoryTableDataWriter::GetCheckpointOptions() const {
	return checkpoint_manager.GetCheckpointOptions();
}

MetadataManager &InMemoryTableDataWriter::GetMetadataManager() {
	return checkpoint_manager.GetMetadataManager();
}

InMemoryPartialBlock::InMemoryPartialBlock(ColumnData &data, ColumnSegment &segment, PartialBlockState state,
                                           BlockManager &block_manager)
    : PartialBlock(state, block_manager, segment.GetBlockHandle()) {
	InMemoryPartialBlock::AddSegmentToTail(data, segment, 0);
}

InMemoryPartialBlock::~InMemoryPartialBlock() {
}

void InMemoryPartialBlock::Flush(QueryContext context, const idx_t free_space_left) {
	Clear();
}

void InMemoryPartialBlock::Merge(PartialBlock &other_p, idx_t offset, idx_t other_size) {
	auto &other = other_p.Cast<InMemoryPartialBlock>();
	other.Clear();
}

void InMemoryPartialBlock::AddSegmentToTail(ColumnData &data, ColumnSegment &segment, uint32_t offset_in_block) {
	segment.SetBlock(block_handle, offset_in_block);
}

void InMemoryPartialBlock::Clear() {
	uninitialized_regions.clear();
	block_handle.reset();
}

} // namespace duckdb
