#include "duckdb/execution/operator/persistent/physical_update.hpp"

#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/algorithm.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/row_id_deduplicator.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parallel/thread_context.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/data_table_info.hpp"
#include "duckdb/storage/table/delete_state.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/storage/table/update_state.hpp"
#include "duckdb/transaction/duck_transaction.hpp"

namespace duckdb {

PhysicalUpdate::PhysicalUpdate(PhysicalPlan &physical_plan, vector<LogicalType> types, DuckTableEntry &tableref,
                               DataTable &table, vector<PhysicalIndex> columns,
                               vector<unique_ptr<Expression>> expressions,
                               vector<unique_ptr<Expression>> bound_defaults,
                               vector<unique_ptr<BoundConstraint>> bound_constraints, idx_t estimated_cardinality,
                               bool return_chunk, bool capture_old_rows, vector<idx_t> old_row_columns,
                               RowIdHandling row_id_handling)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::UPDATE, std::move(types), estimated_cardinality),
      tableref(tableref), table(table), columns(std::move(columns)), expressions(std::move(expressions)),
      bound_defaults(std::move(bound_defaults)), bound_constraints(std::move(bound_constraints)),
      return_chunk(return_chunk), capture_old_rows(capture_old_rows), old_row_columns(std::move(old_row_columns)),
      row_id_handling(row_id_handling), index_update(false) {
	auto &indexes = table.GetDataTableInfo().get()->GetIndexes();
	auto index_columns = indexes.GetIndexedColumns();

	unordered_set<column_t> update_columns;
	update_columns.reserve(this->columns.size());
	for (const auto col : this->columns) {
		update_columns.insert(col.index);
	}

	for (const auto &col : table.Columns()) {
		if (index_columns.find(col.Physical().index) == index_columns.end()) {
			continue;
		}
		if (update_columns.find(col.Physical().index) == update_columns.end()) {
			continue;
		}
		index_update = true;
		break;
	}
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
class UpdateGlobalState : public GlobalSinkState {
public:
	explicit UpdateGlobalState(ClientContext &context, const vector<LogicalType> &return_types,
	                           RowIdHandling row_id_handling)
	    : updated_count(0), return_collection(context, return_types) {
		if (row_id_handling != RowIdHandling::ASSUME_UNIQUE) {
			updated_rows = make_uniq<RowIdDeduplicator>(context, vector<LogicalType> {LogicalType::ROW_TYPE});
		}
	}

	mutex lock;
	atomic<idx_t> updated_count;
	unique_ptr<RowIdDeduplicator> updated_rows;
	ColumnDataCollection return_collection;
};

class UpdateLocalState : public LocalSinkState {
public:
	UpdateLocalState(ClientContext &context, DataTable &table, const vector<unique_ptr<Expression>> &expressions,
	                 const vector<LogicalType> &table_types, const vector<unique_ptr<Expression>> &bound_defaults,
	                 const vector<unique_ptr<BoundConstraint>> &bound_constraints, bool capture_old_rows,
	                 bool index_update)
	    : table_types(table_types), bound_constraints(bound_constraints) {
		// Only DEFAULT columns are computed into the update chunk, all other columns reference the input.
		auto &allocator = Allocator::Get(context);
		vector<LogicalType> update_types;
		vector<bool> initialize;
		update_types.reserve(expressions.size());
		initialize.reserve(expressions.size());
		bool has_default = false;
		for (auto &expr : expressions) {
			const bool is_default = expr->GetExpressionType() == ExpressionType::VALUE_DEFAULT;
			update_types.push_back(expr->GetReturnType());
			initialize.push_back(is_default);
			has_default = has_default || is_default;
		}
		update_chunk.Initialize(allocator, update_types, initialize);
		if (has_default) {
			default_executor = make_uniq<ExpressionExecutor>(context, bound_defaults);
		}

		// The mock and delete chunks only reference other vectors.
		mock_chunk.InitializeEmpty(table_types);
		if (index_update) {
			delete_chunk.InitializeEmpty(table_types);

			// Only unique indexes keep delete entries, so only their key columns are fetched; the rest are NULL.
			auto unique_columns = table.GetDataTableInfo()->GetIndexes().GetUniqueIndexColumns();
			vector<column_t> sorted_columns(unique_columns.begin(), unique_columns.end());
			std::sort(sorted_columns.begin(), sorted_columns.end());
			vector<LogicalType> fetch_types;
			fetch_column_ids.reserve(sorted_columns.size());
			fetch_types.reserve(sorted_columns.size());
			for (auto col : sorted_columns) {
				fetch_column_ids.emplace_back(col);
				fetch_types.push_back(table_types[col]);
			}
			if (!fetch_column_ids.empty()) {
				fetch_chunk.Initialize(allocator, fetch_types);
			}
		}

		// When capturing OLD rows, the return chunk holds the NEW image followed by the OLD image.
		if (capture_old_rows) {
			vector<LogicalType> combined_types(table_types);
			combined_types.insert(combined_types.end(), table_types.begin(), table_types.end());
			combined_chunk.InitializeEmpty(combined_types);
		}
	}

	vector<LogicalType> table_types;
	DataChunk update_chunk;
	DataChunk mock_chunk;
	DataChunk delete_chunk;
	//! Sorted unique-index key columns fetched into fetch_chunk for the delete indexes
	vector<StorageIndex> fetch_column_ids;
	DataChunk fetch_chunk;
	DataChunk combined_chunk;
	unique_ptr<ExpressionExecutor> default_executor;
	unique_ptr<TableDeleteState> delete_state;
	unique_ptr<TableUpdateState> update_state;
	const vector<unique_ptr<BoundConstraint>> &bound_constraints;

	TableDeleteState &GetDeleteState(DataTable &table, TableCatalogEntry &tableref, ClientContext &context) {
		if (!delete_state) {
			delete_state = table.InitializeDelete(tableref, context, bound_constraints);
		}
		return *delete_state;
	}

	TableUpdateState &GetUpdateState(DataTable &table, TableCatalogEntry &tableref, ClientContext &context) {
		if (!update_state) {
			update_state = table.InitializeUpdate(tableref, context, bound_constraints);
		}
		return *update_state;
	}
};

// Append one return-chunk row set: the NEW image (already arranged in table order) followed by the OLD image,
// sourced from the captured OLD columns of the input chunk. old_row_columns[c] is the input-chunk index of the
// c-th physical column's captured OLD value. When a selection vector is given (del+insert dedup), it is applied
// to the OLD columns so their cardinality matches the NEW image.
static void AppendReturnRows(ColumnDataCollection &collection, DataChunk &combined, DataChunk &new_image,
                             DataChunk &input, idx_t new_col_count, const vector<idx_t> &old_row_columns, idx_t count,
                             optional_ptr<const SelectionVector> sel) {
	combined.Reset();
	for (idx_t c = 0; c < new_col_count; c++) {
		combined.data[c].Reference(new_image.data[c]);
		auto &old_source = input.data[old_row_columns[c]];
		if (sel) {
			combined.data[new_col_count + c].Slice(old_source, *sel, count);
		} else {
			combined.data[new_col_count + c].Reference(old_source);
		}
	}
	combined.CheckCardinality(count);
	collection.Append(combined);
}

SinkResultType PhysicalUpdate::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &g_state = input.global_state.Cast<UpdateGlobalState>();
	auto &l_state = input.local_state.Cast<UpdateLocalState>();

	chunk.Flatten();
	if (l_state.default_executor) {
		l_state.default_executor->SetChunk(chunk);
	}

	DataChunk &update_chunk = l_state.update_chunk;
	update_chunk.Reset();

	for (idx_t i = 0; i < expressions.size(); i++) {
		// Default expression, set to the default value of the column.
		if (expressions[i]->GetExpressionType() == ExpressionType::VALUE_DEFAULT) {
			l_state.default_executor->ExecuteExpression(columns[i].index, update_chunk.data[i]);
			continue;
		}

		D_ASSERT(expressions[i]->GetExpressionType() == ExpressionType::BOUND_REF);
		auto &binding = expressions[i]->Cast<BoundReferenceExpression>();
		update_chunk.data[i].Reference(chunk.data[binding.Index()]);
	}

	auto &row_ids = chunk.data[chunk.ColumnCount() - 1];
	DataChunk &mock_chunk = l_state.mock_chunk;

	// Regular in-place update.
	if (!update_is_del_and_insert) {
		// Apply the duplicate row-id policy: ASSUME_UNIQUE keeps the lock-free path; KEEP_FIRST (UPDATE ... FROM)
		// deduplicates so each row is updated at most once, keeping RETURNING and transition tables predictable.
		idx_t update_count = update_chunk.size();
		SelectionVector sel;
		Vector update_row_ids(Vector::Ref(row_ids));
		const bool deduplicate = row_id_handling != RowIdHandling::ASSUME_UNIQUE;
		if (deduplicate) {
			sel.Initialize(update_chunk.size());
			lock_guard<mutex> glock(g_state.lock);
			D_ASSERT(g_state.updated_rows);
			update_count = g_state.updated_rows->Register(row_ids, update_chunk.size(), sel);
			if (row_id_handling == RowIdHandling::ERROR && update_count != update_chunk.size()) {
				throw InvalidInputException("UPDATE command cannot update the same row more than once");
			}
			if (update_count != update_chunk.size()) {
				update_chunk.Slice(sel, update_count);
				update_row_ids.Slice(row_ids, sel, update_count);
			}
		}

		if (return_chunk) {
			// (re)reference all output columns first, then validate + set the cardinality. mock_chunk is not reset
			// here, but with return_chunk the update projects every table column, so all columns are referenced.
			for (idx_t i = 0; i < columns.size(); i++) {
				mock_chunk.data[columns[i].index].Reference(update_chunk.data[i]);
			}
			mock_chunk.CheckCardinality(update_count);
		}
		auto &update_state = l_state.GetUpdateState(table, tableref, context.client);
		table.Update(update_state, context.client, tableref, update_row_ids, columns, update_chunk);

		if (return_chunk) {
			lock_guard<mutex> glock(g_state.lock);
			if (capture_old_rows) {
				// When we deduplicated, apply the selection vector to the OLD columns so they line up with NEW.
				AppendReturnRows(g_state.return_collection, l_state.combined_chunk, mock_chunk, chunk,
				                 mock_chunk.ColumnCount(), old_row_columns, update_count, deduplicate ? &sel : nullptr);
			} else {
				g_state.return_collection.Append(mock_chunk);
			}
		}
		g_state.updated_count += update_count;
		return SinkResultType::NEED_MORE_INPUT;
	}

	// We update an index or a complex type, so we need to split the UPDATE into DELETE + INSERT.

	// Apply the duplicate row-id policy. This path holds the lock for its whole body (the append is serialized),
	// so deduplication runs under it. ASSUME_UNIQUE (plain UPDATE and MERGE-driven updates) keeps every row;
	// KEEP_FIRST (UPDATE ... FROM) drops repeated row-ids so we never delete+insert the same row twice.
	SelectionVector sel(update_chunk.size());
	lock_guard<mutex> glock(g_state.lock);
	idx_t update_count = update_chunk.size();
	const bool deduplicate = row_id_handling != RowIdHandling::ASSUME_UNIQUE;
	if (deduplicate) {
		D_ASSERT(g_state.updated_rows);
		update_count = g_state.updated_rows->Register(row_ids, update_chunk.size(), sel);
		if (row_id_handling == RowIdHandling::ERROR && update_count != update_chunk.size()) {
			throw InvalidInputException("UPDATE command cannot update the same row more than once");
		}
	}

	// The update chunk now contains exactly those rows that we are deleting.
	Vector del_row_ids(Vector::Ref(row_ids));
	if (deduplicate && update_count != update_chunk.size()) {
		update_chunk.Slice(sel, update_count);
		del_row_ids.Slice(row_ids, sel, update_count);
	}

	auto &delete_chunk = index_update ? l_state.delete_chunk : l_state.mock_chunk;
	delete_chunk.Reset();

	// Without unique indexes the delete chunk stays empty, so no delete index entries are added.
	if (index_update && !l_state.fetch_column_ids.empty()) {
		auto &column_ids = l_state.fetch_column_ids;
		auto &fetch_chunk = l_state.fetch_chunk;
		// Fetch needs a flat vector, but deduplication may have sliced del_row_ids into a dictionary.
		del_row_ids.Flatten();
		fetch_chunk.Reset();
		auto fetch_state = ColumnFetchState();
		auto &transaction = DuckTransaction::Get(context.client, table.db);
		table.Fetch(transaction, fetch_chunk, column_ids, del_row_ids, update_count, fetch_state);

		idx_t fetch_idx = 0;
		for (idx_t i = 0; i < delete_chunk.ColumnCount(); i++) {
			if (fetch_idx < column_ids.size() && column_ids[fetch_idx].GetPrimaryIndex() == i) {
				delete_chunk.data[i].Reference(fetch_chunk.data[fetch_idx++]);
			} else {
				delete_chunk.data[i].Reference(Value(l_state.table_types[i]), count_t(update_count));
			}
		}
		delete_chunk.CheckCardinality(update_count);
	}

	auto &delete_state = l_state.GetDeleteState(table, tableref, context.client);
	table.Delete(delete_state, context.client, tableref, del_row_ids, update_count);

	// Arrange the columns in the standard table order, then validate + set the cardinality from the referenced
	// columns. The del+insert path projects every table column (it re-inserts the full row), so all are referenced.
	for (idx_t i = 0; i < columns.size(); i++) {
		mock_chunk.data[columns[i].index].Reference(update_chunk.data[i]);
	}
	mock_chunk.CheckCardinality(update_count);

	table.LocalAppend(tableref, context.client, mock_chunk, bound_constraints, del_row_ids, delete_chunk);
	if (return_chunk) {
		if (capture_old_rows) {
			// Apply the dedup selection vector to the OLD columns so they line up with the NEW image.
			AppendReturnRows(g_state.return_collection, l_state.combined_chunk, mock_chunk, chunk,
			                 mock_chunk.ColumnCount(), old_row_columns, update_count, deduplicate ? &sel : nullptr);
		} else {
			g_state.return_collection.Append(mock_chunk);
		}
	}

	g_state.updated_count += update_count;
	return SinkResultType::NEED_MORE_INPUT;
}

