#include "duckdb/storage/numeric_segment.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/storage/table/append_state.hpp"
#include "duckdb/transaction/update_info.hpp"
#include "duckdb/transaction/transaction.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/common/vector_size.hpp"
#include "duckdb/storage/statistics/numeric_statistics.hpp"
#include "duckdb/planner/table_filter.hpp"

using namespace std;

namespace duckdb {

static NumericSegment::append_function_t GetAppendFunction(PhysicalType type);

static NumericSegment::update_function_t GetUpdateFunction(PhysicalType type);

static NumericSegment::update_info_fetch_function_t GetUpdateInfoFetchFunction(PhysicalType type);

static NumericSegment::rollback_update_function_t GetRollbackUpdateFunction(PhysicalType type);

static NumericSegment::merge_update_function_t GetMergeUpdateFunction(PhysicalType type);

static NumericSegment::update_info_append_function_t GetUpdateInfoAppendFunction(PhysicalType type);

NumericSegment::NumericSegment(BufferManager &manager, PhysicalType type, idx_t row_start, block_id_t block_id)
    : UncompressedSegment(manager, type, row_start) {
	// set up the different functions for this type of segment
	this->append_function = GetAppendFunction(type);
	this->update_function = GetUpdateFunction(type);
	this->fetch_from_update_info = GetUpdateInfoFetchFunction(type);
	this->append_from_update_info = GetUpdateInfoAppendFunction(type);
	this->rollback_update = GetRollbackUpdateFunction(type);
	this->merge_update_function = GetMergeUpdateFunction(type);

	// figure out how many vectors we want to store in this block
	this->type_size = GetTypeIdSize(type);
	this->vector_size = sizeof(nullmask_t) + type_size * STANDARD_VECTOR_SIZE;
	this->max_vector_count = Storage::BLOCK_SIZE / vector_size;

	if (block_id == INVALID_BLOCK) {
		// no block id specified: allocate a buffer for the uncompressed segment
		this->block = manager.RegisterMemory(Storage::BLOCK_ALLOC_SIZE, false);
		auto handle = manager.Pin(block);
		// initialize nullmasks to 0 for all vectors
		for (idx_t i = 0; i < max_vector_count; i++) {
			auto mask = (nullmask_t *)(handle->node->buffer + (i * vector_size));
			mask->reset();
		}
	} else {
		this->block = manager.RegisterBlock(block_id);
	}
}

template <class T, class OP>
void Select(SelectionVector &sel, Vector &result, unsigned char *source, nullmask_t *source_nullmask, T constant,
            idx_t &approved_tuple_count) {
	result.vector_type = VectorType::FLAT_VECTOR;
	auto result_data = FlatVector::GetData(result);
	SelectionVector new_sel(approved_tuple_count);
	idx_t result_count = 0;
	if (source_nullmask->any()) {
		for (idx_t i = 0; i < approved_tuple_count; i++) {
			idx_t src_idx = sel.get_index(i);
			bool comparison_result = !(*source_nullmask)[src_idx] && OP::Operation(((T *)source)[src_idx], constant);
			((T *)result_data)[src_idx] = ((T *)source)[src_idx];
			new_sel.set_index(result_count, src_idx);
			result_count += comparison_result;
		}
	} else {
		for (idx_t i = 0; i < approved_tuple_count; i++) {
			idx_t src_idx = sel.get_index(i);
			bool comparison_result = OP::Operation(((T *)source)[src_idx], constant);
			((T *)result_data)[src_idx] = ((T *)source)[src_idx];
			new_sel.set_index(result_count, src_idx);
			result_count += comparison_result;
		}
	}
	sel.Initialize(new_sel);
	approved_tuple_count = result_count;
}

template <class T, class OPL, class OPR>
void Select(SelectionVector &sel, Vector &result, unsigned char *source, nullmask_t *source_nullmask,
            const T constantLeft, const T constantRight, idx_t &approved_tuple_count) {
	result.vector_type = VectorType::FLAT_VECTOR;
	auto result_data = FlatVector::GetData(result);
	SelectionVector new_sel(approved_tuple_count);
	idx_t result_count = 0;
	if (source_nullmask->any()) {
		for (idx_t i = 0; i < approved_tuple_count; i++) {
			idx_t src_idx = sel.get_index(i);
			bool comparison_result = !(*source_nullmask)[src_idx] &&
			                         OPL::Operation(((T *)source)[src_idx], constantLeft) &&
			                         OPR::Operation(((T *)source)[src_idx], constantRight);
			((T *)result_data)[src_idx] = ((T *)source)[src_idx];
			new_sel.set_index(result_count, src_idx);
			result_count += comparison_result;
		}
	} else {
		for (idx_t i = 0; i < approved_tuple_count; i++) {
			idx_t src_idx = sel.get_index(i);
			bool comparison_result = OPL::Operation(((T *)source)[src_idx], constantLeft) &&
			                         OPR::Operation(((T *)source)[src_idx], constantRight);
			((T *)result_data)[src_idx] = ((T *)source)[src_idx];
			new_sel.set_index(result_count, src_idx);
			result_count += comparison_result;
		}
	}
	sel.Initialize(new_sel);
	approved_tuple_count = result_count;
}

template <class OP>
static void templated_select_operation(SelectionVector &sel, Vector &result, PhysicalType type, unsigned char *source,
                                       nullmask_t *source_mask, Value &constant, idx_t &approved_tuple_count) {
	// the inplace loops take the result as the last parameter
	switch (type) {
	case PhysicalType::INT8: {
		Select<int8_t, OP>(sel, result, source, source_mask, constant.value_.tinyint, approved_tuple_count);
		break;
	}
	case PhysicalType::INT16: {
		Select<int16_t, OP>(sel, result, source, source_mask, constant.value_.smallint, approved_tuple_count);
		;
		break;
	}
	case PhysicalType::INT32: {
		Select<int32_t, OP>(sel, result, source, source_mask, constant.value_.integer, approved_tuple_count);
		break;
	}
	case PhysicalType::INT64: {
		Select<int64_t, OP>(sel, result, source, source_mask, constant.value_.bigint, approved_tuple_count);
		break;
	}
	case PhysicalType::INT128: {
		Select<hugeint_t, OP>(sel, result, source, source_mask, constant.value_.hugeint, approved_tuple_count);
		break;
	}
	case PhysicalType::FLOAT: {
		Select<float, OP>(sel, result, source, source_mask, constant.value_.float_, approved_tuple_count);
		break;
	}
	case PhysicalType::DOUBLE: {
		Select<double, OP>(sel, result, source, source_mask, constant.value_.double_, approved_tuple_count);
		break;
	}
	default:
		throw InvalidTypeException(type, "Invalid type for filter pushed down to table comparison");
	}
}

template <class OPL, class OPR>
static void templated_select_operation_between(SelectionVector &sel, Vector &result, PhysicalType type,
                                               unsigned char *source, nullmask_t *source_mask, Value &constantLeft,
                                               Value &constantRight, idx_t &approved_tuple_count) {
	// the inplace loops take the result as the last parameter
	switch (type) {
	case PhysicalType::INT8: {
		Select<int8_t, OPL, OPR>(sel, result, source, source_mask, constantLeft.value_.tinyint,
		                         constantRight.value_.tinyint, approved_tuple_count);
		break;
	}
	case PhysicalType::INT16: {
		Select<int16_t, OPL, OPR>(sel, result, source, source_mask, constantLeft.value_.smallint,
		                          constantRight.value_.smallint, approved_tuple_count);
		break;
	}
	case PhysicalType::INT32: {
		Select<int32_t, OPL, OPR>(sel, result, source, source_mask, constantLeft.value_.integer,
		                          constantRight.value_.integer, approved_tuple_count);
		break;
	}
	case PhysicalType::INT64: {
		Select<int64_t, OPL, OPR>(sel, result, source, source_mask, constantLeft.value_.bigint,
		                          constantRight.value_.bigint, approved_tuple_count);
		break;
	}
	case PhysicalType::INT128: {
		Select<hugeint_t, OPL, OPR>(sel, result, source, source_mask, constantLeft.value_.hugeint,
		                            constantRight.value_.hugeint, approved_tuple_count);
		break;
	}
	case PhysicalType::FLOAT: {
		Select<float, OPL, OPR>(sel, result, source, source_mask, constantLeft.value_.float_,
		                        constantRight.value_.float_, approved_tuple_count);
		break;
	}
	case PhysicalType::DOUBLE: {
		Select<double, OPL, OPR>(sel, result, source, source_mask, constantLeft.value_.double_,
		                         constantRight.value_.double_, approved_tuple_count);
		break;
	}
	default:
		throw InvalidTypeException(type, "Invalid type for filter pushed down to table comparison");
	}
}

void NumericSegment::Select(ColumnScanState &state, Vector &result, SelectionVector &sel, idx_t &approved_tuple_count,
                            vector<TableFilter> &tableFilter) {
	auto vector_index = state.vector_index;
	D_ASSERT(vector_index < max_vector_count);
	D_ASSERT(vector_index * STANDARD_VECTOR_SIZE <= tuple_count);

	// pin the buffer for this segment
	auto handle = manager.Pin(block);
	auto data = handle->node->buffer;
	auto offset = vector_index * vector_size;
	auto source_nullmask = (nullmask_t *)(data + offset);
	auto source_data = data + offset + sizeof(nullmask_t);

	if (tableFilter.size() == 1) {
		switch (tableFilter[0].comparison_type) {
		case ExpressionType::COMPARE_EQUAL: {
			templated_select_operation<Equals>(sel, result, state.current->type.InternalType(), source_data,
			                                   source_nullmask, tableFilter[0].constant, approved_tuple_count);
			break;
		}
		case ExpressionType::COMPARE_LESSTHAN: {
			templated_select_operation<LessThan>(sel, result, state.current->type.InternalType(), source_data,
			                                     source_nullmask, tableFilter[0].constant, approved_tuple_count);
			break;
		}
		case ExpressionType::COMPARE_GREATERTHAN: {
			templated_select_operation<GreaterThan>(sel, result, state.current->type.InternalType(), source_data,
			                                        source_nullmask, tableFilter[0].constant, approved_tuple_count);
			break;
		}
		case ExpressionType::COMPARE_LESSTHANOREQUALTO: {
			templated_select_operation<LessThanEquals>(sel, result, state.current->type.InternalType(), source_data,
			                                           source_nullmask, tableFilter[0].constant, approved_tuple_count);
			break;
		}
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO: {
			templated_select_operation<GreaterThanEquals>(sel, result, state.current->type.InternalType(), source_data,
			                                              source_nullmask, tableFilter[0].constant,
			                                              approved_tuple_count);
			break;
		}
		default:
			throw NotImplementedException("Unknown comparison type for filter pushed down to table!");
		}
	} else {
		D_ASSERT(tableFilter[0].comparison_type == ExpressionType::COMPARE_GREATERTHAN ||
		         tableFilter[0].comparison_type == ExpressionType::COMPARE_GREATERTHANOREQUALTO);
		D_ASSERT(tableFilter[1].comparison_type == ExpressionType::COMPARE_LESSTHAN ||
		         tableFilter[1].comparison_type == ExpressionType::COMPARE_LESSTHANOREQUALTO);

		if (tableFilter[0].comparison_type == ExpressionType::COMPARE_GREATERTHAN) {
			if (tableFilter[1].comparison_type == ExpressionType::COMPARE_LESSTHAN) {
				templated_select_operation_between<GreaterThan, LessThan>(
				    sel, result, state.current->type.InternalType(), source_data, source_nullmask,
				    tableFilter[0].constant, tableFilter[1].constant, approved_tuple_count);
			} else {
				templated_select_operation_between<GreaterThan, LessThanEquals>(
				    sel, result, state.current->type.InternalType(), source_data, source_nullmask,
				    tableFilter[0].constant, tableFilter[1].constant, approved_tuple_count);
			}
		} else {
			if (tableFilter[1].comparison_type == ExpressionType::COMPARE_LESSTHAN) {
				templated_select_operation_between<GreaterThanEquals, LessThan>(
				    sel, result, state.current->type.InternalType(), source_data, source_nullmask,
				    tableFilter[0].constant, tableFilter[1].constant, approved_tuple_count);
			} else {
				templated_select_operation_between<GreaterThanEquals, LessThanEquals>(
				    sel, result, state.current->type.InternalType(), source_data, source_nullmask,
				    tableFilter[0].constant, tableFilter[1].constant, approved_tuple_count);
			}
		}
	}
}

//===--------------------------------------------------------------------===//
// Scan
//===--------------------------------------------------------------------===//
void NumericSegment::InitializeScan(ColumnScanState &state) {
	// pin the primary buffer
	state.primary_handle = manager.Pin(block);
}

//===--------------------------------------------------------------------===//
// Fetch base data
//===--------------------------------------------------------------------===//
void NumericSegment::FetchBaseData(ColumnScanState &state, idx_t vector_index, Vector &result) {
	D_ASSERT(vector_index < max_vector_count);
	D_ASSERT(vector_index * STANDARD_VECTOR_SIZE <= tuple_count);

	auto data = state.primary_handle->node->buffer;
	auto offset = vector_index * vector_size;

	idx_t count = GetVectorCount(vector_index);
	auto source_nullmask = (nullmask_t *)(data + offset);
	auto source_data = data + offset + sizeof(nullmask_t);

	// fetch the nullmask and copy the data from the base table
	result.vector_type = VectorType::FLAT_VECTOR;
	FlatVector::SetNullmask(result, *source_nullmask);
	memcpy(FlatVector::GetData(result), source_data, count * type_size);
}

void NumericSegment::FetchUpdateData(ColumnScanState &state, Transaction &transaction, UpdateInfo *version,
                                     Vector &result) {
	fetch_from_update_info(transaction, version, result);
}

template <class T>
static void templated_assignment(SelectionVector &sel, data_ptr_t source, data_ptr_t result,
                                 nullmask_t &source_nullmask, nullmask_t &result_nullmask, idx_t approved_tuple_count) {
	if (source_nullmask.any()) {
		for (size_t i = 0; i < approved_tuple_count; i++) {
			if (source_nullmask[sel.get_index(i)]) {
				result_nullmask.set(i, true);
			} else {
				((T *)result)[i] = ((T *)source)[sel.get_index(i)];
			}
		}
	} else {
		for (size_t i = 0; i < approved_tuple_count; i++) {
			((T *)result)[i] = ((T *)source)[sel.get_index(i)];
		}
	}
}

void NumericSegment::FilterFetchBaseData(ColumnScanState &state, Vector &result, SelectionVector &sel,
                                         idx_t &approved_tuple_count) {
	auto vector_index = state.vector_index;
	D_ASSERT(vector_index < max_vector_count);
	D_ASSERT(vector_index * STANDARD_VECTOR_SIZE <= tuple_count);

	// pin the buffer for this segment
	auto data = state.primary_handle->node->buffer;

	auto offset = vector_index * vector_size;
	auto source_nullmask = (nullmask_t *)(data + offset);
	auto source_data = data + offset + sizeof(nullmask_t);
	// fetch the nullmask and copy the data from the base table
	result.vector_type = VectorType::FLAT_VECTOR;
	auto result_data = FlatVector::GetData(result);
	nullmask_t result_nullmask;
	// the inplace loops take the result as the last parameter
	switch (type) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8: {
		templated_assignment<int8_t>(sel, source_data, result_data, *source_nullmask, result_nullmask,
		                             approved_tuple_count);
		break;
	}
	case PhysicalType::INT16: {
		templated_assignment<int16_t>(sel, source_data, result_data, *source_nullmask, result_nullmask,
		                              approved_tuple_count);
		break;
	}
	case PhysicalType::INT32: {
		templated_assignment<int32_t>(sel, source_data, result_data, *source_nullmask, result_nullmask,
		                              approved_tuple_count);
		break;
	}
	case PhysicalType::INT64: {
		templated_assignment<int64_t>(sel, source_data, result_data, *source_nullmask, result_nullmask,
		                              approved_tuple_count);
		break;
	}
	case PhysicalType::INT128: {
		templated_assignment<hugeint_t>(sel, source_data, result_data, *source_nullmask, result_nullmask,
		                                approved_tuple_count);
		break;
	}
	case PhysicalType::FLOAT: {
		templated_assignment<float>(sel, source_data, result_data, *source_nullmask, result_nullmask,
		                            approved_tuple_count);
		break;
	}
	case PhysicalType::DOUBLE: {
		templated_assignment<double>(sel, source_data, result_data, *source_nullmask, result_nullmask,
		                             approved_tuple_count);
		break;
	}
	case PhysicalType::INTERVAL: {
		templated_assignment<interval_t>(sel, source_data, result_data, *source_nullmask, result_nullmask,
		                                 approved_tuple_count);
		break;
	}
	default:
		throw InvalidTypeException(type, "Invalid type for filter scan");
	}

