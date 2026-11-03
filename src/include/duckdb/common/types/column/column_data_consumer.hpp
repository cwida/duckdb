//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/column/column_data_consumer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/types/column/column_data_collection_segment.hpp"
#include "duckdb/common/types/column/column_data_scan_states.hpp"

namespace duckdb {

struct ColumnDataConsumerScanState {
	ColumnDataAllocator *allocator = nullptr;
	ChunkManagementState current_chunk_state;
	idx_t chunk_index;
};

//! ColumnDataConsumer can scan a ColumnDataCollection, and consume it in the process, i.e., read blocks are deleted
class ColumnDataConsumer {
public:
	struct ChunkReference {
	public:
		ChunkReference(ColumnDataCollectionSegment *segment_p, uint32_t chunk_index_p);
		//! The minimum block id the chunk references, invalid for chunks stored entirely in metadata
		optional_idx GetMinimumBlockID() const;
		friend bool operator<(const ChunkReference &lhs, const ChunkReference &rhs) {
			// Sort by allocator first
			if (lhs.segment->allocator.get() != rhs.segment->allocator.get()) {
				return lhs.segment->allocator.get() < rhs.segment->allocator.get();
			}
			// Then by minimum block id, chunks without blocks sort last
			auto lhs_min_block_id = lhs.GetMinimumBlockID();
			auto rhs_min_block_id = rhs.GetMinimumBlockID();
			if (!lhs_min_block_id.IsValid() || !rhs_min_block_id.IsValid()) {
				return lhs_min_block_id.IsValid() && !rhs_min_block_id.IsValid();
			}
			return lhs_min_block_id.GetIndex() < rhs_min_block_id.GetIndex();
		}

	public:
		ColumnDataCollectionSegment *segment;
		uint32_t chunk_index_in_segment;
	};

public:
	ColumnDataConsumer(ColumnDataCollection &collection, vector<column_t> column_ids);

	idx_t Count() const {
		return collection.Count();
	}

	idx_t ChunkCount() const {
		return chunk_count;
	}

public:
	//! Initialize the scan of the ColumnDataCollection
	void InitializeScan();
	//! Assign a chunk to the scan state; the caller must release its handles if no chunk is assigned.
	bool AssignChunk(ColumnDataConsumerScanState &state);
	//! Scan the assigned chunk
	void ScanChunk(ColumnDataConsumerScanState &state, DataChunk &chunk) const;
	//! Indicate that scanning the chunk is done
	void FinishChunk(ColumnDataConsumerScanState &state);

private:
	void ConsumeChunks(idx_t delete_index_start, idx_t delete_index_end);

private:
	mutex lock;
	//! The collection being scanned
	ColumnDataCollection &collection;
	//! The column ids to scan
	vector<column_t> column_ids;
	//! The number of chunk references
	idx_t chunk_count;
	//! The chunks (in order) to be scanned
	vector<ChunkReference> chunk_references;
	//! Current index into "chunks"
	idx_t current_chunk_index;
	//! Chunks currently in progress
	unordered_set<idx_t> chunks_in_progress;
	//! The data has been consumed up to this chunk index
	idx_t chunk_delete_index;
};

} // namespace duckdb
