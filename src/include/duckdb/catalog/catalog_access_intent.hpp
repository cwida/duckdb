//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/catalog_access_intent.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types.hpp"

namespace duckdb {

//! What a statement wants to do with the entry a catalog lookup returns, as a set of access bits. The binder sets it
//! on the lookup of a statement's target (INSERT, COPY FROM, UPDATE, DELETE, TRUNCATE, MERGE INTO, ALTER, COMMENT ON,
//! DROP); every other lookup, including the sources those statements read, has the DEFAULT intent. Catalogs that
//! authorize access per lookup (e.g. against an external access control service) can act on it; other catalogs
//! ignore it.
//! Unlike DatabaseModificationType, which records what a transaction modified, this describes one lookup and covers
//! reads as well as writes.
struct CatalogAccessIntent {
public:
	//! Read the entry's definition (its columns and properties)
	static constexpr idx_t READ_METADATA = 1ULL << 0ULL;
	//! Read the entry's data
	static constexpr idx_t READ_DATA = 1ULL << 1ULL;
	static constexpr idx_t INSERT_DATA = 1ULL << 2ULL;
	static constexpr idx_t UPDATE_DATA = 1ULL << 3ULL;
	static constexpr idx_t DELETE_DATA = 1ULL << 4ULL;
	//! Change the entry's definition (ALTER, COMMENT ON)
	static constexpr idx_t MODIFY_METADATA = 1ULL << 5ULL;
	//! Drop the entry
	static constexpr idx_t DROP_ENTRY = 1ULL << 6ULL;
	//! The intent of a lookup that does not set one: read the entry's definition and its data
	static constexpr idx_t DEFAULT = READ_METADATA | READ_DATA;

	constexpr CatalogAccessIntent() : value(DEFAULT) {
	}
	constexpr CatalogAccessIntent(idx_t value) : value(value) { // NOLINT: allow implicit conversion
	}

	inline constexpr CatalogAccessIntent operator|(CatalogAccessIntent b) const {
		return CatalogAccessIntent(value | b.value);
	}
	inline CatalogAccessIntent &operator|=(CatalogAccessIntent b) {
		value |= b.value;
		return *this;
	}
	bool operator==(const CatalogAccessIntent &other) const {
		return value == other.value;
	}
	bool operator!=(const CatalogAccessIntent &other) const {
		return value != other.value;
	}

	//! Whether every bit of 'other' is set
	bool Contains(CatalogAccessIntent other) const {
		return (value & other.value) == other.value;
	}

	bool ReadsData() const {
		return value & READ_DATA;
	}
	//! INSERT_DATA, UPDATE_DATA or DELETE_DATA
	bool ModifiesData() const {
		return value & (INSERT_DATA | UPDATE_DATA | DELETE_DATA);
	}
	//! MODIFY_METADATA or DROP_ENTRY
	bool ModifiesMetadata() const {
		return value & (MODIFY_METADATA | DROP_ENTRY);
	}
	//! Whether the lookup modifies the entry's data or its definition
	bool IsWrite() const {
		return ModifiesData() || ModifiesMetadata();
	}
	//! Whether the lookup only reads the entry's definition (and never its data)
	bool ReadsMetadataOnly() const {
		return value == READ_METADATA;
	}

	//! The set bits, e.g. "UPDATE_DATA | READ_DATA"
	DUCKDB_API string ToString() const;

private:
	idx_t value;
};

} // namespace duckdb
