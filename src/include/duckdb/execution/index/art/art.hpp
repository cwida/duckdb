//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/index/art/art.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/array.hpp"
#include "duckdb/execution/index/bound_index.hpp"
#include "duckdb/execution/index/index_type.hpp"
#include "duckdb/execution/index/art/node.hpp"

namespace duckdb {

enum class VerifyExistenceType : uint8_t { APPEND = 0, APPEND_FK = 1, DELETE_FK = 2 };
enum class ARTConflictType : uint8_t { NO_CONFLICT = 0, CONSTRAINT = 1 };
enum class ARTSerializationFormat : uint8_t { V1_0_0 = 0, CURRENT = 1 };

//! Result of collecting row IDs; capacity exhaustion cannot be resumed.
enum class ARTSearchResult : uint8_t { COMPLETED, CAPACITY_EXCEEDED };

class ConflictManager;
class ARTKey;
class ARTKeySection;
class RowIdVectorOutput;
class FixedSizeAllocator;

struct ARTIndexScanState;

struct DeleteIndexInfo {
	DeleteIndexInfo() : delete_indexes(nullptr) {
	}
	explicit DeleteIndexInfo(vector<reference<const BoundIndex>> &delete_indexes) : delete_indexes(delete_indexes) {
	}

	optional_ptr<vector<reference<const BoundIndex>>> delete_indexes;
};

class ART : public BoundIndex {
public:
	friend class Leaf;

public:
	//! Index type name for the ART.
	static constexpr const char *TYPE_NAME = "ART";
	//! FixedSizeAllocator count of the ART.
	static constexpr uint8_t ALLOCATOR_COUNT = 9;
	//! FixedSizeAllocator count of deprecated ARTs.
	static constexpr uint8_t DEPRECATED_ALLOCATOR_COUNT = ALLOCATOR_COUNT - 3;

public:
	using AllocatorArray = array<unsafe_unique_ptr<FixedSizeAllocator>, ALLOCATOR_COUNT>;

public:
	ART(const Identifier &name, const IndexConstraintType index_constraint_type, const vector<column_t> &column_ids,
	    TableIOManager &table_io_manager, const vector<unique_ptr<Expression>> &unbound_expressions,
	    AttachedDatabase &db, const shared_ptr<AllocatorArray> &allocators_ptr = nullptr,
	    const IndexStorageInfo &info = IndexStorageInfo());

	//! Create a index instance of this type.
	static unique_ptr<BoundIndex> Create(CreateIndexInput &input) {
		auto art = make_uniq<ART>(input.name, input.constraint_type, input.column_ids, input.table_io_manager,
		                          input.unbound_expressions, input.db, nullptr, input.storage_info);
		return std::move(art);
	}

	static IndexType GetARTIndexType();

	//! Root of the tree.
	NodePtr root_ptr = NodePtr();
	//! Fixed-size allocators holding the ART nodes.
	shared_ptr<AllocatorArray> allocators;
	//! True, if the ART owns its data.
	bool owns_data;
	//! Storage version that the ART was created in, used for backwards compatible key generation
	StorageVersion storage_version;

public:
	//! Try to initialize a scan on the ART with the given expression and filter.
	unique_ptr<IndexScanState> TryInitializeScan(const Expression &expr, const Expression &filter_expr) const;
	//! Initializes a scan for multiple equality lookup values.
	unique_ptr<IndexScanState> InitializeBatchScan(unique_ptr<DataChunk> key_columns) const;
	//! Scans the ART and appends matching row IDs to the output collection.
	//! Returns true on completion, or false and clears the entire collection if its capacity is exceeded.
	bool Scan(IndexScanState &state, RowIdVectorOutput &row_ids) const DUCKDB_EXCLUDES(lock);

	//! Simple merge: scan source ART and delete each (key, rowid) from this ART.
	// FIXME: replace with structural tree delete merge.
	void RemovalMerge(IndexLock &state, BoundIndex &source_index) DUCKDB_REQUIRES(state);
	//! Obtains a lock and calls RemovalMerge while holding that lock.
	void RemovalMerge(BoundIndex &source_index) DUCKDB_EXCLUDES(lock);
	//! Simple merge: scan source ART and insert each (key, rowid) into this ART.
	//! Returns error data if constraint violation.
	// FIXME: This is only used in MergeCheckpointDeltas, and even then it is used in lieu of the existing
	// MergeIndexes which don't support deprecated leaf chains. Once support for that is added, this simpler insert
	// merge may be removed.
	ErrorData InsertMerge(IndexLock &state, BoundIndex &source_index, IndexAppendMode append_mode)
	    DUCKDB_REQUIRES(state);
	//! Obtains a lock and calls InsertMerge while holding that lock.
	ErrorData InsertMerge(BoundIndex &source_index, IndexAppendMode append_mode) DUCKDB_EXCLUDES(lock);

