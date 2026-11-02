#include "duckdb/execution/operator/persistent/physical_delete.hpp"

#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/algorithm.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/vector_size.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/index/bound_index.hpp"
#include "duckdb/execution/row_id_deduplicator.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/delete_state.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/local_storage.hpp"

namespace duckdb {

PhysicalDelete::PhysicalDelete(PhysicalPlan &physical_plan, vector<LogicalType> types, DuckTableEntry &tableref,
                               DataTable &table, vector<unique_ptr<BoundConstraint>> bound_constraints,
                               idx_t row_id_index, idx_t estimated_cardinality, bool return_chunk,
                               vector<idx_t> return_columns)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::DELETE_OPERATOR, std::move(types), estimated_cardinality),
      tableref(tableref), table(table), bound_constraints(std::move(bound_constraints)), row_id_index(row_id_index),
      return_chunk(return_chunk), return_columns(std::move(return_columns)) {
}
//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
class DeleteGlobalState : public GlobalSinkState {
public:
	explicit DeleteGlobalState(ClientContext &context, const vector<LogicalType> &return_types, DuckTableEntry &table,
	                           const vector<unique_ptr<BoundConstraint>> &bound_constraints, bool return_chunk)
	    : deleted_count(0), return_collection(context, return_types), has_unique_indexes(false) {
		// We need to append deletes to the local delete-ART.
		auto &storage = table.GetStorage();
		if (storage.HasUniqueIndexes()) {
			storage.InitializeLocalStorage(delete_index_append_state, table, context, bound_constraints);
			has_unique_indexes = true;
		}
		if (return_chunk) {
			deleted_rows = make_uniq<RowIdDeduplicator>(context, vector<LogicalType> {LogicalType::ROW_TYPE});
		}
	}

	//! Protects deleted_rows and return_collection Combine()
	annotated_mutex return_lock;
	//! Conservatively protect delete-index maintenance (unique indexes)
	annotated_mutex index_lock;

	atomic<idx_t> deleted_count;
	ColumnDataCollection return_collection;
	//! Global deleted row_ids (for cross-thread dedup in Sink; only accessed under return_lock)
	unique_ptr<RowIdDeduplicator> deleted_rows DUCKDB_GUARDED_BY(return_lock);
	LocalAppendState delete_index_append_state;
	bool has_unique_indexes;
};

class DeleteLocalState : public LocalSinkState {
public:
	DeleteLocalState(ClientContext &context, TableCatalogEntry &table,
	                 const vector<unique_ptr<BoundConstraint>> &bound_constraints,
	                 const vector<LogicalType> &return_types, bool return_chunk) {
		// For RETURNING: use operator's return types (may include virtual columns)
		// For non-RETURNING with indexes: use table types for index updates
		auto types = return_chunk ? return_types : table.GetTypes();
		auto initialize = vector<bool>(types.size(), false);
		delete_chunk.Initialize(Allocator::Get(context), types, initialize);

		auto &storage = table.GetStorage();
		delete_state = storage.InitializeDelete(table, context, bound_constraints);

		if (return_chunk) {
			// Collect RETURNING rows per-thread, merge into global in Combine()
			return_collection = make_uniq<ColumnDataCollection>(context, return_types);
			// Per-thread deleted row_ids (filters intra-thread duplicates lock-free)
			deleted_rows = make_uniq<RowIdDeduplicator>(context, vector<LogicalType> {LogicalType::ROW_TYPE});
			// Reusable buffers for the global pass to avoid per-chunk allocation
			global_sel.Initialize(STANDARD_VECTOR_SIZE);
			final_sel.Initialize(STANDARD_VECTOR_SIZE);
		}
	}

public:
	DataChunk delete_chunk;
	unique_ptr<TableDeleteState> delete_state;
	unique_ptr<ColumnDataCollection> return_collection;
	unique_ptr<RowIdDeduplicator> deleted_rows;
	//! Reusable selections for the global row_id pass (avoids allocation in Sink hot path)
	SelectionVector global_sel;
	SelectionVector final_sel;
};

