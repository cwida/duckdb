//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/table/variant_column_data.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/storage/table/column_data.hpp"
#include "duckdb/storage/table/validity_column_data.hpp"

namespace duckdb {

//! Struct column data represents a struct
class VariantColumnData : public ColumnData {
public:
	//! Indices into the inner shredded struct: STRUCT(typed_value <type>, untyped_value_index UINTEGER)
	static constexpr idx_t TYPED_VALUE_INDEX = 0;
	static constexpr idx_t UNTYPED_VALUE_INDEX = 1;

public:
	VariantColumnData(BlockManager &block_manager, DataTableInfo &info, idx_t column_index, LogicalType type,
	                  ColumnDataType data_type, optional_ptr<ColumnData> parent);

	//! The sub-columns of the struct
	vector<shared_ptr<ColumnData>> sub_columns;
	shared_ptr<ValidityColumnData> validity;

public:
	idx_t GetMaxEntry() override;
	bool IsShredded() const {
		return sub_columns.size() == 2;
	}

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

	void VisitBlockIds(BlockIdVisitor &visitor) const override;

	unique_ptr<ColumnCheckpointState> CreateCheckpointState(const RowGroup &row_group,
	                                                        PartialBlockManager &partial_block_manager) override;
	unique_ptr<ColumnCheckpointState> Checkpoint(const RowGroup &row_group, ColumnCheckpointInfo &info,
	                                             const BaseStatistics &old_stats) override;

	bool IsPersistent() override;
	bool HasAnyChanges() const override;
	PersistentColumnData Serialize() override;
	void InitializeColumn(PersistentColumnData &column_data, BaseStatistics &target_stats) override;

	void GetColumnSegmentInfo(const QueryContext &context, duckdb::idx_t row_group_index,
	                          vector<duckdb::idx_t> col_path, vector<duckdb::ColumnSegmentInfo> &result,
	                          const ColumnSegmentInfoScanOptions &options) override;

	void Verify(RowGroup &parent) override;

	static void ShredVariantData(const Vector &input, Vector &output, idx_t count);
	//! Debug/verification helper: shred a (top-level) VARIANT vector in place, deriving the shredding
	//! schema from the first value (so inconsistent values become partially shredded). No-op if the
	//! vector is already shredded or the first value yields no shreddable type.
	static void DebugShred(Vector &variant, idx_t count);

	void SetValidityData(shared_ptr<ValidityColumnData> validity_p);
	void SetChildData(vector<shared_ptr<ColumnData>> child_data);

private:
	Vector CreateUnshreddingIntermediate(idx_t count) const;
	vector<shared_ptr<ColumnData>> WriteShreddedData(const RowGroup &row_group, const LogicalType &shredded_type,
	                                                 BaseStatistics &stats);
	bool PushdownShreddedFieldExtract(const StorageIndex &variant_extract, StorageIndex &out_struct_extract) const;
	void CreateScanStates(ColumnScanState &state);
	idx_t ScanWithCallback(ColumnScanState &state, Vector &result, idx_t target_count,
	                       const std::function<idx_t(ColumnData &column, ColumnScanState &child_state,
	                                                 Vector &target_vector, idx_t count)> &callback) const;
	LogicalType GetShreddedType();
};

} // namespace duckdb
