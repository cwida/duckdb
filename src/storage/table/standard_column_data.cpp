#include "duckdb/storage/table/standard_column_data.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/storage/table/update_segment.hpp"
#include "duckdb/storage/table/append_state.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/column_checkpoint_state.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/storage/table/column_data_checkpointer.hpp"

namespace duckdb {

StandardColumnData::StandardColumnData(BlockManager &block_manager, DataTableInfo &info, idx_t column_index,
                                       LogicalType type, ColumnDataType data_type, optional_ptr<ColumnData> parent)
    : ColumnData(block_manager, info, column_index, std::move(type), data_type, parent) {
	if (data_type != ColumnDataType::CHECKPOINT_TARGET) {
		// don't initialize the child entry if this is a checkpoint target
		validity = make_shared_ptr<ValidityColumnData>(block_manager, info, 0, *this);
	}
}

void StandardColumnData::SetDataType(ColumnDataType data_type) {
	ColumnData::SetDataType(data_type);
	validity->SetDataType(data_type);
}

ScanVectorType StandardColumnData::GetVectorScanType(ColumnScanState &state, idx_t scan_count, Vector &result) {
	// if either the current column data, or the validity column data requires flat vectors, we scan flat vectors
	auto scan_type = ColumnData::GetVectorScanType(state, scan_count, result);
	if (scan_type == ScanVectorType::SCAN_FLAT_VECTOR) {
		return ScanVectorType::SCAN_FLAT_VECTOR;
	}
	if (state.child_states.empty()) {
		return scan_type;
	}
	return validity->GetVectorScanType(state.child_states[0], scan_count, result);
}

void StandardColumnData::InitializePrefetch(PrefetchState &prefetch_state, ColumnScanState &scan_state, idx_t rows) {
	ColumnData::InitializePrefetch(prefetch_state, scan_state, rows);
	validity->InitializePrefetch(prefetch_state, scan_state.child_states[0], rows);
}

void StandardColumnData::InitializeScan(ColumnScanState &state) {
	ColumnData::InitializeScan(state);

	// initialize the validity segment
	D_ASSERT(state.child_states.size() == 1);
	validity->InitializeScan(state.child_states[0]);
}

void StandardColumnData::InitializeScanWithOffset(ColumnScanState &state, idx_t row_idx) {
	ColumnData::InitializeScanWithOffset(state, row_idx);

	// initialize the validity segment
	D_ASSERT(state.child_states.size() == 1);
	validity->InitializeScanWithOffset(state.child_states[0], row_idx);
}

idx_t StandardColumnData::Scan(TransactionData transaction, idx_t vector_index, ColumnScanState &state, Vector &result,
                               idx_t target_count) {
	D_ASSERT(state.offset_in_column == state.child_states[0].offset_in_column);
	auto scan_type = GetVectorScanType(state, target_count, result);
	auto scan_count =
	    ScanVector(transaction, vector_index, state, result, target_count, scan_type, state.update_scan_type);
	validity->ScanVector(transaction, vector_index, state.child_states[0], result, target_count, scan_type,
	                     state.update_scan_type);
	return scan_count;
}

idx_t StandardColumnData::ScanCount(ColumnScanState &state, Vector &result, idx_t count, idx_t result_offset) {
	auto scan_count = ColumnData::ScanCount(state, result, count, result_offset);
	validity->ScanCount(state.child_states[0], result, count, result_offset);
	return scan_count;
}

void StandardColumnData::Filter(TransactionData transaction, idx_t vector_index, ColumnScanState &state, Vector &result,
                                SelectionVector &sel, idx_t &count, const TableFilter &filter,
                                TableFilterState &filter_state) {
	// check if we can do a specialized select
	// the compression functions need to support this
	auto compression = GetCompressionFunction();
	bool has_filter = compression && compression->filter;
	bool filter_includes_validity = compression && compression->validity == CompressionValidity::NO_VALIDITY_REQUIRED;
	auto validity_compression = validity->GetCompressionFunction();
	bool validity_has_filter = filter_includes_validity || (validity_compression && validity_compression->filter);
	auto target_count = GetVectorCount(vector_index);
	auto scan_type = GetVectorScanType(state, target_count, result);
	bool scan_entire_vector = scan_type == ScanVectorType::SCAN_ENTIRE_VECTOR;
	bool verify_fetch_row = state.scan_options && state.scan_options->force_fetch_row;
	if (!has_filter || !validity_has_filter || !scan_entire_vector || verify_fetch_row || filter_state.can_throw) {
		// we are not scanning an entire vector - this can have several causes (updates, etc)
		// a filter that can throw is excluded as well: the compression-level filters evaluate it over the distinct
		// values of the segment (e.g. the dictionary, or the RLE runs), which includes values of rows that another
		// filter already removed - pushing the filter down there must not raise errors that would not occur otherwise
		ColumnData::Filter(transaction, vector_index, state, result, sel, count, filter, filter_state);
		return;
	}
	FilterVector(state, result, target_count, sel, count, filter, filter_state);
	if (!filter_includes_validity) {
		validity->FilterVector(state.child_states[0], result, target_count, sel, count, filter, filter_state);
	} else {
		validity->Skip(state.child_states[0], target_count);
	}
}

void StandardColumnData::Select(TransactionData transaction, idx_t vector_index, ColumnScanState &state, Vector &result,
                                SelectionVector &sel, idx_t sel_count) {
	// check if we can do a specialized select
	// the compression functions need to support this
	auto compression = GetCompressionFunction();
	bool has_select = compression && compression->select;
	auto validity_compression = validity->GetCompressionFunction();
	bool validity_has_select = validity_compression && validity_compression->select;
	auto target_count = GetVectorCount(vector_index);
	auto scan_type = GetVectorScanType(state, target_count, result);
	if (!has_select || !validity_has_select || HasUpdates() || validity->HasUpdates()) {
		// the updates need to be merged into the entire vector, so we cannot select
		ColumnData::Select(transaction, vector_index, state, result, sel, sel_count);
		return;
	}
	SelectVector(state, result, target_count, sel, sel_count, scan_type);
	validity->SelectVector(state.child_states[0], result, target_count, sel, sel_count, scan_type);
}

void StandardColumnData::InitializeAppend(ColumnAppendState &state) {
	ColumnData::InitializeAppend(state);
	ColumnAppendState child_append;
	validity->InitializeAppend(child_append);
	state.child_appends.push_back(std::move(child_append));
}

void StandardColumnData::AppendData(ColumnAppendState &state, UnifiedVectorFormat &vdata, idx_t count) {
	ColumnData::AppendData(state, vdata, count);
	validity->AppendData(state.child_appends[0], vdata, count);
}

void StandardColumnData::FinalizeAppend(ColumnDataFinalizeAppendState &finalize_state, ColumnAppendState &state) {
	ColumnData::FinalizeAppend(finalize_state, state);
	validity->FinalizeAppendLocked(finalize_state, state.child_appends[0]);
}

void StandardColumnData::RevertAppend(row_t new_count) {
	ColumnData::RevertAppend(new_count);
	validity->RevertAppend(new_count);
}

idx_t StandardColumnData::Fetch(ColumnScanState &state, row_t row_id, Vector &result) {
	// fetch validity mask
	if (state.child_states.empty()) {
		ColumnScanState child_state(state.parent);
		child_state.scan_options = state.scan_options;
		state.child_states.push_back(std::move(child_state));
	}
	auto scan_count = ColumnData::Fetch(state, row_id, result);
	validity->Fetch(state.child_states[0], row_id, result);
	return scan_count;
}

void StandardColumnData::Update(TransactionData transaction, DuckTableEntry &table_entry, idx_t column_index,
                                Vector &update_vector, row_t *row_ids, idx_t update_count, idx_t row_group_start) {
	ColumnScanState standard_state(nullptr);
	ColumnScanState validity_state(nullptr);
	Vector base_vector(type);

	FetchUpdateData(standard_state, row_ids, base_vector, row_group_start);
	validity->FetchUpdateData(validity_state, row_ids, base_vector, row_group_start);

	UpdateInternal(transaction, table_entry, column_index, update_vector, row_ids, update_count, base_vector,
	               row_group_start);
	validity->UpdateInternal(transaction, table_entry, column_index, update_vector, row_ids, update_count, base_vector,
	                         row_group_start);
}

void StandardColumnData::UpdateColumn(TransactionData transaction, DuckTableEntry &table_entry,
                                      const vector<column_t> &column_path, Vector &update_vector, row_t *row_ids,
                                      idx_t update_count, idx_t depth, idx_t row_group_start) {
	if (depth >= column_path.size()) {
		// update this column
		ColumnData::Update(transaction, table_entry, column_path[0], update_vector, row_ids, update_count,
		                   row_group_start);
	} else {
		// update the child column (i.e. the validity column)
		validity->UpdateColumn(transaction, table_entry, column_path, update_vector, row_ids, update_count, depth + 1,
		                       row_group_start);
		validity->UpdateWithBase(transaction, table_entry, column_path[0], update_vector, row_ids, update_count, *this,
		                         row_group_start);
	}
}

unique_ptr<BaseStatistics> StandardColumnData::GetUpdateStatistics() {
	unique_ptr<BaseStatistics> stats;
	{
		lock_guard<mutex> update_guard(update_lock);
		stats = updates ? updates->GetStatistics() : nullptr;
	}
	auto validity_stats = validity->GetUpdateStatistics();
	if (!stats && !validity_stats) {
		return nullptr;
	}
	if (!stats) {
		stats = BaseStatistics::CreateEmpty(type).ToUnique();
	}
	if (validity_stats) {
		stats->Merge(*validity_stats);
	}
	return stats;
}

void StandardColumnData::FetchRows(TransactionData transaction, ColumnFetchState &state,
                                   const StorageIndex &storage_index, const idx_t *offsets, const SelectionVector &sel,
                                   idx_t fetch_count, Vector &result, idx_t result_offset) {
	if (state.child_states.empty()) {
		state.child_states.emplace_back(make_uniq<ColumnFetchState>());
	}
	// Bulk fetch the data and the validity in two passes.
	FetchRowsAtSegmentLevel(transaction, state, offsets, sel, fetch_count, result, result_offset);
	validity->FetchRowsAtSegmentLevel(transaction, *state.child_states[0], offsets, sel, fetch_count, result,
	                                  result_offset);
}

void StandardColumnData::VisitBlockIds(BlockIdVisitor &visitor) const {
	ColumnData::VisitBlockIds(visitor);
	validity->VisitBlockIds(visitor);
}

void StandardColumnData::SetValidityData(shared_ptr<ValidityColumnData> validity_p) {
	if (validity) {
		throw InternalException("StandardColumnData::SetValidityData cannot be used to overwrite existing validity");
	}
	validity_p->SetParent(this);
	this->validity = std::move(validity_p);
}

ValidityColumnData &StandardColumnData::GetValidityData() {
	D_ASSERT(validity);
	return *validity;
}

struct StandardColumnCheckpointState : public ColumnCheckpointState {
	StandardColumnCheckpointState(const RowGroup &row_group, ColumnData &column_data,
	                              PartialBlockManager &partial_block_manager)
	    : ColumnCheckpointState(row_group, column_data, partial_block_manager) {
	}

