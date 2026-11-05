//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/index/unbound_index.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/parsed_data/create_index_info.hpp"
#include "duckdb/storage/index.hpp"
#include "duckdb/storage/index_storage_info.hpp"
#include "duckdb/storage/storage_index.hpp"

namespace duckdb {

class BoundIndex;
class ColumnDataCollection;
class DataChunk;
class IndexBinder;

enum class BufferedIndexReplay : uint8_t { INSERT_ENTRY = 0, DEL_ENTRY = 1 };

struct ReplayRange {
	BufferedIndexReplay type;
	// [start, end) - start is inclusive, end is exclusive for the range within the ColumnDataCollection
	// buffer for operations to replay for this range.
	idx_t start;
	idx_t end;
	explicit ReplayRange(const BufferedIndexReplay replay_type, const idx_t start_p, const idx_t end_p)
	    : type(replay_type), start(start_p), end(end_p) {
	}
};

// All inserts and deletes to be replayed are stored in their respective buffers.
// Since the inserts and deletes may be interleaved, however, ranges stores the ordering of operations
// and their offsets in the respective buffer.
// Simple example:
// ranges[0] - INSERT_ENTRY, [0,6)
// ranges[1] - DEL_ENTRY,    [0,3)
// ranges[2] - INSERT_ENTRY  [6,12)
// So even though the buffered_inserts has all the insert data from [0,12), ranges gives us the intervals for
// replaying the index operations in the right order.
struct BufferedIndexReplays {
	vector<ReplayRange> ranges;
	unique_ptr<ColumnDataCollection> buffered_inserts;
	unique_ptr<ColumnDataCollection> buffered_deletes;

	BufferedIndexReplays() = default;

	unique_ptr<ColumnDataCollection> &GetBuffer(const BufferedIndexReplay replay_type) {
		if (replay_type == BufferedIndexReplay::INSERT_ENTRY) {
			return buffered_inserts;
		}
		return buffered_deletes;
	}

	bool HasBufferedReplays() const {
		return !ranges.empty();
	}
};

class UnboundIndex : public Index {
private:
	//! The CreateInfo of the index.
	unique_ptr<CreateInfo> create_info;
	//! The serialized storage information of the index.
	//! This information is owned solely by the unbound index, except during checkpointing.
	shared_ptr<const IndexStorageInfo> storage_info;

	//! Buffered for index operations during WAL replay. They are replayed upon index binding.
	BufferedIndexReplays buffered_replays;

	//! Physical table columns stored in each buffered replay chunk, in buffer order.
	//! Derived from this index's column IDs at construction, deduplicated and sorted.
	vector<StorageIndex> mapped_column_ids;

	//! Whether persistent blocks referenced by `storage_info` have been reclaimed
	//! or handed off to a bound index.
	bool storage_reclaimed = false;

public:
	UnboundIndex(unique_ptr<CreateInfo> create_info, IndexStorageInfo storage_info, TableIOManager &table_io_manager,
	             AttachedDatabase &db);
	~UnboundIndex() override;

public:
	void ResetStorage() override;

	bool IsBound() const override {
		return false;
	}
	const string &GetIndexType() const override {
		return GetCreateInfo().index_type;
	}
	const Identifier &GetIndexName() const override {
		return GetCreateInfo().GetIndexName();
	}
	IndexConstraintType GetConstraintType() const override {
		return GetCreateInfo().constraint_type;
	}
	const CreateIndexInfo &GetCreateInfo() const {
		return create_info->Cast<CreateIndexInfo>();
	}
	const IndexStorageInfo &GetStorageInfo() const {
		return *storage_info;
	}
	const vector<unique_ptr<ParsedExpression>> &GetParsedExpressions() const {
		return GetCreateInfo().parsed_expressions;
	}
	const Identifier &GetTableName() const {
		return GetCreateInfo().table;
	}
	IndexCheckpointResult Checkpoint(PartialBlockManager &partial_block_manager, StorageVersion version) override;

	//! Buffers an insert or delete (replay_type) chunk, to be replayed once the index is bound.
	//! table_chunk uses physical table layout: data[j] holds physical column j. It may be sparse,
	//! but all columns required by this index must be populated.
	void BufferChunk(DataChunk &table_chunk, Vector &row_ids, BufferedIndexReplay replay_type);
	unique_ptr<BoundIndex> Bind(IndexBinder &binder, const vector<LogicalType> &physical_column_types);
	bool HasBufferedReplays() const {
		return buffered_replays.HasBufferedReplays();
	}

	BufferedIndexReplays &GetBufferedReplays() {
		return buffered_replays;
	}

	const vector<StorageIndex> &GetMappedColumnIds() const {
		return mapped_column_ids;
	}
};

} // namespace duckdb