	FlatVector::SetNullmask(result, result_nullmask);
}

//===--------------------------------------------------------------------===//
// Fetch
//===--------------------------------------------------------------------===//
void NumericSegment::FetchRow(ColumnFetchState &state, Transaction &transaction, row_t row_id, Vector &result,
                              idx_t result_idx) {
	auto read_lock = lock.GetSharedLock();
	auto handle = manager.Pin(block);

	// get the vector index
	idx_t vector_index = row_id / STANDARD_VECTOR_SIZE;
	idx_t id_in_vector = row_id - vector_index * STANDARD_VECTOR_SIZE;
	D_ASSERT(vector_index < max_vector_count);

	// first fetch the data from the base table
	auto data = handle->node->buffer + vector_index * vector_size;
	auto &nullmask = *((nullmask_t *)(data));
	auto vector_ptr = data + sizeof(nullmask_t);

	FlatVector::SetNull(result, result_idx, nullmask[id_in_vector]);
	memcpy(FlatVector::GetData(result) + result_idx * type_size, vector_ptr + id_in_vector * type_size, type_size);
	if (versions && versions[vector_index]) {
		// version information: follow the version chain to find out if we need to load this tuple data from any other
		// version
		append_from_update_info(transaction, versions[vector_index], id_in_vector, result, result_idx);
	}
}

