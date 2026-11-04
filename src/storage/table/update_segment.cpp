#include "duckdb/storage/table/update_segment.hpp"

#include "duckdb/common/printer.hpp"
#include "duckdb/storage/table/column_data.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/update_info.hpp"
#include "duckdb/storage/statistics/stats_writer.hpp"

#include <algorithm>

namespace duckdb {

static UpdateSegment::fetch_update_function_t GetFetchUpdateFunction(PhysicalType type);
static UpdateSegment::fetch_committed_function_t GetFetchCommittedFunction(PhysicalType type);
static UpdateSegment::fetch_committed_range_function_t GetFetchCommittedRangeFunction(PhysicalType type);

static UpdateSegment::merge_update_function_t GetMergeUpdateFunction(PhysicalType type);
static UpdateSegment::save_undo_function_t GetSaveUndoFunction(PhysicalType type);
static UpdateSegment::rollback_update_function_t GetRollbackUpdateFunction(PhysicalType type);
static UpdateSegment::statistics_update_function_t GetStatisticsUpdateFunction(PhysicalType type);
static UpdateSegment::fetch_rows_function_t GetFetchRowsFunction(PhysicalType type);
static UpdateSegment::get_effective_updates_t GetEffectiveUpdatesFunction(PhysicalType type);
static UpdateSegment::store_base_values_function_t GetStoreBaseValuesFunction(PhysicalType type);

UpdateSegment::UpdateSegment(ColumnData &column_data)
    : type(column_data.type), buffer_manager(column_data.block_manager.buffer_manager),
      newest_uncheckpointed_update_commit(0), stats(column_data.type),
      heap(make_shared_ptr<StringHeap>(BufferAllocator::Get(column_data.GetDatabase()))) {
	auto physical_type = type.InternalType();

	// the WAL writer describes the updated column through the segment
	reference<const ColumnData> current_column = column_data;
	while (current_column.get().HasParent()) {
		nested_column_path.push_back(current_column.get().column_index);
		current_column = current_column.get().Parent();
	}
	std::reverse(nested_column_path.begin(), nested_column_path.end());

	this->type_size = GetTypeIdSize(physical_type);

	this->fetch_update_function = GetFetchUpdateFunction(physical_type);
	this->fetch_committed_function = GetFetchCommittedFunction(physical_type);
	this->fetch_committed_range = GetFetchCommittedRangeFunction(physical_type);
	this->fetch_rows_function = GetFetchRowsFunction(physical_type);
	this->merge_update_function = GetMergeUpdateFunction(physical_type);
	this->save_undo_function = GetSaveUndoFunction(physical_type);
	this->rollback_update_function = GetRollbackUpdateFunction(physical_type);
	this->statistics_update_function = GetStatisticsUpdateFunction(physical_type);
	this->get_effective_updates = GetEffectiveUpdatesFunction(physical_type);
	this->store_base_values_function = GetStoreBaseValuesFunction(physical_type);
}

UpdateSegment::~UpdateSegment() {
}

//===--------------------------------------------------------------------===//
// Update Info Helpers
//===--------------------------------------------------------------------===//
Value UpdateInfo::GetValue(idx_t index) {
	auto &type = segment->GetType();

	auto tuple_data = GetValues();
	switch (type.id()) {
	case LogicalTypeId::VALIDITY:
		return Value::BOOLEAN(reinterpret_cast<bool *>(tuple_data)[index]);
	case LogicalTypeId::INTEGER:
		return Value::INTEGER(reinterpret_cast<int32_t *>(tuple_data)[index]);
	default:
		throw NotImplementedException("Unimplemented type for UpdateInfo::GetValue");
	}
}

void UpdateInfo::Print() {
	Printer::Print(ToString());
}

string UpdateInfo::ToString() {
	auto &type = segment->GetType();
	string result = "Update Info [" + type.ToString() + ", Count: " + to_string(N) +
	                ", Transaction Id: " + to_string(version_number.load()) + "]\n";
	auto tuples = GetTuples();
	idx_t value_idx = 0;
	for (idx_t i = 0; i < N; i++) {
		if (value_mask_offset != 0 && !GetValueMask().RowIsValid(i)) {
			result += to_string(tuples[i]) + ": (base)\n";
			continue;
		}
		result += to_string(tuples[i]) + ": " + GetValue(value_idx++).ToString() + "\n";
	}
	if (HasNext()) {
		auto next_pin = next.Pin();
		result += "\nChild Segment: " + Get(next_pin).ToString();
	}
	return result;
}

sel_t *UpdateInfo::GetTuples() {
	return reinterpret_cast<sel_t *>(data_ptr_cast(this) + sizeof(UpdateInfo));
}

static idx_t GetValueMaskOffset(idx_t capacity) {
	return AlignValue<idx_t>(sizeof(UpdateInfo) + sizeof(sel_t) * capacity);
}

static idx_t GetTransactionValuesOffset(idx_t capacity) {
	return GetValueMaskOffset(capacity) + ValidityMask::ValidityMaskSize(capacity);
}

data_ptr_t UpdateInfo::GetValues() {
	if (value_mask_offset != 0) {
		return data_ptr_cast(this) + GetTransactionValuesOffset(max);
	}
	return data_ptr_cast(this) + sizeof(UpdateInfo) + sizeof(sel_t) * max;
}

UpdateInfo &UpdateInfo::Get(UndoBufferReference &entry) {
	auto update_info = reinterpret_cast<UpdateInfo *>(entry.GetDataMutable());
	return *update_info;
}

bool UpdateInfo::HasPrev() const {
	return prev.entry;
}

bool UpdateInfo::HasNext() const {
	return next.entry;
}

ValidityMask UpdateInfo::GetValueMask() {
	D_ASSERT(value_mask_offset != 0);
	return ValidityMask(reinterpret_cast<validity_t *>(data_ptr_cast(this) + value_mask_offset), max);
}

idx_t UpdateInfo::GetAllocSize(idx_t type_size, idx_t capacity) {
	return AlignValue<idx_t>(sizeof(UpdateInfo) + (sizeof(sel_t) + type_size) * capacity);
}

idx_t UpdateInfo::GetAllocSize(idx_t type_size, idx_t capacity, idx_t value_capacity) {
	return AlignValue<idx_t>(GetTransactionValuesOffset(capacity) + type_size * value_capacity);
}

idx_t UpdateInfo::GetCompactCapacity(idx_t count) {
	// Round up to next power of two, with a minimum of 8, capped at STANDARD_VECTOR_SIZE
	if (count <= 8) {
		return 8;
	}
	idx_t capacity = NextPowerOfTwo(count);
	return MinValue<idx_t>(capacity, STANDARD_VECTOR_SIZE);
}

void UpdateInfo::Initialize(UpdateInfo &info, DuckTableEntry &table_entry, transaction_t transaction_id,
                            idx_t row_group_start, idx_t capacity) {
	info.max = UnsafeNumericCast<sel_t>(capacity);
	info.row_group_start = row_group_start;
	info.table = &table_entry;
	info.column_index = COLUMN_IDENTIFIER_ROW_ID;
	info.segment = nullptr;
	info.vector_index = 0;
	info.prev = UndoBufferPointer();
	info.next = UndoBufferPointer();
	info.N = 0;
	info.version_number = transaction_id;
	info.value_mask_offset = 0;
	info.value_max = UnsafeNumericCast<sel_t>(capacity);
}

void UpdateInfo::Initialize(UpdateInfo &info, DuckTableEntry &table_entry, transaction_t transaction_id,
                            idx_t row_group_start, idx_t capacity, idx_t value_capacity) {
	D_ASSERT(value_capacity <= capacity);
	Initialize(info, table_entry, transaction_id, row_group_start, capacity);
	info.value_mask_offset = GetValueMaskOffset(capacity);
	info.value_max = UnsafeNumericCast<sel_t>(value_capacity);
}

void UpdateInfo::Verify() {
#ifdef DEBUG
	D_ASSERT(N <= max);
	D_ASSERT(value_mask_offset == 0 || GetValueMask().CountValid(N) <= value_max);
	auto tuples = GetTuples();
	for (idx_t i = 1; i < N; i++) {
		D_ASSERT(tuples[i] > tuples[i - 1] && tuples[i] < STANDARD_VECTOR_SIZE);
	}
#endif
}

//===--------------------------------------------------------------------===//
// Update Fetch
//===--------------------------------------------------------------------===//
static void MergeValidityInfo(UpdateInfo &current, ValidityMask &result_mask) {
	auto tuples = current.GetTuples();
	auto info_data = current.GetData<bool>();
	for (idx_t i = 0; i < current.N; i++) {
		result_mask.Set(tuples[i], info_data[i]);
	}
}

template <class T>
static void MergeUpdateInfo(UpdateInfo &current, T *result_data) {
	auto tuples = current.GetTuples();
	auto info_data = current.GetData<T>();
	if (current.N == STANDARD_VECTOR_SIZE) {
		// special case: update touches ALL tuples of this vector
		// in this case we can just memcpy the data
		// since the layout of the update info is guaranteed to be [0, 1, 2, 3, ...]
		memcpy(result_data, info_data, sizeof(T) * current.N);
	} else {
		for (idx_t i = 0; i < current.N; i++) {
			result_data[tuples[i]] = info_data[i];
		}
	}
}

template <class T>
struct UpdateDataResult {
	explicit UpdateDataResult(Vector &result) : data(FlatVector::GetDataMutable<T>(result)) {
	}
	T Get(idx_t idx) const {
		return data[idx];
	}
	void Set(idx_t idx, T value) {
		data[idx] = value;
	}
	void Apply(UpdateInfo &info) {
		MergeUpdateInfo<T>(info, data);
	}

	T *data;
};

struct UpdateValidityResult {
	explicit UpdateValidityResult(Vector &result) : mask(FlatVector::ValidityMutable(result)) {
	}
	bool Get(idx_t idx) const {
		return mask.RowIsValid(idx);
	}
	void Set(idx_t idx, bool value) {
		mask.Set(idx, value);
	}
	void Apply(UpdateInfo &info) {
		MergeValidityInfo(info, mask);
	}

