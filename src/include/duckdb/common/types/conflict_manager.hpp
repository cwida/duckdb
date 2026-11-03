//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/conflict_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/identifier.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/execution/index/art/art.hpp"

namespace duckdb {

class Index;
class IndexEntry;
class ConflictInfo;
class DataTable;
class DuckTransaction;
class LocalStorage;

enum class ConflictManagerMode : uint8_t {
	SCAN, // Gather conflicts without throwing a constraint exception.
	THROW // Throw on the conflicts that were not found during the scan.
};

enum class LookupResultType : uint8_t { LOOKUP_MISS, LOOKUP_HIT, LOOKUP_NULL };

//! The conflict manager tracks conflicts during constraint verification.
//! It decides when/if to throw, and records conflicts to be used later in case
//! of ON CONFLICT DO. The conflict manager operates on a per-chunk basis,
//! i.e., we construct a new conflict manager per incoming chunk.
//! On conflict manager can gather conflicts across multiple indexes, if
//! they match the ON CONFLICT DO target.
class ConflictManager {
public:
	static constexpr uint8_t FIRST = 0;
	static constexpr uint8_t SECOND = 1;

public:
	ConflictManager(const VerifyExistenceType lookup_type, const idx_t chunk_size,
	                optional_ptr<ConflictInfo> conflict_info = nullptr);

public:
	//! Returns true, if we need to throw, otherwise, adds the hit and returns false.
	bool AddHit(const idx_t index_in_chunk, const row_t row_id);
	//! Returns true, if we need to throw, otherwise, adds the second hit and returns false.
	bool AddSecondHit(const idx_t index_in_chunk, const row_t row_id);
	//! Returns true, if we need to throw, otherwise, adds the NULL and returns false.
	bool AddNull(const idx_t index_in_chunk);
	//! Returns the index of the first (in)valid row, if any.
	optional_idx GetFirstInvalidIndex(const idx_t count, const bool negate = false);

	//! Finalizes a global conflict manager.
	void FinalizeGlobal(DuckTransaction &transaction, DataTable &table);
	//! Finalizes a local conflict manager.
	void FinalizeLocal(DataTable &table, LocalStorage &storage);
	//! Determines if we are finished registering conflicts.
	void FinishLookup();

	//! Get the conflict information.
	const ConflictInfo &GetConflictInfo() const {
		return *conflict_info;
	}
	//! Sets the mode of the conflict manager.
	void SetMode(const ConflictManagerMode mode_p) {
		D_ASSERT(mode_p != ConflictManagerMode::SCAN || conflict_info != nullptr);
		mode = mode_p;
	}

	//! Adds an index and its respective delete_index.
	void AddIndex(const shared_ptr<IndexEntry> &index, shared_ptr<IndexEntry> delete_index) {
		matching_indexes.push_back(index);
		matching_delete_indexes.push_back(std::move(delete_index));
	}
	//! Returns true, if the index is in this conflict manager.
	bool IndexMatches(const shared_ptr<IndexEntry> &index) const {
		for (const auto &matching_index : matching_indexes) {
			if (matching_index == index) {
				return true;
			}
		}
		return false;
	}
	//! Returns a reference to the matching indexes.
	const vector<shared_ptr<IndexEntry>> &MatchingIndexes() const {
		return matching_indexes;
	}
	//! Returns a reference to the matching delete indexes.
	const vector<shared_ptr<IndexEntry>> &MatchingDeleteIndexes() const {
		return matching_delete_indexes;
	}

	//! Returns the existence verification type.
	VerifyExistenceType GetVerifyExistenceType() const {
		return verify_existence_type;
	}
	//! Returns true, if there are any conflicts, else false.
	bool HasConflicts() const {
		return conflict_data[FIRST].sel != nullptr;
	}
	//! Returns the number of conflicts.
	idx_t ConflictCount() const {
		if (!HasConflicts()) {
			return 0;
		}
		return conflict_data[FIRST].count;
	}
	//! Returns a reference to the row IDs.
	//! Must be called after Finalize[Global|Local].
	Vector &GetRowIds() {
		D_ASSERT(!conflict_data[SECOND].sel);
		return *GetConflictData(FIRST).row_ids;
	}
	//! Returns a reference to the inverted selection vector.
	//! Must be called after Finalize[Global|Local].
	SelectionVector &GetInvertedSel() const {
		D_ASSERT(!conflict_data[SECOND].sel);
		return *conflict_data[FIRST].inverted_sel;
	}
	//! Returns the first index in a chunk with a conflict.
	idx_t GetFirstIndex() const {
		return GetInvertedSel().get_index(0);
	}
	//! Returns the validity array of the first conflict data.
	ValidityArray &GetFirstValidity() {
		D_ASSERT(HasConflicts());
		return conflict_data[FIRST].validity;
	}

private:
	//! The type of constraint verification for which we're using the conflict manager.
	VerifyExistenceType verify_existence_type;
	//! The count of the data chunk for which we've created the conflict manager.
	idx_t chunk_size;
	//! Optional information to match indexes to the conflict target.
	optional_ptr<ConflictInfo> conflict_info;
	//! The mode of the conflict manager.
	ConflictManagerMode mode;

