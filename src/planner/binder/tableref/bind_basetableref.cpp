#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"
#include "duckdb/planner/expression_binder/constant_binder.hpp"
#include "duckdb/catalog/catalog_search_path.hpp"
#include "duckdb/planner/tableref/bound_at_clause.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/parser/query_node/cte_node.hpp"
#include "duckdb/planner/operator/logical_dummy_scan.hpp"
#include "duckdb/planner/operator/logical_secure_view.hpp"

namespace duckdb {

static bool TryLoadExtensionForReplacementScan(ClientContext &context, const string &table_name) {
	auto lower_name = StringUtil::Lower(table_name);
	if (!Settings::Get<AutoloadKnownExtensionsSetting>(context)) {
		return false;
	}

	for (const auto &entry : EXTENSION_FILE_POSTFIXES) {
		if (StringUtil::EndsWith(lower_name, entry.name)) {
			ExtensionHelper::AutoLoadExtension(context, entry.extension);
			return true;
		}
	}

	for (const auto &entry : EXTENSION_FILE_CONTAINS) {
		if (StringUtil::Contains(lower_name, entry.name)) {
			ExtensionHelper::AutoLoadExtension(context, entry.extension);
			return true;
		}
	}

	return false;
}

BoundStatement Binder::BindWithReplacementScan(ClientContext &context, BaseTableRef &ref) {
	if (!context.config.use_replacement_scans) {
		return BoundStatement();
	}
	ReplacementScanInput input(ref.GetQualifiedName());
	unique_ptr<TableRef> replacement_function;

	// Connection-scoped replacement scans are consulted first
	for (const auto &scan : context.config.replacement_scans) {
		if (auto result = scan->function(context, input, scan->data.get())) {
			replacement_function = std::move(result);
			break;
		}
	}

	// Then the database-wide ones, including the built-in file scans
	if (!replacement_function) {
		for (const auto &scan : DBConfig::GetConfig(context).replacement_scans.Snapshot()) {
			if (auto result = scan->function(context, input, scan->data.get())) {
				replacement_function = std::move(result);
				break;
			}
		}
	}

	if (!replacement_function) {
		return BoundStatement();
	}

	if (!ref.alias.empty()) {
		// user-provided alias overrides the default alias
		replacement_function->alias = ref.alias;
	} else if (replacement_function->alias.empty()) {
		// if the replacement scan itself did not provide an alias we use the table name
		replacement_function->alias = ref.Table();
	}
	if (replacement_function->type == TableReferenceType::TABLE_FUNCTION) {
		auto &table_function = replacement_function->Cast<TableFunctionRef>();
		table_function.column_name_alias = ref.column_name_alias;
	} else if (replacement_function->type == TableReferenceType::SUBQUERY) {
		auto &subquery = replacement_function->Cast<SubqueryRef>();
		subquery.column_name_alias = ref.column_name_alias;
	} else {
		// carry the alias to the wrapping SubqueryRef so qualified references
		// like `SELECT d.x FROM _ AS d` can resolve against the outer ref
		auto inner_alias = replacement_function->alias;
		auto select_node = make_uniq<SelectNode>();
		select_node->select_list.push_back(make_uniq<StarExpression>());
		select_node->from_table = std::move(replacement_function);
		auto select_stmt = make_uniq<SelectStatement>();
		select_stmt->node = std::move(select_node);
		auto subquery = make_uniq<SubqueryRef>(std::move(select_stmt));
		subquery->alias = std::move(inner_alias);
		subquery->column_name_alias = ref.column_name_alias;
		replacement_function = std::move(subquery);
	}
	if (GetBindingMode() == BindingMode::EXTRACT_REPLACEMENT_SCANS) {
		AddReplacementScan(ref.Table(), replacement_function->Copy());
	}

	return Bind(*replacement_function);
}

unique_ptr<BoundAtClause> Binder::BindAtClause(optional_ptr<AtClause> at_clause) {
	if (!at_clause) {
		return nullptr;
	}
	ConstantBinder binder(*this, context, "AT clause");
	auto expr = binder.Bind(at_clause->ExpressionMutable());
	auto val = ExpressionExecutor::EvaluateScalar(context, *expr);
	return make_uniq<BoundAtClause>(at_clause->Unit(), std::move(val));
}

vector<CatalogSearchEntry> Binder::GetSearchPath(Catalog &catalog, const vector<Identifier> &schema_path,
                                                 bool default_schema_precedence) {
	vector<CatalogSearchEntry> view_search_path;
	auto &catalog_name = catalog.GetName();
	if (!schema_path.empty()) {
		view_search_path.push_back(CatalogSearchEntry::FromSchemaPath(catalog_name, schema_path));
	}
	auto default_schema = catalog.GetDefaultSchema();
	if (default_schema && schema_path.empty() && !default_schema->empty()) {
		view_search_path.emplace_back(catalog_name, *default_schema);
	}
	//! Signal that this catalog should be checked, regardless of the schema in the reference
	view_search_path.emplace_back(catalog_name, INVALID_SCHEMA, default_schema_precedence);
	return view_search_path;
}

void Binder::SetSearchPath(Catalog &catalog, const Identifier &schema) {
	SetSearchPath(catalog, vector<Identifier> {schema});
}

void Binder::SetSearchPath(Catalog &catalog, const vector<Identifier> &schema_path) {
	auto search_path = GetSearchPath(catalog, schema_path);
	entry_retriever.SetSearchPath(std::move(search_path));
}

BoundStatement Binder::Bind(BaseTableRef &ref) {
	QueryErrorContext error_context(ref.query_location);
	// CTEs and views are also referred to using BaseTableRefs, hence need to distinguish here
	// check if the table name refers to a CTE

	// CTE name should never be qualified (i.e. schema_name should be empty)
	// unless we want to refer to the recurring table of "using key".
	BindingAlias binding_alias(ref.GetQualifiedName().Schema(), ref.Table());
	auto ctebinding = GetCTEBinding(binding_alias);
	if (ctebinding && ctebinding->CanBeReferenced()) {
		ctebinding->Reference();

		// There is a CTE binding in the BindContext.
		// This can only be the case if there is a recursive CTE,
		// or a materialized CTE present.
		auto index = GenerateTableIndex();

		auto alias = ref.alias.empty() ? ref.Table() : ref.alias;
		auto names = BindContext::AliasColumnNames(alias, ctebinding->GetColumnNames(), ref.column_name_alias);

		bind_context.AddGenericBinding(index, alias, names, ctebinding->GetColumnTypes());

		bool is_recurring = ref.GetQualifiedName().Schema() == "recurring";

		BoundStatement result;
		result.types = ctebinding->GetColumnTypes();
		result.names = names;
		result.plan = make_uniq<LogicalCTERef>(index, ctebinding->GetIndex(), result.types, names, is_recurring);
		return result;
	}

	// not a CTE
	// extract a table or view from the catalog
	auto at_clause = BindAtClause(ref.at_clause);
	auto entry_at_clause = at_clause ? at_clause.get() : entry_retriever.GetAtClause();
	EntryLookupInfo table_lookup(CatalogType::TABLE_ENTRY, QualifiedName(ref.Table()), entry_at_clause, error_context);
	// resolve the catalog/schema qualification for the lookup only - the reference itself keeps the name as written,
	// which is what name extraction and replacement scans report
	auto bound_name = BindTableName(entry_retriever, ref.GetQualifiedName());
	auto table_or_view =
	    entry_retriever.GetEntry(EntryLookupInfo(table_lookup, bound_name), OnEntryNotFound::RETURN_NULL);
	// we still didn't find the table
	if (GetBindingMode() == BindingMode::EXTRACT_NAMES || GetBindingMode() == BindingMode::EXTRACT_QUALIFIED_NAMES) {
		if (!table_or_view || table_or_view->type == CatalogType::TABLE_ENTRY) {
			// if we are in EXTRACT_NAMES or EXTRACT_QUALIFIED_NAMES, we create a dummy table ref
			if (GetBindingMode() == BindingMode::EXTRACT_QUALIFIED_NAMES) {
				AddTableName(ref.ToString());
			} else {
				AddTableName(ref.Table().GetIdentifierName());
			}

			// add a bind context entry
			auto table_index = GenerateTableIndex();
			auto ref_alias = ref.alias.empty() ? ref.Table() : ref.alias;
			vector<LogicalType> types {LogicalType::INTEGER};
			vector<Identifier> names {Identifier("__dummy_col" + to_string(table_index.index))};
			bind_context.AddGenericBinding(table_index, ref_alias, names, types);

			BoundStatement result;
			result.types = std::move(types);
			result.names = std::move(names);
			result.plan = make_uniq<LogicalDummyScan>(table_index);
			return result;
		}
	}
	if (!table_or_view) {
		// table could not be found: try to bind a replacement scan
		// Try replacement scan bind
		auto replacement_scan_bind_result = BindWithReplacementScan(context, ref);
		if (replacement_scan_bind_result.plan) {
			return replacement_scan_bind_result;
		}

		// Try autoloading an extension, then retry the replacement scan bind
		auto full_path = ReplacementScan::GetFullPath(ref.GetQualifiedName());
		auto extension_loaded = TryLoadExtensionForReplacementScan(context, full_path);
		if (extension_loaded) {
			replacement_scan_bind_result = BindWithReplacementScan(context, ref);
			if (replacement_scan_bind_result.plan) {
				return replacement_scan_bind_result;
			}
		}
		auto &config = DBConfig::GetConfig(context);
		if (context.config.use_replacement_scans && Settings::Get<EnableExternalAccessSetting>(config) &&
		    ExtensionHelper::IsFullPath(full_path)) {
			auto &fs = FileSystem::GetFileSystem(context);
			if (!fs.IsDisabledForPath(full_path) && fs.FileExists(full_path)) {
				throw BinderException(
				    "No extension found that is capable of reading the file \"%s\"\n* If this file is a supported file "
				    "format you can explicitly use the reader functions, such as read_csv, read_json or read_parquet",
				    full_path);
			}
		}

		// if we found a CTE that cannot be referenced that means that there is a circular reference
		if (ctebinding) {
			D_ASSERT(!ctebinding->CanBeReferenced());
			throw BinderException(error_context,
			                      "Circular reference to CTE %s, use WITH RECURSIVE to "
			                      "use recursive CTEs.",
			                      ref.Table());
		}
		// could not find an alternative: bind again to get the error
		// note: this will always throw when using DuckDB as a catalog, but a second look-up might succeed
		// in catalogs that do not have transactional DDL
		table_or_view =
		    entry_retriever.GetEntry(EntryLookupInfo(table_lookup, bound_name), OnEntryNotFound::THROW_EXCEPTION);
	}
	switch (table_or_view->type) {
	case CatalogType::TABLE_ENTRY: {
		// base table
		auto table_index = GenerateTableIndex();
		auto &table = table_or_view->Cast<TableCatalogEntry>();

		auto &properties = GetStatementProperties();
		properties.RegisterDBRead(table.ParentCatalog(), context);

		unique_ptr<FunctionData> bind_data;
		auto scan_function = table.GetScanFunction(context, bind_data, table_lookup);
		if (bind_data && !bind_data->SupportStatementCache()) {
			SetAlwaysRequireRebind();
		}
		// TODO: bundle the type and name vector in a struct (e.g PackedColumnMetadata)
		vector<LogicalType> table_types;
		vector<Identifier> table_names;
		vector<TableColumnType> table_categories;

		vector<LogicalType> return_types;
		vector<Identifier> return_names;
		for (auto &col : table.GetColumns().Logical()) {
			table_types.push_back(col.Type());
			table_names.emplace_back(col.Name());
			return_types.push_back(col.Type());
			return_names.emplace_back(col.Name());
		}
		table_names = BindContext::AliasColumnNames(ref.Table(), table_names, ref.column_name_alias);

		virtual_column_map_t virtual_columns;
		if (scan_function.get_virtual_columns) {
			virtual_columns = scan_function.get_virtual_columns(context, bind_data.get());
		} else {
			virtual_columns = table.GetVirtualColumns();
		}
		auto logical_get =
		    make_uniq<LogicalGet>(table_index, BoundTableFunction(scan_function), std::move(bind_data),
		                          std::move(return_types), std::move(return_names), std::move(virtual_columns));
		if (entry_at_clause) {
			logical_get->at_clause = make_uniq<BoundAtClause>(entry_at_clause->Unit(), entry_at_clause->GetValue());
		}
		auto table_entry = logical_get->GetTable();
		auto &col_ids = logical_get->GetMutableColumnIds();
		if (!table_entry) {
			bind_context.AddBaseTable(table_index, ref.alias, table_names, table_types, col_ids, ref.Table());
		} else {
			bind_context.AddBaseTable(table_index, ref.alias, table_names, table_types, col_ids, *table_entry);
		}
		BoundStatement result;
		result.types = table_types;
		result.names = table_names;
		result.plan = std::move(logical_get);
		return result;
	}
	case CatalogType::VIEW_ENTRY: {
		// the node is a view: get the query that the view represents
		auto &view_catalog_entry = table_or_view->Cast<ViewCatalogEntry>();
		if (!view_catalog_entry.ParentCatalog().IsSystemCatalog()) {
			GetStatementProperties().RegisterDBRead(view_catalog_entry.ParentCatalog(), context);
		}
		// expanding nested views can take a long time, allow the query to be interrupted
		context.InterruptCheck();
		// We need to use a new binder for the view that doesn't reference any CTEs
		// defined for this binder so there are no collisions between the CTEs defined
		// for the view and for the current query
		auto view_binder = Binder::CreateBinder(context, this, BinderType::VIEW_BINDER);
		view_binder->SetCanContainNulls(true);

		// The view may contain CTEs, but maybe only in the cte_map, so we need create CTE nodes for them
		auto query = view_catalog_entry.GetQuery().Copy();
		SubqueryRef subquery(unique_ptr_cast<SQLStatement, SelectStatement>(std::move(query)));

		subquery.alias = ref.alias;
		// construct view names by taking the view aliases
		subquery.column_name_alias = view_catalog_entry.aliases;
		// now apply the subquery column aliases
		for (idx_t i = 0; i < ref.column_name_alias.size(); i++) {
			if (i < subquery.column_name_alias.size()) {
				// override alias
				subquery.column_name_alias[i] = ref.column_name_alias[i];
			} else {
				subquery.column_name_alias.push_back(ref.column_name_alias[i]);
			}
		}

		// when binding a view, we always look into the catalog/schema where the view is stored first
		auto view_search_path =
		    GetSearchPath(view_catalog_entry.ParentCatalog(), view_catalog_entry.ParentSchema().GetSchemaPath(), true);
		view_binder->entry_retriever.SetSearchPath(std::move(view_search_path));
		// propagate the AT clause through the view
		view_binder->entry_retriever.SetAtClause(entry_at_clause);

		// bind the child subquery
		view_binder->AddBoundView(view_catalog_entry);
		auto bound_child = view_binder->Bind(subquery);
		if (!view_binder->correlated_columns.empty()) {
			throw BinderException("Contents of view were altered - view bound correlated columns");
		}
		// update the view binding with the bound view information
		view_catalog_entry.UpdateBinding(bound_child.types, bound_child.names);
		auto root_index = bound_child.plan->GetRootIndex();
		if (view_catalog_entry.security_type == ViewSecurityType::SECURE_VIEW) {
			// wrap the plan of a secure view - this prevents the optimizer from pushing into the view
			bound_child.plan = make_uniq<LogicalSecureView>(
			    view_catalog_entry.name.GetIdentifierName(),
			    view_catalog_entry.ParentSchema().GetQualifiedName(view_catalog_entry.name), bound_child.types,
			    entry_at_clause, std::move(bound_child.plan));
		}
		bind_context.AddView(root_index, subquery.alias, subquery, bound_child, view_catalog_entry);
		return bound_child;
	}
	default:
		throw InternalException("Catalog entry type");
	}
}
} // namespace duckdb
