#include "duckdb/common/vector/array_vector.hpp"
#include "duckdb/storage/table/array_column_data.hpp"
#include "duckdb/storage/statistics/array_stats.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/storage/table/column_checkpoint_state.hpp"
#include "duckdb/storage/table/append_state.hpp"
#include "duckdb/storage/table/scan_state.hpp"

namespace duckdb {

ArrayColumnData::ArrayColumnData(BlockManager &block_manager, DataTableInfo &info, idx_t column_index,
                                 LogicalType type_p, ColumnDataType data_type, optional_ptr<ColumnData> parent)
    : ColumnData(block_manager, info, column_index, std::move(type_p), data_type, parent) {
	D_ASSERT(type.InternalType() == PhysicalType::ARRAY);
	if (data_type != ColumnDataType::CHECKPOINT_TARGET) {
		auto &child_type = ArrayType::GetChildType(type);
		validity = make_shared_ptr<ValidityColumnData>(block_manager, info, 0, *this);
		// the child column, with column index 1 (0 is the validity mask)
		child_column = CreateColumn(block_manager, info, 1, child_type, data_type, this);
	}
}

void ArrayColumnData::SetDataType(ColumnDataType data_type) {
	ColumnData::SetDataType(data_type);
	child_column->SetDataType(data_type);
	validity->SetDataType(data_type);
}

FilterPropagateResult ArrayColumnData::CheckZonemap(ColumnScanState &state, TableFilter &filter,
                                                    optional_ptr<SegmentNode<ColumnSegment>> &checked_segment) {
	return CheckValidityZonemap(state, filter, checked_segment, *validity);
}

void ArrayColumnData::InitializePrefetch(PrefetchState &prefetch_state, ColumnScanState &scan_state, idx_t rows) {
	ColumnData::InitializePrefetch(prefetch_state, scan_state, rows);
	validity->InitializePrefetch(prefetch_state, scan_state.child_states[0], rows);
	auto array_size = ArrayType::GetSize(type);
	child_column->InitializePrefetch(prefetch_state, scan_state.child_states[1], rows * array_size);
}

void ArrayColumnData::InitializeScan(ColumnScanState &state) {
	// initialize the validity segment
	D_ASSERT(state.child_states.size() == 2);

	state.offset_in_column = 0;
	state.current = nullptr;

	validity->InitializeScan(state.child_states[0]);

	// initialize the child scan
	child_column->InitializeScan(state.child_states[1]);
}

void ArrayColumnData::InitializeScanWithOffset(ColumnScanState &state, idx_t row_idx) {
	D_ASSERT(state.child_states.size() == 2);

	if (row_idx == 0) {
		// Trivial case, no offset
		InitializeScan(state);
		return;
	}

	state.offset_in_column = row_idx;
	state.current = nullptr;

	// initialize the validity segment
	validity->InitializeScanWithOffset(state.child_states[0], row_idx);

	auto array_size = ArrayType::GetSize(type);
	auto child_count = row_idx * array_size;

	D_ASSERT(child_count <= child_column->GetMaxEntry());
	if (child_count < child_column->GetMaxEntry()) {
		const auto child_offset = child_count;
		child_column->InitializeScanWithOffset(state.child_states[1], child_offset);
	}
}

idx_t ArrayColumnData::Scan(TransactionData transaction, idx_t vector_index, ColumnScanState &state, Vector &result,
                            idx_t scan_count) {
	return ScanCount(state, result, scan_count);
}

idx_t ArrayColumnData::ScanCount(ColumnScanState &state, Vector &result, idx_t count, idx_t result_offset) {
	// Scan validity
	auto scan_count = validity->ScanCount(state.child_states[0], result, count, result_offset);
	auto array_size = ArrayType::GetSize(type);
	// Scan child column
	auto &child_vec = ArrayVector::GetChildMutable(result);
	child_column->ScanCount(state.child_states[1], child_vec, count * array_size, result_offset * array_size);
	return scan_count;
}

void ArrayColumnData::Select(TransactionData transaction, idx_t vector_index, ColumnScanState &state, Vector &result,
                             SelectionVector &sel, idx_t sel_count) {
	bool is_supported = !child_column->type.IsNested() && child_column->type.InternalType() != PhysicalType::VARCHAR;
	if (!is_supported) {
		ColumnData::Select(transaction, vector_index, state, result, sel, sel_count);
		return;
	}
	// the below specialized Select implementation selects only the required arrays, and skips over non-required data
	// note that this implementation is not necessarily faster than the naive implementation of scanning + slicing
	// this optimization is better:
	// (1) the fewer consecutive ranges we are scanning
	// (2) the larger the arrays are
	// below we try to select the optimal variant

	// first count the number of consecutive requests we must make
	idx_t consecutive_ranges = 0;
	for (idx_t i = 0; i < sel_count; i++) {
		idx_t start_idx = sel.get_index(i);
		idx_t end_idx = start_idx + 1;
		for (; i + 1 < sel_count; i++) {
			auto next_idx = sel.get_index(i + 1);
			if (next_idx > end_idx) {
				// not consecutive - break
				break;
			}
			end_idx = next_idx + 1;
		}
		consecutive_ranges++;
	}

	auto target_count = GetVectorCount(vector_index);

	// experimentally, we want to allow around one consecutive range every ~2 array size
	// for array size = 10, we allow 5 consecutive ranges
	// for array size = 100, we allow 50 consecutive ranges
	auto array_size = ArrayType::GetSize(type);
	auto allowed_ranges = array_size / 2;
	if (allowed_ranges < consecutive_ranges) {
		// fallback to select + filter
		ColumnData::Select(transaction, vector_index, state, result, sel, sel_count);
		return;
	}

	idx_t current_offset = 0;
	idx_t current_position = 0;
	auto &child_vec = ArrayVector::GetChildMutable(result);
	for (idx_t i = 0; i < sel_count; i++) {
		idx_t start_idx = sel.get_index(i);
		idx_t end_idx = start_idx + 1;
		for (; i + 1 < sel_count; i++) {
			auto next_idx = sel.get_index(i + 1);
			if (next_idx > end_idx) {
				// not consecutive - break
				break;
			}
			end_idx = next_idx + 1;
		}
		if (start_idx > current_position) {
			// skip forward
			idx_t skip_amount = start_idx - current_position;
			validity->Skip(state.child_states[0], skip_amount);
			child_column->Skip(state.child_states[1], skip_amount * array_size);
		}
		// scan into the result array
		idx_t scan_count = end_idx - start_idx;
		validity->ScanCount(state.child_states[0], result, scan_count, current_offset);
		child_column->ScanCount(state.child_states[1], child_vec, scan_count * array_size, current_offset * array_size);
		// move the current position forward
		current_offset += scan_count;
		current_position = end_idx;
	}
	// if there is any remaining at the end - skip any trailing rows
	if (current_position < target_count) {
		idx_t skip_amount = target_count - current_position;
		validity->Skip(state.child_states[0], skip_amount);
		child_column->Skip(state.child_states[1], skip_amount * array_size);
	}
}

void ArrayColumnData::Skip(ColumnScanState &state, idx_t count) {
	// Skip validity
	validity->Skip(state.child_states[0], count);
	// Skip child column
	auto array_size = ArrayType::GetSize(type);
	child_column->Skip(state.child_states[1], count * array_size);
}

void ArrayColumnData::InitializeAppend(ColumnAppendState &state) {
	ColumnAppendState validity_append;
	validity_append.transient = state.transient;
	validity->InitializeAppend(validity_append);
	state.child_appends.push_back(std::move(validity_append));

	ColumnAppendState child_append;
	child_append.transient = state.transient;
	child_column->InitializeAppend(child_append);
	state.child_appends.push_back(std::move(child_append));
}

void ArrayColumnData::Append(ColumnAppendState &state, const Vector &vector, idx_t count) {
	if (vector.GetVectorType() != VectorType::FLAT_VECTOR) {
		Vector append_vector(Vector::Ref(vector));
		append_vector.Flatten();
		Append(state, append_vector, count);
		return;
	}

	// Append validity
	validity->Append(state.child_appends[0], vector, count);
	// Append child column
	auto array_size = ArrayType::GetSize(type);
	const auto &child_vec = ArrayVector::GetChild(vector);
	child_column->Append(state.child_appends[1], child_vec, count * array_size);

	this->count += count;
}

void ArrayColumnData::FinalizeAppend(ColumnDataFinalizeAppendState &finalize_state, ColumnAppendState &state) {
	validity->FinalizeAppendLocked(finalize_state, state.child_appends[0]);

	ColumnDataFinalizeAppendState child_finalize_state(finalize_state, LogicalTypeId::ARRAY);
	child_column->FinalizeAppendLocked(child_finalize_state, state.child_appends[1]);
}

void ArrayColumnData::RevertAppend(row_t new_count) {
	// Revert validity
	validity->RevertAppend(new_count);
	// Revert child column
	auto array_size = ArrayType::GetSize(type);
	child_column->RevertAppend(new_count * UnsafeNumericCast<row_t>(array_size));

	this->count = UnsafeNumericCast<idx_t>(new_count);
}

idx_t ArrayColumnData::Fetch(ColumnScanState &state, row_t row_id, Vector &result) {
	throw NotImplementedException("Array Fetch");
}

void ArrayColumnData::Update(TransactionData transaction, DuckTableEntry &table_entry, idx_t column_index,
                             Vector &update_vector, row_t *row_ids, idx_t update_count, idx_t row_group_start) {
	throw NotImplementedException("Array Update is not supported.");
}

void ArrayColumnData::UpdateColumn(TransactionData transaction, DuckTableEntry &table_entry,
                                   const vector<column_t> &column_path, Vector &update_vector, row_t *row_ids,
                                   idx_t update_count, idx_t depth, idx_t row_group_start) {
	throw NotImplementedException("Array Update Column is not supported");
}

unique_ptr<BaseStatistics> ArrayColumnData::GetUpdateStatistics() {
	return nullptr;
}

void ArrayColumnData::FetchRows(TransactionData transaction, ColumnFetchState &state, const StorageIndex &storage_index,
                                const idx_t *offsets, const SelectionVector &sel, idx_t fetch_count, Vector &result,
                                idx_t result_offset) {
	// Create state for validity & child column
	if (state.child_states.empty()) {
		state.child_states.push_back(make_uniq<ColumnFetchState>());
	}

	// Fetch validity
	validity->FetchRowsAtSegmentLevel(transaction, *state.child_states[0], offsets, sel, fetch_count, result,
	                                  result_offset);

	// Fetch child column
	auto &child_vec = ArrayVector::GetChildMutable(result);
	auto &child_type = ArrayType::GetChildType(type);
	auto array_size = ArrayType::GetSize(type);

	for (idx_t idx = 0; idx < fetch_count; idx++) {
		// We need to fetch between [row_id * array_size, (row_id + 1) * array_size)
		ColumnScanState child_state(nullptr);
		child_state.Initialize(state.context, child_type, nullptr);

		const auto row_id = offsets[sel.get_index(idx)];
		const auto child_offset = row_id * array_size;

		child_column->InitializeScanWithOffset(child_state, child_offset);
		Vector child_scan(child_type, array_size);
		child_column->ScanCount(child_state, child_scan, array_size);
		VectorOperations::Copy(child_scan, child_vec, array_size, 0, (result_offset + idx) * array_size);
	}
}

void ArrayColumnData::VisitBlockIds(BlockIdVisitor &visitor) const {
	validity->VisitBlockIds(visitor);
	child_column->VisitBlockIds(visitor);
}

const BaseStatistics &ArrayColumnData::GetChildStats(const ColumnData &child) const {
	if (!RefersToSameObject(child, *child_column)) {
		throw InternalException("ArrayColumnData::GetChildStats provided column data is not a child of this array");
	}
	auto &stats = GetStatisticsRef();
	return ArrayStats::GetChildStats(stats);
}

void ArrayColumnData::SetValidityData(shared_ptr<ValidityColumnData> validity_p) {
	if (validity) {
		throw InternalException("ArrayColumnData::SetValidityData cannot be used to overwrite existing validity");
	}
	validity_p->SetParent(this);
	this->validity = std::move(validity_p);
}

void ArrayColumnData::SetChildData(shared_ptr<ColumnData> child_column_p) {
	if (child_column) {
		throw InternalException("ArrayColumnData::SetChildData cannot be used to overwrite existing data");
	}
	child_column_p->SetParent(this);
	this->child_column = std::move(child_column_p);
}

struct ArrayColumnCheckpointState : public ColumnCheckpointState {
	ArrayColumnCheckpointState(const RowGroup &row_group, ColumnData &column_data,
	                           PartialBlockManager &partial_block_manager)
	    : ColumnCheckpointState(row_group, column_data, partial_block_manager) {
		global_stats = ArrayStats::CreateEmpty(column_data.type).ToUnique();
	}

