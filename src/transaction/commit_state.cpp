#include "duckdb/transaction/commit_state.hpp"
#include "duckdb/transaction/append_info.hpp"
#include "duckdb/transaction/delete_info.hpp"
#include "duckdb/transaction/update_info.hpp"

#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/write_ahead_log.hpp"
#include "duckdb/storage/uncompressed_segment.hpp"
#include "duckdb/catalog/catalog_set.hpp"
#include "duckdb/common/serializer/buffered_deserializer.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/storage/table/update_segment.hpp"
#include "duckdb/storage/table/column_data.hpp"
#include "duckdb/storage/table/row_group.hpp"

#include "duckdb/storage/table/chunk_info.hpp"

namespace duckdb {

CommitState::CommitState(transaction_t commit_id, WriteAheadLog *log)
    : log(log), commit_id(commit_id), current_table_info(nullptr) {
}

void CommitState::SwitchTable(DataTableInfo *table_info, UndoFlags new_op) {
	if (current_table_info != table_info) {
		// write the current table to the log
		log->WriteSetTable(table_info->schema, table_info->table);
		current_table_info = table_info;
	}
}

void CommitState::WriteCatalogEntry(CatalogEntry *entry, data_ptr_t dataptr) {
	if (entry->temporary || entry->parent->temporary) {
		return;
	}
	D_ASSERT(log);
	// look at the type of the parent entry
	auto parent = entry->parent;
	switch (parent->type) {
	case CatalogType::TABLE_ENTRY:
		if (entry->type == CatalogType::TABLE_ENTRY) {
			auto table_entry = (TableCatalogEntry *)entry;
			// ALTER TABLE statement, read the extra data after the entry
			auto extra_data_size = Load<idx_t>(dataptr);
			auto extra_data = (data_ptr_t)(dataptr + sizeof(idx_t));
			// deserialize it
			BufferedDeserializer source(extra_data, extra_data_size);
			auto info = AlterInfo::Deserialize(source);
			// write the alter table in the log
			table_entry->CommitAlter(*info);
			log->WriteAlter(*info);
		} else {
			// CREATE TABLE statement
			log->WriteCreateTable((TableCatalogEntry *)parent);
		}
		break;
	case CatalogType::SCHEMA_ENTRY:
		if (entry->type == CatalogType::SCHEMA_ENTRY) {
			// ALTER TABLE statement, skip it
			return;
		}
		log->WriteCreateSchema((SchemaCatalogEntry *)parent);
		break;
	case CatalogType::VIEW_ENTRY:
		if (entry->type == CatalogType::VIEW_ENTRY) {
			// ALTER TABLE statement, read the extra data after the entry
			auto extra_data_size = Load<idx_t>(dataptr);
			auto extra_data = (data_ptr_t)(dataptr + sizeof(idx_t));
			// deserialize it
			BufferedDeserializer source(extra_data, extra_data_size);
			auto info = AlterInfo::Deserialize(source);
			// write the alter table in the log
			log->WriteAlter(*info);
		} else {
			log->WriteCreateView((ViewCatalogEntry *)parent);
		}
		break;
	case CatalogType::SEQUENCE_ENTRY:
		log->WriteCreateSequence((SequenceCatalogEntry *)parent);
		break;
	case CatalogType::MACRO_ENTRY:
		log->WriteCreateMacro((MacroCatalogEntry *)parent);
		break;
	case CatalogType::DELETED_ENTRY:
		switch (entry->type) {
		case CatalogType::TABLE_ENTRY: {
			auto table_entry = (TableCatalogEntry *)entry;
			table_entry->CommitDrop();
			log->WriteDropTable(table_entry);
			break;
		}
		case CatalogType::SCHEMA_ENTRY:
			log->WriteDropSchema((SchemaCatalogEntry *)entry);
			break;
		case CatalogType::VIEW_ENTRY:
			log->WriteDropView((ViewCatalogEntry *)entry);
			break;
		case CatalogType::SEQUENCE_ENTRY:
			log->WriteDropSequence((SequenceCatalogEntry *)entry);
			break;
		case CatalogType::MACRO_ENTRY:
			log->WriteDropMacro((MacroCatalogEntry *)entry);
			break;
		case CatalogType::INDEX_ENTRY:
		case CatalogType::PREPARED_STATEMENT:
		case CatalogType::SCALAR_FUNCTION_ENTRY:
			// do nothing, indexes/prepared statements/functions aren't persisted to disk
			break;
		default:
			throw InternalException("Don't know how to drop this type!");
		}
		break;
	case CatalogType::INDEX_ENTRY:
	case CatalogType::PREPARED_STATEMENT:
	case CatalogType::AGGREGATE_FUNCTION_ENTRY:
	case CatalogType::SCALAR_FUNCTION_ENTRY:
	case CatalogType::TABLE_FUNCTION_ENTRY:
	case CatalogType::COPY_FUNCTION_ENTRY:
	case CatalogType::PRAGMA_FUNCTION_ENTRY:
	case CatalogType::COLLATION_ENTRY:
		// do nothing, these entries are not persisted to disk
		break;
	default:
		throw InternalException("UndoBuffer - don't know how to write this entry to the WAL");
	}
}

void CommitState::WriteDelete(DeleteInfo *info) {
	D_ASSERT(log);
	// switch to the current table, if necessary
	SwitchTable(info->table->info.get(), UndoFlags::DELETE_TUPLE);

	if (!delete_chunk) {
		delete_chunk = make_unique<DataChunk>();
		vector<LogicalType> delete_types = {LOGICAL_ROW_TYPE};
		delete_chunk->Initialize(delete_types);
	}
	auto rows = FlatVector::GetData<row_t>(delete_chunk->data[0]);
	for (idx_t i = 0; i < info->count; i++) {
		rows[i] = info->base_row + info->rows[i];
	}
	delete_chunk->SetCardinality(info->count);
	log->WriteDelete(*delete_chunk);
}

void CommitState::WriteUpdate(UpdateInfo *info) {
	D_ASSERT(log);
	// switch to the current table, if necessary
	auto &column_data = info->segment->column_data;
	auto &table_info = column_data.GetTableInfo();

	SwitchTable(&table_info, UndoFlags::UPDATE_TUPLE);

	// initialize the update chunk
	vector<LogicalType> update_types;
	if (column_data.type.id() == LogicalTypeId::VALIDITY) {
		update_types.push_back(LogicalType::BOOLEAN);
	} else {
		update_types.push_back(column_data.type);
	}
	update_types.push_back(LOGICAL_ROW_TYPE);

	update_chunk = make_unique<DataChunk>();
	update_chunk->Initialize(update_types);

	// fetch the updated values from the base segment
	info->segment->FetchCommitted(info->vector_index, update_chunk->data[0]);

	// write the row ids into the chunk
	auto row_ids = FlatVector::GetData<row_t>(update_chunk->data[1]);
	idx_t start = column_data.start + info->vector_index * STANDARD_VECTOR_SIZE;
	for (idx_t i = 0; i < info->N; i++) {
		row_ids[info->tuples[i]] = start + info->tuples[i];
	}
	if (column_data.type.id() == LogicalTypeId::VALIDITY) {
		// zero-initialize the booleans
		// FIXME: this is only required because of NullValue<T> in Vector::Serialize...
		auto booleans = FlatVector::GetData<bool>(update_chunk->data[0]);
		for (idx_t i = 0; i < info->N; i++) {
			auto idx = info->tuples[i];
			booleans[idx] = false;
		}
	}
	SelectionVector sel(info->tuples);
	update_chunk->Slice(sel, info->N);

	// construct the column index path
	vector<column_t> column_indexes;
	auto column_data_ptr = &column_data;
	while (column_data_ptr->parent) {
		column_indexes.push_back(column_data_ptr->column_index);
		column_data_ptr = column_data_ptr->parent;
	}
	column_indexes.push_back(info->column_index);
	std::reverse(column_indexes.begin(), column_indexes.end());

	log->WriteUpdate(*update_chunk, column_indexes);
}

template <bool HAS_LOG>
void CommitState::CommitEntry(UndoFlags type, data_ptr_t data) {
	switch (type) {
	case UndoFlags::CATALOG_ENTRY: {
		// set the commit timestamp of the catalog entry to the given id
		auto catalog_entry = Load<CatalogEntry *>(data);
		D_ASSERT(catalog_entry->parent);
		catalog_entry->set->UpdateTimestamp(catalog_entry->parent, commit_id);
		if (catalog_entry->name != catalog_entry->parent->name) {
			catalog_entry->set->UpdateTimestamp(catalog_entry, commit_id);
		}
		if (HAS_LOG) {
			// push the catalog update to the WAL
			WriteCatalogEntry(catalog_entry, data + sizeof(CatalogEntry *));
		}
		break;
	}
	case UndoFlags::INSERT_TUPLE: {
		// append:
		auto info = (AppendInfo *)data;
		if (HAS_LOG && !info->table->info->IsTemporary()) {
			info->table->WriteToLog(*log, info->start_row, info->count);
		}
		// mark the tuples as committed
		info->table->CommitAppend(commit_id, info->start_row, info->count);
		break;
	}
	case UndoFlags::DELETE_TUPLE: {
		// deletion:
		auto info = (DeleteInfo *)data;
		if (HAS_LOG && !info->table->info->IsTemporary()) {
			WriteDelete(info);
		}
		// mark the tuples as committed
		info->vinfo->CommitDelete(commit_id, info->rows, info->count);
		break;
	}
	case UndoFlags::UPDATE_TUPLE: {
		// update:
		auto info = (UpdateInfo *)data;
		if (HAS_LOG && !info->segment->column_data.GetTableInfo().IsTemporary()) {
			WriteUpdate(info);
		}
		info->version_number = commit_id;
		break;
	}
	default:
		throw InternalException("UndoBuffer - don't know how to commit this type!");
	}
}

void CommitState::RevertCommit(UndoFlags type, data_ptr_t data) {
	transaction_t transaction_id = commit_id;
	switch (type) {
	case UndoFlags::CATALOG_ENTRY: {
		// set the commit timestamp of the catalog entry to the given id
		auto catalog_entry = Load<CatalogEntry *>(data);
		D_ASSERT(catalog_entry->parent);
		catalog_entry->set->UpdateTimestamp(catalog_entry->parent, transaction_id);
		if (catalog_entry->name != catalog_entry->parent->name) {
			catalog_entry->set->UpdateTimestamp(catalog_entry, transaction_id);
		}
		break;
	}
	case UndoFlags::INSERT_TUPLE: {
		auto info = (AppendInfo *)data;
		// revert this append
		info->table->RevertAppend(info->start_row, info->count);
		break;
	}
	case UndoFlags::DELETE_TUPLE: {
		// deletion:
		auto info = (DeleteInfo *)data;
		info->table->info->cardinality += info->count;
		// revert the commit by writing the (uncommitted) transaction_id back into the version info
		info->vinfo->CommitDelete(transaction_id, info->rows, info->count);
		break;
	}
	case UndoFlags::UPDATE_TUPLE: {
		// update:
		auto info = (UpdateInfo *)data;
		info->version_number = transaction_id;
		break;
	}
	default:
		throw InternalException("UndoBuffer - don't know how to revert commit of this type!");
	}
}

template void CommitState::CommitEntry<true>(UndoFlags type, data_ptr_t data);
template void CommitState::CommitEntry<false>(UndoFlags type, data_ptr_t data);

} // namespace duckdb
