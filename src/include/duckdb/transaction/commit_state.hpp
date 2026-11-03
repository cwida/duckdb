//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/transaction/commit_state.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/transaction/undo_buffer.hpp"
#include "duckdb/common/vector_size.hpp"
#include "duckdb/common/enums/index_removal_type.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/storage/block.hpp"
#include "duckdb/common/types/data_chunk.hpp"

namespace duckdb {
class BlockManager;
class CatalogEntry;
class TableIndexList;
class IndexEntry;
class DataChunk;
class DuckTransaction;
class WriteAheadLog;
class ClientContext;

struct DataTableInfo;
class DataTable;
struct DeleteInfo;
struct UpdateInfo;

enum class CommitMode { COMMIT, REVERT_COMMIT };

//! An index that a commit drops, which is detached from its table's index list once the commit chain succeeds.
struct DroppedIndex {
	shared_ptr<DataTableInfo> info;
	idx_t index_oid;
	//! The detached index entry, destroyed when the dropped storage is freed
	shared_ptr<IndexEntry> detached_entry;
};

//! Accumulates the storage a commit drops, which is freed once the commit can no longer be reverted
class CommitDropState {
public:
	explicit CommitDropState(optional_ptr<BlockManager> block_manager);

public:
	//! Register an on-disk block, which Free marks as modified.
	void DropBlock(block_id_t block_id);
	//! Register a dropped index, which is detached by DetachIndexes and destroyed when the dropped storage is freed
	void DropIndex(shared_ptr<DataTableInfo> info, idx_t index_oid);
	//! Register a dropped table, whose blocks are collected when the dropped storage is freed.
	void DropTable(shared_ptr<DataTable> table);
	//! Register a column that an ALTER replaced, whose blocks are collected when the dropped storage is freed.
	void DropColumn(shared_ptr<DataTable> table, idx_t column_index);
	//! Removes the dropped indexes from their tables, which keep them for checkpoints until they are destroyed.
	void DetachIndexes();
	//! Loads the dropped tables for older snapshots and excludes the dropped storage from checkpoints, once
	void ExcludeFromCheckpoints();
	//! Frees the dropped storage
	void Free();
	//! True if no work has been queued.
	bool Empty() const;

	//! The commit id of the transaction that dropped the storage
	transaction_t commit_id = 0;

private:
	//! Collects the dropped blocks and destroys the dropped indexes
	void Collect();

	optional_ptr<BlockManager> block_manager;
	vector<block_id_t> dropped_block_ids;
	vector<shared_ptr<DataTable>> dropped_tables;
	vector<pair<shared_ptr<DataTable>, idx_t>> dropped_columns;
	vector<DroppedIndex> dropped_indexes;
	//! Whether ExcludeFromCheckpoints ran
	bool excluded = false;
};

struct IndexDataRemover {
public:
	explicit IndexDataRemover(DuckTransaction &transaction, QueryContext context, IndexRemovalType removal_type);

	void PushDelete(DeleteInfo &info);
	void Verify();

private:
	void Flush(DataTable &table, row_t *row_numbers, idx_t count);

private:
	DuckTransaction &transaction;
	// data for index cleanup
	QueryContext context;
	//! While committing, we remove data from any indexes that was deleted
	IndexRemovalType removal_type;
	DataChunk chunk;
	//! Debug mode only - list of indexes to verify
	reference_map_t<DataTable, shared_ptr<DataTableInfo>> verify_indexes;
};

class CommitState {
public:
	explicit CommitState(DuckTransaction &transaction, transaction_t commit_id,
	                     ActiveTransactionState transaction_state, CommitMode commit_mode);

public:
	void CommitEntry(UndoFlags type, data_ptr_t data, CommitInfo &info);
	void RevertCommit(UndoFlags type, data_ptr_t data);
	void Flush();
	void Verify();
	static IndexRemovalType GetIndexRemovalType(ActiveTransactionState transaction_state, CommitMode commit_mode);

private:
	void CommitEntryDrop(CatalogEntry &entry, data_ptr_t extra_data, CommitInfo &info);
	void CommitDelete(DeleteInfo &info, transaction_t old_id);

private:
	DuckTransaction &transaction;
	transaction_t commit_id;
	IndexDataRemover index_data_remover;
};

} // namespace duckdb
