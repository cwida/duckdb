#include "duckdb/storage/optimistic_data_writer.hpp"

#include "duckdb/main/settings.hpp"
#include "duckdb/storage/partial_block_manager.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/storage/table_io_manager.hpp"
#include "duckdb/storage/table/column_checkpoint_state.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/config.hpp"

namespace duckdb {

OptimisticWriteCollection::~OptimisticWriteCollection() {
}

OptimisticDataWriter::OptimisticDataWriter(ClientContext &context, DataTable &table) : context(context), table(table) {
}

OptimisticDataWriter::OptimisticDataWriter(DataTable &table, OptimisticDataWriter &parent)
    : context(parent.GetClientContext()), table(table) {
	if (parent.partial_manager) {
		parent.partial_manager->ClearBlocks();
	}
}

OptimisticDataWriter::~OptimisticDataWriter() {
}

bool OptimisticDataWriter::CanWriteToDisk() const {
	auto &attached = table.GetAttached();
	auto &storage_manager = StorageManager::Get(attached);
	return !table.IsTemporary() && !storage_manager.InMemory() && !attached.IsReadOnly();
}

bool OptimisticDataWriter::PrepareWrite() {
	// check if optimistic writing is enabled
	if (!Settings::Get<EnableOptimisticWriteSetting>(context)) {
		return false;
	}
	// check if we should pre-emptively write the table to disk
	if (!CanWriteToDisk()) {
		return false;
	}
	// we should! write the second-to-last row group to disk
	// allocate the partial block-manager if none is allocated yet
	if (!partial_manager) {
		auto &block_manager = table.GetTableIOManager().GetBlockManagerForRowData();
		partial_manager = make_uniq<PartialBlockManager>(context, block_manager, PartialBlockType::APPEND_TO_TABLE);
	}
	return true;
}

unique_ptr<OptimisticWriteCollection> OptimisticDataWriter::CreateCollection(DataTable &storage,
                                                                             const vector<LogicalType> &insert_types,
                                                                             OptimisticWritePartialManagers type) {
	auto table_info = storage.GetDataTableInfo();
	auto &io_manager = TableIOManager::Get(storage);

	// Create the local row group collection.
	auto max_row_id = NumericCast<idx_t>(MAX_ROW_ID);
	auto row_groups = make_shared_ptr<RowGroupCollection>(std::move(table_info), io_manager, insert_types, max_row_id);

	auto result = make_uniq<OptimisticWriteCollection>();
	result->collection = std::move(row_groups);
	if (type == OptimisticWritePartialManagers::PER_COLUMN) {
		for (idx_t i = 0; i < insert_types.size(); i++) {
			auto &block_manager = table.GetTableIOManager().GetBlockManagerForRowData();
			result->partial_block_managers.push_back(make_uniq<PartialBlockManager>(
			    QueryContext(context), block_manager, PartialBlockType::APPEND_TO_TABLE));
		}
	}
	return result;
}

void OptimisticDataWriter::WriteNewRowGroup(OptimisticWriteCollection &row_groups, idx_t flushed_row_group_idx) {
	// we finished writing a complete row group
	if (!PrepareWrite()) {
		return;
	}

	row_groups.unflushed_row_groups.insert(flushed_row_group_idx);
	auto allocated_size = row_groups.collection->GetAllocationSize();
	if (row_groups.prev_allocated_size > allocated_size) {
		throw InternalException("Row group prev allocated size is larger than currently allocated size");
	}
	row_groups.unflushed_data_size += allocated_size - row_groups.prev_allocated_size;
	row_groups.prev_allocated_size = allocated_size;
	auto unflushed_row_groups = row_groups.unflushed_row_groups.size();
	// check if we should flush the row groups
	// first check the amount of row groups
	bool need_to_flush = unflushed_row_groups >= Settings::Get<WriteBufferRowGroupCountSetting>(context);
	if (!need_to_flush) {
		// we don't need to flush based on the amount of row groups - but we still might need to flush based on the
		// amount of
		auto &config = DBConfig::GetConfig(context);
		auto memory_limit = config.GetWriteBufferMemoryLimit();
		if (row_groups.unflushed_data_size >= memory_limit) {
			// we exhausted our memory available for buffering - flush
			need_to_flush = true;
		}
	}
	if (need_to_flush) {
		// we have crossed our flush threshold - flush any unwritten row groups to disk
		vector<const_reference<RowGroup>> to_flush;
		vector<int64_t> segment_indexes;
		for (auto &unflushed_idx : row_groups.unflushed_row_groups) {
			auto segment_index = NumericCast<int64_t>(unflushed_idx);
			to_flush.push_back(*row_groups.collection->GetRowGroup(segment_index));
			segment_indexes.push_back(segment_index);
		}
		FlushToDisk(row_groups, to_flush, segment_indexes);
		row_groups.FinalizeFlush();
	}
}

void OptimisticWriteCollection::FinalizeFlush() {
	flushed_row_groups.insert(unflushed_row_groups.begin(), unflushed_row_groups.end());
	unflushed_row_groups.clear();
	unflushed_data_size = 0;
}

void OptimisticWriteCollection::ResetCollectionAccounting() {
	prev_allocated_size = 0;
	unflushed_data_size = 0;
	unflushed_row_groups.clear();
	flushed_row_groups.clear();
}

void OptimisticDataWriter::WriteUnflushedRowGroups(OptimisticWriteCollection &row_groups) {
	auto total_row_groups = row_groups.collection->GetRowGroupCount();
	if (row_groups.flushed_row_groups.size() == total_row_groups && row_groups.partial_block_managers.empty()) {
		// everything is flushed already and there is nothing to merge - a repeated call ends up here
		return;
	}
	// we finished writing a complete row group
	if (!PrepareWrite()) {
		return;
	}
	// add any incomplete row groups to the set of unflushed row groups
	for (idx_t i = 0; i < total_row_groups; i++) {
		// check if this row group was flushed
		auto entry = row_groups.flushed_row_groups.find(i);
		if (entry == row_groups.flushed_row_groups.end()) {
			row_groups.unflushed_row_groups.insert(i);
		}
	}
	if (!row_groups.unflushed_row_groups.empty()) {
		// flush the last batch of row groups
		vector<const_reference<RowGroup>> to_flush;
		vector<int64_t> segment_indexes;
		for (auto &unflushed_idx : row_groups.unflushed_row_groups) {
			auto segment_index = NumericCast<int64_t>(unflushed_idx);
			to_flush.push_back(*row_groups.collection->GetRowGroup(segment_index));
			segment_indexes.push_back(segment_index);
		}

		FlushToDisk(row_groups, to_flush, segment_indexes);
	}

	for (auto &partial_manager : row_groups.partial_block_managers) {
		Merge(partial_manager);
	}
	row_groups.FinalizeFlush();
	row_groups.partial_block_managers.clear();
	// any new append to the row group collection needs to append a new row group
	// otherwise we append to an already flushed row group
	row_groups.collection->SetRowGroupAppendMode(RowGroupAppendMode::REQUIRE_NEW);
}

void OptimisticWriteCollection::MergeStorage(OptimisticWriteCollection &merge_collection) {
	auto &merge_row_groups = *merge_collection.collection;
	if (merge_row_groups.GetTotalRows() == 0) {
		// no rows to merge - done
		return;
	}
	idx_t current_row_group_count = collection->GetRowGroupCount();
	// now we merge the target collection into this one - take over any unflushed row groups but adjust their index
	for (auto &unflushed_idx : merge_collection.unflushed_row_groups) {
		unflushed_row_groups.insert(current_row_group_count + unflushed_idx);
	}
	for (auto &flushed_idx : merge_collection.flushed_row_groups) {
		flushed_row_groups.insert(current_row_group_count + flushed_idx);
	}
	unflushed_data_size += merge_collection.unflushed_data_size;
	// finally perform the actual merge
	collection->MergeStorage(merge_row_groups, nullptr, nullptr);
}

void OptimisticDataWriter::FlushToDisk(OptimisticWriteCollection &collection,
                                       const vector<const_reference<RowGroup>> &row_groups,
                                       const vector<int64_t> &segment_indexes) {
	//! The set of column compression types (if any)
	vector<CompressionType> compression_types;
	D_ASSERT(compression_types.empty());
	for (auto &column : table.Columns()) {
		compression_types.push_back(column.CompressionType());
	}
	RowGroupWriteInfo info(*partial_manager, compression_types, collection.partial_block_managers);
	auto result = RowGroup::WriteToDisk(info, row_groups);
	// move new (checkpointed) row groups to the row group collection
	for (idx_t i = 0; i < row_groups.size(); i++) {
		collection.collection->SetRowGroup(segment_indexes[i], std::move(result[i].result_row_group));
	}
}

void OptimisticDataWriter::Merge(unique_ptr<PartialBlockManager> &other_manager) {
	if (!other_manager) {
		return;
	}
	if (!partial_manager) {
		partial_manager = std::move(other_manager);
		return;
	}
	partial_manager->Merge(*other_manager);
	other_manager.reset();
}

void OptimisticDataWriter::Merge(OptimisticDataWriter &other) {
	Merge(other.partial_manager);
}

void OptimisticDataWriter::FinalFlush() {
	if (partial_manager) {
		partial_manager->FlushPartialBlocks();
		partial_manager.reset();
	}
}

void OptimisticDataWriter::Rollback() {
	if (partial_manager) {
		partial_manager->ClearBlocks();
		partial_manager.reset();
	}
}

} // namespace duckdb
