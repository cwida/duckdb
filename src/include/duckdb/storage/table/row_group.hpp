//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/table/row_group.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/storage/table/chunk_info.hpp"
#include "duckdb/storage/statistics/segment_statistics.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/enums/column_segment_info_scan_type.hpp"
#include "duckdb/common/enums/scan_options.hpp"
#include "duckdb/storage/table/per_column_metadata_blocks.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/parser/column_list.hpp"
#include "duckdb/storage/table/segment_base.hpp"
#include "duckdb/storage/block.hpp"
#include "duckdb/storage/storage_index.hpp"
#include "duckdb/storage/checkpoint/checkpoint_options.hpp"

namespace duckdb {
class AsyncTask;
class AttachedDatabase;
class BlockManager;
class ColumnData;
class ColumnStatistics;
class DatabaseInstance;
class DataTable;
class DuckTableEntry;
class PartialBlockManager;
struct DataTableInfo;
class ExpressionExecutor;
class RowGroupCollection;
class RowGroupWriter;
class UpdateSegment;
class TableStatistics;
struct ColumnSegmentInfo;
class Vector;
struct ColumnCheckpointState;
struct PartitionStatistics;
struct PersistentColumnData;
struct PersistentRowGroupData;
struct RowGroupPointer;
struct TransactionData;
class CollectionScanState;
class TableFilter;
class TableFilterSet;
struct ColumnFetchState;
struct PrefetchState;
struct RowGroupAppendState;
struct SuballocationBlock;
class MetadataManager;
class RowVersionManager;
class CommitDropState;
class ScanFilterInfo;
class StorageCommitState;
template <class T>
struct SegmentNode;
enum class ColumnDataType;
class ClientContext;

struct RowGroupWriteInfo {
	RowGroupWriteInfo(PartialBlockManager &manager, const vector<CompressionType> &compression_types,
	                  CheckpointOptions options = CheckpointOptions());
	RowGroupWriteInfo(PartialBlockManager &manager, const vector<CompressionType> &compression_types,
	                  vector<unique_ptr<PartialBlockManager>> &column_partial_block_managers_p);

private:
	PartialBlockManager &manager;

public:
	const vector<CompressionType> &compression_types;
	CheckpointOptions options;

public:
	PartialBlockManager &GetPartialBlockManager(idx_t column_idx);

private:
	optional_ptr<vector<unique_ptr<PartialBlockManager>>> column_partial_block_managers;
};

enum class RowGroupWriteAction {
	REUSE_EXISTING_ROW_GROUP_METADATA,
	PARTIALLY_REUSE_COLUMN_METADATA,
	FULLY_CHECKPOINT_ROW_GROUP
};

struct RowGroupWriteData {
	shared_ptr<RowGroup> result_row_group;
	vector<unique_ptr<ColumnCheckpointState>> states;
	vector<BaseStatistics> statistics;
	RowGroupWriteAction write_action = RowGroupWriteAction::FULLY_CHECKPOINT_ROW_GROUP;
	optional_idx write_count;
};

//! The version info of a row group, shared with the row groups a checkpoint or an ALTER copies it into
class RowGroupVersionInfo {
public:
	explicit RowGroupVersionInfo(vector<MetaBlockPointer> deletes_pointers = {});

	//! Loads the persisted deletes if necessary
	optional_ptr<RowVersionManager> Get(BlockManager &block_manager);
	//! Does not load persisted deletes
	optional_ptr<RowVersionManager> GetIfLoaded() const {
		return version_info.load();
	}
	shared_ptr<RowVersionManager> GetOrCreate(BlockManager &block_manager);
	bool HasUnloadedDeletes() const;
	const vector<MetaBlockPointer> &GetDeletesPointers() const {
		return deletes_pointers;
	}

private:
	mutex lock;
	vector<MetaBlockPointer> deletes_pointers;
	atomic<bool> deletes_is_loaded;
	shared_ptr<RowVersionManager> owned_version_info;
	atomic<optional_ptr<RowVersionManager>> version_info;
};

//! Collects the ids of the rows that a delete actually deleted, but only once rows were skipped (e.g. because they
//! were matched more than once) - in the common case all input rows are deleted and nothing is collected
struct DeletedRowIds {
	explicit DeletedRowIds(const row_t *input_ids) : input_ids(input_ids) {
	}