	//! Indexes matching the conflict target.
	vector<shared_ptr<IndexEntry>> matching_indexes;
	//! Delete indexes matching the conflict target.
	vector<shared_ptr<IndexEntry>> matching_delete_indexes;

	//! True, if we can skip recording any further conflicts.
	bool finished = false;

	//! ConflictData is a helper struct tracking conflicts for each index in a chunk,
	//! as well as the total conflict count, and the row ID matching each conflict.
	struct ConflictData {
		//! Conflict count.
		idx_t count = 0;
		//! Links the conflicting rows to their row IDs (index_in_chunk -> count).
		unique_ptr<SelectionVector> sel;
		//! Inverted selection vector to slice the input chunk (count -> index_in_chunk).
		unique_ptr<SelectionVector> inverted_sel;
		//! Row IDs.
		unique_ptr<Vector> row_ids;
		//! Optional row ID data.
		row_t *row_ids_data;
		//! Keeps track of the indexes in the chunk for which we've seen a conflict.
		//! True (valid) indicates a conflict.
		ValidityArray validity;

		//! Inserts a conflict into the two selection vectors, the row IDs, and the validity.
		void Insert(const idx_t index_in_chunk, const row_t row_id) {
			D_ASSERT(!validity.RowIsValid(index_in_chunk));
			sel->set_index(index_in_chunk, count);
			inverted_sel->set_index(count, index_in_chunk);
			validity.SetValid(index_in_chunk);
			row_ids_data[count] = row_id;
			count++;
		}

		//! Returns the row ID matching the index in the chunk.
		row_t GetRowId(const idx_t index_in_chunk) {
			D_ASSERT(validity.RowIsValid(index_in_chunk));
			auto idx = sel->get_index(index_in_chunk);
			return row_ids_data[idx];
		}

		//! Resets the conflict data.
		void Reset() {
			count = 0;
			sel = nullptr;
			inverted_sel = nullptr;
			row_ids_data = nullptr;
			row_ids = nullptr;
		}
	};

	//! With the introduction of delete indexes, it is now possible to have up to two row IDs
	//! in a UNIQUE/PRIMARY KEY leaf. Only one of the row IDs is visible to the transaction in
	//! which we're using the conflict manager. Thus, we need to register both row IDs when
	//! scanning, and later Finalize the conflict data to only contain the visible row ID.
	array<ConflictData, 2> conflict_data;

private:
	//! Returns true, if we register a conflict for the lookup type.
	bool IsConflict(LookupResultType type);
	//! Adds a hit to the conflicts.
	bool AddHitInternal(const idx_t index_in_chunk, const row_t row_id, const bool second);
	//! Determine visible row ID for each index with two possible row IDs.
	void Finalize(const std::function<bool(const row_t row_id)> &callback);
	//! Returns true, if the conflict manager should throw an exception, else false.
	bool ShouldThrow(const idx_t index_in_chunk) const;
	//! Returns true, if we ignore NULLs, else false.
	bool IgnoreNulls() const;

	//! Returns a reference to the conflict data (FIRST or SECOND),
	//! and initializes it, if uninitialized.
	ConflictData &GetConflictData(const idx_t i) {
		if (conflict_data[i].sel) {
			return conflict_data[i];
		}
		conflict_data[i].sel = make_uniq<SelectionVector>(chunk_size);
		conflict_data[i].inverted_sel = make_uniq<SelectionVector>(chunk_size);
		conflict_data[i].validity.Initialize(chunk_size, false);
		conflict_data[i].row_ids = make_uniq<Vector>(LogicalType::ROW_TYPE, chunk_size);
		conflict_data[i].row_ids_data = FlatVector::GetDataMutable<row_t>(*conflict_data[i].row_ids);
		return conflict_data[i];
	}

	//! Adds a row ID to the primary conflict data.
	void AddRowId(const idx_t index_in_chunk, const row_t row_id);

	//! Adds a row ID to the secondary conflict data.
	void AddSecondRowId(const idx_t index_in_chunk, const row_t row_id);
};

} // namespace duckdb
