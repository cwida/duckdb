#include "duckdb/transaction/commit_state.hpp"
#include "duckdb/transaction/transaction_data.hpp"

#include "duckdb/catalog/catalog_entry/duck_index_entry.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/duck_schema_entry.hpp"
#include "duckdb/catalog/catalog_entry/trigger_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/scalar_macro_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/type_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/dependency/dependency_entry.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/storage/block_manager.hpp"
#include "duckdb/storage/table/chunk_info.hpp"
#include "duckdb/storage/table/column_data.hpp"
#include "duckdb/storage/table/row_version_manager.hpp"
#include "duckdb/storage/table/table_index_list.hpp"
#include "duckdb/storage/table/update_segment.hpp"
#include "duckdb/storage/write_ahead_log.hpp"
#include "duckdb/transaction/append_info.hpp"
#include "duckdb/transaction/delete_info.hpp"
#include "duckdb/transaction/update_info.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/row_group_collection.hpp"
#include "duckdb/storage/table/row_group_segment_tree.hpp"
#include "duckdb/common/types/data_chunk.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// CommitDropState
//===--------------------------------------------------------------------===//
CommitDropState::CommitDropState(optional_ptr<BlockManager> block_manager) : block_manager(block_manager) {
}

void CommitDropState::DropBlock(block_id_t block_id) {
	dropped_block_ids.push_back(block_id);
}

void CommitDropState::DropIndex(shared_ptr<DataTableInfo> info, idx_t index_oid) {
	dropped_indexes.push_back(DroppedIndex {std::move(info), index_oid, nullptr});
}

void CommitDropState::DropTable(shared_ptr<DataTable> table) {
	dropped_tables.push_back(std::move(table));
}

void CommitDropState::DropColumn(shared_ptr<DataTable> table, idx_t column_index) {
	dropped_columns.emplace_back(std::move(table), column_index);
}

void CommitDropState::DetachIndexes() {
	for (auto &index : dropped_indexes) {
		index.detached_entry = index.info->GetIndexes().DetachIndex(index.index_oid);
	}
}

void CommitDropState::ExcludeFromCheckpoints() {
	if (excluded) {
		return;
	}
	// older snapshots still read the dropped tables, whose metadata the checkpoint frees
	for (auto &table : dropped_tables) {
		for (auto &row_group : table->GetRowGroupCollection()->GetRowGroups()->Segments()) {
			row_group.LoadMetadata();
		}
	}
	excluded = true;
	Collect();
	if (!block_manager) {
		return;
	}
	for (auto block_id : dropped_block_ids) {
		block_manager->MarkBlockAsExcluded(block_id);
	}
}

void CommitDropState::Collect() {
	// nothing uses the detached indexes any more
	for (auto &index : dropped_indexes) {
		if (index.detached_entry) {
			index.info->GetIndexes().RemoveDetachedIndex(*index.detached_entry);
		}
	}
	dropped_indexes.clear();
	// collected now: a checkpoint that wrote the table frees the blocks it replaced itself
	for (auto &table : dropped_tables) {
		table->CommitDropTable(*this);
	}
	for (auto &column : dropped_columns) {
		column.first->CommitDropColumn(column.second, *this);
	}
}

void CommitDropState::Free() {
	if (!excluded) {
		// no snapshot reads the dropped tables any more
		Collect();
	}
	// the index blocks are freed with the others, not when the indexes are destroyed
	for (auto &table : dropped_tables) {
		table->GetDataTableInfo()->GetIndexes().DisownBlocks();
	}
	if (block_manager) {
		for (auto block_id : dropped_block_ids) {
			if (excluded) {
				block_manager->MarkExcludedBlockAsModified(block_id);
			} else {
				block_manager->MarkBlockAsModified(block_id);
			}
		}
	}
	// assert that !block_manager -> dropped_block_ids.empty()
	D_ASSERT(block_manager || dropped_block_ids.empty());
	dropped_block_ids.clear();
}

bool CommitDropState::Empty() const {
	return dropped_tables.empty() && dropped_columns.empty() && dropped_indexes.empty();
}

//===--------------------------------------------------------------------===//
// IndexDataRemover
//===--------------------------------------------------------------------===//
IndexDataRemover::IndexDataRemover(DuckTransaction &transaction_p, QueryContext context, IndexRemovalType removal_type)
    : transaction(transaction_p), context(context), removal_type(removal_type) {
}