	ValidityMask &mask;
};

//! Apply the stored values of an update info to the tuples that are valid in the apply mask
template <class T, class RESULT>
static void ApplyUpdateInfo(UpdateInfo &info, const ValidityMask &apply_mask, RESULT &result) {
	bool has_value_mask = info.value_mask_offset != 0;
	ValidityMask value_mask;
	if (has_value_mask) {
		value_mask = info.GetValueMask();
	}
	if (info.N == STANDARD_VECTOR_SIZE && (!has_value_mask || value_mask.CheckAllValid(info.N))) {
		// the update info contains every tuple of the vector with a value
		if (apply_mask.CheckAllInvalid(STANDARD_VECTOR_SIZE)) {
			return;
		}
		if (apply_mask.CheckAllValid(STANDARD_VECTOR_SIZE)) {
			result.Apply(info);
			return;
		}
	}
	auto tuples = info.GetTuples();
	auto info_data = info.GetData<T>();
	idx_t value_idx = 0;
	for (idx_t i = 0; i < info.N; i++) {
		if (has_value_mask && !value_mask.RowIsValid(i)) {
			continue;
		}
		auto value = info_data[value_idx++];
		if (apply_mask.RowIsValid(tuples[i])) {
			result.Set(tuples[i], value);
		}
	}
}

template <class T, class RESULT>
static void FetchUpdatesInternal(const SnapshotView &view, UpdateInfo &root, RESULT &result) {
	vector<UndoBufferReference> invisible;
	UpdateInfo::GetInvisibleUpdates(root, view, invisible);
	if (invisible.empty()) {
		// the root holds the latest values
		result.Apply(root);
		return;
	}
	// a tuple without a stored value in an invisible update was first updated by that update: it keeps its base value
	validity_t apply_bits[ValidityMask::STANDARD_ENTRY_COUNT];
	memset(apply_bits, 0xFF, sizeof(apply_bits));
	ValidityMask apply_mask(apply_bits, STANDARD_VECTOR_SIZE);
	bool any_keep_base = false;
	for (auto &pin : invisible) {
		auto &info = UpdateInfo::Get(pin);
		auto value_mask = info.GetValueMask();
		if (value_mask.CheckAllValid(info.N)) {
			continue;
		}
		any_keep_base = true;
		auto tuples = info.GetTuples();
		for (idx_t i = 0; i < info.N; i++) {
			if (!value_mask.RowIsValid(i)) {
				apply_mask.SetInvalid(tuples[i]);
			}
		}
	}
	if (!any_keep_base) {
		// all values from before the invisible updates are stored in the update infos
		result.Apply(root);
		for (auto &pin : invisible) {
			result.Apply(UpdateInfo::Get(pin));
		}
		return;
	}
	ApplyUpdateInfo<T>(root, apply_mask, result);
	// undo the invisible updates from newest to oldest
	for (auto &pin : invisible) {
		ApplyUpdateInfo<T>(UpdateInfo::Get(pin), apply_mask, result);
	}
}

static void UpdateMergeValidity(const SnapshotView &view, UpdateInfo &info, Vector &result) {
	UpdateValidityResult validity_result(result);
	FetchUpdatesInternal<bool>(view, info, validity_result);
}

template <class T>
static void UpdateMergeFetch(const SnapshotView &view, UpdateInfo &info, Vector &result) {
	UpdateDataResult<T> data_result(result);
	FetchUpdatesInternal<T>(view, info, data_result);
}

static UpdateSegment::fetch_update_function_t GetFetchUpdateFunction(PhysicalType type) {
	switch (type) {
	case PhysicalType::BIT:
		return UpdateMergeValidity;
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		return UpdateMergeFetch<int8_t>;
	case PhysicalType::INT16:
		return UpdateMergeFetch<int16_t>;
	case PhysicalType::INT32:
		return UpdateMergeFetch<int32_t>;
	case PhysicalType::INT64:
		return UpdateMergeFetch<int64_t>;
	case PhysicalType::UINT8:
		return UpdateMergeFetch<uint8_t>;
	case PhysicalType::UINT16:
		return UpdateMergeFetch<uint16_t>;
	case PhysicalType::UINT32:
		return UpdateMergeFetch<uint32_t>;
	case PhysicalType::UINT64:
		return UpdateMergeFetch<uint64_t>;
	case PhysicalType::INT128:
		return UpdateMergeFetch<hugeint_t>;
	case PhysicalType::UINT128:
		return UpdateMergeFetch<uhugeint_t>;
	case PhysicalType::FLOAT:
		return UpdateMergeFetch<float>;
	case PhysicalType::DOUBLE:
		return UpdateMergeFetch<double>;
	case PhysicalType::INTERVAL:
		return UpdateMergeFetch<interval_t>;
	case PhysicalType::VARCHAR:
		return UpdateMergeFetch<string_t>;
	default:
		throw NotImplementedException("Unimplemented type for update segment");
	}
}

UndoBufferPointer UpdateSegment::GetUpdateNode(StorageLockKey &, idx_t vector_idx) const {
	if (!root) {
		return UndoBufferPointer();
	}
	if (vector_idx >= root->info.size()) {
		return UndoBufferPointer();
	}
	return root->info[vector_idx];
}

//! Keeps the string heap of an update segment alive for a vector holding strings from it
class UpdateStringHeapHolder : public AuxiliaryDataHolder {
public:
	explicit UpdateStringHeapHolder(shared_ptr<StringHeap> heap_p) : heap(std::move(heap_p)) {
	}

private:
	shared_ptr<StringHeap> heap;
};

void UpdateSegment::AddHeapReference(Vector &result) {
	if (type.InternalType() == PhysicalType::VARCHAR) {
		result.AddAuxiliaryData(make_uniq<UpdateStringHeapHolder>(heap));
	}
}

void UpdateSegment::FetchUpdates(TransactionData transaction, idx_t vector_index, Vector &result) {
	auto lock_handle = lock.GetSharedLock();
	auto node = GetUpdateNode(*lock_handle, vector_index);
	if (!node.IsSet()) {
		return;
	}
	// FIXME: normalify if this is not the case... need to pass in count?
	D_ASSERT(result.GetVectorType() == VectorType::FLAT_VECTOR);
	auto pin = node.Pin();
	fetch_update_function(transaction.view, UpdateInfo::Get(pin), result);
	AddHeapReference(result);
}

UpdateNode::UpdateNode(BufferManager &manager) : allocator(manager, false) {
}

UpdateNode::~UpdateNode() {
}

//===--------------------------------------------------------------------===//
// Fetch Committed
//===--------------------------------------------------------------------===//
static void FetchCommittedValidity(UpdateInfo &info, Vector &result) {
	auto &result_mask = FlatVector::ValidityMutable(result);
	MergeValidityInfo(info, result_mask);
}

template <class T>
static void TemplatedFetchCommitted(UpdateInfo &info, Vector &result) {
	auto result_data = FlatVector::GetDataMutable<T>(result);
	MergeUpdateInfo<T>(info, result_data);
}

static UpdateSegment::fetch_committed_function_t GetFetchCommittedFunction(PhysicalType type) {
	switch (type) {
	case PhysicalType::BIT:
		return FetchCommittedValidity;
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		return TemplatedFetchCommitted<int8_t>;
	case PhysicalType::INT16:
		return TemplatedFetchCommitted<int16_t>;
	case PhysicalType::INT32:
		return TemplatedFetchCommitted<int32_t>;
	case PhysicalType::INT64:
		return TemplatedFetchCommitted<int64_t>;
	case PhysicalType::UINT8:
		return TemplatedFetchCommitted<uint8_t>;
	case PhysicalType::UINT16:
		return TemplatedFetchCommitted<uint16_t>;
	case PhysicalType::UINT32:
		return TemplatedFetchCommitted<uint32_t>;
	case PhysicalType::UINT64:
		return TemplatedFetchCommitted<uint64_t>;
	case PhysicalType::INT128:
		return TemplatedFetchCommitted<hugeint_t>;
	case PhysicalType::UINT128:
		return TemplatedFetchCommitted<uhugeint_t>;
	case PhysicalType::FLOAT:
		return TemplatedFetchCommitted<float>;
	case PhysicalType::DOUBLE:
		return TemplatedFetchCommitted<double>;
	case PhysicalType::INTERVAL:
		return TemplatedFetchCommitted<interval_t>;
	case PhysicalType::VARCHAR:
		return TemplatedFetchCommitted<string_t>;
	default:
		throw NotImplementedException("Unimplemented type for update segment");
	}
}

void UpdateSegment::FetchCommitted(idx_t vector_index, Vector &result) {
	auto lock_handle = lock.GetSharedLock();
	auto node = GetUpdateNode(*lock_handle, vector_index);
	if (!node.IsSet()) {
		return;
	}
	// FIXME: normalify if this is not the case... need to pass in count?
	D_ASSERT(result.GetVectorType() == VectorType::FLAT_VECTOR);
	auto pin = node.Pin();
	fetch_committed_function(UpdateInfo::Get(pin), result);
}

//===--------------------------------------------------------------------===//
// Fetch Range
//===--------------------------------------------------------------------===//
static void MergeUpdateInfoRangeValidity(UpdateInfo &current, idx_t start, idx_t end, idx_t result_offset,
                                         ValidityMask &result_mask) {
	auto tuples = current.GetTuples();
	auto info_data = current.GetData<bool>();
	for (idx_t i = 0; i < current.N; i++) {
		auto tuple_idx = tuples[i];
		if (tuple_idx < start) {
			continue;
		} else if (tuple_idx >= end) {
			break;
		}
		auto result_idx = result_offset + tuple_idx - start;
		result_mask.Set(result_idx, info_data[i]);
	}
}

//! Applies updates to the tuples [start, end) of a vector, written to the result from result_offset on
struct UpdateRangeValidityResult {
	UpdateRangeValidityResult(Vector &result, idx_t start, idx_t end, idx_t result_offset)
	    : mask(FlatVector::ValidityMutable(result)), start(start), end(end), result_offset(result_offset) {
	}
	void Set(idx_t idx, bool value) {
		if (idx >= start && idx < end) {
			mask.Set(result_offset + idx - start, value);
		}
	}
	void Apply(UpdateInfo &info) {
		MergeUpdateInfoRangeValidity(info, start, end, result_offset, mask);
	}

