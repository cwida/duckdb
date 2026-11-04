//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/table/row_id_column_data.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/storage/table/column_data.hpp"

namespace duckdb {

class RowIdColumnData : public ColumnData {
public:
	RowIdColumnData(BlockManager &block_manager, DataTableInfo &info);

public:
	void InitializePrefetch(PrefetchState &prefetch_state, ColumnScanState &scan_state, idx_t rows) override;
	void InitializeScan(ColumnScanState &state) override;
	void InitializeScanWithOffset(ColumnScanState &state, idx_t row_idx) override;

	idx_t Scan(TransactionData transaction, idx_t vector_index, ColumnScanState &state, Vector &result,
	           idx_t scan_count) override;
	void ScanCommittedRange(idx_t row_group_start, idx_t offset_in_row_group, idx_t count, Vector &result,
	                        VisibilityBound visibility_bound) override;
	idx_t ScanCount(ColumnScanState &state, Vector &result, idx_t count, idx_t result_offset = 0) override;

	void Filter(TransactionData transaction, idx_t vector_index, ColumnScanState &state, Vector &result,
	            SelectionVector &sel, idx_t &count, const TableFilter &filter, TableFilterState &filter_state) override;
	void Select(TransactionData transaction, idx_t vector_index, ColumnScanState &state, Vector &result,
	            SelectionVector &sel, idx_t count) override;

	idx_t Fetch(ColumnScanState &state, row_t row_id, Vector &result) override;
	void FetchRows(TransactionData transaction, ColumnFetchState &state, const StorageIndex &storage_index,
	               const idx_t *offsets, const SelectionVector &sel, idx_t count, Vector &result,
	               idx_t result_offset) override;

	void Skip(ColumnScanState &state, idx_t count = STANDARD_VECTOR_SIZE) override;

	FilterPropagateResult CheckZonemap(ColumnScanState &state, TableFilter &filter,
	                                   optional_ptr<SegmentNode<ColumnSegment>> &checked_segment) override;

	void InitializeAppend(ColumnAppendState &state) override;
	void Append(ColumnAppendState &state, const Vector &vector, idx_t count) override;
	void AppendData(ColumnAppendState &state, UnifiedVectorFormat &vdata, idx_t count) override;
	void RevertAppend(row_t new_count) override;

	void Update(TransactionData transaction, DuckTableEntry &table_entry, idx_t column_index, Vector &update_vector,
	            row_t *row_ids, idx_t update_count, idx_t row_group_start) override;
	void UpdateColumn(TransactionData transaction, DuckTableEntry &table_entry, const vector<column_t> &column_path,
	                  Vector &update_vector, row_t *row_ids, idx_t update_count, idx_t depth,
	                  idx_t row_group_start) override;

	void VisitBlockIds(BlockIdVisitor &visitor) const override;

	unique_ptr<ColumnCheckpointState> CreateCheckpointState(const RowGroup &row_group,
	                                                        PartialBlockManager &partial_block_manager) override;
	unique_ptr<ColumnCheckpointState> Checkpoint(const RowGroup &row_group, ColumnCheckpointInfo &info,
	                                             const BaseStatistics &old_stats) override;

	void CheckpointScan(ColumnSegment &segment, ColumnScanState &state, idx_t count, Vector &scan_vector,
	                    VisibilityBound visibility_bound) const override;

	bool IsPersistent() override;

	idx_t GetRowStart(ColumnScanState &state);
};

} // namespace duckdb
