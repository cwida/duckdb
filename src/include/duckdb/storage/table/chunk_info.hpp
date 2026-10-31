//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/table/chunk_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/index/index_pointer.hpp"
#include "duckdb/common/enums/scan_options.hpp"
#include "duckdb/transaction/transaction_data.hpp"
#include "duckdb/common/types/validity_mask.hpp"

namespace duckdb {
class RowGroup;
struct SelectionVector;
class Transaction;
struct TransactionData;
struct DeleteInfo;
struct WrittenRows;
class Serializer;
class Deserializer;
class FixedSizeAllocator;

//! The type of a serialized chunk info entry
//! In-memory all chunk infos are represented by ChunkVectorInfo - CONSTANT_INFO (a fully deleted vector) and
//! EMPTY_INFO (no deletes) only exist as tags in the serialized format
enum class ChunkInfoType : uint8_t { CONSTANT_INFO, VECTOR_INFO, EMPTY_INFO };

//! ChunkVectorInfo holds the version information (the insert and delete ids) of the rows of a single vector within
//! a row group. The insert and delete ids are stored as constants when all rows share the same id, and as per-row
//! id arrays otherwise.

//! The result of a CompressVersionIds pass over a vector
enum class VersionCompressionResult : uint8_t {
	//! No per-row id arrays remain - there is nothing left to compress
	FULLY_COMPRESSED,
	//! Some ids are not yet visible to all transactions - compression can succeed once the
	//! lowest active start advances past them, without any further modifications
	PENDING,
	//! Per-row arrays remain that cannot compress without further modifications
	//! (some rows are not deleted)
	SETTLED
};

//! The storage state of the delete side of a ChunkVectorInfo. Exhaustive and mutually exclusive:
//! CONSTANT - all rows share constant_delete_id
//! MASKED   - the rows in deleted_mask share mask_delete_id, the others constant_delete_id (never a delete)
//! ARRAY    - per-row ids in deleted_data
enum class DeleteIdState : uint8_t { CONSTANT, MASKED, ARRAY };

class ChunkVectorInfo {
public:
	explicit ChunkVectorInfo(FixedSizeAllocator &allocator, idx_t start, transaction_t insert_id = 0);
	ChunkVectorInfo(FixedSizeAllocator &allocator, idx_t start, transaction_t insert_id, transaction_t delete_id);
	~ChunkVectorInfo();

	//! The row index of the first row
	idx_t start;

public:
	//! Gets up to max_count entries from the chunk info. If the ret is 0>ret>max_count, the selection vector is filled
	//! with the tuples
	idx_t GetSelVector(ScanOptions options, optional_ptr<SelectionVector> sel_vector, idx_t max_count) const;
	idx_t GetRowCount(ScanOptions options, idx_t max_count);
	//! Returns whether or not a single row in the ChunkVectorInfo should be used or not for the given transaction
	bool Fetch(TransactionData transaction, row_t row);
	void CommitAppend(transaction_t commit_id, idx_t start, idx_t end);
	bool Cleanup(VisibilityBound lowest_visibility_bound) const;
	string ToString(idx_t max_count) const;

	void Append(idx_t start, idx_t end, transaction_t commit_id);
	//! Clears the insert ids of the rows from start onwards, which no longer hold appended rows
	void RevertAppend(idx_t start);

	//! Writes the delete id or update marker into the rows (ids[i] - offset), and outputs what it wrote
	void WriteRows(TransactionData transaction, const row_t ids[], idx_t count, row_t offset, bool is_update,
	               WrittenRows &written);
	//! Replaces the delete id and update marker of old_id on the rows by those of new_id (on commit and revert)
	void CommitWrite(transaction_t old_id, transaction_t new_id, const uint16_t rows[], idx_t count);
	//! Rolls back a write: the rows get back the ids they held before
	void RollbackWrite(const uint16_t rows[], idx_t count, const transaction_t previous_ids[], idx_t previous_id_count);