	ValidityMask &mask;
	idx_t start;
	idx_t end;
	idx_t result_offset;
};

static void FetchCommittedRangeValidity(const SnapshotView &view, UpdateInfo &info, idx_t start, idx_t end,
                                        idx_t result_offset, Vector &result) {
	UpdateRangeValidityResult range_result(result, start, end, result_offset);
	FetchUpdatesInternal<bool>(view, info, range_result);
}

template <class T>
static void MergeUpdateInfoRange(UpdateInfo &current, idx_t start, idx_t end, idx_t result_offset, T *result_data) {
	auto tuples = current.GetTuples();
	auto info_data = current.GetData<T>();
	for (idx_t i = 0; i < current.N; i++) {
		auto tuple_idx = tuples[i];
		if (tuple_idx < start) {
			continue;
		} else if (tuple_idx >= end) {
			break;
		}
		auto result_idx = result_offset + tuple_idx - start;
		result_data[result_idx] = info_data[i];
	}
}

template <class T>
struct UpdateRangeDataResult {
	UpdateRangeDataResult(Vector &result, idx_t start, idx_t end, idx_t result_offset)
	    : data(FlatVector::GetDataMutable<T>(result)), start(start), end(end), result_offset(result_offset) {
	}
	void Set(idx_t idx, T value) {
		if (idx >= start && idx < end) {
			data[result_offset + idx - start] = value;
		}
	}
	void Apply(UpdateInfo &info) {
		MergeUpdateInfoRange<T>(info, start, end, result_offset, data);
	}

