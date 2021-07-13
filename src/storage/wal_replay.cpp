#include "duckdb/storage/write_ahead_log.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/common/serializer/buffered_file_reader.hpp"
#include "duckdb/catalog/catalog_entry/macro_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {
class ReplayState {
public:
	ReplayState(DatabaseInstance &db, ClientContext &context, Deserializer &source)
	    : db(db), context(context), source(source), current_table(nullptr), deserialize_only(false),
	      checkpoint_id(INVALID_BLOCK) {
	}

	DatabaseInstance &db;
	ClientContext &context;
	Deserializer &source;
	TableCatalogEntry *current_table;
	bool deserialize_only;
	block_id_t checkpoint_id;

public:
	void ReplayEntry(WALType entry_type);

private:
	void ReplayCreateTable();
	void ReplayDropTable();
	void ReplayAlter();

	void ReplayCreateView();
	void ReplayDropView();

	void ReplayCreateSchema();
	void ReplayDropSchema();

	void ReplayCreateSequence();
	void ReplayDropSequence();
	void ReplaySequenceValue();

	void ReplayCreateMacro();
	void ReplayDropMacro();

	void ReplayUseTable();
	void ReplayInsert();
	void ReplayDelete();
	void ReplayUpdate();
	void ReplayCheckpoint();
};

bool WriteAheadLog::Replay(DatabaseInstance &database, string &path) {
	auto initial_reader = make_unique<BufferedFileReader>(database.GetFileSystem(), path.c_str());
	if (initial_reader->Finished()) {
		// WAL is empty
		return false;
	}
	Connection con(database);
	con.BeginTransaction();

	// first deserialize the WAL to look for a checkpoint flag
	// if there is a checkpoint flag, we might have already flushed the contents of the WAL to disk
	ReplayState checkpoint_state(database, *con.context, *initial_reader);
	checkpoint_state.deserialize_only = true;
	try {
		while (true) {
			// read the current entry
			WALType entry_type = initial_reader->Read<WALType>();
			if (entry_type == WALType::WAL_FLUSH) {
				// check if the file is exhausted
				if (initial_reader->Finished()) {
					// we finished reading the file: break
					break;
				}
			} else {
				// replay the entry
				checkpoint_state.ReplayEntry(entry_type);
			}
		}
	} catch (std::exception &ex) { // LCOV_EXCL_START
		Printer::Print(StringUtil::Format("Exception in WAL playback during initial read: %s\n", ex.what()));
		return false;
	} catch (...) {
		Printer::Print("Unknown Exception in WAL playback during initial read");
		return false;
	} // LCOV_EXCL_STOP
	initial_reader.reset();
	if (checkpoint_state.checkpoint_id != INVALID_BLOCK) {
		// there is a checkpoint flag: check if we need to deserialize the WAL
		auto &manager = BlockManager::GetBlockManager(database);
		if (manager.IsRootBlock(checkpoint_state.checkpoint_id)) {
			// the contents of the WAL have already been checkpointed
			// we can safely truncate the WAL and ignore its contents
			return true;
		}
	}

	// we need to recover from the WAL: actually set up the replay state
	BufferedFileReader reader(database.GetFileSystem(), path.c_str());
	ReplayState state(database, *con.context, reader);

	// replay the WAL
	// note that everything is wrapped inside a try/catch block here
	// there can be errors in WAL replay because of a corrupt WAL file
	// in this case we should throw a warning but startup anyway
	try {
		while (true) {
			// read the current entry
			WALType entry_type = reader.Read<WALType>();
			if (entry_type == WALType::WAL_FLUSH) {
				// flush: commit the current transaction
				con.Commit();
				// check if the file is exhausted
				if (reader.Finished()) {
					// we finished reading the file: break
					break;
				}
				// otherwise we keep on reading
				con.BeginTransaction();
			} else {
				// replay the entry
				state.ReplayEntry(entry_type);
			}
		}
	} catch (std::exception &ex) { // LCOV_EXCL_START
		// FIXME: this should report a proper warning in the connection
		Printer::Print(StringUtil::Format("Exception in WAL playback: %s\n", ex.what()));
		// exception thrown in WAL replay: rollback
		con.Rollback();
	} catch (...) {
		Printer::Print("Unknown Exception in WAL playback: %s\n");
		// exception thrown in WAL replay: rollback
		con.Rollback();
	} // LCOV_EXCL_STOP
	return false;
}