SinkResultType PhysicalDelete::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &g_state = input.global_state.Cast<DeleteGlobalState>();
	auto &l_state = input.local_state.Cast<DeleteLocalState>();

	auto &row_ids = chunk.data[row_id_index];
	row_ids.Flatten();

	// Fast path: no RETURNING and no unique indexes
	if (!return_chunk && !g_state.has_unique_indexes) {
		auto deleted = table.Delete(*l_state.delete_state, context.client, tableref, row_ids, chunk.size());
		g_state.deleted_count.fetch_add(deleted);
		return SinkResultType::NEED_MORE_INPUT;
	}

	auto types = table.GetTypes();
	l_state.delete_chunk.Reset();

	SelectionVector delete_sel(chunk.size());
	idx_t delete_count = chunk.size();
	Vector delete_row_ids(Vector::Ref(row_ids));

	// If RETURNING is enabled, deduplicate row_ids: per-thread pass (lock-free) then global pass (batched lock).
	// Both passes use buffer-managed hash tables so the set of deleted row ids counts against the memory limit.
	if (return_chunk) {
		D_ASSERT(l_state.deleted_rows);
		idx_t count = l_state.deleted_rows->Register(row_ids, chunk.size(), delete_sel);
		if (count == 0) {
			return SinkResultType::NEED_MORE_INPUT;
		}
		// the global pass only sees the rows that are new to this thread
		Vector local_new_row_ids(Vector::Ref(row_ids));
		local_new_row_ids.Slice(row_ids, delete_sel, count);
		{
			annotated_lock_guard<annotated_mutex> guard(g_state.return_lock);
			D_ASSERT(g_state.deleted_rows);
			if (l_state.global_sel.Capacity() < count) {
				l_state.global_sel.Initialize(count);
				l_state.final_sel.Initialize(count);
			}
			auto final_count = g_state.deleted_rows->Register(local_new_row_ids, count, l_state.global_sel);
			if (final_count == 0) {
				return SinkResultType::NEED_MORE_INPUT;
			}
			// map the selection over the locally new rows back to the input chunk
			for (idx_t i = 0; i < final_count; i++) {
				l_state.final_sel.set_index(i, delete_sel.get_index(l_state.global_sel.get_index(i)));
			}
			delete_count = final_count;
			delete_sel.Initialize(l_state.final_sel.data(), l_state.final_sel.Capacity());
		}
		if (delete_count != chunk.size()) {
			delete_row_ids.Slice(row_ids, delete_sel, delete_count);
		}
	}

	// Plans serialized by pre-v2.0 clients carry no `return_columns` and do not pass the needed columns
	// through the scan, so fetch them by row id instead.
	const bool fetch_by_row_id = return_columns.empty();
	DataChunk fetch_chunk;
	if (fetch_by_row_id) {
		vector<StorageIndex> column_ids;
		if (return_chunk) {
			for (idx_t i = 0; i < table.ColumnCount(); i++) {
				column_ids.emplace_back(i);
			}
		} else {
			auto indexed_columns = table.GetDataTableInfo()->GetIndexes().GetUniqueIndexColumns();
			vector<column_t> sorted_columns(indexed_columns.begin(), indexed_columns.end());
			std::sort(sorted_columns.begin(), sorted_columns.end());
			for (auto col : sorted_columns) {
				column_ids.emplace_back(col);
			}
		}
		vector<LogicalType> column_types;
		for (auto &col : column_ids) {
			column_types.push_back(types[col.GetPrimaryIndex()]);
		}
		// Also fetch the row ids, since Fetch skips rows that are no longer visible.
		column_ids.emplace_back(COLUMN_IDENTIFIER_ROW_ID);
		column_types.push_back(LogicalType::ROW_TYPE);

		delete_row_ids.Flatten();
		fetch_chunk.Initialize(Allocator::Get(context.client), column_types, delete_count);
		ColumnFetchState fetch_state;
		table.Fetch(DuckTransaction::Get(context.client, table.db), fetch_chunk, column_ids, delete_row_ids,
		            delete_count, fetch_state);
		delete_count = fetch_chunk.size();
		if (delete_count == 0) {
			return SinkResultType::NEED_MORE_INPUT;
		}
		delete_row_ids.Reference(fetch_chunk.data.back());

		idx_t fetch_idx = 0;
		for (idx_t i = 0; i < table.ColumnCount(); i++) {
			if (fetch_idx + 1 < column_ids.size() && column_ids[fetch_idx].GetPrimaryIndex() == i) {
				l_state.delete_chunk.data[i].Reference(fetch_chunk.data[fetch_idx++]);
			} else {
				l_state.delete_chunk.data[i].Reference(Value(types[i]), count_t(delete_count));
			}
		}
	} else {
		// Use columns from the input chunk - they were passed through from the scan
		// return_columns maps storage_idx -> chunk_idx
		// For RETURNING: all columns have valid indices
		// For index-only: only indexed columns have valid indices (sparse mapping)
		for (idx_t i = 0; i < table.ColumnCount(); i++) {
			if (return_columns[i] != DConstants::INVALID_INDEX) {
				// Column was passed through from the scan
				l_state.delete_chunk.data[i].Reference(chunk.data[return_columns[i]]);
			} else {
				// Column not in scan (sparse mapping for index-only case) - use NULL placeholder
				l_state.delete_chunk.data[i].Reference(Value(types[i]), count_t(chunk.size()));
			}
		}
	}
	// Add virtual columns (e.g., rowid) after table columns
	for (idx_t i = table.ColumnCount(); i < l_state.delete_chunk.ColumnCount(); i++) {
		l_state.delete_chunk.data[i].Reference(fetch_by_row_id ? delete_row_ids : row_ids);
	}

	// Slice down to the claimed rows (RETURNING only); fetched rows are already limited to them
	if (return_chunk && !fetch_by_row_id && delete_count != chunk.size()) {
		l_state.delete_chunk.Slice(delete_sel, delete_count);
	}

	// Append the deleted row IDs to the delete indexes.
	if (g_state.has_unique_indexes && l_state.delete_chunk.size() != 0) {
		annotated_lock_guard<annotated_mutex> index_guard(g_state.index_lock);
		auto &local_storage = LocalStorage::Get(context.client, table.db);
		auto storage = local_storage.GetStorage(table);
		storage->AppendToDeleteIndexes(delete_row_ids, l_state.delete_chunk);
	}

	auto deleted = table.Delete(*l_state.delete_state, context.client, tableref, delete_row_ids, delete_count);
	g_state.deleted_count.fetch_add(deleted);

	// Collect RETURNING rows per-thread; merge into global in Combine()
	if (return_chunk) {
		D_ASSERT(l_state.return_collection);
		D_ASSERT(deleted == delete_count);
		l_state.return_collection->Append(l_state.delete_chunk);
	}

	return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType PhysicalDelete::Combine(ExecutionContext &, OperatorSinkCombineInput &input) const {
	if (!return_chunk) {
		return SinkCombineResultType::FINISHED;
	}
	auto &g_state = input.global_state.Cast<DeleteGlobalState>();
	auto &l_state = input.local_state.Cast<DeleteLocalState>();
	if (!l_state.return_collection || l_state.return_collection->Count() == 0) {
		return SinkCombineResultType::FINISHED;
	}
	// Merge per-thread return_collection into global (deleted_rows is only needed during Sink)
	annotated_lock_guard<annotated_mutex> guard(g_state.return_lock);
	g_state.return_collection.Combine(*l_state.return_collection);
	return SinkCombineResultType::FINISHED;
}

