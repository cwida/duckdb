#include "duckdb/storage/table/row_number_column_data.hpp"
#include "duckdb/storage/table/scan_state.hpp"

namespace duckdb {

RowNumberColumnData::RowNumberColumnData(BlockManager &block_manager, DataTableInfo &info)
    : ColumnData(block_manager, info, COLUMN_IDENTIFIER_ROW_NUMBER, LogicalType(LogicalTypeId::BIGINT),
                 ColumnDataType::MAIN_TABLE, nullptr) {
	stats->statistics.SetHasNoNullFast();
}

idx_t RowNumberColumnData::GetRowNumberBase(ColumnScanState &state) {
	return state.parent->row_number_base.GetIndex();
}

FilterPropagateResult RowNumberColumnData::CheckZonemap(ColumnScanState &state, TableFilter &filter,
                                                        optional_ptr<SegmentNode<ColumnSegment>> &checked_segment) {
	// row_number columns don't have zonemaps - we cannot prune based on row number
	checked_segment = nullptr;
	return FilterPropagateResult::NO_PRUNING_POSSIBLE;
}

void RowNumberColumnData::InitializePrefetch(PrefetchState &prefetch_state, ColumnScanState &scan_state, idx_t rows) {
}

void RowNumberColumnData::InitializeScan(ColumnScanState &state) {
	InitializeScanWithOffset(state, 0);
}

void RowNumberColumnData::InitializeScanWithOffset(ColumnScanState &state, idx_t row_idx) {
	if (row_idx > count) {
		throw InternalException("row_idx in InitializeScanWithOffset out of range");
	}
	state.current = nullptr;
	state.segment_tree = nullptr;
	state.offset_in_column = 0;
	state.internal_index = 0;
	state.initialized = true;
	state.scan_state.reset();
	state.last_offset = 0;
}

idx_t RowNumberColumnData::Scan(TransactionData transaction, idx_t vector_index, ColumnScanState &state, Vector &result,
                                idx_t scan_count) {
	return ScanCount(state, result, scan_count, 0);
}

void RowNumberColumnData::ScanCommittedRange(idx_t row_group_start, idx_t offset_in_row_group, idx_t count,
                                             Vector &result, VisibilityBound visibility_bound) {
	throw InternalException("ScanCommittedRange is not supported for row number columns");
}

idx_t RowNumberColumnData::ScanCount(ColumnScanState &state, Vector &result, idx_t count, idx_t result_offset) {
	if (result_offset != 0) {
		throw InternalException("RowNumberColumnData result_offset must be 0");
	}
	auto base = GetRowNumberBase(state);
	// row_number is 1-indexed
	result.Sequence(NumericCast<int64_t>(base + state.offset_in_column + 1), 1, count);
	state.offset_in_column += count;
	return count;
}

void RowNumberColumnData::Select(TransactionData transaction, idx_t vector_index, ColumnScanState &state,
                                 Vector &result, SelectionVector &sel, idx_t count) {
	auto base = GetRowNumberBase(state);
	// row_number is a dense sequence - deleted/filtered rows don't get numbers
	// 1-indexed
	result.Sequence(NumericCast<int64_t>(base + state.offset_in_column + 1), 1, count);
	state.offset_in_column += count;
}

idx_t RowNumberColumnData::Fetch(ColumnScanState &state, row_t row_id, Vector &result) {
	throw InternalException("Fetch is not supported for row number columns");
}

void RowNumberColumnData::FetchRows(TransactionData transaction, ColumnFetchState &state,
                                    const StorageIndex &storage_index, const idx_t *offsets, const SelectionVector &sel,
                                    idx_t fetch_count, Vector &result, idx_t result_offset) {
	throw InternalException("FetchRows is not supported for row number columns");
}

void RowNumberColumnData::Skip(ColumnScanState &state, idx_t count) {
	// row_number is dense - skipped rows don't consume row numbers
	// so Skip is a no-op (offset_in_column is not advanced)
}

void RowNumberColumnData::InitializeAppend(ColumnAppendState &state) {
	throw InternalException("RowNumberColumnData cannot be appended to");
}

void RowNumberColumnData::Append(ColumnAppendState &state, const Vector &vector, idx_t count) {
	throw InternalException("RowNumberColumnData cannot be appended to");
}

void RowNumberColumnData::AppendData(ColumnAppendState &state, UnifiedVectorFormat &vdata, idx_t count) {
	throw InternalException("RowNumberColumnData cannot be appended to");
}

void RowNumberColumnData::RevertAppend(row_t new_count) {
	throw InternalException("RowNumberColumnData cannot be appended to");
}

void RowNumberColumnData::Update(TransactionData transaction, DuckTableEntry &table_entry, idx_t column_index,
                                 Vector &update_vector, row_t *row_ids, idx_t update_count, idx_t row_group_start) {
	throw InternalException("RowNumberColumnData cannot be updated");
}

void RowNumberColumnData::UpdateColumn(TransactionData transaction, DuckTableEntry &table_entry,
                                       const vector<column_t> &column_path, Vector &update_vector, row_t *row_ids,
                                       idx_t update_count, idx_t depth, idx_t row_group_start) {
	throw InternalException("RowNumberColumnData cannot be updated");
}

void RowNumberColumnData::VisitBlockIds(BlockIdVisitor &visitor) const {
	throw InternalException("VisitBlockIds not supported for row number");
}

unique_ptr<ColumnCheckpointState>
RowNumberColumnData::CreateCheckpointState(const RowGroup &row_group, PartialBlockManager &partial_block_manager) {
	throw InternalException("RowNumberColumnData cannot be checkpointed");
}

unique_ptr<ColumnCheckpointState> RowNumberColumnData::Checkpoint(const RowGroup &row_group, ColumnCheckpointInfo &info,
                                                                  const BaseStatistics &old_stats) {
	throw InternalException("RowNumberColumnData cannot be checkpointed");
}

void RowNumberColumnData::CheckpointScan(ColumnSegment &segment, ColumnScanState &state, idx_t count,
                                         Vector &scan_vector, VisibilityBound visibility_bound) const {
	throw InternalException("RowNumberColumnData cannot be checkpointed");
}

bool RowNumberColumnData::IsPersistent() {
	throw InternalException("RowNumberColumnData cannot be persisted");
}

} // namespace duckdb
