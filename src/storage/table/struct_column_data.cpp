#include "duckdb/storage/table/struct_column_data.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/storage/statistics/struct_stats.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/storage/table/column_checkpoint_state.hpp"
#include "duckdb/storage/table/append_state.hpp"
#include "duckdb/storage/table/scan_state.hpp"

#include "duckdb/transaction/duck_transaction.hpp"

namespace duckdb {

StructColumnData::StructColumnData(BlockManager &block_manager, DataTableInfo &info, idx_t column_index,
                                   LogicalType type_p, ColumnDataType data_type, optional_ptr<ColumnData> parent)
    : ColumnData(block_manager, info, column_index, std::move(type_p), data_type, parent) {
	D_ASSERT(type.InternalType() == PhysicalType::STRUCT);
	auto &child_types = StructType::GetChildTypes(type);
	if (type.id() == LogicalTypeId::VARIANT) {
		throw NotImplementedException("A table cannot be created from a VARIANT column yet");
	}
	if (data_type != ColumnDataType::CHECKPOINT_TARGET) {
		validity = make_shared_ptr<ValidityColumnData>(block_manager, info, 0, *this);
		// the sub column index, starting at 1 (0 is the validity mask)
		idx_t sub_column_index = 1;
		for (auto &child_type : child_types) {
			sub_columns.push_back(
			    ColumnData::CreateColumn(block_manager, info, sub_column_index, child_type.second, data_type, this));
			sub_column_index++;
		}
	} else {
		// initialize to empty
		sub_columns.resize(child_types.size());
	}
}

void StructColumnData::SetDataType(ColumnDataType data_type) {
	ColumnData::SetDataType(data_type);
	for (auto &sub_column : sub_columns) {
		sub_column->SetDataType(data_type);
	}
	validity->SetDataType(data_type);
}

idx_t StructColumnData::GetMaxEntry() {
	// every struct has a validity column, while an empty TUPLE has no child columns
	return validity->GetMaxEntry();
}

FilterPropagateResult StructColumnData::CheckZonemap(ColumnScanState &state, TableFilter &filter,
                                                     optional_ptr<SegmentNode<ColumnSegment>> &checked_segment) {
	if (state.expression_state) {
		checked_segment = nullptr;
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	if (state.storage_index.IsPushdownExtract()) {
		auto children = GetStructChildren(state);
		D_ASSERT(children.size() == 1);
		return children[0].col.CheckZonemap(children[0].state, filter, checked_segment);
	}
	return CheckValidityZonemap(state, filter, checked_segment, *validity);
}

vector<StructColumnData::StructColumnDataChild> StructColumnData::GetStructChildren(ColumnScanState &state) const {
	vector<StructColumnData::StructColumnDataChild> res;
	if (state.storage_index.IsPushdownExtract()) {
		auto &index_children = state.storage_index.GetChildIndexes();
		D_ASSERT(index_children.size() == 1);
		auto &child_storage_index = index_children[0];
		auto child_index = child_storage_index.GetPrimaryIndex();
		auto &field_state = state.child_states[1];
		D_ASSERT(state.scan_child_column[0]);
		res.emplace_back(*sub_columns[child_index], optional_idx(), field_state, true);
	} else {
		for (idx_t i = 0; i < sub_columns.size(); i++) {
			auto &field_state = state.child_states[1 + i];
			res.emplace_back(*sub_columns[i], i, field_state, state.scan_child_column[i]);
		}
	}
	return res;
}

void StructColumnData::InitializePrefetch(PrefetchState &prefetch_state, ColumnScanState &scan_state, idx_t rows) {
	validity->InitializePrefetch(prefetch_state, scan_state.child_states[0], rows);
	auto struct_children = GetStructChildren(scan_state);
	for (auto &child : struct_children) {
		if (!child.should_scan) {
			continue;
		}
		child.col.InitializePrefetch(prefetch_state, child.state, rows);
	}
}

void StructColumnData::InitializeScan(ColumnScanState &state) {
	state.offset_in_column = 0;
	state.current = nullptr;

	// initialize the validity segment
	validity->InitializeScan(state.child_states[0]);

	// initialize the sub-columns
	auto struct_children = GetStructChildren(state);
	for (auto &child : struct_children) {
		if (!child.should_scan) {
			continue;
		}
		child.col.InitializeScan(child.state);
	}
}

void StructColumnData::InitializeScanWithOffset(ColumnScanState &state, idx_t row_idx) {
	D_ASSERT(row_idx < count);
	state.offset_in_column = row_idx;
	state.current = nullptr;

	// initialize the validity segment
	validity->InitializeScanWithOffset(state.child_states[0], row_idx);

	// initialize the sub-columns
	auto struct_children = GetStructChildren(state);
	for (auto &child : struct_children) {
		if (!child.should_scan) {
			continue;
		}
		child.col.InitializeScanWithOffset(child.state, row_idx);
	}
}

static Vector &GetFieldVectorForScan(Vector &result, optional_idx field_index) {
	if (!field_index.IsValid()) {
		//! Scan is of type PUSHDOWN_EXTRACT, target_vector for the scan is directly to the result
		return result;
	}
	auto index = field_index.GetIndex();
	auto &children = StructVector::GetEntries(result);
	return children[index];
}

static void ScanChild(ColumnScanState &state, Vector &result, const std::function<idx_t(Vector &target)> &callback) {
	if (state.expression_state) {
		auto &expression_state = *state.expression_state;
		D_ASSERT(state.context.Valid());
		auto &executor = expression_state.executor;
		auto &target = expression_state.target;
		auto &input = expression_state.input;

		target.Reset();
		input.Reset();
		auto scan_count = callback(input.data[0]);
		FlatVector::SetSize(input.data[0], scan_count);
		executor.Execute(input, target);
		result.Reference(target.data[0]);
	} else {
		callback(result);
	}
}

idx_t StructColumnData::Scan(TransactionData transaction, idx_t vector_index, ColumnScanState &state, Vector &result,
                             idx_t target_count) {
	idx_t scan_count;
	if (!state.storage_index.IsPushdownExtract()) {
		// if we are scanning the entire struct we need to scan the validity
		scan_count = validity->Scan(transaction, vector_index, state.child_states[0], result, target_count);
		if (result.GetVectorType() == VectorType::CONSTANT_VECTOR) {
			if (!ConstantVector::IsNull(result)) {
				throw InternalException("StructColumnData::Scan returned a constant but not NULL vector ");
			}
			// constant NULL struct - we don't need to scan any children, everything is already NULL
			return scan_count;
		}
	}
	auto struct_children = GetStructChildren(state);
	for (auto &child : struct_children) {
		auto &target_vector = GetFieldVectorForScan(result, child.vector_index);
		if (!child.should_scan) {
			// if we are not scanning this vector - set it to NULL
			ConstantVector::SetNull(target_vector, count_t(target_count));
			continue;
		}
		ScanChild(state, target_vector, [&](Vector &child_result) {
			scan_count = child.col.Scan(transaction, vector_index, child.state, child_result, target_count);
			return scan_count;
		});
	}
	FlatVector::SetSize(result, scan_count);
	return scan_count;
}

idx_t StructColumnData::ScanCount(ColumnScanState &state, Vector &result, idx_t count, idx_t result_offset) {
	idx_t scan_count;
	if (!state.storage_index.IsPushdownExtract()) {
		// if we are scanning the entire struct we need to scan the validity
		scan_count = validity->ScanCount(state.child_states[0], result, count);
	}
	auto struct_children = GetStructChildren(state);
	for (auto &child : struct_children) {
		auto &target_vector = GetFieldVectorForScan(result, child.vector_index);
		if (!child.should_scan) {
			// if we are not scanning this vector - set it to NULL
			target_vector.SetVectorType(VectorType::CONSTANT_VECTOR);
			FlatVector::SetSize(target_vector, count_t(count));
			ConstantVector::SetNull(target_vector, true);
			continue;
		}
		ScanChild(state, target_vector, [&](Vector &child_result) {
			scan_count = child.col.ScanCount(child.state, child_result, count, result_offset);
			return scan_count;
		});
	}
	return scan_count;
}

void StructColumnData::Skip(ColumnScanState &state, idx_t count) {
	validity->Skip(state.child_states[0], count);

	// skip inside the sub-columns
	auto struct_children = GetStructChildren(state);
	for (auto &child : struct_children) {
		if (!child.should_scan) {
			continue;
		}
		child.col.Skip(child.state, count);
	}
}

void StructColumnData::InitializeAppend(ColumnAppendState &state) {
	ColumnAppendState validity_append;
	validity->InitializeAppend(validity_append);
	state.child_appends.push_back(std::move(validity_append));

	for (auto &sub_column : sub_columns) {
		ColumnAppendState child_append;
		sub_column->InitializeAppend(child_append);
		state.child_appends.push_back(std::move(child_append));
	}
}

void StructColumnData::Append(ColumnAppendState &state, const Vector &vector, idx_t count) {
	if (vector.GetVectorType() != VectorType::FLAT_VECTOR) {
		Vector append_vector(Vector::Ref(vector));
		append_vector.Flatten();
		Append(state, append_vector, count);
		return;
	}

	// append the null values
	validity->Append(state.child_appends[0], vector, count);

	auto &child_entries = StructVector::GetEntries(vector);
	for (idx_t i = 0; i < child_entries.size(); i++) {
		sub_columns[i]->Append(state.child_appends[i + 1], child_entries[i], count);
	}
	this->count += count;
}

void StructColumnData::FinalizeAppend(ColumnDataFinalizeAppendState &finalize_state, ColumnAppendState &state) {
	validity->FinalizeAppendLocked(finalize_state, state.child_appends[0]);

	for (idx_t i = 0; i < sub_columns.size(); i++) {
		ColumnDataFinalizeAppendState child_finalize_state(finalize_state, LogicalTypeId::STRUCT, i);
		sub_columns[i]->FinalizeAppendLocked(child_finalize_state, state.child_appends[i + 1]);
	}
}

void StructColumnData::RevertAppend(row_t new_count) {
	validity->RevertAppend(new_count);
	for (auto &sub_column : sub_columns) {
		sub_column->RevertAppend(new_count);
	}
	this->count = UnsafeNumericCast<idx_t>(new_count);
}

idx_t StructColumnData::Fetch(ColumnScanState &state, row_t row_id, Vector &result) {
	// fetch validity mask
	auto &child_entries = StructVector::GetEntries(result);
	// insert any child states that are required
	for (idx_t i = state.child_states.size(); i < child_entries.size() + 1; i++) {
		ColumnScanState child_state(state.parent);
		child_state.scan_options = state.scan_options;
		state.child_states.push_back(std::move(child_state));
	}
	// fetch the validity state
	idx_t scan_count = validity->Fetch(state.child_states[0], row_id, result);
	// fetch the sub-column states
	for (idx_t i = 0; i < child_entries.size(); i++) {
		sub_columns[i]->Fetch(state.child_states[i + 1], row_id, child_entries[i]);
	}
	return scan_count;
}

void StructColumnData::Update(TransactionData transaction, DuckTableEntry &table_entry, idx_t column_index,
                              Vector &update_vector, row_t *row_ids, idx_t update_count, idx_t row_group_start) {
	validity->Update(transaction, table_entry, column_index, update_vector, row_ids, update_count, row_group_start);
	auto &child_entries = StructVector::GetEntries(update_vector);
	for (idx_t i = 0; i < child_entries.size(); i++) {
		sub_columns[i]->Update(transaction, table_entry, column_index, child_entries[i], row_ids, update_count,
		                       row_group_start);
	}
}

void StructColumnData::UpdateColumn(TransactionData transaction, DuckTableEntry &table_entry,
                                    const vector<column_t> &column_path, Vector &update_vector, row_t *row_ids,
                                    idx_t update_count, idx_t depth, idx_t row_group_start) {
	// we can never DIRECTLY update a struct column
	if (depth >= column_path.size()) {
		throw InternalException("Attempting to directly update a struct column - this should not be possible");
	}
	auto update_column = column_path[depth];
	if (update_column == 0) {
		// update the validity column
		validity->UpdateColumn(transaction, table_entry, column_path, update_vector, row_ids, update_count, depth + 1,
		                       row_group_start);
	} else {
		if (update_column > sub_columns.size()) {
			throw InternalException("Update column_path out of range");
		}
		sub_columns[update_column - 1]->UpdateColumn(transaction, table_entry, column_path, update_vector, row_ids,
		                                             update_count, depth + 1, row_group_start);
	}
}

unique_ptr<BaseStatistics> StructColumnData::GetUpdateStatistics() {
	// check if any child column has updates
	auto stats = BaseStatistics::CreateEmpty(type);
	auto validity_stats = validity->GetUpdateStatistics();
	if (validity_stats) {
		stats.Merge(*validity_stats);
	}
	for (idx_t i = 0; i < sub_columns.size(); i++) {
		auto child_stats = sub_columns[i]->GetUpdateStatistics();
		if (child_stats) {
			StructStats::SetChildStats(stats, i, std::move(child_stats));
		}
	}
	return stats.ToUnique();
}

void StructColumnData::FetchRows(TransactionData transaction, ColumnFetchState &state,
                                 const StorageIndex &storage_index, const idx_t *offsets, const SelectionVector &sel,
                                 idx_t fetch_count, Vector &result, idx_t result_offset) {
	// fetch the validity state
	validity->FetchRowsAtSegmentLevel(transaction, state, offsets, sel, fetch_count, result, result_offset);
	if (storage_index.IsPushdownExtract()) {
		auto &index_children = storage_index.GetChildIndexes();
		D_ASSERT(index_children.size() == 1);
		auto &child_storage_index = index_children[0];
		auto child_index = child_storage_index.GetPrimaryIndex();
		auto &sub_column = *sub_columns[child_index];
		auto &child_type = StructType::GetChildTypes(type)[child_index].second;
		if (!child_storage_index.HasChildren() && child_storage_index.HasType() &&
		    child_storage_index.GetType() != child_type) {
			auto context = transaction.transaction->context.lock();
			for (idx_t idx = 0; idx < fetch_count; idx++) {
				const idx_t offset = offsets[sel.get_index(idx)];
				Vector intermediate(child_type, 1);
				sub_column.FetchRows(transaction, state, child_storage_index, &offset,
				                     *FlatVector::IncrementalSelectionVector(), /*count=*/1, intermediate, 0);
				auto fetched_row = intermediate.GetValue(0).CastAs(*context, result.GetType());
				result.SetValue(result_offset + idx, fetched_row);
			}
			return;
		} else {
			sub_column.FetchRows(transaction, state, child_storage_index, offsets, sel, fetch_count, result,
			                     result_offset);
			return;
		}
	}

	auto &child_entries = StructVector::GetEntries(result);
	// fetch the sub-column states
	for (idx_t i = 0; i < child_entries.size(); i++) {
		sub_columns[i]->FetchRows(transaction, state, storage_index, offsets, sel, fetch_count, child_entries[i],
		                          result_offset);
	}
}

void StructColumnData::VisitBlockIds(BlockIdVisitor &visitor) const {
	validity->VisitBlockIds(visitor);
	for (auto &sub_column : sub_columns) {
		sub_column->VisitBlockIds(visitor);
	}
}

void StructColumnData::SetValidityData(shared_ptr<ValidityColumnData> validity_p) {
	if (validity) {
		throw InternalException("StructColumnData::SetValidityData cannot be used to overwrite existing validity");
	}
	validity_p->SetParent(this);
	this->validity = std::move(validity_p);
}

void StructColumnData::SetChildData(idx_t i, shared_ptr<ColumnData> child_column_p) {
	if (sub_columns[i]) {
		throw InternalException("StructColumnData::SetChildData cannot be used to overwrite existing data");
	}
	child_column_p->SetParent(this);
	this->sub_columns[i] = std::move(child_column_p);
}

const ColumnData &StructColumnData::GetChildColumn(idx_t index) const {
	D_ASSERT(index < sub_columns.size());
	return *sub_columns[index];
}

const BaseStatistics &StructColumnData::GetChildStats(const ColumnData &child) const {
	optional_idx index;
	for (idx_t i = 0; i < sub_columns.size(); i++) {
		if (RefersToSameObject(child, *sub_columns[i])) {
			index = i;
			break;
		}
	}
	if (!index.IsValid()) {
		throw InternalException("StructColumnData::GetChildStats: Could not find a matching child index for the "
		                        "provided child (of type %s)",
		                        child.type.ToString());
	}
	auto idx = index.GetIndex();
	auto &stats = GetStatisticsRef();
	return StructStats::GetChildStats(stats, idx);
}

struct StructColumnCheckpointState : public ColumnCheckpointState {
	StructColumnCheckpointState(const RowGroup &row_group, ColumnData &column_data,
	                            PartialBlockManager &partial_block_manager)
	    : ColumnCheckpointState(row_group, column_data, partial_block_manager) {
		global_stats = StructStats::CreateEmpty(column_data.type).ToUnique();
	}

	unique_ptr<ColumnCheckpointState> validity_state;
	vector<unique_ptr<ColumnCheckpointState>> child_states;

public:
	shared_ptr<ColumnData> CreateEmptyColumnData() override {
		return make_shared_ptr<StructColumnData>(original_column.GetBlockManager(), original_column.GetTableInfo(),
		                                         original_column.column_index, original_column.type,
		                                         ColumnDataType::CHECKPOINT_TARGET, nullptr);
	}

	shared_ptr<ColumnData> GetFinalResult() override {
		if (!result_column) {
			result_column = CreateEmptyColumnData();
		}
		auto &column_data = result_column->Cast<StructColumnData>();
		auto validity_child = validity_state->GetFinalResult();
		column_data.SetValidityData(shared_ptr_cast<ColumnData, ValidityColumnData>(std::move(validity_child)));
		for (idx_t i = 0; i < child_states.size(); i++) {
			column_data.SetChildData(i, child_states[i]->GetFinalResult());
		}
		return ColumnCheckpointState::GetFinalResult();
	}
	unique_ptr<BaseStatistics> GetStatistics() override {
		D_ASSERT(global_stats);
		global_stats->Merge(*validity_state->GetStatistics());
		for (idx_t i = 0; i < child_states.size(); i++) {
			StructStats::SetChildStats(*global_stats, i, child_states[i]->GetStatistics());
		}
		return std::move(global_stats);
	}

	PersistentColumnData ToPersistentData() override {
		PersistentColumnData data(original_column.type);
		data.child_columns.push_back(validity_state->ToPersistentData());
		for (auto &child_state : child_states) {
			data.child_columns.push_back(child_state->ToPersistentData());
		}
		return data;
	}
};

unique_ptr<ColumnCheckpointState> StructColumnData::CreateCheckpointState(const RowGroup &row_group,
                                                                          PartialBlockManager &partial_block_manager) {
	return make_uniq<StructColumnCheckpointState>(row_group, *this, partial_block_manager);
}

unique_ptr<ColumnCheckpointState> StructColumnData::Checkpoint(const RowGroup &row_group,
                                                               ColumnCheckpointInfo &checkpoint_info,
                                                               const BaseStatistics &old_stats) {
	auto &partial_block_manager = checkpoint_info.GetPartialBlockManager();
	auto checkpoint_state = make_uniq<StructColumnCheckpointState>(row_group, *this, partial_block_manager);
	checkpoint_state->validity_state = validity->Checkpoint(row_group, checkpoint_info, old_stats);

	for (idx_t col_idx = 0; col_idx < sub_columns.size(); col_idx++) {
		const auto &sub_column = sub_columns[col_idx];
		const auto &old_child_stats = StructStats::GetChildStats(old_stats, col_idx);
		checkpoint_state->child_states.push_back(sub_column->Checkpoint(row_group, checkpoint_info, old_child_stats));
	}
	return std::move(checkpoint_state);
}

bool StructColumnData::IsPersistent() {
	if (!validity->IsPersistent()) {
		return false;
	}
	for (auto &child_col : sub_columns) {
		if (!child_col->IsPersistent()) {
			return false;
		}
	}
	return true;
}

bool StructColumnData::HasAnyChanges() const {
	if (validity->HasAnyChanges()) {
		return true;
	}
	for (auto &child_col : sub_columns) {
		if (child_col->HasAnyChanges()) {
			return true;
		}
	}
	return false;
}

PersistentColumnData StructColumnData::Serialize() {
	PersistentColumnData persistent_data(type);
	persistent_data.child_columns.push_back(validity->Serialize());
	for (auto &sub_column : sub_columns) {
		persistent_data.child_columns.push_back(sub_column->Serialize());
	}
	return persistent_data;
}

void StructColumnData::InitializeColumn(PersistentColumnData &column_data, BaseStatistics &target_stats) {
	validity->InitializeColumn(column_data.child_columns[0], target_stats);
	for (idx_t c_idx = 0; c_idx < sub_columns.size(); c_idx++) {
		auto &child_stats = StructStats::GetChildStats(target_stats, c_idx);
		sub_columns[c_idx]->InitializeColumn(column_data.child_columns[c_idx + 1], child_stats);
	}
	this->count = validity->count.load();
}

void StructColumnData::GetColumnSegmentInfo(const QueryContext &context, idx_t row_group_index, vector<idx_t> col_path,
                                            vector<ColumnSegmentInfo> &result,
                                            const ColumnSegmentInfoScanOptions &options) {
	col_path.push_back(0);
	validity->GetColumnSegmentInfo(context, row_group_index, col_path, result, options);
	for (idx_t i = 0; i < sub_columns.size(); i++) {
		col_path.back() = i + 1;
		sub_columns[i]->GetColumnSegmentInfo(context, row_group_index, col_path, result, options);
	}
}

void StructColumnData::Verify(RowGroup &parent) {
#ifdef DEBUG
	ColumnData::Verify(parent);
	validity->Verify(parent);
	for (auto &sub_column : sub_columns) {
		sub_column->Verify(parent);
	}
#endif
}

} // namespace duckdb
