#include "duckdb/transaction/rollback_state.hpp"
#include "duckdb/transaction/append_info.hpp"
#include "duckdb/transaction/delete_info.hpp"
#include "duckdb/transaction/update_info.hpp"

#include "duckdb/storage/uncompressed_segment.hpp"
#include "duckdb/storage/table/chunk_info.hpp"

#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/catalog/catalog_set.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/update_segment.hpp"

namespace duckdb {

void RollbackState::RollbackEntry(UndoFlags type, data_ptr_t data) {
	switch (type) {
	case UndoFlags::CATALOG_ENTRY: {
		// undo this catalog entry
		auto catalog_entry = Load<CatalogEntry *>(data);
		D_ASSERT(catalog_entry->set);
		catalog_entry->set->Undo(catalog_entry);
		break;
	}
	case UndoFlags::INSERT_TUPLE: {
		auto info = (AppendInfo *)data;
		// revert the append in the base table
		info->table->RevertAppend(info->start_row, info->count);
		break;
	}
	case UndoFlags::DELETE_TUPLE: {
		auto info = (DeleteInfo *)data;
		// reset the deleted flag on rollback
		info->vinfo->CommitDelete(NOT_DELETED_ID, info->rows, info->count);
		break;
	}
	case UndoFlags::UPDATE_TUPLE: {
		auto info = (UpdateInfo *)data;
		info->segment->RollbackUpdate(info);
		break;
	}
	default: // LCOV_EXCL_START
		D_ASSERT(type == UndoFlags::EMPTY_ENTRY);
		break;
	} // LCOV_EXCL_STOP
}

} // namespace duckdb
