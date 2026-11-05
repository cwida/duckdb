//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/enums/database_modification_type.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types.hpp"

namespace duckdb {

struct DatabaseModificationType {
public:
	static constexpr idx_t INSERT_DATA = 1ULL << 0ULL;
	static constexpr idx_t DELETE_DATA = 1ULL << 1ULL;
	static constexpr idx_t UPDATE_DATA = 1ULL << 2ULL;
	//! An ALTER TABLE that may change table storage: not renames, defaults, DROP NOT NULL or comments
	static constexpr idx_t ALTER_TABLE = 1ULL << 3ULL;
	//! Creating a table, index or trigger, or replacing a schema: not entries without table storage
	static constexpr idx_t CREATE_CATALOG_ENTRY = 1ULL << 4ULL;
	//! Dropping a table, index or trigger, or anything with CASCADE: not entries without table storage
	static constexpr idx_t DROP_CATALOG_ENTRY = 1ULL << 5ULL;
	static constexpr idx_t SEQUENCE = 1ULL << 6ULL;
	static constexpr idx_t CREATE_INDEX = 1ULL << 7ULL;

	constexpr DatabaseModificationType() : value(0) {
	}
	constexpr DatabaseModificationType(idx_t value) : value(value) { // NOLINT : allow implicit conversion
	}

	inline constexpr DatabaseModificationType operator|(DatabaseModificationType b) const {
		return DatabaseModificationType(value | b.value);
	}
	inline DatabaseModificationType &operator|=(DatabaseModificationType b) {
		value |= b.value;
		return *this;
	}

	bool Contains(DatabaseModificationType other) const {
		return (value & other.value) == other.value;
	}

	bool InsertData() const {
		return value & INSERT_DATA;
	}
	bool DeleteData() const {
		return value & DELETE_DATA;
	}
	bool UpdateData() const {
		return value & UPDATE_DATA;
	}
	bool AlterTable() const {
		return value & ALTER_TABLE;
	}
	bool CreateCatalogEntry() const {
		return value & CREATE_CATALOG_ENTRY;
	}
	bool DropCatalogEntry() const {
		return value & DROP_CATALOG_ENTRY;
	}
	bool Sequence() const {
		return value & SEQUENCE;
	}
	bool CreateIndex() const {
		return value & CREATE_INDEX;
	}

private:
	idx_t value;
};

} // namespace duckdb
