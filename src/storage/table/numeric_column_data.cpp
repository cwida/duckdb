#include "duckdb/storage/table/numeric_column_data.hpp"

#include "duckdb/common/exception/transaction_exception.hpp"
#include "duckdb/common/types/numeric_blob.hpp"
#include "duckdb/common/types/numeric_type.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/numeric_vector.hpp"
#include "duckdb/common/types/vector_cache.hpp"
#include "duckdb/common/vector/shredded_vector.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/function/scalar/generic_functions.hpp"
#include "duckdb/main/context.hpp"
#include "duckdb/planner/filter/dynamic_filter.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "duckdb/storage/statistics/string_stats.hpp"
#include "duckdb/storage/table/append_state.hpp"
#include "duckdb/storage/table/column_checkpoint_state.hpp"
#include "duckdb/storage/table/scan_state.hpp"

namespace duckdb {

NumericColumnData::NumericColumnData(BlockManager &block_manager, DataTableInfo &info, idx_t column_index,
                                     LogicalType type_p, ColumnDataType data_type, optional_ptr<ColumnData> parent)
    : ColumnData(block_manager, info, column_index, std::move(type_p), data_type, parent) {
	D_ASSERT(type.id() == LogicalTypeId::NUMERIC);
	if (data_type != ColumnDataType::CHECKPOINT_TARGET) {
		// an empty column is stored in the default representation - the first append widens it when needed
		inner = CreateInner(NumericType::DefaultType());
	}
}

shared_ptr<ColumnData> NumericColumnData::CreateInner(const LogicalType &storage_type) {
	return ColumnData::CreateColumn(block_manager, info, column_index, storage_type, GetDataType(), this);
}

shared_ptr<ColumnData> NumericColumnData::GetInner() const {
	lock_guard<mutex> guard(inner_lock);
	return inner;
}

LogicalType NumericColumnData::GetStorageType() const {
	return GetInner()->type;
}

NumericRange NumericColumnData::GetRange() const {
	lock_guard<mutex> guard(inner_lock);
	return range;
}

void NumericColumnData::SetInner(shared_ptr<ColumnData> inner_p) {
	lock_guard<mutex> guard(inner_lock);
	inner = std::move(inner_p);
	inner->SetParent(*this);
}

void NumericColumnData::SetRange(const NumericRange &range_p) {
	lock_guard<mutex> guard(inner_lock);
	range = range_p;
}

ColumnData &NumericColumnData::Pinned(ColumnScanState &state) {
	if (!state.pinned_column) {
		state.pinned_column = GetInner();
	}
	return *state.pinned_column;
}

//! Whether a column holds canonical values: the row group has no common representation
static bool IsCanonical(const ColumnData &column) {
	return NumericType::IsCanonicalStorage(column.type);
}

//! The canonical NUMERIC values of a stored vector: the blobs of canonical storage, otherwise encoded
static void ToCanonical(const Vector &stored, idx_t count, Vector &canonical) {
	D_ASSERT(canonical.GetType().id() == LogicalTypeId::NUMERIC);
	if (NumericType::IsCanonicalStorage(stored.GetType())) {
		canonical.Reinterpret(stored);
		return;
	}
	Vector typed(stored.GetType());
	typed.Reference(stored);
	NumericBlob::Encode(typed, count, canonical);
}

void NumericColumnData::CastVector(const Vector &source, Vector &target, idx_t count) {
	if (source.GetType() == target.GetType()) {
		target.Reference(source);
		return;
	}
	if (NumericType::IsCanonicalStorage(target.GetType())) {
		Vector canonical(LogicalType::NUMERIC, count);
		ToCanonical(source, count, canonical);
		target.Reinterpret(canonical);
		return;
	}
	Vector source_ref(source.GetType());
	source_ref.Reference(source);
	if (NumericType::IsCanonicalStorage(source.GetType())) {
		Vector canonical(LogicalType::NUMERIC);
		canonical.Reinterpret(source_ref);
		NumericBlob::Decode(canonical, count, target);
		return;
	}
	// the representations are chosen so that every value fits - a failure here is a bug
	VectorOperations::DefaultCast(source_ref, target, count, true);
}

//----------------------------------------------------------------------------------------------------------------------
// Scan
//----------------------------------------------------------------------------------------------------------------------
idx_t NumericColumnData::GetMaxEntry() {
	return GetInner()->GetMaxEntry();
}

void NumericColumnData::InitializePrefetch(PrefetchState &prefetch_state, ColumnScanState &scan_state, idx_t start_row,
                                           idx_t end_row) {
	Pinned(scan_state).InitializePrefetch(prefetch_state, scan_state, start_row, end_row);
}

void NumericColumnData::Pin(ColumnScanState &state) {
	// the representation and the range of the values it holds, taken together: every row the scan can see was
	// appended (and its range recorded) before the scan started
	lock_guard<mutex> guard(inner_lock);
	state.pinned_column = inner;
	range.TryGetUnscaled(inner->type, state.numeric_range);
}

void NumericColumnData::InitializeScan(ColumnScanState &state) {
	Pin(state);
	// the scan state was initialized for the NUMERIC type - re-initialize it for the storage type of this row group
	state.child_states.clear();
	state.Initialize(state.context, state.pinned_column->type, state.scan_options);
	state.pinned_column->InitializeScan(state);
}

void NumericColumnData::InitializeScanWithOffset(ColumnScanState &state, idx_t row_idx) {
	Pin(state);
	state.child_states.clear();
	state.Initialize(state.context, state.pinned_column->type, state.scan_options);
	state.pinned_column->InitializeScanWithOffset(state, row_idx);
}

//! Sizes a scanned vector: the row group scan only sizes the vectors of the chunk itself
static void SetScanSize(Vector &vector, idx_t count) {
	auto vector_type = vector.GetVectorType();
	if (vector_type == VectorType::FLAT_VECTOR || vector_type == VectorType::CONSTANT_VECTOR) {
		FlatVector::SetSize(vector, count_t(count));
	} else if (vector.size() != count) {
		throw InternalException("NumericColumnData::Scan - the representation has %d rows, expected %d", vector.size(),
		                        count);
	}
}

idx_t NumericColumnData::Scan(TransactionData transaction, idx_t vector_index, ColumnScanState &state, Vector &result,
                              idx_t target_count) {
	// the stored representation becomes the run-time representation of the result
	auto &column = Pinned(state);
	if (!state.numeric_cache || state.numeric_cache->GetType() != column.type) {
		state.numeric_cache = make_uniq<VectorCache>(Allocator::DefaultAllocator(), column.type);
	}
	Vector typed(*state.numeric_cache);
	auto scan_count = column.Scan(transaction, vector_index, state, typed, target_count);
	SetScanSize(typed, scan_count);
	if (IsCanonical(column)) {
		// no common representation: the canonical values themselves
		result.Reinterpret(typed);
		return scan_count;
	}
	result.Shred(typed, scan_count);
	if (state.numeric_range.valid) {
		NumericVector::SetRange(result, state.numeric_range);
	}
	return scan_count;
}

idx_t NumericColumnData::ScanCount(ColumnScanState &state, Vector &result, idx_t target_count, idx_t result_offset) {
	// partial scans fill a flat result (e.g. the child of a list) with canonical values
	auto &column = Pinned(state);
	Vector typed(column.type, target_count);
	auto scan_count = column.ScanCount(state, typed, target_count, 0);
	Vector canonical(LogicalType::NUMERIC, scan_count);
	ToCanonical(typed, scan_count, canonical);
	VectorOperations::Copy(canonical, result, scan_count, 0, result_offset);
	return scan_count;
}

void NumericColumnData::Skip(ColumnScanState &state, idx_t count) {
	Pinned(state).Skip(state, count);
}

//----------------------------------------------------------------------------------------------------------------------
// Zonemaps
//----------------------------------------------------------------------------------------------------------------------
//! The statistics of a representation described as canonical NUMERIC statistics; false when they are not exact
static bool TryCanonicalStats(const BaseStatistics &typed_stats, BaseStatistics &result) {
	NumericRange range;
	if (!range.TryUpdate(typed_stats)) {
		return false;
	}
	result = range.ToCanonical();
	result.CopyValidity(typed_stats);
	return true;
}

FilterPropagateResult NumericColumnData::CheckCanonicalStatistics(ColumnData &column,
                                                                  optional_ptr<ClientContext> context,
                                                                  SegmentNode<ColumnSegment> &segment,
                                                                  ExpressionFilter &expr_filter) {
	// the segment holds statistics of the representation - the filter compares canonical NUMERIC values
	auto segment_stats = column.GetSegmentStatistics(segment);
	auto canonical = BaseStatistics::CreateUnknown(LogicalType::NUMERIC);
	if (!TryCanonicalStats(*segment_stats, canonical)) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	auto prune_result =
	    context ? expr_filter.CheckStatistics(*context, canonical) : expr_filter.CheckStatistics(canonical);
	if (prune_result == FilterPropagateResult::NO_PRUNING_POSSIBLE) {
		return prune_result;
	}
	auto update_stats = column.GetUpdateStatistics();
	if (!update_stats) {
		return prune_result;
	}
	auto canonical_updates = BaseStatistics::CreateUnknown(LogicalType::NUMERIC);
	if (!TryCanonicalStats(*update_stats, canonical_updates)) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	auto update_result = context ? expr_filter.CheckStatistics(*context, canonical_updates)
	                             : expr_filter.CheckStatistics(canonical_updates);
	if (prune_result == update_result) {
		return prune_result;
	}
	return FilterPropagateResult::NO_PRUNING_POSSIBLE;
}

FilterPropagateResult NumericColumnData::CheckZonemap(ColumnScanState &state, const ScanFilter &filter,
                                                      optional_idx &checked_end) {
	if (filter.IsAlwaysTrue() || state.segment_checked || !state.current) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	auto &column = Pinned(state);
	auto &expr_filter = ExpressionFilter::GetExpressionFilter(filter.filter, "NumericColumnData::CheckZonemap");
	bool is_dynamic = ExpressionFilter::ContainsInternalFunction(*expr_filter.expr, DynamicFilterScalarFun::NAME);
	state.segment_checked = !is_dynamic;
	auto context = state.context.GetClientContext();
	if (IsDirectNullCheckFilter(filter.filter) && !state.child_states.empty() && state.child_states[0].current) {
		// NULL checks are judged on the validity segment, whose statistics do not depend on the representation
		auto &checked_segment = *state.child_states[0].current;
		checked_end = checked_segment.GetRowStart() + checked_segment.GetNode().count;
		auto validity_stats = column.GetSegmentStatistics(checked_segment);
		return context ? expr_filter.CheckStatistics(*context, *validity_stats)
		               : expr_filter.CheckStatistics(*validity_stats);
	}
	auto &checked_segment = *state.current;
	// segment rows are relative to the row group
	checked_end = checked_segment.GetRowStart() + checked_segment.GetNode().count;
	return CheckCanonicalStatistics(column, context, checked_segment, expr_filter);
}

FilterPropagateResult NumericColumnData::CheckZonemapAt(ColumnScanState &state, const ScanFilter &filter, idx_t row,
                                                        optional_idx &checked_end) {
	if (filter.IsAlwaysTrue() || IsDirectNullCheckFilter(filter.filter)) {
		// the filter needs no check in this row group, or the scan judges the null check on the validity child
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	auto &column = Pinned(state);
	auto &segments = column.GetSegmentTree();
	if (!segments.GetRootSegment()) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	auto segment = segments.GetSegment(row);
	auto &expr_filter = ExpressionFilter::GetExpressionFilter(filter.filter, "NumericColumnData::CheckZonemapAt");
	checked_end = segment->GetRowStart() + segment->GetNode().count;
	return CheckCanonicalStatistics(column, state.context.GetClientContext(), *segment, expr_filter);
}

//----------------------------------------------------------------------------------------------------------------------
// Append
//----------------------------------------------------------------------------------------------------------------------
static NumericRange &AppendRange(ColumnAppendState &state) {
	if (!state.numeric_range) {
		state.numeric_range = make_uniq<NumericRange>();
	}
	return *state.numeric_range;
}

void NumericColumnData::InitializeAppend(ColumnAppendState &state) {
	// appends are serialized by the row group - the inner column only changes inside Append
	GetInner()->InitializeAppend(state);
}

shared_ptr<ColumnData> NumericColumnData::Rebuild(ColumnData &source, const LogicalType &new_type) {
	auto new_column = CreateInner(new_type);
	idx_t total_count = source.count.load();
	if (total_count == 0) {
		return new_column;
	}
	ColumnScanState scan_state(nullptr);
	scan_state.Initialize(QueryContext(), source.type, nullptr);
	// committed updates are folded into the copy; uncommitted ones would be lost, so they abort the rebuild
	scan_state.update_scan_type = UpdateScanType::DISALLOW_UPDATES;
	source.InitializeScan(scan_state);

	ColumnAppendState append_state;
	new_column->InitializeAppend(append_state);

	DataChunk scan_chunk;
	scan_chunk.Initialize(Allocator::DefaultAllocator(), {source.type}, STANDARD_VECTOR_SIZE);
	DataChunk append_chunk;
	append_chunk.Initialize(Allocator::DefaultAllocator(), {new_type}, STANDARD_VECTOR_SIZE);

	idx_t vector_index = 0;
	for (idx_t scanned = 0; scanned < total_count; scanned += STANDARD_VECTOR_SIZE) {
		auto to_scan = MinValue<idx_t>(total_count - scanned, STANDARD_VECTOR_SIZE);
		scan_chunk.Reset();
		try {
			source.Scan(TransactionData::Unversioned(), vector_index++, scan_state, scan_chunk.data[0], to_scan);
		} catch (TransactionException &) {
			throw TransactionException("Cannot change the storage type of a NUMERIC column with outstanding updates - "
			                           "retry after the updating transaction has finished");
		}
		append_chunk.Reset();
		CastVector(scan_chunk.data[0], append_chunk.data[0], to_scan);
		new_column->Append(append_state, append_chunk.data[0], to_scan);
	}
	// flush the segment statistics - the column statistics are maintained by the caller
	auto rebuild_stats = BaseStatistics::CreateEmpty(new_type);
	ColumnDataFinalizeAppendState finalize_state(rebuild_stats);
	new_column->FinalizeAppend(finalize_state, append_state);
	return new_column;
}

void NumericColumnData::Widen(ColumnAppendState &state, const LogicalType &new_type) {
	auto current = GetInner();
	auto new_inner = Rebuild(*current, new_type);
	{
		lock_guard<mutex> guard(inner_lock);
		retired.push_back(std::move(inner));
		inner = new_inner;
	}
	// continue the append in the new column - the typed append state is re-initialized from scratch, the exact range
	// of the rows appended so far lives in the append state, and so does their validity from here on
	auto carry_validity = [&](optional_ptr<BaseStatistics> stats) {
		if (!stats) {
			return;
		}
		if (stats->CanHaveNull()) {
			state.numeric_widened_has_null = true;
		}
		if (stats->CanHaveNoNull()) {
			state.numeric_widened_has_no_null = true;
		}
	};
	carry_validity(state.append_stats.get());
	carry_validity(state.full_append_stats.get());
	state.current = nullptr;
	state.child_appends.clear();
	state.append_state.reset();
	state.append_stats.reset();
	state.full_append_stats.reset();
	new_inner->InitializeAppend(state);
}

LogicalType NumericColumnData::AppendTargetType(ColumnAppendState &state, const LogicalType &current_type,
                                                const LogicalType &source_type) {
	if (NumericType::IsCanonicalStorage(current_type)) {
		return current_type;
	}
	LogicalType target_type;
	if (NumericType::TryWiden(current_type, source_type, target_type)) {
		return target_type;
	}
	// no common representation with the current one: the one of the values stored so far, when they have one
	auto stored = GetRange();
	stored.Merge(AppendRange(state));
	if (!stored.HasValues()) {
		return source_type;
	}
	auto stored_type = stored.ResolveType();
	if (NumericType::IsCanonicalStorage(stored_type) || !NumericType::TryWiden(stored_type, source_type, target_type)) {
		return NumericType::CanonicalStorageType();
	}
	return target_type;
}

void NumericColumnData::AppendTyped(ColumnAppendState &state, const Vector &vector, idx_t append_count) {
	auto current = GetInner();
	auto &source_type = vector.GetType();
	auto target_type = AppendTargetType(state, current->type, source_type);
	if (target_type != current->type) {
		Widen(state, target_type);
		current = GetInner();
	}
	if (source_type == current->type) {
		current->Append(state, vector, append_count);
		AppendRange(state).Update(vector, append_count);
	} else {
		Vector cast_vector(current->type, append_count);
		CastVector(vector, cast_vector, append_count);
		current->Append(state, cast_vector, append_count);
		// the range is taken from the values in the narrower of the two representations
		AppendRange(state).Update(IsCanonical(*current) ? vector : cast_vector, append_count);
	}
	count += append_count;
}

void NumericColumnData::AppendCanonical(ColumnAppendState &state, const Vector &canonical, idx_t append_count) {
	auto current = GetInner();
	if (!IsCanonical(*current)) {
		Widen(state, NumericType::CanonicalStorageType());
		current = GetInner();
	}
	Vector blobs(current->type);
	blobs.Reinterpret(canonical);
	current->Append(state, blobs, append_count);
	AppendRange(state).UpdateCanonical(canonical, append_count);
	count += append_count;
}

void NumericColumnData::Append(ColumnAppendState &state, const Vector &vector, idx_t append_count) {
	auto &source_type = vector.GetType();
	if (source_type.id() == LogicalTypeId::NUMERIC) {
		if (vector.GetVectorType() == VectorType::SHREDDED_VECTOR) {
			// the run-time representation of the values
			AppendTyped(state, ShreddedVector::GetShreddedVector(vector), append_count);
			return;
		}
		// canonical values: decode into the representation that holds them, if there is one
		Vector canonical(LogicalType::NUMERIC);
		canonical.Reference(vector);
		LogicalType type;
		if (!NumericBlob::TryRangeType(canonical, append_count, type)) {
			AppendCanonical(state, canonical, append_count);
			return;
		}
		Vector typed(type, append_count);
		NumericBlob::Decode(canonical, append_count, typed);
		AppendTyped(state, typed, append_count);
		return;
	}
	if (source_type.id() == LogicalTypeId::SQLNULL) {
		// untyped NULLs (e.g. from a NULL parameter) are stored in the current representation
		auto current = GetInner();
		Vector null_vector(Value(current->type), count_t(append_count));
		current->Append(state, null_vector, append_count);
		AppendRange(state);
		count += append_count;
		return;
	}
	if (!NumericType::IsExact(source_type)) {
		throw InternalException("NUMERIC column cannot store values of type %s", source_type.ToString());
	}
	AppendTyped(state, vector, append_count);
}

void NumericColumnData::FinalizeAppend(ColumnDataFinalizeAppendState &finalize_state, ColumnAppendState &state) {
	// the inner column flushes its segment statistics; the column and table statistics take the canonical statistics
	// of the exact range of the append, which covers every representation the append went through
	auto current = GetInner();
	auto inner_stats = BaseStatistics::CreateEmpty(current->type);
	ColumnDataFinalizeAppendState inner_finalize(inner_stats);
	current->FinalizeAppend(inner_finalize, state);
	auto &appended = AppendRange(state);
	{
		lock_guard<mutex> guard(inner_lock);
		range.Merge(appended);
	}
	auto canonical = appended.ToCanonical();
	canonical.CopyValidity(inner_stats);
	// the rows appended before the representation was widened are not in the inner column's statistics
	if (state.numeric_widened_has_null) {
		canonical.SetHasNull();
	}
	if (state.numeric_widened_has_no_null) {
		canonical.SetHasNoNull();
	}
	for (auto &stats_ref : finalize_state.global_stats) {
		stats_ref.get().Merge(canonical);
	}
}

void NumericColumnData::RevertAppend(row_t new_count) {
	GetInner()->RevertAppend(new_count);
	count = UnsafeNumericCast<idx_t>(new_count);
}

//----------------------------------------------------------------------------------------------------------------------
// Fetch
//----------------------------------------------------------------------------------------------------------------------
void NumericColumnData::FetchRows(TransactionData transaction, ColumnFetchState &state,
                                  const StorageIndex &storage_index, const idx_t *offsets, const SelectionVector &sel,
                                  idx_t fetch_count, Vector &result, idx_t result_offset) {
	auto column = GetInner();
	Vector typed(column->type, fetch_count);
	column->FetchRows(transaction, state, storage_index, offsets, sel, fetch_count, typed, 0);
	Vector canonical(LogicalType::NUMERIC, fetch_count);
	ToCanonical(typed, fetch_count, canonical);
	VectorOperations::Copy(canonical, result, fetch_count, 0, result_offset);
}

//----------------------------------------------------------------------------------------------------------------------
// Update (updates of NUMERIC columns are executed as a delete and an insert, so these are not reached)
//----------------------------------------------------------------------------------------------------------------------
void NumericColumnData::Update(TransactionData transaction, DuckTableEntry &table_entry, idx_t column_index_p,
                               Vector &update_vector, row_t *row_ids, idx_t update_count, idx_t row_group_start) {
	throw NotImplementedException("In-place updates of NUMERIC columns are not supported");
}

void NumericColumnData::UpdateColumn(TransactionData transaction, DuckTableEntry &table_entry,
                                     const vector<column_t> &column_path, Vector &update_vector, row_t *row_ids,
                                     idx_t update_count, idx_t depth, idx_t row_group_start) {
	throw NotImplementedException("In-place updates of NUMERIC columns are not supported");
}

unique_ptr<BaseStatistics> NumericColumnData::GetUpdateStatistics() {
	return GetInner()->GetUpdateStatistics();
}

//----------------------------------------------------------------------------------------------------------------------
// Checkpoint
//----------------------------------------------------------------------------------------------------------------------
class NumericColumnCheckpointState final : public ColumnCheckpointState {
public:
	NumericColumnCheckpointState(const RowGroup &row_group, ColumnData &column_data,
	                             PartialBlockManager &partial_block_manager)
	    : ColumnCheckpointState(row_group, column_data, partial_block_manager) {
		global_stats = BaseStatistics::CreateEmpty(LogicalType::NUMERIC).ToUnique();
	}

	//! The (possibly rebuilt) inner column that was checkpointed and its checkpoint state
	shared_ptr<ColumnData> inner_column;
	unique_ptr<ColumnCheckpointState> inner_state;
	//! The exact range of the row group, persisted with its representation
	NumericRange range;

	shared_ptr<ColumnData> CreateEmptyColumnData() override {
		return make_shared_ptr<NumericColumnData>(original_column.GetBlockManager(), original_column.GetTableInfo(),
		                                          original_column.column_index, original_column.type,
		                                          ColumnDataType::CHECKPOINT_TARGET, nullptr);
	}

	shared_ptr<ColumnData> GetFinalResult() override {
		if (!result_column) {
			result_column = CreateEmptyColumnData();
		}
		auto &column_data = result_column->Cast<NumericColumnData>();
		column_data.SetInner(inner_state->GetFinalResult());
		column_data.SetRange(range);
		return ColumnCheckpointState::GetFinalResult();
	}

	unique_ptr<BaseStatistics> GetStatistics() override {
		D_ASSERT(global_stats);
		return std::move(global_stats);
	}

	PersistentColumnData ToPersistentData() override {
		auto inner_data = inner_state->ToPersistentData();
		inner_data.extra_data = make_uniq<NumericPersistentColumnData>(inner_column->type, range);
		return inner_data;
	}
};

unique_ptr<ColumnCheckpointState> NumericColumnData::CreateCheckpointState(const RowGroup &row_group,
                                                                           PartialBlockManager &partial_block_manager) {
	return make_uniq<NumericColumnCheckpointState>(row_group, *this, partial_block_manager);
}

unique_ptr<ColumnCheckpointState> NumericColumnData::Checkpoint(const RowGroup &row_group, ColumnCheckpointInfo &info,
                                                                const BaseStatistics &old_stats) {
	auto checkpoint_state = make_uniq<NumericColumnCheckpointState>(row_group, *this, info.GetPartialBlockManager());
	auto current = GetInner();
	auto target_column = current;
	auto current_range = GetRange();
	if (HasAnyChanges() || !IsPersistent()) {
		// store the row group in the narrowest representation that fits its values
		auto target_type = current_range.ResolveType();
		if (target_type != current->type) {
			target_column = Rebuild(*current, target_type);
		}
	}
	// the inner column expects statistics of its own representation
	auto typed_old_stats = current_range.ToTyped(target_column->type);
	typed_old_stats.CopyValidity(old_stats);
	checkpoint_state->inner_column = target_column;
	checkpoint_state->inner_state = target_column->Checkpoint(row_group, info, typed_old_stats);
	auto inner_stats = checkpoint_state->inner_state->GetStatistics();
	checkpoint_state->global_stats = current_range.ToCanonical().ToUnique();
	checkpoint_state->global_stats->CopyValidity(*inner_stats);
	checkpoint_state->range = current_range;
	return std::move(checkpoint_state);
}

bool NumericColumnData::IsPersistent() {
	return GetInner()->IsPersistent();
}

bool NumericColumnData::HasAnyChanges() const {
	return GetInner()->HasAnyChanges();
}

PersistentColumnData NumericColumnData::Serialize() {
	auto column = GetInner();
	auto inner_data = column->Serialize();
	inner_data.extra_data = make_uniq<NumericPersistentColumnData>(column->type, GetRange());
	return inner_data;
}

void NumericColumnData::InitializeColumn(PersistentColumnData &column_data, BaseStatistics &target_stats) {
	if (!column_data.extra_data || column_data.extra_data->GetType() != ExtraPersistentColumnDataType::NUMERIC) {
		throw DataCorruptionException("NUMERIC column data without a storage type");
	}
	auto &numeric_data = column_data.extra_data->Cast<NumericPersistentColumnData>();
	auto new_inner = CreateInner(numeric_data.storage_type);
	auto typed_stats = BaseStatistics::CreateEmpty(numeric_data.storage_type);
	new_inner->InitializeColumn(column_data, typed_stats);
	{
		lock_guard<mutex> guard(inner_lock);
		inner = std::move(new_inner);
		range = numeric_data.range;
	}
	count = inner->count.load();
	// the column statistics describe the exact range written with the row group
	auto canonical = numeric_data.range.ToCanonical();
	canonical.CopyValidity(typed_stats);
	target_stats.Merge(canonical);
}

//----------------------------------------------------------------------------------------------------------------------
// Misc
//----------------------------------------------------------------------------------------------------------------------
void NumericColumnData::GetColumnSegmentInfo(const QueryContext &context, idx_t row_group_index, vector<idx_t> col_path,
                                             vector<ColumnSegmentInfo> &result,
                                             const ColumnSegmentInfoScanOptions &options) {
	GetInner()->GetColumnSegmentInfo(context, row_group_index, std::move(col_path), result, options);
}

void NumericColumnData::Verify(RowGroup &parent) {
	GetInner()->Verify(parent);
}

void NumericColumnData::VisitBlockIds(BlockIdVisitor &visitor) const {
	GetInner()->VisitBlockIds(visitor);
}

} // namespace duckdb