//===--------------------------------------------------------------------===//
// Append
//===--------------------------------------------------------------------===//
idx_t NumericSegment::Append(SegmentStatistics &stats, Vector &data, idx_t offset, idx_t count) {
	D_ASSERT(data.type.InternalType() == type);
	auto handle = manager.Pin(block);

	idx_t initial_count = tuple_count;
	while (count > 0) {
		// get the vector index of the vector to append to and see how many tuples we can append to that vector
		idx_t vector_index = tuple_count / STANDARD_VECTOR_SIZE;
		if (vector_index == max_vector_count) {
			break;
		}
		idx_t current_tuple_count = tuple_count - vector_index * STANDARD_VECTOR_SIZE;
		idx_t append_count = MinValue(STANDARD_VECTOR_SIZE - current_tuple_count, count);

		// now perform the actual append
		append_function(stats, handle->node->buffer + vector_size * vector_index, current_tuple_count, data, offset,
		                append_count);

		count -= append_count;
		offset += append_count;
		tuple_count += append_count;
	}
	return tuple_count - initial_count;
}

//===--------------------------------------------------------------------===//
// Update
//===--------------------------------------------------------------------===//
void NumericSegment::Update(ColumnData &column_data, SegmentStatistics &stats, Transaction &transaction, Vector &update,
                            row_t *ids, idx_t count, idx_t vector_index, idx_t vector_offset, UpdateInfo *node) {
	if (!node) {
		auto handle = manager.Pin(block);

		// create a new node in the undo buffer for this update
		node = CreateUpdateInfo(column_data, transaction, ids, count, vector_index, vector_offset, type_size);
		// now move the original data into the UpdateInfo
		update_function(stats, node, handle->node->buffer + vector_index * vector_size, update);
	} else {
		// node already exists for this transaction, we need to merge the new updates with the existing updates
		auto handle = manager.Pin(block);

		merge_update_function(stats, node, handle->node->buffer + vector_index * vector_size, update, ids, count,
		                      vector_offset);
	}
}

