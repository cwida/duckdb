#include "duckdb/function/table/system_catalog_scanner.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/schema_scan_filters.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/profiler/profiling_node.hpp"
#include "duckdb/optimizer/column_lifetime_analyzer.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/table_filter_set.hpp"

#include <algorithm>

namespace duckdb {

bool SystemCatalogScannerData::IsIdentityColumn(idx_t column_index) const {
	if (database_name_idx.IsValid() && database_name_idx.GetIndex() == column_index) {
		return true;
	}
	if (database_oid_idx.IsValid() && database_oid_idx.GetIndex() == column_index) {
		return true;
	}
	if (schema_name_idx.IsValid() && schema_name_idx.GetIndex() == column_index) {
		return true;
	}
	if (schema_oid_idx.IsValid() && schema_oid_idx.GetIndex() == column_index) {
		return true;
	}
	return false;
}

unique_ptr<FunctionData> SystemCatalogScannerData::Copy() const {
	auto result = make_uniq<SystemCatalogScannerData>();
	result->database_name_idx = database_name_idx;
	result->database_oid_idx = database_oid_idx;
	result->schema_name_idx = schema_name_idx;
	result->schema_oid_idx = schema_oid_idx;
	return std::move(result);
}

bool SystemCatalogScannerData::Equals(const FunctionData &other_p) const {
	auto &other = other_p.Cast<SystemCatalogScannerData>();
	return database_name_idx == other.database_name_idx && database_oid_idx == other.database_oid_idx &&
	       schema_name_idx == other.schema_name_idx && schema_oid_idx == other.schema_oid_idx;
}

unique_ptr<FunctionData> SystemCatalogScanner::CreateBindData(const vector<Identifier> &names) {
	auto result = make_uniq<SystemCatalogScannerData>();
	for (idx_t i = 0; i < names.size(); i++) {
		if (names[i] == "database_name") {
			result->database_name_idx = i;
		} else if (names[i] == "database_oid") {
			result->database_oid_idx = i;
		} else if (names[i] == "schema_name") {
			result->schema_name_idx = i;
		} else if (names[i] == "schema_oid") {
			result->schema_oid_idx = i;
		}
	}
	return std::move(result);
}

// pushdown_expression: accept an expression only if the single column it references is an identity column.
static bool PushdownExpression(ClientContext &context, const LogicalGet &get, Expression &expr) {
	if (!get.bind_data) {
		return false;
	}
	auto &data = get.bind_data->Cast<SystemCatalogScannerData>();
	vector<ColumnBinding> bindings;
	ColumnLifetimeAnalyzer::ExtractColumnBindings(expr, bindings);
	if (bindings.empty() || bindings[0].table_index != get.table_index) {
		return false;
	}
	auto &column_ids = get.GetColumnIds();
	auto pos = bindings[0].column_index.GetIndex();
	if (pos >= column_ids.size() || !column_ids[pos].HasPrimaryIndex() || column_ids[pos].IsVirtualColumn()) {
		return false;
	}
	return data.IsIdentityColumn(column_ids[pos].GetPrimaryIndex());
}

// supports_pushdown_type: filters on any other column are re-applied above the scan by the planner.
static bool SupportsPushdown(const FunctionData &bind_data, idx_t col_idx) {
	return bind_data.Cast<SystemCatalogScannerData>().IsIdentityColumn(col_idx);
}

static bool Matches(const optional_idx &idx, idx_t column_index) {
	return idx.IsValid() && idx.GetIndex() == column_index;
}

vector<reference<SchemaCatalogEntry>> SystemCatalogScanner::GetSchemas(ClientContext &context,
                                                                       TableFunctionInitInput &input,
                                                                       SystemCatalogScannerStats &stats) {
	if (!input.bind_data || !input.filters || !input.filters->HasFilters()) {
		return Catalog::GetAllSchemas(context);
	}
	auto &data = input.bind_data->Cast<SystemCatalogScannerData>();

	// filters are keyed by position in input.column_indexes. A filter on any other column is either re-applied above
	// the scan by the planner (supports_pushdown_type) or an optional join filter the join re-checks; skip it.
	optional_ptr<const TableFilter> database_name_filter;
	optional_ptr<const TableFilter> database_oid_filter;
	SchemaScanFilters schema_filters;
	for (auto &entry : *input.filters) {
		auto &column = input.column_indexes[entry.GetIndex().GetIndex()];
		auto column_index = column.GetPrimaryIndex();
		if (Matches(data.database_name_idx, column_index)) {
			database_name_filter = &entry.Filter();
		} else if (Matches(data.database_oid_idx, column_index)) {
			database_oid_filter = &entry.Filter();
		} else if (Matches(data.schema_name_idx, column_index)) {
			schema_filters.schema_name = &entry.Filter();
		} else if (Matches(data.schema_oid_idx, column_index)) {
			schema_filters.schema_oid = &entry.Filter();
		}
	}
	if (!database_name_filter && !database_oid_filter && !schema_filters.schema_name && !schema_filters.schema_oid) {
		return Catalog::GetAllSchemas(context);
	}

	vector<reference<SchemaCatalogEntry>> result;
	auto &db_manager = DatabaseManager::Get(context);
	for (auto &database : db_manager.GetDatabases(context)) {
		if (database->GetVisibility() == AttachVisibility::HIDDEN) {
			continue; // same skip as Catalog::GetAllSchemas
		}
		auto &catalog = database->GetCatalog();
		stats.catalogs_total++;
		if (!SchemaScanFilters::Satisfies(context, database_name_filter, Value(catalog.GetName())) ||
		    !SchemaScanFilters::Satisfies(context, database_oid_filter,
		                                  Value::BIGINT(NumericCast<int64_t>(catalog.GetOid())))) {
			continue;
		}
		stats.catalogs_scanned++;
		for (auto &schema : catalog.GetSchemasForScan(context, schema_filters)) {
			stats.schemas_total++;
			// the catalog may return schemas the filters reject
			if (!schema_filters.Accepts(context, schema.get())) {
				continue;
			}
			stats.schemas_scanned++;
			result.push_back(schema);
		}
	}

	// same order as Catalog::GetAllSchemas
	std::sort(result.begin(), result.end(), [](reference<SchemaCatalogEntry> l, reference<SchemaCatalogEntry> r) {
		auto &left = l.get();
		auto &right = r.get();
		if (left.catalog.GetName() < right.catalog.GetName()) {
			return true;
		}
		if (left.catalog.GetName() == right.catalog.GetName()) {
			return left.name < right.name;
		}
		return false;
	});
	return result;
}

void SystemCatalogScanner::ReportMetrics(TableFunctionGetMetricsInput &input, const SystemCatalogScannerStats &stats) {
	input.operator_metrics.AddExtraInfo("Catalogs Scanned",
	                                    to_string(stats.catalogs_scanned) + "/" + to_string(stats.catalogs_total));
	input.operator_metrics.AddExtraInfo("Schemas Scanned",
	                                    to_string(stats.schemas_scanned) + "/" + to_string(stats.schemas_total));
}

void SystemCatalogScanner::RegisterPushdown(TableFunction &function) {
	function.filter_pushdown = true;
	function.pushdown_expression = PushdownExpression;
	function.supports_pushdown_type = SupportsPushdown;
}

} // namespace duckdb
