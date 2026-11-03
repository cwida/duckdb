//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/table/geo_column_data.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/storage/table/column_data.hpp"
#include "duckdb/storage/table/validity_column_data.hpp"

namespace duckdb {

class GeoColumnData final : public ColumnData {
public:
	GeoColumnData(BlockManager &block_manager, DataTableInfo &info, idx_t column_index, LogicalType type,
	              ColumnDataType data_type, optional_ptr<ColumnData> parent);

	//! The actual column data
	//! The "shape" of this might differ depending on if we "shred" this column or not.
	shared_ptr<ColumnData> base_column;

	//! Shredding state of the column
	GeometryStorageType storage_type = GeometryStorageType::WKB;

public:
	idx_t GetMaxEntry() override;

	void InitializeChildScanStates(ColumnScanState &state);

	void InitializePrefetch(PrefetchState &prefetch_state, ColumnScanState &scan_state, idx_t start_row,
	                        idx_t end_row) override;
	void InitializeScan(ColumnScanState &state) override;
	void InitializeScanWithOffset(ColumnScanState &state, idx_t row_idx) override;

	idx_t Scan(TransactionData transaction, idx_t vector_index, ColumnScanState &state, Vector &result,
	           idx_t scan_count) override;

	idx_t ScanCount(ColumnScanState &state, Vector &result, idx_t count, idx_t result_offset = 0) override;

	void Skip(ColumnScanState &state, idx_t count = STANDARD_VECTOR_SIZE) override;

	void InitializeAppend(ColumnAppendState &state) override;
	void Append(ColumnAppendState &state, const Vector &vector, idx_t count) override;
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

	unique_ptr<ColumnCheckpointState> CreateCheckpointState(const RowGroup &row_group,
	                                                        PartialBlockManager &partial_block_manager) override;
	unique_ptr<ColumnCheckpointState> Checkpoint(const RowGroup &row_group, ColumnCheckpointInfo &info,
	                                             const BaseStatistics &old_stats) override;

	bool IsPersistent() override;
	bool HasAnyChanges() const override;
	PersistentColumnData Serialize() override;
	void InitializeColumn(PersistentColumnData &column_data, BaseStatistics &target_stats) override;

	void GetColumnSegmentInfo(const QueryContext &context, idx_t row_group_index, vector<idx_t> col_path,
	                          vector<ColumnSegmentInfo> &result, const ColumnSegmentInfoScanOptions &options) override;

	void Verify(RowGroup &parent) override;

	void VisitBlockIds(BlockIdVisitor &visitor) const override;

private:
	static void Specialize(const Vector &source, Vector &target, idx_t count, GeometryStorageType storage_type);
	static void Reassemble(const Vector &source, Vector &target, idx_t count, GeometryStorageType storage_type,
	                       idx_t offset);
	static void InterpretStats(const BaseStatistics &source, BaseStatistics &target, GeometryType geom_type,
	                           VertexType vert_type);
};

} // namespace duckdb
