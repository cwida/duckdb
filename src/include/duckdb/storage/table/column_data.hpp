//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/table/column_data.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/data_pointer.hpp"
#include "duckdb/storage/table/persistent_table_data.hpp"
#include "duckdb/storage/statistics/segment_statistics.hpp"
#include "duckdb/storage/table/segment_tree.hpp"
#include "duckdb/storage/table/column_segment_tree.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/enums/scan_vector_type.hpp"
#include "duckdb/common/enums/scan_options.hpp"
#include "duckdb/common/serializer/serialization_traits.hpp"
#include "duckdb/common/atomic_ptr.hpp"

namespace duckdb {
class ColumnData;
class ExpressionFilter;
class ColumnSegment;
class DatabaseInstance;
class PartialBlockManager;
class DuckTableEntry;
struct ColumnCheckpointState;
struct ColumnFetchState;
struct ColumnSegmentInfo;
struct ColumnSegmentInfoScanOptions;
class RowGroup;
class RowGroupWriter;
class StorageManager;
class TableDataWriter;
class TableStorageInfo;
class UpdateSegment;
struct DataTableInfo;
struct PrefetchState;
struct RowGroupWriteInfo;
struct TransactionData;
struct PersistentColumnData;
class ValidityColumnData;
struct ColumnDataFinalizeAppendState;
struct SuballocationBlock;

using column_segment_vector_t = vector<SegmentNode<ColumnSegment>>;

struct ColumnCheckpointInfo {
	ColumnCheckpointInfo(RowGroupWriteInfo &info, idx_t column_idx);

	idx_t column_idx;

public:
	PartialBlockManager &GetPartialBlockManager();
	CompressionType GetCompressionType();

private:
	RowGroupWriteInfo &info;
};

enum class ColumnDataType { MAIN_TABLE, INITIAL_TRANSACTION_LOCAL, TRANSACTION_LOCAL, CHECKPOINT_TARGET };

class ColumnData : public enable_shared_from_this<ColumnData> {
	friend class ColumnDataCheckpointer;

public:
	ColumnData(BlockManager &block_manager, DataTableInfo &info, idx_t column_index, LogicalType type,
	           ColumnDataType data_type, optional_ptr<ColumnData> parent);
	virtual ~ColumnData();

	//! The count of the column data
	atomic<idx_t> count;
	//! The block manager
	BlockManager &block_manager;
	//! Table info for the column
	DataTableInfo &info;
	//! The column index of the column, either within the parent table or within the parent
	idx_t column_index;
	//! The type of the column
	LogicalType type;

public:
	virtual FilterPropagateResult CheckZonemap(ColumnScanState &state, TableFilter &filter,
	                                           optional_ptr<SegmentNode<ColumnSegment>> &checked_segment);

	BlockManager &GetBlockManager() const {
		return block_manager;
	}
	DatabaseInstance &GetDatabase() const;
	DataTableInfo &GetTableInfo() const;
	StorageManager &GetStorageManager() const;
	virtual idx_t GetMaxEntry();

	idx_t GetAllocationSize() const {
		return allocation_size;
	}
	optional_ptr<const CompressionFunction> GetCompressionFunction() const {
		return compression.get();
	}
	virtual void SetDataType(ColumnDataType data_type);
	ColumnDataType GetDataType() const {
		return data_type;
	}

	bool HasParent() const {
		return parent;
	}
	void SetParent(optional_ptr<ColumnData> parent) {
		this->parent = parent;
	}
	const ColumnData &Parent() const {
		D_ASSERT(HasParent());
		return *parent;
	}

	virtual const BaseStatistics &GetChildStats(const ColumnData &child) const;

	const LogicalType &GetType() const {
		return type;
	}
	ColumnSegmentTree &GetSegmentTree() {
		return data;
	}
	void SetCount(idx_t new_count) {
		this->count = new_count;
	}

	//! Whether or not the column has any updates
	bool HasUpdates() const;
	bool HasChanges(idx_t start_row, idx_t end_row) const;
	//! Whether or not the column has changes at this level
	bool HasChanges() const;

	//! Whether or not the column has ANY changes, including in child columns
	virtual bool HasAnyChanges() const;
	//! Whether or not we can scan an entire vector
	virtual ScanVectorType GetVectorScanType(ColumnScanState &state, idx_t scan_count, Vector &result);

	//! Initialize prefetch state with required I/O data for the next N rows
	virtual void InitializePrefetch(PrefetchState &prefetch_state, ColumnScanState &scan_state, idx_t rows);
	//! Initialize a scan of the column
	virtual void InitializeScan(ColumnScanState &state);
	//! Initialize a scan starting at the specified offset
	virtual void InitializeScanWithOffset(ColumnScanState &state, idx_t row_idx);
	//! Scan the next vector from the column
	idx_t Scan(TransactionData transaction, idx_t vector_index, ColumnScanState &state, Vector &result);
	virtual idx_t Scan(TransactionData transaction, idx_t vector_index, ColumnScanState &state, Vector &result,
	                   idx_t scan_count);

