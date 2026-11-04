#include "duckdb/transaction/local_storage.hpp"
#include "duckdb/common/vector/vector_writer.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/transaction/commit_state.hpp"

#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/valid_checker.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/partial_block_manager.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/storage/table/append_state.hpp"
#include "duckdb/storage/table/column_data.hpp"
#include "duckdb/storage/table/data_table_info.hpp"
#include "duckdb/storage/table/row_group.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"

namespace duckdb {

LocalTableStorage::LocalTableStorage(ClientContext &context, DataTable &table)
    : context(context), table_ref(table), allocator(Allocator::Get(table.db)), deleted_rows(0),
      optimistic_writer(context, table) {
	auto types = table.GetTypes();
	auto data_table_info = table.GetDataTableInfo();
	row_groups = optimistic_writer.CreateCollection(table, types, OptimisticWritePartialManagers::GLOBAL);
	auto &collection = *row_groups->collection;
	collection.InitializeEmpty();

	// Create the transaction-local delete and append indexes.
	data_table_info->GetIndexes().InitializeLocalIndexes(delete_indexes, append_indexes);
}

LocalTableStorage::LocalTableStorage(ClientContext &context, DataTable &new_data_table, LocalTableStorage &parent,
                                     const idx_t alter_column_index, const LogicalType &target_type,
                                     const vector<StorageIndex> &bound_columns, Expression &cast_expr,
                                     TransactionData transaction)
    : context(context), table_ref(new_data_table), allocator(Allocator::Get(new_data_table.db)),
      deleted_rows(parent.deleted_rows.load()), optimistic_collections(std::move(parent.optimistic_collections)),
      optimistic_writer(new_data_table, parent.optimistic_writer) {
	// Alter the column type.
	auto &parent_collection = *parent.row_groups->collection;
	auto new_collection =
	    parent_collection.AlterType(context, alter_column_index, target_type, bound_columns, cast_expr, transaction);
	parent_collection.CommitDropColumn(alter_column_index);
	row_groups = std::move(parent.row_groups);
	row_groups->collection = std::move(new_collection);
	row_groups->ResetCollectionAccounting();

	append_indexes.Move(parent.append_indexes);
	delete_indexes.Move(parent.delete_indexes);
}

LocalTableStorage::LocalTableStorage(DataTable &new_data_table, LocalTableStorage &parent,
                                     const idx_t drop_column_index)
    : table_ref(new_data_table), allocator(Allocator::Get(new_data_table.db)), deleted_rows(parent.deleted_rows.load()),
      optimistic_collections(std::move(parent.optimistic_collections)),
      optimistic_writer(new_data_table, parent.optimistic_writer) {
	// Remove the column from the previous table storage.
	auto &parent_collection = *parent.row_groups->collection;
	auto new_collection = parent_collection.RemoveColumn(drop_column_index);
	parent_collection.CommitDropColumn(drop_column_index);
	row_groups = std::move(parent.row_groups);
	row_groups->collection = std::move(new_collection);
	row_groups->ResetCollectionAccounting();

	append_indexes.Move(parent.append_indexes);
	delete_indexes.Move(parent.delete_indexes);
}

LocalTableStorage::LocalTableStorage(ClientContext &context, DataTable &new_dt, LocalTableStorage &parent,
                                     ColumnDefinition &new_column, ExpressionExecutor &default_executor)
    : table_ref(new_dt), allocator(Allocator::Get(new_dt.db)), deleted_rows(parent.deleted_rows.load()),
      optimistic_collections(std::move(parent.optimistic_collections)),
      optimistic_writer(new_dt, parent.optimistic_writer) {
	auto &parent_collection = *parent.row_groups->collection;
	auto new_collection = parent_collection.AddColumn(context, new_column, default_executor);
	row_groups = std::move(parent.row_groups);
	row_groups->collection = std::move(new_collection);
	row_groups->ResetCollectionAccounting();
	append_indexes.Move(parent.append_indexes);
	delete_indexes.Move(parent.delete_indexes);
}

LocalTableStorage::~LocalTableStorage() {
}

void LocalTableStorage::InitializeScan(CollectionScanState &state, optional_ptr<TableFilterSet> table_filters) {
	auto &collection = *row_groups->collection;
	if (collection.GetTotalRows() == 0) {
		throw InternalException("No rows in LocalTableStorage row group for scan");
	}
	collection.InitializeScan(context, state, state.GetColumnIds(), table_filters.get());
}

idx_t LocalTableStorage::EstimatedSize() const {
	// count the appended rows
	auto &collection = *row_groups->collection;
	idx_t data_size = 0;

	if (collection.GetTotalRows() >= collection.GetRowGroupSize() && deleted_rows == 0) {
		// Optimistic insertion does not generate many WAL logs, so we estimate the size of the Data Block Pointers here
		idx_t row_group_count = row_groups->collection->GetRowGroupCount();
		idx_t column_count = collection.GetTypes().size();

		data_size = row_group_count * (sizeof(PersistentRowGroupData) +
		                               column_count * (sizeof(PersistentColumnData) + sizeof(DataPointer)));
	} else {
		idx_t appended_rows = collection.GetTotalRows() - deleted_rows;

		// get the (estimated) size of a row (no compressions, etc.)
		idx_t row_size = 0;
		auto &types = collection.GetTypes();
		for (auto &type : types) {
			row_size += GetTypeIdSize(type.InternalType());
		}

		data_size = appended_rows * row_size;
	}

	// return the size of the appended rows and the index size
	return data_size + append_indexes.GetInMemorySize();
}

void LocalTableStorage::WriteNewRowGroup(idx_t flushed_row_group_idx) {
	if (deleted_rows != 0) {
		// we have deletes - we cannot merge row groups
		return;
	}
	optimistic_writer.WriteNewRowGroup(*row_groups, flushed_row_group_idx);
}

void LocalTableStorage::FlushBlocks() {
	auto &collection = *row_groups->collection;
	const idx_t row_group_size = collection.GetRowGroupSize();
	if (collection.GetTotalRows() >= row_group_size) {
		// write any unflushed row groups
		optimistic_writer.WriteUnflushedRowGroups(*row_groups);
	}
	optimistic_writer.FinalFlush();
}

bool LocalTableStorage::WritesToDisk() const {
	return optimistic_writer.CanWriteToDisk();
}

bool LocalTableStorage::HasFlushedRowGroups() const {
	return !row_groups->flushed_row_groups.empty();
}

bool LocalTableStorage::IsBulkAppend() const {
	if (is_dropped || deleted_rows != 0) {
		return false;
	}
	auto &collection = *row_groups->collection;
	return collection.GetTotalRows() >= collection.GetRowGroupSize();
}

ErrorData LocalTableStorage::AppendToIndexes(DuckTransaction &transaction, RowGroupCollection &source,
                                             TableIndexList &index_list, const vector<LogicalType> &table_types,
                                             row_t &start_row) {
	// We only scan the indexed columns: mapped_column_ids lists them (sorted for a deterministic order),
	// and the scan below produces index_chunk in that order, i.e., index_chunk.data[i] holds the data of
	// the table's physical column mapped_column_ids[i].
	D_ASSERT(!index_list.Empty());
	auto indexed_columns = index_list.GetIndexedColumns();
	vector<StorageIndex> mapped_column_ids;
	for (auto &col : indexed_columns) {
		mapped_column_ids.emplace_back(col);
	}
	std::sort(mapped_column_ids.begin(), mapped_column_ids.end());
	auto checkpoint_id = transaction.GetTransactionManager().Cast<DuckTransactionManager>().GetActiveCheckpoint();

	// The bound expressions of the indexes (and their bound column references) are in relation to
	// ALL table columns, so we create an empty table chunk based on the table types. It references
	// the indexed columns, and contains nothing for all non-indexed columns.
	DataChunk table_chunk;
	table_chunk.InitializeEmpty(table_types);

	ErrorData error;
	for (auto &index_chunk : source.Chunks(transaction, mapped_column_ids)) {
		D_ASSERT(index_chunk.ColumnCount() == mapped_column_ids.size());
		for (idx_t i = 0; i < mapped_column_ids.size(); i++) {
			auto col_id = mapped_column_ids[i].GetPrimaryIndex();
			table_chunk.data[col_id].Reference(index_chunk.data[i]);
		}

		error = index_list.Append(delete_indexes, table_chunk, start_row, index_append_mode, checkpoint_id);
		if (error.HasError()) {
			break;
		}
		start_row += UnsafeNumericCast<row_t>(index_chunk.size());
	}
	return error;
}

void LocalTableStorage::AppendToTable(DuckTransaction &transaction, TableAppendState &append_state) {
	auto &table = table_ref.get();
	table.InitializeAppend(transaction, append_state);
	auto &collection = *row_groups->collection;
	for (auto &table_chunk : collection.Chunks(transaction)) {
		// Append to the base table.
		table.Append(table_chunk, append_state);
	}
	table.FinalizeAppend(transaction, append_state);
}

void LocalTableStorage::AppendToIndexes(DuckTransaction &transaction, TableAppendState &append_state) {
	// In this function, we might scan all table columns,
	// as we might also append to the table itself (append_to_table).
	auto &table = table_ref.get();
	if (!table.HasIndexes()) {
		// no indexes to append to
		return;
	}
	auto data_table_info = table.GetDataTableInfo();
	auto &index_list = data_table_info->GetIndexes();
	auto &collection = *row_groups->collection;
	auto error = AppendToIndexes(transaction, collection, index_list, table.GetTypes(), append_state.current_row);
	if (error.HasError()) {
		// Revert all appended row IDs.
		auto revert_error = RevertAppendToIndexes(transaction, append_state.row_start, append_state.current_row);
		if (revert_error.HasError()) {
			revert_error.Throw();
		}
		error.Throw();
	}
}

ErrorData LocalTableStorage::RevertAppendToIndexes(DuckTransaction &transaction, row_t row_start, row_t row_end) {
	auto &table = table_ref.get();
	if (!table.HasIndexes()) {
		return ErrorData();
	}
	auto &index_list = table.GetDataTableInfo()->GetIndexes();
	auto &collection = *row_groups->collection;
	ErrorData error;
	row_t current_row = row_start;
	// Remove the data from the indexes, if any.
	for (auto &chunk : collection.Chunks(transaction)) {
		if (current_row >= row_end) {
			// Finished deleting all rows from the index.
			break;
		}
		// Remove the chunk.
		try {
			index_list.RevertAppend(chunk, current_row);
		} catch (std::exception &ex) { // LCOV_EXCL_START
			error = ErrorData(ex);
			break;
		} // LCOV_EXCL_STOP

		current_row += UnsafeNumericCast<row_t>(chunk.size());
	}

#ifdef DEBUG
	// Verify that our index memory is stable.
	table.VerifyIndexBuffers();
#endif
	return error;
}

PhysicalIndex LocalTableStorage::CreateOptimisticCollection(unique_ptr<OptimisticWriteCollection> collection) {
	lock_guard<mutex> l(collections_lock);
	optimistic_collections.push_back(std::move(collection));
	return PhysicalIndex(optimistic_collections.size() - 1);
}

OptimisticWriteCollection &LocalTableStorage::GetOptimisticCollection(const PhysicalIndex collection_index) {
	lock_guard<mutex> l(collections_lock);
	auto &collection = optimistic_collections[collection_index.index];
	return *collection;
}

void LocalTableStorage::ResetOptimisticCollection(const PhysicalIndex collection_index) {
	lock_guard<mutex> l(collections_lock);
	optimistic_collections[collection_index.index].reset();
}

OptimisticDataWriter &LocalTableStorage::GetOptimisticWriter() {
	return optimistic_writer;
}

void LocalTableStorage::Rollback() {
	optimistic_writer.Rollback();

	CommitDropState drop_state(&row_groups->collection->GetBlockManager());
	for (auto &collection : optimistic_collections) {
		if (!collection) {
			continue;
		}
		collection->collection->CommitDropTable(drop_state);
	}
	optimistic_collections.clear();
	row_groups->collection->CommitDropTable(drop_state);
	drop_state.FinalizeCommit();
}

//===--------------------------------------------------------------------===//
// LocalTableManager
//===--------------------------------------------------------------------===//
optional_ptr<LocalTableStorage> LocalTableManager::GetStorage(const DataTable &table) const {
	lock_guard<mutex> l(table_storage_lock);
	auto entry = table_storage.find(table);
	return entry == table_storage.end() ? nullptr : entry->second.get();
}

LocalTableStorage &LocalTableManager::GetOrCreateStorage(ClientContext &context, DataTable &table,
                                                         optional_ptr<DuckTableEntry> table_entry) {
	lock_guard<mutex> l(table_storage_lock);
	auto entry = table_storage.find(table);
	if (entry == table_storage.end()) {
		auto new_storage = make_shared_ptr<LocalTableStorage>(context, table);
		entry = table_storage.insert(make_pair(reference<const DataTable>(table), std::move(new_storage))).first;
	}
	if (table_entry) {
		entry->second->table_entry = table_entry;
	}
	return *entry->second;
}

bool LocalTableManager::IsEmpty() const {
	lock_guard<mutex> l(table_storage_lock);
	return table_storage.empty();
}

shared_ptr<LocalTableStorage> LocalTableManager::MoveEntry(DataTable &table) {
	lock_guard<mutex> l(table_storage_lock);
	auto entry = table_storage.find(table);
	if (entry == table_storage.end()) {
		return nullptr;
	}
	auto storage_entry = std::move(entry->second);
	table_storage.erase(entry);
	return storage_entry;
}

reference_map_t<const DataTable, shared_ptr<LocalTableStorage>> LocalTableManager::MoveEntries() {
	lock_guard<mutex> l(table_storage_lock);
	return std::move(table_storage);
}

vector<shared_ptr<LocalTableStorage>> LocalTableManager::GetEntries() const {
	lock_guard<mutex> l(table_storage_lock);
	vector<shared_ptr<LocalTableStorage>> result;
	result.reserve(table_storage.size());
	for (auto &entry : table_storage) {
		result.push_back(entry.second);
	}
	return result;
}

idx_t LocalTableManager::EstimatedSize() const {
	lock_guard<mutex> l(table_storage_lock);
	idx_t estimated_size = 0;
	for (auto &storage : table_storage) {
		estimated_size += storage.second->EstimatedSize();
	}
	return estimated_size;
}

void LocalTableManager::InsertEntry(DataTable &table, shared_ptr<LocalTableStorage> entry) {
	lock_guard<mutex> l(table_storage_lock);
	D_ASSERT(table_storage.find(table) == table_storage.end());
	table_storage[table] = std::move(entry);
}

//===--------------------------------------------------------------------===//
// LocalStorage
//===--------------------------------------------------------------------===//
LocalStorage::LocalStorage(ClientContext &context, DuckTransaction &transaction)
    : context(context), transaction(transaction) {
}

LocalStorage::CommitState::CommitState() {
}

LocalStorage::CommitState::~CommitState() {
}

LocalStorage &LocalStorage::Get(DuckTransaction &transaction) {
	return transaction.GetLocalStorage();
}

LocalStorage &LocalStorage::Get(ClientContext &context, AttachedDatabase &db) {
	return DuckTransaction::Get(context, db).GetLocalStorage();
}

LocalStorage &LocalStorage::Get(ClientContext &context, Catalog &catalog) {
	return LocalStorage::Get(context, catalog.GetAttached());
}

void LocalStorage::InitializeScan(DataTable &table, CollectionScanState &state,
                                  optional_ptr<TableFilterSet> table_filters) {
	auto storage = table_manager.GetStorage(table);
	if (storage == nullptr || storage->GetCollection().GetTotalRows() == 0) {
		return;
	}
	storage->InitializeScan(state, table_filters);
}

void LocalStorage::Scan(CollectionScanState &state, const vector<StorageIndex> &, DataChunk &result) {
	state.Scan(transaction, result);
}

void LocalStorage::InitializeParallelScan(DataTable &table, ParallelCollectionScanState &state) {
	auto storage = table_manager.GetStorage(table);
	if (!storage) {
		state.max_row = 0;
		state.vector_index = 0;
		state.current_row_group = nullptr;
	} else {
		storage->GetCollection().InitializeParallelScan(state);
	}
}

RowGroupCollection &LocalTableStorage::GetCollection() {
	return *row_groups->collection;
}

OptimisticWriteCollection &LocalTableStorage::GetPrimaryCollection() {
	return *row_groups;
}

optional_idx LocalStorage::NextParallelScan(ClientContext &context, DataTable &table,
                                            ParallelCollectionScanState &state, CollectionScanState &scan_state,
                                            bool initialize_columns) {
	auto storage = table_manager.GetStorage(table);
	if (!storage) {
		return optional_idx();
	}
	return storage->GetCollection().NextParallelScan(context, state, scan_state, initialize_columns);
}

void LocalStorage::InitializeAppend(LocalAppendState &state, DataTable &table, DuckTableEntry &table_entry) {
	state.storage = &table_manager.GetOrCreateStorage(context, table, table_entry);
	state.storage->GetCollection().InitializeAppend(TransactionData(transaction), state.append_state);
}

void LocalStorage::InitializeStorage(LocalAppendState &state, DataTable &table, DuckTableEntry &table_entry) {
	state.storage = &table_manager.GetOrCreateStorage(context, table, table_entry);
}

void LocalTableStorage::AppendToDeleteIndexes(Vector &row_ids, DataChunk &delete_chunk) {
	if (delete_chunk.size() == 0) {
		return;
	}

	// Only committed row IDs (< MAX_ROW_ID) belong in the delete indexes.
	// Local row IDs (>= MAX_ROW_ID) live in transaction-local storage and are
	// handled directly by LocalStorage::Delete.
	row_ids.Flatten();
	auto flat_row_ids = FlatVector::GetData<row_t>(row_ids);
	idx_t committed_count = 0;
	SelectionVector committed_sel(delete_chunk.size());
	for (idx_t i = 0; i < delete_chunk.size(); i++) {
		if (flat_row_ids[i] < MAX_ROW_ID) {
			committed_sel.set_index(committed_count++, i);
		}
	}

	if (committed_count == 0) {
		return;
	}

	DataChunk committed_chunk;
	committed_chunk.Initialize(Allocator::DefaultAllocator(), delete_chunk.GetTypes());
	committed_chunk.Slice(delete_chunk, committed_sel, committed_count);

	Vector committed_row_ids(row_ids, committed_sel, committed_count);
	committed_row_ids.Flatten();

	delete_indexes.AppendToDeleteIndexes(committed_chunk, committed_row_ids);
}

void LocalStorage::Append(LocalAppendState &state, DataChunk &table_chunk) {
	auto storage = state.storage;
	auto offset = NumericCast<idx_t>(MAX_ROW_ID) + storage->GetCollection().GetNextRowId();
	idx_t base_id = offset + state.append_state.total_append_count;

	if (!storage->append_indexes.Empty()) {
		auto error = storage->append_indexes.Append(storage->delete_indexes, table_chunk, NumericCast<row_t>(base_id),
		                                            storage->index_append_mode, optional_idx());
		if (error.HasError()) {
			error.Throw();
		}
	}

	// Append the chunk to the local storage.
	auto flushed_row_group_idx = storage->GetCollection().Append(table_chunk, state.append_state);

	// Check if we should pre-emptively flush blocks to disk.
	if (flushed_row_group_idx.IsValid()) {
		storage->WriteNewRowGroup(flushed_row_group_idx.GetIndex());
	}
}

void LocalStorage::FinalizeAppend(LocalAppendState &state) {
	state.storage->GetCollection().FinalizeAppend(state.append_state.transaction, state.append_state);
}

void LocalStorage::LocalMerge(DataTable &table, DuckTableEntry &table_entry, OptimisticWriteCollection &collection) {
	auto &storage = table_manager.GetOrCreateStorage(context, table, table_entry);
	if (!storage.append_indexes.Empty()) {
		// append data to indexes if required
		row_t base_id = MAX_ROW_ID + NumericCast<row_t>(storage.GetCollection().GetNextRowId());
		auto error = storage.AppendToIndexes(transaction, *collection.collection, storage.append_indexes,
		                                     table.GetTypes(), base_id);
		if (error.HasError()) {
			error.Throw();
		}
	}
	auto &target = storage.GetPrimaryCollection();
	target.MergeStorage(collection);
}

PhysicalIndex LocalStorage::CreateOptimisticCollection(DataTable &table,
                                                       unique_ptr<OptimisticWriteCollection> collection) {
	auto &storage = table_manager.GetOrCreateStorage(context, table);
	return storage.CreateOptimisticCollection(std::move(collection));
}

OptimisticWriteCollection &LocalStorage::GetOptimisticCollection(DataTable &table,
                                                                 const PhysicalIndex collection_index) {
	auto &storage = table_manager.GetOrCreateStorage(context, table);
	return storage.GetOptimisticCollection(collection_index);
}

void LocalStorage::ResetOptimisticCollection(DataTable &table, const PhysicalIndex collection_index) {
	auto &storage = table_manager.GetOrCreateStorage(context, table);
	storage.ResetOptimisticCollection(collection_index);
}

OptimisticDataWriter &LocalStorage::GetOptimisticWriter(DataTable &table) {
	auto &storage = table_manager.GetOrCreateStorage(context, table);
	return storage.GetOptimisticWriter();
}

bool LocalStorage::ChangesMade() noexcept {
	return !table_manager.IsEmpty();
}

bool LocalStorage::Find(DataTable &table) {
	return table_manager.GetStorage(table) != nullptr;
}

idx_t LocalStorage::EstimatedSize() {
	return table_manager.EstimatedSize();
}

idx_t LocalStorage::Delete(DataTable &table, DuckTableEntry &table_entry, Vector &row_ids, idx_t count) {
	auto storage = table_manager.GetStorage(table);
	D_ASSERT(storage);

	auto ids = FlatVector::GetDataMutable<row_t>(row_ids);
	if (storage->append_indexes.Empty()) {
		idx_t delete_count = storage->GetCollection().Delete(TransactionData::Unversioned(), table_entry, ids, count);
		storage->deleted_rows += delete_count;
		return delete_count;
	}
	// a row can be matched multiple times (e.g. DELETE ... USING) - only remove the rows that were deleted now from
	// the unique indices
	DeletedRowIds deleted_ids(ids);
	idx_t delete_count =
	    storage->GetCollection().Delete(TransactionData::Unversioned(), table_entry, ids, count, deleted_ids);
	storage->deleted_rows += delete_count;
	if (!deleted_ids.has_skipped_rows) {
		// all rows were deleted
		storage->GetCollection().RemoveFromIndexes(context, storage->append_indexes, row_ids, count,
		                                           IndexRemovalType::MAIN_INDEX_ONLY);
		return delete_count;
	}
	auto &deleted_row_ids = deleted_ids.row_ids;
	if (!deleted_row_ids.empty()) {
		Vector deleted_row_id_vector(LogicalType::ROW_TYPE, deleted_row_ids.size());
		{
			auto writer = FlatVector::Writer<row_t>(deleted_row_id_vector, deleted_row_ids.size());
			for (auto &row_id : deleted_row_ids) {
				writer.WriteValue(row_id);
			}
		}
		storage->GetCollection().RemoveFromIndexes(context, storage->append_indexes, deleted_row_id_vector,
		                                           deleted_row_ids.size(), IndexRemovalType::MAIN_INDEX_ONLY);
	}
	return delete_count;
}

void LocalStorage::Update(DataTable &table, DuckTableEntry &table_entry, Vector &row_ids,
                          const vector<PhysicalIndex> &column_ids, DataChunk &updates) {
	D_ASSERT(updates.size() >= 1);
	auto storage = table_manager.GetStorage(table);
	D_ASSERT(storage);

	auto ids = FlatVector::GetDataMutable<row_t>(row_ids);
	storage->GetCollection().Update(TransactionData::Unversioned(), table_entry, ids, column_ids, updates);
}

void LocalStorage::Flush(DataTable &table, LocalTableStorage &storage, optional_ptr<StorageCommitState> commit_state) {
	if (storage.is_dropped) {
		storage.Rollback();
		return;
	}
	if (storage.GetCollection().GetTotalRows() <= storage.deleted_rows) {
		// all rows that we added were deleted
		// rollback any partial blocks that are still outstanding
		storage.Rollback();
		return;
	}

	auto append_count = storage.GetCollection().GetTotalRows() - storage.deleted_rows;

	TableAppendState append_state;
	table.AppendLock(transaction, append_state);
	if (storage.IsBulkAppend() ||
	    (append_state.row_start == 0 && storage.deleted_rows == 0 && !storage.WritesToDisk())) {
		// bulk append (at least one full row group, no deletes): move over the storage directly.
		// Appends to an empty table are also merged directly if the table cannot be written to
		// disk (temporary / in-memory / read-only, e.g. WAL replay of a read-only attach) -
		// there are no optimistically written blocks to manage, and merging avoids re-appending
		// row by row.
		// first flush any outstanding blocks
		storage.FlushBlocks();
		// Append to the indexes.
		storage.AppendToIndexes(transaction, append_state);
		// finally move over the row groups
		table.MergeStorage(storage.GetCollection(), commit_state);
	} else {
		// check if we have written data
		// if we have, we cannot merge to disk after all
		// so we need to revert the data we have already written
		// this happens when rows were deleted after a bulk append, or when the optimistic writer
		// flushed a partial row group that does not qualify as a bulk append
		storage.Rollback();
		// append to the indexes
		storage.AppendToIndexes(transaction, append_state);
		const auto index_end = append_state.current_row;
		// after that is successful - append to the table
		try {
			storage.AppendToTable(transaction, append_state);
		} catch (...) {
			// remove the rows and index entries that the failed append added
			try {
				table.RevertFailedAppend(append_state);
				// free the memory of the reverted rows before this storage is scanned again
				append_state.row_group_append_state.states.reset();
				append_state.row_groups.reset();
				auto error = storage.RevertAppendToIndexes(transaction, append_state.row_start, index_end);
				if (error.HasError()) {
					error.Throw();
				}
			} catch (std::exception &ex) {
				ValidChecker::Invalidate(table.db.GetDatabase(),
				                         "Failed to revert a failed append: " + ErrorData(ex).RawMessage());
			}
			throw;
		}
	}
	// Initialization and ALTER keep table_entry current until commit.
	D_ASSERT(storage.table_entry);
	transaction.PushAppend(*storage.table_entry, NumericCast<idx_t>(append_state.row_start), append_count);

#ifdef DEBUG
	// Verify that our index memory is stable.
	table.VerifyIndexBuffers();
#endif
}

void LocalStorage::FlushBulkAppendBlocksAndSync(AttachedDatabase &db) {
	bool requires_sync = false;
	for (auto &storage : table_manager.GetEntries()) {
		if (storage->IsBulkAppend()) {
			// Flush() is guaranteed to take the bulk path - the blocks will be used as written
			storage->FlushBlocks();
			requires_sync |= storage->HasFlushedRowGroups();
		}
	}
	if (requires_sync) {
		// the WAL will reference the flushed row groups (flushed just now or already during the
		// statement, e.g. by batch inserts) - persist them now so that the commit does not have
		// to FileSync while holding the commit lock
		db.GetStorageManager().GetBlockManager().FileSync();
		synced_flushed_blocks = true;
	}
}

void LocalStorage::Commit(optional_ptr<StorageCommitState> commit_state) {
	// commit local storage
	// iterate over all entries in the table storage map and commit them
	// after this, the local storage is no longer required and can be cleared
	auto table_storage = table_manager.MoveEntries();
	for (auto &entry : table_storage) {
		auto storage = entry.second.get();
		D_ASSERT(RefersToSameObject(entry.first, storage->table_ref));
		Flush(storage->table_ref, *storage, commit_state);
		entry.second.reset();
	}
}

void LocalStorage::Rollback() {
	// rollback local storage
	// after this, the local storage is no longer required and can be cleared
	auto table_storage = table_manager.MoveEntries();
	for (auto &entry : table_storage) {
		auto storage = entry.second.get();
		if (!storage) {
			continue;
		}
		storage->Rollback();
		entry.second.reset();
	}
}

idx_t LocalStorage::AddedRows(DataTable &table) {
	auto storage = table_manager.GetStorage(table);
	if (!storage) {
		return 0;
	}
	return storage->GetCollection().GetTotalRows() - storage->deleted_rows;
}

vector<PartitionStatistics> LocalStorage::GetPartitionStats(DataTable &table, TransactionData transaction) const {
	auto storage = table_manager.GetStorage(table);
	if (!storage) {
		return vector<PartitionStatistics>();
	}
	return storage->GetCollection().GetPartitionStats(transaction);
}

void LocalStorage::DropTable(DataTable &table) {
	auto storage = table_manager.GetStorage(table);
	if (!storage) {
		return;
	}
	storage->is_dropped = true;
}

void LocalStorage::MoveStorage(DataTable &old_dt, DataTable &new_dt) {
	// check if there are any pending appends for the old version of the table
	auto new_storage = table_manager.MoveEntry(old_dt);
	if (!new_storage) {
		return;
	}
	// take over the storage from the old entry
	new_storage->table_ref = new_dt;
	table_manager.InsertEntry(new_dt, std::move(new_storage));
}

void LocalStorage::AddColumn(DataTable &old_dt, DataTable &new_dt, ColumnDefinition &new_column,
                             ExpressionExecutor &default_executor) {
	// check if there are any pending appends for the old version of the table
	auto storage = table_manager.MoveEntry(old_dt);
	if (!storage) {
		return;
	}
	auto new_storage = make_shared_ptr<LocalTableStorage>(context, new_dt, *storage, new_column, default_executor);
	table_manager.InsertEntry(new_dt, std::move(new_storage));
}

void LocalStorage::DropColumn(DataTable &old_dt, DataTable &new_dt, const idx_t drop_column_index) {
	// check if there are any pending appends for the old version of the table
	auto storage = table_manager.MoveEntry(old_dt);
	if (!storage) {
		return;
	}
	auto new_storage = make_shared_ptr<LocalTableStorage>(new_dt, *storage, drop_column_index);
	table_manager.InsertEntry(new_dt, std::move(new_storage));
}

void LocalStorage::ChangeType(DataTable &old_dt, DataTable &new_dt, idx_t changed_idx, const LogicalType &target_type,
                              const vector<StorageIndex> &bound_columns, Expression &cast_expr) {
	// check if there are any pending appends for the old version of the table
	auto storage = table_manager.MoveEntry(old_dt);
	if (!storage) {
		return;
	}
	auto new_storage = make_shared_ptr<LocalTableStorage>(context, new_dt, *storage, changed_idx, target_type,
	                                                      bound_columns, cast_expr, transaction);
	table_manager.InsertEntry(new_dt, std::move(new_storage));
}

void LocalStorage::FetchChunk(DataTable &table, const Vector &row_ids, idx_t count, const vector<StorageIndex> &col_ids,
                              DataChunk &chunk, ColumnFetchState &fetch_state) {
	auto storage = table_manager.GetStorage(table);
	if (!storage) {
		throw InternalException("LocalStorage::FetchChunk - local storage not found");
	}
	storage->GetCollection().Fetch(transaction, chunk, col_ids, row_ids, count, fetch_state);
}

bool LocalStorage::CanFetch(DataTable &table, const row_t row_id) {
	auto storage = table_manager.GetStorage(table);
	if (!storage) {
		throw InternalException("LocalStorage::CanFetch - local storage not found");
	}
	return storage->GetCollection().CanFetch(transaction, row_id);
}

TableIndexList &LocalStorage::GetIndexes(ClientContext &context, DataTable &table) {
	auto &storage = table_manager.GetOrCreateStorage(context, table);
	return storage.append_indexes;
}

optional_ptr<LocalTableStorage> LocalStorage::GetStorage(const DataTable &table) {
	return table_manager.GetStorage(table);
}

void LocalStorage::VerifyNewConstraint(DataTable &parent, const BoundConstraint &constraint) {
	auto storage = table_manager.GetStorage(parent);
	if (!storage) {
		return;
	}
	storage->GetCollection().VerifyNewConstraint(context, transaction, parent, constraint);
}

} // namespace duckdb
