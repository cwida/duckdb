#include "duckdb/storage/table/row_id_column_data.hpp"

#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/storage/table/scan_state.hpp"

namespace duckdb {

RowIdColumnData::RowIdColumnData(BlockManager &block_manager, DataTableInfo &info)
    : ColumnData(block_manager, info, COLUMN_IDENTIFIER_ROW_ID, LogicalType(LogicalTypeId::BIGINT),
                 ColumnDataType::MAIN_TABLE, nullptr) {
	stats->statistics.SetHasNoNullFast();
}

idx_t RowIdColumnData::GetRowStart(ColumnScanState &state) {
	return state.parent->GetRowGroup()->GetRowStart();
}

FilterPropagateResult RowIdColumnData::CheckZonemap(ColumnScanState &state, TableFilter &filter,
                                                    optional_ptr<SegmentNode<ColumnSegment>> &checked_segment) {
	checked_segment = nullptr;
	auto row_start = GetRowStart(state);
	return RowGroup::CheckRowIdFilter(filter, row_start, row_start + count);
}

void RowIdColumnData::InitializePrefetch(PrefetchState &prefetch_state, ColumnScanState &scan_state, idx_t rows) {
}

void RowIdColumnData::InitializeScan(ColumnScanState &state) {
	InitializeScanWithOffset(state, 0);
}

void RowIdColumnData::InitializeScanWithOffset(ColumnScanState &state, idx_t row_idx) {
	if (row_idx > count) {
		throw InternalException("row_idx in InitializeScanWithOffset out of range");
	}
	state.current = nullptr;
	state.segment_tree = nullptr;
	state.offset_in_column = row_idx;
	state.internal_index = state.offset_in_column;
	state.initialized = true;
	state.scan_state.reset();
	state.last_offset = 0;
}

idx_t RowIdColumnData::Scan(TransactionData transaction, idx_t vector_index, ColumnScanState &state, Vector &result,
                            idx_t scan_count) {
	return ScanCount(state, result, scan_count, 0);
}

void RowIdColumnData::ScanCommittedRange(idx_t row_group_start, idx_t offset_in_row_group, idx_t count,
                                         Vector &result) {
	result.Sequence(UnsafeNumericCast<int64_t>(row_group_start + offset_in_row_group), 1, count);
}

idx_t RowIdColumnData::ScanCount(ColumnScanState &state, Vector &result, idx_t count, idx_t result_offset) {
	auto row_start = GetRowStart(state);
	if (result_offset != 0) {
		throw InternalException("RowIdColumnData result_offset must be 0");
	}
	ScanCommittedRange(row_start, state.offset_in_column, count, result);
	state.offset_in_column += count;
	return count;
}

void RowIdColumnData::Filter(TransactionData transaction, idx_t vector_index, ColumnScanState &state, Vector &result,
                             SelectionVector &sel, idx_t &count, const TableFilter &filter,
                             TableFilterState &filter_state) {
	auto row_start = GetRowStart(state);
	auto current_row = row_start + state.offset_in_column;
	auto max_count = GetVectorCount(vector_index);
	state.offset_in_column += max_count;
	// We do another quick statistics scan for row ids here
	const auto rowid_start = current_row;
	const auto rowid_end = current_row + max_count;
	const auto prune_result = RowGroup::CheckRowIdFilter(filter, rowid_start, rowid_end);
	if (prune_result == FilterPropagateResult::FILTER_ALWAYS_FALSE) {
		// We can just break out of the loop here.
		count = 0;
		return;
	}

	// Generate row ids
	// Create sequence for row ids
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = FlatVector::ScatterWriter<row_t>(result);
	for (size_t sel_idx = 0; sel_idx < count; sel_idx++) {
		result_data[sel.get_index(sel_idx)] = UnsafeNumericCast<int64_t>(current_row + sel.get_index(sel_idx));
	}
	// the writes above scatter into positions sel[0..count) which can be anywhere in [0, max_count),
	// so the vector's logical size must cover the full max_count - using `count` would leave any
	// sel index >= count looking out-of-bounds when later slices read through sel
	FlatVector::SetSize(result, count_t(max_count));

	// Was this filter always true? If so, we dont need to apply it
	if (prune_result == FilterPropagateResult::FILTER_ALWAYS_TRUE) {
		return;
	}

	// Now apply the filter
	ColumnSegment::FilterSelection(sel, result, filter_state, count, count);
}

void RowIdColumnData::Select(TransactionData transaction, idx_t vector_index, ColumnScanState &state, Vector &result,
                             SelectionVector &sel, idx_t count) {
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = FlatVector::Writer<row_t>(result, count);
	auto row_start = GetRowStart(state);
	for (size_t sel_idx = 0; sel_idx < count; sel_idx++) {
		result_data.WriteValue(UnsafeNumericCast<row_t>(row_start + state.offset_in_column + sel.get_index(sel_idx)));
	}
	state.offset_in_column += GetVectorCount(vector_index);
}

idx_t RowIdColumnData::Fetch(ColumnScanState &state, row_t row_id, Vector &result) {
	throw InternalException("Fetch is not supported for row id columns");
}

void RowIdColumnData::FetchRows(TransactionData transaction, ColumnFetchState &state, const StorageIndex &storage_index,
                                const idx_t *offsets, const SelectionVector &sel, idx_t fetch_count, Vector &result,
                                idx_t result_offset) {
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto data = FlatVector::GetDataMutable<row_t>(result);
	auto row_start = state.row_group->GetRowStart();
	for (idx_t idx = 0; idx < fetch_count; idx++) {
		data[result_offset + idx] = NumericCast<row_t>(row_start + offsets[sel.get_index(idx)]);
	}
}

void RowIdColumnData::Skip(ColumnScanState &state, idx_t count) {
	state.offset_in_column += count;
	state.internal_index = state.offset_in_column;
}

void RowIdColumnData::InitializeAppend(ColumnAppendState &state) {
	throw InternalException("RowIdColumnData cannot be appended to");
}

void RowIdColumnData::Append(ColumnAppendState &state, const Vector &vector, idx_t count) {
	throw InternalException("RowIdColumnData cannot be appended to");
}

void RowIdColumnData::AppendData(ColumnAppendState &state, UnifiedVectorFormat &vdata, idx_t count) {
	throw InternalException("RowIdColumnData cannot be appended to");
}

void RowIdColumnData::RevertAppend(row_t new_count) {
	throw InternalException("RowIdColumnData cannot be appended to");
}

void RowIdColumnData::Update(TransactionData transaction, DuckTableEntry &table_entry, idx_t column_index,
                             Vector &update_vector, row_t *row_ids, idx_t update_count, idx_t row_group_start) {
	throw InternalException("RowIdColumnData cannot be updated");
}

void RowIdColumnData::UpdateColumn(TransactionData transaction, DuckTableEntry &table_entry,
                                   const vector<column_t> &column_path, Vector &update_vector, row_t *row_ids,
                                   idx_t update_count, idx_t depth, idx_t row_group_start) {
	throw InternalException("RowIdColumnData cannot be updated");
}

void RowIdColumnData::VisitBlockIds(BlockIdVisitor &visitor) const {
	throw InternalException("VisitBlockIds not supported for rowid");
}

unique_ptr<ColumnCheckpointState> RowIdColumnData::CreateCheckpointState(const RowGroup &row_group,
                                                                         PartialBlockManager &partial_block_manager) {
	throw InternalException("RowIdColumnData cannot be checkpointed");
}

unique_ptr<ColumnCheckpointState> RowIdColumnData::Checkpoint(const RowGroup &row_group, ColumnCheckpointInfo &info,
                                                              const BaseStatistics &old_stats) {
	throw InternalException("RowIdColumnData cannot be checkpointed");
}

void RowIdColumnData::CheckpointScan(ColumnSegment &segment, ColumnScanState &state, idx_t count,
                                     Vector &scan_vector) const {
	throw InternalException("RowIdColumnData cannot be checkpointed");
}

bool RowIdColumnData::IsPersistent() {
	throw InternalException("RowIdColumnData cannot be persisted");
}

} // namespace duckdb