	virtual void ScanCommittedRange(idx_t row_group_start, idx_t offset_in_row_group, idx_t count, Vector &result);
	virtual idx_t ScanCount(ColumnScanState &state, Vector &result, idx_t count, idx_t result_offset = 0);

	//! Select
	virtual void Filter(TransactionData transaction, idx_t vector_index, ColumnScanState &state, Vector &result,
	                    SelectionVector &sel, idx_t &count, const TableFilter &filter, TableFilterState &filter_state);
	virtual void Select(TransactionData transaction, idx_t vector_index, ColumnScanState &state, Vector &result,
	                    SelectionVector &sel, idx_t count);

	//! Skip the scan forward by "count" rows
	virtual void Skip(ColumnScanState &state, idx_t count = STANDARD_VECTOR_SIZE);

	//! Initialize an appending phase for this column
	virtual void InitializeAppend(ColumnAppendState &state);
	//! Append a vector of type [type] to the end of the column
	virtual void Append(ColumnAppendState &state, const Vector &vector, idx_t count);
	virtual void AppendData(ColumnAppendState &state, UnifiedVectorFormat &vdata, idx_t count);
	//! Finalize appending
	virtual void FinalizeAppend(ColumnDataFinalizeAppendState &finalize_state, ColumnAppendState &state);
	void FinalizeAppend(optional_ptr<BaseStatistics> table_stats, ColumnAppendState &state);
	//! Finalize appending while holding stats_lock (for use by child column calls)
	void FinalizeAppendLocked(ColumnDataFinalizeAppendState &finalize_state, ColumnAppendState &state);
	//! Revert a set of appends to the ColumnData
	virtual void RevertAppend(row_t new_count);

	//! Fetch the vector from the column data that belongs to this specific row
	virtual idx_t Fetch(ColumnScanState &state, row_t row_id, Vector &result);
	//! Fetch a batch of row offsets and append them to the vector
	virtual void FetchRows(TransactionData transaction, ColumnFetchState &state, const StorageIndex &storage_index,
	                       const idx_t *offsets, const SelectionVector &sel, idx_t count, Vector &result,
	                       idx_t result_offset);
	//! Fetches a batch of row offsets for leaf columns.
	void FetchRowsAtSegmentLevel(TransactionData transaction, ColumnFetchState &state, const idx_t *offsets,
	                             const SelectionVector &sel, idx_t count, Vector &result, idx_t result_offset);
	//! Fetches the base data of a batch of row offsets, without updates
	void FetchBaseRows(const QueryContext &context, const idx_t *offsets, const SelectionVector &sel, idx_t count,
	                   Vector &result, idx_t result_offset);
	//! Reads the selected rows of a scan through FetchBaseRows (debug_force_fetch_row)
	void FetchScannedRows(ColumnScanState &state, const SelectionVector &sel, idx_t count, Vector &result,
	                      idx_t result_offset);

	virtual void Update(TransactionData transaction, DuckTableEntry &table_entry, idx_t column_index,
	                    Vector &update_vector, row_t *row_ids, idx_t update_count, idx_t row_group_start);
	virtual void UpdateColumn(TransactionData transaction, DuckTableEntry &table_entry,
	                          const vector<column_t> &column_path, Vector &update_vector, row_t *row_ids,
	                          idx_t update_count, idx_t depth, idx_t row_group_start);
	virtual unique_ptr<BaseStatistics> GetUpdateStatistics();

	virtual void VisitBlockIds(BlockIdVisitor &visitor) const;

	virtual unique_ptr<ColumnCheckpointState> CreateCheckpointState(const RowGroup &row_group,
	                                                                PartialBlockManager &partial_block_manager);
	//! If this is a nested column, "stats" are the corresponding statistics from the parent column
	//! Otherwise, "stats" == this->statistics->stats
	unique_ptr<ColumnCheckpointState> Checkpoint(const RowGroup &row_group, ColumnCheckpointInfo &info);
	virtual unique_ptr<ColumnCheckpointState> Checkpoint(const RowGroup &row_group, ColumnCheckpointInfo &info,
	                                                     const BaseStatistics &stats);

	virtual void CheckpointScan(ColumnSegment &segment, ColumnScanState &state, idx_t count, Vector &scan_vector) const;

	virtual bool IsPersistent();
	vector<DataPointer> GetDataPointers();