	T *data;
	idx_t start;
	idx_t end;
	idx_t result_offset;
};

template <class T>
static void TemplatedFetchCommittedRange(const SnapshotView &view, UpdateInfo &info, idx_t start, idx_t end,
                                         idx_t result_offset, Vector &result) {
	UpdateRangeDataResult<T> range_result(result, start, end, result_offset);
	FetchUpdatesInternal<T>(view, info, range_result);
}

static UpdateSegment::fetch_committed_range_function_t GetFetchCommittedRangeFunction(PhysicalType type) {
	switch (type) {
	case PhysicalType::BIT:
		return FetchCommittedRangeValidity;
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		return TemplatedFetchCommittedRange<int8_t>;
	case PhysicalType::INT16:
		return TemplatedFetchCommittedRange<int16_t>;
	case PhysicalType::INT32:
		return TemplatedFetchCommittedRange<int32_t>;
	case PhysicalType::INT64:
		return TemplatedFetchCommittedRange<int64_t>;
	case PhysicalType::UINT8:
		return TemplatedFetchCommittedRange<uint8_t>;
	case PhysicalType::UINT16:
		return TemplatedFetchCommittedRange<uint16_t>;
	case PhysicalType::UINT32:
		return TemplatedFetchCommittedRange<uint32_t>;
	case PhysicalType::UINT64:
		return TemplatedFetchCommittedRange<uint64_t>;
	case PhysicalType::INT128:
		return TemplatedFetchCommittedRange<hugeint_t>;
	case PhysicalType::UINT128:
		return TemplatedFetchCommittedRange<uhugeint_t>;
	case PhysicalType::FLOAT:
		return TemplatedFetchCommittedRange<float>;
	case PhysicalType::DOUBLE:
		return TemplatedFetchCommittedRange<double>;
	case PhysicalType::INTERVAL:
		return TemplatedFetchCommittedRange<interval_t>;
	case PhysicalType::VARCHAR:
		return TemplatedFetchCommittedRange<string_t>;
	default:
		throw NotImplementedException("Unimplemented type for update segment");
	}
}

void UpdateSegment::FetchCommittedRange(const SnapshotView &view, idx_t start_row, idx_t count, Vector &result) {
	D_ASSERT(count > 0);
	D_ASSERT(result.GetVectorType() == VectorType::FLAT_VECTOR);
	auto lock_handle = lock.GetSharedLock();
	if (!root) {
		return;
	}

	idx_t end_row = start_row + count;
	idx_t start_vector = start_row / STANDARD_VECTOR_SIZE;
	idx_t end_vector = (end_row - 1) / STANDARD_VECTOR_SIZE;
	D_ASSERT(start_vector <= end_vector);

	bool fetched = false;
	for (idx_t vector_idx = start_vector; vector_idx <= end_vector; vector_idx++) {
		auto entry = GetUpdateNode(*lock_handle, vector_idx);
		if (!entry.IsSet()) {
			continue;
		}
		auto pin = entry.Pin();
		idx_t start_in_vector = vector_idx == start_vector ? start_row - start_vector * STANDARD_VECTOR_SIZE : 0;
		idx_t end_in_vector =
		    vector_idx == end_vector ? end_row - end_vector * STANDARD_VECTOR_SIZE : STANDARD_VECTOR_SIZE;
		D_ASSERT(start_in_vector < end_in_vector);
		D_ASSERT(end_in_vector > 0 && end_in_vector <= STANDARD_VECTOR_SIZE);
		idx_t result_offset = ((vector_idx * STANDARD_VECTOR_SIZE) + start_in_vector) - start_row;
		fetch_committed_range(view, UpdateInfo::Get(pin), start_in_vector, end_in_vector, result_offset, result);
		fetched = true;
	}
	if (fetched) {
		AddHeapReference(result);
	}
}

//===--------------------------------------------------------------------===//
// Fetch Rows
//===--------------------------------------------------------------------===//
static bool FindUpdatedTuple(UpdateInfo &current, idx_t row_idx, idx_t &update_idx) {
	auto tuples = current.GetTuples();
	idx_t left = 0;
	idx_t right = current.N;
	while (left < right) {
		idx_t mid = left + (right - left) / 2;
		if (tuples[mid] == row_idx) {
			update_idx = mid;
			return true;
		} else if (tuples[mid] < row_idx) {
			left = mid + 1;
		} else {
			right = mid;
		}
	}
	return false;
}

template <class T, class RESULT>
static void FetchRowsInternal(const SnapshotView &view, UpdateInfo &root, const idx_t *offsets,
                              const SelectionVector &sel, idx_t fetch_offset, idx_t count, idx_t vector_offset,
                              RESULT &result, idx_t result_offset) {
	vector<UndoBufferReference> invisible;
	UpdateInfo::GetInvisibleUpdates(root, view, invisible);
	auto root_data = root.GetData<T>();
	for (idx_t idx = 0; idx < count; idx++) {
		const idx_t row_idx = offsets[sel.get_index(fetch_offset + idx)] - vector_offset;
		const idx_t result_idx = result_offset + fetch_offset + idx;
		idx_t update_idx;
		if (!FindUpdatedTuple(root, row_idx, update_idx)) {
			// every update in the chain is also present in the root
			continue;
		}
		auto base_value = result.Get(result_idx);
		auto value = root_data[update_idx];
		// undo the invisible updates from newest to oldest
		for (auto &pin : invisible) {
			auto &info = UpdateInfo::Get(pin);
			if (!FindUpdatedTuple(info, row_idx, update_idx)) {
				continue;
			}
			auto value_mask = info.GetValueMask();
			if (!value_mask.RowIsValid(update_idx)) {
				value = base_value;
			} else {
				// values are only stored for the valid tuples
				value = info.GetData<T>()[value_mask.CountValid(update_idx)];
			}
		}
		result.Set(result_idx, value);
	}
}

static void FetchRowsValidity(const SnapshotView &view, UpdateInfo &info, const idx_t *offsets,
                              const SelectionVector &sel, idx_t fetch_offset, idx_t count, idx_t vector_offset,
                              Vector &result, idx_t result_offset) {
	UpdateValidityResult validity_result(result);
	FetchRowsInternal<bool>(view, info, offsets, sel, fetch_offset, count, vector_offset, validity_result,
	                        result_offset);
}

template <class T>
static void TemplatedFetchRows(const SnapshotView &view, UpdateInfo &info, const idx_t *offsets,
                               const SelectionVector &sel, idx_t fetch_offset, idx_t count, idx_t vector_offset,
                               Vector &result, idx_t result_offset) {
	UpdateDataResult<T> data_result(result);
	FetchRowsInternal<T>(view, info, offsets, sel, fetch_offset, count, vector_offset, data_result, result_offset);
}

static UpdateSegment::fetch_rows_function_t GetFetchRowsFunction(PhysicalType type) {
	switch (type) {
	case PhysicalType::BIT:
		return FetchRowsValidity;
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		return TemplatedFetchRows<int8_t>;
	case PhysicalType::INT16:
		return TemplatedFetchRows<int16_t>;
	case PhysicalType::INT32:
		return TemplatedFetchRows<int32_t>;
	case PhysicalType::INT64:
		return TemplatedFetchRows<int64_t>;
	case PhysicalType::UINT8:
		return TemplatedFetchRows<uint8_t>;
	case PhysicalType::UINT16:
		return TemplatedFetchRows<uint16_t>;
	case PhysicalType::UINT32:
		return TemplatedFetchRows<uint32_t>;
	case PhysicalType::UINT64:
		return TemplatedFetchRows<uint64_t>;
	case PhysicalType::INT128:
		return TemplatedFetchRows<hugeint_t>;
	case PhysicalType::UINT128:
		return TemplatedFetchRows<uhugeint_t>;
	case PhysicalType::FLOAT:
		return TemplatedFetchRows<float>;
	case PhysicalType::DOUBLE:
		return TemplatedFetchRows<double>;
	case PhysicalType::INTERVAL:
		return TemplatedFetchRows<interval_t>;
	case PhysicalType::VARCHAR:
		return TemplatedFetchRows<string_t>;
	default:
		throw NotImplementedException("Unimplemented type for update segment fetch rows");
	}
}

void UpdateSegment::FetchRows(TransactionData transaction, const idx_t *offsets, const SelectionVector &sel,
                              idx_t fetch_count, Vector &result, idx_t result_offset) {
	if (fetch_count == 0) {
		return;
	}

	auto lock_handle = lock.GetSharedLock();
	if (!root) {
		return;
	}
	bool fetched = false;
	for (idx_t idx = 0; idx < fetch_count;) {
		const idx_t offset = offsets[sel.get_index(idx)];
		const idx_t vector_index = offset / STANDARD_VECTOR_SIZE;
		const idx_t vector_offset = vector_index * STANDARD_VECTOR_SIZE;
		idx_t vector_count = 1;
		while (idx + vector_count < fetch_count) {
			const idx_t next_offset = offsets[sel.get_index(idx + vector_count)];
			if (next_offset / STANDARD_VECTOR_SIZE != vector_index) {
				break;
			}
			vector_count++;
		}

		auto entry = GetUpdateNode(*lock_handle, vector_index);
		if (entry.IsSet()) {
			auto pin = entry.Pin();
			fetch_rows_function(transaction.view, UpdateInfo::Get(pin), offsets, sel, idx, vector_count, vector_offset,
			                    result, result_offset);
			fetched = true;
		}
		idx += vector_count;
	}
	if (fetched) {
		AddHeapReference(result);
	}
}

//===--------------------------------------------------------------------===//
// Rollback update
//===--------------------------------------------------------------------===//
template <class T>
static void RollbackUpdate(UpdateInfo &root, UpdateInfo &rollback_info) {
	auto root_data = root.GetData<T>();
	auto root_tuples = root.GetTuples();
	auto rollback_data = rollback_info.GetData<T>();
	auto rollback_tuples = rollback_info.GetTuples();
	auto value_mask = rollback_info.GetValueMask();
	idx_t rollback_idx = 0;
	idx_t rollback_value_idx = 0;
	idx_t result_count = 0;
	for (idx_t i = 0; i < root.N; i++) {
		auto id = root_tuples[i];
		auto value = root_data[i];
		if (rollback_idx < rollback_info.N && rollback_tuples[rollback_idx] == id) {
			if (!value_mask.RowIsValid(rollback_idx++)) {
				// the tuple had no update before: remove it from the root
				continue;
			}
			value = rollback_data[rollback_value_idx++];
		}
		root_tuples[result_count] = id;
		root_data[result_count++] = value;
	}
	D_ASSERT(rollback_idx == rollback_info.N);
	root.N = UnsafeNumericCast<sel_t>(result_count);
}

static UpdateSegment::rollback_update_function_t GetRollbackUpdateFunction(PhysicalType type) {
	switch (type) {
	case PhysicalType::BIT:
		return RollbackUpdate<bool>;
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		return RollbackUpdate<int8_t>;
	case PhysicalType::INT16:
		return RollbackUpdate<int16_t>;
	case PhysicalType::INT32:
		return RollbackUpdate<int32_t>;
	case PhysicalType::INT64:
		return RollbackUpdate<int64_t>;
	case PhysicalType::UINT8:
		return RollbackUpdate<uint8_t>;
	case PhysicalType::UINT16:
		return RollbackUpdate<uint16_t>;
	case PhysicalType::UINT32:
		return RollbackUpdate<uint32_t>;
	case PhysicalType::UINT64:
		return RollbackUpdate<uint64_t>;
	case PhysicalType::INT128:
		return RollbackUpdate<hugeint_t>;
	case PhysicalType::UINT128:
		return RollbackUpdate<uhugeint_t>;
	case PhysicalType::FLOAT:
		return RollbackUpdate<float>;
	case PhysicalType::DOUBLE:
		return RollbackUpdate<double>;
	case PhysicalType::INTERVAL:
		return RollbackUpdate<interval_t>;
	case PhysicalType::VARCHAR:
		return RollbackUpdate<string_t>;
	default:
		throw NotImplementedException("Unimplemented type for uncompressed segment");
	}
}

void UpdateSegment::RollbackUpdate(UpdateInfo &info) {
	// obtain an exclusive lock
	auto lock_handle = lock.GetExclusiveLock();

	if (info.N == 0 || !info.HasPrev()) {
		// this update info was replaced by a larger one, has no updates, or was never linked (the update failed)
		return;
	}
	// move the data from the UpdateInfo back into the root
	auto entry = GetUpdateNode(*lock_handle, info.vector_index);
	if (!entry.IsSet()) {
		return;
	}
	auto pin = entry.Pin();
	rollback_update_function(UpdateInfo::Get(pin), info);

	// clean up the update chain
	CleanupUpdateInternal(*lock_handle, info);
}

//===--------------------------------------------------------------------===//
// Cleanup Update
//===--------------------------------------------------------------------===//
void UpdateSegment::CleanupUpdateInternal(const StorageLockKey &lock, UpdateInfo &info) {
	// pin both neighbours before modifying either: Pin can throw (OOM), and a half-unlinked node corrupts the chain
	UndoBufferReference prev_pin;
	UndoBufferReference next_pin;
	if (info.HasPrev()) {
		prev_pin = info.prev.Pin();
	}
	if (info.HasNext()) {
		next_pin = info.next.Pin();
	}
	if (prev_pin.IsSet()) {
		UpdateInfo::Get(prev_pin).next = info.next;
	}
	if (next_pin.IsSet()) {
		UpdateInfo::Get(next_pin).prev = info.prev;
	}
}

void UpdateSegment::CleanupUpdate(UpdateInfo &info) {
	// obtain an exclusive lock
	auto lock_handle = lock.GetExclusiveLock();
	if (!info.HasPrev() && info.HasNext()) {
		// a checkpoint replaced this update info by a copy with its base values: clean up the copy
		auto copy_pin = info.next.Pin();
		CleanupUpdateInternal(*lock_handle, UpdateInfo::Get(copy_pin));
		return;
	}
	CleanupUpdateInternal(*lock_handle, info);
}

//===--------------------------------------------------------------------===//
// Find the update of the transaction
//===--------------------------------------------------------------------===//
//! The transaction's own update of the vector, if any (conflicts are detected in the version info)
static UndoBufferReference GetTransactionUpdate(UndoBufferPointer next_ptr, TransactionData transaction) {
	while (next_ptr.IsSet()) {
		auto pin = next_ptr.Pin();
		auto &info = UpdateInfo::Get(pin);
		if (info.version_number == transaction.GetTransactionId()) {
			return pin;
		}
		next_ptr = info.next;
	}
	return UndoBufferReference();
}

//===--------------------------------------------------------------------===//
// Merge update info
//===--------------------------------------------------------------------===//
//! Merge "count" new tuples (sorted by id) into the info - new values replace existing values of the same tuple
template <class T, class FETCH_ID, class FETCH_VALUE>
static void MergeIntoUpdateInfo(UpdateInfo &info, idx_t count, FETCH_ID &&fetch_id, FETCH_VALUE &&fetch_value) {
	auto info_data = info.GetData<T>();
	auto info_tuples = info.GetTuples();
	if (info.N == 0) {
		// no existing updates: write the new tuples directly
		D_ASSERT(count <= info.max);
		for (idx_t i = 0; i < count; i++) {
			info_tuples[i] = fetch_id(i);
			info_data[i] = fetch_value(i);
		}
		info.N = UnsafeNumericCast<sel_t>(count);
		return;
	}
	// merge into temporary arrays, and copy them back afterwards
	T result_values[STANDARD_VECTOR_SIZE];
	sel_t result_ids[STANDARD_VECTOR_SIZE];
	idx_t info_idx = 0;
	idx_t new_idx = 0;
	idx_t result_count = 0;
	while (info_idx < info.N && new_idx < count) {
		auto new_id = fetch_id(new_idx);
		auto old_id = info_tuples[info_idx];
		if (new_id <= old_id) {
			if (new_id == old_id) {
				// the new value replaces the old value
				info_idx++;
			}
			result_ids[result_count] = new_id;
			result_values[result_count++] = fetch_value(new_idx++);
		} else {
			result_ids[result_count] = old_id;
			result_values[result_count++] = info_data[info_idx++];
		}
	}
	for (; info_idx < info.N; info_idx++) {
		result_ids[result_count] = info_tuples[info_idx];
		result_values[result_count++] = info_data[info_idx];
	}
	for (; new_idx < count; new_idx++) {
		result_ids[result_count] = fetch_id(new_idx);
		result_values[result_count++] = fetch_value(new_idx);
	}
	D_ASSERT(result_count <= info.max);
	info.N = UnsafeNumericCast<sel_t>(result_count);
	memcpy(info_data, result_values, result_count * sizeof(T));
	memcpy(info_tuples, result_ids, result_count * sizeof(sel_t));
}

static void VerifyUpdateIds(row_t *ids, idx_t count, const SelectionVector &sel, idx_t vector_offset) {
#ifdef DEBUG
	// the ids should be sorted, unique and part of the same vector
	for (idx_t i = 0; i < count; i++) {
		auto id = ids[sel.get_index(i)];
		D_ASSERT(id >= row_t(vector_offset) && id < row_t(vector_offset + STANDARD_VECTOR_SIZE));
		D_ASSERT(i == 0 || id > ids[sel.get_index(i - 1)]);
	}
#endif
}

static void MergeValidityLoop(UpdateInfo &info, UnifiedVectorFormat &update, row_t *ids, idx_t count,
                              const SelectionVector &sel, idx_t vector_offset) {
	VerifyUpdateIds(ids, count, sel, vector_offset);
	auto &update_validity = update.validity;
	MergeIntoUpdateInfo<bool>(
	    info, count,
	    [&](idx_t i) {
		    return UnsafeNumericCast<sel_t>(UnsafeNumericCast<idx_t>(ids[sel.get_index(i)]) - vector_offset);
	    },
	    [&](idx_t i) { return update_validity.RowIsValid(update.sel->get_index(sel.get_index(i))); });
}

template <class T>
static void MergeUpdateLoop(UpdateInfo &info, UnifiedVectorFormat &update, row_t *ids, idx_t count,
                            const SelectionVector &sel, idx_t vector_offset) {
	VerifyUpdateIds(ids, count, sel, vector_offset);
	auto update_data = UnifiedVectorFormat::GetData<T>(update);
	MergeIntoUpdateInfo<T>(
	    info, count,
	    [&](idx_t i) {
		    return UnsafeNumericCast<sel_t>(UnsafeNumericCast<idx_t>(ids[sel.get_index(i)]) - vector_offset);
	    },
	    [&](idx_t i) { return update_data[update.sel->get_index(sel.get_index(i))]; });
}

static UpdateSegment::merge_update_function_t GetMergeUpdateFunction(PhysicalType type) {
	switch (type) {
	case PhysicalType::BIT:
		return MergeValidityLoop;
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		return MergeUpdateLoop<int8_t>;
	case PhysicalType::INT16:
		return MergeUpdateLoop<int16_t>;
	case PhysicalType::INT32:
		return MergeUpdateLoop<int32_t>;
	case PhysicalType::INT64:
		return MergeUpdateLoop<int64_t>;
	case PhysicalType::UINT8:
		return MergeUpdateLoop<uint8_t>;
	case PhysicalType::UINT16:
		return MergeUpdateLoop<uint16_t>;
	case PhysicalType::UINT32:
		return MergeUpdateLoop<uint32_t>;
	case PhysicalType::UINT64:
		return MergeUpdateLoop<uint64_t>;
	case PhysicalType::INT128:
		return MergeUpdateLoop<hugeint_t>;
	case PhysicalType::UINT128:
		return MergeUpdateLoop<uhugeint_t>;
	case PhysicalType::FLOAT:
		return MergeUpdateLoop<float>;
	case PhysicalType::DOUBLE:
		return MergeUpdateLoop<double>;
	case PhysicalType::INTERVAL:
		return MergeUpdateLoop<interval_t>;
	case PhysicalType::VARCHAR:
		return MergeUpdateLoop<string_t>;
	default:
		throw NotImplementedException("Unimplemented type for uncompressed segment");
	}
}

//! Writes a validity mask in order, one entry at a time
struct ValidityAppender {
	explicit ValidityAppender(validity_t *data_p) : data(data_p) {
	}

