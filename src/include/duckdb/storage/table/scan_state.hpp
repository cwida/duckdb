//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/table/scan_state.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/storage/buffer/buffer_handle.hpp"
#include "duckdb/storage/storage_lock.hpp"
#include "duckdb/storage/table/row_group_reorderer.hpp"
#include "duckdb/common/random_engine.hpp"
#include "duckdb/storage/table/segment_lock.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/parser/parsed_data/sample_options.hpp"
#include "duckdb/common/enums/scan_options.hpp"
#include "duckdb/storage/storage_index.hpp"
#include "duckdb/planner/table_filter_state.hpp"

namespace duckdb {
class AdaptiveFilter;
class AsyncTask;
class ColumnSegment;
class LocalTableStorage;
class CollectionScanState;
class Index;
class RowGroup;
class RowGroupCollection;
class UpdateSegment;
class TableScanState;
class ColumnSegment;
class ColumnSegmentTree;
class ValiditySegment;
class TableFilterSet;
class ColumnData;
class DuckTransaction;
class RowGroupSegmentTree;
class TableFilter;
struct AdaptiveFilterState;
struct TableScanOptions;
struct ScanSamplingInfo;
struct TableFilterState;
template <class T>
struct SegmentNode;

struct SegmentScanState {
	virtual ~SegmentScanState() {
	}

	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}
};

struct IndexScanState {
	virtual ~IndexScanState() {
	}

	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}
};

struct PushedDownExpressionState {
public:
	explicit PushedDownExpressionState(ClientContext &context) : executor(context) {
	}

public:
	//! The executor to execute the expression
	ExpressionExecutor executor;
	//! The pushed down expression to execute
	unique_ptr<Expression> expression;
	//! The target chunk to store the result of the execution
	DataChunk target;
	DataChunk input;
};

struct ColumnScanState {
	explicit ColumnScanState(optional_ptr<CollectionScanState> parent_p) : parent(parent_p) {
	}

	optional_ptr<CollectionScanState> parent;
	//! The query context for this scan
	QueryContext context;
	//! The column segment that is currently being scanned
	optional_ptr<SegmentNode<ColumnSegment>> current;
	//! Column segment tree
	ColumnSegmentTree *segment_tree = nullptr;
	//! The current row offset in the column
	idx_t offset_in_column = 0;
	//! The internal row index (i.e. the position of the SegmentScanState)
	idx_t internal_index = 0;
	//! Storage index of the current column that's being scanned
	StorageIndex storage_index;
	//! Segment scan state
	unique_ptr<SegmentScanState> scan_state;
	//! Child states of the vector
	unsafe_vector<ColumnScanState> child_states;
	//! Whether or not InitializeState has been called for this segment
	bool initialized = false;
	//! If this segment has already been checked for skipping purposes
	bool segment_checked = false;
	//! We initialize one SegmentScanState per segment, however, if scanning a DataChunk requires us to scan over more
	//! than one Segment, we need to keep the scan states of the previous segments around
	vector<unique_ptr<SegmentScanState>> previous_states;
	//! The last read offset in the child state (used for LIST columns only)
	idx_t last_offset = 0;
	//! Whether or not we should scan a specific child column
	vector<bool> scan_child_column;
	//! Contains TableScan level config for scanning
	optional_ptr<TableScanOptions> scan_options;
	//! (optionally) the expression state for any pushed down expression(s)
	unique_ptr<PushedDownExpressionState> expression_state;
	//! Whether or not updates should be allowed
	UpdateScanType update_scan_type = UpdateScanType::STANDARD;
	//! Whether scans and selects copy strings into the result instead of pointing into the scanned blocks
	StringScanType string_scan_type = StringScanType::STANDARD;

public:
	void PushDownCast(const LogicalType &original_type, const LogicalType &cast_type);

public:
	void Initialize(const QueryContext &context_p, const LogicalType &type, const StorageIndex &column_id,
	                optional_ptr<TableScanOptions> options);
	void Initialize(const QueryContext &context_p, const LogicalType &type, optional_ptr<TableScanOptions> options);
	//! Move the scan state forward by "count" rows (including all child states)
	void Next(idx_t count);
	//! Move ONLY this state forward by "count" rows (i.e. not the child states)
	void NextInternal(idx_t count);
	//! Returns the current row position in the segment
	idx_t GetPositionInSegment() const;
	//! Releases the segment scan states (and the blocks they pin), including those of the child states
	void ReleasePins();
};