	virtual PersistentColumnData Serialize();
	void InitializeColumn(PersistentColumnData &column_data);
	virtual void InitializeColumn(PersistentColumnData &column_data, BaseStatistics &target_stats);
	static shared_ptr<ColumnData> Deserialize(BlockManager &block_manager, DataTableInfo &info, idx_t column_index,
	                                          ReadStream &source, const LogicalType &type);

	virtual void GetColumnSegmentInfo(const QueryContext &context, idx_t row_group_index, vector<idx_t> col_path,
	                                  vector<ColumnSegmentInfo> &result, const ColumnSegmentInfoScanOptions &options);
	virtual void Verify(RowGroup &parent);

	FilterPropagateResult CheckZonemap(optional_ptr<ClientContext> context, const StorageIndex &index,
	                                   TableFilter &filter);
	//! End of the last vector in [start_row, end_row) that the segment zonemaps do not reject for the filter
	idx_t ZonemapScanEnd(optional_ptr<ClientContext> context, idx_t start_row, idx_t end_row, TableFilter &filter);

	static shared_ptr<ColumnData> CreateColumn(BlockManager &block_manager, DataTableInfo &info, idx_t column_index,
	                                           const LogicalType &type,
	                                           ColumnDataType data_type = ColumnDataType::MAIN_TABLE,
	                                           optional_ptr<ColumnData> parent = nullptr);

	void MergeStatistics(const BaseStatistics &other);
	void MergeIntoStatistics(BaseStatistics &other);
	unique_ptr<BaseStatistics> GetStatistics() const;
	const BaseStatistics &GetStatisticsRef() const;

protected:
	//! Append a transient segment
	void AppendTransientSegment(SegmentLock &l, optional_ptr<SuballocationBlock> transient,
	                            optional_ptr<ColumnSegment> prev_segment);
	void AppendSegment(SegmentLock &l, unique_ptr<ColumnSegment> segment);

	void BeginScanVectorInternal(ColumnScanState &state);
	//! Scans a base vector from the column
	idx_t ScanVector(ColumnScanState &state, Vector &result, idx_t remaining, ScanVectorType scan_type,
	                 idx_t result_offset = 0);
	//! Scans a vector from the column merged with any potential updates
	idx_t ScanVector(TransactionData transaction, idx_t vector_index, ColumnScanState &state, Vector &result,
	                 idx_t target_scan, ScanVectorType scan_type, UpdateScanType update_type);
	idx_t ScanVector(TransactionData transaction, idx_t vector_index, ColumnScanState &state, Vector &result,
	                 idx_t target_scan, UpdateScanType update_type);
	void SelectVector(ColumnScanState &state, Vector &result, idx_t target_count, const SelectionVector &sel,
	                  idx_t sel_count, ScanVectorType scan_type, idx_t result_offset = 0);
	void FilterVector(ColumnScanState &state, Vector &result, idx_t target_count, SelectionVector &sel,
	                  idx_t &sel_count, const TableFilter &filter, TableFilterState &filter_state);

	void FetchUpdates(TransactionData transaction, idx_t vector_index, Vector &result, idx_t scan_count,
	                  UpdateScanType update_type);
	void FetchUpdateRow(TransactionData transaction, row_t row_id, Vector &result, idx_t result_idx);
	void UpdateInternal(TransactionData transaction, DuckTableEntry &table_entry, idx_t column_index,
	                    Vector &update_vector, row_t *row_ids, idx_t update_count, Vector &base_vector,
	                    idx_t row_group_start);
	idx_t FetchUpdateData(ColumnScanState &state, row_t *row_ids, Vector &base_vector, idx_t row_group_start);

	idx_t GetVectorCount(idx_t vector_index) const;

	static bool IsDirectNullCheckFilter(const TableFilter &filter);
	//! Checks the filter against the statistics of one segment
	FilterPropagateResult CheckSegmentStatistics(optional_ptr<ClientContext> context,
	                                             SegmentNode<ColumnSegment> &segment, ExpressionFilter &expr_filter);
	FilterPropagateResult CheckValidityZonemap(ColumnScanState &state, TableFilter &filter,
	                                           optional_ptr<SegmentNode<ColumnSegment>> &checked_segment,
	                                           ColumnData &validity_column);

private:
	void UpdateCompressionFunction(SegmentLock &l, const CompressionFunction &function);

protected:
	//! The segments holding the data of this column segment
	ColumnSegmentTree data;
	//! The lock for the updates
	mutable mutex update_lock;
	//! The updates for this column segment
	unique_ptr<UpdateSegment> updates;
	//! The lock for the stats
	mutable mutex stats_lock;
	//! Total transient allocation size
	atomic<idx_t> allocation_size;
	//! The stats of the root segment
	unique_ptr<SegmentStatistics> stats;

private:
	//! Whether or not this column data belongs to a main table or if it is transaction local
	atomic<ColumnDataType> data_type;
	//! The parent column (if any)
	optional_ptr<ColumnData> parent;
	//!	The compression function used by the ColumnData
	//! This is empty if the segments have mixed compression or the ColumnData is empty
	atomic_ptr<const CompressionFunction> compression;

public:
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

enum class ExtraPersistentColumnDataType : uint8_t {
	INVALID = 0,
	VARIANT = 1,
	GEOMETRY = 2,
};

class ExtraPersistentColumnData {
public:
	ExtraPersistentColumnDataType GetType() const {
		return type;
	}

