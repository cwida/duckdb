//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/transaction/undo_buffer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/undo_flags.hpp"
#include "duckdb/transaction/undo_buffer_allocator.hpp"
#include "duckdb/common/enums/active_transaction_state.hpp"

namespace duckdb {
class BufferManager;
class ClientContext;
class CommitDropState;
class DuckTransaction;
class StorageCommitState;
class WriteAheadLog;
struct UndoBufferPointer;
struct CommitInfo;

struct UndoBufferProperties {
	idx_t estimated_size = 0;
	bool has_updates = false;
	bool has_deletes = false;
	bool has_index_deletes = false;
	bool has_catalog_changes = false;
	bool has_dropped_entries = false;
};

//! The undo buffer of a transaction is used to hold previous versions of tuples
//! that might be required in the future (because of rollbacks or previous
//! transactions accessing them)
class UndoBuffer {
public:
	struct IteratorState {
		BufferHandle handle;
		optional_ptr<UndoBufferEntry> current;
		data_ptr_t start;
		data_ptr_t end;
		bool started = false;
	};

public:
	explicit UndoBuffer(DuckTransaction &transaction, ClientContext &context);

	//! Write a specified entry to the undo buffer
	UndoBufferReference CreateEntry(UndoFlags type, idx_t len);

	bool ChangesMade();
	UndoBufferProperties GetProperties();

	//! Cleanup the undo buffer
	void Cleanup(VisibilityBound lowest_visibility_bound);
	//! Commit the changes made in the UndoBuffer: should be called on commit
	void WriteToWAL(WriteAheadLog &wal, optional_ptr<StorageCommitState> commit_state);
	//! Iterate the undo buffer and commit each entry. Deferred drop side effects accumulate in
	//! info.drop_state so they can be applied after the commit chain succeeds.
	void Commit(UndoBuffer::IteratorState &iterator_state, CommitInfo &info);
	//! Revert committed changes made in the UndoBuffer up until the currently committed state
	void RevertCommit(UndoBuffer::IteratorState &iterator_state, transaction_t transaction_id);
	//! Rollback the changes made in this UndoBuffer: should be called on
	//! rollback
	void Rollback();

private:
	DuckTransaction &transaction;
	UndoBufferAllocator allocator;
	ActiveTransactionState active_transaction_state = ActiveTransactionState::UNSET;
	//! Number of entries already cleaned up, so that a cleanup that threw part-way can be resumed
	idx_t cleaned_up_entries = 0;

private:
	template <class T>
	void IterateEntries(UndoBuffer::IteratorState &state, T &&callback);
	template <class T>
	void ReverseIterateEntries(UndoBuffer::IteratorState &end_state, T &&callback);
	template <class T>
	void ReverseIterateEntries(T &&callback);
};

} // namespace duckdb
