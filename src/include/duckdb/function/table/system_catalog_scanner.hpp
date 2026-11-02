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
#include "duckdb/function/function.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckdb {

//! Bind data for the system table functions that emit the catalog/schema identity columns
//! (database_name, database_oid, schema_name, schema_oid). Filter pushdown is restricted to these columns.
struct SystemCatalogScannerData : public FunctionData {
	//! Position of each identity column in the function's output; invalid when the function lacks it.
	optional_idx database_name_idx;
	optional_idx database_oid_idx;
	optional_idx schema_name_idx;
	optional_idx schema_oid_idx;

	//! Whether the given column index is one of the four identity columns.
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
};

class SystemCatalogScanner {
public:
	//! Bind helper: locate the identity columns by name in `names`. Always returns non-null bind data.
	static unique_ptr<FunctionData> CreateBindData(const vector<Identifier> &names);
	//! Init helper: the schemas that satisfy the pushed-down filters, in Catalog::GetAllSchemas order.
	//! Without filters this is exactly Catalog::GetAllSchemas(context).
	static vector<reference<SchemaCatalogEntry>> GetSchemas(ClientContext &context, TableFunctionInitInput &input,
	                                                        SystemCatalogScannerStats &stats);
	//! get_metrics helper: writes the counters into input.operator_metrics via AddExtraInfo.
	static void ReportMetrics(TableFunctionGetMetricsInput &input, const SystemCatalogScannerStats &stats);
	//! Enables filter pushdown on the identity columns only. get_metrics is per function because
	//! it needs the function's own GlobalTableFunctionState type to reach the stats.
	static void RegisterPushdown(TableFunction &function);
};

} // namespace duckdb