	//! The row ids passed to the delete
	const row_t *input_ids;
	//! Whether any of the input rows were skipped - only then are the deleted row ids collected
	bool has_skipped_rows = false;
	vector<row_t> row_ids;
};

class RowGroup : public SegmentBase<RowGroup> {
public:
	friend class ColumnData;

public:
	RowGroup(RowGroupCollection &collection, idx_t count);
	RowGroup(RowGroupCollection &collection, RowGroupPointer pointer);
	RowGroup(RowGroupCollection &collection, PersistentRowGroupData &data);
	~RowGroup();

private:
	//! The RowGroupCollection this row-group is a part of
	reference<RowGroupCollection> collection;
	//! The version info of the row_group (inserted and deleted tuple info)
	shared_ptr<RowGroupVersionInfo> version_info;
	//! The column data of the row_group (mutable because `const` can lazily load)
	mutable vector<shared_ptr<ColumnData>> columns;

public:
	void MoveToCollection(RowGroupCollection &collection);
	RowGroupCollection &GetCollection() const {
		return collection.get();
	}
	//! Compute per-column metadata blocks by reading column metadata from disk
	PerColumnMetadataBlocks ComputePerColumnMetadataBlocks() const;

	const vector<MetaBlockPointer> &GetColumnStartPointers() const;

	vector<MetaBlockPointer> GetExtraMetadataBlockPointers() const;

	BlockManager &GetBlockManager() const;
	DataTableInfo &GetTableInfo() const;

	unique_ptr<RowGroup> AlterType(RowGroupCollection &collection, const LogicalType &target_type, idx_t changed_idx,
	                               ExpressionExecutor &executor, CollectionScanState &scan_state,
	                               SegmentNode<RowGroup> &node, DataChunk &scan_chunk, TransactionData transaction,
	                               ColumnStatistics &changed_stats);
	unique_ptr<RowGroup> AddColumn(RowGroupCollection &collection, ColumnDefinition &new_column,
	                               ExpressionExecutor &executor, ColumnStatistics &new_column_stats);
	unique_ptr<RowGroup> RemoveColumn(RowGroupCollection &collection, idx_t removed_column);

	//! Accumulates this row group's on-disk blocks into the drop state.
	void CommitDrop(CommitDropState &drop_state);
	//! Accumulates the given column's on-disk blocks into the drop state.
	void CommitDropColumn(const idx_t column_index, CommitDropState &drop_state);
	//! Drops every column's on-disk blocks and marks them as modified immediately.
	void CommitDrop();

	void InitializeEmpty(const vector<LogicalType> &types, ColumnDataType data_type);
	bool HasChanges(VisibilityBound bound) const;

	//! Initialize a scan over this row_group
	bool InitializeScan(CollectionScanState &state, SegmentNode<RowGroup> &node);
	bool InitializeScanWithOffset(CollectionScanState &state, SegmentNode<RowGroup> &node, idx_t vector_offset,
	                              bool initialize_columns = true);
	//! Initializes the column scans of the assignment set up by InitializeScanWithOffset
	void InitializeColumnScans(CollectionScanState &state);
	//! Checks the given set of table filters against the row-group statistics. Returns false if the entire row group
	//! can be skipped.
	bool CheckZonemap(optional_ptr<ClientContext> context, ScanFilterInfo &filters, idx_t row_start);
	//! Checks the given set of table filters against the per-segment statistics. Returns false if any segments were
	//! skipped.
	bool CheckZonemapSegments(CollectionScanState &state);
	void Scan(ScanOptions options, CollectionScanState &state, DataChunk &result);
	void Scan(CollectionScanState &state, DataChunk &result, TableScanType type);
	//! Synchronously prefetches the blocks required to scan the next row_count rows
	void PrefetchScanIO(CollectionScanState &state, idx_t row_count) const;
	//! Collects the async I/O tasks required to scan the next row_count rows, without performing any I/O
	vector<unique_ptr<AsyncTask>> CollectScanIOTasks(CollectionScanState &state, idx_t row_count) const;
	//! Rows of the remaining assignment worth prefetching, trailing vectors the zonemaps reject are left out
	idx_t PrefetchRowCount(CollectionScanState &state);
	//! Prepares the next eligible vector in the assigned range, idempotent, returns false when none remain
	bool PrepareScan(ScanOptions options, CollectionScanState &state);
	//! Processes the vector prepared by PrepareScan, clearing the prepared state when the vector is finished
	void ProcessPreparedScan(ScanOptions options, CollectionScanState &state, DataChunk &result);

	idx_t GetSelVector(ScanOptions options, idx_t vector_idx, SelectionVector &sel_vector, idx_t max_count);

