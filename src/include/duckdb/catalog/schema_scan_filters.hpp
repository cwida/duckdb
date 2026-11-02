//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/schema_scan_filters.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/optional_ptr.hpp"

namespace duckdb {
class ClientContext;
class SchemaCatalogEntry;
class TableFilter;
class Value;

//! Filters pushed into a system catalog scan (duckdb_tables, duckdb_views) on the schema identity columns.
struct SchemaScanFilters {
	optional_ptr<const TableFilter> schema_name;
	optional_ptr<const TableFilter> schema_oid;

	//! Whether the schema satisfies every filter present.
	bool Accepts(ClientContext &context, const SchemaCatalogEntry &schema) const;
	//! Whether a single filter accepts the value; a missing filter accepts everything.
	static bool Satisfies(ClientContext &context, optional_ptr<const TableFilter> filter, const Value &value);
};

} // namespace duckdb
