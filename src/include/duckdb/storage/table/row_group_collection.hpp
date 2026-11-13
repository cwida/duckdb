//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/table/row_group_collection.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/storage/table/row_group.hpp"
#include "duckdb/storage/table/segment_tree.hpp"
#include "duckdb/storage/statistics/column_statistics.hpp"
#include "duckdb/storage/table/table_statistics.hpp"
#include "duckdb/storage/storage_index.hpp"
#include "duckdb/common/enums/column_segment_info_scan_type.hpp"
#include "duckdb/common/enums/index_removal_type.hpp"
#include "duckdb/common/enums/row_group_append_mode.hpp"

namespace duckdb {

struct ParallelTableScanState;
struct ParallelCollectionScanState;
class CreateIndexScanState;
class CollectionScanState;
class PersistentTableData;
class TableDataWriter;
class IndexEntry;
class TableIndexList;
class TableStatistics;
struct TableAppendState;
class DuckTransaction;
class BoundConstraint;
class RowGroupSegmentTree;
class StorageCommitState;
struct ColumnSegmentInfo;
class MetadataManager;
struct VacuumState;
struct CollectionCheckpointState;

struct PersistentCollectionData;
class CheckpointTask;
class TableIOManager;
class CommitDropState;
class DataTable;
class DuckTableEntry;
class RowGroupIterationHelper;
class TableScanState;
class BoundIndex;

//! How checkpoint vacuum handles table indexes when rowids may change.
enum class VacuumIndexStrategy : uint8_t {
	//! Indexes require stable rowids.
	KEEP_ROW_IDS,
	//! No index maintenance is needed.
	NO_INDEXES,
	//! Rebuild all indexes after vacuum.
	REBUILD,
	//! Incrementally remap affected index rowids.
	REMAP
};

//! Snapshot state used to iterate row groups without holding the row-group segment-tree
//! lock for the duration of the scan. Holding row_groups pins the snapshot alive; consistency
//! follows the same model as table scans.
struct ColumnSegmentInfoScanState {
	shared_ptr<RowGroupSegmentTree> row_groups;
	optional_ptr<SegmentNode<RowGroup>> current_row_group;
	ColumnSegmentInfoScanOptions options;
};

//! The row groups that existed when the checkpoint started
struct CollectionCheckpointSnapshot {
	shared_ptr<RowGroupSegmentTree> row_groups;
	idx_t segment_count = 0;
	//! The row group count recorded by the first append that saw this checkpoint, if any
	optional_idx row_group_count;
};

//! The rewritten row groups, installed by InstallCheckpoint
struct CollectionCheckpointResult {
	//! Null if the table was unchanged
	shared_ptr<RowGroupSegmentTree> row_groups;
	idx_t total_rows = 0;
	idx_t next_row_id = 0;
	//! The live row groups past this count were appended while the checkpoint ran
	idx_t checkpointed_row_group_count = 0;
};

class RowGroupCollection {
public:
	RowGroupCollection(shared_ptr<DataTableInfo> info, TableIOManager &io_manager, vector<LogicalType> types,
	                   idx_t row_start, idx_t total_rows = 0);
	RowGroupCollection(shared_ptr<DataTableInfo> info, BlockManager &block_manager, vector<LogicalType> types,
	                   idx_t row_start, idx_t total_rows, idx_t row_group_size);
	//! Shares the source row groups in a fixed, read-only collection for scans.
	static unique_ptr<const RowGroupCollection> CreateScanSnapshot(const RowGroupCollection &source);

public:
	idx_t GetTotalRows() const;
	idx_t GetNextRowId() const;
	idx_t GetRowGroupCount() const;
	Allocator &GetAllocator() const;

	void Initialize(PersistentCollectionData &data);
	void Initialize(PersistentTableData &data);
	void InitializeEmpty();
	void FinalizeCheckpoint(MetaBlockPointer pointer, const vector<MetaBlockPointer> &existing_pointers);

	bool IsEmpty() const;

	void AppendRowGroup(SegmentLock &l, idx_t start_row);
	//! Get the nth row-group, negative numbers start from the back (so -1 is the last row group, etc)
	optional_ptr<RowGroup> GetRowGroup(int64_t index);
	//! Overrides a row group - should only be used if you know what you're doing (will likely be removed in the future)
	void SetRowGroup(int64_t index, shared_ptr<RowGroup> new_row_group);
	void Verify();
	void Destroy();

	void InitializeScan(const QueryContext &context, CollectionScanState &state, const vector<StorageIndex> &column_ids,
	                    optional_ptr<TableFilterSet> table_filters);
	void InitializeCreateIndexScan(CreateIndexScanState &state);
	void InitializeScanWithOffset(const QueryContext &context, CollectionScanState &state,
	                              const vector<StorageIndex> &column_ids, idx_t start_row, idx_t end_row);
	static bool InitializeScanInRowGroup(ClientContext &context, CollectionScanState &state,
	                                     const RowGroupCollection &collection, SegmentNode<RowGroup> &row_group,
	                                     idx_t vector_index, idx_t max_row, bool initialize_columns = true);
	void InitializeParallelScan(ParallelCollectionScanState &state) const;
	//! Claims the next assignment and returns its rows, invalid once none is left, deferring column setup when asked
	optional_idx NextParallelScan(ClientContext &context, ParallelCollectionScanState &state,
	                              CollectionScanState &scan_state, bool initialize_columns = true) const;

