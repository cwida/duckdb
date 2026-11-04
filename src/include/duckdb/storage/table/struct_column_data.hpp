//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/table/struct_column_data.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/storage/table/column_data.hpp"
#include "duckdb/storage/table/validity_column_data.hpp"

namespace duckdb {

//! Struct column data represents a struct
class StructColumnData : public ColumnData {
public:
	struct StructColumnDataChild {
	public:
		StructColumnDataChild(ColumnData &col, optional_idx vector_index, ColumnScanState &child, bool should_scan)
		    : col(col), vector_index(vector_index), state(child), should_scan(should_scan) {
		}

	public:
		ColumnData &col;
		optional_idx vector_index;
		ColumnScanState &state;
		bool should_scan;
	};

public:
	StructColumnData(BlockManager &block_manager, DataTableInfo &info, idx_t column_index, LogicalType type,
	                 ColumnDataType data_type, optional_ptr<ColumnData> parent);

public:
	void SetDataType(ColumnDataType data_type) override;
	idx_t GetMaxEntry() override;
	FilterPropagateResult CheckZonemap(ColumnScanState &state, TableFilter &filter,
	                                   optional_ptr<SegmentNode<ColumnSegment>> &checked_segment) override;

	void InitializePrefetch(PrefetchState &prefetch_state, ColumnScanState &scan_state, idx_t rows) override;
	void InitializeScan(ColumnScanState &state) override;
	void InitializeScanWithOffset(ColumnScanState &state, idx_t row_idx) override;

	vector<StructColumnDataChild> GetStructChildren(ColumnScanState &state) const;

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

	void VisitBlockIds(BlockIdVisitor &visitor) const override;

	unique_ptr<ColumnCheckpointState> CreateCheckpointState(const RowGroup &row_group,
	                                                        PartialBlockManager &partial_block_manager) override;
	unique_ptr<ColumnCheckpointState> Checkpoint(const RowGroup &row_group, ColumnCheckpointInfo &info,
	                                             const BaseStatistics &old_stats) override;

	bool IsPersistent() override;
	bool HasAnyChanges() const override;
	bool HasInexactStatistics() const override;
	PersistentColumnData Serialize() override;
	void InitializeColumn(PersistentColumnData &column_data, BaseStatistics &target_stats) override;

	void GetColumnSegmentInfo(const QueryContext &context, duckdb::idx_t row_group_index,
	                          vector<duckdb::idx_t> col_path, vector<duckdb::ColumnSegmentInfo> &result,
	                          const ColumnSegmentInfoScanOptions &options) override;

	void Verify(RowGroup &parent) override;

	void SetValidityData(shared_ptr<ValidityColumnData> validity_p);
	void SetChildData(idx_t i, shared_ptr<ColumnData> child_column_p);
	const ColumnData &GetChildColumn(idx_t index) const;

	const BaseStatistics &GetChildStats(const ColumnData &child) const override;

protected:
	//! The sub-columns of the struct
	vector<shared_ptr<ColumnData>> sub_columns;
	//! The validity column data of the struct
	shared_ptr<ValidityColumnData> validity;
};

} // namespace duckdb
