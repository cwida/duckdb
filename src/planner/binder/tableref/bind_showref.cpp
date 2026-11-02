#include "duckdb/parser/parser.hpp"
#include "duckdb/function/pragma/pragma_functions.hpp"
#include "duckdb/function/table/system_functions.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"
#include "duckdb/parser/tableref/showref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/operator/logical_column_data_get.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/extension_entries.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/common/enums/show_behavior.hpp"

namespace duckdb {

struct BaseTableColumnInfo {
	optional_ptr<TableCatalogEntry> table = nullptr;
	optional_ptr<const ColumnDefinition> column = nullptr;
};

BaseTableColumnInfo FindBaseTableColumn(LogicalOperator &op, ColumnBinding binding) {
	BaseTableColumnInfo result;
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_GET: {
		auto &get = op.Cast<LogicalGet>();
		if (get.table_index != binding.table_index) {
			return result;
		}
		auto table = get.GetTable();
		if (!table) {
			break;
		}
		if (!get.projection_ids.empty()) {
			throw InternalException("Projection ids should not exist here");
		}
		auto base_column_id = get.GetColumnIndex(binding.column_index);
		if (base_column_id.IsVirtualColumn()) {
			//! Virtual column (like ROW_ID) does not have a ColumnDefinition entry in the TableCatalogEntry
			return result;
		}
		result.table = table;
		result.column = &table->GetColumn(LogicalIndex(base_column_id.GetPrimaryIndex()));
		return result;
	}
	case LogicalOperatorType::LOGICAL_PROJECTION: {
		auto &projection = op.Cast<LogicalProjection>();
		if (binding.table_index != projection.table_index) {
			break;
		}
		auto &expr = projection.GetExpression(binding);
		if (expr.GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
			// if the projection at this index only has a column reference we can directly trace it to the base table
			auto &bound_colref = expr.Cast<BoundColumnRefExpression>();
			return FindBaseTableColumn(*projection.children[0], bound_colref.Binding());
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_LIMIT:
	case LogicalOperatorType::LOGICAL_ORDER_BY:
	case LogicalOperatorType::LOGICAL_TOP_N:
	case LogicalOperatorType::LOGICAL_SAMPLE:
	case LogicalOperatorType::LOGICAL_DISTINCT:
	case LogicalOperatorType::LOGICAL_FILTER:
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
	case LogicalOperatorType::LOGICAL_JOIN:
	case LogicalOperatorType::LOGICAL_ANY_JOIN:
	case LogicalOperatorType::LOGICAL_ASOF_JOIN:
	case LogicalOperatorType::LOGICAL_CROSS_PRODUCT:
		// for any "pass-through" operators - search in children directly
		for (auto &child : op.children) {
			result = FindBaseTableColumn(*child, binding);
			if (result.table) {
				return result;
			}
		}
		break;
	default:
		// unsupported operator
		break;
	}
	return result;
}

BaseTableColumnInfo FindBaseTableColumn(LogicalOperator &op, idx_t column_index) {
	auto bindings = op.GetColumnBindings();
	return FindBaseTableColumn(op, bindings[column_index]);
}

BoundStatement Binder::BindDescribeQuery(ShowRef &ref) {
	// bind the child plan of the DESCRIBE statement
	auto child_binder = Binder::CreateBinder(context, this);
	auto plan = child_binder->Bind(*ref.query);

	// construct a column data collection with the result
	vector<Identifier> return_names = {"column_name", "column_type", "null", "key", "default", "extra"};
	vector<LogicalType> return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                                    LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
	DataChunk output;
	output.Initialize(Allocator::Get(context), return_types);

	auto collection = make_uniq<ColumnDataCollection>(context, return_types);
	ColumnDataAppendState append_state;
	collection->InitializeAppend(append_state);
	for (idx_t column_idx = 0; column_idx < plan.types.size(); column_idx++) {
		// check if we can trace the column to a base table so that we can figure out constraint information
		auto result = FindBaseTableColumn(*plan.plan, column_idx);
		idx_t row_index = output.size();
		auto &alias = plan.names[column_idx];
		if (result.table) {
			// we can! emit the information from the base table directly
			PragmaTableInfo::GetColumnInfo(*result.table, *result.column, output);
			// Override the base column name with the alias if one is specified.
			if (alias.GetIdentifierName() != result.column->Name().GetIdentifierName()) {
				output.data[0].SetValue(row_index, Value(alias));
			}
		} else {
			// we cannot - read the type/name from the plan instead
			auto type = plan.types[column_idx];

			// "name", VARCHAR
			output.data[0].Append(Value(alias));
			// "type", VARCHAR
			output.data[1].Append(Value(type.ToString()));
			// "null", VARCHAR
			output.data[2].Append(Value("YES"));
			// "pk", VARCHAR
			output.data[3].Append(Value());
			// "dflt_value", VARCHAR
			output.data[4].Append(Value());
			// "extra", VARCHAR
			output.data[5].Append(Value());
		}

		// both branches above append exactly one row to the child vectors, growing output.size() accordingly
		if (output.size() == STANDARD_VECTOR_SIZE) {
			collection->Append(append_state, output);
			output.Reset();
		}
	}
	collection->Append(append_state, output);

	auto table_index = GenerateTableIndex();

	BoundStatement result;
	result.names = return_names;
	result.types = return_types;
	result.plan = make_uniq<LogicalColumnDataGet>(table_index, return_types, std::move(collection));
	bind_context.AddGenericBinding(table_index, "__show_select", return_names, return_types);
	return result;
}

BoundStatement Binder::BindDescribeTable(ShowRef &ref) {
	auto &lname = ref.GetTableName();

	string sql;
	if (lname == "\"databases\"") {
		sql = PragmaShowDatabases();
	} else if (lname == "\"schemas\"") {
		sql = "SELECT "
		      " schema.database_name, "
		      " schema.schema_name, "
		      " ((select current_schema() = schema.schema_name) "
		      "  and (select current_database() = schema.database_name)) \"current\" "
		      "FROM duckdb_schemas() schema "
		      "JOIN duckdb_databases dbs USING (database_oid) "
		      "WHERE dbs.internal = false "
		      "ORDER BY all;";
	} else if (lname == "\"tables\"") {
		sql = PragmaShowTables();
	} else if (ref.show_type == ShowType::SHOW_FROM) {
		auto catalog_name = ref.GetCatalogName();
		auto schema_name = ref.GetSchemaName();

		// Check for unqualified name, promote schema to catalog if unambiguous, and set schema_name to empty if so
		Binder::BindSchemaOrCatalog(catalog_name, schema_name);

		optional_idx schema_oid;
		// If fully qualified, check if the schema exists
		if (!catalog_name.empty() && !schema_name.empty()) {
			auto schema_entry = Catalog::GetSchema(context, catalog_name, schema_name, OnEntryNotFound::RETURN_NULL);
			if (!schema_entry) {
				// "a.b" can also name a nested schema
				vector<Identifier> nested_path {catalog_name, schema_name};
				schema_entry = Catalog::GetSchema(context, Identifier(), nested_path, OnEntryNotFound::RETURN_NULL);
				if (schema_entry) {
					schema_oid = schema_entry->oid;
				}
			}
			if (!schema_entry) {
				throw CatalogException("SHOW TABLES FROM: No catalog + schema named \"%s.%s\" found.",
				                       catalog_name.GetIdentifierName(), schema_name.GetIdentifierName());
			}
		} else if (catalog_name.empty() && !schema_name.empty()) {
			// We have a schema name, use default catalog
			auto &client_data = ClientData::Get(context);
			auto &default_entry = client_data.catalog_search_path->GetDefault();
			catalog_name = default_entry.GetCatalog();
			auto schema_entry = Catalog::GetSchema(context, catalog_name, schema_name, OnEntryNotFound::RETURN_NULL);
			if (!schema_entry) {
				throw CatalogException("SHOW TABLES FROM: No catalog + schema named \"%s.%s\" found.",
				                       catalog_name.GetIdentifierName(), schema_name.GetIdentifierName());
			}
		}
		sql = PragmaShowTables(catalog_name.GetIdentifierName(), schema_name.GetIdentifierName(), schema_oid);
	} else if (lname == "\"variables\"") {
		sql = PragmaShowVariables();
	} else if (lname == "__show_tables_expanded") {
		sql = PragmaShowTablesExpanded();
	} else {
		sql = PragmaShow(ref.GetTableName().GetIdentifierName());
	}
	auto parser = Parser::GetBuiltinParser();
	auto select = CreateViewInfo::ParseSelect(parser, sql);
	auto subquery = make_uniq<SubqueryRef>(std::move(select));
	return Bind(*subquery);
}

bool Binder::TryBindShowSetting(ShowRef &ref, BoundStatement &result) {
	auto setting_name = ref.GetTableName();
	Value setting_value;
	if (!context.TryGetCurrentSetting(setting_name, setting_value)) {
		// The setting might be provided by an extension that has not been loaded yet (e.g. "timezone" -> icu).
		// If this is not a known setting at all, fall back to describing a table with this name.
		if (ExtensionHelper::FindExtensionInEntries(setting_name, EXTENSION_SETTINGS).empty()) {
			return false;
		}
		// It is a known extension setting: autoload the extension (this throws a helpful error if autoloading is
		// disabled or the extension is unavailable) and then read the value, mirroring current_setting().
		Catalog::AutoloadExtensionByConfigName(context, setting_name);
		context.TryGetCurrentSetting(setting_name, setting_value);
	}

	vector<LogicalType> types {setting_value.type()};
	vector<Identifier> names {setting_name};

	DataChunk output;
	output.Initialize(Allocator::Get(context), types);
	output.data[0].Append(setting_value);
	output.CheckCardinality(1);

	auto collection = make_uniq<ColumnDataCollection>(context, types);
	collection->Append(output);

	auto table_index = GenerateTableIndex();
	result.names = names;
	result.types = types;
	result.plan = make_uniq<LogicalColumnDataGet>(table_index, types, std::move(collection));
	bind_context.AddGenericBinding(table_index, "__show_setting", names, types);
	return true;
}

//! Warn that using "SHOW name" to describe a table is deprecated in favor of DESCRIBE
static void WarnShowDescribesTable(ClientContext &context, const string &name) {
	DUCKDB_LOG_WARNING(context,
	                   "\"SHOW %s\" resolved to a table. Describing a table with SHOW is deprecated and will be "
	                   "removed in a future release - use \"DESCRIBE %s\" instead, or \"SET show_behavior='setting'\" "
	                   "to make SHOW resolve settings only.",
	                   name, name);
}

//! Warn that using "SHOW (query)" to describe a query is deprecated in favor of DESCRIBE
static void WarnShowDescribesQuery(ClientContext &context) {
	DUCKDB_LOG_WARNING(context, "Using \"SHOW (query)\" to describe a query is deprecated and will be removed in a "
	                            "future release, use \"DESCRIBE (query)\" instead.");
}

BoundStatement Binder::BindShow(ShowRef &ref) {
	BoundStatement result;
	if (ref.GetTableName().empty()) {
		if (!ref.query) {
			throw BinderException("SHOW requires a setting name, a table name or a query");
		}
		// "SHOW (SELECT ...)": describe the columns of the query. Deprecated in favor of DESCRIBE, and not affected
		// by show_behavior (a parenthesized query is neither a setting nor a named table).
		WarnShowDescribesQuery(context);
		return BindDescribeQuery(ref);
	}

	auto behavior = Settings::Get<ShowBehaviorSetting>(context);
	auto name = ref.qualified_name.ToString();
	// A qualified name ("SHOW schema.table") cannot be a setting, so it is only ever a table.
	bool can_be_setting = ref.GetSchemaName().empty() && ref.GetCatalogName().empty();

	switch (behavior) {
	case ShowBehaviorType::SETTING:
		// "SHOW name" only ever resolves a setting.
		if (can_be_setting && TryBindShowSetting(ref, result)) {
			return result;
		}
		throw CatalogException("Setting with name \"%s\" does not exist", name);
	case ShowBehaviorType::TABLE:
		// "SHOW name" only ever describes a table (the historical behavior). Describing a table via SHOW is
		// deprecated, so warn whenever one is shown.
		result = BindDescribe(ref);
		WarnShowDescribesTable(context, name);
		return result;
	default:
		// ShowBehaviorType::AUTO: describe a table if one exists (backwards-compatible, but deprecated - warn), and
		// otherwise fall back to resolving a setting (silently - settings are the long-term behavior).
		try {
			result = BindDescribe(ref);
		} catch (const std::exception &ex) {
			if (can_be_setting && ErrorData(ex).Type() == ExceptionType::CATALOG && TryBindShowSetting(ref, result)) {
				// No such table, but it is a setting - show its value silently.
				return result;
			}
			throw;
		}
		WarnShowDescribesTable(context, name);
		return result;
	}
}

BoundStatement Binder::BindDescribe(ShowRef &ref) {
	return ref.query ? BindDescribeQuery(ref) : BindDescribeTable(ref);
}

BoundStatement Binder::Bind(ShowRef &ref) {
	if (ref.show_type == ShowType::SUMMARY) {
		return BindSummarize(ref);
	}
	if (ref.show_type == ShowType::SHOW) {
		return BindShow(ref);
	}
	return BindDescribe(ref);
}

} // namespace duckdb
