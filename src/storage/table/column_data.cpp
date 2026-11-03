#include "duckdb/storage/table/column_data.hpp"
#include "duckdb/main/attached_database.hpp"

#include "duckdb/common/exception/transaction_exception.hpp"
#include "duckdb/common/types/validity_mask.hpp"
#include "duckdb/common/operator/add.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/function/compression_function.hpp"
#include "duckdb/function/variant/variant_shredding.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/storage/data_pointer.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/storage/table/column_data_checkpointer.hpp"
#include "duckdb/storage/table/list_column_data.hpp"
#include "duckdb/storage/table/standard_column_data.hpp"
#include "duckdb/storage/table/array_column_data.hpp"
#include "duckdb/storage/table/geo_column_data.hpp"
#include "duckdb/storage/table/struct_column_data.hpp"
#include "duckdb/storage/table/variant_column_data.hpp"
#include "duckdb/storage/table/update_segment.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "duckdb/storage/statistics/array_stats.hpp"
#include "duckdb/storage/statistics/list_stats.hpp"
#include "duckdb/storage/statistics/struct_stats.hpp"
#include "duckdb/storage/table/data_table_info.hpp"
#include "duckdb/storage/table/append_state.hpp"
#include "duckdb/storage/table/scan_state.hpp"

namespace duckdb {

bool ColumnData::IsDirectNullCheckFilter(const TableFilter &filter) {
	auto &expr = ExpressionFilter::GetExpressionFilter(filter, "ColumnData::IsDirectNullCheckFilter").expr;
	if (expr->GetExpressionClass() != ExpressionClass::BOUND_OPERATOR) {
		return false;
	}
	auto &op = expr->Cast<BoundOperatorExpression>();
	if ((op.GetExpressionType() != ExpressionType::OPERATOR_IS_NULL &&
	     op.GetExpressionType() != ExpressionType::OPERATOR_IS_NOT_NULL) ||
	    op.GetChildren().size() != 1) {
		return false;
	}
	return op.GetChildren()[0]->GetExpressionClass() == ExpressionClass::BOUND_REF;
}

FilterPropagateResult ColumnData::CheckValidityZonemap(ColumnScanState &state, TableFilter &filter,
                                                       optional_ptr<SegmentNode<ColumnSegment>> &checked_segment,
                                                       ColumnData &validity_column) {
	if (!IsDirectNullCheckFilter(filter) || state.child_states.empty()) {
		checked_segment = nullptr;
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	return validity_column.CheckZonemap(state.child_states[0], filter, checked_segment);
}

ColumnData::ColumnData(BlockManager &block_manager, DataTableInfo &info, idx_t column_index, LogicalType type_p,
                       ColumnDataType data_type_p, optional_ptr<ColumnData> parent_p)
    : count(0), block_manager(block_manager), info(info), column_index(column_index), type(std::move(type_p)),
      allocation_size(0),
      data_type(data_type_p == ColumnDataType::CHECKPOINT_TARGET ? ColumnDataType::MAIN_TABLE : data_type_p),
      parent(parent_p) {
	if (!parent) {
		stats = make_uniq<SegmentStatistics>(type);
	}
}

ColumnData::~ColumnData() {
}

void ColumnData::SetDataType(ColumnDataType data_type_p) {
	this->data_type = data_type_p;
}

DatabaseInstance &ColumnData::GetDatabase() const {
	return info.GetDB().GetDatabase();
}

DataTableInfo &ColumnData::GetTableInfo() const {
	return info;
}

StorageManager &ColumnData::GetStorageManager() const {
	return info.GetDB().GetStorageManager();
}

bool ColumnData::HasUpdates() const {
	lock_guard<mutex> update_guard(update_lock);
	return updates.get();
}

bool ColumnData::HasChanges(idx_t start_row, idx_t end_row) const {
	lock_guard<mutex> update_guard(update_lock);
	if (!updates) {
		return false;
	}
	if (updates->HasUpdates(start_row, end_row)) {
		return true;
	}
	return false;
}

bool ColumnData::HasChanges() const {
	for (auto &segment_node : data.SegmentNodes()) {
		auto &segment = segment_node.GetNode();
		if (segment.GetSegmentType() == ColumnSegmentType::TRANSIENT) {
			// transient segment: always need to write to disk
			return true;
		}
		// persistent segment; check if there were any updates or deletions in this segment
		idx_t start_row_idx = segment_node.GetRowStart();
		idx_t end_row_idx = start_row_idx + segment.count;
		if (HasChanges(start_row_idx, end_row_idx)) {
			return true;
		}
	}
	return false;
}

bool ColumnData::HasAnyChanges() const {
	return HasChanges();
}

idx_t ColumnData::GetMaxEntry() {
	return count;
}

void ColumnData::InitializeScan(ColumnScanState &state) {
	state.current = data.GetRootSegment();
	state.segment_tree = &data;
	state.offset_in_column = state.current ? state.current->GetRowStart() : 0;
	state.internal_index = state.offset_in_column;
	state.initialized = false;
	state.scan_state.reset();
	state.last_offset = 0;
}

void ColumnData::InitializeScanWithOffset(ColumnScanState &state, idx_t row_idx) {
	if (row_idx > count) {
		throw InternalException("row_idx in InitializeScanWithOffset out of range");
	}
	state.current = data.GetSegment(row_idx);
	state.segment_tree = &data;
	state.offset_in_column = row_idx;
	state.internal_index = state.current->GetRowStart();
	state.initialized = false;
	state.scan_state.reset();
	state.last_offset = 0;
}

ScanVectorType ColumnData::GetVectorScanType(ColumnScanState &state, idx_t scan_count, Vector &result) {
	if (result.GetVectorType() != VectorType::FLAT_VECTOR) {
		return ScanVectorType::SCAN_ENTIRE_VECTOR;
	}
	if (HasUpdates()) {
		// if we have updates we need to merge in the updates
		// always need to scan flat vectors
		return ScanVectorType::SCAN_FLAT_VECTOR;
	}
	// check if the current segment has enough data remaining
	auto &current = state.current->GetNode();
	idx_t remaining_in_segment = state.current->GetRowStart() + current.count - state.offset_in_column;
	if (remaining_in_segment < scan_count) {
		// there is not enough data remaining in the current segment so we need to scan across segments
		// we need flat vectors here
		return ScanVectorType::SCAN_FLAT_VECTOR;
	}
	return ScanVectorType::SCAN_ENTIRE_VECTOR;
}

void ColumnData::InitializePrefetch(PrefetchState &prefetch_state, ColumnScanState &scan_state, idx_t remaining) {
	auto current_segment = scan_state.current;
	if (!current_segment) {
		return;
	}
	if (!scan_state.initialized) {
		// need to prefetch for the current segment if we have not yet initialized the scan for this segment
		current_segment->GetNode().InitializePrefetch(prefetch_state, scan_state);
	}
	idx_t row_index = scan_state.offset_in_column;
	while (remaining > 0) {
		auto &current = current_segment->GetNode();
		idx_t scan_count = MinValue<idx_t>(remaining, current_segment->GetRowStart() + current.count - row_index);
		remaining -= scan_count;
		row_index += scan_count;
		if (remaining > 0) {
			auto next = data.GetNextSegment(*current_segment);
			if (!next) {
				break;
			}
			next->GetNode().InitializePrefetch(prefetch_state, scan_state);
			current_segment = next;
		}
	}
}

void ColumnData::BeginScanVectorInternal(ColumnScanState &state) {
	D_ASSERT(state.current);

	state.previous_states.clear();
	if (!state.initialized) {
		auto &current = state.current->GetNode();
		current.InitializeScan(state);
		state.internal_index = state.current->GetRowStart();
		state.initialized = true;
	}
	D_ASSERT(data.HasSegment(*state.current));
	D_ASSERT(state.internal_index <= state.offset_in_column);
	if (state.internal_index < state.offset_in_column) {
		auto &current = state.current->GetNode();
		current.Skip(state);
	}
	D_ASSERT(state.current->GetNode().GetType() == type);
}

idx_t ColumnData::ScanVector(ColumnScanState &state, Vector &result, idx_t remaining, ScanVectorType scan_type,
                             idx_t base_result_offset) {
	if (scan_type == ScanVectorType::SCAN_FLAT_VECTOR && result.GetVectorType() != VectorType::FLAT_VECTOR) {
		throw InternalException("ScanVector called with SCAN_FLAT_VECTOR but result is not a flat vector");
	}
	BeginScanVectorInternal(state);
	idx_t initial_remaining = remaining;
	while (remaining > 0) {
		auto &current = state.current->GetNode();
		auto current_start = state.current->GetRowStart();
		D_ASSERT(state.offset_in_column >= current_start && state.offset_in_column <= current_start + current.count);
		idx_t scan_count = MinValue<idx_t>(remaining, current_start + current.count - state.offset_in_column);
		idx_t result_offset = base_result_offset + initial_remaining - remaining;
		if (scan_count > 0) {
			if (state.scan_options && state.scan_options->force_fetch_row) {
				FetchScannedRows(state, *FlatVector::IncrementalSelectionVector(), scan_count, result, result_offset);
			} else {
				current.Scan(state, scan_count, result, result_offset, scan_type);
			}

			state.offset_in_column += scan_count;
			remaining -= scan_count;
		}

		if (remaining > 0) {
			auto next = data.GetNextSegment(*state.current);
			if (!next) {
				break;
			}
			state.previous_states.emplace_back(std::move(state.scan_state));
			state.current = next;
			state.current->GetNode().InitializeScan(state);
			state.segment_checked = false;
			D_ASSERT(state.offset_in_column >= state.current->GetRowStart() &&
			         state.offset_in_column <= state.current->GetRowStart() + state.current->GetNode().count);
		}
	}
	state.internal_index = state.offset_in_column;
	return initial_remaining - remaining;
}

void ColumnData::SelectVector(ColumnScanState &state, Vector &result, idx_t target_count, const SelectionVector &sel,
                              idx_t sel_count, ScanVectorType scan_type, idx_t result_offset) {
	BeginScanVectorInternal(state);
	// scratch space to rebase the selection vector to the start of a segment scan
	// this owns its data, as compression functions can retain the selection vector (e.g. by slicing the result)
	SelectionVector rebased_sel;
	idx_t sel_offset = 0;
	idx_t scanned_count = 0;
	// the position up to which the scan state of the current segment has been advanced
	// segments without any selected rows are not scanned, so they must be skipped in the next scan
	idx_t segment_internal_index = state.offset_in_column;
	while (scanned_count < target_count) {
		auto &current = state.current->GetNode();
		auto current_start = state.current->GetRowStart();
		idx_t scan_count =
		    MinValue<idx_t>(target_count - scanned_count, current_start + current.count - state.offset_in_column);
		segment_internal_index = state.offset_in_column;
		// The final segment contains all remaining selected rows.
		idx_t segment_sel_count = sel_count - sel_offset;
		if (scanned_count + scan_count < target_count) {
			segment_sel_count = 0;
			while (sel_offset + segment_sel_count < sel_count &&
			       sel.get_index(sel_offset + segment_sel_count) < scanned_count + scan_count) {
				segment_sel_count++;
			}
		}
		if (segment_sel_count > 0) {
			// the indices must be relative to the start of the segment scan - for the first segment this is
			// already the case, so we can forward the selection vector of the caller directly
			reference<const SelectionVector> segment_sel(sel);
			if (scanned_count > 0) {
				if (!rebased_sel.IsSet()) {
					rebased_sel.Initialize();
				}
				for (idx_t i = 0; i < segment_sel_count; i++) {
					rebased_sel.set_index(i, sel.get_index(sel_offset + i) - scanned_count);
				}
				segment_sel = rebased_sel;
			}
			if (state.scan_options && state.scan_options->force_fetch_row) {
				FetchScannedRows(state, segment_sel.get(), segment_sel_count, result, result_offset + sel_offset);
			} else {
				current.Select(state, scan_count, result, segment_sel.get(), segment_sel_count,
				               result_offset + sel_offset, scan_type);
			}
			segment_internal_index = state.offset_in_column + scan_count;
		}
		sel_offset += segment_sel_count;
		scanned_count += scan_count;
		state.offset_in_column += scan_count;
		if (scanned_count < target_count) {
			auto next = data.GetNextSegment(*state.current);
			if (!next) {
				break;
			}
			state.previous_states.emplace_back(std::move(state.scan_state));
			state.current = next;
			state.current->GetNode().InitializeScan(state);
			state.segment_checked = false;
		}
	}
	if (scan_type == ScanVectorType::SCAN_FLAT_VECTOR) {
		// not every compression function sets the size of the result
		FlatVector::SetSize(result, count_t(result_offset + sel_count));
	}
	state.internal_index = segment_internal_index;
}

void ColumnData::FilterVector(ColumnScanState &state, Vector &result, idx_t target_count, SelectionVector &sel,
                              idx_t &sel_count, const TableFilter &filter, TableFilterState &filter_state) {
	BeginScanVectorInternal(state);
	auto &current = state.current->GetNode();
	if (state.current->GetRowStart() + current.count - state.offset_in_column < target_count) {
		throw InternalException("ColumnData::Filter should be able to fetch everything from one segment");
	}
	current.Filter(state, target_count, result, sel, sel_count, filter, filter_state);
	state.offset_in_column += target_count;
	state.internal_index = state.offset_in_column;
}

unique_ptr<BaseStatistics> ColumnData::GetUpdateStatistics() {
	lock_guard<mutex> update_guard(update_lock);
	return updates ? updates->GetStatistics() : nullptr;
}

void ColumnData::FetchUpdates(TransactionData transaction, idx_t vector_index, Vector &result, idx_t scan_count,
                              UpdateScanType update_type) {
	lock_guard<mutex> update_guard(update_lock);
	if (!updates) {
		return;
	}
	if (update_type == UpdateScanType::DISALLOW_UPDATES && updates->HasUncommittedUpdates(vector_index)) {
		throw TransactionException("Cannot create index with outstanding updates");
	}
	result.Flatten();
	updates->FetchUpdates(transaction, vector_index, result);
}

void ColumnData::FetchUpdateRow(TransactionData transaction, row_t row_id, Vector &result, idx_t result_idx) {
	lock_guard<mutex> update_guard(update_lock);
	if (!updates) {
		return;
	}
	const idx_t offset = NumericCast<idx_t>(row_id);
	updates->FetchRows(transaction, &offset, *FlatVector::IncrementalSelectionVector(), 1, result, result_idx);
}

void ColumnData::UpdateInternal(TransactionData transaction, DuckTableEntry &table_entry, idx_t column_index,
                                Vector &update_vector, row_t *row_ids, idx_t update_count, Vector &base_vector,
                                idx_t row_group_start) {
	lock_guard<mutex> update_guard(update_lock);
	if (!updates) {
		updates = make_uniq<UpdateSegment>(*this);
	}
	updates->Update(transaction, table_entry, column_index, update_vector, row_ids, update_count, base_vector,
	                row_group_start);
}

idx_t ColumnData::ScanVector(TransactionData transaction, idx_t vector_index, ColumnScanState &state, Vector &result,
                             idx_t target_scan, ScanVectorType scan_type, UpdateScanType update_type) {
	auto scan_count = ScanVector(state, result, target_scan, scan_type);
	if (scan_type != ScanVectorType::SCAN_ENTIRE_VECTOR) {
		// if we are scanning an entire vector we cannot have updates
		FetchUpdates(transaction, vector_index, result, scan_count, update_type);
	}
	return scan_count;
}

idx_t ColumnData::ScanVector(TransactionData transaction, idx_t vector_index, ColumnScanState &state, Vector &result,
                             idx_t target_scan, UpdateScanType update_type) {
	auto scan_type = GetVectorScanType(state, target_scan, result);
	return ScanVector(transaction, vector_index, state, result, target_scan, scan_type, update_type);
}

idx_t ColumnData::Scan(TransactionData transaction, idx_t vector_index, ColumnScanState &state, Vector &result) {
	auto target_count = GetVectorCount(vector_index);
	return Scan(transaction, vector_index, state, result, target_count);
}

idx_t ColumnData::Scan(TransactionData transaction, idx_t vector_index, ColumnScanState &state, Vector &result,
                       idx_t scan_count) {
	return ScanVector(transaction, vector_index, state, result, scan_count, state.update_scan_type);
}

idx_t ColumnData::GetVectorCount(idx_t vector_index) const {
	idx_t current_row = vector_index * STANDARD_VECTOR_SIZE;
	return MinValue<idx_t>(STANDARD_VECTOR_SIZE, count - current_row);
}

void ColumnData::ScanCommittedRange(idx_t row_group_start, idx_t offset_in_row_group, idx_t s_count, Vector &result) {
	ColumnScanState child_state(nullptr);
	InitializeScanWithOffset(child_state, offset_in_row_group);
	bool has_updates = HasUpdates();
	ScanVector(child_state, result, s_count, ScanVectorType::SCAN_FLAT_VECTOR);
	if (has_updates) {
		D_ASSERT(result.GetVectorType() == VectorType::FLAT_VECTOR);
		result.Flatten();
		updates->FetchCommittedRange(offset_in_row_group, s_count, result);
	}
}

idx_t ColumnData::ScanCount(ColumnScanState &state, Vector &result, idx_t scan_count, idx_t result_offset) {
	if (scan_count == 0) {
		return 0;
	}
	// ScanCount can only be used if there are no updates
	D_ASSERT(!HasUpdates());
	return ScanVector(state, result, scan_count, ScanVectorType::SCAN_FLAT_VECTOR, result_offset);
}

void ColumnData::Filter(TransactionData transaction, idx_t vector_index, ColumnScanState &state, Vector &result,
                        SelectionVector &sel, idx_t &s_count, const TableFilter &filter,
                        TableFilterState &filter_state) {
	idx_t scan_count = Scan(transaction, vector_index, state, result);
	FlatVector::SetSize(result, count_t(scan_count));

	ColumnSegment::FilterSelection(sel, result, filter_state, scan_count, s_count);
}

void ColumnData::Select(TransactionData transaction, idx_t vector_index, ColumnScanState &state, Vector &result,
                        SelectionVector &sel, idx_t s_count) {
	idx_t scan_count = Scan(transaction, vector_index, state, result);
	FlatVector::SetSize(result, count_t(scan_count));
	result.Slice(sel, s_count);
}

void ColumnData::Skip(ColumnScanState &state, idx_t s_count) {
	state.Next(s_count);
}

void ColumnData::Append(ColumnAppendState &state, const Vector &vector, idx_t append_count) {
	UnifiedVectorFormat vdata;
	vector.ToUnifiedFormat(vdata);
	AppendData(state, vdata, append_count);
}

void ColumnData::FinalizeAppend(ColumnDataFinalizeAppendState &finalize_state, ColumnAppendState &state) {
	// flush remaining stats
	state.FinalFlush(finalize_state.global_stats);
}

void ColumnData::FinalizeAppend(optional_ptr<BaseStatistics> table_stats, ColumnAppendState &state) {
	if (!stats) {
		throw InternalException("ColumnData::FinalizeAppend called on a column with a parent or without stats");
	}
	lock_guard<mutex> l(stats_lock);
	ColumnDataFinalizeAppendState finalize_state(stats->statistics);
	if (table_stats) {
		finalize_state.global_stats.emplace_back(*table_stats);
	}
	FinalizeAppend(finalize_state, state);
}

void ColumnData::FinalizeAppendLocked(ColumnDataFinalizeAppendState &finalize_state, ColumnAppendState &state) {
	lock_guard<mutex> l(stats_lock);
	FinalizeAppend(finalize_state, state);
}

FilterPropagateResult ColumnData::CheckZonemap(ColumnScanState &state, TableFilter &filter,
                                               optional_ptr<SegmentNode<ColumnSegment>> &checked_segment) {
	checked_segment = nullptr;
	if (state.segment_checked) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	if (!state.current) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	// for dynamic filters we never consider the segment being "checked" as it can always change
	auto &expr_filter = ExpressionFilter::GetExpressionFilter(filter, "ColumnData::CheckZonemap");
	bool is_dynamic = ExpressionFilter::ContainsInternalFunction(*expr_filter.expr, DynamicFilterScalarFun::NAME);
	state.segment_checked = !is_dynamic;
	checked_segment = IsDirectNullCheckFilter(filter) && !state.child_states.empty() && state.child_states[0].current
	                      ? state.child_states[0].current
	                      : state.current;
	auto prune_result = CheckSegmentStatistics(state.context.GetClientContext(), *checked_segment, expr_filter);
	if (prune_result == FilterPropagateResult::NO_PRUNING_POSSIBLE) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	auto update_stats = GetUpdateStatistics();
	if (!update_stats) {
		// no updates - return original result
		return prune_result;
	}
	// combine the update and original prune result
	auto context = state.context.GetClientContext();
	FilterPropagateResult update_result =
	    context ? expr_filter.CheckStatistics(*context, *update_stats) : expr_filter.CheckStatistics(*update_stats);
	if (prune_result == update_result) {
		return prune_result;
	}
	return FilterPropagateResult::NO_PRUNING_POSSIBLE;
}

FilterPropagateResult ColumnData::CheckSegmentStatistics(optional_ptr<ClientContext> context,
                                                         SegmentNode<ColumnSegment> &segment,
                                                         ExpressionFilter &expr_filter) {
	lock_guard<mutex> l(stats_lock);
	auto &segment_stats = segment.GetNode().GetStatsMutable();
	return context ? expr_filter.CheckStatistics(*context, segment_stats) : expr_filter.CheckStatistics(segment_stats);
}

idx_t ColumnData::ZonemapScanEnd(optional_ptr<ClientContext> context, idx_t start_row, idx_t end_row,
                                 TableFilter &filter) {
	if (!data.GetRootSegment() || IsDirectNullCheckFilter(filter)) {
		// columns without segments of their own have no zonemaps, null checks are judged on the validity child
		return end_row;
	}
	auto &expr_filter = ExpressionFilter::GetExpressionFilter(filter, "ColumnData::ZonemapScanEnd");
	auto update_stats = GetUpdateStatistics();
	if (update_stats) {
		auto update_result =
		    context ? expr_filter.CheckStatistics(*context, *update_stats) : expr_filter.CheckStatistics(*update_stats);
		if (update_result != FilterPropagateResult::FILTER_ALWAYS_FALSE) {
			// updated rows can pass the filter anywhere in the column
			return end_row;
		}
	}
	// a vector is only skipped when it lies entirely inside a rejected segment
	idx_t scan_end = start_row;
	for (auto segment = data.GetSegment(start_row); segment; segment = data.GetNextSegment(*segment)) {
		const idx_t segment_start = segment->GetRowStart();
		if (segment_start >= end_row) {
			break;
		}
		const idx_t segment_end = MinValue<idx_t>(segment_start + segment->GetNode().count, end_row);
		const auto prune_result = CheckSegmentStatistics(context, *segment, expr_filter);
		const bool straddles_vector = segment_end < end_row && segment_end % STANDARD_VECTOR_SIZE != 0;
		if (prune_result != FilterPropagateResult::FILTER_ALWAYS_FALSE || straddles_vector) {
			scan_end = AlignValue<idx_t, STANDARD_VECTOR_SIZE>(segment_end);
		}
	}
	return MinValue<idx_t>(scan_end, end_row);
}

FilterPropagateResult ColumnData::CheckZonemap(optional_ptr<ClientContext> context, const StorageIndex &index,
                                               TableFilter &filter) {
	if (!stats) {
		throw InternalException("ColumnData::CheckZonemap called on a column without stats");
	}
	lock_guard<mutex> l(stats_lock);
	if (index.IsPushdownExtract()) {
		auto child_stats = stats->statistics.PushdownExtract(index.GetChildIndex(0));
		if (!child_stats) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		auto &expr_filter = ExpressionFilter::GetExpressionFilter(filter, "ColumnData::CheckZonemap");
		return context ? expr_filter.CheckStatistics(*context, *child_stats)
		               : expr_filter.CheckStatistics(*child_stats);
	}
	auto &expr_filter = ExpressionFilter::GetExpressionFilter(filter, "ColumnData::CheckZonemap");
	return context ? expr_filter.CheckStatistics(*context, stats->statistics)
	               : expr_filter.CheckStatistics(stats->statistics);
}

const BaseStatistics &ColumnData::GetStatisticsRef() const {
	if (stats) {
		return stats->statistics;
	}
	D_ASSERT(HasParent());
	return parent->GetChildStats(*this);
}

const BaseStatistics &ColumnData::GetChildStats(const ColumnData &child) const {
	throw InternalException("GetChildStats not implemented for ColumnData of type %s", type.ToString());
}

unique_ptr<BaseStatistics> ColumnData::GetStatistics() const {
	if (!stats) {
		throw InternalException("ColumnData::GetStatistics called on a column without stats");
	}
	lock_guard<mutex> l(stats_lock);
	return stats->statistics.ToUnique();
}

void ColumnData::MergeStatistics(const BaseStatistics &other) {
	if (!stats) {
		throw InternalException("ColumnData::MergeStatistics called on a column without stats");
	}
	lock_guard<mutex> l(stats_lock);
	return stats->statistics.Merge(other);
}

void ColumnData::MergeIntoStatistics(BaseStatistics &other) {
	if (!stats) {
		throw InternalException("ColumnData::MergeIntoStatistics called on a column without stats");
	}
	lock_guard<mutex> l(stats_lock);
	return other.Merge(stats->statistics);
}

void ColumnAppendState::InitializeStats(const LogicalType &type) {
	append_stats = BaseStatistics::CreateEmpty(type).ToUnique();
}

void ColumnAppendState::FlushSegmentStats() {
	// we finished appending to this segment but we have another segment to append to
	// first flush the stats into the column segment
	// flush stats into the ColumnData and ColumnSegment stats
	auto &append_segment = current->GetNode();
	append_segment.GetStatsMutable().Merge(*append_stats);

	// now merge the stats into the "full_append_stats"
	if (!full_append_stats) {
		full_append_stats = std::move(append_stats);
	} else {
		full_append_stats->Merge(*append_stats);
	}
	// clear the current append stats
	append_stats.reset();
}

void ColumnAppendState::FinalFlush(vector<reference<BaseStatistics>> &global_stats) {
	// flush stats to the final segment
	FlushSegmentStats();
	// now merge the final append stats into the global stats
	for (auto &stats_ref : global_stats) {
		auto &target_stats = stats_ref.get();
		target_stats.Merge(*full_append_stats);
	}
}

ColumnDataFinalizeAppendState::ColumnDataFinalizeAppendState(ColumnDataFinalizeAppendState &parent,
                                                             LogicalTypeId type_transform, optional_idx child_offset) {
	for (auto &parent_stats_ref : parent.global_stats) {
		auto &parent_stats = parent_stats_ref.get();

		optional_ptr<BaseStatistics> transformed_stats;
		switch (type_transform) {
		case LogicalTypeId::ARRAY:
			transformed_stats = ArrayStats::GetChildStats(parent_stats);
			break;
		case LogicalTypeId::LIST:
			transformed_stats = ListStats::GetChildStats(parent_stats);
			break;
		case LogicalTypeId::STRUCT:
			transformed_stats = StructStats::GetChildStats(parent_stats, child_offset.GetIndex());
			break;
		case LogicalTypeId::VARIANT:
			transformed_stats = VariantStats::GetUnshreddedStats(parent_stats);
			break;
		default:
			throw InternalException("Unsupported type for ColumnDataFinalizeAppendState inheritance");
		}
		global_stats.emplace_back(*transformed_stats);
	}
}

void ColumnData::InitializeAppend(ColumnAppendState &state) {
	auto l = data.Lock();
	if (data.IsEmpty(l)) {
		// no segments yet, append an empty segment
		AppendTransientSegment(l, state.transient, nullptr);
	}
	auto segment = data.GetLastSegment(l);
	auto &last_segment = segment->GetNode();
	if (last_segment.GetSegmentType() == ColumnSegmentType::PERSISTENT ||
	    !last_segment.GetCompressionFunction().init_append) {
		// we cannot append to this segment - append a new segment
		AppendTransientSegment(l, state.transient, last_segment);
		state.current = data.GetLastSegment(l);
	} else {
		state.current = segment;
	}
	state.InitializeStats(GetType());
	auto &append_segment = state.current->GetNode();
	D_ASSERT(append_segment.GetSegmentType() == ColumnSegmentType::TRANSIENT);
	append_segment.InitializeAppend(state);
	D_ASSERT(append_segment.GetCompressionFunction().append);
}

void ColumnData::AppendData(ColumnAppendState &state, UnifiedVectorFormat &vdata, idx_t append_count) {
	idx_t offset = 0;
	while (true) {
		// append the data from the vector
		auto &append_segment = state.current->GetNode();
		idx_t copied_elements = append_segment.Append(state, vdata, offset, append_count);
		this->count += copied_elements;
		if (copied_elements == append_count) {
			// finished copying everything
			break;
		}
		// segment is full and we have more to copy
		// first flush the stats into the segment and the column data
		{
			lock_guard<mutex> guard(stats_lock);
			state.FlushSegmentStats();
		}
		// re-initialize the stats
		state.InitializeStats(GetType());

		// we couldn't fit everything we wanted in the current column segment, create a new one
		{
			auto l = data.Lock();
			AppendTransientSegment(l, state.transient, append_segment);
			state.current = data.GetLastSegment(l);
			state.current->GetNode().InitializeAppend(state);
		}
		offset += copied_elements;
		append_count -= copied_elements;
	}
}

void ColumnData::RevertAppend(row_t new_count_p) {
	idx_t new_count = NumericCast<idx_t>(new_count_p);
	auto l = data.Lock();
	// check if this row is in the segment tree at all
	auto last_segment_node = data.GetLastSegment(l);
	if (!last_segment_node) {
		return;
	}
	auto &last_segment = last_segment_node->GetNode();
	if (new_count >= last_segment_node->GetRowStart() + last_segment.count) {
		// the start row is equal to the final portion of the column data: nothing was ever appended here
		D_ASSERT(new_count == last_segment_node->GetRowStart() + last_segment.count);
		return;
	}
	// find the segment index that the current row belongs to
	idx_t segment_index = data.GetSegmentIndex(l, new_count);
	auto segment = data.GetSegmentByIndex(l, UnsafeNumericCast<int64_t>(segment_index));
	if (segment->GetRowStart() == new_count) {
		// we are truncating exactly this segment - erase it entirely
		data.EraseSegments(l, segment_index);
		if (segment_index > 0) {
			// if we have a previous segment, we need to update the next pointer
			auto previous_segment = data.GetSegmentByIndex(l, UnsafeNumericCast<int64_t>(segment_index - 1));
			previous_segment->SetNext(nullptr);
		}
	} else {
		// we need to truncate within the segment
		// remove any segments AFTER this segment: they should be deleted entirely
		data.EraseSegments(l, segment_index + 1);

		auto &transient = segment->GetNode();
		D_ASSERT(transient.GetSegmentType() == ColumnSegmentType::TRANSIENT);
		segment->SetNext(nullptr);
		transient.RevertAppend(new_count - segment->GetRowStart());
	}

	this->count = new_count;
}

idx_t ColumnData::Fetch(ColumnScanState &state, row_t row_id, Vector &result) {
	if (UnsafeNumericCast<idx_t>(row_id) > count) {
		throw InternalException("ColumnData::Fetch - row_id out of range");
	}
	D_ASSERT(row_id >= 0);
	// perform the fetch within the segment
	state.offset_in_column = UnsafeNumericCast<idx_t>(row_id) / STANDARD_VECTOR_SIZE * STANDARD_VECTOR_SIZE;
	state.current = data.GetSegment(state.offset_in_column);
	state.internal_index = state.current->GetRowStart();
	return ScanVector(state, result, STANDARD_VECTOR_SIZE, ScanVectorType::SCAN_FLAT_VECTOR);
}

void ColumnData::FetchRows(TransactionData transaction, ColumnFetchState &state, const StorageIndex &storage_index,
                           const idx_t *offsets, const SelectionVector &sel, idx_t fetch_count, Vector &result,
                           idx_t result_offset) {
	FetchRowsAtSegmentLevel(transaction, state, offsets, sel, fetch_count, result, result_offset);
}

#ifdef D_ASSERT_IS_ENABLED
//! Verify that no scanned string points into the segment's block, which is unpinned once the fetch is done
static void VerifyStringsCopied(ColumnSegment &segment, Vector &result, idx_t result_offset, idx_t count) {
	if (segment.GetType().InternalType() != PhysicalType::VARCHAR || !segment.GetBlockHandle()) {
		return;
	}
	auto &buffer_manager = segment.GetBlockHandle()->GetMemory().GetBufferManager();
	auto handle = buffer_manager.Pin(segment.GetBlockHandle());
	auto block_start = handle.Ptr();
	// transient segments can be smaller than a block, so bound by the pinned buffer rather than the block size
	auto block_end = block_start + handle.GetFileBuffer().Size();
	auto strings = FlatVector::GetData<string_t>(result);
	auto &validity = FlatVector::Validity(result);
	for (idx_t i = result_offset; i < result_offset + count; i++) {
		if (!validity.RowIsValid(i) || strings[i].IsInlined()) {
			continue;
		}
		auto str_ptr = const_data_ptr_cast(strings[i].GetData());
		if (str_ptr >= block_start && str_ptr < block_end) {
			throw InternalException("FetchRowsAtSegmentLevel: scanned string points into the segment's block, "
			                        "the compression function ignored StringScanType::COPY_STRINGS");
		}
	}
}
#endif

void ColumnData::FetchRowsAtSegmentLevel(TransactionData transaction, ColumnFetchState &state, const idx_t *offsets,
                                         const SelectionVector &sel, idx_t fetch_count, Vector &result,
                                         idx_t result_offset) {
	if (fetch_count == 0) {
		return;
	}
	FetchBaseRows(state.context, offsets, sel, fetch_count, result, result_offset);
	{
		const lock_guard<mutex> update_guard(update_lock);
		if (updates) {
			updates->FetchRows(transaction, offsets, sel, fetch_count, result, result_offset);
		}
	}
}

void ColumnData::FetchScannedRows(ColumnScanState &state, const SelectionVector &sel, idx_t count, Vector &result,
                                  idx_t result_offset) {
	vector<idx_t> offsets(count);
	for (idx_t i = 0; i < count; i++) {
		offsets[i] = state.offset_in_column + sel.get_index(i);
	}
	FetchBaseRows(state.context, offsets.data(), *FlatVector::IncrementalSelectionVector(), count, result,
	              result_offset);
}

void ColumnData::FetchBaseRows(const QueryContext &context, const idx_t *offsets, const SelectionVector &sel,
                               idx_t fetch_count, Vector &result, idx_t result_offset) {
	optional_ptr<SegmentNode<ColumnSegment>> current_segment;
	idx_t segment_start = 0;
	idx_t segment_end = 0;
	ColumnScanState scan_state(nullptr);
	scan_state.context = context;
	scan_state.string_scan_type = StringScanType::COPY_STRINGS;
	SelectionVector window_sel;
	idx_t idx = 0;
	while (idx < fetch_count) {
		const idx_t offset = offsets[sel.get_index(idx)];
		if (offset > count) {
			throw InternalException("ColumnData::FetchRowsAtSegmentLevel - row_id %lld out of range for count %lld",
			                        offset, count);
		}
		if (!current_segment || offset < segment_start || offset >= segment_end) {
			current_segment = data.GetSegment(offset);
			segment_start = current_segment->GetRowStart();
			segment_end = segment_start + current_segment->GetNode().count;
		}
		auto &segment = current_segment->GetNode();
		const idx_t window_end = MinValue<idx_t>(segment_end, offset + STANDARD_VECTOR_SIZE);
		idx_t window_count = 1;
		idx_t last_offset = offset;
		while (idx + window_count < fetch_count) {
			const idx_t next_offset = offsets[sel.get_index(idx + window_count)];
			if (next_offset <= last_offset || next_offset >= window_end) {
				break;
			}
			last_offset = next_offset;
			window_count++;
		}
		if (scan_state.current.get() == current_segment.get() && scan_state.offset_in_column <= offset) {
			scan_state.offset_in_column = offset;
		} else {
			ColumnData::InitializeScanWithOffset(scan_state, offset);
		}
		if (window_count > 1 && segment.GetCompressionFunction().select) {
			if (!window_sel.IsSet()) {
				window_sel.Initialize(STANDARD_VECTOR_SIZE);
			}
			for (idx_t i = 0; i < window_count; i++) {
				window_sel.set_index(i, offsets[sel.get_index(idx + i)] - offset);
			}
			SelectVector(scan_state, result, last_offset - offset + 1, window_sel, window_count,
			             ScanVectorType::SCAN_FLAT_VECTOR, result_offset + idx);
		} else {
			idx_t run_idx = 0;
			while (run_idx < window_count) {
				const idx_t run_offset = offsets[sel.get_index(idx + run_idx)];
				idx_t run_count = 1;
				while (run_idx + run_count < window_count &&
				       offsets[sel.get_index(idx + run_idx + run_count)] == run_offset + run_count) {
					run_count++;
				}
				scan_state.offset_in_column = run_offset;
				ScanVector(scan_state, result, run_count, ScanVectorType::SCAN_FLAT_VECTOR,
				           result_offset + idx + run_idx);
				run_idx += run_count;
			}
		}
#ifdef D_ASSERT_IS_ENABLED
		VerifyStringsCopied(segment, result, result_offset + idx, window_count);
#endif
		idx += window_count;
	}
}

idx_t ColumnData::FetchUpdateData(ColumnScanState &state, row_t *row_ids, Vector &base_vector, idx_t row_group_start) {
	if (row_ids[0] < UnsafeNumericCast<row_t>(row_group_start)) {
		throw InternalException("ColumnData::FetchUpdateData out of range");
	}
	auto fetch_count = ColumnData::Fetch(state, row_ids[0] - UnsafeNumericCast<row_t>(row_group_start), base_vector);
	base_vector.Flatten();
	return fetch_count;
}

void ColumnData::Update(TransactionData transaction, DuckTableEntry &table_entry, idx_t column_index,
                        Vector &update_vector, row_t *row_ids, idx_t update_count, idx_t row_group_start) {
	Vector base_vector(type);
	ColumnScanState state(nullptr);
	FetchUpdateData(state, row_ids, base_vector, row_group_start);

	UpdateInternal(transaction, table_entry, column_index, update_vector, row_ids, update_count, base_vector,
	               row_group_start);
}

void ColumnData::UpdateColumn(TransactionData transaction, DuckTableEntry &table_entry,
                              const vector<column_t> &column_path, Vector &update_vector, row_t *row_ids,
                              idx_t update_count, idx_t depth, idx_t row_group_start) {
	// this method should only be called at the end of the path in the base column case
	D_ASSERT(depth >= column_path.size());
	ColumnData::Update(transaction, table_entry, column_path[0], update_vector, row_ids, update_count, row_group_start);
}

void ColumnData::AppendTransientSegment(SegmentLock &l, optional_ptr<SuballocationBlock> transient,
                                        optional_ptr<ColumnSegment> prev_segment) {
	auto &db = GetDatabase();
	auto &config = DBConfig::GetConfig(db);
	const auto initial_bytes = Settings::Get<InitialColumnSegmentSizeSetting>(config);

	idx_t segment_size;
	if (!prev_segment || prev_segment->GetSegmentType() == ColumnSegmentType::PERSISTENT) {
		// We start with the `initial_bytes` setting, but we ensure that we have enough space for at least one row.
		segment_size = MaxValue<idx_t>(GetTypeIdSize(type.InternalType()), initial_bytes);
	} else {
		segment_size = prev_segment->SegmentSize() * 2;
	}

	// BIT (validity) segments can only hold rows in multiples of STANDARD_VECTOR_SIZE;
	// any segment below STANDARD_MASK_SIZE triggers a dead-segment overflow chain
	if (type.InternalType() == PhysicalType::BIT) {
		segment_size = MaxValue(segment_size, ValidityMask::STANDARD_MASK_SIZE);
	}

	// We set the segment size to the next power of two minus the block header size to
	// ensure that we have fixed-size segments which we can group when offloading to temporary storage.
	// FIXME: turn this into the min. temporary buffer size instead of a magical number,
	// FIXME: once we allow offloading tinier buffers.
	if (segment_size >= 1024) {
		const auto block_header_size = block_manager.GetBlockHeaderSize();
		segment_size = NextPowerOfTwo(segment_size) - block_header_size;
	}

	// The maximum segment size is always the block size of the corresponding block manager.
	const auto block_size = block_manager.GetBlockSize();
	segment_size = MinValue<idx_t>(block_size, segment_size);
	allocation_size += segment_size;

	auto function = config.GetCompressionFunction(CompressionType::COMPRESSION_UNCOMPRESSED, type.InternalType());
	unique_ptr<ColumnSegment> new_segment;
	if (transient && segment_size < block_size && initial_bytes < segment_size) {
		new_segment = transient->CreateTransientSegment(db, function, type, segment_size, block_manager);
	} else {
		new_segment = ColumnSegment::CreateTransientSegment(db, function, type, segment_size, block_manager);
	}
	AppendSegment(l, std::move(new_segment));
}

void ColumnData::UpdateCompressionFunction(SegmentLock &l, const CompressionFunction &function) {
	if (!compression) {
		// compression is empty...
		// if we have no segments - we have not set it yet, so assign it
		// if we have segments, the compression is mixed, so ignore it
		if (data.GetSegmentCount(l) == 0) {
			compression.set(function);
		}
	} else if (compression->type != function.type) {
		// we already have compression set - and we are adding a segment with a different compression
		// compression in the segment is mixed - clear the compression pointer
		compression.reset();
	}
}

void ColumnData::AppendSegment(SegmentLock &l, unique_ptr<ColumnSegment> segment) {
	UpdateCompressionFunction(l, segment->GetCompressionFunction());
	data.AppendSegment(l, std::move(segment));
}

void ColumnData::VisitBlockIds(BlockIdVisitor &visitor) const {
	for (auto &segment_p : data.Segments()) {
		auto &segment = segment_p;
		segment.VisitBlockIds(visitor);
	}
}

unique_ptr<ColumnCheckpointState> ColumnData::CreateCheckpointState(const RowGroup &row_group,
                                                                    PartialBlockManager &partial_block_manager) {
	return make_uniq<ColumnCheckpointState>(row_group, *this, partial_block_manager);
}

void ColumnData::CheckpointScan(ColumnSegment &segment, ColumnScanState &state, idx_t count,
                                Vector &scan_vector) const {
	segment.Scan(state, count, scan_vector, 0, ScanVectorType::SCAN_FLAT_VECTOR);

	if (updates) {
		D_ASSERT(scan_vector.GetVectorType() == VectorType::FLAT_VECTOR);
		updates->FetchCommittedRange(state.offset_in_column, count, scan_vector);
	}
}

unique_ptr<ColumnCheckpointState> ColumnData::Checkpoint(const RowGroup &row_group,
                                                         ColumnCheckpointInfo &checkpoint_info) {
	if (!stats) {
		throw InternalException("ColumnData::Checkpoint called without stats on a nested column");
	}
	return Checkpoint(row_group, checkpoint_info, this->stats->statistics);
}

unique_ptr<ColumnCheckpointState>
ColumnData::Checkpoint(const RowGroup &row_group, ColumnCheckpointInfo &checkpoint_info, const BaseStatistics &stats) {
	// scan the segments of the column data
	// set up the checkpoint state
	auto &partial_block_manager = checkpoint_info.GetPartialBlockManager();
	auto checkpoint_state = CreateCheckpointState(row_group, partial_block_manager);
	checkpoint_state->global_stats = BaseStatistics::CreateEmpty(type).ToUnique();

	if (!data.GetRootSegment()) {
		// empty table: flush the empty list
		return checkpoint_state;
	}

	vector<reference<ColumnCheckpointState>> states {*checkpoint_state};
	ColumnDataCheckpointer checkpointer(states, GetStorageManager(), row_group, checkpoint_info);
	checkpointer.Checkpoint();
	checkpointer.FinalizeCheckpoint();
	return checkpoint_state;
}

void ColumnData::InitializeColumn(PersistentColumnData &column_data) {
	InitializeColumn(column_data, stats->statistics);
}

void ColumnData::InitializeColumn(PersistentColumnData &column_data, BaseStatistics &target_stats) {
	if (type.InternalType() != column_data.logical_type.InternalType()) {
		throw DataCorruptionException("Column data of type %s found for a column of type %s", column_data.logical_type,
		                              type);
	}
	// construct the segments based on the data pointers
	this->count = 0;
	for (auto &data_pointer : column_data.pointers) {
		// Update the count and statistics
		data_pointer.row_start = count;
		idx_t new_count;
		if (!TryAddOperator::Operation<idx_t, idx_t, idx_t>(count.load(), data_pointer.tuple_count, new_count)) {
			throw DataCorruptionException("Column data has more rows than can be represented");
		}
		this->count = new_count;

		// Merge the statistics. If this is a child column, the target_stats reference will point into the parents stats
		// otherwise if this is a top level column, `stats->statistics` == `target_stats`

		target_stats.Merge(data_pointer.statistics);

		// create a persistent segment
		auto segment = ColumnSegment::CreatePersistentSegment(GetDatabase(), block_manager, data_pointer);

		auto l = data.Lock();
		AppendSegment(l, std::move(segment));
	}
}

bool ColumnData::IsPersistent() {
	for (auto &segment : data.Segments()) {
		if (segment.GetSegmentType() != ColumnSegmentType::PERSISTENT) {
			return false;
		}
	}
	return true;
}

vector<DataPointer> ColumnData::GetDataPointers() {
	vector<DataPointer> pointers;
	idx_t row_start = 0;
	for (auto &segment : data.Segments()) {
		pointers.push_back(segment.GetDataPointer(row_start));
		row_start += segment.count;
	}
	return pointers;
}

PersistentColumnData::PersistentColumnData(const LogicalType &logical_type_p) : logical_type(logical_type_p) {
}

PersistentColumnData::PersistentColumnData(const LogicalType &logical_type_p, vector<DataPointer> pointers_p)
    : logical_type(logical_type_p), pointers(std::move(pointers_p)) {
	D_ASSERT(!pointers.empty());
}

PersistentColumnData::~PersistentColumnData() {
}

void PersistentColumnData::Serialize(Serializer &serializer) const {
	if (has_updates) {
		throw InternalException("Column data with updates cannot be serialized");
	}

	// Serialize the extra data
	serializer.WritePropertyWithDefault(99, "extra_data", extra_data);

	// TODO: Dont special-case this
	if (logical_type.id() == LogicalTypeId::VARIANT) {
		serializer.WritePropertyWithDefault(100, "data_pointers", pointers);
		serializer.WriteProperty(101, "validity", child_columns[0]);
		serializer.WriteProperty(102, "unshredded", child_columns[1]);
		if (child_columns.size() > 2) {
			serializer.WriteProperty(103, "shredded", child_columns[2]);
		}
		return;
	}

	switch (logical_type.InternalType()) {
	case PhysicalType::BIT: {
		serializer.WritePropertyWithDefault(100, "data_pointers", pointers);
	} break;
	case PhysicalType::ARRAY:
	case PhysicalType::LIST: {
		D_ASSERT(child_columns.size() == 2);
		serializer.WritePropertyWithDefault(100, "data_pointers", pointers);
		serializer.WriteProperty(101, "validity", child_columns[0]);
		serializer.WriteProperty(102, "child_column", child_columns[1]);
	} break;
	case PhysicalType::STRUCT: {
		serializer.WritePropertyWithDefault(100, "data_pointers", pointers);
		serializer.WriteProperty(101, "validity", child_columns[0]);
		serializer.WriteList(102, "sub_columns", child_columns.size() - 1,
		                     [&](Serializer::List &list, idx_t i) { list.WriteElement(child_columns[i + 1]); });
	} break;
	default: {
		serializer.WritePropertyWithDefault(100, "data_pointers", pointers);
		serializer.WriteProperty(101, "validity", child_columns[0]);
	} break;
	}
}

void PersistentColumnData::DeserializeField(Deserializer &deserializer, field_id_t field_idx, const char *field_name,
                                            const LogicalType &type) {
	deserializer.Set<const LogicalType &>(type);
	child_columns.push_back(deserializer.ReadProperty<PersistentColumnData>(field_idx, field_name));
	deserializer.Unset<LogicalType>();
}

static PersistentColumnData GetPersistentColumnDataType(Deserializer &deserializer) {
	auto extra_data =
	    deserializer.ReadPropertyWithExplicitDefault<unique_ptr<ExtraPersistentColumnData>>(99, "extra_data", nullptr);

	if (!extra_data) {
		// Get the type from the parent scope
		auto &type = deserializer.Get<const LogicalType &>();
		return PersistentColumnData(type);
	}

	// Otherwise, the type of this segment may depend on extra data
	auto &column_type = deserializer.Get<const LogicalType &>();
	switch (extra_data->GetType()) {
	case ExtraPersistentColumnDataType::VARIANT: {
		if (column_type.id() != LogicalTypeId::VARIANT) {
			throw DataCorruptionException("VARIANT column data found for a column of type %s", column_type);
		}
		auto unshredded_type = VariantShredding::GetUnshreddedType();
		PersistentColumnData result(LogicalType::VARIANT());
		result.extra_data = std::move(extra_data);
		return result;
	}
	case ExtraPersistentColumnDataType::GEOMETRY: {
		if (column_type.id() != LogicalTypeId::GEOMETRY) {
			throw DataCorruptionException("GEOMETRY column data found for a column of type %s", column_type);
		}
		const auto &geometry_data = extra_data->Cast<GeometryPersistentColumnData>();
		// WKB is stored as the column's own type, which may carry type parameters (e.g., a CRS)
		PersistentColumnData result(geometry_data.storage_type == GeometryStorageType::WKB
		                                ? deserializer.Get<const LogicalType &>()
		                                : Geometry::GetVectorizedType(geometry_data.storage_type));
		result.extra_data = std::move(extra_data);
		return result;
	}
	default:
		throw InternalException("");
	}
}

PersistentColumnData PersistentColumnData::Deserialize(Deserializer &deserializer) {
	auto result = GetPersistentColumnDataType(deserializer);
	const auto &type = result.logical_type;

	// TODO: Dont special-case this
	if (type.id() == LogicalTypeId::VARIANT) {
		deserializer.ReadPropertyWithDefault(100, "data_pointers", result.pointers);
		result.DeserializeField(deserializer, 101, "validity", LogicalTypeId::VALIDITY);
		result.DeserializeField(deserializer, 102, "unshredded", VariantShredding::GetUnshreddedType());
		if (result.extra_data) {
			auto &variant_data = result.extra_data->Cast<VariantPersistentColumnData>();
			result.DeserializeField(deserializer, 103, "shredded", variant_data.logical_type);
		}
		return result;
	}

	// TODO: This is ugly
	if (result.extra_data && result.extra_data->GetType() == ExtraPersistentColumnDataType::GEOMETRY) {
		const auto &actual_type = type;

		// We need to set the actual type in scope, as when we deserialize "data_pointers" we use it to detect
		// the type of the statistics.
		deserializer.Set<const LogicalType &>(actual_type);

		switch (actual_type.InternalType()) {
		case PhysicalType::BIT: {
			deserializer.ReadPropertyWithDefault(100, "data_pointers", result.pointers);
		} break;
		case PhysicalType::ARRAY: {
			deserializer.ReadPropertyWithDefault(100, "data_pointers", result.pointers);
			result.DeserializeField(deserializer, 101, "validity", LogicalTypeId::VALIDITY);
			result.DeserializeField(deserializer, 102, "child_column", ArrayType::GetChildType(type));
		} break;
		case PhysicalType::LIST: {
			deserializer.ReadPropertyWithDefault(100, "data_pointers", result.pointers);
			result.DeserializeField(deserializer, 101, "validity", LogicalTypeId::VALIDITY);
			result.DeserializeField(deserializer, 102, "child_column", ListType::GetChildType(type));
		} break;
		case PhysicalType::STRUCT: {
			deserializer.ReadPropertyWithDefault(100, "data_pointers", result.pointers);
			result.DeserializeField(deserializer, 101, "validity", LogicalTypeId::VALIDITY);
			const auto &child_types = StructType::GetChildTypes(type);
			deserializer.ReadList(102, "sub_columns", [&](Deserializer::List &list, idx_t i) {
				deserializer.Set<const LogicalType &>(child_types[i].second);
				result.child_columns.push_back(list.ReadElement<PersistentColumnData>());
				deserializer.Unset<LogicalType>();
			});
		} break;
		default: {
			deserializer.ReadPropertyWithDefault(100, "data_pointers", result.pointers);
			result.DeserializeField(deserializer, 101, "validity", LogicalTypeId::VALIDITY);
		} break;
		}

		deserializer.Unset<LogicalType>();

		return result;
	}

	switch (type.InternalType()) {
	case PhysicalType::BIT: {
		deserializer.ReadPropertyWithDefault(100, "data_pointers", result.pointers);
	} break;
	case PhysicalType::ARRAY: {
		deserializer.ReadPropertyWithDefault(100, "data_pointers", result.pointers);
		result.DeserializeField(deserializer, 101, "validity", LogicalTypeId::VALIDITY);
		result.DeserializeField(deserializer, 102, "child_column", ArrayType::GetChildType(type));
	} break;
	case PhysicalType::LIST: {
		deserializer.ReadPropertyWithDefault(100, "data_pointers", result.pointers);
		result.DeserializeField(deserializer, 101, "validity", LogicalTypeId::VALIDITY);
		result.DeserializeField(deserializer, 102, "child_column", ListType::GetChildType(type));
	} break;
	case PhysicalType::STRUCT: {
		deserializer.ReadPropertyWithDefault(100, "data_pointers", result.pointers);
		result.DeserializeField(deserializer, 101, "validity", LogicalTypeId::VALIDITY);
		const auto &child_types = StructType::GetChildTypes(type);
		deserializer.ReadList(102, "sub_columns", [&](Deserializer::List &list, idx_t i) {
			deserializer.Set<const LogicalType &>(child_types[i].second);
			result.child_columns.push_back(list.ReadElement<PersistentColumnData>());
			deserializer.Unset<LogicalType>();
		});
	} break;
	default: {
		deserializer.ReadPropertyWithDefault(100, "data_pointers", result.pointers);
		result.DeserializeField(deserializer, 101, "validity", LogicalTypeId::VALIDITY);
	} break;
	}
	return result;
}

bool PersistentColumnData::HasUpdates() const {
	if (has_updates) {
		return true;
	}
	for (auto &child_col : child_columns) {
		if (child_col.HasUpdates()) {
			return true;
		}
	}
	return false;
}

PersistentRowGroupData::PersistentRowGroupData(vector<LogicalType> types_p) : types(std::move(types_p)) {
}

void PersistentRowGroupData::Serialize(Serializer &serializer) const {
	serializer.WriteProperty(100, "types", types);
	serializer.WriteProperty(101, "columns", column_data);
	serializer.WriteProperty(102, "start", start);
	serializer.WriteProperty(103, "count", count);
}

PersistentRowGroupData PersistentRowGroupData::Deserialize(Deserializer &deserializer) {
	PersistentRowGroupData data;
	deserializer.ReadProperty(100, "types", data.types);
	deserializer.ReadList(101, "columns", [&](Deserializer::List &list, idx_t i) {
		deserializer.Set<const LogicalType &>(data.types[i]);
		data.column_data.push_back(list.ReadElement<PersistentColumnData>());
		deserializer.Unset<LogicalType>();
	});
	deserializer.ReadProperty(102, "start", data.start);
	deserializer.ReadProperty(103, "count", data.count);
	return data;
}

bool PersistentRowGroupData::HasUpdates() const {
	for (auto &col : column_data) {
		if (col.HasUpdates()) {
			return true;
		}
	}
	return false;
}

void PersistentCollectionData::Serialize(Serializer &serializer) const {
	serializer.WriteProperty(100, "row_groups", row_group_data);
}

PersistentCollectionData PersistentCollectionData::Deserialize(Deserializer &deserializer) {
	PersistentCollectionData data;
	deserializer.ReadProperty(100, "row_groups", data.row_group_data);
	return data;
}

bool PersistentCollectionData::HasUpdates() const {
	for (auto &row_group : row_group_data) {
		if (row_group.HasUpdates()) {
			return true;
		}
	}
	return false;
}

static void TraverseBlocksRecursive(const PersistentColumnData &col_data, vector<block_id_t> &result) {
	for (auto &pointer : col_data.pointers) {
		auto block_id = pointer.block_pointer.block_id;
		if (block_id != INVALID_BLOCK) {
			result.push_back(block_id);
		}
		if (pointer.segment_state) {
			for (auto &block : pointer.segment_state->blocks) {
				result.push_back(block);
			}
		}
	}
	for (auto &child_column : col_data.child_columns) {
		TraverseBlocksRecursive(child_column, result);
	}
}
vector<block_id_t> PersistentCollectionData::GetBlockIds() const {
	vector<block_id_t> result;
	for (auto &group : row_group_data) {
		for (auto &col_data : group.column_data) {
			TraverseBlocksRecursive(col_data, result);
		}
	}
	return result;
}

void ExtraPersistentColumnData::Serialize(Serializer &serializer) const {
	serializer.WritePropertyWithDefault(100, "type", type, ExtraPersistentColumnDataType::INVALID);
	switch (GetType()) {
	case ExtraPersistentColumnDataType::VARIANT: {
		const auto &variant_data = Cast<VariantPersistentColumnData>();
		serializer.WriteProperty<LogicalType>(101, "storage_type", variant_data.logical_type);
	} break;
	case ExtraPersistentColumnDataType::GEOMETRY: {
		const auto &geometry_data = Cast<GeometryPersistentColumnData>();
		serializer.WritePropertyWithDefault(101, "storage_type", geometry_data.storage_type, GeometryStorageType::WKB);
	} break;
	default:
		throw InternalException("Unknown PersistentColumnData type");
	}
}
unique_ptr<ExtraPersistentColumnData> ExtraPersistentColumnData::Deserialize(Deserializer &deserializer) {
	auto type = deserializer.ReadPropertyWithExplicitDefault<ExtraPersistentColumnDataType>(
	    100, "type", ExtraPersistentColumnDataType::INVALID);
	switch (type) {
	case ExtraPersistentColumnDataType::VARIANT: {
		auto storage_type =
		    deserializer.ReadPropertyWithExplicitDefault<LogicalType>(101, "storage_type", LogicalType());
		return make_uniq<VariantPersistentColumnData>(storage_type);
	}
	case ExtraPersistentColumnDataType::GEOMETRY: {
		const auto storage_type = deserializer.ReadPropertyWithExplicitDefault<GeometryStorageType>(
		    101, "storage_type", GeometryStorageType::WKB);
		return make_uniq<GeometryPersistentColumnData>(storage_type);
	}
	default:
		throw InternalException("Unknown PersistentColumnData type");
	}
}

PersistentColumnData ColumnData::Serialize() {
	auto result = count ? PersistentColumnData(type, GetDataPointers()) : PersistentColumnData(type);
	result.has_updates = HasUpdates();
	return result;
}

shared_ptr<ColumnData> ColumnData::Deserialize(BlockManager &block_manager, DataTableInfo &info, idx_t column_index,
                                               ReadStream &source, const LogicalType &type) {
	auto entry = ColumnData::CreateColumn(block_manager, info, column_index, type);

	// deserialize the persistent column data
	BinaryDeserializer deserializer(source);
	deserializer.Begin();
	deserializer.Set<DatabaseInstance &>(info.GetDB().GetDatabase());
	CompressionInfo compression_info(block_manager);
	deserializer.Set<const CompressionInfo &>(compression_info);
	deserializer.Set<const LogicalType &>(type);

	auto &catalog = info.GetDB().GetStorageManager().GetAttached().GetCatalog();
	deserializer.Set<Catalog &>(catalog);

	auto persistent_column_data = PersistentColumnData::Deserialize(deserializer);

	deserializer.Unset<Catalog>();
	deserializer.Unset<LogicalType>();
	deserializer.Unset<const CompressionInfo>();
	deserializer.Unset<DatabaseInstance>();
	deserializer.End();

	// initialize the column
	entry->InitializeColumn(persistent_column_data, entry->stats->statistics);
	return entry;
}

struct ListBlockIds : public BlockIdVisitor {
	explicit ListBlockIds(vector<block_id_t> &block_ids) : block_ids(block_ids) {
	}

	void Visit(block_id_t block_id) override {
		block_ids.push_back(block_id);
	}

	vector<block_id_t> &block_ids;
};

void ColumnData::GetColumnSegmentInfo(const QueryContext &context, idx_t row_group_index, vector<idx_t> col_path,
                                      vector<ColumnSegmentInfo> &result, const ColumnSegmentInfoScanOptions &options) {
	D_ASSERT(!col_path.empty());

	// convert the column path to a string
	string col_path_str = "[";
	for (idx_t i = 0; i < col_path.size(); i++) {
		if (i > 0) {
			col_path_str += ", ";
		}
		col_path_str += to_string(col_path[i]);
	}
	col_path_str += "]";

	// iterate over the segments
	idx_t segment_idx = 0;
	for (auto &segment_node : data.SegmentNodes()) {
		auto &segment = segment_node.GetNode();
		ColumnSegmentInfo column_info;
		column_info.row_group_index = row_group_index;
		column_info.column_id = col_path[0];
		column_info.column_path = col_path_str;
		column_info.segment_idx = segment_idx;
		column_info.segment_type = type.ToString();
		column_info.segment_start = segment_node.GetRowStart();
		column_info.segment_count = segment.count;
		column_info.compression_type = CompressionTypeToString(segment.GetCompressionFunction().type);
		{
			lock_guard<mutex> l(stats_lock);
			column_info.segment_stats = segment.GetStats().ToStruct();
		}
		column_info.has_updates = ColumnData::HasUpdates();
		// persistent
		// block_id
		// block_offset
		if (segment.GetSegmentType() == ColumnSegmentType::PERSISTENT) {
			column_info.persistent = true;
			column_info.block_id = segment.GetBlockId();
			column_info.block_offset = segment.GetBlockOffset();
		} else {
			column_info.persistent = false;
			column_info.block_id = INVALID_BLOCK;
			column_info.block_offset = 0;
		}
		auto &compression_function = segment.GetCompressionFunction();
		auto segment_state = segment.GetSegmentState();
		if (segment_state) {
			column_info.segment_info = segment_state->GetSegmentInfo();
			if (compression_function.visit_block_ids) {
				ListBlockIds list_block_ids(column_info.additional_blocks);
				compression_function.visit_block_ids(segment, list_block_ids);
			}
		}
		// get_segment_info can be expensive (e.g. for bitpacked segments it reads each block to
		// gather per-segment compression mode counts). Skip it unless EXTENDED info is requested.
		if (options.include_segment_info && compression_function.get_segment_info) {
			auto segment_info = compression_function.get_segment_info(context, segment);
			vector<string> sinfo;
			for (auto &item : segment_info) {
				auto &mode = item.first;
				auto &count = item.second;
				sinfo.push_back(StringUtil::Format("%s: %s", mode, count));
			}
			column_info.segment_info = StringUtil::Join(sinfo, ", ");
		}
		result.emplace_back(column_info);

		segment_idx++;
	}
}

void ColumnData::Verify(RowGroup &parent) {
#ifdef DEBUG
	data.Verify();

	bool is_geometry_child_column = false;
	if (type.id() == LogicalTypeId::GEOMETRY && this->parent && this->parent->type.id() == LogicalTypeId::GEOMETRY) {
		// Geometry child column
		is_geometry_child_column = true;
	}

	if (type.InternalType() == PhysicalType::STRUCT || type.InternalType() == PhysicalType::ARRAY ||
	    (type.id() == LogicalTypeId::GEOMETRY && !is_geometry_child_column)) {
		// structs and fixed size lists don't have segments
		D_ASSERT(!data.GetRootSegment());
		return;
	}

	idx_t current_index = 0;
	idx_t current_start = 0;
	idx_t total_count = 0;
	for (auto &segment : data.SegmentNodes()) {
		D_ASSERT(segment.GetIndex() == current_index);
		D_ASSERT(segment.GetRowStart() == current_start);
		current_start += segment.GetNode().count;
		total_count += segment.GetNode().count;
		current_index++;
	}
	D_ASSERT(this->count == total_count);
#endif
}

shared_ptr<ColumnData> ColumnData::CreateColumn(BlockManager &block_manager, DataTableInfo &info, idx_t column_index,
                                                const LogicalType &type, ColumnDataType data_type,
                                                optional_ptr<ColumnData> parent) {
	if (type.id() == LogicalTypeId::GEOMETRY) {
		return make_shared_ptr<GeoColumnData>(block_manager, info, column_index, type, data_type, parent);
	}
	if (type.id() == LogicalTypeId::VARIANT) {
		return make_shared_ptr<VariantColumnData>(block_manager, info, column_index, type, data_type, parent);
	}
	if (type.InternalType() == PhysicalType::STRUCT) {
		return make_shared_ptr<StructColumnData>(block_manager, info, column_index, type, data_type, parent);
	}
	if (type.InternalType() == PhysicalType::LIST) {
		return make_shared_ptr<ListColumnData>(block_manager, info, column_index, type, data_type, parent);
	}
	if (type.InternalType() == PhysicalType::ARRAY) {
		return make_shared_ptr<ArrayColumnData>(block_manager, info, column_index, type, data_type, parent);
	}
	if (type.id() == LogicalTypeId::VALIDITY) {
		return make_shared_ptr<ValidityColumnData>(block_manager, info, column_index, data_type, parent);
	}
	return make_shared_ptr<StandardColumnData>(block_manager, info, column_index, type, data_type, parent);
}

} // namespace duckdb