void NumericSegment::RollbackUpdate(UpdateInfo *info) {
	// obtain an exclusive lock
	auto lock_handle = lock.GetExclusiveLock();
	auto handle = manager.Pin(block);

	// move the data from the UpdateInfo back into the base table
	rollback_update(info, handle->node->buffer + info->vector_index * vector_size);

	CleanupUpdate(info);
}

//===--------------------------------------------------------------------===//
// Append
//===--------------------------------------------------------------------===//
template <class T> static inline void update_numeric_statistics_internal(T new_value, T &min, T &max) {
	if (LessThan::Operation(new_value, min)) {
		min = new_value;
	}
	if (GreaterThan::Operation(new_value, max)) {
		max = new_value;
	}
}

template <class T> static inline void update_numeric_statistics(SegmentStatistics &stats, T new_value);

template <> inline void update_numeric_statistics<int8_t>(SegmentStatistics &stats, int8_t new_value) {
	auto &nstats = (NumericStatistics &)*stats.statistics;
	update_numeric_statistics_internal<int8_t>(new_value, nstats.min.value_.tinyint, nstats.max.value_.tinyint);
}

template <> inline void update_numeric_statistics<int16_t>(SegmentStatistics &stats, int16_t new_value) {
	auto &nstats = (NumericStatistics &)*stats.statistics;
	update_numeric_statistics_internal<int16_t>(new_value, nstats.min.value_.smallint, nstats.max.value_.smallint);
}