	//! Bulk visibility check. For each offset in [0, count), writes the input index into `visible_sel` if that row is
	//! visible to the transaction. Returns the number of visible rows.
	idx_t Fetch(TransactionData transaction, const idx_t *offsets, idx_t count, SelectionVector &visible_sel);
	//! Bulk row fetch. For each `i` in [0, visible_count), fetches the row at `offsets[visible_sel.get_index(i)]`
	//! and writes every requested column into `result.data[col_idx][result_offset + i]`.
	void FetchRows(TransactionData transaction, ColumnFetchState &state, const vector<StorageIndex> &column_ids,
	               const idx_t *offsets, const SelectionVector &visible_sel, idx_t visible_count, DataChunk &result,
	               idx_t result_offset);

	//! Append count rows to the version info
	void AppendVersionInfo(TransactionData transaction, idx_t count);
	//! Commit a previous append made by RowGroup::AppendVersionInfo
	void CommitAppend(transaction_t commit_id, idx_t start, idx_t count);
	//! Revert a previous append made by RowGroup::AppendVersionInfo
	void RevertAppend(idx_t new_count);
	//! Clean up append states that can either be compressed or deleted
	void CleanupAppend(VisibilityBound lowest_visibility_bound, idx_t start, idx_t count);

	//! Delete the given set of rows in the version manager - optionally collects the ids of the rows that were deleted
	//! (input_offset is the position of row_ids in the row ids passed to the collection delete)
	idx_t Delete(TransactionData transaction, DuckTableEntry &table_entry, row_t *row_ids, idx_t count,
	             idx_t row_group_start, optional_ptr<DeletedRowIds> deleted_ids = nullptr, idx_t input_offset = 0);

	static vector<RowGroupWriteData> WriteToDisk(RowGroupWriteInfo &info,
	                                             const vector<const_reference<RowGroup>> &row_groups);
	//! Write the data inside this RowGroup to disk and return a struct with information about the write
	//! Including the new RowGroup that contains the columns in their written-to-disk form
	RowGroupWriteData WriteToDisk(RowGroupWriteInfo &info) const;
	//! Returns the number of committed rows (count - committed deletes)
	idx_t GetCommittedRowCount();
	//! Returns the number of rows visible to the given transaction
	idx_t GetVisibleRowCount(TransactionData transaction);
	//! Count visible rows in a scan assignment starting at a vector boundary
	idx_t GetVisibleRowCount(TransactionData transaction, idx_t start_vector, idx_t scan_count);
	bool CanReuseMetadata(RowGroupWriter &writer) const;
	RowGroupWriteData WriteToDisk(RowGroupWriter &writer);
	RowGroupPointer Checkpoint(RowGroupWriteData write_data, RowGroupWriter &writer, TableStatistics &global_stats,
	                           idx_t row_group_start);
	bool IsPersistent() const;
	PersistentRowGroupData SerializeRowGroupInfo(idx_t row_group_start) const;

	static void InitializeAppend(SegmentNode<RowGroup> &row_group, RowGroupAppendState &append_state);
	//! Continue a finalized append in a new row group, retaining unused transient block space
	static void InitializeNextAppend(SegmentNode<RowGroup> &row_group, RowGroupAppendState &append_state);
	void Append(RowGroupAppendState &append_state, DataChunk &chunk, idx_t append_count);
	void FinalizeAppend(RowGroupAppendState &append_state);

	void Update(TransactionData transaction, DuckTableEntry &table_entry, DataChunk &updates, row_t *ids, idx_t offset,
	            idx_t count, const vector<PhysicalIndex> &column_ids, idx_t row_group_start);
	//! Update a single column; corresponds to DataTable::UpdateColumn
	//! This method should only be called from the WAL
	void UpdateColumn(TransactionData transaction, DuckTableEntry &table_entry, DataChunk &updates, Vector &row_ids,
	                  idx_t offset, idx_t count, const vector<column_t> &column_path, idx_t row_group_start);

	void MergeStatistics(idx_t column_idx, const BaseStatistics &other);
	void MergeIntoStatistics(idx_t column_idx, BaseStatistics &other);
	void MergeIntoStatistics(TableStatistics &other);
	unique_ptr<BaseStatistics> GetStatistics(idx_t column_idx) const;
	unique_ptr<BaseStatistics> GetStatistics(const StorageIndex &column_idx) const;

	void GetColumnSegmentInfo(const QueryContext &context, idx_t row_group_index, vector<ColumnSegmentInfo> &result,
	                          const ColumnSegmentInfoScanOptions &options = ColumnSegmentInfoScanOptions {});
	static PartitionStatistics GetPartitionStats(SegmentNode<RowGroup> &row_group, TransactionData transaction);