enum class FetchType {
	//! Verify if each row is valid for the transaction prior to fetching
	TRANSACTIONAL_FETCH,
	// Force fetch the row, regardless of it if is valid for the transaction or not
	FORCE_FETCH
};

struct ColumnFetchState {
	FetchType fetch_type = FetchType::TRANSACTIONAL_FETCH;
	//! The query context for this fetch
	QueryContext context;
	//! Any child states of the fetch
	vector<unique_ptr<ColumnFetchState>> child_states;
	//! The current row group we are fetching from
	optional_ptr<SegmentNode<RowGroup>> row_group;
};

struct ScanFilter {
	ScanFilter(ClientContext &context, ProjectionIndex index, const vector<StorageIndex> &column_ids,
	           TableFilter &filter);

	ProjectionIndex scan_column_index;
	StorageIndex table_column_index;
	TableFilter &filter;
	bool always_true;
	unique_ptr<TableFilterState> filter_state;

	bool IsAlwaysTrue() const {
		return always_true;
	}
};

class ScanFilterInfo {
public:
	~ScanFilterInfo();

	void Initialize(ClientContext &context, TableFilterSet &filters, const vector<StorageIndex> &column_ids);

	const vector<ScanFilter> &GetFilterList() const {
		return filter_list;
	}
	optional_ptr<const TableFilterSet> GetTableFilters() const {
		return table_filters.get();
	}
	optional_ptr<const vector<StorageIndex>> GetColumnIds() const {
		return column_ids;
	}

	optional_ptr<AdaptiveFilter> GetAdaptiveFilter();
	AdaptiveFilterState BeginFilter() const;
	void EndFilter(AdaptiveFilterState state);

	//! Whether or not there is any filter we need to execute
	bool HasFilters() const;

	//! Whether or not there is a filter we need to execute for this column currently
	bool ColumnHasFilters(idx_t col_idx);

	//! Resets any SetFilterAlwaysTrue flags
	void CheckAllFilters();
	//! Labels the filters for this specific column as always true
	//! We do not need to execute them anymore until CheckAllFilters is called
	void SetFilterAlwaysTrue(idx_t filter_idx);

private:
	//! The table filters (if any)
	optional_ptr<TableFilterSet> table_filters;
	//! Maps scan projection indexes to storage column indexes
	optional_ptr<const vector<StorageIndex>> column_ids;
	//! Adaptive filter info (if any)
	unique_ptr<AdaptiveFilter> adaptive_filter;
	//! The set of filters
	vector<ScanFilter> filter_list;
	//! Whether or not the column has a filter active right now
	unsafe_vector<bool> column_has_filter;
	//! Whether or not the column has a filter active at all
	unsafe_vector<bool> base_column_has_filter;
	//! The amount of filters that are always true currently
	idx_t always_true_filters = 0;
};

enum class VectorPrepareState : uint8_t {
	//! No vector is currently prepared for processing
	NONE,
	//! A vector is prepared for processing
	PREPARED,
	//! A vector is prepared and its I/O has been registered
	IO_REGISTERED
};

//! Eligibility state of one vector, computed by RowGroup::PrepareScan and consumed by ProcessPreparedScan
struct PreparedScanVector {
	PreparedScanVector();

	//! The prepare state of the current vector
	VectorPrepareState prepare_state = VectorPrepareState::NONE;
	//! The number of rows in the prepared vector
	idx_t max_count = 0;
	//! The number of rows visible to the transaction (held in CollectionScanState::valid_sel)
	idx_t visible_count = 0;
	//! Whether the prepared vector has a system sample selection
	bool has_sample_selection = false;
	//! The number of sampled rows (held in sample_sel)
	idx_t sample_count = 0;
	//! The system sample selection
	SelectionVector sample_sel;

