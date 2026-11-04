#include "duckdb/common/types/conflict_manager.hpp"

#include "duckdb/common/types/constraint_conflict_info.hpp"
#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/transaction/local_storage.hpp"

namespace duckdb {

ConflictManager::ConflictManager(const VerifyExistenceType verify_existence_type, const idx_t chunk_size,
                                 optional_ptr<ConflictInfo> conflict_info)
    : verify_existence_type(verify_existence_type), chunk_size(chunk_size), conflict_info(conflict_info),
      mode(ConflictManagerMode::THROW) {
}

void ConflictManager::FinishLookup() {
	if (mode == ConflictManagerMode::THROW) {
		return;
	}
	if (!conflict_info->SingleIndexTarget()) {
		return;
	}

	// We recorded conflicts of one of the indexes.
	// We can skip any duplicate indexes matching the same conflict target.
	finished = HasConflicts();
}

bool ConflictManager::IsConflict(LookupResultType type) {
	switch (type) {
	case LookupResultType::LOOKUP_NULL:
		if (IgnoreNulls()) {
			return false;
		}
		// If we ignore NULL, then we treat this as a hit.
		return IsConflict(LookupResultType::LOOKUP_HIT);
	case LookupResultType::LOOKUP_HIT:
		return true;
	case LookupResultType::LOOKUP_MISS:
		return false;
	default:
		throw NotImplementedException("Type not implemented for LookupResultType");
	}
}

void ConflictManager::AddRowId(const idx_t index_in_chunk, const row_t row_id) {
	// Only ON CONFLICT DO NOTHING can have multiple conflict targets,
	// which can cause multiple row IDs per index_in_chunk.
	// We ignore later row IDs, as we don't need them.
	auto &data = GetConflictData(FIRST);
	if (!data.validity.RowIsValid(index_in_chunk)) {
		data.Insert(index_in_chunk, row_id);
	}
}

void ConflictManager::AddSecondRowId(const idx_t index_in_chunk, const row_t row_id) {
	D_ASSERT(HasConflicts());
	D_ASSERT(conflict_data[FIRST].validity.RowIsValid(index_in_chunk));
	GetConflictData(SECOND).Insert(index_in_chunk, row_id);
}

bool ConflictManager::AddHitInternal(const idx_t index_in_chunk, const row_t row_id, const bool second) {
	D_ASSERT(index_in_chunk < chunk_size);
	if (ShouldThrow(index_in_chunk)) {
		return true;
	}

	if (mode == ConflictManagerMode::THROW) {
		// On THROW, and if the index is part of the previously scanned conflicts,
		// we ignore the conflict.
		D_ASSERT(!ShouldThrow(index_in_chunk));
		return false;
	}
	D_ASSERT(conflict_info);

	// Add the conflict and don't throw.
	D_ASSERT(mode == ConflictManagerMode::SCAN);
	D_ASSERT(!ShouldThrow(index_in_chunk));
	if (finished) {
		return false;
	}
	if (second) {
		AddSecondRowId(index_in_chunk, row_id);
	} else {
		AddRowId(index_in_chunk, row_id);
	}
	return false;
}

bool ConflictManager::AddHit(const idx_t index_in_chunk, const row_t row_id) {
	return AddHitInternal(index_in_chunk, row_id, false);
}

bool ConflictManager::AddSecondHit(const idx_t index_in_chunk, const row_t row_id) {
	return AddHitInternal(index_in_chunk, row_id, true);
}

bool ConflictManager::AddNull(const idx_t index_in_chunk) {
	D_ASSERT(index_in_chunk < chunk_size);
	if (!IsConflict(LookupResultType::LOOKUP_NULL)) {
		return false;
	}
	auto row_id = static_cast<row_t>(DConstants::INVALID_INDEX);
	return AddHit(index_in_chunk, row_id);
}

optional_idx ConflictManager::GetFirstInvalidIndex(const idx_t count, const bool negate) {
	auto &validity = GetFirstValidity();
	for (idx_t i = 0; i < count; i++) {
		if (negate && !validity.RowIsValid(i)) {
			return i;
		} else if (!negate && validity.RowIsValid(i)) {
			return i;
		}
	}
	return optional_idx();
}

void ConflictManager::Finalize(const std::function<bool(const row_t row_id)> &callback) {
	if (!GetConflictData(SECOND).sel) {
		return;
	}

	for (idx_t i = 0; i < GetConflictData(SECOND).count; i++) {
		auto row_id = GetConflictData(SECOND).row_ids_data[i];
		if (callback(row_id)) {
			// Replace the primary row ID with the secondary row ID.
			auto index = GetConflictData(SECOND).inverted_sel->get_index(i);
			auto count = GetConflictData(FIRST).sel->get_index(index);
			GetConflictData(FIRST).row_ids_data[count] = row_id;
		}
	}

	GetConflictData(SECOND).Reset();
}

void ConflictManager::FinalizeGlobal(DuckTransaction &transaction, DataTable &table) {
	Finalize([&](const row_t row_id) { return table.CanFetch(transaction, row_id); });
}

void ConflictManager::FinalizeLocal(DataTable &table, LocalStorage &storage) {
	Finalize([&](const row_t row_id) { return storage.CanFetch(table, row_id); });
}

bool ConflictManager::ShouldThrow(const idx_t index_in_chunk) const {
	// Never throw on scans.
	if (mode == ConflictManagerMode::SCAN) {
		return false;
	}

	// If we have already seen a conflict for this index, then we don't throw.
	D_ASSERT(mode == ConflictManagerMode::THROW);
	return !HasConflicts() || !conflict_data[FIRST].validity.RowIsValid(index_in_chunk);
}

bool ConflictManager::IgnoreNulls() const {
	switch (verify_existence_type) {
	case VerifyExistenceType::APPEND:
		return true;
	case VerifyExistenceType::APPEND_FK:
		return false;
	case VerifyExistenceType::DELETE_FK:
		return true;
	default:
		throw InternalException("Type not implemented for VerifyExistenceType");
	}
}

} // namespace duckdb