template <> inline void update_numeric_statistics<int32_t>(SegmentStatistics &stats, int32_t new_value) {
	auto &nstats = (NumericStatistics &)*stats.statistics;
	update_numeric_statistics_internal<int32_t>(new_value, nstats.min.value_.integer, nstats.max.value_.integer);
}

template <> inline void update_numeric_statistics<int64_t>(SegmentStatistics &stats, int64_t new_value) {
	auto &nstats = (NumericStatistics &)*stats.statistics;
	update_numeric_statistics_internal<int64_t>(new_value, nstats.min.value_.bigint, nstats.max.value_.bigint);
}

template <> inline void update_numeric_statistics<hugeint_t>(SegmentStatistics &stats, hugeint_t new_value) {
	auto &nstats = (NumericStatistics &)*stats.statistics;
	update_numeric_statistics_internal<hugeint_t>(new_value, nstats.min.value_.hugeint, nstats.max.value_.hugeint);
}

template <> inline void update_numeric_statistics<float>(SegmentStatistics &stats, float new_value) {
	auto &nstats = (NumericStatistics &)*stats.statistics;
	update_numeric_statistics_internal<float>(new_value, nstats.min.value_.float_, nstats.max.value_.float_);
}

template <> inline void update_numeric_statistics<double>(SegmentStatistics &stats, double new_value) {
	auto &nstats = (NumericStatistics &)*stats.statistics;
	update_numeric_statistics_internal<double>(new_value, nstats.min.value_.double_, nstats.max.value_.double_);
}