	unique_ptr<ColumnCheckpointState> validity_state;

public:
	shared_ptr<ColumnData> CreateEmptyColumnData() override {
		return make_shared_ptr<StandardColumnData>(original_column.GetBlockManager(), original_column.GetTableInfo(),
		                                           original_column.column_index, original_column.type,
		                                           ColumnDataType::CHECKPOINT_TARGET, nullptr);
	}

	shared_ptr<ColumnData> GetFinalResult() override {
		if (result_column) {
			auto &column_data = result_column->Cast<StandardColumnData>();
			auto validity_child = validity_state->GetFinalResult();
			column_data.SetValidityData(shared_ptr_cast<ColumnData, ValidityColumnData>(std::move(validity_child)));
		}
		return ColumnCheckpointState::GetFinalResult();
	}

	unique_ptr<BaseStatistics> GetStatistics() override {
		D_ASSERT(global_stats);
		global_stats->Merge(*validity_state->GetStatistics());
		return std::move(global_stats);
	}

	PersistentColumnData ToPersistentData() override {
		auto data = ColumnCheckpointState::ToPersistentData();
		data.child_columns.push_back(validity_state->ToPersistentData());
		return data;
	}
};

unique_ptr<ColumnCheckpointState>
StandardColumnData::CreateCheckpointState(const RowGroup &row_group, PartialBlockManager &partial_block_manager) {
	return make_uniq<StandardColumnCheckpointState>(row_group, *this, partial_block_manager);
}

unique_ptr<ColumnCheckpointState> StandardColumnData::Checkpoint(const RowGroup &row_group,
                                                                 ColumnCheckpointInfo &checkpoint_info,
                                                                 const BaseStatistics &stats) {
	// we need to checkpoint the main column data first
	// that is because the checkpointing of the main column data ALSO scans the validity data
	// to prevent reading the validity data immediately after it is checkpointed we first checkpoint the main column
	// this is necessary for concurrent checkpointing as due to the partial block manager checkpointed data might be
	// flushed to disk by a different thread than the one that wrote it, causing a data race
	auto &partial_block_manager = checkpoint_info.GetPartialBlockManager();
	auto base_state = CreateCheckpointState(row_group, partial_block_manager);
	base_state->global_stats = BaseStatistics::CreateEmpty(type).ToUnique();
	auto validity_state_p = validity->CreateCheckpointState(row_group, partial_block_manager);
	validity_state_p->global_stats = BaseStatistics::CreateEmpty(validity->type).ToUnique();

	auto &validity_state = *validity_state_p;
	auto &checkpoint_state = base_state->Cast<StandardColumnCheckpointState>();
	checkpoint_state.validity_state = std::move(validity_state_p);

	if (!data.GetRootSegment()) {
		// empty table: flush the empty list
		return base_state;
	}

	vector<reference<ColumnCheckpointState>> checkpoint_states;
	checkpoint_states.emplace_back(checkpoint_state);
	checkpoint_states.emplace_back(validity_state);

	ColumnDataCheckpointer checkpointer(checkpoint_states, GetStorageManager(), row_group, checkpoint_info);
	checkpointer.Checkpoint();
	checkpointer.FinalizeCheckpoint();

	// merge validity stats into base stats
	base_state->global_stats->Merge(*validity_state.global_stats);

	return base_state;
}

void StandardColumnData::CheckpointScan(ColumnSegment &segment, ColumnScanState &state, idx_t count,
                                        Vector &scan_vector) const {
	ColumnData::CheckpointScan(segment, state, count, scan_vector);

	idx_t offset_in_row_group = state.offset_in_column;
	validity->ScanCommittedRange(0, offset_in_row_group, count, scan_vector);
}

bool StandardColumnData::IsPersistent() {
	return ColumnData::IsPersistent() && validity->IsPersistent();
}

bool StandardColumnData::HasAnyChanges() const {
	return ColumnData::HasAnyChanges() || validity->HasAnyChanges();
}

PersistentColumnData StandardColumnData::Serialize() {
	auto persistent_data = ColumnData::Serialize();
	persistent_data.child_columns.push_back(validity->Serialize());
	return persistent_data;
}

void StandardColumnData::InitializeColumn(PersistentColumnData &column_data, BaseStatistics &target_stats) {
	ColumnData::InitializeColumn(column_data, target_stats);
	validity->InitializeColumn(column_data.child_columns[0], target_stats);
	// segments without a byte size come from older storage, which may record NULLs only in the validity statistics
	bool legacy_segments = false;
	for (auto &pointer : column_data.pointers) {
		legacy_segments = legacy_segments || !pointer.byte_size;
	}
	bool validity_has_null = false;
	for (auto &pointer : column_data.child_columns[0].pointers) {
		validity_has_null = validity_has_null || pointer.statistics.CanHaveNull();
	}
	if (!legacy_segments || !validity_has_null) {
		return;
	}
	for (auto &node : data.SegmentNodes()) {
		node.GetNode().GetStatsMutable().SetHasNull();
	}
}

void StandardColumnData::GetColumnSegmentInfo(const QueryContext &context, duckdb::idx_t row_group_index,
                                              vector<duckdb::idx_t> col_path, vector<duckdb::ColumnSegmentInfo> &result,
                                              const ColumnSegmentInfoScanOptions &options) {
	ColumnData::GetColumnSegmentInfo(context, row_group_index, col_path, result, options);
	col_path.push_back(0);
	validity->GetColumnSegmentInfo(context, row_group_index, std::move(col_path), result, options);
}

void StandardColumnData::Verify(RowGroup &parent) {
#ifdef DEBUG
	ColumnData::Verify(parent);
	validity->Verify(parent);
#endif
}

} // namespace duckdb
