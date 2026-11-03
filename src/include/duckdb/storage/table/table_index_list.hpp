//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/table/table_index_list.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/mutex.hpp"
#include "duckdb/common/enums/index_removal_type.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/shared_ptr_ipp.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/storage/table/index_entry.hpp"
#include "duckdb/execution/index/bound_index.hpp"
#include "duckdb/storage/index.hpp"

namespace duckdb {

struct BlockIdVisitor;
class ConflictManager;
class ConflictInfo;
class IndexEntry;
class TableIndexList;
template <class TARGET>
class IndexReadHandle;
template <class TARGET>
class IndexWriteHandle;
class IndexBinder;
struct IndexStorageInfo;
struct DataTableInfo;
struct IndexCheckpointResult;
template <class T>
class TableIndexIterationHelper;
class TableIndexWriter;

class TableIndexList {
public:
	~TableIndexList();

	//! Iterates over shared ownership of stable index entries while holding the entry-list lock.
	TableIndexIterationHelper<shared_ptr<IndexEntry>> IndexEntries() const;
	//! Adds an index entry to the list of index entries, and returns it.
	shared_ptr<IndexEntry> AddIndex(unique_ptr<Index> index, idx_t index_oid, ConstraintCheckMode check_mode);
	//! Visits the on-disk blocks of every index, once per use
	void VisitBlockIds(BlockIdVisitor &visitor);
	//! Disowns the on-disk blocks of every index: destroying an index no longer frees them
	void DisownBlocks();
	//! Initializes the transaction-local delete and append indexes.
	void InitializeLocalIndexes(TableIndexList &delete_indexes, TableIndexList &append_indexes) const;
	//! Appends a chunk to all index entries.
	void Append(DataChunk &chunk, Vector &row_ids);
	//! Appends a table chunk with generated row IDs, using delete and checkpoint indexes where required.
	ErrorData Append(optional_ptr<TableIndexList> delete_indexes, DataChunk &chunk, row_t row_start,
	                 IndexAppendMode append_mode, optional_idx active_checkpoint);
	//! Reverts an append to all index entries.
	void RevertAppend(DataChunk &chunk, Vector &row_ids);
	//! Reverts an append with generated row IDs starting at row_start.
	void RevertAppend(DataChunk &chunk, row_t row_start);
	//! Appends deleted rows to all unique indexes.
	void AppendToDeleteIndexes(DataChunk &chunk, Vector &row_ids);
	//! Applies a removal or removal rollback to all index entries.
	void RemoveFromIndexes(DataChunk &chunk, Vector &row_ids, IndexRemovalType removal_type,
	                       optional_idx active_checkpoint = optional_idx());
	//! Removes an index entry from the list of index entries and release any storage the index owns.
	void RemoveIndex(idx_t index_oid);
	//! Removes an index entry without releasing it: checkpoints still write it until RemoveDetachedIndex
	shared_ptr<IndexEntry> DetachIndex(idx_t index_oid);
	//! Removes an index entry detached by DetachIndex and releases any storage the index owns.
	void RemoveDetachedIndex(const IndexEntry &entry);
	//! Returns the entry matching the index OID.
	shared_ptr<IndexEntry> FindEntry(const IndexEntry &index) const;
	shared_ptr<IndexEntry> FindEntry(idx_t index_oid) const;
	//! Binds unbound indexes possibly present after loading an extension.
	void Bind(ClientContext &context, DataTableInfo &table_info, const optional<string> &index_type = {});
	//! Returns true, if there are no index entries.
	bool Empty() const {
		return Count() == 0;
	}
	//! Returns the number of index entries.
	idx_t Count() const {
		annotated_lock_guard lock(index_entries_lock);
		return index_entries.size();
	}
	//! Returns true, if there are unbound indexes.
	bool HasUnbound() const {
		annotated_lock_guard lock(index_entries_lock);
		return unbound_count != 0;
	}
	//! Returns true if an unbound index has buffered WAL operations.
	bool HasBufferedReplays() const;
	//! Returns true, if there are unique indexes.
	bool HasUniqueIndexes() const;
	//! Verifies all unique ART indexes that are not deferred, optionally recording conflicts.
	void VerifyUniqueIndexes(optional_ptr<const TableIndexList> delete_indexes, DataChunk &chunk,
	                         optional_ptr<ConflictManager> manager) const;
	//! Vacuums all bound indexes.
	void Vacuum();
	//! Rebuilds all indexes with chunks supplied by the scan callback.
	void Rebuild(const IndexRebuildScan &scan);
	//! Verifies the buffers of all bound indexes and their delete deltas.
	void VerifyBuffers() const;
	//! Verifies that no index is updated by the given columns.
	void VerifyUpdate(const vector<PhysicalIndex> &column_ids) const;
	//! Returns table storage metadata for all indexes.
	vector<IndexInfo> GetStorageInfo() const;
	//! Returns the combined in-memory size of all bound indexes.
	idx_t GetInMemorySize() const;
	//! Returns the set of distinct index types across all bound indexes.
	unordered_set<string> DistinctIndexTypes() const;
	//! Returns true if every index is bound and has the given type (vacuously true for an empty list).
	bool AllIndexesBoundOfType(const string &index_type) const;
	//! Overwrite this list with the other list.
	void Move(TableIndexList &other) DUCKDB_NO_THREAD_SAFETY_ANALYSIS {
		D_ASSERT(this != &other);
		annotated_unique_lock lock(index_entries_lock, std::defer_lock);
		annotated_unique_lock other_lock(other.index_entries_lock, std::defer_lock);
		std::lock(lock, other_lock);
		D_ASSERT(index_entries.empty());
		index_entries = std::move(other.index_entries);
		unbound_count = other.unbound_count;
		other.unbound_count = 0;
	}
	//! Merge any changes added to deltas during a checkpoint back into the main indexes
	void MergeCheckpointDeltas(optional_idx checkpoint_id) const;
	//! Returns true, if all indexes
	//! Find the foreign key matching the keys.
	shared_ptr<IndexEntry> FindForeignKeyIndex(const vector<PhysicalIndex> &fk_keys, const ForeignKeyType fk_type);
	//! Verify a foreign key constraint.
	void VerifyForeignKey(optional_ptr<const TableIndexList> delete_indexes, const vector<PhysicalIndex> &fk_keys,
	                      DataChunk &chunk, ConflictManager &conflict_manager);
	//! Persist the buffers of the indexes the writer asks for and install replacements, returning their metadata.
	vector<shared_ptr<const IndexStorageInfo>> Checkpoint(TableIndexWriter &writer);
	//! Returns the physical table columns referenced by any index.
	unordered_set<column_t> GetIndexedColumns() const;
	//! Returns the column sets of unique indexes matching the conflict target.
	vector<unordered_set<column_t>> GetConflictTargetColumns(const ConflictInfo &conflict_info) const;
	//! Get the combined column ids of the unique indexes.
	unordered_set<column_t> GetUniqueIndexColumns() const;
	//! Serializes the index matching the OID for the write-ahead log, if it exists.
	unique_ptr<IndexStorageInfo> SerializeToWAL(idx_t index_oid, const StorageVersion version);

public:
	//! Initialize an index_chunk from a table.
	static void InitializeIndexChunk(DataChunk &index_chunk, const vector<LogicalType> &table_types,
	                                 vector<StorageIndex> &mapped_column_ids, DataTableInfo &data_table_info);
	//! Reference the indexed columns of a table chunk.
	static void ReferenceIndexChunk(DataChunk &table_chunk, DataChunk &index_chunk,
	                                vector<StorageIndex> &mapped_column_ids);

private:
	//! Removes an index entry from the list of index entries, and returns it.
	shared_ptr<IndexEntry> ExtractIndex(idx_t index_oid) DUCKDB_REQUIRES(index_entries_lock);
	//! Returns the index entries with these OIDs, also among the ones that a drop detached.
	vector<shared_ptr<IndexEntry>> FindEntries(const unordered_set<idx_t> &index_oids) const
	    DUCKDB_REQUIRES(index_entries_lock);