	//! Attempts to compress the per-row insert/delete ids into constants. Ids that precede
	//! lowest_visibility_bound look the same to every active and future transaction, so they can collapse
	VersionCompressionResult CompressVersionIds(VisibilityBound lowest_visibility_bound);
	//! Whether a compression pass could achieve anything for this vector (see recheck_compression)
	bool RecheckCompression() const {
		return recheck_compression;
	}
	//! Verifies (in DEBUG) that a disarmed recheck_compression matches the actual ids: nothing may be
	//! compressible now or in the future without a modification that re-arms the check
	void VerifyCachedCompressionState() const;

	bool HasDeletes(VisibilityBound visibility_bound = VisibilityBound::IncludingUncommitted()) const;
	bool HasUncommittedChanges() const;
	bool AnyDeleted() const;
	bool HasConstantInsertionId() const;
	transaction_t ConstantInsertId() const;
	bool HasConstantDeleteId() const;
	transaction_t ConstantDeleteId() const;

	void Write(WriteStream &writer, VisibilityBound visibility_bound) const;
	static unique_ptr<ChunkVectorInfo> Read(FixedSizeAllocator &allocator, ReadStream &reader);

private:
	template <class INSERT_OP, class DELETE_OP>
	idx_t TemplatedGetSelVector(const SnapshotView &view, optional_ptr<SelectionVector> sel_vector,
	                            idx_t max_count) const;

	IndexPointer GetInsertedPointer() const;
	IndexPointer GetDeletedPointer() const;
	IndexPointer GetInitializedInsertedPointer();
	IndexPointer GetInitializedDeletedPointer();
	//! Frees the per-row delete ids (if any)
	void FreeDeleteData();
	//! ARRAY -> MASKED: record alive rows as invalid bits, free the per-row delete array. mask_id is the
	//! shared committed id of the deleted rows (0 when they are already visible to all transactions)
	void CompressDeleteToMask(transaction_t mask_id);
	//! MASKED -> ARRAY: re-materialize the per-row delete array from the bitmask
	void DecompressDeleteMask();
	//! The delete id or update marker of a row of a vector without per-row ids
	transaction_t GetDeleteId(idx_t row) const;
	//! Whether the rows (0, 1, ..., count - 1 if rows is nullptr) are all rows outside of the mask
	bool RowsAreAllOutsideMask(const uint16_t rows[], idx_t count) const;
	//! Gives the rows (0, 1, ..., count - 1 if rows is nullptr) the id, in per-row ids only if the mask cannot hold it
	void AssignId(const uint16_t rows[], idx_t count, transaction_t id);
	//! Sets the ids of the rows to new_id(id, i), compressing them into a constant if all rows end up with the same one
	template <class F>
	void SetIds(const uint16_t rows[], idx_t count, F &&new_id);

private:
	FixedSizeAllocator &allocator;
	//! The transaction ids of the transactions that inserted the tuples (if any)
	IndexPointer inserted_data;
	//! The constant insert id (if there is only one)
	transaction_t constant_insert_id;

	//! The per-row delete ids (or update markers) of the tuples (if any)
	IndexPointer deleted_data;
	//! The delete id or update marker of the rows outside of the mask (all rows in the CONSTANT state)
	transaction_t constant_delete_id;
	//! Bitmask used in the MASKED state: valid bit == the row holds mask_delete_id, invalid bit == constant_delete_id.
	//! Matches the on-disk VECTOR_INFO orientation. Only meaningful when delete_state == DeleteIdState::MASKED.
	ValidityMask deleted_mask;
	//! The id of the rows in the mask in the MASKED state (0 for deletes visible to all, as read from disk)
	transaction_t mask_delete_id = 0;
	//! The current delete-side storage state - the single source of truth for the delete side
	DeleteIdState delete_state = DeleteIdState::CONSTANT;
	//! Whether a compression pass could achieve anything for this vector: armed by any id modification,
	//! disarmed when a pass compresses the vector fully or finds it settled (live rows block the collapse
	//! until a further delete re-arms it). CompressVersionIds returns the cached SETTLED without
	//! re-scanning the ids while this is false.
	bool recheck_compression = true;
};

} // namespace duckdb