//===--------------------------------------------------------------------===//
// Replay Entries
//===--------------------------------------------------------------------===//
void ReplayState::ReplayEntry(WALType entry_type) {
	switch (entry_type) {
	case WALType::CREATE_TABLE:
		ReplayCreateTable();
		break;
	case WALType::DROP_TABLE:
		ReplayDropTable();
		break;
	case WALType::ALTER_INFO:
		ReplayAlter();
		break;
	case WALType::CREATE_VIEW:
		ReplayCreateView();
		break;
	case WALType::DROP_VIEW:
		ReplayDropView();
		break;
	case WALType::CREATE_SCHEMA:
		ReplayCreateSchema();
		break;
	case WALType::DROP_SCHEMA:
		ReplayDropSchema();
		break;
	case WALType::CREATE_SEQUENCE:
		ReplayCreateSequence();
		break;
	case WALType::DROP_SEQUENCE:
		ReplayDropSequence();
		break;
	case WALType::SEQUENCE_VALUE:
		ReplaySequenceValue();
		break;
	case WALType::CREATE_MACRO:
		ReplayCreateMacro();
		break;
	case WALType::DROP_MACRO:
		ReplayDropMacro();
		break;
	case WALType::USE_TABLE:
		ReplayUseTable();
		break;
	case WALType::INSERT_TUPLE:
		ReplayInsert();
		break;
	case WALType::DELETE_TUPLE:
		ReplayDelete();
		break;
	case WALType::UPDATE_TUPLE:
		ReplayUpdate();
		break;
	case WALType::CHECKPOINT:
		ReplayCheckpoint();
		break;
	default:
		throw InternalException("Invalid WAL entry type!");
	}
}

//===--------------------------------------------------------------------===//
// Replay Table
//===--------------------------------------------------------------------===//
void ReplayState::ReplayCreateTable() {
	auto info = TableCatalogEntry::Deserialize(source);
	if (deserialize_only) {
		return;
	}

	// bind the constraints to the table again
	auto binder = Binder::CreateBinder(context);
	auto bound_info = binder->BindCreateTableInfo(move(info));

	auto &catalog = Catalog::GetCatalog(context);
	catalog.CreateTable(context, bound_info.get());
}

void ReplayState::ReplayDropTable() {
	DropInfo info;

	info.type = CatalogType::TABLE_ENTRY;
	info.schema = source.Read<string>();
	info.name = source.Read<string>();
	if (deserialize_only) {
		return;
	}

	auto &catalog = Catalog::GetCatalog(context);
	catalog.DropEntry(context, &info);
}

void ReplayState::ReplayAlter() {
	auto info = AlterInfo::Deserialize(source);
	if (deserialize_only) {
		return;
	}
	auto &catalog = Catalog::GetCatalog(context);
	catalog.Alter(context, info.get());
}

//===--------------------------------------------------------------------===//
// Replay View
//===--------------------------------------------------------------------===//
void ReplayState::ReplayCreateView() {
	auto entry = ViewCatalogEntry::Deserialize(source);
	if (deserialize_only) {
		return;
	}

	auto &catalog = Catalog::GetCatalog(context);
	catalog.CreateView(context, entry.get());
}

void ReplayState::ReplayDropView() {
	DropInfo info;
	info.type = CatalogType::VIEW_ENTRY;
	info.schema = source.Read<string>();
	info.name = source.Read<string>();
	if (deserialize_only) {
		return;
	}
	auto &catalog = Catalog::GetCatalog(context);
	catalog.DropEntry(context, &info);
}

//===--------------------------------------------------------------------===//
// Replay Schema
//===--------------------------------------------------------------------===//
void ReplayState::ReplayCreateSchema() {
	CreateSchemaInfo info;
	info.schema = source.Read<string>();
	if (deserialize_only) {
		return;
	}

	auto &catalog = Catalog::GetCatalog(context);
	catalog.CreateSchema(context, &info);
}

void ReplayState::ReplayDropSchema() {
	DropInfo info;

	info.type = CatalogType::SCHEMA_ENTRY;
	info.name = source.Read<string>();
	if (deserialize_only) {
		return;
	}

	auto &catalog = Catalog::GetCatalog(context);
	catalog.DropEntry(context, &info);
}

//===--------------------------------------------------------------------===//
// Replay Sequence
//===--------------------------------------------------------------------===//
void ReplayState::ReplayCreateSequence() {
	auto entry = SequenceCatalogEntry::Deserialize(source);
	if (deserialize_only) {
		return;
	}

	auto &catalog = Catalog::GetCatalog(context);
	catalog.CreateSequence(context, entry.get());
}