	void Reset();
};

class CollectionScanState {
public:
	explicit CollectionScanState(TableScanState &parent_p);
	//! The query context for this scan
	QueryContext context;
	//! The vector index within the row_group
	idx_t vector_index;
	//! The maximum row within the row group
	idx_t max_row_group_row;
	//! Child column scans
	unsafe_vector<ColumnScanState> column_scans;
	//! Row group segment tree we are scanning
	shared_ptr<RowGroupSegmentTree> row_groups;
	//! The total maximum row index
	idx_t max_row;
	//! The current batch index
	idx_t batch_index;
	//! The row_number base for the current batch (number of committed rows before this batch)
	//! Only set when the row_number virtual column is being scanned
	optional_idx row_number_base;
	//! The valid selection
	SelectionVector valid_sel;
	//! The currently prepared vector (see RowGroup::PrepareScan)
	PreparedScanVector prepared_vector;
	//! Whether scan I/O for the current row group assignment has been registered
	bool assignment_io_registered = false;
	//! Whether the column scans of the current assignment still have to be initialized
	bool column_scans_pending = false;

	//! The amount of tuples considered by a scan, before applying filters
	idx_t rows_scanned = 0;

	//! Optional state for custom row group ordering
	unique_ptr<RowGroupReorderer> reorderer;

public:
	void Initialize(const QueryContext &context_p, const vector<LogicalType> &types);
	const vector<StorageIndex> &GetColumnIds();
	ScanFilterInfo &GetFilterInfo();
	ScanSamplingInfo &GetSamplingInfo();
	RandomEngine &GetRandomEngine();
	TableScanOptions &GetOptions();
	optional_ptr<SegmentNode<RowGroup>> GetRowGroup() const;
	void SetRowGroup(optional_ptr<SegmentNode<RowGroup>> row_group);
	optional_ptr<SegmentNode<RowGroup>> GetNextRowGroup(SegmentNode<RowGroup> &row_group) const;
	optional_ptr<SegmentNode<RowGroup>> GetNextRowGroup(SegmentLock &l, SegmentNode<RowGroup> &row_group) const;
	optional_ptr<SegmentNode<RowGroup>> GetRootSegment() const;
	bool Scan(DuckTransaction &transaction, DataChunk &result);
	bool Scan(ScanOptions options, DataChunk &result, optional_ptr<SegmentLock> l = nullptr);
	bool Scan(DataChunk &result, TableScanType type, optional_ptr<SegmentLock> l = nullptr);
	//! Prepares the next eligible vector, collecting its I/O tasks, or the remaining assignment's when registering it
	bool PrepareScanIO(DuckTransaction &transaction, vector<unique_ptr<AsyncTask>> &tasks,
	                   bool register_assignment = false);
	//! Rows of the assignment left to scan from the current vector onwards
	idx_t RemainingAssignmentRows() const;
	//! Initializes the column scans a claim deferred
	void InitializeColumnScans();
	//! Processes the vector prepared by PrepareScanIO
	void ProcessPreparedScan(DuckTransaction &transaction, DataChunk &result);
	//! Drops the prepared row group and returns false when the current filters prune it
	bool RecheckPreparedRowGroup();
	//! Releases the row group and the blocks the column scans pin, the next assignment initializes them again
	void ReleasePins();

private:
	//! Registers the remaining assignment's scan I/O, returning the async tasks that execute it
	vector<unique_ptr<AsyncTask>> RegisterAssignmentIO();

private:
	//! The segment node is the traversal cursor, while the shared pointer pins its replaceable payload
	optional_ptr<SegmentNode<RowGroup>> row_group;
	shared_ptr<RowGroup> pinned_row_group;
	TableScanState &parent;
	//! The random engine for system sampling
	unique_ptr<RandomEngine> random;
};