template <> void update_numeric_statistics<interval_t>(SegmentStatistics &stats, interval_t new_value) {
}

template <class T>
static void append_loop(SegmentStatistics &stats, data_ptr_t target, idx_t target_offset, Vector &source, idx_t offset,
                        idx_t count) {
	auto &nullmask = *((nullmask_t *)target);

	VectorData adata;
	source.Orrify(count, adata);

	auto sdata = (T *)adata.data;
	auto tdata = (T *)(target + sizeof(nullmask_t));
	if (adata.nullmask->any()) {
		for (idx_t i = 0; i < count; i++) {
			auto source_idx = adata.sel->get_index(offset + i);
			auto target_idx = target_offset + i;
			bool is_null = (*adata.nullmask)[source_idx];
			if (is_null) {
				nullmask[target_idx] = true;
				stats.statistics->has_null = true;
			} else {
				update_numeric_statistics<T>(stats, sdata[source_idx]);
				tdata[target_idx] = sdata[source_idx];
			}
		}
	} else {
		for (idx_t i = 0; i < count; i++) {
			auto source_idx = adata.sel->get_index(offset + i);
			auto target_idx = target_offset + i;
			update_numeric_statistics<T>(stats, sdata[source_idx]);
			tdata[target_idx] = sdata[source_idx];
		}
	}
}

static NumericSegment::append_function_t GetAppendFunction(PhysicalType type) {
	switch (type) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		return append_loop<int8_t>;
	case PhysicalType::INT16:
		return append_loop<int16_t>;
	case PhysicalType::INT32:
		return append_loop<int32_t>;
	case PhysicalType::INT64:
		return append_loop<int64_t>;
	case PhysicalType::INT128:
		return append_loop<hugeint_t>;
	case PhysicalType::FLOAT:
		return append_loop<float>;
	case PhysicalType::DOUBLE:
		return append_loop<double>;
	case PhysicalType::INTERVAL:
		return append_loop<interval_t>;
	default:
		throw NotImplementedException("Unimplemented type for uncompressed segment");
	}
}

//===--------------------------------------------------------------------===//
// Update
//===--------------------------------------------------------------------===//
template <class T>
static void update_loop_null(T *__restrict undo_data, T *__restrict base_data, T *__restrict new_data,
                             nullmask_t &undo_nullmask, nullmask_t &base_nullmask, nullmask_t &new_nullmask,
                             idx_t count, sel_t *__restrict base_sel, SegmentStatistics &stats) {
	for (idx_t i = 0; i < count; i++) {
		bool is_null = new_nullmask[i];
		// first move the base data into the undo buffer info
		undo_data[i] = base_data[base_sel[i]];
		undo_nullmask[base_sel[i]] = base_nullmask[base_sel[i]];
		// now move the new data in-place into the base table
		base_data[base_sel[i]] = new_data[i];
		base_nullmask[base_sel[i]] = is_null;
		// update the min max with the new data
		if (is_null) {
			stats.statistics->has_null = true;
		} else {
			update_numeric_statistics<T>(stats, new_data[i]);
		}
	}
}

template <class T>
static void update_loop_no_null(T *__restrict undo_data, T *__restrict base_data, T *__restrict new_data, idx_t count,
                                sel_t *__restrict base_sel, SegmentStatistics &stats) {
	for (idx_t i = 0; i < count; i++) {
		// first move the base data into the undo buffer info
		undo_data[i] = base_data[base_sel[i]];
		// now move the new data in-place into the base table
		base_data[base_sel[i]] = new_data[i];
		// update the min max with the new data
		update_numeric_statistics<T>(stats, new_data[i]);
	}
}

