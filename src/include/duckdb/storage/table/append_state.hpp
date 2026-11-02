//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/table/append_state.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/vector.hpp"
#include "duckdb/function/compression_function.hpp"
#include "duckdb/planner/bound_constraint.hpp"
#include "duckdb/storage/storage_lock.hpp"
#include "duckdb/storage/table/table_statistics.hpp"
#include "duckdb/transaction/transaction_data.hpp"

namespace duckdb {
class ColumnSegment;
class DataTable;
class LocalTableStorage;
class RowGroup;
class UpdateSegment;
class TableCatalogEntry;
template <class T>
struct SegmentNode;
class RowGroupSegmentTree;

struct TableAppendState;

struct SuballocationBlock {
	//! The current block being allocated from.
	shared_ptr<BlockHandle> block;
	//! The block id
	block_id_t block_id = INVALID_BLOCK;
	//! The offset into the block
	idx_t allocated = 0;

	unique_ptr<ColumnSegment> CreateTransientSegment(DatabaseInstance &db, const CompressionFunction &function,
	                                                 const LogicalType &type, const idx_t segment_size,
	                                                 BlockManager &block_manager);
};

struct ColumnAppendState {
	//! The current segment of the append
	optional_ptr<SegmentNode<ColumnSegment>> current;
	//! Child append states
	vector<ColumnAppendState> child_appends;
	//! The write lock that is held by the append
	unique_ptr<StorageLockKey> lock;
	//! The compression append state
	unique_ptr<CompressionAppendState> append_state;
	//! Stats for the append to the current segment
	unique_ptr<BaseStatistics> append_stats;
	//! Stats for the full append to this column
	unique_ptr<BaseStatistics> full_append_stats;
	//! The optional block to use for transient allocations
	optional_ptr<SuballocationBlock> transient;

public:
	void InitializeStats(const LogicalType &type);
	void FlushSegmentStats();
	void FinalFlush(vector<reference<BaseStatistics>> &global_stats);
};

struct ColumnDataFinalizeAppendState {
	explicit ColumnDataFinalizeAppendState(BaseStatistics &table_stats) {
		global_stats.emplace_back(table_stats);
	}
	ColumnDataFinalizeAppendState(BaseStatistics &table_stats, BaseStatistics &column_data_stats) {
		global_stats.emplace_back(table_stats);
		global_stats.emplace_back(column_data_stats);
	}
	ColumnDataFinalizeAppendState(ColumnDataFinalizeAppendState &parent, LogicalTypeId type_transform,
	                              optional_idx child_id = optional_idx());

	vector<reference<BaseStatistics>> global_stats;
};

struct RowGroupAppendState {
	explicit RowGroupAppendState(TableAppendState &parent_p);
	~RowGroupAppendState();

	//! The parent append state
	TableAppendState &parent;
	//! The current row_group we are appending to
	optional_ptr<SegmentNode<RowGroup>> row_group;
	//! The column append states
	unsafe_unique_array<ColumnAppendState> states;
	//! Offset within the row_group
	idx_t offset_in_row_group;
};

struct TableAppendState {
	TableAppendState();
	~TableAppendState();

	RowGroupAppendState row_group_append_state;
	unique_lock<mutex> append_lock;
	row_t row_start;
	row_t current_row;
	//! The total number of rows appended by the append operation
	idx_t total_append_count;
	idx_t row_group_start;
	//! The row group segment tree we are appending to
	shared_ptr<RowGroupSegmentTree> row_groups;
	//! The first row-group that has been appended to
	optional_ptr<SegmentNode<RowGroup>> start_row_group;
	//! The transaction data
	TransactionData transaction;
	//! Table statistics gathered during the Append phase - flushed to the table in FinalizeAppend
	TableStatistics stats;
	//! Cached hash vector
	Vector hashes;
};

struct ConstraintState {
	explicit ConstraintState(TableCatalogEntry &table_p, const vector<unique_ptr<BoundConstraint>> &bound_constraints)
	    : table(table_p), bound_constraints(bound_constraints) {
	}

	TableCatalogEntry &table;
	const vector<unique_ptr<BoundConstraint>> &bound_constraints;
};

struct LocalAppendState {
	TableAppendState append_state;
	LocalTableStorage *storage;
	unique_ptr<ConstraintState> constraint_state;
};

} // namespace duckdb