unique_ptr<GlobalSinkState> PhysicalUpdate::GetGlobalSinkState(ClientContext &context) const {
	return make_uniq<UpdateGlobalState>(context, GetTypes(), row_id_handling);
}

unique_ptr<LocalSinkState> PhysicalUpdate::GetLocalSinkState(ExecutionContext &context) const {
	return make_uniq<UpdateLocalState>(context.client, table, expressions, table.GetTypes(), bound_defaults,
	                                   bound_constraints, capture_old_rows, index_update);
}

SinkCombineResultType PhysicalUpdate::Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const {
	auto &client_profiler = QueryProfiler::Get(context.client);
	context.thread.profiler.Flush(*this);
	client_profiler.Flush(context.thread.profiler);
	return SinkCombineResultType::FINISHED;
}

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
class UpdateSourceState : public GlobalSourceState {
public:
	explicit UpdateSourceState(const PhysicalUpdate &op) : total_rows(1), rows_scanned(0) {
		if (op.return_chunk) {
			D_ASSERT(op.sink_state);
			auto &g = op.sink_state->Cast<UpdateGlobalState>();
			g.return_collection.InitializeScan(scan_state);
			total_rows = g.return_collection.Count();
		}
	}

	ColumnDataScanState scan_state;
	idx_t total_rows;
	atomic<idx_t> rows_scanned;
};

