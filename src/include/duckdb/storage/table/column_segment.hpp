//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/table/column_segment.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/scan_vector_type.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/function/compression_function.hpp"
#include "duckdb/storage/buffer/block_handle.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/statistics/segment_statistics.hpp"
#include "duckdb/storage/table/segment_base.hpp"

namespace duckdb {

class BaseStatistics;
class BlockManager;
class ColumnData;
class ColumnSegment;
class DatabaseInstance;
class TableFilter;
class Transaction;
class UpdateSegment;
struct ColumnAppendState;
struct ColumnFetchState;
struct ColumnScanState;
struct PrefetchState;
struct TableFilterState;

enum class ColumnSegmentType : uint8_t { TRANSIENT, PERSISTENT };
//! TableFilter represents a filter pushed down into the table scan.

class ColumnSegment : public SegmentBase<ColumnSegment> {
public:
	//! Construct a column segment.
	ColumnSegment(DatabaseInstance &db, shared_ptr<BlockHandle> block, const ColumnSegmentType segment_type,
	              const idx_t count, const CompressionFunction &function_p, BaseStatistics statistics,
	              const block_id_t block_id_p, const idx_t offset, const idx_t segment_size_p,
	              unique_ptr<ColumnSegmentState> segment_state_p = nullptr,
	              optional<uint32_t> data_size_in_bytes = optional<uint32_t>());
	//! Construct a column segment from another column segment.
	//! The other column segment becomes invalid (std::move).
	ColumnSegment(ColumnSegment &other);
	~ColumnSegment();

public:
	static unique_ptr<ColumnSegment> CreatePersistentSegment(DatabaseInstance &db, BlockManager &block_manager,
	                                                         DataPointer &data_pointer);
	static unique_ptr<ColumnSegment> CreateTransientSegment(DatabaseInstance &db, const CompressionFunction &function,
	                                                        const LogicalType &type, const idx_t segment_size,
	                                                        BlockManager &block_manager);

public:
	void InitializePrefetch(PrefetchState &prefetch_state, ColumnScanState &scan_state);
	void InitializeScan(ColumnScanState &state);
	//! Scan one vector from this segment
	void Scan(ColumnScanState &state, idx_t scan_count, Vector &result, idx_t result_offset, ScanVectorType scan_type);
	//! Scan a subset of a vector (defined by the selection vector)
	void Select(ColumnScanState &state, idx_t scan_count, Vector &result, const SelectionVector &sel, idx_t sel_count,
	            idx_t result_offset, ScanVectorType scan_type);
	//! Scan one vector while applying a filter to the vector, returning only the matching elements
	void Filter(ColumnScanState &state, idx_t scan_count, Vector &result, SelectionVector &sel, idx_t &sel_count,
	            const TableFilter &filter, TableFilterState &filter_state);
	//! Fetch a value of the specific row id and append it to the result
	void FetchRow(ColumnFetchState &state, row_t row_id, Vector &result, idx_t result_idx);

	[[deprecated("UnifiedVectorFormat parameter is ignored")]] static idx_t
	FilterSelection(SelectionVector &sel, Vector &vector, UnifiedVectorFormat &vdata, const TableFilter &filter,
	                TableFilterState &filter_state, idx_t scan_count, idx_t &approved_tuple_count);
	static idx_t FilterSelection(SelectionVector &sel, Vector &vector, TableFilterState &filter_state, idx_t scan_count,
	                             idx_t &approved_tuple_count);

	//! Skip a scan forward to the row_index specified in the scan state
	void Skip(ColumnScanState &state);

	// The maximum size of the buffer (in bytes)
	idx_t SegmentSize() const;
	//! Resize the block
	void Resize(idx_t segment_size);
	const CompressionFunction &GetCompressionFunction();

	//! Initialize an append of this segment. Appends are only supported on transient segments.
	void InitializeAppend(ColumnAppendState &state);
	//! Appends a (part of) vector to the segment, returns the amount of entries successfully appended
	idx_t Append(ColumnAppendState &state, UnifiedVectorFormat &data, idx_t offset, idx_t count);
	//! Finalize the segment for appending - no more appends can follow on this segment
	//! The segment should be compacted as much as possible
	//! Returns the number of bytes occupied within the segment
	idx_t FinalizeAppend(ColumnAppendState &state);
	//! Revert an append made to this segment
	void RevertAppend(idx_t new_count);

	//! Convert a transient in-memory segment to a persistent segment backed by an on-disk block.
	//! Only used during checkpointing.
	void ConvertToPersistent(QueryContext context, optional_ptr<BlockManager> block_manager, const block_id_t block_id);
	//! Updates pointers to refer to the given block and offset. This is only used
	//! when sharing a block among segments. This is invoked only AFTER the block is written.
	void MarkAsPersistent(shared_ptr<BlockHandle> block, uint32_t offset_in_block);
	//! Read the copy in the partial block from now on, so the segment's own buffer can be freed.
	void UsePackedCopy(shared_ptr<BlockHandle> block, uint32_t offset_in_block);
	void SetBlock(shared_ptr<BlockHandle> block, uint32_t offset);
	//! Gets a data pointer from a persistent column segment
	DataPointer GetDataPointer(idx_t row_start);

	block_id_t GetBlockId() const {
		D_ASSERT(segment_type == ColumnSegmentType::PERSISTENT);
		return block_id;
	}

	const LogicalType &GetType() const {
		return stats.statistics.GetType();
	}

	//! Returns the size of the underlying block of the segment. It is size is the size available for usage on a block.
	idx_t GetBlockSize() const {
		return block->GetBlockSize();
	}

	idx_t GetBlockOffset() const {
		return offset;
	}

	const optional<uint32_t> &GetDataSize() const {
		return data_size_in_bytes;
	}

	void SetByteSize(uint32_t data_size_in_bytes_p) {
		data_size_in_bytes = data_size_in_bytes_p;
	}

	optional_ptr<CompressedSegmentState> GetSegmentState() const {
		return segment_state.get();
	}

	DatabaseInstance &GetDatabase() const {
		return db;
	}

	ColumnSegmentType GetSegmentType() const {
		return segment_type;
	}

	void SetSegmentType(ColumnSegmentType type) {
		segment_type = type;
	}

	shared_ptr<BlockHandle> &GetBlockHandle() {
		return block;
	}

	void VisitBlockIds(BlockIdVisitor &visitor) const;

	const BaseStatistics &GetStats() const {
		return stats.statistics;
	}

	BaseStatistics &GetStatsMutable() {
		return stats.statistics;
	}

private:
	void Scan(ColumnScanState &state, idx_t scan_count, Vector &result);
	void ScanPartial(ColumnScanState &state, idx_t scan_count, Vector &result, idx_t result_offset);

private:
	//! The database instance
	DatabaseInstance &db;
	//! The column segment type (transient or persistent)
	ColumnSegmentType segment_type;
	//! The block that this segment relates to
	shared_ptr<BlockHandle> block;
	//! The compression function
	reference<const CompressionFunction> function;
	//! The block id that this segment relates to (persistent segment only)
	block_id_t block_id;
	//! The offset into the block (persistent segment only)
	idx_t offset;
	//! The allocated segment size, which is bounded by Storage::BLOCK_SIZE
	idx_t segment_size;
	//! Number of bytes occupied by this segment within its block, if recorded
	optional<uint32_t> data_size_in_bytes;
	//! Storage associated with the compressed segment
	unique_ptr<CompressedSegmentState> segment_state;
	//! The statistics for the segment
	SegmentStatistics stats;
};

} // namespace duckdb