	void Append(bool valid) {
		current |= validity_t(valid) << (count % ValidityMask::BITS_PER_VALUE);
		count++;
		if (count % ValidityMask::BITS_PER_VALUE == 0) {
			data[count / ValidityMask::BITS_PER_VALUE - 1] = current;
			current = 0;
		}
	}
	void Finalize() {
		if (count % ValidityMask::BITS_PER_VALUE != 0) {
			data[count / ValidityMask::BITS_PER_VALUE] = current;
		}
	}

	validity_t *data;
	validity_t current = 0;
	idx_t count = 0;
};

//! Count the tuples and values the update info of the transaction requires after adding the new tuples
static void CountUndoValues(optional_ptr<UpdateInfo> node, UpdateInfo &root, row_t *ids, idx_t count,
                            const SelectionVector &sel, idx_t vector_offset, idx_t &tuple_count, idx_t &value_count) {
	idx_t node_count = node ? node->N : 0;
	auto node_tuples = node ? node->GetTuples() : nullptr;
	auto root_tuples = root.GetTuples();
	tuple_count = node_count;
	value_count = node ? node->GetValueMask().CountValid(node_count) : 0;
	idx_t node_idx = 0;
	idx_t root_idx = 0;
	for (idx_t i = 0; i < count; i++) {
		auto id = UnsafeNumericCast<sel_t>(UnsafeNumericCast<idx_t>(ids[sel.get_index(i)]) - vector_offset);
		while (node_idx < node_count && node_tuples[node_idx] < id) {
			node_idx++;
		}
		if (node_idx < node_count && node_tuples[node_idx] == id) {
			continue;
		}
		while (root_idx < root.N && root_tuples[root_idx] < id) {
			root_idx++;
		}
		tuple_count++;
		if (root_idx < root.N && root_tuples[root_idx] == id) {
			value_count++;
		}
	}
}

//! Merge the tuples in the update info of the transaction with the new tuples into the output arrays
//! For tuples that are not yet in the root, the value before the update is the base value: these are flagged
template <class T>
static idx_t MergeUndoValues(UpdateInfo &node, UpdateInfo &root, row_t *ids, idx_t count, const SelectionVector &sel,
                             idx_t vector_offset, sel_t *out_tuples, T *out_data, validity_t *out_mask) {
	auto node_tuples = node.GetTuples();
	auto node_data = node.GetData<T>();
	auto node_mask = node.GetValueMask();
	auto root_tuples = root.GetTuples();
	auto root_data = root.GetData<T>();

	ValidityAppender mask_appender(out_mask);
	idx_t node_idx = 0;
	idx_t node_value_idx = 0;
	idx_t root_idx = 0;
	idx_t result_count = 0;
	idx_t result_value_count = 0;
	auto copy_node_entry = [&]() {
		bool has_value = node_mask.RowIsValid(node_idx);
		if (has_value) {
			out_data[result_value_count++] = node_data[node_value_idx++];
		}
		out_tuples[result_count++] = node_tuples[node_idx++];
		mask_appender.Append(has_value);
	};
	for (idx_t i = 0; i < count; i++) {
		auto id = UnsafeNumericCast<sel_t>(UnsafeNumericCast<idx_t>(ids[sel.get_index(i)]) - vector_offset);
		while (node_idx < node.N && node_tuples[node_idx] < id) {
			copy_node_entry();
		}
		if (node_idx < node.N && node_tuples[node_idx] == id) {
			// this transaction updated the tuple before - the value from before that update is already saved
			copy_node_entry();
			continue;
		}
		while (root_idx < root.N && root_tuples[root_idx] < id) {
			root_idx++;
		}
		// tuples that are not in the root have no saved value - their value before the update is the base value
		bool in_root = root_idx < root.N && root_tuples[root_idx] == id;
		if (in_root) {
			out_data[result_value_count++] = root_data[root_idx];
		}
		out_tuples[result_count++] = id;
		mask_appender.Append(in_root);
	}
	while (node_idx < node.N) {
		copy_node_entry();
	}
	mask_appender.Finalize();
	return result_count;
}

//! Save the values from before the update - the target is either the source, or a larger update info replacing it
template <class T>
static void SaveUndoValues(UpdateInfo &source, UpdateInfo &root, row_t *ids, idx_t count, const SelectionVector &sel,
                           idx_t vector_offset, UpdateInfo &target) {
	idx_t result_count;
	if (&source != &target || source.N == 0) {
		// we are not reading from the target: write directly into it
		result_count = MergeUndoValues<T>(source, root, ids, count, sel, vector_offset, target.GetTuples(),
		                                  target.GetData<T>(), target.GetValueMask().GetData());
	} else {
		// merge into temporary arrays, and copy them back afterwards
		T result_values[STANDARD_VECTOR_SIZE];
		sel_t result_tuples[STANDARD_VECTOR_SIZE];
		validity_t result_mask[ValidityMask::STANDARD_ENTRY_COUNT];
		result_count =
		    MergeUndoValues<T>(source, root, ids, count, sel, vector_offset, result_tuples, result_values, result_mask);
		auto value_count = ValidityMask(result_mask, STANDARD_VECTOR_SIZE).CountValid(result_count);
		memcpy(target.GetTuples(), result_tuples, result_count * sizeof(sel_t));
		memcpy(target.GetData<T>(), result_values, value_count * sizeof(T));
		memcpy(target.GetValueMask().GetData(), result_mask, ValidityMask::ValidityMaskSize(result_count));
	}
	target.N = UnsafeNumericCast<sel_t>(result_count);
	D_ASSERT(target.N <= target.max);
}

static UpdateSegment::save_undo_function_t GetSaveUndoFunction(PhysicalType type) {
	switch (type) {
	case PhysicalType::BIT:
		return SaveUndoValues<bool>;
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		return SaveUndoValues<int8_t>;
	case PhysicalType::INT16:
		return SaveUndoValues<int16_t>;
	case PhysicalType::INT32:
		return SaveUndoValues<int32_t>;
	case PhysicalType::INT64:
		return SaveUndoValues<int64_t>;
	case PhysicalType::UINT8:
		return SaveUndoValues<uint8_t>;
	case PhysicalType::UINT16:
		return SaveUndoValues<uint16_t>;
	case PhysicalType::UINT32:
		return SaveUndoValues<uint32_t>;
	case PhysicalType::UINT64:
		return SaveUndoValues<uint64_t>;
	case PhysicalType::INT128:
		return SaveUndoValues<hugeint_t>;
	case PhysicalType::UINT128:
		return SaveUndoValues<uhugeint_t>;
	case PhysicalType::FLOAT:
		return SaveUndoValues<float>;
	case PhysicalType::DOUBLE:
		return SaveUndoValues<double>;
	case PhysicalType::INTERVAL:
		return SaveUndoValues<interval_t>;
	case PhysicalType::VARCHAR:
		return SaveUndoValues<string_t>;
	default:
		throw NotImplementedException("Unimplemented type for update segment");
	}
}

//===--------------------------------------------------------------------===//
// Update statistics
//===--------------------------------------------------------------------===//
unique_ptr<BaseStatistics> UpdateSegment::GetStatistics() {
	auto read_lock = lock.GetSharedLock();
	if (!root) {
		return nullptr;
	}
	lock_guard<mutex> stats_guard(stats_lock);
	return stats.statistics.ToUnique();
}

void UpdateSegment::MergeStatistics(const BaseStatistics &other) {
	lock_guard<mutex> stats_guard(stats_lock);
	stats.statistics.Merge(other);
}

void UpdateValidityStatistics(UpdateSegment *segment, SegmentStatistics &stats, UnifiedVectorFormat &update,
                              idx_t count) {
	auto &mask = update.validity;
	auto &validity = stats.statistics;
	if (mask.CanHaveNull() && !validity.CanHaveNull()) {
		for (idx_t i = 0; i < count; i++) {
			auto idx = update.sel->get_index(i);
			if (!mask.RowIsValid(idx)) {
				validity.SetHasNullFast();
				break;
			}
		}
	}
	if (!validity.CanHaveNoNull() && !mask.CheckAllInvalid(count)) {
		validity.SetHasNoNullFast();
	}
}

template <class T>
void TemplatedUpdateNumericStatistics(UpdateSegment *segment, SegmentStatistics &stats, UnifiedVectorFormat &update,
                                      idx_t count) {
	auto update_data = update.GetData<T>(update);
	auto &mask = update.validity;

	if (mask.CannotHaveNull()) {
		stats.statistics.SetHasNoNullFast();
		for (idx_t i = 0; i < count; i++) {
			auto idx = update.sel->get_index(i);
			stats.statistics.UpdateNumericStats<T>(update_data[idx]);
		}
		return;
	}
	for (idx_t i = 0; i < count; i++) {
		auto idx = update.sel->get_index(i);
		if (mask.RowIsValid(idx)) {
			stats.statistics.SetHasNoNullFast();
			stats.statistics.UpdateNumericStats<T>(update_data[idx]);
		} else {
			stats.statistics.SetHasNullFast();
		}
	}
}

void UpdateStringStatistics(UpdateSegment *segment, SegmentStatistics &stats, UnifiedVectorFormat &update,
                            idx_t count) {
	auto update_data = update.GetData<string_t>(update);
	auto &mask = update.validity;

	StatsWriter<string_t> stats_writer(stats.statistics.GetType());
	if (mask.CannotHaveNull()) {
		stats.statistics.SetHasNoNullFast();
		for (idx_t i = 0; i < count; i++) {
			auto idx = update.sel->get_index(i);
			auto &str = update_data[idx];
			stats_writer.Update(str);
		}
	} else {
		for (idx_t i = 0; i < count; i++) {
			auto idx = update.sel->get_index(i);
			if (mask.RowIsValid(idx)) {
				stats.statistics.SetHasNoNullFast();
				auto &str = update_data[idx];
				stats_writer.Update(str);
			} else {
				stats.statistics.SetHasNullFast();
			}
		}
	}
	stats_writer.Merge(stats.statistics);
}

UpdateSegment::statistics_update_function_t GetStatisticsUpdateFunction(PhysicalType type) {
	switch (type) {
	case PhysicalType::BIT:
		return UpdateValidityStatistics;
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		return TemplatedUpdateNumericStatistics<int8_t>;
	case PhysicalType::INT16:
		return TemplatedUpdateNumericStatistics<int16_t>;
	case PhysicalType::INT32:
		return TemplatedUpdateNumericStatistics<int32_t>;
	case PhysicalType::INT64:
		return TemplatedUpdateNumericStatistics<int64_t>;
	case PhysicalType::UINT8:
		return TemplatedUpdateNumericStatistics<uint8_t>;
	case PhysicalType::UINT16:
		return TemplatedUpdateNumericStatistics<uint16_t>;
	case PhysicalType::UINT32:
		return TemplatedUpdateNumericStatistics<uint32_t>;
	case PhysicalType::UINT64:
		return TemplatedUpdateNumericStatistics<uint64_t>;
	case PhysicalType::INT128:
		return TemplatedUpdateNumericStatistics<hugeint_t>;
	case PhysicalType::UINT128:
		return TemplatedUpdateNumericStatistics<uhugeint_t>;
	case PhysicalType::FLOAT:
		return TemplatedUpdateNumericStatistics<float>;
	case PhysicalType::DOUBLE:
		return TemplatedUpdateNumericStatistics<double>;
	case PhysicalType::INTERVAL:
		return TemplatedUpdateNumericStatistics<interval_t>;
	case PhysicalType::VARCHAR:
		return UpdateStringStatistics;
	default:
		throw NotImplementedException("Unimplemented type for uncompressed segment");
	}
}

//===--------------------------------------------------------------------===//
// Get Effective Updates
//===--------------------------------------------------------------------===//
idx_t GetEffectiveUpdatesValidity(UnifiedVectorFormat &update, row_t *ids, idx_t count, SelectionVector &sel,
                                  Vector &base_data, idx_t id_offset) {
	auto &original_validity = FlatVector::ValidityMutable(base_data);

	SelectionVector new_sel;
	new_sel.Initialize(STANDARD_VECTOR_SIZE);

	idx_t effective_count = 0;
	for (idx_t i = 0; i < count; i++) {
		auto sel_idx = sel.get_index(i);
		auto update_idx = update.sel->get_index(sel_idx);
		auto original_idx = UnsafeNumericCast<idx_t>(ids[sel_idx]) - id_offset;
		if (original_validity.RowIsValid(original_idx) == update.validity.RowIsValid(update_idx)) {
			continue;
		}
		new_sel.set_index(effective_count++, sel_idx);
	}
	sel.Initialize(new_sel);
	return effective_count;
}

template <class T>
idx_t TemplatedGetEffectiveUpdates(UnifiedVectorFormat &update, row_t *ids, idx_t count, SelectionVector &sel,
                                   Vector &base_data, idx_t id_offset) {
	auto data = UnifiedVectorFormat::GetData<T>(update);

	auto original_data = FlatVector::GetData<T>(base_data);
	auto &original_validity = FlatVector::ValidityMutable(base_data);

	SelectionVector new_sel;
	new_sel.Initialize(STANDARD_VECTOR_SIZE);

	idx_t effective_count = 0;
	for (idx_t i = 0; i < count; i++) {
		auto sel_idx = sel.get_index(i);
		auto update_idx = update.sel->get_index(sel_idx);
		auto original_idx = UnsafeNumericCast<idx_t>(ids[sel_idx]) - id_offset;
		if (!update.validity.RowIsValid(update_idx)) {
			// a row updated to NULL keeps its current value: a rewrite drops the values below NULLs
			if (!original_validity.RowIsValid(original_idx)) {
				// already NULL: there is no value to keep
				continue;
			}
		} else if (original_validity.RowIsValid(original_idx) && data[update_idx] == original_data[original_idx]) {
			// data is equivalent - skip
			continue;
		}
		new_sel.set_index(effective_count++, sel_idx);
	}
	sel.Initialize(new_sel);
	return effective_count;
}

UpdateSegment::get_effective_updates_t GetEffectiveUpdatesFunction(PhysicalType type) {
	switch (type) {
	case PhysicalType::BIT:
		return GetEffectiveUpdatesValidity;
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		return TemplatedGetEffectiveUpdates<int8_t>;
	case PhysicalType::INT16:
		return TemplatedGetEffectiveUpdates<int16_t>;
	case PhysicalType::INT32:
		return TemplatedGetEffectiveUpdates<int32_t>;
	case PhysicalType::INT64:
		return TemplatedGetEffectiveUpdates<int64_t>;
	case PhysicalType::UINT8:
		return TemplatedGetEffectiveUpdates<uint8_t>;
	case PhysicalType::UINT16:
		return TemplatedGetEffectiveUpdates<uint16_t>;
	case PhysicalType::UINT32:
		return TemplatedGetEffectiveUpdates<uint32_t>;
	case PhysicalType::UINT64:
		return TemplatedGetEffectiveUpdates<uint64_t>;
	case PhysicalType::INT128:
		return TemplatedGetEffectiveUpdates<hugeint_t>;
	case PhysicalType::UINT128:
		return TemplatedGetEffectiveUpdates<uhugeint_t>;
	case PhysicalType::FLOAT:
		return TemplatedGetEffectiveUpdates<float>;
	case PhysicalType::DOUBLE:
		return TemplatedGetEffectiveUpdates<double>;
	case PhysicalType::INTERVAL:
		return TemplatedGetEffectiveUpdates<interval_t>;
	case PhysicalType::VARCHAR:
		return TemplatedGetEffectiveUpdates<string_t>;
	default:
		throw NotImplementedException("Unimplemented type for uncompressed segment");
	}
}

//===--------------------------------------------------------------------===//
// Keep current values
//===--------------------------------------------------------------------===//
template <class T>
static T GetUpdateNullPadding() {
	return T();
}

template <>
string_t GetUpdateNullPadding() {
	return string_t(uint32_t(0));
}

//! A row updated to NULL keeps its current value: a rewrite drops the values below NULLs
template <class T>
static void TemplatedKeepCurrentValues(optional_ptr<UpdateInfo> root_info, Vector &update, row_t *ids, idx_t count,
                                       const SelectionVector &sel, Vector &base_data, idx_t vector_offset) {
	auto update_data = FlatVector::GetDataMutable<T>(update);
	auto &update_validity = FlatVector::Validity(update);
	auto base_values = FlatVector::GetData<T>(base_data);
	auto &base_validity = FlatVector::Validity(base_data);
	idx_t root_idx = 0;
	for (idx_t i = 0; i < count; i++) {
		auto idx = sel.get_index(i);
		if (update_validity.RowIsValid(idx)) {
			continue;
		}
		auto id = UnsafeNumericCast<sel_t>(UnsafeNumericCast<idx_t>(ids[idx]) - vector_offset);
		if (root_info) {
			auto root_tuples = root_info->GetTuples();
			while (root_idx < root_info->N && root_tuples[root_idx] < id) {
				root_idx++;
			}
			if (root_idx < root_info->N && root_tuples[root_idx] == id) {
				update_data[idx] = root_info->GetData<T>()[root_idx];
				continue;
			}
		}
		update_data[idx] = base_validity.RowIsValid(id) ? base_values[id] : GetUpdateNullPadding<T>();
	}
}

static void KeepCurrentValues(optional_ptr<UpdateInfo> root_info, Vector &update, row_t *ids, idx_t count,
                              const SelectionVector &sel, Vector &base_data, idx_t vector_offset) {
	switch (update.GetType().InternalType()) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		return TemplatedKeepCurrentValues<int8_t>(root_info, update, ids, count, sel, base_data, vector_offset);
	case PhysicalType::INT16:
		return TemplatedKeepCurrentValues<int16_t>(root_info, update, ids, count, sel, base_data, vector_offset);
	case PhysicalType::INT32:
		return TemplatedKeepCurrentValues<int32_t>(root_info, update, ids, count, sel, base_data, vector_offset);
	case PhysicalType::INT64:
		return TemplatedKeepCurrentValues<int64_t>(root_info, update, ids, count, sel, base_data, vector_offset);
	case PhysicalType::UINT8:
		return TemplatedKeepCurrentValues<uint8_t>(root_info, update, ids, count, sel, base_data, vector_offset);
	case PhysicalType::UINT16:
		return TemplatedKeepCurrentValues<uint16_t>(root_info, update, ids, count, sel, base_data, vector_offset);
	case PhysicalType::UINT32:
		return TemplatedKeepCurrentValues<uint32_t>(root_info, update, ids, count, sel, base_data, vector_offset);
	case PhysicalType::UINT64:
		return TemplatedKeepCurrentValues<uint64_t>(root_info, update, ids, count, sel, base_data, vector_offset);
	case PhysicalType::INT128:
		return TemplatedKeepCurrentValues<hugeint_t>(root_info, update, ids, count, sel, base_data, vector_offset);
	case PhysicalType::UINT128:
		return TemplatedKeepCurrentValues<uhugeint_t>(root_info, update, ids, count, sel, base_data, vector_offset);
	case PhysicalType::FLOAT:
		return TemplatedKeepCurrentValues<float>(root_info, update, ids, count, sel, base_data, vector_offset);
	case PhysicalType::DOUBLE:
		return TemplatedKeepCurrentValues<double>(root_info, update, ids, count, sel, base_data, vector_offset);
	case PhysicalType::INTERVAL:
		return TemplatedKeepCurrentValues<interval_t>(root_info, update, ids, count, sel, base_data, vector_offset);
	case PhysicalType::VARCHAR:
		return TemplatedKeepCurrentValues<string_t>(root_info, update, ids, count, sel, base_data, vector_offset);
	default:
		throw NotImplementedException("Unimplemented type for update segment");
	}
}