	RowGroupIterationHelper Chunks(DuckTransaction &transaction);
	RowGroupIterationHelper Chunks(DuckTransaction &transaction, const vector<StorageIndex> &column_ids);

	void Fetch(TransactionData transaction, DataChunk &result, const vector<StorageIndex> &column_ids,
	           const Vector &row_identifiers, idx_t fetch_count, ColumnFetchState &state);

	//! Returns true, if the row group can fetch the row id for the transaction.
	bool CanFetch(TransactionData, const row_t row_id);

	//! Initialize an append of a variable number of rows. FinalizeAppend must be called after appending is done.
	void InitializeAppend(TableAppendState &state);
	//! Initialize an append with a variable number of rows. FinalizeAppend should not be called after appending is
	//! done.
	void InitializeAppend(TransactionData transaction, TableAppendState &state);
	//! Appends to the row group collection. Returns the finished row group index if a new row group has been appended
	//! to
	optional_idx Append(DataChunk &chunk, TableAppendState &state);
	//! FinalizeAppend flushes an append with a variable number of rows.
	void FinalizeAppend(TransactionData transaction, TableAppendState &state);
	void CommitAppend(transaction_t commit_id, idx_t row_start, idx_t count);
	void RevertAppendInternal(idx_t start_row);
	//! Reverts an append that failed before it was finalized
	void RevertFailedAppend(TableAppendState &state);
	void CleanupAppend(VisibilityBound lowest_visibility_bound, idx_t start, idx_t count);

	void MergeStorage(RowGroupCollection &data, optional_ptr<DataTable> table,
	                  optional_ptr<StorageCommitState> commit_state);
	bool IsPersistent() const;

	void RemoveFromIndexes(const QueryContext &context, TableIndexList &indexes, Vector &row_identifiers, idx_t count,
	                       IndexRemovalType removal_type, optional_idx active_checkpoint = optional_idx());

	//! Delete the given rows - optionally collects the ids of the rows that were deleted (excluding duplicates)
	idx_t Delete(TransactionData transaction, DuckTableEntry &table_entry, row_t *ids, idx_t count,
	             optional_ptr<DeletedRowIds> deleted_ids = nullptr);
	void Update(TransactionData transaction, DuckTableEntry &table_entry, row_t *ids,
	            const vector<PhysicalIndex> &column_ids, DataChunk &updates);
	void UpdateColumn(TransactionData transaction, DuckTableEntry &table_entry, Vector &row_ids,
	                  const vector<column_t> &column_path, DataChunk &updates);

	CollectionCheckpointSnapshot SnapshotForCheckpoint(TableDataWriter &writer) const;
	CollectionCheckpointResult Checkpoint(TableDataWriter &writer, TableStatistics &global_stats,
	                                      const CollectionCheckpointSnapshot &snapshot);
	//! Takes over the row groups appended meanwhile. Runs under the table's append lock
	void InstallCheckpoint(CollectionCheckpointResult result, TableStatistics &checkpoint_stats);

	//! Decides how vacuum handles this table's indexes.
	VacuumIndexStrategy
	GetVacuumIndexStrategy(AttachedDatabase &attached,
	                       optional_ptr<vector<shared_ptr<IndexEntry>>> remap_indexes = nullptr) const;

	void InitializeVacuumState(CollectionCheckpointState &checkpoint_state, VacuumState &state,
	                           optional_idx checkpoint_row_group_count);
	bool ScheduleVacuumTasks(CollectionCheckpointState &checkpoint_state, VacuumState &state, idx_t segment_idx,
	                         bool schedule_vacuum);
	unique_ptr<CheckpointTask> GetCheckpointTask(CollectionCheckpointState &checkpoint_state, idx_t segment_idx);

	//! Accumulates block drops for every row group's copy of the column into the drop state.
	void CommitDropColumn(const idx_t column_index, CommitDropState &drop_state);
	//! Accumulates block drops for every row group into the drop state.
	void CommitDropTable(CommitDropState &drop_state);
	//! Drops every row group's copy of the column and immediately marks the blocks as modified.
	void CommitDropColumn(const idx_t column_index);
	//! Drops every row group and immediately marks the blocks as modified.
	void CommitDropTable();

	vector<PartitionStatistics> GetPartitionStats(TransactionData transaction) const;
	vector<ColumnSegmentInfo>
	GetColumnSegmentInfo(const QueryContext &context,
	                     const ColumnSegmentInfoScanOptions &options = ColumnSegmentInfoScanOptions {}) const;
	//! Initialize an incremental scan over column segment info, pinning the current row groups for consistency.
	void InitializeColumnSegmentInfoScan(ColumnSegmentInfoScanState &state) const;
	//! Append the next row group's column segment info to result. Returns false when no row groups remain.
	bool ScanColumnSegmentInfo(const QueryContext &context, ColumnSegmentInfoScanState &state,
	                           vector<ColumnSegmentInfo> &result) const;
	bool SupportsPerColumnWrites() const;
	bool SupportsPerColumnWrites();
	const vector<LogicalType> &GetTypes() const;

