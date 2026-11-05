//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/table/numeric_column_data.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/storage/table/column_data.hpp"
#include "duckdb/storage/table/numeric_range.hpp"

namespace duckdb {

//! A NUMERIC column stores every row group in one exact representation (an integer type or a DECIMAL) that holds the
//! values of the row group. Appends widen the representation when needed and the checkpoint re-picks the narrowest
//! one. Scans deliver the stored representation as the run-time representation of the NUMERIC vectors.
class NumericColumnData final : public ColumnData {
public:
	NumericColumnData(BlockManager &block_manager, DataTableInfo &info, idx_t column_index, LogicalType type,
	                  ColumnDataType data_type, optional_ptr<ColumnData> parent);

	//! The representation the data is currently stored in
	LogicalType GetStorageType() const;
	shared_ptr<ColumnData> GetInner() const;
	//! The exact range of the values appended so far (deleted rows are not removed from it)
	NumericRange GetRange() const;
	//! Replace the inner column (used by the checkpoint)
	void SetInner(shared_ptr<ColumnData> inner);
	//! Set the exact range of the values (used by the checkpoint, which keeps the range of the checkpointed rows)
	void SetRange(const NumericRange &range);

public:
	idx_t GetMaxEntry() override;

	void InitializePrefetch(PrefetchState &prefetch_state, ColumnScanState &scan_state, idx_t start_row,
	                        idx_t end_row) override;
	void InitializeScan(ColumnScanState &state) override;
	void InitializeScanWithOffset(ColumnScanState &state, idx_t row_idx) override;
	idx_t Scan(TransactionData transaction, idx_t vector_index, ColumnScanState &state, Vector &result,
	           idx_t scan_count) override;
	idx_t ScanCount(ColumnScanState &state, Vector &result, idx_t count, idx_t result_offset = 0) override;
	void Skip(ColumnScanState &state, idx_t count = STANDARD_VECTOR_SIZE) override;

	FilterPropagateResult CheckZonemap(ColumnScanState &state, const ScanFilter &filter,
	                                   optional_idx &checked_end) override;
	FilterPropagateResult CheckZonemapAt(ColumnScanState &state, const ScanFilter &filter, idx_t row,
	                                     optional_idx &checked_end) override;

	void InitializeAppend(ColumnAppendState &state) override;
	void Append(ColumnAppendState &state, const Vector &vector, idx_t count) override;
	void FinalizeAppend(ColumnDataFinalizeAppendState &finalize_state, ColumnAppendState &state) override;
	void RevertAppend(row_t new_count) override;

	void FetchRows(TransactionData transaction, ColumnFetchState &state, const StorageIndex &storage_index,
	               const idx_t *offsets, const SelectionVector &sel, idx_t fetch_count, Vector &result,
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
	//! Cast between exact representations
	static void CastVector(const Vector &source, Vector &target, idx_t count);
	//! The inner column pinned by a scan: scans keep reading the representation they started on
	ColumnData &Pinned(ColumnScanState &state);
	//! Pins the current inner column and its value range for a scan
	void Pin(ColumnScanState &state);
	shared_ptr<ColumnData> CreateInner(const LogicalType &storage_type);
	//! A copy of the source in another representation
	shared_ptr<ColumnData> Rebuild(ColumnData &source, const LogicalType &new_type);
	//! Switch the inner column to a wider representation in the middle of an append
	void Widen(ColumnAppendState &state, const LogicalType &new_type);
	//! The representation the column takes to hold its values and those of an appended vector
	LogicalType AppendTargetType(ColumnAppendState &state, const LogicalType &current_type,
	                             const LogicalType &source_type);
	//! Append a vector of an exact representation
	void AppendTyped(ColumnAppendState &state, const Vector &typed, idx_t count);
	//! Append canonical values that have no common representation
	void AppendCanonical(ColumnAppendState &state, const Vector &canonical, idx_t count);
	//! Checks the filter against the canonical statistics of a segment of the representation and of the updates
	static FilterPropagateResult CheckCanonicalStatistics(ColumnData &column, optional_ptr<ClientContext> context,
	                                                      SegmentNode<ColumnSegment> &segment,
	                                                      ExpressionFilter &expr_filter);

private:
	mutable mutex inner_lock;
	shared_ptr<ColumnData> inner;
	//! Columns replaced by widening: scans that started on them keep them alive
	vector<shared_ptr<ColumnData>> retired;
	//! The exact range of the values, kept across appends and persisted with the row group
	NumericRange range;
};

} // namespace duckdb
