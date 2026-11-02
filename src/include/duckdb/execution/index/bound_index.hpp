//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/index/bound_index.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/error_data.hpp"
#include "duckdb/common/mutex.hpp"

#include "duckdb/execution/index/unbound_index.hpp"
#include "duckdb/common/enums/index_constraint_type.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/storage/index.hpp"

namespace duckdb {
class BoundIndex;

class ClientContext;
class TableIOManager;
class Transaction;
class ConflictManager;
class IndexDeltas;
class IndexEntry;

struct DUCKDB_CAPABILITY("mutex") DUCKDB_SCOPED_CAPABILITY IndexLock;
struct IndexScanState;

enum class IndexAppendMode : uint8_t { DEFAULT = 0, IGNORE_DUPLICATES = 1, INSERT_DUPLICATES = 2 };

class IndexAppendInfo {
public:
	IndexAppendInfo() : append_mode(IndexAppendMode::DEFAULT) {
	}
	IndexAppendInfo(const IndexAppendMode append_mode, optional_ptr<BoundIndex> delete_index)
	    : append_mode(append_mode) {
		if (delete_index) {
			delete_indexes.push_back(*delete_index);
		}
	}

public:
	IndexAppendMode append_mode;
	vector<reference<const BoundIndex>> delete_indexes;
};

//! Identifies the logical purpose of a delta owned by an index entry.
enum class IndexDeltaType : uint8_t {
	DELETED_ROWS_IN_USE,
	ADDED_DATA_DURING_CHECKPOINT,
	REMOVED_DATA_DURING_CHECKPOINT
};

//! The index is an abstract base class that serves as the basis for indexes
class BoundIndex : public Index {
	friend struct IndexLock;

public:
	BoundIndex(const Identifier &name, const string &index_type, IndexConstraintType index_constraint_type,
	           const vector<column_t> &column_ids, TableIOManager &table_io_manager,
	           const vector<unique_ptr<Expression>> &unbound_expressions, AttachedDatabase &db);

	//! The physical types stored in the index
	vector<PhysicalType> types;
	//! The logical types of the expressions
	vector<LogicalType> logical_types;

	//! The name of the index
	Identifier name;
	//! The index type (ART, B+-tree, Skip-List, ...)
	string index_type;
	//! The index constraint type
	IndexConstraintType index_constraint_type;

	//! The vector of unbound expressions, which are later turned into bound expressions.
	//! We need to store the unbound expressions, as we might not always have the context
	//! available to bind directly.
	//! The leaves of these unbound expressions are BoundColumnRefExpressions.
	//! These BoundColumnRefExpressions contain a binding (ColumnBinding),
	//! and that contains a table_index and a column_index.
	//! The table_index is a dummy placeholder.
	//! The column_index indexes the column_ids vector in the Index base class.
	//! Those column_ids store the physical table indexes of the Index,
	//! and we use them when binding the unbound expressions.
	vector<unique_ptr<Expression>> unbound_expressions;

public:
	bool IsBound() const override {
		return true;
	}
	const string &GetIndexType() const override {
		return index_type;
	}
	const Identifier &GetIndexName() const override {
		return name;
	}
	IndexConstraintType GetConstraintType() const override {
		return index_constraint_type;
	}
	idx_t UnboundExpressionCount() const;
	unique_ptr<Expression> CopyUnboundExpression(idx_t index) const;
	const vector<LogicalType> &GetLogicalTypes() const {
		return logical_types;
	}

public:
	//! Appends data to the locked index.
	virtual ErrorData Append(IndexLock &l, DataChunk &chunk, Vector &row_ids) DUCKDB_REQUIRES(l) = 0;
	//! Obtains a lock and calls Append while holding that lock.
	ErrorData Append(DataChunk &chunk, Vector &row_ids) DUCKDB_EXCLUDES(lock);
	//! Appends data to the locked index and verifies constraint violations.
	virtual ErrorData Append(IndexLock &l, DataChunk &chunk, Vector &row_ids, IndexAppendInfo &info) DUCKDB_REQUIRES(l);
	//! Obtains a lock and calls Append while holding that lock.
	ErrorData Append(DataChunk &chunk, Vector &row_ids, IndexAppendInfo &info) DUCKDB_EXCLUDES(lock);

	//! Verify that data can be appended to the index without a constraint violation.
	virtual void VerifyAppend(DataChunk &chunk, IndexAppendInfo &info, optional_ptr<ConflictManager> manager)
	    DUCKDB_EXCLUDES(lock);
	//! Verifies the constraint for a chunk of data.
	virtual void VerifyConstraint(DataChunk &chunk, IndexAppendInfo &info, ConflictManager &manager)
	    DUCKDB_EXCLUDES(lock);

	//! Resets all index storage, clearing the index entirely. The index lock must be held.
	virtual void ResetStorage(IndexLock &index_lock) DUCKDB_REQUIRES(index_lock) = 0;
	//! Obtains a lock and calls ResetStorage while holding that lock.
	void ResetStorage() override DUCKDB_EXCLUDES(lock);

