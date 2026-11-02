//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/transaction/delete_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/vector_size.hpp"

namespace duckdb {
class DuckTableEntry;
class RowVersionManager;

//! The undo entry of a delete (DELETE_TUPLE) or of the update markers of an update (UPDATE_ROWS) in one vector
struct DeleteInfo {
	DuckTableEntry *table;
	RowVersionManager *version_info;
	idx_t vector_idx;
	idx_t count;
	idx_t base_row;
	//! Whether or not row ids are consecutive (0, 1, 2, ..., count).
	//! If this is true no rows are stored and `rows` should not be accessed.
	bool is_consecutive;
	//! How many of the ids that the rows held before are stored after the rows: 1 (shared) or count
	idx_t previous_id_count;

	uint16_t *GetRows() {
		if (is_consecutive) {
			throw InternalException("DeleteInfo is consecutive - rows are not accessible");
		}
		return rows;
	}
	const uint16_t *GetRows() const {
		if (is_consecutive) {
			throw InternalException("DeleteInfo is consecutive - rows are not accessible");
		}
		return rows;
	}
	//! The rows, or nullptr if they are consecutive
	const uint16_t *GetStoredRows() const {
		return is_consecutive ? nullptr : rows;
	}
	//! The previous ids are stored after the rows, aligned
	transaction_t *GetPreviousIds() {
		auto rows_end = reinterpret_cast<uintptr_t>(rows) + (is_consecutive ? 0 : count * sizeof(uint16_t));
		return reinterpret_cast<transaction_t *>(AlignValue<uintptr_t, sizeof(transaction_t)>(rows_end));
	}
	//! The size of the entry (an upper bound, as the rows start before the end of the struct)
	static idx_t GetAllocationSize(idx_t count, bool is_consecutive, idx_t previous_id_count) {
		return sizeof(DeleteInfo) + (is_consecutive ? 0 : count * sizeof(uint16_t)) +
		       (previous_id_count + 1) * sizeof(transaction_t);
	}

private:
	//! The per-vector row identifiers (actual row id is base_row + rows[x])
	uint16_t rows[1];
};

//! The rows that a delete or an update wrote in one vector, and the ids they held before (stored in its DeleteInfo)
struct WrittenRows {
	idx_t count = 0;
	//! Whether the rows are 0, 1, ..., count - 1
	bool is_consecutive = true;
	//! How many previous_ids are set: 1 (the rows held the same id) or count
	idx_t previous_id_count = 1;
	uint16_t rows[STANDARD_VECTOR_SIZE];
	transaction_t previous_ids[STANDARD_VECTOR_SIZE];

	//! The rows as an undo entry stores them: nullptr if they are consecutive
	const uint16_t *GetStoredRows() const {
		return is_consecutive ? nullptr : rows;
	}
};

} // namespace duckdb
