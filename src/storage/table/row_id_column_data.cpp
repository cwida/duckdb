#include "duckdb/storage/table/row_id_column_data.hpp"

#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
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

//! Optional filters are not executed, but the row ids they reject still let the scan skip vectors
static bool PrunesVectors(const ScanFilter &filter) {
	return !filter.IsAlwaysTrue() || ExpressionFilter::IsRootNonSelectivityOptionalFilter(filter.filter);
}

static bool RejectsRowIds(const TableFilter &filter, idx_t start_row_id, idx_t end_row_id) {
	const auto prune_result = RowGroup::CheckRowIdFilter(filter, start_row_id, end_row_id);
	return prune_result == FilterPropagateResult::FILTER_ALWAYS_FALSE ||
	       prune_result == FilterPropagateResult::FILTER_FALSE_OR_NULL;
}

FilterPropagateResult RowIdColumnData::CheckZonemap(ColumnScanState &state, const ScanFilter &filter,
                                                    optional_idx &checked_end) {
	return CheckZonemapAt(state, filter, state.parent->vector_index * STANDARD_VECTOR_SIZE, checked_end);
}

FilterPropagateResult RowIdColumnData::CheckZonemapAt(ColumnScanState &state, const ScanFilter &filter, idx_t row,
                                                      optional_idx &checked_end) {
	if (!PrunesVectors(filter)) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	// the row ids of each vector are known, so look ahead for the first vector with a row id the filter accepts
	const auto row_group_start = GetRowStart(state);
	const idx_t scan_end = state.parent->max_row_group_row;
	idx_t vector_start = row;
	while (vector_start < scan_end) {
		const idx_t vector_end = MinValue<idx_t>(AlignValue<idx_t, STANDARD_VECTOR_SIZE>(vector_start + 1), scan_end);
		if (!RejectsRowIds(filter.filter, row_group_start + vector_start, row_group_start + vector_end)) {
			break;
		}
		vector_start = vector_end;
	}
	if (vector_start == row) {
		// the vector of row can hold row ids the filter accepts, the next vector has to be checked again
		checked_end = MinValue<idx_t>(AlignValue<idx_t, STANDARD_VECTOR_SIZE>(row + 1), scan_end);
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	checked_end = vector_start;
	return FilterPropagateResult::FILTER_ALWAYS_FALSE;
}

void RowIdColumnData::InitializePrefetch(PrefetchState &prefetch_state, ColumnScanState &scan_state, idx_t start_row,
                                         idx_t end_row) {
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