	shared_ptr<RowGroupCollection> AddColumn(ClientContext &context, ColumnDefinition &new_column,
	                                         ExpressionExecutor &default_executor);
	shared_ptr<RowGroupCollection> RemoveColumn(idx_t col_idx);
	shared_ptr<RowGroupCollection> AlterType(ClientContext &context, idx_t changed_idx, const LogicalType &target_type,
	                                         vector<StorageIndex> bound_columns, Expression &cast_expr,
	                                         TransactionData transaction);
	void VerifyNewConstraint(const QueryContext &context, DuckTransaction &transaction, DataTable &parent,
	                         const BoundConstraint &constraint);

	void CopyStats(TableStatistics &stats);
	unique_ptr<BaseStatistics> CopyStats(const StorageIndex &column_id);
	unique_ptr<BlockingSample> GetSample();
	void SetDistinct(column_t column_id, unique_ptr<DistinctStatistics> distinct_stats);

	AttachedDatabase &GetAttached() const;
	DatabaseInstance &GetDatabase() const;
	BlockManager &GetBlockManager() const {
		return block_manager;
	}
	MetadataManager &GetMetadataManager();
	const DataTableInfo &GetTableInfo() const {
		return *info;
	}
	DataTableInfo &GetTableInfo() {
		return *info;
	}

	idx_t GetAllocationSize() const {
		return allocation_size;
	}

	idx_t GetRowGroupSize() const {
		return row_group_size;
	}
	void SetRowGroupAppendMode(RowGroupAppendMode mode);
	//! Returns the total amount of segments - use sparingly, as this forces all segments to be loaded
	idx_t GetSegmentCount();

	//! Get a ptr to the raw segment tree. This can be useful for some extensions to have directly exposed.
	shared_ptr<RowGroupSegmentTree> GetRowGroups() const;

	//! The metadata blocks of the table data of this collection, as read from disk or written by the last checkpoint
	const vector<MetaBlockPointer> &GetMetadataPointers() const {
		return metadata_pointers;
	}

private:
	optional_ptr<SegmentNode<RowGroup>> NextUpdateRowGroup(RowGroupSegmentTree &row_groups, row_t *ids, idx_t &pos,
	                                                       idx_t count) const;

	void SetRowGroups(shared_ptr<RowGroupSegmentTree> row_groups);

private:
	//! BlockManager
	BlockManager &block_manager;
	//! The row group size of the row group collection
	const idx_t row_group_size;
	//! The number of rows in the table
	atomic<idx_t> total_rows;
	//! Next rowid offset relative to the row group tree base rowid.
	//! For main table storage the base is 0, so this is also the absolute next rowid.
	atomic<idx_t> next_row_id;
	//! The data table info
	shared_ptr<DataTableInfo> info;
	//! The column types of the row group collection
	vector<LogicalType> types;
	//! Lock held when accessing or modifying the owned_row_groups pointer
	mutable mutex row_group_pointer_lock;
	//! The owning pointer of the segment tree
	shared_ptr<RowGroupSegmentTree> owned_row_groups;
	//! Table statistics
	TableStatistics stats;
	//! Allocation size, only tracked for appends
	atomic<idx_t> allocation_size;
	//! Root metadata pointer, if the collection is loaded from disk
	MetaBlockPointer metadata_pointer;
	//! Other metadata pointers
	vector<MetaBlockPointer> metadata_pointers;
	//! Controls whether the next append creates a new row group or reuses the existing one
	RowGroupAppendMode row_group_append_mode;
	//! Whether or not we can append to a checkpointed row group
	bool can_append_to_checkpointed_row_group = true;
};

class RowGroupIterationHelper {
public:
	RowGroupIterationHelper(RowGroupCollection &collection, DuckTransaction &transaction,
	                        vector<StorageIndex> column_ids);

private:
	RowGroupCollection &collection;
	DuckTransaction &transaction;
	vector<StorageIndex> column_ids;

private:
	class RowGroupIterator {
	public:
		RowGroupIterator(optional_ptr<RowGroupCollection> collection, optional_ptr<DuckTransaction> transaction,
		                 const vector<StorageIndex> &column_ids);
		~RowGroupIterator();
		//! enable move constructor
		RowGroupIterator(RowGroupIterator &&other) noexcept;

		optional_ptr<RowGroupCollection> collection;
		optional_ptr<DuckTransaction> transaction;
		unique_ptr<DataChunk> chunk;
		unique_ptr<TableScanState> state;

	public:
		RowGroupIterator &operator++();
		bool operator!=(const RowGroupIterator &other) const;
		DataChunk &operator*() const;
	};

public:
	RowGroupIterator begin(); // NOLINT: match stl API
	RowGroupIterator end();   // NOLINT: match stl API
};

} // namespace duckdb