unique_ptr<GlobalSourceState> PhysicalUpdate::GetGlobalSourceState(ClientContext &context) const {
	return make_uniq<UpdateSourceState>(*this);
}

ProgressData PhysicalUpdate::GetProgress(ClientContext &context, GlobalSourceState &gstate) const {
	auto &state = gstate.Cast<UpdateSourceState>();
	ProgressData progress;
	progress.total = double(MaxValue<idx_t>(state.total_rows, 1));
	progress.done = state.total_rows == 0 ? 1.0 : double(state.rows_scanned.load(std::memory_order_relaxed));
	return progress;
}

SourceResultType PhysicalUpdate::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                 OperatorSourceInput &input) const {
	auto &state = input.global_state.Cast<UpdateSourceState>();
	auto &g = sink_state->Cast<UpdateGlobalState>();
	if (!return_chunk) {
		chunk.data[0].Append(Value::BIGINT(NumericCast<int64_t>(g.updated_count.load())));
		state.rows_scanned.store(1, std::memory_order_relaxed);
		return SourceResultType::FINISHED;
	}

	g.return_collection.Scan(state.scan_state, chunk);
	state.rows_scanned.fetch_add(chunk.size(), std::memory_order_relaxed);

	return chunk.size() == 0 ? SourceResultType::FINISHED : SourceResultType::HAVE_MORE_OUTPUT;
}

} // namespace duckdb