//===--------------------------------------------------------------------===//
// Update
//===--------------------------------------------------------------------===//
static idx_t SortSelectionVector(SelectionVector &sel, idx_t count, row_t *ids) {
	D_ASSERT(count > 0);

	bool is_sorted = true;
	for (idx_t i = 1; i < count; i++) {
		auto prev_idx = sel.get_index(i - 1);
		auto idx = sel.get_index(i);
		if (ids[idx] <= ids[prev_idx]) {
			is_sorted = false;
			break;
		}
	}
	if (is_sorted) {
		// already sorted: bailout
		return count;
	}
	// not sorted: need to sort the selection vector
	SelectionVector sorted_sel(count);
	for (idx_t i = 0; i < count; i++) {
		sorted_sel.set_index(i, sel.get_index(i));
	}
	std::sort(sorted_sel.data(), sorted_sel.data() + count, [&](sel_t l, sel_t r) { return ids[l] < ids[r]; });
	// eliminate any duplicates
	idx_t pos = 1;
	for (idx_t i = 1; i < count; i++) {
		auto prev_idx = sorted_sel.get_index(i - 1);
		auto idx = sorted_sel.get_index(i);
		D_ASSERT(ids[idx] >= ids[prev_idx]);
		if (ids[prev_idx] != ids[idx]) {
			sorted_sel.set_index(pos++, idx);
		}
	}
#ifdef DEBUG
	for (idx_t i = 1; i < pos; i++) {
		auto prev_idx = sorted_sel.get_index(i - 1);
		auto idx = sorted_sel.get_index(i);
		D_ASSERT(ids[idx] > ids[prev_idx]);
	}
#endif

	sel.Initialize(sorted_sel);
	D_ASSERT(pos > 0);
	return pos;
}

