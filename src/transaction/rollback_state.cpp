#include "duckdb/transaction/rollback_state.hpp"
#include "duckdb/transaction/append_info.hpp"
#include "duckdb/transaction/delete_info.hpp"
#include "duckdb/transaction/update_info.hpp"

#include "duckdb/storage/table/chunk_info.hpp"

#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_set.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/update_segment.hpp"
#include "duckdb/storage/table/row_version_manager.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/external_resources_manager.hpp"
#include "duckdb/main/database_manager.hpp"

namespace duckdb {

RollbackState::RollbackState(DuckTransaction &transaction_p) : transaction(transaction_p) {
}

void RollbackState::RollbackEntry(UndoFlags type, data_ptr_t data) {
	switch (type) {
	case UndoFlags::CATALOG_ENTRY: {
		// Load and undo the catalog entry.
		auto catalog_entry = Load<CatalogEntry *>(data);
		D_ASSERT(catalog_entry->set);
		catalog_entry->set->Undo(*catalog_entry);
		break;
	}
	case UndoFlags::INSERT_TUPLE: {
		auto info = reinterpret_cast<AppendInfo *>(data);
		// revert the append in the base table
		info->table->GetStorage().RevertAppend(transaction, info->start_row, info->count);
		break;
	}
	case UndoFlags::DELETE_TUPLE:
	case UndoFlags::UPDATE_ROWS: {
		auto info = reinterpret_cast<DeleteInfo *>(data);
		// the rows fall back to the ids they held before the delete or update
		info->version_info->RollbackWrite(info->vector_idx, info->GetStoredRows(), info->count, info->GetPreviousIds(),
		                                  info->previous_id_count);
		break;
	}
	case UndoFlags::UPDATE_TUPLE: {
		auto info = reinterpret_cast<UpdateInfo *>(data);
		info->segment->RollbackUpdate(*info);
		break;
	}
	case UndoFlags::ATTACHED_DATABASE: {
		auto db = Load<AttachedDatabase *>(data);
		auto &db_manager = DatabaseManager::Get(db->GetDatabase());
		// after this transaction detached it, another connection can have attached a database under the name
		auto attached_db = db_manager.DetachInternal(db->name, db);
		if (attached_db) {
			// Teardown runs SQL, which cannot happen under the transaction lock: it would need a
			// transaction on the same manager, and rollback may not fail. Extracting the deleter is a
			// field move, safe here; the DatabaseManager drains the queue once rollback completes.
			auto deleter = attached_db->ExtractDeleter();
			if (deleter) {
				db_manager.AddPendingTeardown(std::move(deleter));
			}
		}
		break;
	}
	case UndoFlags::SEQUENCE_VALUE:
		break;
	default: // LCOV_EXCL_START
		D_ASSERT(type == UndoFlags::EMPTY_ENTRY);
		break;
	} // LCOV_EXCL_STOP
}

} // namespace duckdb
