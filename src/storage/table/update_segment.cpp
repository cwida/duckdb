#include "duckdb/storage/table/update_segment.hpp"

#include "duckdb/common/exception/transaction_exception.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/storage/table/column_data.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/update_info.hpp"
#include "duckdb/storage/statistics/stats_writer.hpp"

#include <algorithm>

namespace duckdb {

static UpdateSegment::initialize_update_function_t GetInitializeUpdateFunction(PhysicalType type);
static UpdateSegment::fetch_update_function_t GetFetchUpdateFunction(PhysicalType type);
static UpdateSegment::fetch_committed_function_t GetFetchCommittedFunction(PhysicalType type);
static UpdateSegment::fetch_committed_range_function_t GetFetchCommittedRangeFunction(PhysicalType type);

static UpdateSegment::merge_update_function_t GetMergeUpdateFunction(PhysicalType type);
static UpdateSegment::rollback_update_function_t GetRollbackUpdateFunction(PhysicalType type);
static UpdateSegment::statistics_update_function_t GetStatisticsUpdateFunction(PhysicalType type);
static UpdateSegment::fetch_rows_function_t GetFetchRowsFunction(PhysicalType type);
static UpdateSegment::get_effective_updates_t GetEffectiveUpdatesFunction(PhysicalType type);

UpdateSegment::UpdateSegment(ColumnData &column_data)
    : column_data(column_data), stats(column_data.type), heap(BufferAllocator::Get(column_data.GetDatabase())) {
	auto physical_type = column_data.type.InternalType();

	this->type_size = GetTypeIdSize(physical_type);

	this->initialize_update_function = GetInitializeUpdateFunction(physical_type);
	this->fetch_update_function = GetFetchUpdateFunction(physical_type);
	this->fetch_committed_function = GetFetchCommittedFunction(physical_type);
	this->fetch_committed_range = GetFetchCommittedRangeFunction(physical_type);
	this->fetch_rows_function = GetFetchRowsFunction(physical_type);
	this->merge_update_function = GetMergeUpdateFunction(physical_type);
	this->rollback_update_function = GetRollbackUpdateFunction(physical_type);
	this->statistics_update_function = GetStatisticsUpdateFunction(physical_type);
	this->get_effective_updates = GetEffectiveUpdatesFunction(physical_type);
}

UpdateSegment::~UpdateSegment() {
}

//===--------------------------------------------------------------------===//
// Update Info Helpers
//===--------------------------------------------------------------------===//
Value UpdateInfo::GetValue(idx_t index) {
	auto &type = segment->column_data.type;

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
	auto &type = segment->column_data.type;
	string result = "Update Info [" + type.ToString() + ", Count: " + to_string(N) +
	                ", Transaction Id: " + to_string(version_number.load()) + "]\n";
	auto tuples = GetTuples();
	for (idx_t i = 0; i < N; i++) {
		result += to_string(tuples[i]) + ": " + GetValue(i).ToString() + "\n";
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

data_ptr_t UpdateInfo::GetValues() {
	return reinterpret_cast<data_ptr_t>(data_ptr_cast(this) + sizeof(UpdateInfo) + sizeof(sel_t) * max);
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

idx_t UpdateInfo::GetAllocSize(idx_t type_size) {
	return AlignValue<idx_t>(sizeof(UpdateInfo) + (sizeof(sel_t) + type_size) * STANDARD_VECTOR_SIZE);
}

idx_t UpdateInfo::GetAllocSize(idx_t type_size, idx_t capacity) {
	return AlignValue<idx_t>(sizeof(UpdateInfo) + (sizeof(sel_t) + type_size) * capacity);
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
}

void UpdateInfo::Initialize(UpdateInfo &info, DuckTableEntry &table_entry, transaction_t transaction_id,
                            idx_t row_group_start) {
	info.max = STANDARD_VECTOR_SIZE;
	info.row_group_start = row_group_start;
	info.version_number = transaction_id;
	info.table = &table_entry;
	info.segment = nullptr;
	info.prev.entry = nullptr;
	info.next.entry = nullptr;
}

void UpdateInfo::Verify() {
#ifdef DEBUG
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

static void UpdateMergeValidity(const SnapshotView &view, UpdateInfo &info, Vector &result) {
	auto &result_mask = FlatVector::ValidityMutable(result);
	UpdateInfo::UpdatesForTransaction(info, view,
	                                  [&](UpdateInfo &current) { MergeValidityInfo(current, result_mask); });
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
static void UpdateMergeFetch(const SnapshotView &view, UpdateInfo &info, Vector &result) {
	auto result_data = FlatVector::GetDataMutable<T>(result);
	UpdateInfo::UpdatesForTransaction(info, view,
	                                  [&](UpdateInfo &current) { MergeUpdateInfo<T>(current, result_data); });
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
}

UpdateNode::UpdateNode(BufferManager &manager) : allocator(manager) {
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

static void FetchCommittedRangeValidity(UpdateInfo &info, idx_t start, idx_t end, idx_t result_offset, Vector &result) {
	auto &result_mask = FlatVector::ValidityMutable(result);
	MergeUpdateInfoRangeValidity(info, start, end, result_offset, result_mask);
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
static void TemplatedFetchCommittedRange(UpdateInfo &info, idx_t start, idx_t end, idx_t result_offset,
                                         Vector &result) {
	auto result_data = FlatVector::GetDataMutable<T>(result);
	MergeUpdateInfoRange<T>(info, start, end, result_offset, result_data);
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

void UpdateSegment::FetchCommittedRange(idx_t start_row, idx_t count, Vector &result) {
	D_ASSERT(count > 0);
	if (!root) {
		return;
	}
	D_ASSERT(start_row <= column_data.count);
	D_ASSERT(start_row + count <= column_data.count);
	D_ASSERT(result.GetVectorType() == VectorType::FLAT_VECTOR);

	idx_t end_row = start_row + count;
	idx_t start_vector = start_row / STANDARD_VECTOR_SIZE;
	idx_t end_vector = (end_row - 1) / STANDARD_VECTOR_SIZE;
	D_ASSERT(start_vector <= end_vector);

	auto lock_handle = lock.GetSharedLock();
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
		fetch_committed_range(UpdateInfo::Get(pin), start_in_vector, end_in_vector, result_offset, result);
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

static void FetchRowsValidity(const SnapshotView &view, UpdateInfo &info, const idx_t *offsets,
                              const SelectionVector &sel, idx_t fetch_offset, idx_t count, idx_t vector_offset,
                              Vector &result, idx_t result_offset) {
	auto &result_mask = FlatVector::ValidityMutable(result);
	UpdateInfo::UpdatesForTransaction(info, view, [&](UpdateInfo &current) {
		auto info_data = current.GetData<bool>();
		for (idx_t idx = 0; idx < count; idx++) {
			const idx_t row_idx = offsets[sel.get_index(fetch_offset + idx)] - vector_offset;
			idx_t update_idx;
			if (FindUpdatedTuple(current, row_idx, update_idx)) {
				result_mask.Set(result_offset + fetch_offset + idx, info_data[update_idx]);
			}
		}
	});
}

template <class T>
static void TemplatedFetchRows(const SnapshotView &view, UpdateInfo &info, const idx_t *offsets,
                               const SelectionVector &sel, idx_t fetch_offset, idx_t count, idx_t vector_offset,
                               Vector &result, idx_t result_offset) {
	auto result_data = FlatVector::GetDataMutable<T>(result);
	UpdateInfo::UpdatesForTransaction(info, view, [&](UpdateInfo &current) {
		auto info_data = current.GetData<T>();
		for (idx_t idx = 0; idx < count; idx++) {
			const idx_t row_idx = offsets[sel.get_index(fetch_offset + idx)] - vector_offset;
			idx_t update_idx;
			if (FindUpdatedTuple(current, row_idx, update_idx)) {
				result_data[result_offset + fetch_offset + idx] = info_data[update_idx];
			}
		}
	});
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
	if (fetch_count == 0 || !root) {
		return;
	}

	auto lock_handle = lock.GetSharedLock();
	for (idx_t idx = 0; idx < fetch_count;) {
		const idx_t offset = offsets[sel.get_index(idx)];
		if (offset > column_data.count) {
			throw InternalException("UpdateSegment::FetchRows out of range");
		}
		const idx_t vector_index = offset / STANDARD_VECTOR_SIZE;
		const idx_t vector_offset = vector_index * STANDARD_VECTOR_SIZE;
		idx_t vector_count = 1;
		while (idx + vector_count < fetch_count) {
			const idx_t next_offset = offsets[sel.get_index(idx + vector_count)];
			if (next_offset > column_data.count) {
				throw InternalException("UpdateSegment::FetchRows out of range");
			}
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
		}
		idx += vector_count;
	}
}

//===--------------------------------------------------------------------===//
// Rollback update
//===--------------------------------------------------------------------===//
template <class T>
static void RollbackUpdate(UpdateInfo &base_info, UpdateInfo &rollback_info) {
	auto base_data = base_info.GetData<T>();
	auto base_tuples = base_info.GetTuples();
	auto rollback_data = rollback_info.GetData<T>();
	auto rollback_tuples = rollback_info.GetTuples();
	idx_t base_offset = 0;
	for (idx_t i = 0; i < rollback_info.N; i++) {
		auto id = rollback_tuples[i];
		while (base_tuples[base_offset] < id) {
			base_offset++;
			D_ASSERT(base_offset < base_info.N);
		}
		base_data[base_offset] = rollback_data[i];
	}
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

	if (!info.HasPrev()) {
		// never linked (the update failed): data may be partial and the vector root may belong to another update
		return;
	}
	// move the data from the UpdateInfo back into the base info
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
	CleanupUpdateInternal(*lock_handle, info);
}

//===--------------------------------------------------------------------===//
// Check for conflicts in update
//===--------------------------------------------------------------------===//
static void CheckForConflicts(UndoBufferPointer next_ptr, TransactionData transaction, row_t *ids,
                              const SelectionVector &sel, idx_t count, row_t offset, UndoBufferReference &node_ref) {
	while (next_ptr.IsSet()) {
		auto pin = next_ptr.Pin();
		auto &info = UpdateInfo::Get(pin);
		if (info.version_number == transaction.GetTransactionId()) {
			// this UpdateInfo belongs to the current transaction, set it in the node
			node_ref = std::move(pin);
		} else if (info.version_number.load() >= transaction.view.visibility_bound) {
			// potential conflict, check that tuple ids do not conflict
			// as both ids and info->tuples are sorted, this is similar to a merge join
			idx_t i = 0, j = 0;
			auto tuples = info.GetTuples();
			while (true) {
				auto id = ids[sel.get_index(i)] - offset;
				if (id == tuples[j]) {
					throw TransactionException("Conflict on update!");
				} else if (id < tuples[j]) {
					// id < the current tuple in info, move to next id
					i++;
					if (i == count) {
						break;
					}
				} else {
					// id > the current tuple, move to next tuple in info
					j++;
					if (j == info.N) {
						break;
					}
				}
			}
		}
		next_ptr = info.next;
	}
}

//===--------------------------------------------------------------------===//
// Initialize update info
//===--------------------------------------------------------------------===//
void UpdateSegment::InitializeUpdateInfo(UpdateInfo &info, row_t *ids, const SelectionVector &sel, idx_t count,
                                         idx_t vector_index, idx_t vector_offset) {
	info.segment = this;
	info.vector_index = vector_index;
	info.prev = UndoBufferPointer();
	info.next = UndoBufferPointer();

	// set up the tuple ids
	info.N = UnsafeNumericCast<sel_t>(count);
	auto tuples = info.GetTuples();
	for (idx_t i = 0; i < count; i++) {
		auto idx = sel.get_index(i);
		auto id = ids[idx];
		D_ASSERT(idx_t(id) >= vector_offset && idx_t(id) < vector_offset + STANDARD_VECTOR_SIZE);
		tuples[i] = NumericCast<sel_t>(NumericCast<idx_t>(id) - vector_offset);
	};
}

static void InitializeUpdateValidity(UpdateInfo &base_info, Vector &base_data, UpdateInfo &update_info,
                                     UnifiedVectorFormat &update, const SelectionVector &sel) {
	auto &update_mask = update.validity;
	auto tuple_data = update_info.GetData<bool>();

	if (update_mask.CanHaveNull()) {
		for (idx_t i = 0; i < update_info.N; i++) {
			auto idx = update.sel->get_index(sel.get_index(i));
			tuple_data[i] = update_mask.RowIsValidUnsafe(idx);
		}
	} else {
		for (idx_t i = 0; i < update_info.N; i++) {
			tuple_data[i] = true;
		}
	}

	auto &base_mask = FlatVector::ValidityMutable(base_data);
	auto base_tuple_data = base_info.GetData<bool>();
	auto base_tuples = base_info.GetTuples();
	if (base_mask.CanHaveNull()) {
		for (idx_t i = 0; i < base_info.N; i++) {
			base_tuple_data[i] = base_mask.RowIsValidUnsafe(base_tuples[i]);
		}
	} else {
		for (idx_t i = 0; i < base_info.N; i++) {
			base_tuple_data[i] = true;
		}
	}
}

struct UpdateSelectElement {
	template <class T>
	static T Operation(UpdateSegment &segment, T element) {
		return element;
	}
};

template <>
string_t UpdateSelectElement::Operation(UpdateSegment &segment, string_t element) {
	return segment.GetStringHeap().AddBlob(element);
}

template <class T>
static T GetUpdateNullPadding() {
	return T();
}

template <>
string_t GetUpdateNullPadding() {
	return string_t(uint32_t(0));
}

template <class T>
static void InitializeUpdateData(UpdateInfo &base_info, Vector &base_data, UpdateInfo &update_info,
                                 UnifiedVectorFormat &update, const SelectionVector &sel) {
	auto update_data = update.GetData<T>(update);
	auto tuple_data = update_info.GetData<T>();

	for (idx_t i = 0; i < update_info.N; i++) {
		auto idx = update.sel->get_index(sel.get_index(i));
		tuple_data[i] = update_data[idx];
	}

	auto base_array_data = FlatVector::GetData<T>(base_data);
	auto &base_validity = FlatVector::ValidityMutable(base_data);
	auto base_tuple_data = base_info.GetData<T>();
	auto base_tuples = base_info.GetTuples();
	for (idx_t i = 0; i < base_info.N; i++) {
		auto base_idx = base_tuples[i];
		if (!base_validity.RowIsValid(base_idx)) {
			base_tuple_data[i] = GetUpdateNullPadding<T>();
			continue;
		}
		base_tuple_data[i] = UpdateSelectElement::Operation<T>(*base_info.segment, base_array_data[base_idx]);
	}
}

static UpdateSegment::initialize_update_function_t GetInitializeUpdateFunction(PhysicalType type) {
	switch (type) {
	case PhysicalType::BIT:
		return InitializeUpdateValidity;
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		return InitializeUpdateData<int8_t>;
	case PhysicalType::INT16:
		return InitializeUpdateData<int16_t>;
	case PhysicalType::INT32:
		return InitializeUpdateData<int32_t>;
	case PhysicalType::INT64:
		return InitializeUpdateData<int64_t>;
	case PhysicalType::UINT8:
		return InitializeUpdateData<uint8_t>;
	case PhysicalType::UINT16:
		return InitializeUpdateData<uint16_t>;
	case PhysicalType::UINT32:
		return InitializeUpdateData<uint32_t>;
	case PhysicalType::UINT64:
		return InitializeUpdateData<uint64_t>;
	case PhysicalType::INT128:
		return InitializeUpdateData<hugeint_t>;
	case PhysicalType::UINT128:
		return InitializeUpdateData<uhugeint_t>;
	case PhysicalType::FLOAT:
		return InitializeUpdateData<float>;
	case PhysicalType::DOUBLE:
		return InitializeUpdateData<double>;
	case PhysicalType::INTERVAL:
		return InitializeUpdateData<interval_t>;
	case PhysicalType::VARCHAR:
		return InitializeUpdateData<string_t>;
	default:
		throw NotImplementedException("Unimplemented type for update segment");
	}
}

//===--------------------------------------------------------------------===//
// Merge update info
//===--------------------------------------------------------------------===//
template <class F1, class F2, class F3>
static idx_t MergeLoop(row_t a[], sel_t b[], idx_t acount, idx_t bcount, idx_t aoffset, F1 merge, F2 pick_a, F3 pick_b,
                       const SelectionVector &asel) {
	idx_t aidx = 0, bidx = 0;
	idx_t count = 0;
	while (aidx < acount && bidx < bcount) {
		auto a_index = asel.get_index(aidx);
		auto a_id = UnsafeNumericCast<idx_t>(a[a_index]) - aoffset;
		auto b_id = b[bidx];
		if (a_id == b_id) {
			merge(a_id, a_index, bidx, count);
			aidx++;
			bidx++;
			count++;
		} else if (a_id < b_id) {
			pick_a(a_id, a_index, count);
			aidx++;
			count++;
		} else {
			pick_b(b_id, bidx, count);
			bidx++;
			count++;
		}
	}
	for (; aidx < acount; aidx++) {
		auto a_index = asel.get_index(aidx);
		pick_a(UnsafeNumericCast<idx_t>(a[a_index]) - aoffset, a_index, count);
		count++;
	}
	for (; bidx < bcount; bidx++) {
		pick_b(b[bidx], bidx, count);
		count++;
	}
	return count;
}

struct ExtractStandardEntry {
	template <class T, class V>
	static T Extract(const V *data, idx_t entry) {
		return data[entry];
	}
};

struct ExtractValidityEntry {
	template <class T, class V>
	static T Extract(const V *data, idx_t entry) {
		return data->RowIsValid(entry);
	}
};

template <class T, class V, class OP = ExtractStandardEntry>
static void MergeUpdateLoopInternal(UpdateInfo &base_info, V *base_table_data, UpdateInfo &update_info,
                                    const SelectionVector &update_vector_sel, const V *update_vector_data, row_t *ids,
                                    idx_t count, const SelectionVector &sel, idx_t row_group_start,
                                    const ValidityMask *base_table_validity = nullptr) {
	auto base_id = row_group_start + base_info.vector_index * STANDARD_VECTOR_SIZE;
#ifdef DEBUG
	// all of these should be sorted, otherwise the below algorithm does not work
	for (idx_t i = 1; i < count; i++) {
		auto prev_idx = sel.get_index(i - 1);
		auto idx = sel.get_index(i);
		D_ASSERT(ids[idx] > ids[prev_idx] && ids[idx] >= row_t(base_id) &&
		         ids[idx] < row_t(base_id + STANDARD_VECTOR_SIZE));
	}
#endif

	// we have a new batch of updates (update, ids, count)
	// we already have existing updates (base_info)
	// and potentially, this transaction already has updates present (update_info)
	// we need to merge these all together so that the latest updates get merged into base_info
	// and the "old" values (fetched from EITHER base_info OR from base_data) get placed into update_info
	auto base_info_data = base_info.GetData<T>();
	auto base_tuples = base_info.GetTuples();
	auto update_info_data = update_info.GetData<T>();
	auto update_tuples = update_info.GetTuples();

	// we first do the merging of the old values
	// what we are trying to do here is update the "update_info" of this transaction with all the old data we require
	// this means we need to merge (1) any previously updated values (stored in update_info->tuples)
	// together with (2)
	// to simplify this, we create new arrays here
	// we memcpy these over afterwards
	T result_values[STANDARD_VECTOR_SIZE];
	sel_t result_ids[STANDARD_VECTOR_SIZE];

	idx_t base_info_offset = 0;
	idx_t update_info_offset = 0;
	idx_t result_offset = 0;
	for (idx_t i = 0; i < count; i++) {
		auto idx = sel.get_index(i);
		// we have to merge the info for "ids[i]"
		auto update_id = UnsafeNumericCast<idx_t>(ids[idx]) - base_id;

		while (update_info_offset < update_info.N && update_tuples[update_info_offset] < update_id) {
			// old id comes before the current id: write it
			result_values[result_offset] = update_info_data[update_info_offset];
			result_ids[result_offset++] = update_tuples[update_info_offset];
			update_info_offset++;
		}
		// write the new id
		if (update_info_offset < update_info.N && update_tuples[update_info_offset] == update_id) {
			// we have an id that is equivalent in the current update info: write the update info
			result_values[result_offset] = update_info_data[update_info_offset];
			result_ids[result_offset++] = update_tuples[update_info_offset];
			update_info_offset++;
			continue;
		}

		/// now check if we have the current update_id in the base_info, or if we should fetch it from the base data
		while (base_info_offset < base_info.N && base_tuples[base_info_offset] < update_id) {
			base_info_offset++;
		}
		if (base_info_offset < base_info.N && base_tuples[base_info_offset] == update_id) {
			// it is! we have to move the tuple from base_info->ids[base_info_offset] to update_info
			result_values[result_offset] = base_info_data[base_info_offset];
		} else if (base_table_validity && !base_table_validity->RowIsValid(update_id)) {
			result_values[result_offset] = GetUpdateNullPadding<T>();
		} else {
			// it is not! we have to move base_table_data[update_id] to update_info
			result_values[result_offset] = UpdateSelectElement::Operation<T>(
			    *base_info.segment, OP::template Extract<T, V>(base_table_data, update_id));
		}
		result_ids[result_offset++] = UnsafeNumericCast<sel_t>(update_id);
	}
	// write any remaining entries from the old updates
	while (update_info_offset < update_info.N) {
		result_values[result_offset] = update_info_data[update_info_offset];
		result_ids[result_offset++] = update_tuples[update_info_offset];
		update_info_offset++;
	}
	// now copy them back
	update_info.N = UnsafeNumericCast<sel_t>(result_offset);
	memcpy(update_info_data, result_values, result_offset * sizeof(T));
	memcpy(update_tuples, result_ids, result_offset * sizeof(sel_t));

	// now we merge the new values into the base_info
	result_offset = 0;
	auto pick_new = [&](idx_t id, idx_t aidx, idx_t count) {
		result_values[result_offset] =
		    OP::template Extract<T, V>(update_vector_data, update_vector_sel.get_index(aidx));
		result_ids[result_offset] = UnsafeNumericCast<sel_t>(id);
		result_offset++;
	};
	auto pick_old = [&](idx_t id, idx_t bidx, idx_t count) {
		result_values[result_offset] = base_info_data[bidx];
		result_ids[result_offset] = UnsafeNumericCast<sel_t>(id);
		result_offset++;
	};
	// now we perform a merge of the new ids with the old ids
	auto merge = [&](idx_t id, idx_t aidx, idx_t bidx, idx_t count) {
		pick_new(id, aidx, count);
	};
	MergeLoop(ids, base_tuples, count, base_info.N, base_id, merge, pick_new, pick_old, sel);

	base_info.N = UnsafeNumericCast<sel_t>(result_offset);
	memcpy(base_info_data, result_values, result_offset * sizeof(T));
	memcpy(base_tuples, result_ids, result_offset * sizeof(sel_t));
}

static void MergeValidityLoop(UpdateInfo &base_info, Vector &base_data, UpdateInfo &update_info,
                              UnifiedVectorFormat &update, row_t *ids, idx_t count, const SelectionVector &sel,
                              idx_t row_group_start) {
	auto &base_validity = FlatVector::ValidityMutable(base_data);
	auto &update_validity = update.validity;
	MergeUpdateLoopInternal<bool, ValidityMask, ExtractValidityEntry>(
	    base_info, &base_validity, update_info, *update.sel, &update_validity, ids, count, sel, row_group_start);
}

template <class T>
static void MergeUpdateLoop(UpdateInfo &base_info, Vector &base_data, UpdateInfo &update_info,
                            UnifiedVectorFormat &update, row_t *ids, idx_t count, const SelectionVector &sel,
                            idx_t row_group_start) {
	auto base_table_data = FlatVector::GetDataMutable<T>(base_data);
	auto update_vector_data = update.GetData<T>(update);
	auto &base_validity = FlatVector::Validity(base_data);
	MergeUpdateLoopInternal<T, T>(base_info, base_table_data, update_info, *update.sel, update_vector_data, ids, count,
	                              sel, row_group_start, &base_validity);
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

//===--------------------------------------------------------------------===//
// Update statistics
//===--------------------------------------------------------------------===//
unique_ptr<BaseStatistics> UpdateSegment::GetStatistics() {
	lock_guard<mutex> stats_guard(stats_lock);
	return stats.statistics.ToUnique();
}

idx_t UpdateValidityStatistics(UpdateSegment *segment, SegmentStatistics &stats, UnifiedVectorFormat &update,
                               idx_t count, SelectionVector &sel) {
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
	sel.Initialize(nullptr);
	return count;
}

template <class T>
idx_t TemplatedUpdateNumericStatistics(UpdateSegment *segment, SegmentStatistics &stats, UnifiedVectorFormat &update,
                                       idx_t count, SelectionVector &sel) {
	auto update_data = update.GetData<T>(update);
	auto &mask = update.validity;

	if (mask.CannotHaveNull()) {
		stats.statistics.SetHasNoNullFast();
		for (idx_t i = 0; i < count; i++) {
			auto idx = update.sel->get_index(i);
			stats.statistics.UpdateNumericStats<T>(update_data[idx]);
		}
		sel.Initialize(nullptr);
		return count;
	} else {
		idx_t not_null_count = 0;
		sel.Initialize(STANDARD_VECTOR_SIZE);
		for (idx_t i = 0; i < count; i++) {
			auto idx = update.sel->get_index(i);
			if (mask.RowIsValid(idx)) {
				stats.statistics.SetHasNoNullFast();
				sel.set_index(not_null_count++, i);
				stats.statistics.UpdateNumericStats<T>(update_data[idx]);
			} else {
				stats.statistics.SetHasNullFast();
			}
		}
		return not_null_count;
	}
}

idx_t UpdateStringStatistics(UpdateSegment *segment, SegmentStatistics &stats, UnifiedVectorFormat &update, idx_t count,
                             SelectionVector &sel) {
	auto update_data = update.GetData<string_t>(update);
	auto &mask = update.validity;

	StatsWriter<string_t> stats_writer(stats.statistics.GetType());
	idx_t not_null_count = 0;
	if (mask.CannotHaveNull()) {
		stats.statistics.SetHasNoNullFast();
		for (idx_t i = 0; i < count; i++) {
			auto idx = update.sel->get_index(i);
			auto &str = update_data[idx];
			stats_writer.Update(str);
		}
		sel.Initialize(nullptr);
		not_null_count = count;
	} else {
		sel.Initialize(STANDARD_VECTOR_SIZE);
		for (idx_t i = 0; i < count; i++) {
			auto idx = update.sel->get_index(i);
			if (mask.RowIsValid(idx)) {
				stats.statistics.SetHasNoNullFast();
				sel.set_index(not_null_count++, i);
				auto &str = update_data[idx];
				stats_writer.Update(str);
			} else {
				stats.statistics.SetHasNullFast();
			}
		}
		if (not_null_count == count) {
			sel.Initialize(nullptr);
		}
	}
	stats_writer.Merge(stats.statistics);
	return not_null_count;
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
		// NULL values in the updates should have been filtered out before
		D_ASSERT(update.validity.RowIsValid(update_idx));
		if (original_validity.RowIsValid(original_idx) && data[update_idx] == original_data[original_idx]) {
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

UpdateInfo *CreateEmptyUpdateInfo(TransactionData transaction, DuckTableEntry &table_entry, idx_t type_size,
                                  idx_t count, unsafe_unique_array<char> &data, idx_t row_group_start) {
	data = make_unsafe_uniq_array_uninitialized<char>(UpdateInfo::GetAllocSize(type_size));
	auto update_info = reinterpret_cast<UpdateInfo *>(data.get());
	UpdateInfo::Initialize(*update_info, table_entry, transaction.GetTransactionId(), row_group_start);
	return update_info;
}

void UpdateSegment::InitializeUpdateInfo(idx_t vector_idx) {
	// create the versions for this segment, if there are none yet
	if (!root) {
		root = make_uniq<UpdateNode>(column_data.block_manager.buffer_manager);
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
	SelectionVector sel;
	SegmentStatistics update_stats(column_data.type);
	count = statistics_update_function(this, update_stats, update_format, count, sel);
	if (count == 0) {
		MergeStatistics(update_stats);
		return;
	}

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
			MergeStatistics(update_stats);
			return;
		}
	}
	if (statistics_update_function == UpdateStringStatistics) {
		// for strings - we need to push all strings we are going to place here into the string heap of the segment
		update_p.Flatten();
		auto update_data = FlatVector::GetDataMutable<string_t>(update_p);
		for (idx_t i = 0; i < count; i++) {
			auto idx = sel.get_index(i);
			update_data[idx] = GetStringHeap().AddBlob(update_data[idx]);
		}
		update_p.ToUnifiedFormat(update_format);
	}

	InitializeUpdateInfo(vector_index);

	D_ASSERT(idx_t(first_id) >= row_group_start);

	if (root->info[vector_index].IsSet()) {
		// there is already a version here, check if there are any conflicts and search for the node that belongs to
		// this transaction in the version chain
		auto root_pointer = root->info[vector_index];
		auto root_pin = root_pointer.Pin();

		UndoBufferReference node_ref;
		CheckForConflicts(UpdateInfo::Get(root_pin).next, transaction, ids, sel, count,
		                  UnsafeNumericCast<row_t>(vector_offset), node_ref);

		ReallocateRootInfoIfNeeded(UpdateInfo::Get(root_pin), count, vector_index);
		root_pointer = root->info[vector_index];
		root_pin = root_pointer.Pin();
		auto &base_info = UpdateInfo::Get(root_pin);

		// there are no conflicts - continue with the update
		unsafe_unique_array<char> update_info_data;
		optional_ptr<UpdateInfo> node;
		if (!node_ref.IsSet()) {
			// no updates made yet by this transaction: initially the update info to empty
			if (transaction.transaction) {
				auto &dtransaction = transaction.transaction->Cast<DuckTransaction>();
				node_ref = dtransaction.CreateUpdateInfo(table_entry, type_size, count, row_group_start);
				node = &UpdateInfo::Get(node_ref);
			} else {
				node = CreateEmptyUpdateInfo(transaction, table_entry, type_size, count, update_info_data,
				                             row_group_start);
			}
			node->segment = this;
			node->vector_index = vector_index;
			node->N = 0;
			node->column_index = column_index;

			// insert the new node into the chain
			// pin first: Pin can throw (OOM), and rolling back a half-linked node corrupts the chain
			UndoBufferReference next_pin;
			if (base_info.next.IsSet()) {
				next_pin = base_info.next.Pin();
			}
			node->next = base_info.next;
			if (next_pin.IsSet()) {
				auto &next_info = UpdateInfo::Get(next_pin);
				next_info.prev = node_ref.GetBufferPointer();
			}
			node->prev = root_pointer;
			base_info.next = transaction.transaction ? node_ref.GetBufferPointer() : UndoBufferPointer();
		} else {
			// we already had updates made to this transaction
			node = &UpdateInfo::Get(node_ref);
		}
		base_info.Verify();
		node->Verify();

		// now we are going to perform the merge
		merge_update_function(base_info, base_data, *node, update_format, ids, count, sel, row_group_start);

		base_info.Verify();
		node->Verify();
	} else {
		// there is no version info yet: create the top level update info and fill it with the updates
		// allocate space for the UpdateInfo in the allocator (compact: sized to actual count)
		idx_t compact_capacity = UpdateInfo::GetCompactCapacity(count);
		idx_t alloc_size = UpdateInfo::GetAllocSize(type_size, compact_capacity);
		auto handle = root->allocator.Allocate(alloc_size);
		auto &update_info = UpdateInfo::Get(handle);
		UpdateInfo::Initialize(update_info, table_entry, MAX_COMMIT_ID, row_group_start, compact_capacity);
		update_info.column_index = column_index;

		InitializeUpdateInfo(update_info, ids, sel, count, vector_index, vector_offset);

		// now create the transaction level update info in the undo log
		unsafe_unique_array<char> update_info_data;
		UndoBufferReference node_ref;
		optional_ptr<UpdateInfo> transaction_node;
		if (transaction.transaction) {
			node_ref = transaction.transaction->CreateUpdateInfo(table_entry, type_size, count, row_group_start);
			transaction_node = &UpdateInfo::Get(node_ref);
		} else {
			transaction_node =
			    CreateEmptyUpdateInfo(transaction, table_entry, type_size, count, update_info_data, row_group_start);
		}

		InitializeUpdateInfo(*transaction_node, ids, sel, count, vector_index, vector_offset);

		// we write the updates in the update node data, and write the updates in the info
		initialize_update_function(*transaction_node, base_data, update_info, update_format, sel);

		update_info.next = transaction.transaction ? node_ref.GetBufferPointer() : UndoBufferPointer();
		update_info.prev = UndoBufferPointer();
		transaction_node->next = UndoBufferPointer();
		transaction_node->prev = handle.GetBufferPointer();
		transaction_node->column_index = column_index;

		transaction_node->Verify();
		update_info.Verify();

		root->info[vector_index] = handle.GetBufferPointer();
	}
	MergeStatistics(update_stats);
}

void UpdateSegment::MergeStatistics(const SegmentStatistics &update_stats) {
	lock_guard<mutex> stats_guard(stats_lock);
	stats.statistics.Merge(update_stats.statistics);
}

bool UpdateSegment::HasUpdates() const {
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

} // namespace duckdb
