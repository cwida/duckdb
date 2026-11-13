//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/index.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/index_constraint_type.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/storage/storage_info.hpp"

namespace duckdb {

class AttachedDatabase;
class TableIOManager;
class TableIndexWriter;
class PartialBlockManager;
struct IndexCheckpointResult;

//! Describes how an index participates in a checkpoint when persisting data to the filesystem.
//!
//! IMMEDIATE (default): flush buffers under the index lock.
//! DEFERRED: share blocks across indexes and finish flushing after releasing the lock. Return a logically equivalent
//! shadow with independent backing for modified buffers; keep the live index usable until installation after flushing.
//! Concurrent changes use deltas, requiring SupportsDeltaIndexes(), CreateEmptyCopy(), and MergeCheckpointDelta().
enum class IndexCheckpointMode : uint8_t { IMMEDIATE, DEFERRED };

//! The index is an abstract base class that serves as the basis for indexes
class Index {
protected:
	Index(const vector<column_t> &column_ids, TableIOManager &table_io_manager, AttachedDatabase &db);

	//! The physical column ids of the indexed columns.
	//! For example, given a table with the following columns:
	//! (a INT, gen AS (2 * a), b INT, c VARCHAR), an index on columns (a,c) would have physical
	//! column_ids [0,2] (since the virtual column is skipped in the physical representation).
	//! Also see comments in bound_index.hpp to see how these column IDs are used in the context of
	//! bound/unbound expressions.
	//! Note that these are the columns for this Index, not all Indexes on the table.
	vector<column_t> column_ids;
	//! Unordered set of column_ids used by the Index
	unordered_set<column_t> column_id_set;

public:
	//! Associated table io manager
	TableIOManager &table_io_manager;
	//! Attached database instance
	AttachedDatabase &db;

public:
	virtual ~Index() = default;

	//! Returns true if the index is a bound index, and false otherwise
	virtual bool IsBound() const = 0;

	//! The index type (ART, B+-tree, Skip-List, ...)
	virtual const string &GetIndexType() const = 0;

	//! The name of the index
	virtual const Identifier &GetIndexName() const = 0;

	//! The index constraint type
	virtual IndexConstraintType GetConstraintType() const = 0;

	//! Returns how this index participates in a checkpoint, defaults to IMMEDIATE mode.
	virtual IndexCheckpointMode GetCheckpointMode() const {
		return IndexCheckpointMode::IMMEDIATE;
	}

	//! Checkpoint an index, based on the mode supported by the index flushing may be deferred.
	virtual IndexCheckpointResult Checkpoint(PartialBlockManager &partial_block_manager, const StorageVersion version);

	//! Returns unique flag
	bool IsUnique() const {
		auto type = GetConstraintType();
		return type == IndexConstraintType::UNIQUE || type == IndexConstraintType::PRIMARY;
	}

	//! Returns primary key flag
	bool IsPrimary() const {
		auto index_constraint_type = GetConstraintType();
		return (index_constraint_type == IndexConstraintType::PRIMARY);
	}

	//! Returns foreign key flag
	bool IsForeign() const {
		auto index_constraint_type = GetConstraintType();
		return (index_constraint_type == IndexConstraintType::FOREIGN);
	}

	const vector<column_t> &GetColumnIds() const {
		return column_ids;
	}

	const unordered_set<column_t> &GetColumnIdSet() const {
		return column_id_set;
	}

	virtual void ResetStorage() = 0;

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

} // namespace duckdb
