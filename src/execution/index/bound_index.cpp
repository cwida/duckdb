#include "duckdb/execution/index/bound_index.hpp"

#include "duckdb/common/array.hpp"
#include "duckdb/common/radix.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/execution/index/index_lock.hpp"
#include "duckdb/common/types/selection_vector.hpp"
#include "duckdb/common/types/column/column_data_scan_states.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"

namespace duckdb {

//-------------------------------------------------------------------------------
// Bound index
//-------------------------------------------------------------------------------

BoundIndex::BoundIndex(const Identifier &name, const string &index_type, IndexConstraintType index_constraint_type,
                       const vector<column_t> &column_ids, TableIOManager &table_io_manager,
                       const vector<unique_ptr<Expression>> &unbound_expressions_p, AttachedDatabase &db)
    : Index(column_ids, table_io_manager, db), name(name), index_type(index_type),
      index_constraint_type(index_constraint_type) {
	for (auto &expr : unbound_expressions_p) {
		types.push_back(expr->GetReturnType().InternalType());
		logical_types.push_back(expr->GetReturnType());
		unbound_expressions.emplace_back(expr->Copy());
		bound_expressions.push_back(BindExpression(expr->Copy()));
		executor.AddExpression(*bound_expressions.back());
	}
}

ErrorData BoundIndex::Append(DataChunk &chunk, Vector &row_ids) {
	IndexLock l(*this);
	return Append(l, chunk, row_ids);
}

ErrorData BoundIndex::Append(IndexLock &l, DataChunk &chunk, Vector &row_ids, IndexAppendInfo &info) {
	l.AssertHeld(*this);
	// Fallback to the old Append.
	return Append(l, chunk, row_ids);
}

ErrorData BoundIndex::Append(DataChunk &chunk, Vector &row_ids, IndexAppendInfo &info) {
	IndexLock l(*this);
	return Append(l, chunk, row_ids, info);
}

void BoundIndex::VerifyAppend(DataChunk &chunk, IndexAppendInfo &info, optional_ptr<ConflictManager> manager) {
	throw NotImplementedException("this implementation of VerifyAppend does not exist.");
}

void BoundIndex::VerifyConstraint(DataChunk &chunk, IndexAppendInfo &info, ConflictManager &manager) {
	throw NotImplementedException("this implementation of VerifyConstraint does not exist.");
}

void BoundIndex::ResetStorage() {
	IndexLock index_lock(*this);
	ResetStorage(index_lock);
}

idx_t BoundIndex::TryDelete(DataChunk &entries, Vector &row_identifiers, optional_ptr<SelectionVector> deleted_sel,
                            optional_ptr<SelectionVector> non_deleted_sel) {
	IndexLock state(*this);
	return TryDelete(state, entries, row_identifiers, deleted_sel, non_deleted_sel);
}

idx_t BoundIndex::TryDelete(IndexLock &state, DataChunk &entries, Vector &row_identifiers,
                            optional_ptr<SelectionVector> deleted_sel, optional_ptr<SelectionVector> non_deleted_sel) {
	state.AssertHeld(*this);
	throw InternalException("TryDelete not implemented");
}

void BoundIndex::Delete(DataChunk &entries, Vector &row_identifiers) {
	IndexLock state(*this);
	Delete(state, entries, row_identifiers);
}

void BoundIndex::Delete(IndexLock &state, DataChunk &entries, Vector &row_identifiers) {
	state.AssertHeld(*this);
	auto deleted_rows = TryDelete(state, entries, row_identifiers);
	if (deleted_rows != entries.size()) {
		throw InvalidInputException("Failed to delete all rows from index. Only deleted %d out of %d rows.\nChunk: %s",
		                            deleted_rows, entries.size(), entries.ToString());
	}
}

ErrorData BoundIndex::Insert(IndexLock &l, DataChunk &chunk, Vector &row_ids, IndexAppendInfo &info) {
	l.AssertHeld(*this);
	throw NotImplementedException("this implementation of Insert does not exist.");
}

bool BoundIndex::MergeIndexes(BoundIndex &other_index) {
	IndexLock state(*this);
	return MergeIndexes(state, other_index);
}

void BoundIndex::Verify() {
	IndexLock l(*this);
	Verify(l);
}

string BoundIndex::ToString(bool display_ascii) {
	IndexLock l(*this);
	return ToString(l, display_ascii);
}

void BoundIndex::VerifyAllocations() {
	IndexLock l(*this);
	return VerifyAllocations(l);
}

void BoundIndex::VerifyBuffers(IndexLock &l) {
	l.AssertHeld(*this);
	throw NotImplementedException("this implementation of VerifyBuffers does not exist");
}

void BoundIndex::VerifyBuffers() {
	IndexLock l(*this);
	return VerifyBuffers(l);
}

void BoundIndex::Vacuum() {
	IndexLock state(*this);
	Vacuum(state);
}

idx_t BoundIndex::GetInMemorySize() const {
	IndexLock state(*this);
	return GetInMemorySize(state);
}

void BoundIndex::ExecuteExpressions(DataChunk &input, DataChunk &result) const {
	executor.Execute(input, result);
}

idx_t BoundIndex::UnboundExpressionCount() const {
	return unbound_expressions.size();
}

unique_ptr<Expression> BoundIndex::CopyUnboundExpression(const idx_t index) const {
	D_ASSERT(index < unbound_expressions.size());
	auto &expression = unbound_expressions[index];
	D_ASSERT(expression);
	return expression->Copy();
}

unique_ptr<Expression> BoundIndex::BindExpression(unique_ptr<Expression> root_expr) {
	ExpressionIterator::VisitExpressionMutable<BoundColumnRefExpression>(
	    root_expr, [&](BoundColumnRefExpression &bound_colref, unique_ptr<Expression> &expr) {
		    expr = make_uniq<BoundReferenceExpression>(expr->GetReturnType(),
		                                               column_ids[bound_colref.Binding().column_index]);
	    });
	return root_expr;
}

bool BoundIndex::IndexIsUpdated(const vector<PhysicalIndex> &column_ids_p) const {
	for (auto &column : column_ids_p) {
		if (column_id_set.find(column.index) != column_id_set.end()) {
			return true;
		}
	}
	return false;
}

bool BoundIndex::SupportsDeltaIndexes() const {
	return false;
}

unique_ptr<BoundIndex> BoundIndex::CreateEmptyCopy(IndexConstraintType) const {
	throw InternalException("BoundIndex::CreateEmptyCopy is not supported for this index type");
}

ErrorData BoundIndex::MergeCheckpointDelta(IndexDeltaType, BoundIndex &) {
	throw InternalException("BoundIndex::MergeCheckpointDelta is not supported for this index type");
}

IndexStorageInfo BoundIndex::SerializeToWAL(const StorageVersion) {
	throw NotImplementedException("The implementation of this index WAL serialization does not exist.");
}

string BoundIndex::AppendRowError(DataChunk &input, idx_t index) {
	string error;
	for (idx_t c = 0; c < input.ColumnCount(); c++) {
		if (c > 0) {
			error += ", ";
		}
		error += input.GetValue(c, index).ToString();
	}
	return error;
}

namespace {

struct BufferedReplayState {
	optional_ptr<ColumnDataCollection> buffer = nullptr;
	ColumnDataScanState scan_state;
	DataChunk current_chunk;
	bool scan_initialized = false;
};
} // namespace

void BoundIndex::ApplyBufferedReplays(const vector<LogicalType> &table_types, BufferedIndexReplays &buffered_replays,
                                      const vector<StorageIndex> &mapped_column_ids) {
	if (!buffered_replays.HasBufferedReplays()) {
		return;
	}

	// We have two replay states: one for inserts and one for deletes. These are indexed into using the
	// replay_type. Both scans are interleaved, so the state maintains the position of each scan.
	array<BufferedReplayState, 2> replay_states;
	DataChunk table_chunk;
	table_chunk.InitializeEmpty(table_types);

	for (const auto &replay_range : buffered_replays.ranges) {
		const auto type_idx = static_cast<idx_t>(replay_range.type);
		auto &state = replay_states[type_idx];

		// Initialize the scan state if necessary. Take ownership of buffered operations, since we won't need
		// them after replaying anyways.
		if (!state.scan_initialized) {
			state.buffer = buffered_replays.GetBuffer(replay_range.type);
			state.buffer->InitializeScan(state.scan_state);
			state.buffer->InitializeScanChunk(state.current_chunk);
			state.scan_initialized = true;
		}

		idx_t current_row = replay_range.start;
		while (current_row < replay_range.end) {
			// Scan the next DataChunk from the ColumnDataCollection buffer if the current row is on or after
			// that chunk's starting row index.
			if (current_row >= state.scan_state.next_row_index) {
				if (!state.buffer->Scan(state.scan_state, state.current_chunk)) {
					throw InternalException("Buffered index data exhausted during replay");
				}
			}

			// We need to process the remaining rows in the current chunk, which is the minimum of the available
			// rows in the chunk and the remaining rows in the current range.
			const auto offset_in_chunk = current_row - state.scan_state.current_row_index;
			const auto available_in_chunk = state.current_chunk.size() - offset_in_chunk;
			// [start, end) in ReplayRange is [inclusive, exclusive).
			const auto range_remaining = replay_range.end - current_row;
			const auto rows_to_process = MinValue<idx_t>(available_in_chunk, range_remaining);

			SelectionVector sel(offset_in_chunk, rows_to_process);

			// Buffered chunks are in mapped_column_ids layout (plus a trailing rowid column).
			D_ASSERT(state.current_chunk.ColumnCount() == mapped_column_ids.size() + 1);
			for (idx_t col_idx = 0; col_idx < state.current_chunk.ColumnCount() - 1; col_idx++) {
				const auto col_id = mapped_column_ids[col_idx].GetPrimaryIndex();
				table_chunk.data[col_id].Reference(state.current_chunk.data[col_idx]);
				table_chunk.data[col_id].Slice(sel, rows_to_process);
			}
			Vector row_ids(state.current_chunk.data.back(), sel, rows_to_process);

			if (replay_range.type == BufferedIndexReplay::INSERT_ENTRY) {
				IndexAppendInfo append_info(IndexAppendMode::INSERT_DUPLICATES, nullptr);
				const auto error = Append(table_chunk, row_ids, append_info);
				if (error.HasError()) {
					throw InternalException("error while applying buffered appends: " + error.Message());
				}
				current_row += rows_to_process;
				continue;
			}
			Delete(table_chunk, row_ids);
			current_row += rows_to_process;
		}
	}
}

} // namespace duckdb
