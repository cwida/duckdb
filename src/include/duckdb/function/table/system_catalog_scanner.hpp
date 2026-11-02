//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/table/system_catalog_scanner.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/entry_scan_filters.hpp"
#include "duckdb/function/function.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckdb {

//! Bind data for the system table functions that emit the identity columns of a catalog entry: the catalog
//! (database_name, database_oid), the schema (schema_name, schema_oid) and the entry itself (e.g. table_name,
//! table_oid). Filter pushdown is restricted to these columns.
struct SystemCatalogScannerData : public FunctionData {
	//! Position of each identity column in the function's output; invalid when the function lacks it.
	optional_idx database_name_idx;
	optional_idx database_oid_idx;
	optional_idx schema_name_idx;
	optional_idx schema_oid_idx;
	optional_idx entry_name_idx;
	optional_idx entry_oid_idx;

	//! Whether the given column index is one of the identity columns.
	bool IsIdentityColumn(idx_t column_index) const;

	unique_ptr<FunctionData> Copy() const override;
	bool Equals(const FunctionData &other) const override;
};

//! Counters reported through get_metrics for EXPLAIN ANALYZE.
struct SystemCatalogScannerStats {
	idx_t catalogs_total = 0;
	idx_t catalogs_scanned = 0;
	idx_t schemas_total = 0;
	idx_t schemas_scanned = 0;
	//! Entries the schemas returned (of any type) / entries the filters accepted.
	idx_t entries_total = 0;
	idx_t entries_scanned = 0;
};

class SystemCatalogScanner {
public:
	//! Bind helper: locate the identity columns by name in `names`. A column counts only when its declared type is
	//! the one the filters are evaluated against (VARCHAR for names, BIGINT for oids). `entry_name_column` and
	//! `entry_oid_column` name the function's entry identity columns (e.g. "table_name", "table_oid"); pass nullptr
	//! for a function without them. Always returns non-null bind data.
	static unique_ptr<FunctionData> CreateBindData(const vector<Identifier> &names,
	                                               const vector<LogicalType> &return_types,
	                                               const char *entry_name_column = nullptr,
	                                               const char *entry_oid_column = nullptr);
	//! Init helper: the schemas that satisfy the pushed-down catalog and schema filters, in Catalog::GetAllSchemas
	//! order. Without such filters this is exactly Catalog::GetAllSchemas(context).
	static vector<reference<SchemaCatalogEntry>> GetSchemas(ClientContext &context, TableFunctionInitInput &input,
	                                                        SystemCatalogScannerStats &stats);
	//! Init helper: the pushed-down entry filters of this scan; empty without scanner bind data or filters.
	static EntryScanFilters GetEntryFilters(TableFunctionInitInput &input);
	//! Init helper: the entries of `type` in one schema that satisfy `filters`, in the order the schema returns them.
	//! Every entry the schema returns is re-checked against `filters`.
	static vector<reference<CatalogEntry>> GetSchemaEntries(ClientContext &context, SchemaCatalogEntry &schema,
	                                                        CatalogType type, const EntryScanFilters &filters,
	                                                        SystemCatalogScannerStats &stats);
	//! Init helper: GetSchemaEntries over every schema GetSchemas returns, in that order. Without entry filters this
	//! is exactly Scan(type) over those schemas. A function calls either GetSchemas or GetEntries, not both: the
	//! catalog and schema counters are incremented by the schema enumeration.
	static vector<reference<CatalogEntry>> GetEntries(ClientContext &context, TableFunctionInitInput &input,
	                                                  CatalogType type, SystemCatalogScannerStats &stats);
	//! get_metrics helper: writes the counters into input.operator_metrics via AddExtraInfo.
	static void ReportMetrics(TableFunctionGetMetricsInput &input, const SystemCatalogScannerStats &stats);
	//! Enables filter pushdown on the identity columns only. get_metrics is per function because
	//! it needs the function's own GlobalTableFunctionState type to reach the stats.
	static void RegisterPushdown(TableFunction &function);
};

} // namespace duckdb
