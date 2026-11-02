#include "duckdb/function/table/system_catalog_scanner.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/entry_scan_filters.hpp"
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

static bool Matches(const optional_idx &idx, idx_t column_index) {
	return idx.IsValid() && idx.GetIndex() == column_index;
}

bool SystemCatalogScannerData::IsIdentityColumn(idx_t column_index) const {
	return Matches(database_name_idx, column_index) || Matches(database_oid_idx, column_index) ||
	       Matches(schema_name_idx, column_index) || Matches(schema_oid_idx, column_index) ||
	       Matches(entry_name_idx, column_index) || Matches(entry_oid_idx, column_index);
}

unique_ptr<FunctionData> SystemCatalogScannerData::Copy() const {
	auto result = make_uniq<SystemCatalogScannerData>();
	result->database_name_idx = database_name_idx;
	result->database_oid_idx = database_oid_idx;
	result->schema_name_idx = schema_name_idx;
	result->schema_oid_idx = schema_oid_idx;
	result->entry_name_idx = entry_name_idx;
	result->entry_oid_idx = entry_oid_idx;
	return std::move(result);
}

bool SystemCatalogScannerData::Equals(const FunctionData &other_p) const {
	auto &other = other_p.Cast<SystemCatalogScannerData>();
	return database_name_idx == other.database_name_idx && database_oid_idx == other.database_oid_idx &&
	       schema_name_idx == other.schema_name_idx && schema_oid_idx == other.schema_oid_idx &&
	       entry_name_idx == other.entry_name_idx && entry_oid_idx == other.entry_oid_idx;
}

// the filters are evaluated against a VARCHAR (names) or BIGINT (oids) value, so only a column of that type can be
// an identity column
static bool IsIdentityType(const LogicalType &type, bool is_oid) {
	return type.id() == (is_oid ? LogicalTypeId::BIGINT : LogicalTypeId::VARCHAR);
}

