#include "duckdb/transaction/wal_write_state.hpp"

#include "duckdb/catalog/catalog_entry/duck_index_entry.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/scalar_macro_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/trigger_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/type_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/catalog/catalog_set.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/chunk_info.hpp"
#include "duckdb/storage/table/column_data.hpp"
#include "duckdb/storage/table/data_table_info.hpp"
#include "duckdb/storage/table/row_group.hpp"
#include "duckdb/storage/table/row_version_manager.hpp"
#include "duckdb/storage/table/update_segment.hpp"
#include "duckdb/storage/write_ahead_log.hpp"
#include "duckdb/transaction/append_info.hpp"
#include "duckdb/transaction/delete_info.hpp"
#include "duckdb/transaction/update_info.hpp"

namespace duckdb {

WALWriteState::WALWriteState(DuckTransaction &transaction_p, WriteAheadLog &log,
                             optional_ptr<StorageCommitState> commit_state)
    : transaction(transaction_p), log(log), commit_state(commit_state), current_table_entry(nullptr) {
}

void WALWriteState::SwitchTable(DuckTableEntry &table_entry, UndoFlags new_op) {
	if (current_table_entry.get() != &table_entry) {
		// write the current table to the log
		log.WriteSetTable(QualifiedName(table_entry.schema.GetSchemaPath(), table_entry.name));
		current_table_entry = table_entry;
	}
}

void WALWriteState::WriteCatalogEntry(CatalogEntry &entry, data_ptr_t dataptr) {
	if (entry.temporary || entry.Parent().temporary) {
		return;
	}

	// look at the type of the parent entry
	auto &parent = entry.Parent();

	switch (parent.type) {
	case CatalogType::TRIGGER_ENTRY:
		D_ASSERT(entry.type != CatalogType::RENAMED_ENTRY);
		if (entry.type == parent.type) {
			// Column-rename propagation from the owning ALTER TABLE — the ALTER_INFO record
			// already covers this on replay; writing a second CREATE_TRIGGER would be redundant.
			return;
		}
		log.WriteCreateTrigger(parent.Cast<TriggerCatalogEntry>());
		break;
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

			auto &alter_info = parse_info->Cast<AlterInfo>();
			log.WriteAlter(entry, alter_info);
		} else {
			switch (parent.type) {
			case CatalogType::TABLE_ENTRY:
				// CREATE TABLE statement
				log.WriteCreateTable(parent.Cast<TableCatalogEntry>());
				break;
			case CatalogType::VIEW_ENTRY:
				// CREATE VIEW statement
				log.WriteCreateView(parent.Cast<ViewCatalogEntry>());
				break;
			case CatalogType::INDEX_ENTRY:
				// CREATE INDEX statement
				log.WriteCreateIndex(parent.Cast<IndexCatalogEntry>());
				break;
			case CatalogType::SEQUENCE_ENTRY:
				// CREATE SEQUENCE statement
				log.WriteCreateSequence(parent.Cast<SequenceCatalogEntry>());
				break;
			case CatalogType::TYPE_ENTRY:
				// CREATE TYPE statement
				log.WriteCreateType(parent.Cast<TypeCatalogEntry>());
				break;
			case CatalogType::MACRO_ENTRY:
				log.WriteCreateMacro(parent.Cast<ScalarMacroCatalogEntry>());
				break;
			case CatalogType::TABLE_MACRO_ENTRY:
				log.WriteCreateTableMacro(parent.Cast<TableMacroCatalogEntry>());
				break;
			default:
				throw InternalException("Don't know how to create this type!");
			}
		}
		break;
	case CatalogType::SCHEMA_ENTRY:
		if (entry.type == CatalogType::RENAMED_ENTRY || entry.type == CatalogType::SCHEMA_ENTRY) {
			// ALTER TABLE statement, skip it
			return;
		}
		log.WriteCreateSchema(parent.Cast<SchemaCatalogEntry>());
		break;
	case CatalogType::RENAMED_ENTRY:
		// This is a rename, nothing needs to be done for this
		break;
	case CatalogType::DELETED_ENTRY:
		switch (entry.type) {
		case CatalogType::TABLE_ENTRY: {
			auto &table_entry = entry.Cast<DuckTableEntry>();
			D_ASSERT(table_entry.IsDuckTable());
			log.WriteDropTable(table_entry);
			break;
		}
		case CatalogType::SCHEMA_ENTRY:
			log.WriteDropSchema(entry.Cast<SchemaCatalogEntry>());
			break;
		case CatalogType::VIEW_ENTRY:
			log.WriteDropView(entry.Cast<ViewCatalogEntry>());
			break;
		case CatalogType::SEQUENCE_ENTRY:
			log.WriteDropSequence(entry.Cast<SequenceCatalogEntry>());
			break;
		case CatalogType::MACRO_ENTRY:
			log.WriteDropMacro(entry.Cast<ScalarMacroCatalogEntry>());
			break;
		case CatalogType::TABLE_MACRO_ENTRY:
			log.WriteDropTableMacro(entry.Cast<TableMacroCatalogEntry>());
			break;
		case CatalogType::TYPE_ENTRY:
			log.WriteDropType(entry.Cast<TypeCatalogEntry>());
			break;
		case CatalogType::INDEX_ENTRY: {
			log.WriteDropIndex(entry.Cast<IndexCatalogEntry>());
			break;
		}
		case CatalogType::TRIGGER_ENTRY:
			log.WriteDropTrigger(entry.Cast<TriggerCatalogEntry>());
			break;
		case CatalogType::RENAMED_ENTRY:
		case CatalogType::PREPARED_STATEMENT:
		case CatalogType::SCALAR_FUNCTION_ENTRY:
		case CatalogType::DEPENDENCY_ENTRY:
		case CatalogType::SECRET_ENTRY:
		case CatalogType::SECRET_TYPE_ENTRY:
		case CatalogType::SECRET_FUNCTION_ENTRY:
			// do nothing, prepared statements and scalar functions aren't persisted to disk
			break;
		default:
			throw InternalException("Don't know how to drop this type!");
		}
		break;
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

void WALWriteState::WriteDelete(DeleteInfo &info) {
	// switch to the current table, if necessary
	SwitchTable(*info.table, UndoFlags::DELETE_TUPLE);

	if (!delete_chunk) {
		delete_chunk = make_uniq<DataChunk>();
		vector<LogicalType> delete_types = {LogicalType::ROW_TYPE};
		delete_chunk->Initialize(Allocator::DefaultAllocator(), delete_types);
	}
	auto rows = FlatVector::GetDataMutable<row_t>(delete_chunk->data[0]);
	if (info.is_consecutive) {
		for (idx_t i = 0; i < info.count; i++) {
			rows[i] = UnsafeNumericCast<int64_t>(info.base_row + i);
		}
	} else {
		auto delete_rows = info.GetRows();
		for (idx_t i = 0; i < info.count; i++) {
			rows[i] = UnsafeNumericCast<int64_t>(info.base_row) + delete_rows[i];
		}
	}
	delete_chunk->SetChildCardinality(info.count);
	log.WriteDelete(*delete_chunk);
}

void WALWriteState::WriteUpdate(UpdateInfo &info) {
	// switch to the current table, if necessary
	auto &column_data = info.segment->column_data;

	SwitchTable(*info.table, UndoFlags::UPDATE_TUPLE);

	// initialize the update chunk
	vector<LogicalType> update_types;
	if (column_data.type.id() == LogicalTypeId::VALIDITY) {
		update_types.emplace_back(LogicalType::BOOLEAN);
	} else {
		update_types.push_back(column_data.type);
	}
	update_types.emplace_back(LogicalType::ROW_TYPE);

	update_chunk = make_uniq<DataChunk>();
	update_chunk->Initialize(Allocator::DefaultAllocator(), update_types);

	// fetch the updated values from the base segment
	info.segment->FetchCommitted(info.vector_index, update_chunk->data[0]);

	// write the row ids into the chunk
	auto row_ids = FlatVector::GetDataMutable<row_t>(update_chunk->data[1]);
	idx_t start = info.row_group_start + info.vector_index * STANDARD_VECTOR_SIZE;
	auto tuples = info.GetTuples();
	for (idx_t i = 0; i < info.N; i++) {
		row_ids[tuples[i]] = UnsafeNumericCast<int64_t>(start + tuples[i]);
	}
	if (column_data.type.id() == LogicalTypeId::VALIDITY) {
		// zero-initialize the booleans
		// FIXME: this is only required because of NullValue<T> in Vector::Serialize...
		auto booleans = FlatVector::GetDataMutable<bool>(update_chunk->data[0]);
		for (idx_t i = 0; i < info.N; i++) {
			auto idx = tuples[i];
			booleans[idx] = false;
		}
	}
	SelectionVector sel(tuples, info.N);
	update_chunk->Slice(sel, info.N);

	// construct the column index path
	vector<column_t> column_indexes;
	reference<const ColumnData> current_column_data = column_data;
	while (current_column_data.get().HasParent()) {
		column_indexes.push_back(current_column_data.get().column_index);
		current_column_data = current_column_data.get().Parent();
	}
	column_indexes.push_back(info.column_index);
	std::reverse(column_indexes.begin(), column_indexes.end());

	log.WriteUpdate(*update_chunk, column_indexes);
}

void WALWriteState::CommitEntry(UndoFlags type, data_ptr_t data) {
	switch (type) {
	case UndoFlags::CATALOG_ENTRY: {
		// set the commit timestamp of the catalog entry to the given id
		auto catalog_entry = Load<CatalogEntry *>(data);
		D_ASSERT(catalog_entry->HasParent());
		// push the catalog update to the WAL
		WriteCatalogEntry(*catalog_entry, data + sizeof(CatalogEntry *));
		break;
	}
	case UndoFlags::INSERT_TUPLE: {
		// append:
		auto info = reinterpret_cast<AppendInfo *>(data);
		if (!info->table->GetStorage().IsTemporary()) {
			info->table->GetStorage().WriteToLog(transaction, log, info->start_row, info->count, commit_state.get());
		}
		break;
	}
	case UndoFlags::DELETE_TUPLE: {
		// deletion:
		auto info = reinterpret_cast<DeleteInfo *>(data);
		if (!info->table->GetStorage().IsTemporary()) {
			WriteDelete(*info);
		}
		break;
	}
	case UndoFlags::UPDATE_TUPLE: {
		// update:
		auto info = reinterpret_cast<UpdateInfo *>(data);
		// update infos without updates were replaced by a larger update info
		if (info->N > 0 && !info->segment->column_data.GetTableInfo().IsTemporary()) {
			WriteUpdate(*info);
		}
		break;
	}
	case UndoFlags::ATTACHED_DATABASE:
	case UndoFlags::UPDATE_ROWS:
		break;
	case UndoFlags::SEQUENCE_VALUE: {
		auto info = reinterpret_cast<SequenceValue *>(data);
		log.WriteSequenceValue(*info);
		break;
	}
	default:
		throw InternalException("UndoBuffer - don't know how to commit this type!");
	}
}

} // namespace duckdb