struct ScanSamplingInfo {
	//! Whether or not to do a system sample during scanning
	bool do_system_sample = false;
	//! The sampling rate to use (for percentage-based sampling)
	double sample_rate;
	//! The seeded phase used for row-count based systematic sampling
	double sample_phase = 0;
	//! Whether the sampling is row-count based or percentage-based
	bool is_percentage = false;
	//! Target number of rows to sample (for row-count based sampling)
	idx_t target_sample_rows = 0;
};

struct TableScanOptions {
	//! Read the rows of scans through the fetch by row id instead of scanning them
	bool force_fetch_row = false;
};

class TableScanState {
public:
	TableScanState();
	~TableScanState();

	//! The underlying table scan state
	CollectionScanState table_state;
	//! Transaction-local scan state
	CollectionScanState local_state;
	//! Options for scanning
	TableScanOptions options;
	//! Filter info
	ScanFilterInfo filters;
	//! Sampling info
	ScanSamplingInfo sampling_info;

public:
	void Initialize(vector<StorageIndex> column_ids, optional_ptr<ClientContext> context = nullptr,
	                optional_ptr<TableFilterSet> table_filters = nullptr,
	                optional_ptr<SampleOptions> table_sampling = nullptr, idx_t estimated_table_row_count = 0);

	const vector<StorageIndex> &GetColumnIds();

	ScanFilterInfo &GetFilterInfo();

	ScanSamplingInfo &GetSamplingInfo();
	//! Initializes the column scans a claim deferred, for whichever collection holds the assignment
	void InitializeColumnScans();
	//! Rows scanned from persistent and transaction-local storage
	idx_t RowsScanned() const;
	//! Releases what the scans of both storages pin, for a state that is kept around between assignments
	void ReleasePins();

private:
	//! The column identifiers of the scan
	vector<StorageIndex> column_ids;
};

struct ParallelCollectionScanState {
	ParallelCollectionScanState();
	void AssignRowGroup(optional_ptr<SegmentNode<RowGroup>> row_group);
	optional_ptr<SegmentNode<RowGroup>> GetRootSegment(RowGroupSegmentTree &row_groups) const;
	optional_ptr<SegmentNode<RowGroup>> GetNextRowGroup(RowGroupSegmentTree &row_groups,
	                                                    SegmentNode<RowGroup> &row_group) const;

	//! The row group collection we are scanning
	const RowGroupCollection *collection;
	shared_ptr<RowGroupSegmentTree> row_groups;
	optional_ptr<SegmentNode<RowGroup>> current_row_group;
	idx_t vector_index;
	idx_t max_row;
	idx_t batch_index;
	//! Rows of assignments that were skipped entirely (e.g. by zonemaps) without being handed out
	atomic<idx_t> skipped_rows;
	optional_idx row_number_base;
	mutex lock;

	//! Optional state for custom row group ordering
	unique_ptr<RowGroupReorderer> reorderer;
	//! Subset of partition indices to scan, if null, scan all
	optional_ptr<const unordered_set<idx_t>> partitions_to_scan;

	//! Whether this row group should be scanned
	bool ShouldScanPartition(SegmentNode<RowGroup> &row_group) const {
		return !partitions_to_scan || partitions_to_scan->count(row_group.GetIndex()) > 0;
	}
};

struct ParallelTableScanState {
	//! Parallel scan state for the table
	ParallelCollectionScanState scan_state;
	//! Parallel scan state for the transaction-local state
	ParallelCollectionScanState local_state;
};

struct PrefetchState {
	~PrefetchState();

	void AddBlock(shared_ptr<BlockHandle> block);

	vector<shared_ptr<BlockHandle>> blocks;
};

class CreateIndexScanState : public TableScanState {
public:
	shared_ptr<RowGroupSegmentTree> row_groups;
	vector<unique_ptr<StorageLockKey>> locks;
	unique_lock<mutex> append_lock;
	SegmentLock segment_lock;
};

} // namespace duckdb