void UpdateSegment::InitializeUpdateInfo(idx_t vector_idx) {
	// create the versions for this segment, if there are none yet
	if (!root) {
		root = make_uniq<UpdateNode>(buffer_manager);
	}
	if (vector_idx < root->info.size()) {
		return;
	}
	root->info.reserve(vector_idx + 1);
	for (idx_t i = root->info.size(); i <= vector_idx; i++) {
		root->info.emplace_back();
	}
}

void UpdateSegment::ReallocateRootInfoIfNeeded(UpdateInfo &current_info, idx_t update_count, idx_t vector_index) {
	idx_t required_capacity = MinValue<idx_t>(idx_t(current_info.N) + update_count, STANDARD_VECTOR_SIZE);
	if (required_capacity <= idx_t(current_info.max)) {
		return;
	}
	idx_t new_capacity = UpdateInfo::GetCompactCapacity(required_capacity);
	idx_t new_alloc_size = UpdateInfo::GetAllocSize(type_size, new_capacity);
	auto new_handle = root->allocator.Allocate(new_alloc_size);
	auto &new_info = UpdateInfo::Get(new_handle);

	new_info.segment = current_info.segment;
	new_info.table = current_info.table;
	new_info.column_index = current_info.column_index;
	new_info.row_group_start = current_info.row_group_start;
	new_info.version_number.store(current_info.version_number.load());
	new_info.vector_index = current_info.vector_index;
	new_info.N = current_info.N;
	new_info.max = UnsafeNumericCast<sel_t>(new_capacity);
	new_info.prev = current_info.prev;
	new_info.next = current_info.next;
	new_info.value_mask_offset = 0;
	new_info.value_max = UnsafeNumericCast<sel_t>(new_capacity);

	memcpy(new_info.GetTuples(), current_info.GetTuples(), sizeof(sel_t) * current_info.N);
	memcpy(new_info.GetValues(), current_info.GetValues(), type_size * current_info.N);

	if (new_info.next.IsSet()) {
		auto next_pin = new_info.next.Pin();
		auto &next_info = UpdateInfo::Get(next_pin);
		next_info.prev = new_handle.GetBufferPointer();
	}

	root->info[vector_index] = new_handle.GetBufferPointer();
}

void UpdateSegment::Update(TransactionData transaction, DuckTableEntry &table_entry, idx_t column_index,
                           Vector &update_p, row_t *ids, idx_t count, Vector &base_data, idx_t row_group_start) {
	// obtain an exclusive lock
	auto write_lock = lock.GetExclusiveLock();

	UnifiedVectorFormat update_format;
	update_p.ToUnifiedFormat(update_format);

	// the statistics of this update join those of the segment only once it cannot fail anymore, as RowGroup::Update
	// publishes them after the next update of the column, which must not see the values of a failed update
	SegmentStatistics update_stats(type);
	statistics_update_function(this, update_stats, update_format, count);
	// rows updated to NULL take part too: they keep their current value
	SelectionVector sel;

	// subsequent algorithms used by the update require row ids to be (1) sorted, and (2) unique
	// this is usually the case for "standard" queries (e.g. UPDATE tbl SET x=bla WHERE cond)
	// however, for more exotic queries involving e.g. cross products/joins this might not be the case
	// hence we explicitly check here if the ids are sorted and, if not, sort + duplicate eliminate them
	count = SortSelectionVector(sel, count, ids);
	D_ASSERT(count > 0);

	// get the vector index based on the first id
	// we assert that all updates must be part of the same vector
	auto first_id = ids[sel.get_index(0)];
	idx_t vector_index = (UnsafeNumericCast<idx_t>(first_id) - row_group_start) / STANDARD_VECTOR_SIZE;
	idx_t vector_offset = row_group_start + vector_index * STANDARD_VECTOR_SIZE;

	if (!root || vector_index >= root->info.size() || !root->info[vector_index].IsSet()) {
		// get a list of effective updates - i.e. updates that actually change rows
		// if updates have the same value as the base row we can skip them
		// we only do that if we have no updates for this vector
		// we could do it otherwise - but that would require merging updates in order to find the current values
		count = get_effective_updates(update_format, ids, count, sel, base_data, vector_offset);
		if (count == 0) {
			MergeStatistics(update_stats.statistics);
			return;
		}
	}
	if (type.InternalType() != PhysicalType::BIT && !update_format.validity.CannotHaveNull()) {
		update_p.Flatten();
		optional_ptr<UpdateInfo> root_info;
		UndoBufferReference root_pin;
		if (root && vector_index < root->info.size() && root->info[vector_index].IsSet()) {
			root_pin = root->info[vector_index].Pin();
			root_info = &UpdateInfo::Get(root_pin);
		}
		KeepCurrentValues(root_info, update_p, ids, count, sel, base_data, vector_offset);
		update_p.ToUnifiedFormat(update_format);
	}
	if (statistics_update_function == UpdateStringStatistics) {
		// for strings - we need to push all strings we are going to place here into the string heap of the segment
		update_p.Flatten();
		auto update_data = FlatVector::GetDataMutable<string_t>(update_p);
		for (idx_t i = 0; i < count; i++) {
			auto idx = sel.get_index(i);
			update_data[idx] = heap->AddBlob(update_data[idx]);
		}
		update_p.ToUnifiedFormat(update_format);
	}

	InitializeUpdateInfo(vector_index);

	D_ASSERT(idx_t(first_id) >= row_group_start);

	if (!root->info[vector_index].IsSet()) {
		// there are no updates for this vector yet: create the root update info
		idx_t compact_capacity = UpdateInfo::GetCompactCapacity(count);
		auto handle = root->allocator.Allocate(UpdateInfo::GetAllocSize(type_size, compact_capacity));
		auto &root_info = UpdateInfo::Get(handle);
		UpdateInfo::Initialize(root_info, table_entry, MAX_COMMIT_ID, row_group_start, compact_capacity);
		root_info.segment = this;
		root_info.vector_index = vector_index;
		root_info.column_index = column_index;
		root->info[vector_index] = handle.GetBufferPointer();
	}

	// make room in the root before modifying anything
	{
		auto root_pin = root->info[vector_index].Pin();
		ReallocateRootInfoIfNeeded(UpdateInfo::Get(root_pin), count, vector_index);
	}
	auto root_pointer = root->info[vector_index];
	auto root_pin = root_pointer.Pin();
	auto &root_info = UpdateInfo::Get(root_pin);
	if (transaction.transaction) {
		// search for the update info that belongs to this transaction
		auto node_ref = GetTransactionUpdate(root_info.next, transaction);
		optional_ptr<UpdateInfo> node;
		if (node_ref.IsSet()) {
			node = &UpdateInfo::Get(node_ref);
			node->Verify();
		}
		// determine the tuples and values the update info of this transaction requires
		// we only count them with a merge walk if they cannot be derived or bounded cheaply
		bool save_required = true;
		bool fits = false;
		idx_t tuple_count = 0;
		idx_t value_count = 0;
		if (node) {
			if (node->N == STANDARD_VECTOR_SIZE) {
				// this transaction updated every tuple of the vector before - their values before it are saved
				save_required = false;
			} else {
				auto node_value_count = node->GetValueMask().CountValid(node->N);
				fits = node->N + count <= node->max && node_value_count + count <= node->value_max;
			}
		} else if (root_info.N == 0 || root_info.N == STANDARD_VECTOR_SIZE) {
			// either none or all of the tuples have a value in the root
			tuple_count = count;
			value_count = root_info.N == 0 ? 0 : count;
		} else {
			CountUndoValues(node, root_info, ids, count, sel, vector_offset, tuple_count, value_count);
		}
		if (save_required && node && !fits) {
			CountUndoValues(node, root_info, ids, count, sel, vector_offset, tuple_count, value_count);
			if (tuple_count == node->N) {
				// this transaction updated all of these tuples before - their values before it are saved
				save_required = false;
			}
			fits = tuple_count <= node->max && value_count <= node->value_max;
		}
		if (!save_required) {
			// the update info of this transaction does not change
		} else if (fits) {
			// the values from before the update fit into the update info of this transaction
			save_undo_function(*node, root_info, ids, count, sel, vector_offset, *node);
			node->Verify();
		} else {
			// create a new update info that fits all values from before the updates of this transaction
			idx_t capacity = UpdateInfo::GetCompactCapacity(tuple_count);
			idx_t value_capacity = value_count == 0 ? 0 : UpdateInfo::GetCompactCapacity(value_count);
			if (node) {
				capacity = MaxValue<idx_t>(capacity, node->max);
				value_capacity = MaxValue<idx_t>(value_capacity, node->value_max);
			}
			auto new_ref = transaction.transaction->CreateUpdateInfo(table_entry, type_size, capacity, value_capacity,
			                                                         row_group_start);
			auto &new_node = UpdateInfo::Get(new_ref);
			new_node.segment = this;
			new_node.vector_index = vector_index;
			new_node.column_index = column_index;
			save_undo_function(node ? *node : new_node, root_info, ids, count, sel, vector_offset, new_node);
			new_node.Verify();

			// insert the new update info into the chain - replacing the old one (if any)
			auto new_pointer = new_ref.GetBufferPointer();
			new_node.prev = node ? node->prev : root_pointer;
			new_node.next = node ? node->next : root_info.next;
			// pin both neighbours before modifying either: Pin can throw (OOM) and must not leave a half-linked node
			auto prev_pin = new_node.prev.Pin();
			UndoBufferReference next_pin;
			if (new_node.next.IsSet()) {
				next_pin = new_node.next.Pin();
			}
			UpdateInfo::Get(prev_pin).next = new_pointer;
			if (next_pin.IsSet()) {
				UpdateInfo::Get(next_pin).prev = new_pointer;
			}
			if (node) {
				// the old update info stays in the undo buffer, but no longer contains any updates
				node->N = 0;
				node->prev = UndoBufferPointer();
				node->next = UndoBufferPointer();
			}
		}
	}
	// write the new values into the root
	merge_update_function(root_info, update_format, ids, count, sel, vector_offset);
	root_info.Verify();
	MergeStatistics(update_stats.statistics);
}

