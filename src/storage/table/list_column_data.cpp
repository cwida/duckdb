#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/storage/table/list_column_data.hpp"
#include "duckdb/storage/statistics/list_stats.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/storage/table/column_checkpoint_state.hpp"
#include "duckdb/storage/table/append_state.hpp"
#include "duckdb/storage/table/scan_state.hpp"

namespace duckdb {

[[noreturn]] static void ThrowListOffsetOutOfRange() {
	throw DataCorruptionException("Corrupted LIST column: offset exceeds the child column count");
}

[[noreturn]] static void ThrowListOffsetsOutOfOrder() {
	throw DataCorruptionException("Corrupted LIST column: later offset is smaller than the preceding offset");
}

static void ValidateListOffset(idx_t offset, idx_t child_count) {
	if (offset > child_count) {
		ThrowListOffsetOutOfRange();
	}
}

static idx_t GetListLength(idx_t start_offset, idx_t end_offset, idx_t child_count) {
	if (end_offset < start_offset) {
		ThrowListOffsetsOutOfOrder();
	}
	ValidateListOffset(end_offset, child_count);
	return end_offset - start_offset;
}

ListColumnData::ListColumnData(BlockManager &block_manager, DataTableInfo &info, idx_t column_index, LogicalType type_p,
                               ColumnDataType data_type, optional_ptr<ColumnData> parent)
    : ColumnData(block_manager, info, column_index, std::move(type_p), data_type, parent) {
	D_ASSERT(type.InternalType() == PhysicalType::LIST);
	if (data_type != ColumnDataType::CHECKPOINT_TARGET) {
		auto &child_type = ListType::GetChildType(type);
		validity = make_shared_ptr<ValidityColumnData>(block_manager, info, 0, *this);
		// the child column, with column index 1 (0 is the validity mask)
		child_column = CreateColumn(block_manager, info, 1, child_type, data_type, this);
	}
}

void ListColumnData::SetDataType(ColumnDataType data_type) {
	ColumnData::SetDataType(data_type);
	child_column->SetDataType(data_type);
	validity->SetDataType(data_type);
}

FilterPropagateResult ListColumnData::CheckZonemap(ColumnScanState &state, TableFilter &filter,
                                                   optional_ptr<SegmentNode<ColumnSegment>> &checked_segment) {
	return CheckValidityZonemap(state, filter, checked_segment, *validity);
}

void ListColumnData::InitializePrefetch(PrefetchState &prefetch_state, ColumnScanState &scan_state, idx_t rows) {
	ColumnData::InitializePrefetch(prefetch_state, scan_state, rows);
	validity->InitializePrefetch(prefetch_state, scan_state.child_states[0], rows);

	// we can't know how many rows we need to prefetch for the child of this list without looking at the actual data
	// we make an estimation by looking at how many rows the child column has versus this column
	// e.g if the child column has 10K rows, and we have 1K rows, we estimate that each list has 10 elements
	idx_t rows_per_list = 1;
	if (child_column->count > this->count && this->count > 0) {
		rows_per_list = child_column->count / this->count;
	}
	child_column->InitializePrefetch(prefetch_state, scan_state.child_states[1], rows * rows_per_list);
}

void ListColumnData::InitializeScan(ColumnScanState &state) {
	ColumnData::InitializeScan(state);

	// initialize the validity segment
	D_ASSERT(state.child_states.size() == 2);
	validity->InitializeScan(state.child_states[0]);

	// initialize the child scan
	child_column->InitializeScan(state.child_states[1]);
}

uint64_t ListColumnData::FetchListOffset(idx_t row_idx) {
	if (row_idx >= count) {
		throw DataCorruptionException("Corrupted database: list offset row ID is out of range");
	}
	ColumnScanState offset_state(nullptr);
	ColumnData::InitializeScanWithOffset(offset_state, row_idx);
	Vector result(LogicalType::UBIGINT, 1);
	ScanVector(offset_state, result, 1, ScanVectorType::SCAN_FLAT_VECTOR);
	auto offset = FlatVector::GetData<uint64_t>(result)[0];
	ValidateListOffset(offset, child_column->GetMaxEntry());

	// initialize the child scan with the required offset
	return offset;
}

void ListColumnData::InitializeScanWithOffset(ColumnScanState &state, idx_t row_idx) {
	if (row_idx == 0) {
		InitializeScan(state);
		return;
	}
	ColumnData::InitializeScanWithOffset(state, row_idx);

	// initialize the validity segment
	D_ASSERT(state.child_states.size() == 2);
	validity->InitializeScanWithOffset(state.child_states[0], row_idx);

	// we need to read the list at position row_idx to get the correct row offset of the child
	auto child_offset = FetchListOffset(row_idx - 1);
	if (child_offset < child_column->GetMaxEntry()) {
		child_column->InitializeScanWithOffset(state.child_states[1], child_offset);
	}
	state.last_offset = child_offset;
}

idx_t ListColumnData::Scan(TransactionData transaction, idx_t vector_index, ColumnScanState &state, Vector &result,
                           idx_t scan_count) {
	return ScanCount(state, result, scan_count);
}

idx_t ListColumnData::ScanCount(ColumnScanState &state, Vector &result, idx_t count, idx_t result_offset) {
	if (result_offset > 0) {
		throw InternalException("ListColumnData::ScanCount not supported with result_offset > 0");
	}
	if (count == 0) {
		return 0;
	}
	// updates not supported for lists
	D_ASSERT(!updates);

	Vector offset_vector(LogicalType::UBIGINT, count);
	idx_t scan_count = ScanVector(state, offset_vector, count, ScanVectorType::SCAN_FLAT_VECTOR);
	D_ASSERT(scan_count > 0);
	validity->ScanCount(state.child_states[0], result, count);

	auto data = offset_vector.Values<uint64_t>();
	// shift all offsets so they are 0 at the first entry
	auto result_data = FlatVector::Writer<list_entry_t>(result, scan_count);
	auto previous_offset = state.last_offset;
	idx_t current_offset = 0;
	auto child_count = child_column->GetMaxEntry();
	for (idx_t i = 0; i < scan_count; i++) {
		auto offset = data[i].GetValueUnsafe();
		auto length = GetListLength(previous_offset, offset, child_count);
		result_data.WriteValue(list_entry_t(current_offset, length));
		current_offset += length;
		previous_offset = offset;
	}

	idx_t child_scan_count = current_offset;
	ListVector::Reserve(result, child_scan_count);

	if (child_scan_count > 0) {
		auto &child_entry = ListVector::GetChildMutable(result);
		if (child_entry.GetType().InternalType() != PhysicalType::STRUCT &&
		    child_entry.GetType().InternalType() != PhysicalType::ARRAY &&
		    state.child_states[1].offset_in_column + child_scan_count > child_column->GetMaxEntry()) {
			throw InternalException("ListColumnData::ScanCount - internal list scan offset is out of range");
		}
		child_column->ScanCount(state.child_states[1], child_entry, child_scan_count);
	}
	state.last_offset = previous_offset;

	ListVector::SetListSize(result, child_scan_count);
	return scan_count;
}

void ListColumnData::Skip(ColumnScanState &state, idx_t count) {
	// skip inside the validity segment
	validity->Skip(state.child_states[0], count);

	// we need to read the list entries/offsets to figure out how much to skip
	// note that we only need to read the first and last entry
	// however, let's just read all "count" entries for now
	Vector offset_vector(LogicalType::UBIGINT, count);
	idx_t scan_count = ScanVector(state, offset_vector, count, ScanVectorType::SCAN_FLAT_VECTOR);
	if (scan_count == 0) {
		// no offsets were scanned - there is nothing to skip in the child, and reading offset[-1] below would be
		// an out-of-bounds read
		return;
	}

	UnifiedVectorFormat offsets;
	offset_vector.ToUnifiedFormat(offsets);
	auto data = UnifiedVectorFormat::GetData<uint64_t>(offsets);
	auto last_entry = data[offsets.sel->get_index(scan_count - 1)];
	idx_t child_scan_count = GetListLength(state.last_offset, last_entry, child_column->GetMaxEntry());
	if (child_scan_count == 0) {
		return;
	}
	state.last_offset = last_entry;

	// skip the child state forward by the child_scan_count
	child_column->Skip(state.child_states[1], child_scan_count);
}

void ListColumnData::InitializeAppend(ColumnAppendState &state) {
	// initialize the list offset append
	ColumnData::InitializeAppend(state);

	// initialize the validity append
	ColumnAppendState validity_append_state;
	validity->InitializeAppend(validity_append_state);
	state.child_appends.push_back(std::move(validity_append_state));

	// initialize the child column append
	ColumnAppendState child_append_state;
	child_column->InitializeAppend(child_append_state);
	state.child_appends.push_back(std::move(child_append_state));
}

void ListColumnData::Append(ColumnAppendState &state, const Vector &vector, idx_t count) {
	D_ASSERT(count > 0);

	// construct the list_entry_t entries to append to the column data
	auto input_offsets = vector.Values<list_entry_t>();
	auto start_offset = child_column->GetMaxEntry();
	idx_t child_count = 0;

	ValidityMask append_mask(count);
	auto append_offsets = unique_ptr<uint64_t[]>(new uint64_t[count]);
	bool child_contiguous = true;
	for (idx_t i = 0; i < count; i++) {
		auto list_entry = input_offsets[i];
		if (!list_entry.IsValid()) {
			append_mask.SetInvalid(i);
			append_offsets[i] = start_offset + child_count;
			continue;
		}
		auto &input_list = list_entry.GetValue();
		if (input_list.offset != child_count) {
			child_contiguous = false;
		}
		append_offsets[i] = start_offset + child_count + input_list.length;
		child_count += input_list.length;
	}
	auto &list_child = ListVector::GetChild(vector);
	Vector child_vector(Vector::Ref(list_child));
	if (!child_contiguous) {
		// if the child of the list vector is a non-contiguous vector (i.e. list elements are repeating or have gaps)
		// we first push a selection vector and flatten the child vector to turn it into a contiguous vector
		SelectionVector child_sel(child_count);
		idx_t current_count = 0;
		for (idx_t i = 0; i < count; i++) {
			auto list_entry = input_offsets[i];
			if (!list_entry.IsValid()) {
				continue;
			}
			auto &input_list = list_entry.GetValue();
			for (idx_t list_idx = 0; list_idx < input_list.length; list_idx++) {
				child_sel.set_index(current_count++, input_list.offset + list_idx);
			}
		}
		D_ASSERT(current_count == child_count);
		child_vector.Slice(list_child, child_sel, child_count);
	}

	UnifiedVectorFormat vdata;
	vdata.sel = FlatVector::IncrementalSelectionVector();
	vdata.data = data_ptr_cast(append_offsets.get());
	vdata.physical_type = PhysicalType::UINT64;

	// append the child vector
	if (child_count > 0) {
		child_column->Append(state.child_appends[1], child_vector, child_count);
	}
	// append the list offsets
	ColumnData::AppendData(state, vdata, count);
	// append the validity data
	vdata.validity = append_mask;
	validity->AppendData(state.child_appends[0], vdata, count);
}

void ListColumnData::FinalizeAppend(ColumnDataFinalizeAppendState &finalize_state, ColumnAppendState &state) {
	ColumnData::FinalizeAppend(finalize_state, state);
	validity->FinalizeAppendLocked(finalize_state, state.child_appends[0]);

	ColumnDataFinalizeAppendState child_finalize_state(finalize_state, LogicalTypeId::LIST);
	child_column->FinalizeAppendLocked(child_finalize_state, state.child_appends[1]);
}

void ListColumnData::RevertAppend(row_t new_count) {
	ColumnData::RevertAppend(new_count);
	validity->RevertAppend(new_count);
	// revert append in the child column, also when no list is left
	auto column_count = GetMaxEntry();
	auto list_offset = column_count > 0 ? FetchListOffset(column_count - 1) : 0;
	child_column->RevertAppend(UnsafeNumericCast<row_t>(list_offset));
}

idx_t ListColumnData::Fetch(ColumnScanState &state, row_t row_id, Vector &result) {
	throw NotImplementedException("List Fetch");
}

void ListColumnData::Update(TransactionData transaction, DuckTableEntry &table_entry, idx_t column_index,
                            Vector &update_vector, row_t *row_ids, idx_t update_count, idx_t row_group_start) {
	throw NotImplementedException("List Update is not supported.");
}

void ListColumnData::UpdateColumn(TransactionData transaction, DuckTableEntry &table_entry,
                                  const vector<column_t> &column_path, Vector &update_vector, row_t *row_ids,
                                  idx_t update_count, idx_t depth, idx_t row_group_start) {
	throw NotImplementedException("List Update Column is not supported");
}

unique_ptr<BaseStatistics> ListColumnData::GetUpdateStatistics() {
	return nullptr;
}
void ListColumnData::FetchRows(TransactionData transaction, ColumnFetchState &state, const StorageIndex &storage_index,
                               const idx_t *offsets, const SelectionVector &sel, idx_t fetch_count, Vector &result,
                               idx_t result_offset) {
	// insert the validity child state
	if (state.child_states.empty()) {
		auto child_state = make_uniq<ColumnFetchState>();
		state.child_states.push_back(std::move(child_state));
	}

	validity->FetchRowsAtSegmentLevel(transaction, *state.child_states[0], offsets, sel, fetch_count, result,
	                                  result_offset);

	// each list ends at its own offset and starts at the offset of the row before it
	Vector end_offsets(LogicalType::UBIGINT, fetch_count);
	FetchRowsAtSegmentLevel(transaction, state, offsets, sel, fetch_count, end_offsets, 0);
	auto previous_rows = make_unsafe_uniq_array<idx_t>(fetch_count);
	SelectionVector previous_sel(fetch_count);
	idx_t previous_count = 0;
	for (idx_t idx = 0; idx < fetch_count; idx++) {
		const auto row_id = offsets[sel.get_index(idx)];
		if (row_id > 0) {
			previous_rows[idx] = row_id - 1;
			previous_sel.set_index(previous_count++, idx);
		}
	}
	Vector start_offsets(LogicalType::UBIGINT, fetch_count);
	FetchRowsAtSegmentLevel(transaction, state, previous_rows.get(), previous_sel, previous_count, start_offsets, 0);
	auto end_data = FlatVector::GetData<uint64_t>(end_offsets);
	auto start_data = FlatVector::GetData<uint64_t>(start_offsets);

	auto &validity_mask = FlatVector::ValidityMutable(result);
	auto list_data = FlatVector::GetDataMutable<list_entry_t>(result);
	idx_t start_idx = 0;
	for (idx_t idx = 0; idx < fetch_count; idx++) {
		const auto row_id = offsets[sel.get_index(idx)];
		auto start_offset = row_id == 0 ? 0 : start_data[start_idx++];
		auto end_offset = end_data[idx];
		auto length = GetListLength(start_offset, end_offset, child_column->GetMaxEntry());
		auto result_idx = result_offset + idx;
		auto &list_entry = list_data[result_idx];
		// set the list entry offset to the size of the current list
		list_entry.offset = ListVector::GetListSize(result);
		list_entry.length = length;
		if (!validity_mask.RowIsValid(result_idx)) {
			// the list is NULL! no need to fetch the child
			list_entry.length = 0;
			continue;
		}

		// now we need to read from the child all the elements between [offset...length]
		auto child_scan_count = list_entry.length;
		if (child_scan_count > 0) {
			ColumnScanState child_state(nullptr);
			auto &child_type = ListType::GetChildType(result.GetType());
			Vector child_scan(child_type, child_scan_count);
			// seek the scan towards the specified position and read [length] entries
			child_state.Initialize(state.context, child_type, nullptr);
			child_column->InitializeScanWithOffset(child_state, start_offset);
			D_ASSERT(child_type.InternalType() == PhysicalType::STRUCT ||
			         child_state.offset_in_column + child_scan_count <= child_column->GetMaxEntry());
			child_column->ScanCount(child_state, child_scan, child_scan_count);

			ListVector::Append(result, child_scan, child_scan_count);
		}
	}
}

void ListColumnData::VisitBlockIds(BlockIdVisitor &visitor) const {
	ColumnData::VisitBlockIds(visitor);
	validity->VisitBlockIds(visitor);
	child_column->VisitBlockIds(visitor);
}

const BaseStatistics &ListColumnData::GetChildStats(const ColumnData &child) const {
	if (!RefersToSameObject(child, *child_column)) {
		throw InternalException("ListColumnData::GetChildStats provided column data is not a child of this list");
	}
	auto &stats = GetStatisticsRef();
	return ListStats::GetChildStats(stats);
}

void ListColumnData::SetValidityData(shared_ptr<ValidityColumnData> validity_p) {
	if (validity) {
		throw InternalException("ListColumnData::SetValidityData cannot be used to overwrite existing validity");
	}
	validity_p->SetParent(this);
	this->validity = std::move(validity_p);
}

void ListColumnData::SetChildData(shared_ptr<ColumnData> child_column_p) {
	if (child_column) {
		throw InternalException("ListColumnData::SetChildData cannot be used to overwrite existing data");
	}
	child_column_p->SetParent(this);
	this->child_column = std::move(child_column_p);
}

struct ListColumnCheckpointState : public ColumnCheckpointState {
	ListColumnCheckpointState(const RowGroup &row_group, ColumnData &column_data,
	                          PartialBlockManager &partial_block_manager)
	    : ColumnCheckpointState(row_group, column_data, partial_block_manager) {
		global_stats = ListStats::CreateEmpty(column_data.type).ToUnique();
	}