unique_ptr<FunctionData> SystemCatalogScanner::CreateBindData(const vector<Identifier> &names,
                                                              const vector<LogicalType> &return_types,
                                                              const char *entry_name_column,
                                                              const char *entry_oid_column) {
	D_ASSERT(names.size() == return_types.size());
	auto result = make_uniq<SystemCatalogScannerData>();
	for (idx_t i = 0; i < names.size(); i++) {
		auto &name = names[i];
		auto &type = return_types[i];
		if (name == "database_name" && IsIdentityType(type, false)) {
			result->database_name_idx = i;
		} else if (name == "database_oid" && IsIdentityType(type, true)) {
			result->database_oid_idx = i;
		} else if (name == "schema_name" && IsIdentityType(type, false)) {
			result->schema_name_idx = i;
		} else if (name == "schema_oid" && IsIdentityType(type, true)) {
			result->schema_oid_idx = i;
		} else if (entry_name_column && name == entry_name_column && IsIdentityType(type, false)) {
			result->entry_name_idx = i;
		} else if (entry_oid_column && name == entry_oid_column && IsIdentityType(type, true)) {
			result->entry_oid_idx = i;
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

namespace {

//! The pushed-down filters of a scan, bucketed by identity column.
struct ScanFilters {
	optional_ptr<const TableFilter> database_name;
	optional_ptr<const TableFilter> database_oid;
	SchemaScanFilters schema;
	EntryScanFilters entry;

	bool HasSchemaLevelFilters() const {
		return database_name || database_oid || schema.GetSchemaName() || schema.GetSchemaOid();
	}
};

// filters are keyed by position in input.column_indexes. A filter on any other column is either re-applied above
// the scan by the planner (supports_pushdown_type) or an optional join filter the join re-checks; skip it.
ScanFilters CollectFilters(TableFunctionInitInput &input, const SystemCatalogScannerData &data) {
	ScanFilters result;
	if (!input.filters) {
		return result;
	}
	for (auto &entry : *input.filters) {
		auto &column = input.column_indexes[entry.GetIndex().GetIndex()];
		auto column_index = column.GetPrimaryIndex();
		if (Matches(data.database_name_idx, column_index)) {
			result.database_name = &entry.Filter();
		} else if (Matches(data.database_oid_idx, column_index)) {
			result.database_oid = &entry.Filter();
		} else if (Matches(data.schema_name_idx, column_index)) {
			result.schema.SetSchemaName(&entry.Filter());
		} else if (Matches(data.schema_oid_idx, column_index)) {
			result.schema.SetSchemaOid(&entry.Filter());
		} else if (Matches(data.entry_name_idx, column_index)) {
			result.entry.SetName(&entry.Filter());
		} else if (Matches(data.entry_oid_idx, column_index)) {
			result.entry.SetOid(&entry.Filter());
		}
	}
	return result;
}

vector<reference<SchemaCatalogEntry>> GetSchemasWithFilters(ClientContext &context, const ScanFilters &filters,
                                                            SystemCatalogScannerStats &stats) {
	if (!filters.HasSchemaLevelFilters()) {
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
		if (!SchemaScanFilters::Satisfies(context, filters.database_name, Value(catalog.GetName())) ||
		    !SchemaScanFilters::Satisfies(context, filters.database_oid,
		                                  Value::BIGINT(NumericCast<int64_t>(catalog.GetOid())))) {
			continue;
		}
		stats.catalogs_scanned++;
		for (auto &schema : catalog.GetSchemasForScan(context, filters.schema)) {
			stats.schemas_total++;
			// the catalog may return schemas the filters reject
			if (!filters.schema.Accepts(context, schema.get())) {
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

} // namespace

vector<reference<SchemaCatalogEntry>> SystemCatalogScanner::GetSchemas(ClientContext &context,
                                                                       TableFunctionInitInput &input,
                                                                       SystemCatalogScannerStats &stats) {
	if (!input.bind_data) {
		return Catalog::GetAllSchemas(context);
	}
	auto &data = input.bind_data->Cast<SystemCatalogScannerData>();
	return GetSchemasWithFilters(context, CollectFilters(input, data), stats);
}

EntryScanFilters SystemCatalogScanner::GetEntryFilters(TableFunctionInitInput &input) {
	if (!input.bind_data) {
		return EntryScanFilters();
	}
	return CollectFilters(input, input.bind_data->Cast<SystemCatalogScannerData>()).entry;
}

vector<reference<CatalogEntry>> SystemCatalogScanner::GetSchemaEntries(ClientContext &context,
                                                                       SchemaCatalogEntry &schema, CatalogType type,
                                                                       const EntryScanFilters &filters,
                                                                       SystemCatalogScannerStats &stats) {
	vector<reference<CatalogEntry>> result;
	std::function<void(CatalogEntry &)> collect = [&](CatalogEntry &entry) {
		stats.entries_total++;
		// the schema may return entries the filters reject
		if (!filters.Accepts(context, entry)) {
			return;
		}
		stats.entries_scanned++;
		result.push_back(entry);
	};
	SchemaScanInfo info {type, collect, &filters};
	schema.Scan(context, info);
	return result;
}

vector<reference<CatalogEntry>> SystemCatalogScanner::GetEntries(ClientContext &context, TableFunctionInitInput &input,
                                                                 CatalogType type, SystemCatalogScannerStats &stats) {
	vector<reference<CatalogEntry>> result;
	if (!input.bind_data) {
		for (auto &schema : Catalog::GetAllSchemas(context)) {
			schema.get().Scan(context, type, [&](CatalogEntry &entry) { result.push_back(entry); });
		}
		return result;
	}
	auto &data = input.bind_data->Cast<SystemCatalogScannerData>();
	auto filters = CollectFilters(input, data);
	for (auto &schema : GetSchemasWithFilters(context, filters, stats)) {
		auto entries = GetSchemaEntries(context, schema.get(), type, filters.entry, stats);
		result.insert(result.end(), entries.begin(), entries.end());
	}
	return result;
}

void SystemCatalogScanner::ReportMetrics(TableFunctionGetMetricsInput &input, const SystemCatalogScannerStats &stats) {
	input.operator_metrics.AddExtraInfo("Catalogs Scanned",
	                                    to_string(stats.catalogs_scanned) + "/" + to_string(stats.catalogs_total));
	input.operator_metrics.AddExtraInfo("Schemas Scanned",
	                                    to_string(stats.schemas_scanned) + "/" + to_string(stats.schemas_total));
	input.operator_metrics.AddExtraInfo("Entries Scanned",
	                                    to_string(stats.entries_scanned) + "/" + to_string(stats.entries_total));
}

void SystemCatalogScanner::RegisterPushdown(TableFunction &function) {
	function.filter_pushdown = true;
	function.pushdown_expression = PushdownExpression;
	function.supports_pushdown_type = SupportsPushdown;
}

} // namespace duckdb