template <class T>
static void update_loop(SegmentStatistics &stats, UpdateInfo *info, data_ptr_t base, Vector &update) {
	auto update_data = FlatVector::GetData<T>(update);
	auto &update_nullmask = FlatVector::Nullmask(update);
	auto nullmask = (nullmask_t *)base;
	auto base_data = (T *)(base + sizeof(nullmask_t));
	auto undo_data = (T *)info->tuple_data;

	if (update_nullmask.any() || nullmask->any()) {
		update_loop_null(undo_data, base_data, update_data, info->nullmask, *nullmask, update_nullmask, info->N,
		                 info->tuples, stats);
	} else {
		update_loop_no_null(undo_data, base_data, update_data, info->N, info->tuples, stats);
	}
}

static NumericSegment::update_function_t GetUpdateFunction(PhysicalType type) {
	switch (type) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		return update_loop<int8_t>;
	case PhysicalType::INT16:
		return update_loop<int16_t>;
	case PhysicalType::INT32:
		return update_loop<int32_t>;
	case PhysicalType::INT64:
		return update_loop<int64_t>;
	case PhysicalType::INT128:
		return update_loop<hugeint_t>;
	case PhysicalType::FLOAT:
		return update_loop<float>;
	case PhysicalType::DOUBLE:
		return update_loop<double>;
	case PhysicalType::INTERVAL:
		return update_loop<interval_t>;
	default:
		throw NotImplementedException("Unimplemented type for uncompressed segment");
	}
}

//===--------------------------------------------------------------------===//
// Merge Update
//===--------------------------------------------------------------------===//
template <class T>
static void merge_update_loop(SegmentStatistics &stats, UpdateInfo *node, data_ptr_t base, Vector &update, row_t *ids,
                              idx_t count, idx_t vector_offset) {
	auto &base_nullmask = *((nullmask_t *)base);
	auto base_data = (T *)(base + sizeof(nullmask_t));
	auto info_data = (T *)node->tuple_data;
	auto update_data = FlatVector::GetData<T>(update);
	auto &update_nullmask = FlatVector::Nullmask(update);
	for (idx_t i = 0; i < count; i++) {
		update_numeric_statistics<T>(stats, update_data[i]);
	}

	// first we copy the old update info into a temporary structure
	sel_t old_ids[STANDARD_VECTOR_SIZE];
	T old_data[STANDARD_VECTOR_SIZE];

	memcpy(old_ids, node->tuples, node->N * sizeof(sel_t));
	memcpy(old_data, node->tuple_data, node->N * sizeof(T));

	// now we perform a merge of the new ids with the old ids
	auto merge = [&](idx_t id, idx_t aidx, idx_t bidx, idx_t count) {
		// new_id and old_id are the same:
		// insert the new data into the base table
		base_nullmask[id] = update_nullmask[aidx];
		base_data[id] = update_data[aidx];
		// insert the old data in the UpdateInfo
		info_data[count] = old_data[bidx];
		node->tuples[count] = id;
	};
	auto pick_new = [&](idx_t id, idx_t aidx, idx_t count) {
		// new_id comes before the old id
		// insert the base table data into the update info
		info_data[count] = base_data[id];
		node->nullmask[id] = base_nullmask[id];

		// and insert the update info into the base table
		base_nullmask[id] = update_nullmask[aidx];
		base_data[id] = update_data[aidx];

		node->tuples[count] = id;
	};
	auto pick_old = [&](idx_t id, idx_t bidx, idx_t count) {
		// old_id comes before new_id, insert the old data
		info_data[count] = old_data[bidx];
		node->tuples[count] = id;
	};
	// perform the merge
	node->N = merge_loop(ids, old_ids, count, node->N, vector_offset, merge, pick_new, pick_old);
}

static NumericSegment::merge_update_function_t GetMergeUpdateFunction(PhysicalType type) {
	switch (type) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		return merge_update_loop<int8_t>;
	case PhysicalType::INT16:
		return merge_update_loop<int16_t>;
	case PhysicalType::INT32:
		return merge_update_loop<int32_t>;
	case PhysicalType::INT64:
		return merge_update_loop<int64_t>;
	case PhysicalType::INT128:
		return merge_update_loop<hugeint_t>;
	case PhysicalType::FLOAT:
		return merge_update_loop<float>;
	case PhysicalType::DOUBLE:
		return merge_update_loop<double>;
	case PhysicalType::INTERVAL:
		return merge_update_loop<interval_t>;
	default:
		throw NotImplementedException("Unimplemented type for uncompressed segment");
	}
}

