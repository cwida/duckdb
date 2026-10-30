#include "duckdb/storage/table/validity_column_data.hpp"
#include "duckdb/storage/table/column_checkpoint_state.hpp"
#include "duckdb/storage/table/scan_state.hpp"

namespace duckdb {

ValidityColumnData::ValidityColumnData(BlockManager &block_manager, DataTableInfo &info, idx_t column_index,
                                       ColumnDataType data_type, optional_ptr<ColumnData> parent)
    : ColumnData(block_manager, info, column_index, LogicalType(LogicalTypeId::VALIDITY), data_type, parent) {
}

ValidityColumnData::ValidityColumnData(BlockManager &block_manager, DataTableInfo &info, idx_t column_index,
                                       ColumnData &parent)
    : ValidityColumnData(block_manager, info, column_index, parent.GetDataType(), parent) {
}

void ValidityColumnData::UpdateWithBase(TransactionData transaction, DuckTableEntry &table_entry, idx_t column_index,
                                        Vector &update_vector, row_t *row_ids, idx_t update_count, ColumnData &base,
                                        idx_t row_group_start) {
	Vector base_vector(base.type);
	ColumnScanState validity_scan_state(nullptr);
	FetchUpdateData(validity_scan_state, row_ids, update_count, base_vector, row_group_start);
	auto &segment = data.GetSegment(UnsafeNumericCast<idx_t>(row_ids[0]) - row_group_start)->GetNode();
	if (segment.GetCompressionFunction().type == CompressionType::COMPRESSION_EMPTY) {
		// The validity is actually covered by the data, so we read it to get the validity for UpdateInternal.
		ColumnScanState data_scan_state(nullptr);
		base.FetchUpdateData(data_scan_state, row_ids, update_count, base_vector, row_group_start);
	}

	UpdateInternal(transaction, table_entry, column_index, update_vector, row_ids, update_count, base_vector,
	               row_group_start);
}

void ValidityColumnData::AppendData(ColumnAppendState &state, UnifiedVectorFormat &vdata, idx_t count) {
	ColumnData::AppendData(state, vdata, count);
}

struct ValidityColumnCheckpointState : public ColumnCheckpointState {
	ValidityColumnCheckpointState(const RowGroup &row_group, ColumnData &column_data,
	                              PartialBlockManager &partial_block_manager)
	    : ColumnCheckpointState(row_group, column_data, partial_block_manager) {
	}

public:
	shared_ptr<ColumnData> CreateEmptyColumnData() override {
		return make_shared_ptr<ValidityColumnData>(original_column.GetBlockManager(), original_column.GetTableInfo(),
		                                           original_column.column_index, ColumnDataType::CHECKPOINT_TARGET,
		                                           nullptr);
	}
};

unique_ptr<ColumnCheckpointState>
ValidityColumnData::CreateCheckpointState(const RowGroup &row_group, PartialBlockManager &partial_block_manager) {
	return make_uniq<ValidityColumnCheckpointState>(row_group, *this, partial_block_manager);
}

void ValidityColumnData::Verify(RowGroup &parent) {
	D_ASSERT(HasParent());
	ColumnData::Verify(parent);
}

} // namespace duckdb