void IndexDataRemover::PushDelete(DeleteInfo &info) {
	auto &version_table = info.table->GetStorage();
	if (!version_table.HasIndexes()) {
		// this table has no indexes: no cleanup to be done
		return;
	}

	idx_t count = 0;
	row_t row_numbers[STANDARD_VECTOR_SIZE];
	if (info.is_consecutive) {
		for (idx_t i = 0; i < info.count; i++) {
			row_numbers[count++] = UnsafeNumericCast<int64_t>(info.base_row + i);
		}
	} else {
		auto rows = info.GetRows();
		for (idx_t i = 0; i < info.count; i++) {
			row_numbers[count++] = UnsafeNumericCast<int64_t>(info.base_row + rows[i]);
		}
	}
	Flush(version_table, row_numbers, count);
}

void IndexDataRemover::Verify() {
#ifdef DEBUG
	// Verify that our index memory is stable.
	for (auto &table : verify_indexes) {
		table.second->VerifyIndexBuffers();
	}
#endif
}

void CommitState::Verify() {
	index_data_remover.Verify();
}

void IndexDataRemover::Flush(DataTable &table, row_t *row_numbers, idx_t count) {
	if (count == 0) {
		return;
	}
#ifdef DEBUG
	verify_indexes.insert(make_pair(reference<DataTable>(table), table.GetDataTableInfo()));
#endif

	// set up the row identifiers vector
	Vector row_identifiers(LogicalType::ROW_TYPE, data_ptr_cast(row_numbers), count);

	auto checkpoint_id = transaction.GetTransactionManager().Cast<DuckTransactionManager>().GetActiveCheckpoint();
	// delete the tuples from all the indexes.
	// If there is any issue with removal, a FatalException must be thrown since there may be a corruption of
	// data, hence the transaction cannot be guaranteed.
	try {
		table.RemoveFromIndexes(context, row_identifiers, count, removal_type, checkpoint_id);
	} catch (std::exception &ex) {
		throw FatalException(ErrorData(ex).Message());
	} catch (...) {
		throw FatalException("unknown failure in CommitState::Flush");
	}

	count = 0;
}

//===--------------------------------------------------------------------===//
// CommitState
//===--------------------------------------------------------------------===//
CommitState::CommitState(DuckTransaction &transaction_p, transaction_t commit_id,
                         ActiveTransactionState transaction_state, CommitMode commit_mode)
    : transaction(transaction_p), commit_id(commit_id),
      index_data_remover(transaction, *transaction.context.lock(),
                         GetIndexRemovalType(transaction_state, commit_mode)) {
	D_ASSERT(commit_mode != CommitMode::COMMIT || IsCommitted(commit_id));
}

IndexRemovalType CommitState::GetIndexRemovalType(ActiveTransactionState transaction_state, CommitMode commit_mode) {
	if (commit_mode == CommitMode::COMMIT) {
		if (transaction_state == ActiveTransactionState::NO_OTHER_TRANSACTIONS) {
			// if there are no other active transactions we don't need to store removed rows in deleted_rows_in_use
			return IndexRemovalType::MAIN_INDEX_ONLY;
		}
		return IndexRemovalType::MAIN_INDEX;
	}
	// revert the appends to the indexes
	if (transaction_state == ActiveTransactionState::NO_OTHER_TRANSACTIONS) {
		return IndexRemovalType::REVERT_MAIN_INDEX_ONLY;
	}
	return IndexRemovalType::REVERT_MAIN_INDEX;
}