	idx_t GetAllocationSize() const {
		return allocation_size;
	}

	void Verify();

	void NextVector(CollectionScanState &state);

	//! Writes a delete or an update into the version info of the rows (ids[i] - offset) and pushes its undo entry
	void WriteRows(TransactionData transaction, DuckTableEntry &table_entry, idx_t vector_idx, idx_t base_row,
	               const row_t ids[], idx_t count, row_t offset, bool is_update, WrittenRows &written);
	RowVersionManager &GetOrCreateVersionInfo();
	//! Loads the columns and deletes, which are otherwise read from metadata when first needed
	void LoadMetadata();

	// Serialization
	static void Serialize(RowGroupPointer &pointer, Serializer &serializer, bool supports_per_column_writes);
	static RowGroupPointer Deserialize(Deserializer &deserializer);

	idx_t GetRowGroupSize() const;

	static FilterPropagateResult CheckRowIdFilter(const TableFilter &filter, idx_t beg_row, idx_t end_row);
	idx_t GetColumnCount() const;

	vector<MetaBlockPointer> CheckpointDeletes(RowGroupWriter &writer);
	//! Attempts to compress the version information of the row group. Insert and delete ids that precede
	//! lowest_visibility_bound look the same to every active and future transaction, so they can be
	//! collapsed into constants
	void CompressVersionInfo(VisibilityBound lowest_visibility_bound);

	//! Direct accessors, fall outside of general use but can be useful to some extensions
	ColumnData &GetRawColumnData(const StorageIndex &c) const;
	ColumnData &GetRawColumnData(storage_t c) const;

private:
	//! Registers prefetch candidates for the next row_count rows, returns false when prefetching is not supported
	bool RegisterScanIO(CollectionScanState &state, idx_t row_count, PrefetchState &prefetch_state) const;
	//! Shared scan-state setup for InitializeScan and InitializeScanWithOffset
	bool InitializeScanInternal(CollectionScanState &state, SegmentNode<RowGroup> &node, idx_t vector_offset);
	//! Advances the scan past the current vector, clearing the prepared state
	void FinishVector(CollectionScanState &state);
	void InitializeAppendInternal(RowGroupAppendState &append_state);
	optional_ptr<RowVersionManager> GetVersionInfo();
	optional_ptr<RowVersionManager> GetVersionInfoIfLoaded() const;
	shared_ptr<RowVersionManager> GetOrCreateVersionInfoPtr();

	ColumnData &GetColumn(storage_t c) const;
	void LoadColumn(storage_t c) const;
	ColumnData &GetColumn(const StorageIndex &c) const;
	vector<shared_ptr<ColumnData>> &GetColumns();
	void LoadRowIdColumnData() const;
	void LoadRowNumberColumnData() const;
	void SetCount(idx_t count);
	bool ColumnIsLoaded(storage_t c) const;
	void UnloadColumn(storage_t c);
	bool HasUnchangedColumns() const;
	static shared_ptr<ColumnData> CheckpointColumn(const RowGroup &row_group, idx_t column_idx, RowGroupWriteInfo &info,
	                                               RowGroupWriteData &write_data);

	bool HasUnloadedDeletes() const;
	unique_ptr<RowGroup> CreateNewRowGroupCopy(RowGroupCollection &new_collection, idx_t new_column_count);

private:
	mutable mutex row_group_lock;
	vector<MetaBlockPointer> column_pointers;
	//! Whether or not each column is loaded (mutable because `const` can lazy load)
	mutable unique_ptr<atomic<bool>[]> is_loaded;
	bool has_metadata_blocks = false;
	vector<idx_t> extra_metadata_blocks;
	bool has_per_column_metadata_blocks = false;
	PerColumnMetadataBlocks per_column_metadata_blocks;
	atomic<idx_t> allocation_size;
	//! Append cursor, retained between appends and moved when appending to the next row group
	unique_ptr<SuballocationBlock> transient;
	//! The row id column data (mutable because `const` can lazy load)
	mutable unique_ptr<ColumnData> row_id_column_data;
	//! Whether or not `row_id_column_data` is loaded (mutable because `const` can lazy load)
	mutable atomic<bool> row_id_is_loaded;
	//! The row number column data (mutable because `const` can lazy load)
	mutable unique_ptr<ColumnData> row_number_column_data;
	//! Whether or not `row_number_column_data` is loaded (mutable because `const` can lazy load)
	mutable atomic<bool> row_number_is_loaded;
	atomic<bool> has_changes;
};

} // namespace duckdb
