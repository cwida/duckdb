#include "duckdb/storage/table/scan_state.hpp"

#include "duckdb/execution/adaptive_filter.hpp"
#include "duckdb/parallel/async_result.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/storage/table/column_data.hpp"
#include "duckdb/storage/table/column_segment.hpp"
#include "duckdb/storage/table/row_group.hpp"
#include "duckdb/storage/table/row_group_collection.hpp"
#include "duckdb/storage/table/row_group_segment_tree.hpp"
#include "duckdb/common/numeric_utils.hpp"

namespace duckdb {

TableScanState::TableScanState() : table_state(*this), local_state(*this) {
}

TableScanState::~TableScanState() {
}

void TableScanState::Initialize(vector<StorageIndex> column_ids_p, optional_ptr<ClientContext> context,
                                optional_ptr<TableFilterSet> table_filters, optional_ptr<SampleOptions> table_sampling,
                                idx_t estimated_table_row_count) {
	this->column_ids = std::move(column_ids_p);
	if (table_filters) {
		filters.Initialize(*context, *table_filters, column_ids);
	}
	if (table_sampling) {
		sampling_info.do_system_sample = table_sampling->method == SampleMethod::SYSTEM_SAMPLE;
		if (table_sampling->is_percentage) {
			// Percentage-based system sampling
			sampling_info.is_percentage = true;
			sampling_info.sample_rate = table_sampling->sample_size.GetValue<double>() / 100.0;
		} else {
			// Row-count based system sampling: convert target row count to approximate rate.
			// Prefer the pre-calculated sample_rate from the optimizer if available,
			// otherwise derive from estimated_table_row_count.
			sampling_info.is_percentage = false;
			sampling_info.target_sample_rows = NumericCast<idx_t>(table_sampling->sample_size.GetValue<int64_t>());
			if (table_sampling->sample_rate > 0) {
				sampling_info.sample_rate = table_sampling->sample_rate;
			} else if (estimated_table_row_count > 0) {
				sampling_info.sample_rate = static_cast<double>(sampling_info.target_sample_rows) /
				                            static_cast<double>(estimated_table_row_count);
			} else {
				// No estimate available, use a conservative rate
				sampling_info.sample_rate = 1.0;
			}
			sampling_info.sample_rate = MinValue(1.0, MaxValue(0.0, sampling_info.sample_rate));
			RandomEngine random(table_sampling->seed.IsValid() ? static_cast<int64_t>(table_sampling->seed.GetIndex())
			                                                   : -1);
			sampling_info.sample_phase = random.NextRandom();
		}
		if (table_sampling->seed.IsValid()) {
			table_state.GetRandomEngine().SetSeed(table_sampling->seed.GetIndex());
		}
	}
}

const vector<StorageIndex> &TableScanState::GetColumnIds() {
	D_ASSERT(!column_ids.empty());
	return column_ids;
}

ScanFilterInfo::~ScanFilterInfo() {
}

ScanFilterInfo &TableScanState::GetFilterInfo() {
	return filters;
}

ScanSamplingInfo &TableScanState::GetSamplingInfo() {
	return sampling_info;
}

idx_t TableScanState::RowsScanned() const {
	return table_state.rows_scanned + local_state.rows_scanned;
}

ScanFilter::ScanFilter(ClientContext &context, ProjectionIndex index, const vector<StorageIndex> &column_ids,
                       TableFilter &filter)
    : scan_column_index(index), table_column_index(column_ids[index]), filter(filter), always_true(false) {
	filter_state = TableFilterState::Initialize(context, filter);
}

void ScanFilterInfo::Initialize(ClientContext &context, TableFilterSet &filters,
                                const vector<StorageIndex> &column_ids) {
	D_ASSERT(filters.HasFilters() || filters.HasMultiColumnFilters());
	table_filters = &filters;
	this->column_ids = &column_ids;
	if (filters.HasFilters()) {
		adaptive_filter = make_uniq<AdaptiveFilter>(filters);
		adaptive_filter->SetLogger(context.logger);
	}
	filter_list.reserve(filters.FilterCount());
	for (auto &entry : filters) {
		filter_list.emplace_back(context, entry.GetIndex(), column_ids, entry.Filter());
	}
	column_has_filter.reserve(column_ids.size());
	for (auto col_idx : ProjectionIndex::GetIndexes(column_ids.size())) {
		bool has_filter = table_filters->HasFilter(col_idx);
		column_has_filter.push_back(has_filter);
	}
	base_column_has_filter = column_has_filter;
}

bool ScanFilterInfo::ColumnHasFilters(idx_t column_idx) {
	if (column_idx < column_has_filter.size()) {
		return column_has_filter[column_idx];
	} else {
		return false;
	}
}

bool ScanFilterInfo::HasFilters() const {
	if (!table_filters) {
		// no filters
		return false;
	}
	// if we have filters - check if we need to check any of them
	return always_true_filters < filter_list.size();
}

void ScanFilterInfo::CheckAllFilters() {
	always_true_filters = 0;
	// reset the "column_has_filter" bitmask to the original
	for (idx_t col_idx = 0; col_idx < column_has_filter.size(); col_idx++) {
		column_has_filter[col_idx] = base_column_has_filter[col_idx];
	}
	// set "always_true" in the individual filters to false
	for (auto &filter : filter_list) {
		filter.always_true = false;
	}
}

void ScanFilterInfo::SetFilterAlwaysTrue(idx_t filter_idx) {
	auto &filter = filter_list[filter_idx];
	if (filter.always_true) {
		return;
	}
	filter.always_true = true;
	column_has_filter[filter.scan_column_index] = false;
	always_true_filters++;
}

optional_ptr<AdaptiveFilter> ScanFilterInfo::GetAdaptiveFilter() {
	return adaptive_filter.get();
}

AdaptiveFilterState ScanFilterInfo::BeginFilter() const {
	if (!adaptive_filter) {
		return AdaptiveFilterState();
	}
	return adaptive_filter->BeginFilter();
}

void ScanFilterInfo::EndFilter(AdaptiveFilterState state) {
	if (!adaptive_filter) {
		return;
	}
	adaptive_filter->EndFilter(state);
}

void ColumnScanState::NextInternal(idx_t count) {
	if (!current) {
		//! There is no column segment
		return;
	}
	offset_in_column += count;
	while (offset_in_column >= current->GetRowStart() + current->GetNode().count) {
		current = segment_tree->GetNextSegment(*current);
		initialized = false;
		segment_checked = false;
		if (!current) {
			break;
		}
	}
	D_ASSERT(!current || (offset_in_column >= current->GetRowStart() &&
	                      offset_in_column < current->GetRowStart() + current->GetNode().count));
}

idx_t ColumnScanState::GetPositionInSegment() const {
	return offset_in_column - (current ? current->GetRowStart() : 0);
}

void ColumnScanState::Next(idx_t count) {
	NextInternal(count);
	for (auto &child_state : child_states) {
		child_state.Next(count);
	}
}

void ColumnScanState::ReleasePins() {
	current = nullptr;
	initialized = false;
	scan_state.reset();
	previous_states.clear();
	for (auto &child_state : child_states) {
		child_state.ReleasePins();
	}
}

const vector<StorageIndex> &CollectionScanState::GetColumnIds() {
	return parent.GetColumnIds();
}

TableFilterSet &GetFilters();

ScanFilterInfo &CollectionScanState::GetFilterInfo() {
	return parent.GetFilterInfo();
}

ScanSamplingInfo &CollectionScanState::GetSamplingInfo() {
	return parent.GetSamplingInfo();
}

TableScanOptions &CollectionScanState::GetOptions() {
	return parent.options;
}

ParallelCollectionScanState::ParallelCollectionScanState()
    : collection(nullptr), current_row_group(nullptr), skipped_rows(0) {
}

void ParallelCollectionScanState::AssignRowGroup(optional_ptr<SegmentNode<RowGroup>> row_group) {
	current_row_group = row_group;
	while (current_row_group && !ShouldScanPartition(*current_row_group)) {
		current_row_group = GetNextRowGroup(*row_groups, *current_row_group).get();
	}
}

optional_ptr<SegmentNode<RowGroup>> ParallelCollectionScanState::GetRootSegment(RowGroupSegmentTree &row_groups) const {
	if (reorderer) {
		return reorderer->GetRootSegment(row_groups);
	}
	return row_groups.GetRootSegment();
}

optional_ptr<SegmentNode<RowGroup>>
ParallelCollectionScanState::GetNextRowGroup(RowGroupSegmentTree &row_groups, SegmentNode<RowGroup> &row_group) const {
	if (reorderer) {
		return reorderer->GetNextRowGroup(row_group);
	}
	return row_groups.GetNextSegment(row_group);
}

CollectionScanState::CollectionScanState(TableScanState &parent_p)
    : vector_index(0), max_row_group_row(0), row_groups(nullptr), max_row(0), batch_index(0),
      valid_sel(STANDARD_VECTOR_SIZE), row_group(nullptr), parent(parent_p) {
}

RandomEngine &CollectionScanState::GetRandomEngine() {
	if (!random) {
		random = make_uniq<RandomEngine>();
	}
	return *random;
}

optional_ptr<SegmentNode<RowGroup>> CollectionScanState::GetRowGroup() const {
	return row_group;
}

void CollectionScanState::SetRowGroup(optional_ptr<SegmentNode<RowGroup>> row_group_p) {
	row_group = row_group_p;
	pinned_row_group = row_group ? row_group->ReferenceNode() : nullptr;
}

optional_ptr<SegmentNode<RowGroup>> CollectionScanState::GetNextRowGroup(SegmentNode<RowGroup> &row_group) const {
	if (reorderer) {
		return reorderer->GetNextRowGroup(row_group);
	}
	return row_groups->GetNextSegment(row_group);
}

optional_ptr<SegmentNode<RowGroup>> CollectionScanState::GetNextRowGroup(SegmentLock &l,
                                                                         SegmentNode<RowGroup> &row_group) const {
	D_ASSERT(!reorderer);
	return row_groups->GetNextSegment(l, row_group);
}

optional_ptr<SegmentNode<RowGroup>> CollectionScanState::GetRootSegment() const {
	if (reorderer) {
		return reorderer->GetRootSegment(*row_groups);
	}
	return row_groups->GetRootSegment();
}

bool CollectionScanState::Scan(DuckTransaction &transaction, DataChunk &result) {
	return Scan(ScanOptions(TransactionData(transaction)), result);
}

bool CollectionScanState::Scan(ScanOptions options, DataChunk &result, optional_ptr<SegmentLock> l) {
	while (row_group) {
		pinned_row_group->Scan(options, *this, result);
		if (result.size() > 0) {
			return true;
		}
		if (max_row <= row_group->GetRowStart() + pinned_row_group->count) {
			SetRowGroup(nullptr);
			return false;
		}
		do {
			if (l) {
				SetRowGroup(GetNextRowGroup(*l, *row_group));
			} else {
				SetRowGroup(GetNextRowGroup(*row_group));
			}
			if (row_group) {
				if (row_group->GetRowStart() >= max_row) {
					SetRowGroup(nullptr);
					break;
				}
				bool scan_row_group = row_group->GetNode().InitializeScan(*this, *row_group);
				if (scan_row_group) {
					// scan this row group
					break;
				}
			}
		} while (row_group);
	}
	return false;
}

bool CollectionScanState::Scan(DataChunk &result, TableScanType type, optional_ptr<SegmentLock> l) {
	while (row_group) {
		pinned_row_group->Scan(*this, result, type);
		if (result.size() > 0) {
			return true;
		}
		// move to the next row group
		if (l) {
			SetRowGroup(GetNextRowGroup(*l, *row_group));
		} else {
			SetRowGroup(GetNextRowGroup(*row_group));
		}
		if (row_group) {
			row_group->GetNode().InitializeScan(*this, *row_group);
		}
	}
	return false;
}

bool CollectionScanState::PrepareScanIO(DuckTransaction &transaction, vector<unique_ptr<AsyncTask>> &tasks,
                                        bool register_assignment) {
	if (!row_group) {
		return false;
	}
	auto &current_row_group = *pinned_row_group;
	ScanOptions options {TransactionData(transaction)};
	if (!current_row_group.PrepareScan(options, *this)) {
		// the assignment is exhausted
		D_ASSERT(max_row <= row_group->GetRowStart() + current_row_group.count);
		SetRowGroup(nullptr);
		return false;
	}
	if (prepared_vector.prepare_state == VectorPrepareState::IO_REGISTERED) {
		// I/O for the prepared vector was already registered (e.g. we are resuming after BLOCKED)
		return true;
	}
	prepared_vector.prepare_state = VectorPrepareState::IO_REGISTERED;
	if (register_assignment) {
		tasks = RegisterAssignmentIO();
	} else if (!assignment_io_registered) {
		// read-ahead jobs registered their whole assignment when produced, otherwise collect the vector's I/O
		tasks = current_row_group.CollectScanIOTasks(*this, prepared_vector.max_count);
	}
	return true;
}

vector<unique_ptr<AsyncTask>> CollectionScanState::RegisterAssignmentIO() {
	D_ASSERT(row_group);
	D_ASSERT(!assignment_io_registered);
	assignment_io_registered = true;
	auto &current_row_group = *pinned_row_group;
	return current_row_group.CollectScanIOTasks(*this, current_row_group.PrefetchRowCount(*this));
}

void CollectionScanState::InitializeColumnScans() {
	if (column_scans_pending) {
		pinned_row_group->InitializeColumnScans(*this);
	}
}

void TableScanState::InitializeColumnScans() {
	table_state.InitializeColumnScans();
	local_state.InitializeColumnScans();
}

void CollectionScanState::ReleasePins() {
	SetRowGroup(nullptr);
	prepared_vector.Reset();
	column_scans_pending = false;
	for (auto &column_scan : column_scans) {
		column_scan.ReleasePins();
	}
}

void TableScanState::ReleasePins() {
	table_state.ReleasePins();
	local_state.ReleasePins();
}

idx_t CollectionScanState::RemainingAssignmentRows() const {
	const idx_t current_row = vector_index * STANDARD_VECTOR_SIZE;
	return current_row < max_row_group_row ? max_row_group_row - current_row : 0;
}

void CollectionScanState::ProcessPreparedScan(DuckTransaction &transaction, DataChunk &result) {
	D_ASSERT(row_group);
	D_ASSERT(prepared_vector.prepare_state == VectorPrepareState::IO_REGISTERED);
	ScanOptions options {TransactionData(transaction)};
	pinned_row_group->ProcessPreparedScan(options, *this, result);
}

bool CollectionScanState::RecheckPreparedRowGroup() {
	if (!row_group || prepared_vector.prepare_state == VectorPrepareState::NONE) {
		return true;
	}
	if (pinned_row_group->CheckZonemap(context.GetClientContext(), GetFilterInfo(), row_group->GetRowStart())) {
		return true;
	}
	// the prepared vector was counted as scanned when it was prepared, it is never decoded
	D_ASSERT(rows_scanned >= prepared_vector.visible_count);
	rows_scanned -= prepared_vector.visible_count;
	prepared_vector.Reset();
	SetRowGroup(nullptr);
	return false;
}

PreparedScanVector::PreparedScanVector() : sample_sel(STANDARD_VECTOR_SIZE) {
}

void PreparedScanVector::Reset() {
	prepare_state = VectorPrepareState::NONE;
}

PrefetchState::~PrefetchState() {
}

void PrefetchState::AddBlock(shared_ptr<BlockHandle> block) {
	blocks.push_back(std::move(block));
}

} // namespace duckdb