void CommitState::CommitEntryDrop(CatalogEntry &entry, data_ptr_t dataptr, CommitInfo &info) {
	auto &drop_state = *info.drop_state;
	if (entry.temporary || entry.Parent().temporary) {
		return;
	}

	// look at the type of the parent entry
	auto &parent = entry.Parent();

	switch (parent.type) {
	case CatalogType::TRIGGER_ENTRY:
	case CatalogType::TABLE_ENTRY:
	case CatalogType::VIEW_ENTRY:
	case CatalogType::INDEX_ENTRY:
	case CatalogType::SEQUENCE_ENTRY:
	case CatalogType::TYPE_ENTRY:
	case CatalogType::MACRO_ENTRY:
	case CatalogType::TABLE_MACRO_ENTRY:
		if (entry.type == CatalogType::RENAMED_ENTRY || entry.type == parent.type) {
			// ALTER statement, read the extra data after the entry
			auto extra_data_size = Load<idx_t>(dataptr);
			auto extra_data = data_ptr_cast(dataptr + sizeof(idx_t));

			MemoryStream source(extra_data, extra_data_size);
			BinaryDeserializer deserializer(source);
			deserializer.Begin();
			auto column_name = deserializer.ReadProperty<string>(100, "column_name");
			auto parse_info = deserializer.ReadProperty<unique_ptr<ParseInfo>>(101, "alter_info");
			deserializer.End();

			switch (parent.type) {
			case CatalogType::TABLE_ENTRY:
				if (!column_name.empty()) {
					D_ASSERT(entry.type != CatalogType::RENAMED_ENTRY);
					auto &table_entry = entry.Cast<DuckTableEntry>();
					D_ASSERT(table_entry.IsDuckTable());
					// write the alter table in the log
					table_entry.CommitAlter(column_name, drop_state);
				}
				break;
			case CatalogType::VIEW_ENTRY:
			case CatalogType::INDEX_ENTRY:
			case CatalogType::SEQUENCE_ENTRY:
			case CatalogType::TYPE_ENTRY:
			case CatalogType::MACRO_ENTRY:
			case CatalogType::TABLE_MACRO_ENTRY:
			case CatalogType::TRIGGER_ENTRY:
				(void)column_name;
				break;
			default:
				throw InternalException("Don't know how to alter this type!");
			}
		} else {
			switch (parent.type) {
			case CatalogType::TABLE_ENTRY:
			case CatalogType::VIEW_ENTRY:
			case CatalogType::INDEX_ENTRY:
			case CatalogType::SEQUENCE_ENTRY:
			case CatalogType::TYPE_ENTRY:
			case CatalogType::MACRO_ENTRY:
			case CatalogType::TABLE_MACRO_ENTRY:
			case CatalogType::TRIGGER_ENTRY:
				break;
			default:
				throw InternalException("Don't know how to create this type!");
			}
		}
		break;
	case CatalogType::SCHEMA_ENTRY:
		break;
	case CatalogType::RENAMED_ENTRY:
		// This is a rename, nothing needs to be done for this
		break;
	case CatalogType::DELETED_ENTRY:
		switch (entry.type) {
		case CatalogType::TABLE_ENTRY: {
			auto &table_entry = entry.Cast<DuckTableEntry>();
			D_ASSERT(table_entry.IsDuckTable());

			// If the table was renamed, we do not need to drop the DataTable.
			table_entry.CommitDrop(drop_state);
			break;
		}
		case CatalogType::INDEX_ENTRY: {
			auto &index_entry = entry.Cast<DuckIndexEntry>();
			index_entry.CommitDrop(drop_state);
			break;
		}
		default:
			// no action required
			break;
		}
		break;
	case CatalogType::DATABASE_ENTRY:
	case CatalogType::PREPARED_STATEMENT:
	case CatalogType::AGGREGATE_FUNCTION_ENTRY:
	case CatalogType::SCALAR_FUNCTION_ENTRY:
	case CatalogType::TABLE_FUNCTION_ENTRY:
	case CatalogType::COPY_FUNCTION_ENTRY:
	case CatalogType::PRAGMA_FUNCTION_ENTRY:
	case CatalogType::COLLATION_ENTRY:
	case CatalogType::COORDINATE_SYSTEM_ENTRY:
	case CatalogType::DEPENDENCY_ENTRY:
	case CatalogType::SECRET_ENTRY:
	case CatalogType::SECRET_TYPE_ENTRY:
	case CatalogType::SECRET_FUNCTION_ENTRY:
		// do nothing, these entries are not persisted to disk
		break;
	default:
		throw InternalException("UndoBuffer - don't know how to write this entry to the WAL");
	}
}