	//! Delete a chunk of entries from the index. The index lock must be held.
	//! Returns the amount of rows successfully deleted from the index.
	//! If either deleted_sel or non_deleted_sel are provided the exact rows that were (not) deleted are written there
	virtual idx_t TryDelete(IndexLock &state, DataChunk &entries, Vector &row_identifiers,
	                        optional_ptr<SelectionVector> deleted_sel = nullptr,
	                        optional_ptr<SelectionVector> non_deleted_sel = nullptr) DUCKDB_REQUIRES(state);
	//! Obtains a lock and calls TryDelete while holding that lock
	idx_t TryDelete(DataChunk &entries, Vector &row_identifiers, optional_ptr<SelectionVector> deleted_sel = nullptr,
	                optional_ptr<SelectionVector> non_deleted_sel = nullptr) DUCKDB_EXCLUDES(lock);
	//! Delete a chunk of entries from the index. The index lock must be held.
	//! Throws an error if not all rows are deleted
	virtual void Delete(IndexLock &state, DataChunk &entries, Vector &row_identifiers) DUCKDB_REQUIRES(state);
	//! Obtains a lock and calls Delete while holding that lock
	void Delete(DataChunk &entries, Vector &row_identifiers) DUCKDB_EXCLUDES(lock);

	//! Insert a chunk.
	virtual ErrorData Insert(IndexLock &l, DataChunk &chunk, Vector &row_ids) DUCKDB_REQUIRES(l) = 0;
	//! Insert a chunk and verifies constraint violations.
	virtual ErrorData Insert(IndexLock &l, DataChunk &chunk, Vector &row_ids, IndexAppendInfo &info) DUCKDB_REQUIRES(l);

	//! Merge another index into this index while holding the index lock.
	//! The caller must have exclusive access to the source index.
	virtual bool MergeIndexes(IndexLock &state, BoundIndex &other_index) DUCKDB_REQUIRES(state) = 0;
	//! Obtains a lock and calls MergeIndexes while holding that lock
	bool MergeIndexes(BoundIndex &other_index) DUCKDB_EXCLUDES(lock);

	//! Performs a full traversal of the ART while vacuuming the qualifying nodes.
	//! The index lock must be held.
	virtual void Vacuum(IndexLock &l) DUCKDB_REQUIRES(l) = 0;
	//! Obtains a lock and calls Vacuum while holding that lock.
	void Vacuum() DUCKDB_EXCLUDES(lock);

	//! Whether or not the index supports the creation of delta indexes
	virtual bool SupportsDeltaIndexes() const;

	//! Returns the in-memory usage of the index. The index lock must be held
	virtual idx_t GetInMemorySize(IndexLock &state) const DUCKDB_REQUIRES(state) = 0;
	//! Returns the in-memory usage of the index
	idx_t GetInMemorySize() const DUCKDB_EXCLUDES(lock);

	//! Returns the string representation of an index, or only traverses and verifies the index.
	virtual void Verify(IndexLock &l) DUCKDB_REQUIRES(l) = 0;
	//! Obtains a lock and calls VerifyAndToString.
	void Verify() DUCKDB_EXCLUDES(lock);

	//! Returns the string representation of an index.
	virtual string ToString(IndexLock &l, bool display_ascii = false) DUCKDB_REQUIRES(l) = 0;
	//! Obtains a lock and calls ToString.
	string ToString(bool display_ascii = false) DUCKDB_EXCLUDES(lock);

	//! Ensures that the node allocation counts match the node counts.
	virtual void VerifyAllocations(IndexLock &l) DUCKDB_REQUIRES(l) = 0;
	//! Obtains a lock and calls VerifyAllocations.
	void VerifyAllocations() DUCKDB_EXCLUDES(lock);

	//! Verify the index buffers.
	virtual void VerifyBuffers(IndexLock &l) DUCKDB_REQUIRES(l);
	//! Obtains a lock and calls VerifyBuffers.
	void VerifyBuffers() DUCKDB_EXCLUDES(lock);

	//! Returns true if the index is affected by updates on the specified column IDs, and false otherwise
	bool IndexIsUpdated(const vector<PhysicalIndex> &column_ids) const;

	//! Serializes index memory to the WAL, the caller must have exclusive access to the index.
	virtual IndexStorageInfo SerializeToWAL(StorageVersion storage_version);

	//! Execute the index expressions on an input chunk
	void ExecuteExpressions(DataChunk &input, DataChunk &result) const;
	static string AppendRowError(DataChunk &input, idx_t index);

	//! Throw a constraint violation exception
	virtual string GetConstraintViolationMessage(VerifyExistenceType verify_type, idx_t failed_index,
	                                             DataChunk &input) const DUCKDB_EXCLUDES(lock) = 0;

	//! Replay index insert and delete operations buffered during WAL replay.
	//! table_types has the physical types of the table in the order they appear, not logical (no generated columns).
	//! mapped_column_ids maps each buffered data column to its physical table column.
	void ApplyBufferedReplays(const vector<LogicalType> &table_types, BufferedIndexReplays &buffered_replays,
	                          const vector<StorageIndex> &mapped_column_ids);

protected:
	friend class IndexDeltas;
	friend class IndexEntry;

	//! Creates an empty physical copy with the requested constraint type.
	virtual unique_ptr<BoundIndex> CreateEmptyCopy(IndexConstraintType constraint_type) const;
	//! Merges a checkpoint delta into this index.
	virtual ErrorData MergeCheckpointDelta(IndexDeltaType type, BoundIndex &delta_index);

	//! Lock used for any changes to the index
	mutable annotated_mutex lock;

	//! The vector of bound expressions to generate the Index keys based on a data chunk.
	//! The leaves of the bound expressions are BoundReferenceExpressions.
	//! These BoundReferenceExpressions contain offsets into the DataChunk to retrieve the columns
	//! for the expression.
	//!	With these offsets into the DataChunk, the expression executor can now evaluate the expression
	//! on incoming data chunks to generate the keys.
	vector<unique_ptr<Expression>> bound_expressions;

private:
	//! Expression executor to execute the index expressions
	mutable ExpressionExecutor executor;

	//! Bind the unbound expressions of the index
	unique_ptr<Expression> BindExpression(unique_ptr<Expression> expr);
};

} // namespace duckdb
