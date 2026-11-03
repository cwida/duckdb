#include "duckdb/transaction/undo_buffer.hpp"

#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/duck_index_entry.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/list.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/pair.hpp"
#include "duckdb/execution/index/bound_index.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/write_ahead_log.hpp"
#include "duckdb/transaction/cleanup_state.hpp"
#include "duckdb/transaction/commit_state.hpp"
#include "duckdb/transaction/delete_info.hpp"
#include "duckdb/transaction/rollback_state.hpp"
#include "duckdb/transaction/wal_write_state.hpp"
#include "duckdb/transaction/duck_transaction.hpp"

namespace duckdb {
constexpr uint32_t UNDO_ENTRY_HEADER_SIZE = sizeof(UndoFlags) + sizeof(uint32_t);

UndoBuffer::UndoBuffer(DuckTransaction &transaction_p, ClientContext &context_p)
    : transaction(transaction_p), allocator(DatabaseInstance::GetDatabase(context_p).GetBufferManager(), true) {
}

UndoBufferReference UndoBuffer::CreateEntry(UndoFlags type, idx_t len) {
	idx_t alloc_len = AlignValue<idx_t>(len + UNDO_ENTRY_HEADER_SIZE);
	auto handle = allocator.Allocate(alloc_len);
	auto data = handle.GetDataMutable();
	// write the undo entry metadata
	Store<UndoFlags>(type, data);
	data += sizeof(UndoFlags);
	Store<uint32_t>(UnsafeNumericCast<uint32_t>(alloc_len - UNDO_ENTRY_HEADER_SIZE), data);
	// increment the position of the header past the undo entry metadata
	handle.position += UNDO_ENTRY_HEADER_SIZE;
	return handle;
}

template <class T>
void UndoBuffer::IterateEntries(UndoBuffer::IteratorState &state, T &&callback) {
	// iterate in insertion order: start with the tail
	state.current = allocator.tail.get();
	state.started = true;
	while (state.current) {
		state.handle = allocator.buffer_manager.Pin(state.current->block);
		state.start = state.handle.GetDataMutable();
		state.end = state.start + state.current->position;
		while (state.start < state.end) {
			auto len_position = state.start + sizeof(UndoFlags);
			auto payload_position = len_position + sizeof(uint32_t);
			auto type = Load<UndoFlags>(state.start);
			auto len = Load<uint32_t>(len_position);

			callback(type, payload_position);
			state.start = payload_position + len;
		}
		state.current = state.current->prev;
	}
}

template <class T>
void UndoBuffer::ReverseIterateEntries(UndoBuffer::IteratorState &end_state, T &&callback) {
	if (!end_state.started) {
		return;
	}

	idx_t end_position = 0;
	if (end_state.current) {
		auto block_start = end_state.handle.GetDataMutable();
		D_ASSERT(end_state.start >= block_start);
		end_position = NumericCast<idx_t>(end_state.start - block_start);
		D_ASSERT(end_position <= end_state.current->position);
	}

	// Start at the newest committed entry and iterate back to the oldest one.
	auto current = end_state.current ? end_state.current : allocator.head.get();
	while (current) {
		auto handle = allocator.buffer_manager.Pin(current->block);
		data_ptr_t start = handle.GetDataMutable();
		data_ptr_t end = start + (current == end_state.current ? end_position : current->position);
		vector<pair<UndoFlags, data_ptr_t>> nodes;
		while (start < end) {
			auto type = Load<UndoFlags>(start);
			start += sizeof(UndoFlags);
			auto len = Load<uint32_t>(start);
			start += sizeof(uint32_t);
			nodes.emplace_back(type, start);
			start += len;
		}
		for (idx_t i = nodes.size(); i > 0; i--) {
			callback(nodes[i - 1].first, nodes[i - 1].second);
		}
		current = current->next.get();
	}
}

template <class T>
void UndoBuffer::ReverseIterateEntries(T &&callback) {
	UndoBuffer::IteratorState state;
	state.started = true;
	ReverseIterateEntries(state, std::forward<T>(callback));
}

bool UndoBuffer::ChangesMade() {
	// we need to search for any index creation entries
	return allocator.head.get();
}

UndoBufferProperties UndoBuffer::GetProperties() {
	UndoBufferProperties properties;
	if (!ChangesMade()) {
		return properties;
	}
	auto node = allocator.head.get();
	while (node) {
		properties.estimated_size += node->position;
		node = node->next.get();
	}

	// we need to search for any index creation entries
	IteratorState iterator_state;
	IterateEntries(iterator_state, [&](UndoFlags entry_type, data_ptr_t data) {
		switch (entry_type) {
		case UndoFlags::UPDATE_TUPLE:
			properties.has_updates = true;
			break;
		case UndoFlags::DELETE_TUPLE: {
			auto info = reinterpret_cast<DeleteInfo *>(data);
			if (info->is_consecutive) {
				properties.estimated_size += sizeof(row_t) * info->count;
			}
			if (info->table->GetStorage().HasIndexes()) {
				properties.has_index_deletes = true;
			}
			properties.has_deletes = true;
			break;
		}
		case UndoFlags::CATALOG_ENTRY: {
			properties.has_catalog_changes = true;

			auto catalog_entry = Load<CatalogEntry *>(data);
			auto &parent = catalog_entry->Parent();
			switch (parent.type) {
			case CatalogType::INDEX_ENTRY: {
				auto &index = parent.Cast<DuckIndexEntry>();
				properties.estimated_size += index.initial_index_size;
				break;
			}
			default:
				break;
			}
			break;
		}
		default:
			break;
		}
	});
	return properties;
}

void UndoBuffer::Cleanup(VisibilityBound lowest_visibility_bound) {
	// garbage collect everything in the Undo Chunk
	// this should only happen if
	//  (1) the transaction this UndoBuffer belongs to has successfully
	//  committed
	//      (on Rollback the Rollback() function should be called, that clears
	//      the chunks)
	//  (2) there is no active transaction with start_id < commit_id of this
	//  transaction
	CleanupState state(transaction, lowest_visibility_bound, active_transaction_state);
	UndoBuffer::IteratorState iterator_state;
	idx_t entry_idx = 0;
	IterateEntries(iterator_state, [&](UndoFlags type, data_ptr_t data) {
		if (entry_idx++ < cleaned_up_entries) {
			return;
		}
		state.CleanupEntry(type, data);
		cleaned_up_entries++;
	});
}

void UndoBuffer::WriteToWAL(WriteAheadLog &wal, optional_ptr<StorageCommitState> commit_state) {
	WALWriteState state(transaction, wal, commit_state);
	UndoBuffer::IteratorState iterator_state;
	IterateEntries(iterator_state, [&](UndoFlags type, data_ptr_t data) { state.CommitEntry(type, data); });
}

void UndoBuffer::Commit(UndoBuffer::IteratorState &iterator_state, CommitInfo &info) {
	// No undo entries are allocated after this
	allocator.ReleaseHeadPin();
	active_transaction_state = info.active_transactions;

	CommitState state(transaction, info.commit_id, active_transaction_state, CommitMode::COMMIT);
	IterateEntries(iterator_state, [&](UndoFlags type, data_ptr_t data) { state.CommitEntry(type, data, info); });
	state.Verify();
}

void UndoBuffer::RevertCommit(UndoBuffer::IteratorState &end_state, transaction_t transaction_id) {
	CommitState state(transaction, transaction_id, active_transaction_state, CommitMode::REVERT_COMMIT);
	ReverseIterateEntries(end_state, [&](UndoFlags type, data_ptr_t data) { state.RevertCommit(type, data); });

	state.Verify();
}

void UndoBuffer::Rollback() {
	allocator.ReleaseHeadPin();
	// rollback needs to be performed in reverse
	RollbackState state(transaction);
	ReverseIterateEntries([&](UndoFlags type, data_ptr_t data) { state.RollbackEntry(type, data); });
}
} // namespace duckdb