void CommitState::CommitEntry(UndoFlags type, data_ptr_t data, CommitInfo &info) {
	switch (type) {
	case UndoFlags::CATALOG_ENTRY: {
		auto &old_entry = *Load<CatalogEntry *>(data);
		D_ASSERT(old_entry.HasParent());

		auto &catalog = old_entry.ParentCatalog();
		D_ASSERT(catalog.IsDuckCatalog());

		auto &new_entry = old_entry.Parent();
		if (old_entry.type == CatalogType::TABLE_ENTRY && new_entry.type == CatalogType::TABLE_ENTRY) {
			auto &old_storage = old_entry.Cast<DuckTableEntry>().GetStorage();
			auto &new_storage = new_entry.Cast<DuckTableEntry>().GetStorage();
			if (!RefersToSameObject(old_storage, new_storage) && old_storage.IsMainTable()) {
				throw TransactionException("Failed to alter table %s because the underlying table state was "
				                           "reverted by a concurrent transaction",
				                           old_entry.name);
			}
		}
		if (new_entry.type == CatalogType::DEPENDENCY_ENTRY) {
			auto &dep = new_entry.Cast<DependencyEntry>();
			if (dep.Side() == DependencyEntryType::SUBJECT) {
				new_entry.set->VerifyExistenceOfDependency(commit_id, new_entry);
			}
		} else if (new_entry.type == CatalogType::DELETED_ENTRY && old_entry.set) {
			old_entry.set->CommitDrop(commit_id, transaction.view.visibility_bound, old_entry);
		}
		// Grab a write lock on the catalog
		auto &duck_catalog = catalog.Cast<DuckCatalog>();
		lock_guard<mutex> write_lock(duck_catalog.GetWriteLock());
		lock_guard<mutex> read_lock(old_entry.set->GetCatalogLock());

		// For a genuine CREATE TRIGGER (not an ALTER propagation), verify that the table version
		// the trigger was bound against is still current at commit time.  Checking column
		// existence is insufficient: a concurrent transaction can rename the column and re-add
		// a new column with the same name, making ColumnExists pass while the trigger is stale.
		// Instead we compare table entry pointers: any structural change to the table produces a
		// new CatalogEntry object.  Two orderings need to be handled:
		//   (A) rename commits before this trigger commits  → bound and current entries differ
		//   (B) this trigger commits before rename commits  → head entry has a write-write conflict
		// old_entry.type is TRIGGER_ENTRY only for column-rename propagations (ALTER), which we skip.
		if (new_entry.type == CatalogType::TRIGGER_ENTRY && old_entry.type != CatalogType::TRIGGER_ENTRY) {
			auto &trig = new_entry.Cast<TriggerCatalogEntry>();
			if (!trig.columns.empty()) {
				auto &table_set = trig.schema.Cast<DuckSchemaEntry>().GetCatalogSet(CatalogType::TABLE_ENTRY);
				// Transaction view at bind time (what the trigger saw when it was created).
				// Use commit_id as the transaction_id so that earlier catalog changes in this
				// same transaction (already stamped with commit_id) are visible here.
				CatalogTransaction bind_txn(duck_catalog.GetDatabase(), commit_id, transaction.view.visibility_bound);
				// Transaction view at commit time (all changes committed before this commit)
				CatalogTransaction commit_txn(duck_catalog.GetDatabase(), MAX_TRANSACTION_ID,
				                              VisibilityBound::Through(commit_id));

				auto bound_table = trig.schema.GetEntry(bind_txn, CatalogType::TABLE_ENTRY, trig.base_table->Table());
				auto current_table =
				    trig.schema.GetEntry(commit_txn, CatalogType::TABLE_ENTRY, trig.base_table->Table());

				// Case (A): a concurrent alter was committed while this trigger was binding
				if (bound_table && current_table && !RefersToSameObject(*bound_table, *current_table)) {
					throw TransactionException("Catalog write-write conflict on create with %s: "
					                           "table %s was altered by a concurrent transaction",
					                           trig.name, trig.base_table->Table());
				}

				// Case (B): an uncommitted alter is in flight; this trigger would commit first,
				// leaving the catalog in an inconsistent state once the alter commits.
				auto head_entry = table_set.GetHeadEntry(trig.base_table->Table());
				if (head_entry && table_set.HasConflict(commit_txn, head_entry->timestamp) &&
				    head_entry->type == CatalogType::TABLE_ENTRY && !head_entry->deleted) {
					throw TransactionException("Catalog write-write conflict on create with %s: "
					                           "table %s is being altered by a concurrent transaction",
					                           trig.name, trig.base_table->Table());
				}
			}
		}

		// Set the timestamp of the catalog entry to the given commit_id, marking it as committed
		CatalogSet::UpdateTimestamp(old_entry.Parent(), commit_id);

		// drop any blocks associated with the catalog entry if possible (e.g. in case of a DROP or ALTER)
		CommitEntryDrop(old_entry, data + sizeof(CatalogEntry *), info);
		break;
	}
	case UndoFlags::INSERT_TUPLE: {
		// append:
		auto info = reinterpret_cast<AppendInfo *>(data);
		if (info->table->HasParent() && info->table->Parent().timestamp != transaction.GetTransactionId()) {
			auto &storage = info->table->GetStorage();
			auto table_name = storage.GetTableName();
			auto table_modification = storage.TableModification();
			throw TransactionException("Attempting to modify table %s but another transaction has %s this table",
			                           table_name, table_modification);
		}
		// mark the tuples as committed
		info->table->GetStorage().CommitAppend(commit_id, info->start_row, info->count);
		break;
	}
	case UndoFlags::DELETE_TUPLE: {
		// deletion:
		auto info = reinterpret_cast<DeleteInfo *>(data);
		if (info->table->HasParent() && info->table->Parent().timestamp != transaction.GetTransactionId()) {
			auto &storage = info->table->GetStorage();
			auto table_name = storage.GetTableName();
			auto table_modification = storage.TableModification();
			throw TransactionException("Attempting to modify table %s but another transaction has %s this table",
			                           table_name, table_modification);
		}
		CommitDelete(*info, transaction.GetTransactionId());
		break;
	}
	case UndoFlags::UPDATE_TUPLE: {
		// update:
		auto info = reinterpret_cast<UpdateInfo *>(data);
		if (info->table->HasParent() && info->table->Parent().timestamp != transaction.GetTransactionId()) {
			auto &storage = info->table->GetStorage();
			auto table_name = storage.GetTableName();
			auto table_modification = storage.TableModification();
			throw TransactionException("Attempting to modify table %s but another transaction has %s this table",
			                           table_name, table_modification);
		}
		info->version_number = commit_id;
		break;
	}
	case UndoFlags::UPDATE_ROWS: {
		auto info = reinterpret_cast<DeleteInfo *>(data);
		info->version_info->CommitWrite(transaction.GetTransactionId(), commit_id, *info, false);
		break;
	}
	case UndoFlags::ATTACHED_DATABASE:
	case UndoFlags::SEQUENCE_VALUE: {
		break;
	}
	default:
		throw InternalException("UndoBuffer - don't know how to commit this type!");
	}
}