	virtual ~ExtraPersistentColumnData() = default;

	template <class TARGET>
	TARGET &Cast() {
		D_ASSERT(type == TARGET::TYPE);
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		D_ASSERT(type == TARGET::TYPE);
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}

	void Serialize(Serializer &serializer) const;
	static unique_ptr<ExtraPersistentColumnData> Deserialize(Deserializer &deserializer);

protected:
	explicit ExtraPersistentColumnData(ExtraPersistentColumnDataType type_p) : type(type_p) {
	}

private:
	ExtraPersistentColumnDataType type;
};

class VariantPersistentColumnData final : public ExtraPersistentColumnData {
public:
	static constexpr auto TYPE = ExtraPersistentColumnDataType::VARIANT;
	VariantPersistentColumnData() : ExtraPersistentColumnData(TYPE) {
	}
	explicit VariantPersistentColumnData(const LogicalType &storage_type)
	    : ExtraPersistentColumnData(TYPE), logical_type(storage_type) {
	}

	LogicalType logical_type;
};

class GeometryPersistentColumnData final : public ExtraPersistentColumnData {
public:
	static constexpr auto TYPE = ExtraPersistentColumnDataType::GEOMETRY;
	GeometryPersistentColumnData() : ExtraPersistentColumnData(TYPE) {
	}
	explicit GeometryPersistentColumnData(GeometryStorageType storage_type)
	    : ExtraPersistentColumnData(TYPE), storage_type(storage_type) {
	}

	GeometryStorageType storage_type;
};

struct PersistentColumnData {
public:
	explicit PersistentColumnData(const LogicalType &logical_type);
	PersistentColumnData(const LogicalType &logical_type, vector<DataPointer> pointers);
	// disable copy constructors
	PersistentColumnData(const PersistentColumnData &other) = delete;
	PersistentColumnData &operator=(const PersistentColumnData &) = delete;
	//! enable move constructors
	PersistentColumnData(PersistentColumnData &&other) noexcept = default;
	PersistentColumnData &operator=(PersistentColumnData &&) = default;
	~PersistentColumnData();

public:
	void Serialize(Serializer &serializer) const;
	static PersistentColumnData Deserialize(Deserializer &deserializer);
	void DeserializeField(Deserializer &deserializer, field_id_t field_idx, const char *field_name,
	                      const LogicalType &type);
	bool HasUpdates() const;

public:
	LogicalType logical_type;
	vector<DataPointer> pointers;
	vector<PersistentColumnData> child_columns;
	bool has_updates = false;

	//! Extra persistent data for specific column types
	unique_ptr<ExtraPersistentColumnData> extra_data;
};

struct PersistentRowGroupData {
	explicit PersistentRowGroupData(vector<LogicalType> types);
	PersistentRowGroupData() = default;
	// disable copy constructors
	PersistentRowGroupData(const PersistentRowGroupData &other) = delete;
	PersistentRowGroupData &operator=(const PersistentRowGroupData &) = delete;
	//! enable move constructors
	PersistentRowGroupData(PersistentRowGroupData &&other) noexcept = default;
	PersistentRowGroupData &operator=(PersistentRowGroupData &&) = default;
	~PersistentRowGroupData() = default;

	vector<LogicalType> types;
	vector<PersistentColumnData> column_data;
	idx_t start;
	idx_t count;

	void Serialize(Serializer &serializer) const;
	static PersistentRowGroupData Deserialize(Deserializer &deserializer);
	bool HasUpdates() const;
};

struct PersistentCollectionData {
	PersistentCollectionData() = default;
	// disable copy constructors
	PersistentCollectionData(const PersistentCollectionData &other) = delete;
	PersistentCollectionData &operator=(const PersistentCollectionData &) = delete;
	//! enable move constructors
	PersistentCollectionData(PersistentCollectionData &&other) noexcept = default;
	PersistentCollectionData &operator=(PersistentCollectionData &&) = default;
	~PersistentCollectionData() = default;

	vector<PersistentRowGroupData> row_group_data;

	void Serialize(Serializer &serializer) const;
	static PersistentCollectionData Deserialize(Deserializer &deserializer);
	bool HasUpdates() const;
	vector<block_id_t> GetBlockIds() const;
};

} // namespace duckdb
