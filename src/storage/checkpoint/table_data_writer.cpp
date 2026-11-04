#include "duckdb/storage/checkpoint/table_data_writer.hpp"
#include "duckdb/common/shared_ptr_ipp.hpp"
#include "duckdb/storage/checkpoint/table_index_writer.hpp"

#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/storage/checkpoint/table_data_reader.hpp"
#include "duckdb/storage/index_storage_info.hpp"
#include "duckdb/storage/metadata/metadata_reader.hpp"
#include "duckdb/storage/table/table_index_list.hpp"
#include "duckdb/storage/table/table_statistics.hpp"
#include "duckdb/storage/data_table.hpp"

namespace duckdb {

TableDataWriter::TableDataWriter(TableCatalogEntry &table_p, QueryContext context)
    : table(table_p.Cast<DuckTableEntry>()), context(context.GetClientContext()) {
	D_ASSERT(table_p.IsDuckTable());

	auto storage_compatibility = StorageCompatibility::FromDatabase(table_p.ParentCatalog().GetAttached());
	if (storage_compatibility.storage_version < StorageVersion::V1_4_4) {
		// older storage versions require legacy start row to be written
		require_legacy_start_row = true;
	}
	can_persist_rowid_gaps = storage_compatibility.CanPersistRowIdGaps();
}

TableDataWriter::~TableDataWriter() {
}

void TableDataWriter::WriteTableData(Serializer &metadata_serializer) {
	// start scanning the table and append the data to the uncompressed segments
	table.GetStorage().Checkpoint(*this, metadata_serializer);
}

void TableDataWriter::AddRowGroup(RowGroupPointer &&row_group_pointer, unique_ptr<RowGroupWriter> writer) {
	row_group_pointers.push_back(std::move(row_group_pointer));
}

AttachedDatabase &TableDataWriter::GetAttached() {
	return table.ParentCatalog().GetAttached();
}

DatabaseInstance &TableDataWriter::GetDatabase() {
	return table.ParentCatalog().GetDatabase();
}

unique_ptr<TaskExecutor> TableDataWriter::CreateTaskExecutor() {
	if (context) {
		return make_uniq<TaskExecutor>(*context);
	}
	return make_uniq<TaskExecutor>(TaskScheduler::GetScheduler(GetDatabase()));
}

optional_ptr<ClientContext> TableDataWriter::TryGetClientContext() const {
	return context;
}

SingleFileTableDataWriter::SingleFileTableDataWriter(SingleFileCheckpointWriter &checkpoint_manager,
                                                     TableCatalogEntry &table, MetadataWriter &table_data_writer)
    : TableDataWriter(table, checkpoint_manager.GetClientContext()), checkpoint_manager(checkpoint_manager),
      table_data_writer(table_data_writer) {
}

unique_ptr<RowGroupWriter> SingleFileTableDataWriter::GetRowGroupWriter(RowGroup &row_group) {
	return make_uniq<SingleFileRowGroupWriter>(table, checkpoint_manager.partial_block_manager, *this,
	                                           table_data_writer);
}

CheckpointOptions SingleFileTableDataWriter::GetCheckpointOptions() const {
	return checkpoint_manager.GetCheckpointOptions();
}

MetadataManager &SingleFileTableDataWriter::GetMetadataManager() {
	return checkpoint_manager.GetMetadataManager();
}

void SingleFileTableDataWriter::WriteUnchangedTable(MetaBlockPointer pointer,
                                                    const vector<MetaBlockPointer> &metadata_pointers, idx_t total_rows,
                                                    idx_t next_row_id) {
	existing_pointer = pointer;
	existing_pointers = metadata_pointers;
	existing_rows = total_rows;
	existing_next_row_id = next_row_id;
}

unique_ptr<TableIndexWriter> SingleFileTableDataWriter::GetTableIndexWriter(StorageVersion version) {
	return make_uniq<SingleFileTableIndexWriter>(checkpoint_manager, version);
}

void SingleFileTableDataWriter::FlushPartialBlocks() {
	checkpoint_manager.partial_block_manager.FlushPartialBlocks();
}

void SingleFileTableDataWriter::FinalizeTable(const TableStatistics &global_stats, DataTableInfo &info,
                                              RowGroupCollection &collection,
                                              const vector<shared_ptr<const IndexStorageInfo>> &index_infos,
                                              Serializer &serializer) {
	MetaBlockPointer pointer;
	idx_t total_rows;
	idx_t next_row_id;
	auto debug_verify_blocks = Settings::Get<DebugVerifyBlocksSetting>(GetDatabase());
	if (!existing_pointer.IsValid()) {
		auto supports_per_column_writes = collection.SupportsPerColumnWrites();
		// write the metadata
		// store the current position in the metadata writer
		// this is where the row groups for this table start
		pointer = table_data_writer.GetMetaBlockPointer();
		vector<MetaBlockPointer> written_pointers;
		table_data_writer.SetWrittenPointers(written_pointers);

		// Serialize statistics as a single unit
		BinarySerializer stats_serializer(table_data_writer, serializer.GetOptions());
		stats_serializer.Begin();
		global_stats.Serialize(stats_serializer);
		stats_serializer.End();

		// now start writing the row group pointers to disk
		table_data_writer.Write<uint64_t>(row_group_pointers.size());
		total_rows = 0;
		next_row_id = 0;
		for (auto &row_group_pointer : row_group_pointers) {
			total_rows += row_group_pointer.tuple_count;
			auto row_group_end = row_group_pointer.row_start + row_group_pointer.tuple_count;
			if (row_group_end > next_row_id) {
				next_row_id = row_group_end;
			}

			// Each RowGroup is its own unit
			BinarySerializer row_group_serializer(table_data_writer, serializer.GetOptions());
			row_group_serializer.Begin();
			RowGroup::Serialize(row_group_pointer, row_group_serializer, supports_per_column_writes);
			row_group_serializer.End();
		}
		table_data_writer.SetWrittenPointers(nullptr);
		collection.FinalizeCheckpoint(pointer, written_pointers);
	} else {
		// we have existing metadata and the table is unchanged - write a pointer to the existing metadata
		pointer = existing_pointer;
		total_rows = existing_rows.GetIndex();
		next_row_id = existing_next_row_id.GetIndex();
		D_ASSERT(next_row_id >= total_rows);

		// label the blocks as used again to prevent them from being freed
		auto &metadata_manager = checkpoint_manager.GetMetadataManager();
		metadata_manager.ClearModifiedBlocks(existing_pointers);

		// verify that existing_pointers indeed corresponds to the metadata blocks
		if (debug_verify_blocks) {
			vector<MetaBlockPointer> read_pointers;
			MetadataReader reader(metadata_manager, pointer, read_pointers);
			auto bound_info = Binder::BindCreateTableCheckpoint(table.GetInfo(), table.schema);
			TableDataReader data_reader(reader, *bound_info, pointer);
			data_reader.ReadTableData();
			for (idx_t row_group = 0; row_group < bound_info->data->row_group_count; ++row_group) {
				BinaryDeserializer deserializer(reader);
				deserializer.Begin();
				auto row_group_pointer = RowGroup::Deserialize(deserializer);
				deserializer.End();
			}
			set<idx_t> existing_block_ids;
			for (auto &ptr : existing_pointers) {
				existing_block_ids.insert(ptr.block_pointer);
			}
			set<idx_t> all_read_block_ids;
			for (auto &ptr : read_pointers) {
				all_read_block_ids.insert(ptr.block_pointer);
			}
			if (existing_block_ids != all_read_block_ids) {
				std::stringstream oss;
				oss << "Existing: ";
				for (auto &block : existing_pointers) {
					oss << block << ", ";
				}
				oss << "\n";
				oss << "Read: ";
				for (auto &block : read_pointers) {
					oss << block << ", ";
				}
				oss << "\n";
				throw InternalException("Reading existing blocks does not yield same blocks: " + oss.str());
			}
		}
	}

	D_ASSERT(next_row_id >= total_rows);
	// Now begin the metadata as a unit
	// Pointer to the table itself goes to the metadata stream.
	serializer.WriteProperty(101, "table_pointer", pointer);
	serializer.WriteProperty(102, "total_rows", total_rows);

	if (debug_verify_blocks) {
		for (const auto &index_info : index_infos) {
			for (const auto &allocator : index_info->allocator_infos) {
				for (const auto &block : allocator.block_pointers) {
					checkpoint_manager.verify_block_usage_count[block.block_id]++;
				}
			}
		}
	}

	// write empty block pointers for forwards compatibility
	const vector<BlockPointer> compat_block_pointers;
	serializer.WriteProperty(103, "index_pointers", compat_block_pointers);
	serializer.WriteList(104, "index_storage_infos", index_infos.size(),
	                     [&](Serializer::List &list, idx_t i) { list.WriteElement(*index_infos[i]); });

	if (serializer.ShouldSerialize(StorageVersion::V2_0_0)) {
		serializer.WriteProperty(105, "next_row_id", next_row_id);
	}
	// ¬serializer.ShouldSerialize(StorageVersion::V2_0_0) ==> (next_row_id == total_rows)
	D_ASSERT(serializer.ShouldSerialize(StorageVersion::V2_0_0) || (next_row_id == total_rows));
}

} // namespace duckdb