	template <class>
	friend class TableIndexIterationHelper;

	//! A lock to prevent any concurrent changes to the index entries.
	mutable annotated_mutex index_entries_lock;
	//! The index entries of the table.
	vector<shared_ptr<IndexEntry>> index_entries DUCKDB_GUARDED_BY(index_entries_lock);
	//! Contains the number of unbound indexes.
	idx_t unbound_count DUCKDB_GUARDED_BY(index_entries_lock) = 0;
	//! Index entries detached by a commit, which checkpoints with an earlier bound still write
	vector<shared_ptr<IndexEntry>> detached_index_entries DUCKDB_GUARDED_BY(index_entries_lock);
};

template <class T>
class TableIndexIterationHelper {
public:
	explicit TableIndexIterationHelper(const TableIndexList &index_list);

private:
	annotated_unique_lock<annotated_mutex> lock;
	const vector<shared_ptr<IndexEntry>> &index_entries;

private:
	class TableIndexIterator {
	public:
		explicit TableIndexIterator(optional_ptr<const vector<shared_ptr<IndexEntry>>> index_entries);

		optional_ptr<const vector<shared_ptr<IndexEntry>>> index_entries;
		optional_idx index;

	public:
		TableIndexIterator &operator++();
		bool operator!=(const TableIndexIterator &other) const;
		T operator*() const;
	};

public:
	TableIndexIterator begin() { // NOLINT: match stl API
		return TableIndexIterator(&index_entries);
	}
	TableIndexIterator end() { // NOLINT: match stl API
		return TableIndexIterator(nullptr);
	}
};

template <>
shared_ptr<IndexEntry> TableIndexIterationHelper<shared_ptr<IndexEntry>>::TableIndexIterator::operator*() const;

} // namespace duckdb