	//! Appends data to the locked index.
	ErrorData Append(IndexLock &l, DataChunk &chunk, Vector &row_ids) override DUCKDB_REQUIRES(l);
	//! Appends data to the locked index and verifies constraint violations.
	ErrorData Append(IndexLock &l, DataChunk &chunk, Vector &row_ids, IndexAppendInfo &info) override
	    DUCKDB_REQUIRES(l);

	//! Insert a chunk.
	ErrorData Insert(IndexLock &l, DataChunk &chunk, Vector &row_ids) override DUCKDB_REQUIRES(l);
	//! Insert a chunk and verify constraint violations (generates keys and calls InsertKeys which does the
	//! verification).
	ErrorData Insert(IndexLock &l, DataChunk &data, Vector &row_ids, IndexAppendInfo &info) override DUCKDB_REQUIRES(l);
	//! Insert keys and row_ids into ART and verify constraint violations.
	ErrorData InsertKeys(ArenaAllocator &arena, unsafe_vector<ARTKey> &keys, unsafe_vector<ARTKey> &row_id_keys,
	                     idx_t count, const DeleteIndexInfo &delete_info, IndexAppendMode append_mode,
	                     optional_ptr<DataChunk> chunk = nullptr);

	//! Verify that data can be appended to the index without a constraint violation.
	void VerifyAppend(DataChunk &chunk, IndexAppendInfo &info, optional_ptr<ConflictManager> manager) override
	    DUCKDB_EXCLUDES(lock);

	//! Delete a chunk from the ART.
	idx_t TryDelete(IndexLock &state, DataChunk &entries, Vector &row_identifiers,
	                optional_ptr<SelectionVector> deleted_sel, optional_ptr<SelectionVector> non_deleted_sel) override
	    DUCKDB_REQUIRES(state);
	//! Delete keys and row_ids from the ART.
	idx_t DeleteKeys(unsafe_vector<ARTKey> &keys, unsafe_vector<ARTKey> &row_id_keys, idx_t count,
	                 optional_ptr<SelectionVector> deleted_sel = nullptr,
	                 optional_ptr<SelectionVector> non_deleted_sel = nullptr);

	//! Reset all ART storage.
	void ResetStorage(IndexLock &index_lock) override DUCKDB_REQUIRES(index_lock);

	//! Build an ART from a vector of sorted keys and their row IDs.
	ARTConflictType Build(unsafe_vector<ARTKey> &keys, unsafe_vector<ARTKey> &row_ids, const idx_t row_count);

	//! Merge another ART into this locked ART; the caller must have exclusive access to the source.
	//! FIXME: Return ARTConflictType instead of a boolean.
	bool MergeIndexes(IndexLock &state, BoundIndex &other_index) override DUCKDB_REQUIRES(state);

	//! Vacuums the ART storage.
	void Vacuum(IndexLock &state) override DUCKDB_REQUIRES(state);

	//! Serializes ART memory to the WAL and returns the ART storage information.
	IndexStorageInfo SerializeToWAL(StorageVersion target_version) override;
	//! ART checkpoints are deferred so mutations can be routed through checkpoint deltas.
	IndexCheckpointMode GetCheckpointMode() const override {
		return IndexCheckpointMode::DEFERRED;
	}
	IndexCheckpointResult Checkpoint(PartialBlockManager &partial_block_manager, const StorageVersion version) override;

	//! Returns the in-memory usage of the ART.
	idx_t GetInMemorySize(IndexLock &index_lock) const override DUCKDB_REQUIRES(index_lock);

	bool SupportsDeltaIndexes() const override;

protected:
	unique_ptr<BoundIndex> CreateEmptyCopy(IndexConstraintType constraint_type) const override;
	ErrorData MergeCheckpointDelta(IndexDeltaType type, BoundIndex &delta_index) override;

public:
	//! ART key generation.
	template <bool IS_NOT_NULL = false>
	void GenerateKeys(ArenaAllocator &allocator, DataChunk &input, unsafe_vector<ARTKey> &keys) const;
	void GenerateKeyVectors(ArenaAllocator &allocator, DataChunk &input, const Vector &row_ids,
	                        unsafe_vector<ARTKey> &keys, unsafe_vector<ARTKey> &row_id_keys);
	//! Returns true if this ART may hold GEOMETRY keys in the legacy pre-v1.5.0 encoding,
	//! which key generation cannot reproduce byte-identically.
	bool HasLegacyGeometryKeys() const;