	unique_ptr<ColumnCheckpointState> validity_state;
	unique_ptr<ColumnCheckpointState> child_state;

public:
	shared_ptr<ColumnData> CreateEmptyColumnData() override {
		return make_shared_ptr<ArrayColumnData>(original_column.GetBlockManager(), original_column.GetTableInfo(),
		                                        original_column.column_index, original_column.type,
		                                        ColumnDataType::CHECKPOINT_TARGET, nullptr);
	}

	shared_ptr<ColumnData> GetFinalResult() override {
		if (!result_column) {
			result_column = CreateEmptyColumnData();
		}
		auto &column_data = result_column->Cast<ArrayColumnData>();
		auto validity_child = validity_state->GetFinalResult();
		column_data.SetValidityData(shared_ptr_cast<ColumnData, ValidityColumnData>(std::move(validity_child)));
		column_data.SetChildData(child_state->GetFinalResult());
		return ColumnCheckpointState::GetFinalResult();
	}

	unique_ptr<BaseStatistics> GetStatistics() override {
		auto stats = global_stats->Copy();
		stats.Merge(*validity_state->GetStatistics());
		ArrayStats::SetChildStats(stats, child_state->GetStatistics());
		return stats.ToUnique();
	}

	PersistentColumnData ToPersistentData() override {
		PersistentColumnData data(original_column.type);
		data.child_columns.push_back(validity_state->ToPersistentData());
		data.child_columns.push_back(child_state->ToPersistentData());
		return data;
	}
};

unique_ptr<ColumnCheckpointState> ArrayColumnData::CreateCheckpointState(const RowGroup &row_group,
                                                                         PartialBlockManager &partial_block_manager) {
	return make_uniq<ArrayColumnCheckpointState>(row_group, *this, partial_block_manager);
}

unique_ptr<ColumnCheckpointState> ArrayColumnData::Checkpoint(const RowGroup &row_group,
                                                              ColumnCheckpointInfo &checkpoint_info,
                                                              const BaseStatistics &old_stats) {
	auto &partial_block_manager = checkpoint_info.GetPartialBlockManager();
	auto checkpoint_state = make_uniq<ArrayColumnCheckpointState>(row_group, *this, partial_block_manager);
	checkpoint_state->validity_state = validity->Checkpoint(row_group, checkpoint_info, old_stats);
	checkpoint_state->child_state =
	    child_column->Checkpoint(row_group, checkpoint_info, ArrayStats::GetChildStats(old_stats));
	return std::move(checkpoint_state);
}

bool ArrayColumnData::IsPersistent() {
	return validity->IsPersistent() && child_column->IsPersistent();
}

bool ArrayColumnData::HasAnyChanges() const {
	return child_column->HasAnyChanges() || validity->HasAnyChanges();
}

PersistentColumnData ArrayColumnData::Serialize() {
	PersistentColumnData persistent_data(type);
	persistent_data.child_columns.push_back(validity->Serialize());
	persistent_data.child_columns.push_back(child_column->Serialize());
	return persistent_data;
}

void ArrayColumnData::InitializeColumn(PersistentColumnData &column_data, BaseStatistics &target_stats) {
	D_ASSERT(column_data.pointers.empty());
	validity->InitializeColumn(column_data.child_columns[0], target_stats);
	auto &child_stats = ArrayStats::GetChildStats(target_stats);
	child_column->InitializeColumn(column_data.child_columns[1], child_stats);
	this->count = validity->count.load();
}

void ArrayColumnData::GetColumnSegmentInfo(const QueryContext &context, idx_t row_group_index, vector<idx_t> col_path,
                                           vector<ColumnSegmentInfo> &result,
                                           const ColumnSegmentInfoScanOptions &options) {
	col_path.push_back(0);
	validity->GetColumnSegmentInfo(context, row_group_index, col_path, result, options);
	col_path.back() = 1;
	child_column->GetColumnSegmentInfo(context, row_group_index, col_path, result, options);
}

void ArrayColumnData::Verify(RowGroup &parent) {
#ifdef DEBUG
	ColumnData::Verify(parent);
	validity->Verify(parent);
	child_column->Verify(parent);
#endif
}

} // namespace duckdb