	unique_ptr<ColumnCheckpointState> validity_state;
	unique_ptr<ColumnCheckpointState> child_state;

public:
	shared_ptr<ColumnData> CreateEmptyColumnData() override {
		return make_shared_ptr<ListColumnData>(original_column.GetBlockManager(), original_column.GetTableInfo(),
		                                       original_column.column_index, original_column.type,
		                                       ColumnDataType::CHECKPOINT_TARGET, nullptr);
	}

	shared_ptr<ColumnData> GetFinalResult() override {
		if (result_column) {
			auto &column_data = result_column->Cast<ListColumnData>();
			auto validity_child = validity_state->GetFinalResult();
			column_data.SetValidityData(shared_ptr_cast<ColumnData, ValidityColumnData>(std::move(validity_child)));
			column_data.SetChildData(child_state->GetFinalResult());
		}
		return ColumnCheckpointState::GetFinalResult();
	}

	unique_ptr<BaseStatistics> GetStatistics() override {
		auto stats = global_stats->Copy();
		stats.Merge(*validity_state->GetStatistics());
		ListStats::SetChildStats(stats, child_state->GetStatistics());
		return stats.ToUnique();
	}

	PersistentColumnData ToPersistentData() override {
		auto data = ColumnCheckpointState::ToPersistentData();
		data.child_columns.push_back(validity_state->ToPersistentData());
		data.child_columns.push_back(child_state->ToPersistentData());
		return data;
	}
};

unique_ptr<ColumnCheckpointState> ListColumnData::CreateCheckpointState(const RowGroup &row_group,
                                                                        PartialBlockManager &partial_block_manager) {
	return make_uniq<ListColumnCheckpointState>(row_group, *this, partial_block_manager);
}

unique_ptr<ColumnCheckpointState> ListColumnData::Checkpoint(const RowGroup &row_group,
                                                             ColumnCheckpointInfo &checkpoint_info,
                                                             const BaseStatistics &old_stats) {
	auto base_state = ColumnData::Checkpoint(row_group, checkpoint_info, old_stats);
	auto validity_state = validity->Checkpoint(row_group, checkpoint_info, old_stats);
	auto child_state = child_column->Checkpoint(row_group, checkpoint_info, ListStats::GetChildStats(old_stats));

	auto &checkpoint_state = base_state->Cast<ListColumnCheckpointState>();
	checkpoint_state.validity_state = std::move(validity_state);
	checkpoint_state.child_state = std::move(child_state);
	return base_state;
}

bool ListColumnData::IsPersistent() {
	return ColumnData::IsPersistent() && validity->IsPersistent() && child_column->IsPersistent();
}

bool ListColumnData::HasAnyChanges() const {
	return ColumnData::HasAnyChanges() || validity->HasAnyChanges() || child_column->HasAnyChanges();
}

PersistentColumnData ListColumnData::Serialize() {
	auto persistent_data = ColumnData::Serialize();
	persistent_data.child_columns.push_back(validity->Serialize());
	persistent_data.child_columns.push_back(child_column->Serialize());
	return persistent_data;
}

void ListColumnData::InitializeColumn(PersistentColumnData &column_data, BaseStatistics &target_stats) {
	ColumnData::InitializeColumn(column_data, target_stats);
	validity->InitializeColumn(column_data.child_columns[0], target_stats);
	auto &child_stats = ListStats::GetChildStats(target_stats);
	child_column->InitializeColumn(column_data.child_columns[1], child_stats);
}

void ListColumnData::GetColumnSegmentInfo(const QueryContext &context, idx_t row_group_index, vector<idx_t> col_path,
                                          vector<ColumnSegmentInfo> &result,
                                          const ColumnSegmentInfoScanOptions &options) {
	ColumnData::GetColumnSegmentInfo(context, row_group_index, col_path, result, options);
	col_path.push_back(0);
	validity->GetColumnSegmentInfo(context, row_group_index, col_path, result, options);
	col_path.back() = 1;
	child_column->GetColumnSegmentInfo(context, row_group_index, col_path, result, options);
}

} // namespace duckdb