unique_ptr<GlobalSinkState> PhysicalDelete::GetGlobalSinkState(ClientContext &context) const {
	return make_uniq<DeleteGlobalState>(context, GetTypes(), tableref, bound_constraints, return_chunk);
}

unique_ptr<LocalSinkState> PhysicalDelete::GetLocalSinkState(ExecutionContext &context) const {
	return make_uniq<DeleteLocalState>(context.client, tableref, bound_constraints, GetTypes(), return_chunk);
}

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
class DeleteSourceState : public GlobalSourceState {
public:
	explicit DeleteSourceState(const PhysicalDelete &op) : total_rows(1), rows_scanned(0) {
		if (op.return_chunk) {
			D_ASSERT(op.sink_state);
			auto &g = op.sink_state->Cast<DeleteGlobalState>();
			g.return_collection.InitializeScan(global_scan_state);
			max_threads = MaxValue<idx_t>(g.return_collection.ChunkCount(), 1);
			total_rows = g.return_collection.Count();
		} else {
			max_threads = 1;
		}
	}

	idx_t MaxThreads() override {
		return max_threads;
	}

	ColumnDataParallelScanState global_scan_state;
	idx_t max_threads;
	idx_t total_rows;
	atomic<idx_t> rows_scanned;
};

class DeleteLocalSourceState : public LocalSourceState {
public:
	ColumnDataLocalScanState local_scan_state;
};

unique_ptr<GlobalSourceState> PhysicalDelete::GetGlobalSourceState(ClientContext &context) const {
	return make_uniq<DeleteSourceState>(*this);
}

unique_ptr<LocalSourceState> PhysicalDelete::GetLocalSourceState(ExecutionContext &, GlobalSourceState &) const {
	return make_uniq<DeleteLocalSourceState>();
}

ProgressData PhysicalDelete::GetProgress(ClientContext &context, GlobalSourceState &gstate) const {
	auto &state = gstate.Cast<DeleteSourceState>();
	ProgressData progress;
	progress.total = double(MaxValue<idx_t>(state.total_rows, 1));
	progress.done = state.total_rows == 0 ? 1.0 : double(state.rows_scanned.load(std::memory_order_relaxed));
	return progress;
}

SourceResultType PhysicalDelete::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                 OperatorSourceInput &input) const {
	auto &state = input.global_state.Cast<DeleteSourceState>();
	auto &g = sink_state->Cast<DeleteGlobalState>();
	if (!return_chunk) {
		chunk.data[0].Append(Value::BIGINT(NumericCast<int64_t>(g.deleted_count.load())));
		state.rows_scanned.store(1, std::memory_order_relaxed);
		return SourceResultType::FINISHED;
	}

	auto &lstate = input.local_state.Cast<DeleteLocalSourceState>();
	g.return_collection.Scan(state.global_scan_state, lstate.local_scan_state, chunk);
	state.rows_scanned.fetch_add(chunk.size(), std::memory_order_relaxed);
	return chunk.size() == 0 ? SourceResultType::FINISHED : SourceResultType::HAVE_MORE_OUTPUT;
}

} // namespace duckdb