bool UpdateSegment::HasUpdates() const {
	auto read_lock = lock.GetSharedLock();
	return root.get() != nullptr;
}

bool UpdateSegment::HasUpdates(idx_t vector_index) const {
	auto read_lock = lock.GetSharedLock();
	return GetUpdateNode(*read_lock, vector_index).IsSet();
}

bool UpdateSegment::HasUncommittedUpdates(idx_t vector_index) {
	auto read_lock = lock.GetSharedLock();
	auto entry = GetUpdateNode(*read_lock, vector_index);
	if (!entry.IsSet()) {
		return false;
	}
	auto pin = entry.Pin();
	auto &info = UpdateInfo::Get(pin);
	if (info.HasNext()) {
		return true;
	}
	return false;
}

bool UpdateSegment::HasUpdates(idx_t start_row_index, idx_t end_row_index) {
	auto read_lock = lock.GetSharedLock();
	if (!root) {
		return false;
	}
	idx_t base_vector_index = start_row_index / STANDARD_VECTOR_SIZE;
	idx_t end_vector_index = end_row_index / STANDARD_VECTOR_SIZE;
	for (idx_t i = base_vector_index; i <= end_vector_index; i++) {
		auto entry = GetUpdateNode(*read_lock, i);
		if (entry.IsSet()) {
			return true;
		}
	}
	return false;
}

//===--------------------------------------------------------------------===//
// Store base values
//===--------------------------------------------------------------------===//
//! Copies the update info, with the base value for every tuple that has no stored value
template <class T, class GET_BASE>
static void CopyWithBaseValues(UpdateInfo &source, UpdateInfo &target, GET_BASE &&get_base) {
	auto source_tuples = source.GetTuples();
	auto source_data = source.GetData<T>();
	auto source_mask = source.GetValueMask();
	auto target_tuples = target.GetTuples();
	auto target_data = target.GetData<T>();
	ValidityAppender mask_appender(target.GetValueMask().GetData());
	idx_t value_idx = 0;
	for (idx_t i = 0; i < source.N; i++) {
		target_tuples[i] = source_tuples[i];
		target_data[i] = source_mask.RowIsValid(i) ? source_data[value_idx++] : get_base(source_tuples[i]);
		mask_appender.Append(true);
	}
	mask_appender.Finalize();
	target.N = source.N;
}

template <class T>
static T CopyBaseValue(StringHeap &heap, T value) {
	return value;
}

template <>
string_t CopyBaseValue(StringHeap &heap, string_t value) {
	// the string lives in the blocks of the column that the rewrite replaces
	return heap.AddBlob(value);
}

static void StoreBaseValidity(UpdateInfo &source, Vector &base_data, StringHeap &heap, UpdateInfo &target) {
	auto &base_validity = FlatVector::Validity(base_data);
	CopyWithBaseValues<bool>(source, target, [&](sel_t tuple) { return base_validity.RowIsValid(tuple); });
}

template <class T>
static void TemplatedStoreBaseValues(UpdateInfo &source, Vector &base_data, StringHeap &heap, UpdateInfo &target) {
	auto base_values = FlatVector::GetData<T>(base_data);
	auto &base_validity = FlatVector::Validity(base_data);
	CopyWithBaseValues<T>(source, target, [&](sel_t tuple) {
		return base_validity.RowIsValid(tuple) ? CopyBaseValue<T>(heap, base_values[tuple]) : GetUpdateNullPadding<T>();
	});
}

static UpdateSegment::store_base_values_function_t GetStoreBaseValuesFunction(PhysicalType type) {
	switch (type) {
	case PhysicalType::BIT:
		return StoreBaseValidity;
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		return TemplatedStoreBaseValues<int8_t>;
	case PhysicalType::INT16:
		return TemplatedStoreBaseValues<int16_t>;
	case PhysicalType::INT32:
		return TemplatedStoreBaseValues<int32_t>;
	case PhysicalType::INT64:
		return TemplatedStoreBaseValues<int64_t>;
	case PhysicalType::UINT8:
		return TemplatedStoreBaseValues<uint8_t>;
	case PhysicalType::UINT16:
		return TemplatedStoreBaseValues<uint16_t>;
	case PhysicalType::UINT32:
		return TemplatedStoreBaseValues<uint32_t>;
	case PhysicalType::UINT64:
		return TemplatedStoreBaseValues<uint64_t>;
	case PhysicalType::INT128:
		return TemplatedStoreBaseValues<hugeint_t>;
	case PhysicalType::UINT128:
		return TemplatedStoreBaseValues<uhugeint_t>;
	case PhysicalType::FLOAT:
		return TemplatedStoreBaseValues<float>;
	case PhysicalType::DOUBLE:
		return TemplatedStoreBaseValues<double>;
	case PhysicalType::INTERVAL:
		return TemplatedStoreBaseValues<interval_t>;
	case PhysicalType::VARCHAR:
		return TemplatedStoreBaseValues<string_t>;
	default:
		throw NotImplementedException("Unimplemented type for update segment");
	}
}

//! Whether a committed update takes old values from base data that a rewrite as of the bound replaces
static bool ReadsReplacedBase(UpdateInfo &info, VisibilityBound visibility_bound) {
	return info.version_number.load() < visibility_bound && !info.GetValueMask().CheckAllValid(info.N);
}

vector<idx_t> UpdateSegment::GetVectorsReadingBase(VisibilityBound visibility_bound) {
	vector<idx_t> result;
	auto read_lock = lock.GetSharedLock();
	if (!root) {
		return result;
	}
	for (idx_t vector_idx = 0; vector_idx < root->info.size(); vector_idx++) {
		auto entry = GetUpdateNode(*read_lock, vector_idx);
		if (!entry.IsSet()) {
			continue;
		}
		auto root_pin = entry.Pin();
		auto next = UpdateInfo::Get(root_pin).next;
		while (next.IsSet()) {
			auto pin = next.Pin();
			auto &info = UpdateInfo::Get(pin);
			if (ReadsReplacedBase(info, visibility_bound)) {
				result.push_back(vector_idx);
				break;
			}
			next = info.next;
		}
	}
	return result;
}

void UpdateSegment::StoreBaseValues(VisibilityBound visibility_bound, idx_t vector_index, Vector &base_data) {
	auto write_lock = lock.GetExclusiveLock();
	auto entry = GetUpdateNode(*write_lock, vector_index);
	if (!entry.IsSet()) {
		return;
	}
	auto root_pin = entry.Pin();
	auto next = UpdateInfo::Get(root_pin).next;
	while (next.IsSet()) {
		auto pin = next.Pin();
		auto &info = UpdateInfo::Get(pin);
		next = info.next;
		if (!ReadsReplacedBase(info, visibility_bound)) {
			continue;
		}
		// the copy lives in the segment: the undo buffer of the transaction only has room for the original
		auto handle = root->allocator.Allocate(UpdateInfo::GetAllocSize(type_size, info.N, info.N));
		auto &copy = UpdateInfo::Get(handle);
		UpdateInfo::Initialize(copy, *info.table, info.version_number.load(), info.row_group_start, info.N, info.N);
		copy.segment = this;
		copy.vector_index = info.vector_index;
		copy.column_index = info.column_index;
		store_base_values_function(info, base_data, *heap, copy);
		copy.Verify();

		// replace the original in the chain - pin both neighbours before modifying either, as Pin can throw
		auto copy_pointer = handle.GetBufferPointer();
		copy.prev = info.prev;
		copy.next = info.next;
		auto prev_pin = copy.prev.Pin();
		UndoBufferReference next_pin;
		if (copy.next.IsSet()) {
			next_pin = copy.next.Pin();
		}
		UpdateInfo::Get(prev_pin).next = copy_pointer;
		if (next_pin.IsSet()) {
			UpdateInfo::Get(next_pin).prev = copy_pointer;
		}
		// the cleanup of the transaction follows the original to the copy
		info.N = 0;
		info.prev = UndoBufferPointer();
		info.next = copy_pointer;
	}
}

//===--------------------------------------------------------------------===//
// Checkpoint interaction
//===--------------------------------------------------------------------===//
bool UpdateSegment::HasUnserializedChanges() const {
	return newest_uncheckpointed_update_commit.load() != 0;
}

void UpdateSegment::MarkCommitted(transaction_t commit_id) {
	// commits are serialized under the transaction lock, so commit ids only grow
	newest_uncheckpointed_update_commit = commit_id;
}

void UpdateSegment::MarkCheckpointed(VisibilityBound visibility_bound) {
	auto current = newest_uncheckpointed_update_commit.load();
	if (current != 0 && current < visibility_bound) {
		// a commit at or above the bound that races with this call keeps its id
		newest_uncheckpointed_update_commit.compare_exchange_strong(current, 0);
	}
}

void ColumnUpdates::ClearIfLastHolder(const unique_lock<mutex> &guard, idx_t holders) {
	D_ASSERT(guard.owns_lock() && guard.mutex() == &lock);
	if (holders != 1 || !newest_column || !segment) {
		return;
	}
	segment->ClearIfUnused();
}

bool UpdateSegment::IsUnused(StorageLockKey &lock_key) const {
	if (HasUnserializedChanges()) {
		return false;
	}
	if (!root) {
		return true;
	}
	for (idx_t vector_idx = 0; vector_idx < root->info.size(); vector_idx++) {
		auto entry = GetUpdateNode(lock_key, vector_idx);
		if (!entry.IsSet()) {
			continue;
		}
		auto pin = entry.Pin();
		if (UpdateInfo::Get(pin).HasNext()) {
			return false;
		}
	}
	return true;
}

bool UpdateSegment::CanBeCleared() const {
	auto read_lock = lock.GetSharedLock();
	return IsUnused(*read_lock);
}

void UpdateSegment::ClearIfUnused() {
	// checked under the same lock that the clear holds, so no update can slip in between
	auto write_lock = lock.GetExclusiveLock();
	if (!IsUnused(*write_lock)) {
		return;
	}
	root.reset();
	// results that scans filled from the old heap keep it alive for as long as they need it
	heap = make_shared_ptr<StringHeap>(buffer_manager.GetBufferAllocator());
	lock_guard<mutex> stats_guard(stats_lock);
	stats.statistics = BaseStatistics::CreateEmpty(type);
}

} // namespace duckdb