void CommitState::CommitDelete(DeleteInfo &info, transaction_t old_id) {
	// mark the tuples as committed
	info.version_info->CommitWrite(old_id, commit_id, info, true);
	// delete from indexes
	index_data_remover.PushDelete(info);
}

void CommitState::RevertCommit(UndoFlags type, data_ptr_t data) {
	transaction_t transaction_id = commit_id;
	switch (type) {
	case UndoFlags::CATALOG_ENTRY: {
		// set the commit timestamp of the catalog entry to the given id
		auto catalog_entry = Load<CatalogEntry *>(data);
		D_ASSERT(catalog_entry->HasParent());
		CatalogSet::UpdateTimestamp(catalog_entry->Parent(), transaction_id);
		if (catalog_entry->name != catalog_entry->Parent().name) {
			CatalogSet::UpdateTimestamp(*catalog_entry, transaction_id);
		}
		break;
	}
	case UndoFlags::INSERT_TUPLE: {
		auto info = reinterpret_cast<AppendInfo *>(data);
		// revert this append
		info->table->GetStorage().RevertAppend(transaction, info->start_row, info->count);
		break;
	}
	case UndoFlags::DELETE_TUPLE: {
		// deletion:
		auto info = reinterpret_cast<DeleteInfo *>(data);
		// revert the commit by writing the (uncommitted) transaction_id back into the version info
		CommitDelete(*info, transaction.commit_id);
		break;
	}
	case UndoFlags::UPDATE_TUPLE: {
		// update:
		auto info = reinterpret_cast<UpdateInfo *>(data);
		info->version_number = transaction_id;
		break;
	}
	case UndoFlags::UPDATE_ROWS: {
		auto info = reinterpret_cast<DeleteInfo *>(data);
		// revert the commit by writing the (uncommitted) transaction_id back into the version info
		info->version_info->CommitWrite(transaction.commit_id, transaction_id, *info, false);
		break;
	}
	case UndoFlags::ATTACHED_DATABASE:
	case UndoFlags::SEQUENCE_VALUE: {
		break;
	}
	default:
		throw InternalException("UndoBuffer - don't know how to revert commit of this type!");
	}
}

} // namespace duckdb
