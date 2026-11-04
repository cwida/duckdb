//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/table/standard_column_data.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/storage/table/column_data.hpp"
#include "duckdb/storage/table/validity_column_data.hpp"

namespace duckdb {

//! Standard column data represents a regular flat column (e.g. a column of type INTEGER or STRING)
class StandardColumnData : public ColumnData {
public:
	StandardColumnData(BlockManager &block_manager, DataTableInfo &info, idx_t column_index, LogicalType type,
	                   ColumnDataType data_type, optional_ptr<ColumnData> parent);

public:
	void SetDataType(ColumnDataType data_type) override;

	ScanVectorType GetVectorScanType(ColumnScanState &state, idx_t scan_count, Vector &result) override;
	void InitializePrefetch(PrefetchState &prefetch_state, ColumnScanState &scan_state, idx_t rows) override;
	void InitializeScan(ColumnScanState &state) override;
	void InitializeScanWithOffset(ColumnScanState &state, idx_t row_idx) override;

	idx_t Scan(TransactionData transaction, idx_t vector_index, ColumnScanState &state, Vector &result,
	           idx_t target_count) override;
	idx_t ScanCount(ColumnScanState &state, Vector &result, idx_t count, idx_t result_offset) override;

	void Filter(TransactionData transaction, idx_t vector_index, ColumnScanState &state, Vector &result,
	            SelectionVector &sel, idx_t &count, const TableFilter &filter, TableFilterState &filter_state) override;
	void Select(TransactionData transaction, idx_t vector_index, ColumnScanState &state, Vector &result,
	            SelectionVector &sel, idx_t sel_count) override;

	void InitializeAppend(ColumnAppendState &state) override;
	void AppendData(ColumnAppendState &state, UnifiedVectorFormat &vdata, idx_t count) override;
	void FinalizeAppend(ColumnDataFinalizeAppendState &finalize_state, ColumnAppendState &state) override;
	void RevertAppend(row_t new_count) override;
	idx_t Fetch(ColumnScanState &state, row_t row_id, Vector &result) override;
	void FetchRows(TransactionData transaction, ColumnFetchState &state, const StorageIndex &storage_index,
	               const idx_t *offsets, const SelectionVector &sel, idx_t count, Vector &result,
	               idx_t result_offset) override;
	void Update(TransactionData transaction, DuckTableEntry &table_entry, idx_t column_index, Vector &update_vector,
	            row_t *row_ids, idx_t update_count, idx_t row_group_start) override;
	void UpdateColumn(TransactionData transaction, DuckTableEntry &table_entry, const vector<column_t> &column_path,
	                  Vector &update_vector, row_t *row_ids, idx_t update_count, idx_t depth,
	                  idx_t row_group_start) override;
	unique_ptr<BaseStatistics> GetUpdateStatistics() override;

	void VisitBlockIds(BlockIdVisitor &visitor) const override;

	unique_ptr<ColumnCheckpointState> CreateCheckpointState(const RowGroup &row_group,
	                                                        PartialBlockManager &partial_block_manager) override;
	unique_ptr<ColumnCheckpointState> Checkpoint(const RowGroup &row_group, ColumnCheckpointInfo &info,
	                                             const BaseStatistics &stats) override;
	void CheckpointScan(ColumnSegment &segment, ColumnScanState &state, idx_t count, Vector &scan_vector,
	                    VisibilityBound visibility_bound) const override;

	void GetColumnSegmentInfo(const QueryContext &context, duckdb::idx_t row_group_index,
	                          vector<duckdb::idx_t> col_path, vector<duckdb::ColumnSegmentInfo> &result,
	                          const ColumnSegmentInfoScanOptions &options) override;

	bool IsPersistent() override;
	bool HasAnyChanges() const override;
	bool HasInexactStatistics() const override;
	PersistentColumnData Serialize() override;
	void InitializeColumn(PersistentColumnData &column_data, BaseStatistics &target_stats) override;

	void Verify(RowGroup &parent) override;

	void SetValidityData(shared_ptr<ValidityColumnData> validity);
	//! Direct access to the validity column data. Intended for extensions that need to walk storage internals.
	ValidityColumnData &GetValidityData();

protected:
	//! The validity column data
	shared_ptr<ValidityColumnData> validity;
};

} // namespace duckdb