//===--------------------------------------------------------------------===//
// Update Fetch
//===--------------------------------------------------------------------===//
template <class T> static void update_info_fetch(Transaction &transaction, UpdateInfo *info, Vector &result) {
	auto result_data = FlatVector::GetData<T>(result);
	auto &result_mask = FlatVector::Nullmask(result);
	UpdateInfo::UpdatesForTransaction(info, transaction, [&](UpdateInfo *current) {
		auto info_data = (T *)current->tuple_data;
		for (idx_t i = 0; i < current->N; i++) {
			result_data[current->tuples[i]] = info_data[i];
			result_mask[current->tuples[i]] = current->nullmask[current->tuples[i]];
		}
	});
}

static NumericSegment::update_info_fetch_function_t GetUpdateInfoFetchFunction(PhysicalType type) {
	switch (type) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		return update_info_fetch<int8_t>;
	case PhysicalType::INT16:
		return update_info_fetch<int16_t>;
	case PhysicalType::INT32:
		return update_info_fetch<int32_t>;
	case PhysicalType::INT64:
		return update_info_fetch<int64_t>;
	case PhysicalType::INT128:
		return update_info_fetch<hugeint_t>;
	case PhysicalType::FLOAT:
		return update_info_fetch<float>;
	case PhysicalType::DOUBLE:
		return update_info_fetch<double>;
	case PhysicalType::INTERVAL:
		return update_info_fetch<interval_t>;
	default:
		throw NotImplementedException("Unimplemented type for uncompressed segment");
	}
}

//===--------------------------------------------------------------------===//
// Update Append
//===--------------------------------------------------------------------===//
template <class T>
static void update_info_append(Transaction &transaction, UpdateInfo *info, idx_t row_id, Vector &result,
                               idx_t result_idx) {
	auto result_data = FlatVector::GetData<T>(result);
	auto &result_mask = FlatVector::Nullmask(result);
	UpdateInfo::UpdatesForTransaction(info, transaction, [&](UpdateInfo *current) {
		auto info_data = (T *)current->tuple_data;
		// loop over the tuples in this UpdateInfo
		for (idx_t i = 0; i < current->N; i++) {
			if (current->tuples[i] == row_id) {
				// found the relevant tuple
				result_data[result_idx] = info_data[i];
				result_mask[result_idx] = current->nullmask[current->tuples[i]];
				break;
			} else if (current->tuples[i] > row_id) {
				// tuples are sorted: so if the current tuple is > row_id we will not find it anymore
				break;
			}
		}
	});
}

static NumericSegment::update_info_append_function_t GetUpdateInfoAppendFunction(PhysicalType type) {
	switch (type) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		return update_info_append<int8_t>;
	case PhysicalType::INT16:
		return update_info_append<int16_t>;
	case PhysicalType::INT32:
		return update_info_append<int32_t>;
	case PhysicalType::INT64:
		return update_info_append<int64_t>;
	case PhysicalType::INT128:
		return update_info_append<hugeint_t>;
	case PhysicalType::FLOAT:
		return update_info_append<float>;
	case PhysicalType::DOUBLE:
		return update_info_append<double>;
	case PhysicalType::INTERVAL:
		return update_info_append<interval_t>;
	default:
		throw NotImplementedException("Unimplemented type for uncompressed segment");
	}
}

//===--------------------------------------------------------------------===//
// Rollback Update
//===--------------------------------------------------------------------===//
template <class T> static void rollback_update(UpdateInfo *info, data_ptr_t base) {
	auto &nullmask = *((nullmask_t *)base);
	auto info_data = (T *)info->tuple_data;
	auto base_data = (T *)(base + sizeof(nullmask_t));

	for (idx_t i = 0; i < info->N; i++) {
		base_data[info->tuples[i]] = info_data[i];
		nullmask[info->tuples[i]] = info->nullmask[info->tuples[i]];
	}
}

static NumericSegment::rollback_update_function_t GetRollbackUpdateFunction(PhysicalType type) {
	switch (type) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		return rollback_update<int8_t>;
	case PhysicalType::INT16:
		return rollback_update<int16_t>;
	case PhysicalType::INT32:
		return rollback_update<int32_t>;
	case PhysicalType::INT64:
		return rollback_update<int64_t>;
	case PhysicalType::INT128:
		return rollback_update<hugeint_t>;
	case PhysicalType::FLOAT:
		return rollback_update<float>;
	case PhysicalType::DOUBLE:
		return rollback_update<double>;
	case PhysicalType::INTERVAL:
		return rollback_update<interval_t>;
	default:
		throw NotImplementedException("Unimplemented type for uncompressed segment");
	}
}

} // namespace duckdb
