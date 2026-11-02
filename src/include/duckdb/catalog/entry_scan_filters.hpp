//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/entry_scan_filters.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/optional_ptr.hpp"

namespace duckdb {
class CatalogEntry;
class ClientContext;
class TableFilter;

//! Filters pushed into a system catalog scan on the entry identity columns (e.g. table_name / table_oid).
class EntryScanFilters {
public:
	optional_ptr<const TableFilter> GetName() const {
		return name;
	}
	optional_ptr<const TableFilter> GetOid() const {
		return oid;
	}
	void SetName(optional_ptr<const TableFilter> filter) {
		name = filter;
	}
	void SetOid(optional_ptr<const TableFilter> filter) {
		oid = filter;
	}

	//! Whether the entry satisfies every filter present; a missing filter accepts everything.
	bool Accepts(ClientContext &context, const CatalogEntry &entry) const;

private:
	optional_ptr<const TableFilter> name;
	optional_ptr<const TableFilter> oid;
};

} // namespace duckdb