	//! Verifies the nodes.
	void Verify(IndexLock &l) override DUCKDB_REQUIRES(l);
	//! Verifies that the node allocations match the node counts.
	void VerifyAllocations(IndexLock &l) override DUCKDB_REQUIRES(l);
	//! Verifies the index buffers.
	void VerifyBuffers(IndexLock &l) override DUCKDB_REQUIRES(l);

	//! Returns string representation of the ART.
	string ToString(IndexLock &l, bool display_ascii = false) override DUCKDB_REQUIRES(l);

	//! Returns the configured prefix byte capacity.
	uint8_t PrefixCount() const {
		return prefix_count;
	}

private:
	//! Constructor for creating a shadow index based on the source index.
	ART(const ART &src, shared_ptr<AllocatorArray> allocators_ptr);
	//! The number of bytes fitting in the prefix.
	uint8_t prefix_count;

	//! Returns how many allocators are used based on the target serialization format.
	static uint8_t GetAllocatorCount(ARTSerializationFormat format);

	ARTSearchResult ScanInternal(IndexScanState &state, RowIdVectorOutput &row_ids) const;
	ARTSearchResult ScanRange(ARTIndexScanState &scan_state, RowIdVectorOutput &row_ids) const;
	ARTSearchResult ScanBatch(DataChunk &input, RowIdVectorOutput &row_ids) const;
	ARTSearchResult SearchEqual(const ARTKey &key, RowIdVectorOutput &row_ids) const;
	ARTSearchResult SearchGreater(const ARTKey &key, bool equal, RowIdVectorOutput &row_ids) const;
	ARTSearchResult SearchLess(const ARTKey &upper_bound, bool equal, RowIdVectorOutput &row_ids) const;
	ARTSearchResult SearchCloseRange(const ARTKey &lower_bound, const ARTKey &upper_bound, bool left_equal,
	                                 bool right_equal, RowIdVectorOutput &row_ids) const;

	string GenerateErrorKeyName(DataChunk &input, idx_t row) const;
	string GenerateConstraintErrorMessage(VerifyExistenceType verify_type, const string &key_name) const;
	void VerifyLeaf(const NodePtr &leaf_ptr, const ARTKey &key, DeleteIndexInfo delete_index_info,
	                ConflictManager &manager, optional_idx &conflict_idx, idx_t i) const;
	void VerifyConstraint(DataChunk &chunk, IndexAppendInfo &info, ConflictManager &manager) override
	    DUCKDB_EXCLUDES(lock);
	string GetConstraintViolationMessage(VerifyExistenceType verify_type, idx_t failed_index,
	                                     DataChunk &input) const override DUCKDB_EXCLUDES(lock);

	void InitializeMergeUpperBounds(unsafe_vector<idx_t> &upper_bounds);
	void InitializeMerge(NodePtr &other_root_ptr, unsafe_vector<idx_t> &upper_bounds);

	void InitializeVacuum(unordered_set<uint8_t> &indexes);
	void FinalizeVacuum(const unordered_set<uint8_t> &indexes);

	void InitAllocators(const IndexStorageInfo &info);
	void TransformToDeprecated();
	//! Gathers metadata used at serialization, optionally transforms the in-memory ART to a deprecated representation
	//! based on the target serialization format.
	IndexStorageInfo PrepareSerialize(ARTSerializationFormat target_format);
	//! Get the serialization format of the ART based on the active storage version.
	static ARTSerializationFormat GetSerializationFormat(StorageVersion storage_version);
	void Deserialize(const BlockPointer &pointer);
	void SetPrefixCount(const IndexStorageInfo &info);

	string ToStringInternal(bool display_ascii);
	void VerifyInternal();
	void VerifyAllocationsInternal();
};

template <>
void ART::GenerateKeys<>(ArenaAllocator &allocator, DataChunk &input, unsafe_vector<ARTKey> &keys) const;

template <>
void ART::GenerateKeys<true>(ArenaAllocator &allocator, DataChunk &input, unsafe_vector<ARTKey> &keys) const;

} // namespace duckdb