void ReplayState::ReplayDropSequence() {
	DropInfo info;
	info.type = CatalogType::SEQUENCE_ENTRY;
	info.schema = source.Read<string>();
	info.name = source.Read<string>();
	if (deserialize_only) {
		return;
	}

	auto &catalog = Catalog::GetCatalog(context);
	catalog.DropEntry(context, &info);
}

void ReplayState::ReplaySequenceValue() {
	auto schema = source.Read<string>();
	auto name = source.Read<string>();
	auto usage_count = source.Read<uint64_t>();
	auto counter = source.Read<int64_t>();
	if (deserialize_only) {
		return;
	}

	// fetch the sequence from the catalog
	auto &catalog = Catalog::GetCatalog(context);
	auto seq = catalog.GetEntry<SequenceCatalogEntry>(context, schema, name);
	if (usage_count > seq->usage_count) {
		seq->usage_count = usage_count;
		seq->counter = counter;
	}
}

//===--------------------------------------------------------------------===//
// Replay Macro
//===--------------------------------------------------------------------===//
void ReplayState::ReplayCreateMacro() {
	auto entry = MacroCatalogEntry::Deserialize(source);
	if (deserialize_only) {
		return;
	}

	auto &catalog = Catalog::GetCatalog(context);
	catalog.CreateFunction(context, entry.get());
}

void ReplayState::ReplayDropMacro() {
	DropInfo info;
	info.type = CatalogType::MACRO_ENTRY;
	info.schema = source.Read<string>();
	info.name = source.Read<string>();
	if (deserialize_only) {
		return;
	}

	auto &catalog = Catalog::GetCatalog(context);
	catalog.DropEntry(context, &info);
}

//===--------------------------------------------------------------------===//
// Replay Data
//===--------------------------------------------------------------------===//
void ReplayState::ReplayUseTable() {
	auto schema_name = source.Read<string>();
	auto table_name = source.Read<string>();
	if (deserialize_only) {
		return;
	}
	auto &catalog = Catalog::GetCatalog(context);
	current_table = catalog.GetEntry<TableCatalogEntry>(context, schema_name, table_name);
}

void ReplayState::ReplayInsert() {
	DataChunk chunk;
	chunk.Deserialize(source);
	if (deserialize_only) {
		return;
	}
	if (!current_table) {
		throw Exception("Corrupt WAL: insert without table");
	}

	// append to the current table
	current_table->storage->Append(*current_table, context, chunk);
}

void ReplayState::ReplayDelete() {
	DataChunk chunk;
	chunk.Deserialize(source);
	if (deserialize_only) {
		return;
	}
	if (!current_table) {
		throw InternalException("Corrupt WAL: delete without table");
	}

	D_ASSERT(chunk.ColumnCount() == 1 && chunk.data[0].GetType() == LOGICAL_ROW_TYPE);
	row_t row_ids[1];
	Vector row_identifiers(LOGICAL_ROW_TYPE, (data_ptr_t)row_ids);

	auto source_ids = FlatVector::GetData<row_t>(chunk.data[0]);
	// delete the tuples from the current table
	for (idx_t i = 0; i < chunk.size(); i++) {
		row_ids[0] = source_ids[i];
		current_table->storage->Delete(*current_table, context, row_identifiers, 1);
	}
}

void ReplayState::ReplayUpdate() {
	vector<column_t> column_path;
	auto column_index_count = source.Read<idx_t>();
	column_path.reserve(column_index_count);
	for (idx_t i = 0; i < column_index_count; i++) {
		column_path.push_back(source.Read<column_t>());
	}
	DataChunk chunk;
	chunk.Deserialize(source);
	if (deserialize_only) {
		return;
	}
	if (!current_table) {
		throw InternalException("Corrupt WAL: update without table");
	}

	if (column_path[0] >= current_table->columns.size()) {
		throw InternalException("Corrupt WAL: column index for update out of bounds");
	}

	// remove the row id vector from the chunk
	auto row_ids = move(chunk.data.back());
	chunk.data.pop_back();

	// now perform the update
	current_table->storage->UpdateColumn(*current_table, context, row_ids, column_path, chunk);
}

void ReplayState::ReplayCheckpoint() {
	checkpoint_id = source.Read<block_id_t>();
}

} // namespace duckdb
