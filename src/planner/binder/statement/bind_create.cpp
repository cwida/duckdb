#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/trigger_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/type_catalog_entry.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/catalog/catalog_search_path.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/function/scalar_macro_function.hpp"
#include "duckdb/function/table/table_scan.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/optimizer/remote_pushdown_optimizer.hpp"
#include "duckdb/parser/constraints/foreign_key_constraint.hpp"
#include "duckdb/parser/constraints/list.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/parsed_data/create_index_info.hpp"
#include "duckdb/parser/parsed_data/create_macro_info.hpp"
#include "duckdb/parser/parsed_data/create_trigger_info.hpp"
#include "duckdb/parser/parsed_data/create_secret_info.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/common_table_expression_info.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/bound_query_node.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression_binder/constant_binder.hpp"
#include "duckdb/planner/expression_binder/index_binder.hpp"
#include "duckdb/planner/expression_binder/select_binder.hpp"
#include "duckdb/planner/operator/logical_create.hpp"
#include "duckdb/planner/operator/logical_create_table.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/planner/query_node/bound_select_node.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/common/extension_type_info.hpp"
#include "duckdb/common/type_visitor.hpp"
#include "duckdb/function/table_macro_function.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parser/expression/type_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"

namespace duckdb {

static unique_ptr<CommonTableExpressionInfo> MakeTriggerValidationCTE(const TableCatalogEntry &table) {
	auto alias_select = make_uniq<SelectNode>();
	alias_select->select_list.push_back(make_uniq<StarExpression>());
	auto alias_table_ref = make_uniq<BaseTableRef>();
	alias_table_ref->SetQualifiedName(table.schema.GetQualifiedName(table.name));
	alias_select->from_table = std::move(alias_table_ref);
	auto alias_cte = make_uniq<CommonTableExpressionInfo>();
	alias_cte->query_node = std::move(alias_select);
	return alias_cte;
}

void Binder::BindSchemaOrCatalog(CatalogEntryRetriever &retriever, Identifier &catalog, Identifier &schema) {
	auto &context = retriever.GetContext();
	if (schema.empty()) {
		return;
	}
	if (!catalog.empty()) {
		return;
	}
	// schema is specified - but catalog is not
	// try searching for the catalog instead
	auto &db_manager = DatabaseManager::Get(context);
	auto database = db_manager.GetDatabase(context, schema);
	if (!database) {
		//! No database by that name was found
		return;
	}
	// we have a database with this name
	// check if there is a schema
	auto &search_path = retriever.GetSearchPath();
	auto catalog_names = search_path.GetCatalogsForSchema(schema);
	if (catalog_names.empty()) {
		// with no default database there is no schema for the name to be ambiguous with
		auto default_database = DatabaseManager::TryGetDefaultDatabase(context);
		if (!IsInvalidCatalog(default_database)) {
			catalog_names.emplace_back(std::move(default_database));
		}
	}
	for (auto &catalog_name : catalog_names) {
		auto catalog_ptr = Catalog::GetCatalogEntry(retriever, catalog_name);
		if (!catalog_ptr) {
			continue;
		}
		if (catalog_ptr->CheckAmbiguousCatalogOrSchema(context, schema)) {
			throw BinderException(
			    "Ambiguous reference to catalog or schema %s - use a fully qualified path like '%s.%s'", schema,
			    SQLIdentifier(catalog_name), SQLIdentifier(schema));
		}
	}
	catalog = schema;
	schema = Identifier();
}

void Binder::BindSchemaOrCatalog(ClientContext &context, Identifier &catalog, Identifier &schema) {
	CatalogEntryRetriever retriever(context);
	BindSchemaOrCatalog(retriever, catalog, schema);
}

void Binder::BindSchemaOrCatalog(Identifier &catalog, Identifier &schema) {
	BindSchemaOrCatalog(context, catalog, schema);
}

void Binder::BindSchemaOrCatalog(CatalogEntryRetriever &retriever, QualifiedName &qualified_name) {
	auto &path = qualified_name.Path();
	if (path.size() != 2) {
		// only a lone qualifier ("x.name") can be either a schema or a catalog - any deeper qualification is
		// positional and is resolved by ResolveCatalog
		return;
	}
	Identifier catalog;
	Identifier schema = path[0];
	BindSchemaOrCatalog(retriever, catalog, schema);
	qualified_name = QualifiedName(std::move(catalog), std::move(schema), qualified_name.Name());
}

void Binder::BindSchemaOrCatalog(ClientContext &context, QualifiedName &qualified_name) {
	CatalogEntryRetriever retriever(context);
	BindSchemaOrCatalog(retriever, qualified_name);
}

void Binder::BindSchemaOrCatalog(QualifiedName &qualified_name) {
	BindSchemaOrCatalog(context, qualified_name);
}

Identifier Binder::BindCatalog(const Identifier &catalog) {
	auto &db_manager = DatabaseManager::Get(context);
	optional_ptr<AttachedDatabase> database = db_manager.GetDatabase(context, catalog);
	if (database) {
		return db_manager.GetDatabase(context, catalog).get()->GetName();
	} else {
		return db_manager.GetDefaultDatabase(context);
	}
}

void Binder::SearchSchema(CreateInfo &info) {
	auto &search_path = ClientData::Get(context).catalog_search_path;
	// determine the catalog and the (possibly nested) schema path. ResolveCatalog (via BindSchemaOrCatalog) decides
	// which leading components form the catalog; we pass default_catalog=false so the catalog is left empty when none
	// is given - that lets us tell "no catalog specified" apart from an explicit catalog when applying defaults below.
	auto resolved = ResolveCatalog(context, info.GetQualifiedName(), false);
	auto &path = resolved.Path();
	Identifier name = path.back();
	Identifier catalog = path.front();
	// the schema components sit between the catalog and the name; skip empty placeholders (e.g. from
	// QualifiedName(catalog, "", name)) so that "no schema given" applies the default schema below
	vector<Identifier> schema_path;
	for (idx_t i = 1; i + 1 < path.size(); i++) {
		if (!path[i].empty()) {
			schema_path.push_back(path[i]);
		}
	}

	if (IsInvalidCatalog(catalog) && info.temporary) {
		catalog = Identifier::TempCatalog();
	}
	if (IsInvalidCatalog(catalog) && schema_path.empty()) {
		// no catalog and no schema given: use the search path default for both
		auto &default_entry = search_path->GetDefault();
		catalog = default_entry.GetCatalog();
		schema_path.push_back(default_entry.GetSchema());
	} else if (schema_path.empty()) {
		// a catalog was given but no schema: use the catalog's default schema
		auto default_schema = search_path->GetDefaultSchema(context, catalog);
		if (!default_schema) {
			throw BinderException("Catalog %s has no default schema - specify a schema explicitly", catalog);
		}
		schema_path.push_back(*default_schema);
	} else if (IsInvalidCatalog(catalog)) {
		// a schema was given but no catalog: resolve the catalog that holds it
		catalog = search_path->ResolveCatalog(schema_path[0]);
	}
	if (IsInvalidCatalog(catalog)) {
		catalog = DatabaseManager::GetDefaultDatabase(context);
	}
	if (!info.temporary) {
		if (catalog == TEMP_CATALOG) {
			throw ParserException("Only TEMPORARY table names can use the \"%s\" catalog", TEMP_CATALOG);
		}
	} else if (catalog != TEMP_CATALOG) {
		throw ParserException("TEMPORARY table names can *only* use the \"%s\" catalog", TEMP_CATALOG);
	}
	info.SetQualifiedName(
	    QualifiedName::FromCatalogSchema(std::move(catalog), std::move(schema_path), std::move(name)));
}

QualifiedName Binder::ResolveCatalog(ClientContext &context, const QualifiedName &name, bool default_catalog) {
	CatalogEntryRetriever retriever(context);
	return ResolveCatalog(retriever, name, default_catalog);
}

QualifiedName Binder::ResolveCatalog(CatalogEntryRetriever &retriever, const QualifiedName &name,
                                     bool default_catalog) {
	auto &context = retriever.GetContext();
	auto path = name.Path();
	if (path.empty()) {
		return name;
	}
	// split off the trailing name; the remaining components are the qualification to resolve
	Identifier trailing = std::move(path.back());
	path.pop_back();
	Identifier catalog;
	if (path.size() == 1) {
		// a single qualifier ("x.name"): x may be a schema, or a catalog - and if it names both it is ambiguous
		Identifier candidate;
		Identifier first = path[0];
		BindSchemaOrCatalog(retriever, candidate, first);
		if (!candidate.empty()) {
			catalog = std::move(candidate);
			path.erase(path.begin());
		}
	} else if (path.size() > 1) {
		// multiple qualifiers ("cat.schema.name" or a nested "s1.s2.name"): the leading component is the catalog when
		// it names an attached database. It is positionally unambiguous, so we do not run the catalog/schema ambiguity
		// check (which is only relevant for a lone qualifier).
		auto &db_manager = DatabaseManager::Get(context);
		if (db_manager.GetDatabase(context, path[0])) {
			catalog = std::move(path[0]);
			path.erase(path.begin());
		}
	}
	if (default_catalog && IsInvalidCatalog(catalog)) {
		// the leading component (if any) is a schema - resolve the catalog that holds it, else the default database
		catalog = retriever.GetSearchPath().ResolveCatalog(path.empty() ? Identifier() : path[0]);
	}
	path.insert(path.begin(), std::move(catalog));
	return QualifiedName(std::move(path), std::move(trailing));
}

QualifiedName Binder::BindTableName(CatalogEntryRetriever &retriever, const QualifiedName &name) {
	auto resolved = ResolveCatalog(retriever, name, false);
	auto &path = resolved.Path();
	if (path.size() < 2) {
		throw InternalException("Binder::BindTableName called with an empty name");
	}
	vector<Identifier> schema_path(path.begin() + 1, path.end() - 1);
	if (schema_path.size() <= 1) {
		// unqualified, or qualified with a single schema level: keep the (catalog, schema, name) shape so that the
		// regular search-path based lookup applies
		return QualifiedName(path.front(), schema_path.empty() ? Identifier() : schema_path[0], resolved.Name());
	}
	// a nested schema path was given - resolve the catalog holding the outermost schema so that the resulting
	// [catalog, schema path..., name] is fully qualified
	auto catalog = path.front();
	if (IsInvalidCatalog(catalog)) {
		EntryLookupInfo schema_lookup(CatalogType::SCHEMA_ENTRY, QualifiedName(schema_path[0]));
		auto schema = Catalog::GetSchema(retriever, schema_lookup, OnEntryNotFound::RETURN_NULL);
		if (schema) {
			catalog = schema->ParentCatalog().GetName();
		} else {
			catalog = DatabaseManager::GetDefaultDatabase(retriever.GetContext());
		}
	}
	return QualifiedName::FromCatalogSchema(std::move(catalog), std::move(schema_path), resolved.Name());
}

QualifiedName Binder::BindTableName(const QualifiedName &name) {
	CatalogEntryRetriever retriever(context);
	return BindTableName(retriever, name);
}

void Binder::RegisterEntryRead(optional_ptr<Binder> binder, ClientContext &context, CatalogEntry &entry) {
	if (!binder) {
		// no binder available (e.g. when re-binding a deserialized plan) - nothing is cached in that case
		return;
	}
	binder->GetStatementProperties().RegisterDBRead(entry.ParentCatalog(), context);
}

void Binder::BindCreateSchema(CreateSchemaInfo &info) {
	if (info.temporary) {
		// unsupported - otherwise the schema silently lands in (and is persisted to) the default catalog
		throw BinderException("Temporary schemas are not supported");
	}
	// the qualified name carries the dotted path with the new schema as the last component; resolve its leading
	// component into a catalog (prepending the default catalog when it is a schema)
	info.SetQualifiedName(ResolveCatalog(context, info.GetQualifiedName()));

	auto &resolved_catalog = Catalog::GetCatalog(context, info.SchemaCatalog());
	auto supports_create_schema = resolved_catalog.SupportsCreateSchema(info);
	if (supports_create_schema.HasError()) {
		supports_create_schema.Throw();
	}
	if (info.IsNested()) {
		// nested schemas can only be persisted with storage version v2.0.0 or higher
		auto &attached = resolved_catalog.GetAttached();
		if (attached.HasStorageManager()) {
			auto &storage_manager = attached.GetStorageManager();
			if (!attached.IsTemporary() && !storage_manager.InMemory() &&
			    storage_manager.GetStorageVersion() < StorageVersion::V2_0_0) {
				throw BinderException("Nested schemas are only supported for storage versions v2.0.0 and higher.\n"
				                      "Use an in-memory database, or ATTACH with (STORAGE_VERSION 'v2.0.0')");
			}
		}
	}
}

SchemaCatalogEntry &Binder::BindSchema(CreateInfo &info) {
	// resolve the target into [catalog, schema_path..., name] and fetch the (possibly nested) schema it lives in
	SearchSchema(info);
	auto &path = info.GetQualifiedName().Path();
	vector<Identifier> schema_path(path.begin() + 1, path.end() - 1);
	auto &schema_obj = *Catalog::GetSchema(context, path.front(), schema_path, OnEntryNotFound::THROW_EXCEPTION);
	D_ASSERT(schema_obj.type == CatalogType::SCHEMA_ENTRY);
	GetStatementProperties().RegisterDBModify(schema_obj.catalog, context,
	                                          DatabaseModificationType::CREATE_CATALOG_ENTRY);
	return schema_obj;
}

SchemaCatalogEntry &Binder::BindCreateSchema(CreateInfo &info) {
	auto &schema = BindSchema(info);
	if (schema.catalog.IsSystemCatalog()) {
		throw BinderException("Cannot create entry in system catalog");
	}
	return schema;
}

void Binder::SetCatalogLookupCallback(catalog_entry_callback_t callback) {
	entry_retriever.SetCallback(std::move(callback));
}

void Binder::BindView(ClientContext &context, const SelectStatement &stmt, const Identifier &catalog_name,
                      const vector<Identifier> &schema_path, optional_ptr<LogicalDependencyList> dependencies,
                      const vector<Identifier> &aliases, vector<LogicalType> &result_types,
                      vector<Identifier> &result_names) {
	auto view_binder = Binder::CreateBinder(context);
	auto &catalog = Catalog::GetCatalog(context, catalog_name);

	if (dependencies) {
		view_binder->SetCatalogLookupCallback([&dependencies, &catalog](CatalogEntry &entry) {
			if (&catalog != &entry.ParentCatalog()) {
				// Don't register dependencies between catalogs
				return;
			}
			dependencies->AddDependency(entry);
		});
	}
	view_binder->SetCanContainNulls(true);

	auto view_search_path = view_binder->GetSearchPath(catalog, schema_path, true);
	view_binder->entry_retriever.SetSearchPath(std::move(view_search_path));

	auto copy = stmt.Copy();
	auto query_node = view_binder->Bind(*copy);
	if (aliases.size() > query_node.names.size()) {
		throw BinderException("More VIEW aliases than columns in query result");
	}
	result_types = query_node.types;
	result_names = query_node.names;
}

void Binder::BindCreateViewInfo(CreateViewInfo &base) {
	// references to the view's own catalog are resolved through the view's search path anyway - drop the qualifier so
	// the view keeps working when the database is attached under a different alias
	auto &view_catalog = base.GetQualifiedName().Catalog();
	if (base.query && !view_catalog.empty()) {
		RemotePushdownOptimizer::StripCatalogName(*base.query, view_catalog);
	}
	if (base.binding_mode == CreateViewBindingMode::SKIP_BINDING) {
		return;
	}
	// DML statements (INSERT/UPDATE/DELETE) are not allowed as CTE bodies inside a view,
	// because the DML would execute every time the view is queried.
	for (auto &kv : base.query->node->cte_map.map) {
		auto &cte = *kv.second;
		if (!cte.query_node) {
			continue;
		}
		auto t = cte.query_node->type;
		if (t == QueryNodeType::INSERT_QUERY_NODE || t == QueryNodeType::UPDATE_QUERY_NODE ||
		    t == QueryNodeType::DELETE_QUERY_NODE) {
			throw BinderException("DML statements (INSERT/UPDATE/DELETE) are not allowed as CTE bodies inside a VIEW");
		}
		if (t == QueryNodeType::COPY_QUERY_NODE) {
			throw BinderException("COPY statements are not allowed as CTE bodies inside a VIEW");
		}
	}
	optional_ptr<LogicalDependencyList> dependencies;
	if (Settings::Get<EnableViewDependenciesSetting>(context)) {
		dependencies = base.dependencies;
	}
	BindView(context, *base.query, base.GetQualifiedName().Catalog(), base.GetQualifiedName().SchemaPath(),
	         dependencies, base.aliases, base.types, base.names);
}

SchemaCatalogEntry &Binder::BindCreateFunctionInfo(CreateInfo &info) {
	//! Set to identify exact matches in macro overloads
	struct VectorOfLogicalTypeHash {
		std::size_t operator()(const vector<LogicalType> &k) const {
			auto hash = std::hash<size_t>()(k.size());
			for (auto &type : k) {
				hash = CombineHash(hash, type.Hash());
			}
			return hash;
		}
	};

	struct VectorOfLogicalTypeEquality {
		bool operator()(const vector<LogicalType> &a, const vector<LogicalType> &b) const {
			if (a.size() != b.size()) {
				return false;
			}
			for (idx_t i = 0; i < a.size(); i++) {
				if (a[i] != b[i]) {
					return false;
				}
			}
			return true;
		}
	};

	using vector_of_logical_type_set_t =
	    unordered_set<vector<LogicalType>, VectorOfLogicalTypeHash, VectorOfLogicalTypeEquality>;

	// Bind the catalog/schema
	SearchSchema(info);
	auto &catalog = Catalog::GetCatalog(context, info.GetQualifiedName().Catalog());

	// Figure out if we can store typed macro parameters
	auto &attached = catalog.GetAttached();
	auto store_types = true;
	if (attached.HasStorageManager()) {
		// If DuckDB is used as a storage, we must check the version.
		auto &storage_manager = attached.GetStorageManager();
		const auto since = StorageCompatibility::FromString("v1.4.0").storage_version;
		store_types = info.temporary || attached.IsTemporary() || storage_manager.InMemory() ||
		              storage_manager.GetStorageVersion() >= since;
	}
	// try to bind each of the included functions
	vector_of_logical_type_set_t type_overloads;
	auto &base = info.Cast<CreateMacroInfo>();
	for (auto &function : base.macros) {
		if (!store_types) {
			for (const auto &type : function->types) {
				if (type.id() != LogicalTypeId::UNKNOWN) {
					string msg = "Typed macro parameters are only supported for storage versions v1.4.0 and higher.\n";
					msg += "Use an in-memory database, ATTACH with (STORAGE_VERSION v1.4.0), or create a TEMP macro";
					throw BinderException(msg);
				}
			}
		}

		if (info.type == CatalogType::MACRO_ENTRY) {
			auto &scalar_function = function->Cast<ScalarMacroFunction>();
			if (scalar_function.expression->HasParameter()) {
				throw BinderException("Parameter expressions within macro's are not supported!");
			}
		} else {
			D_ASSERT(info.type == CatalogType::TABLE_MACRO_ENTRY);
			auto &table_function = function->Cast<TableMacroFunction>();
			ParsedExpressionIterator::EnumerateQueryNodeChildren(
			    *table_function.query_node, [](unique_ptr<ParsedExpression> &child) {
				    if (child->HasParameter()) {
					    throw BinderException("Parameter expressions within macro's are not supported!");
				    }
			    });
		}

		// Constant-fold all default parameter expressions
		identifier_map_t<Value> default_values;
		identifier_set_t integer_literal_defaults;
		for (auto &it : function->default_parameters) {
			auto &param_name = it.first;
			auto &param_expr = it.second;

			if (param_expr->GetExpressionType() == ExpressionType::VALUE_CONSTANT) {
				auto value = param_expr->Cast<ConstantExpression>().GetLiteral().ToValue();
				if (value.type().IsIntegral() && !value.IsNull()) {
					integer_literal_defaults.insert(param_name);
				}
				default_values[param_name] = std::move(value);
				continue;
			}

			ConstantBinder binder(*this, context, StringUtil::Format("Default value for parameter %s", param_name));
			auto default_expr = param_expr->Copy();
			auto bound_default = binder.Bind(default_expr);
			if (!bound_default->IsFoldable()) {
				auto msg = StringUtil::Format("Default value '%s' for parameter %s is not a constant expression.",
				                              param_expr->ToString(), param_name);
				throw BinderException(msg);
			}

			auto default_val = ExpressionExecutor::EvaluateScalar(context, *bound_default);

			// Save this back as a constant expression
			auto const_expr = ConstantExpression::FromValue(default_val);
			const_expr->SetAlias(param_name);
			it.second = std::move(const_expr);
			default_values[param_name] = std::move(default_val);
		}

		// Resolve any user type arguments
		for (idx_t param_idx = 0; param_idx < function->types.size(); param_idx++) {
			auto &type = function->types[param_idx];
			if (type.id() == LogicalTypeId::UNKNOWN) {
				continue;
			}
			if (type.id() == LogicalTypeId::UNBOUND) {
				BindLogicalType(type);
			}
			const auto &param_name = function->parameters[param_idx]->Cast<ColumnRefExpression>().GetColumnName();
			auto it = default_values.find(param_name);
			if (it != default_values.end()) {
				auto &value = it->second;
				auto val_type = value.type();
				if (integer_literal_defaults.find(param_name) != integer_literal_defaults.end()) {
					val_type = LogicalType::INTEGER_LITERAL(value);
				}
				if (CastFunctionSet::ImplicitCastCost(context, val_type, type) < 0) {
					auto msg = StringUtil::Format(
					    "Default value '%s' for parameter %s cannot be implicitly cast to '%s'.",
					    function->default_parameters[param_name]->ToString(), param_name, type.ToString());
					throw BinderException(msg + " Please add an explicit type cast.");
				}
			}
		}

		vector<LogicalType> dummy_types;
		vector<Identifier> dummy_names;
		// positional parameters
		for (idx_t param_idx = 0; param_idx < function->parameters.size(); param_idx++) {
			dummy_types.emplace_back(function->types.empty() ? LogicalType::UNKNOWN : function->types[param_idx]);
			dummy_names.emplace_back(function->parameters[param_idx]->Cast<ColumnRefExpression>().GetColumnName());
		}

		if (!type_overloads.insert(dummy_types).second) {
			throw BinderException(
			    "Ambiguity in macro overloads - macro %s() has multiple definitions with the same parameters",
			    base.GetFunctionName().GetIdentifierName());
		}

		auto this_macro_binding =
		    make_uniq<DummyBinding>(dummy_types, dummy_names, base.GetFunctionName().GetIdentifierName());
		macro_binding = this_macro_binding.get();

		auto &dependencies = base.dependencies;
		const auto should_create_dependencies = Settings::Get<EnableMacroDependenciesSetting>(context);
		const auto binder_callback = [&dependencies, &catalog](CatalogEntry &entry) {
			if (&catalog != &entry.ParentCatalog()) {
				// Don't register any cross-catalog dependencies
				return;
			}
			// Register any catalog entry required to bind the macro function
			dependencies.AddDependency(entry);
		};

		// bind it to verify the function was defined correctly
		ErrorData error;
		if (info.type == CatalogType::MACRO_ENTRY) {
			BoundSelectNode sel_node;
			SelectBinder binder(*this, context, sel_node);
			if (should_create_dependencies) {
				binder.SetCatalogLookupCallback(binder_callback);
			}

			// create a copy of the expression because we do not want to alter the original
			auto expression = function->Cast<ScalarMacroFunction>().expression->Copy();
			ExpressionBinder::QualifyColumnNames(*this, expression);
			try {
				auto bind_result = binder.Bind(expression, 0, false);
				if (bind_result.HasError()) {
					error = std::move(bind_result.error);
					error.Throw();
				}
			} catch (const std::exception &ex) {
				error = ErrorData(ex);
			}
		} else {
			D_ASSERT(info.type == CatalogType::TABLE_MACRO_ENTRY);
			auto dummy_binder = CreateBinder(context, this);
			if (should_create_dependencies) {
				dummy_binder->SetCatalogLookupCallback(binder_callback);
			}

			// create a copy of the query node because we do not want to alter the original
			auto query_node = function->Cast<TableMacroFunction>().query_node->Copy();
			ParsedExpressionIterator::EnumerateQueryNodeChildren(
			    *query_node, [&dummy_binder](unique_ptr<ParsedExpression> &child) {
				    ExpressionBinder::QualifyColumnNames(*dummy_binder, child);
			    });
			try {
				dummy_binder->Bind(*query_node);
			} catch (const std::exception &ex) {
				error = ErrorData(ex);
			}
		}

		// if we cannot resolve parameters we postpone binding until the macro function is used
		if (error.HasError() && error.Type() != ExceptionType::PARAMETER_NOT_RESOLVED) {
			error.Throw();
		}
	}

	return BindCreateSchema(info);
}

LogicalType Binder::BindLogicalType(const ParsedExpression &type_expr) {
	ConstantBinder binder(*this, context, "Type binding");
	auto copy = type_expr.Copy();
	auto expr = binder.Bind(copy);

	if (!expr->IsFoldable()) {
		throw BinderException(type_expr, "Type expression is not constant");
	}

	if (expr->GetReturnType() != LogicalTypeId::TYPE) {
		throw BinderException(type_expr, "Expected a type returning expression, but got expression of type '%s'",
		                      expr->GetReturnType().ToString());
	}

	// Shortcut for constant expressions
	if (expr->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		auto &const_expr = expr->Cast<BoundConstantExpression>();
		return TypeValue::GetType(const_expr.GetValue());
	}

	// Else, evaluate the type expression
	auto type_value = ExpressionExecutor::EvaluateScalar(context, *expr);
	D_ASSERT(type_value.type().id() == LogicalTypeId::TYPE);
	return TypeValue::GetType(type_value);
}

void Binder::BindLogicalType(LogicalType &type) {
	// Check if we need to bind this type at all
	if (!TypeVisitor::Contains(type, LogicalTypeId::UNBOUND)) {
		return;
	}

	// Replace all unbound types within the type
	//   Normally, the unbound type is the root type, but it can also be nested within other types if we e.g.
	//   alter-table and change a struct field.
	type = TypeVisitor::VisitReplace(type, [&](const LogicalType &ty) {
		if (ty.id() == LogicalTypeId::UNBOUND) {
			auto &type_expr = UnboundType::GetTypeExpression(ty);
			return BindLogicalType(*type_expr);
		}

		return ty;
	});
}

bool BoundBodyContainsTrigger(const LogicalOperator &op);

SchemaCatalogEntry &Binder::BindCreateTriggerInfo(CreateTriggerInfo &create_trigger_info) {
	if (create_trigger_info.temporary) {
		// unsupported - a trigger is temporary iff its base table is (see below)
		throw BinderException("Temporary triggers are not supported");
	}
	// Resolve the base table first — triggers inherit catalog/schema from their table (like Postgres).
	TableDescription table_description(create_trigger_info.base_table->GetQualifiedName());
	auto table_ref = make_uniq<BaseTableRef>(table_description);
	auto bound_table = Bind(*table_ref);
	if (bound_table.plan->type != LogicalOperatorType::LOGICAL_GET) {
		throw BinderException("CREATE TRIGGER requires a base table, not a view or subquery");
	}
	auto &get = bound_table.plan->Cast<LogicalGet>();
	auto table_ptr = get.GetTable();
	if (!table_ptr) {
		throw BinderException("CREATE TRIGGER requires a base table");
	}
	auto &table = *table_ptr;
	// Dropping the trigger's own table must not require CASCADE. If the trigger body also references this table,
	// the body-reference dependency below merges in ALTER-blocking flags.
	create_trigger_info.dependencies.AddDependency(table, DependencyDependentFlags());

	// Trigger inherits the catalog and the (possibly nested) schema from the base table
	create_trigger_info.SetQualifiedName(table.schema.GetQualifiedName(create_trigger_info.GetQualifiedName().Name()));
	// ... and its temporariness, so that a trigger on a temporary table lands in the temp catalog
	create_trigger_info.temporary = table.temporary;

	auto &schema = BindCreateSchema(create_trigger_info);

	// Block trigger creation on databases with an older storage version
	auto &catalog = Catalog::GetCatalog(context, create_trigger_info.GetQualifiedName().Catalog());
	auto &attached = catalog.GetAttached();
	if (attached.HasStorageManager()) {
		auto &storage_manager = attached.GetStorageManager();
		const auto since = StorageVersion::V2_0_0;
		if (!create_trigger_info.temporary && !attached.IsTemporary() && !storage_manager.InMemory() &&
		    storage_manager.GetStorageVersion() < since) {
			string msg = "CREATE TRIGGER is only supported for storage versions v2.0.0 and higher.\n";
			msg += "Use an in-memory database, ATTACH with (STORAGE_VERSION v2.0.0)";
			throw BinderException(msg);
		}
	}

	if (create_trigger_info.timing == TriggerTiming::INSTEAD_OF) {
		throw NotImplementedException("INSTEAD OF triggers are not yet supported");
	}

	// Validate UPDATE OF columns exist
	if (create_trigger_info.event_type == TriggerEventType::UPDATE_EVENT && !create_trigger_info.columns.empty()) {
		for (const auto &col_name : create_trigger_info.columns) {
			if (!table.ColumnExists(col_name)) {
				throw BinderException("Column %s does not exist in table %s", col_name, table.name);
			}
		}
	}
	if (create_trigger_info.for_each == TriggerForEach::ROW &&
	    (!create_trigger_info.referencing_new_table.empty() || !create_trigger_info.referencing_old_table.empty())) {
		throw BinderException("REFERENCING is not valid for FOR EACH ROW triggers");
	}
	if (create_trigger_info.for_each == TriggerForEach::ROW && create_trigger_info.timing != TriggerTiming::AFTER) {
		throw NotImplementedException("BEFORE FOR EACH ROW triggers are not yet supported");
	}
	if (create_trigger_info.for_each == TriggerForEach::ROW &&
	    create_trigger_info.event_type == TriggerEventType::UPDATE_EVENT) {
		throw NotImplementedException("UPDATE FOR EACH ROW triggers are not yet supported");
	}
	if ((!create_trigger_info.referencing_new_table.empty() || !create_trigger_info.referencing_old_table.empty()) &&
	    create_trigger_info.timing != TriggerTiming::AFTER) {
		throw BinderException("Transition tables can only be specified for AFTER triggers");
	}
	if ((!create_trigger_info.referencing_new_table.empty() || !create_trigger_info.referencing_old_table.empty()) &&
	    !create_trigger_info.columns.empty()) {
		throw BinderException("UPDATE OF is not valid with transition tables");
	}
	if (!create_trigger_info.referencing_old_table.empty()) {
		if (create_trigger_info.event_type == TriggerEventType::INSERT_EVENT) {
			throw BinderException("REFERENCING OLD TABLE AS is not valid for AFTER INSERT triggers");
		}
	}
	if (!create_trigger_info.referencing_new_table.empty() &&
	    create_trigger_info.event_type == TriggerEventType::DELETE_EVENT) {
		throw BinderException("REFERENCING NEW TABLE AS is not valid for AFTER DELETE triggers");
	}

	auto opposite_for_each =
	    create_trigger_info.for_each == TriggerForEach::ROW ? TriggerForEach::STATEMENT : TriggerForEach::ROW;
	// Statement and row triggers use separate expansion paths that don't compose, so reject mixing them per event.
	// CREATE OR REPLACE that targets the same-named trigger is allowed: that trigger is atomically replaced,
	// so the final catalog contains only the new one and there is no mixing.
	auto conflicting = table.GetTriggersForEvent(table.ParentCatalog().GetCatalogTransaction(context),
	                                             create_trigger_info.event_type, opposite_for_each);
	bool is_replace = create_trigger_info.on_conflict == OnCreateConflict::REPLACE_ON_CONFLICT;
	auto has_real_conflict =
	    std::any_of(conflicting.begin(), conflicting.end(), [&](const_reference<TriggerCatalogEntry> t) {
		    return !(is_replace && t.get().name == create_trigger_info.GetTriggerName());
	    });
	if (has_real_conflict) {
		throw NotImplementedException(
		    "Mixing FOR EACH STATEMENT and FOR EACH ROW triggers on the same table is not yet supported");
	}

	// Validate the trigger body using an isolated binder (own GlobalBinderState).
	// Set up trigger_expanded_tables to match runtime behavior.
	// Set up trigger_creation_table to detect recursive triggers during the validation.
	auto validation_binder = Binder::CreateBinder(context);
	validation_binder->global_binder_state->trigger_expanded_tables.insert(table);
	validation_binder->global_binder_state->trigger_creation_table = &table;
	validation_binder->global_binder_state->trigger_creation_name = create_trigger_info.GetTriggerName();
	// Track every catalog entry resolved while binding the trigger body as a dependency, so an ALTER that could
	// invalidate the body is blocked, and DROP of a table/view/function referenced by the body is blocked too -
	// except for the trigger's own base table, which can never outlive it regardless of what the body reads.
	validation_binder->SetCatalogLookupCallback([&create_trigger_info, &catalog, &table](CatalogEntry &entry) {
		if (&catalog != &entry.ParentCatalog()) {
			if (entry.internal) {
				return;
			}
			throw BinderException("Trigger %s cannot reference %s from a different catalog (%s)",
			                      create_trigger_info.GetTriggerName(), entry.name, entry.ParentCatalog().GetName());
		}
		DependencyDependentFlags flags;
		flags.SetAlterBlocking();
		if (&entry != &table) {
			flags.SetBlocking();
		}
		create_trigger_info.dependencies.AddDependency(entry, flags);
	});
	auto body_copy = create_trigger_info.trigger_action->Copy();

	for (const auto &alias : {create_trigger_info.referencing_new_table, create_trigger_info.referencing_old_table}) {
		if (!alias.empty() && body_copy->cte_map.map.find(alias) == body_copy->cte_map.map.end()) {
			body_copy->cte_map.map[alias] = MakeTriggerValidationCTE(table);
		}
	}
	// For FOR EACH ROW: register NEW (INSERT) or OLD (DELETE) as a generic binding so that scope resolution can
	// resolve NEW.col / OLD.col references.
	unique_ptr<ExpressionBinder> row_scope_binder;
	if (create_trigger_info.for_each == TriggerForEach::ROW) {
		if (table.HasGeneratedColumns()) {
			throw NotImplementedException(
			    "FOR EACH ROW triggers on tables with generated columns are not yet supported");
		}
		if (create_trigger_info.trigger_action->type == QueryNodeType::UPDATE_QUERY_NODE) {
			throw NotImplementedException("UPDATE trigger bodies in FOR EACH ROW triggers are not yet supported");
		}
		vector<Identifier> col_names;
		vector<LogicalType> col_types;
		for (auto &col : table.GetColumns().Physical()) {
			col_names.push_back(col.GetName());
			col_types.push_back(col.GetType());
		}
		auto new_idx = validation_binder->GenerateTableIndex();
		auto scope_name = Binder::RowScopeName(create_trigger_info.event_type);
		row_scope_binder = validation_binder->SetupRowScope(new_idx, col_names, col_types, scope_name);
	}
	if (row_scope_binder) {
		auto body_binder = Binder::CreateBinder(context, validation_binder.get());
		auto bound_body = body_binder->Bind(*body_copy);
		validation_binder->PopScope();
		if (body_binder->correlated_columns.empty()) {
			throw BinderException("FOR EACH ROW trigger %s on table %s must reference at least one NEW or OLD "
			                      "column in the trigger body (use FOR EACH STATEMENT if row data is not needed)",
			                      create_trigger_info.GetTriggerName(), table.name);
		}
		if (BoundBodyContainsTrigger(*bound_body.plan)) {
			throw NotImplementedException(
			    "FOR EACH ROW trigger %s on table %s writes to a table that has its own FOR EACH ROW "
			    "trigger (cascading row triggers are not yet supported)",
			    create_trigger_info.GetTriggerName(), table.name);
		}
	} else {
		validation_binder->Bind(*body_copy);
	}

	return schema;
}

unique_ptr<LogicalOperator> DuckCatalog::BindCreateIndex(Binder &binder, CreateStatement &stmt,
                                                         TableCatalogEntry &table, unique_ptr<LogicalOperator> plan) {
	D_ASSERT(plan->type == LogicalOperatorType::LOGICAL_GET);
	auto create_index_info = unique_ptr_cast<CreateInfo, CreateIndexInfo>(std::move(stmt.info));
	IndexBinder index_binder(binder, binder.context);
	return index_binder.BindCreateIndex(binder.context, std::move(create_index_info), table, std::move(plan), nullptr);
}

BoundStatement Binder::Bind(CreateStatement &stmt) {
	BoundStatement result;
	result.names = {"Count"};
	result.types = {LogicalType::BIGINT};

	auto catalog_type = stmt.info->type;
	auto return_type = StatementReturnType::NOTHING;
	auto &properties = GetStatementProperties();
	switch (catalog_type) {
	case CatalogType::SCHEMA_ENTRY: {
		auto &info = stmt.info->Cast<CreateSchemaInfo>();
		BindCreateSchema(info);
		properties.RegisterDBModify(Catalog::GetCatalog(context, info.SchemaCatalog()), context,
		                            DatabaseModificationType::CREATE_CATALOG_ENTRY);
		result.plan = make_uniq<LogicalCreate>(LogicalOperatorType::LOGICAL_CREATE_SCHEMA, std::move(stmt.info));
		break;
	}
	case CatalogType::VIEW_ENTRY: {
		auto &base = stmt.info->Cast<CreateViewInfo>();
		// bind the schema
		auto &schema = BindCreateSchema(*stmt.info);
		if (base.security_type == ViewSecurityType::SECURE_VIEW) {
			// secure views cannot be persisted in older storage formats - block their creation instead of silently
			// turning them into regular views on the next checkpoint
			auto &attached = schema.ParentCatalog().GetAttached();
			if (!base.temporary && !attached.IsTemporary() && attached.HasStorageManager()) {
				auto &storage_manager = attached.GetStorageManager();
				if (!storage_manager.InMemory() && storage_manager.GetStorageVersion() < StorageVersion::V2_0_0) {
					throw BinderException("CREATE SECURE VIEW is only supported for storage versions v2.0.0 and "
					                      "higher.\nUse an in-memory database, or ATTACH with (STORAGE_VERSION "
					                      "v2.0.0)");
				}
			}
		}
		if (stmt.info->on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT) {
			CatalogTransaction transaction(schema.ParentCatalog(), context);
			auto existing_entry = schema.GetEntry(transaction, CatalogType::VIEW_ENTRY, base.GetViewName());
			if (existing_entry && existing_entry->type == CatalogType::VIEW_ENTRY) {
				// IF EXISTS and the view already exists - avoid binding
				base.binding_mode = CreateViewBindingMode::SKIP_BINDING;
			}
		}
		BindCreateViewInfo(base);
		result.plan = make_uniq<LogicalCreate>(LogicalOperatorType::LOGICAL_CREATE_VIEW, std::move(stmt.info), &schema);
		break;
	}
	case CatalogType::SEQUENCE_ENTRY: {
		auto &schema = BindCreateSchema(*stmt.info);
		result.plan =
		    make_uniq<LogicalCreate>(LogicalOperatorType::LOGICAL_CREATE_SEQUENCE, std::move(stmt.info), &schema);
		break;
	}
	case CatalogType::TABLE_MACRO_ENTRY: {
		auto &schema = BindCreateFunctionInfo(*stmt.info);
		result.plan =
		    make_uniq<LogicalCreate>(LogicalOperatorType::LOGICAL_CREATE_MACRO, std::move(stmt.info), &schema);
		break;
	}
	case CatalogType::MACRO_ENTRY: {
		auto &schema = BindCreateFunctionInfo(*stmt.info);
		auto logical_create =
		    make_uniq<LogicalCreate>(LogicalOperatorType::LOGICAL_CREATE_MACRO, std::move(stmt.info), &schema);
		result.plan = std::move(logical_create);
		break;
	}
	case CatalogType::INDEX_ENTRY: {
		auto &create_index_info = stmt.info->Cast<CreateIndexInfo>();
		if (create_index_info.temporary) {
			// unsupported - an index is temporary iff the table it is created on is (see below)
			throw BinderException("Temporary indexes are not supported");
		}

		// Plan the table scan - the table lives in the same (possibly nested) schema as the index.
		TableDescription table_description(create_index_info.GetQualifiedName().WithName(create_index_info.table));
		auto table_ref = make_uniq<BaseTableRef>(table_description);
		auto bound_table = Bind(*table_ref);
		auto plan = std::move(bound_table.plan);
		if (plan->type != LogicalOperatorType::LOGICAL_GET) {
			throw BinderException("can only create an index on a base table");
		}
		auto &get = plan->Cast<LogicalGet>();
		auto table_ptr = get.GetTable();
		if (!table_ptr) {
			throw BinderException("can only create an index on a base table");
		}

		auto &table = *table_ptr;
		if (table.temporary) {
			stmt.info->temporary = true;
		}
		properties.RegisterDBModify(table.catalog, context, DatabaseModificationType::CREATE_INDEX);
		result.plan = table.catalog.BindCreateIndex(*this, stmt, table, std::move(plan));
		break;
	}
	case CatalogType::TABLE_ENTRY: {
		// Skip real catalog/schema resolution in EXTRACT_NAMES or EXTRACT_QUALIFIED_NAMES mode
		if (GetBindingMode() == BindingMode::EXTRACT_NAMES ||
		    GetBindingMode() == BindingMode::EXTRACT_QUALIFIED_NAMES) {
			auto &create_info = stmt.info->Cast<CreateTableInfo>();
			if (GetBindingMode() == BindingMode::EXTRACT_QUALIFIED_NAMES) {
				AddTableName(create_info.GetQualifiedName().ToString());
			} else {
				AddTableName(create_info.GetQualifiedName().Name().GetIdentifierName());
			}
			if (create_info.query) {
				auto query_obj = Bind(*create_info.query);
				result.plan = std::move(query_obj.plan);
			}
			break;
		}
		auto bound_info = BindCreateTableInfo(std::move(stmt.info));
		auto root = std::move(bound_info->query);

		// create the logical operator
		auto &schema = bound_info->schema;
		auto create_table = make_uniq<LogicalCreateTable>(schema, std::move(bound_info));
		if (root) {
			// CREATE TABLE AS
			return_type = StatementReturnType::CHANGED_ROWS;
			create_table->children.push_back(std::move(root));
		}
		result.plan = std::move(create_table);
		break;
	}
	case CatalogType::TYPE_ENTRY: {
		auto &schema = BindCreateSchema(*stmt.info);
		auto &create_type_info = stmt.info->Cast<CreateTypeInfo>();
		result.plan = make_uniq<LogicalCreate>(LogicalOperatorType::LOGICAL_CREATE_TYPE, std::move(stmt.info), &schema);

		auto &catalog = Catalog::GetCatalog(context, create_type_info.GetQualifiedName().Catalog());
		auto &dependencies = create_type_info.dependencies;
		auto dependency_callback = [&dependencies, &catalog](CatalogEntry &entry) {
			if (&catalog != &entry.ParentCatalog()) {
				// Don't register any cross-catalog dependencies
				return;
			}
			dependencies.AddDependency(entry);
		};
		if (create_type_info.query) {
			// CREATE TYPE mood AS ENUM (SELECT 'happy')
			auto query_obj = Bind(*create_type_info.query);
			auto query = std::move(query_obj.plan);
			create_type_info.query.reset();

			auto &sql_types = query_obj.types;
			if (sql_types.size() != 1) {
				// add cast expression?
				throw BinderException("The query must return a single column");
			}
			if (sql_types[0].id() != LogicalType::VARCHAR) {
				// push a projection casting to varchar
				vector<unique_ptr<Expression>> select_list;
				auto ref = make_uniq<BoundColumnRefExpression>(sql_types[0], query->GetColumnBindings()[0]);
				auto cast_expr = BoundCastExpression::AddCastToType(context, std::move(ref), LogicalType::VARCHAR);
				select_list.push_back(std::move(cast_expr));
				auto proj = make_uniq<LogicalProjection>(GenerateTableIndex(), std::move(select_list));
				proj->AddChild(std::move(query));
				query = std::move(proj);
			}

			result.plan->AddChild(std::move(query));
		} else {
			SetCatalogLookupCallback(dependency_callback);
			// Bind the underlying type
			BindLogicalType(create_type_info.type);
		}
		break;
	}
	case CatalogType::SECRET_ENTRY: {
		CatalogTransaction transaction = CatalogTransaction(Catalog::GetSystemCatalog(context), context);
		return_type = StatementReturnType::QUERY_RESULT;

		auto &info = stmt.info->Cast<CreateSecretInfo>();

		// We need to execute all expressions in the CreateSecretInfo to construct a CreateSecretInput
		ConstantBinder default_binder(*this, context, "Secret Parameter");

		string provider_string, type_string;
		vector<string> scope_strings;

		if (info.provider) {
			auto bound_provider = default_binder.Bind(info.provider);
			if (bound_provider->HasParameter()) {
				throw InvalidInputException("Create Secret expressions can not have parameters!");
			}
			provider_string =
			    StringUtil::Lower(ExpressionExecutor::EvaluateScalar(context, *bound_provider, true).ToString());
		}
		if (info.type) {
			auto bound_type = default_binder.Bind(info.type);
			if (bound_type->HasParameter()) {
				throw InvalidInputException("Create Secret expressions can not have parameters!");
			}
			type_string = StringUtil::Lower(ExpressionExecutor::EvaluateScalar(context, *bound_type, true).ToString());
		}
		if (info.scope) {
			auto bound_scope = default_binder.Bind(info.scope);
			if (bound_scope->HasParameter()) {
				throw InvalidInputException("Create Secret expressions can not have parameters!");
			}
			// Execute all scope expressions
			Value scope = ExpressionExecutor::EvaluateScalar(context, *bound_scope, true);
			if (scope.type() == LogicalType::VARCHAR) {
				scope_strings.push_back(scope.ToString());
			} else if (scope.type() == LogicalType::LIST(LogicalType::VARCHAR)) {
				for (const auto &item : ListValue::GetChildren(scope)) {
					scope_strings.push_back(item.GetValue<string>());
				}
			} else if (scope.type().InternalType() == PhysicalType::STRUCT) {
				// struct expression with empty keys is also allowed for backwards compatibility to when the create
				// secret statement would be parsed differently: this allows CREATE SECRET (TYPE x, SCOPE ('bla',
				// 'bloe'))
				for (const auto &child : StructValue::GetChildren(scope)) {
					if (child.type() != LogicalType::VARCHAR) {
						throw InvalidInputException(
						    "Invalid input to scope parameter of create secret: only struct of VARCHARs is allowed");
					}
					scope_strings.push_back(child.GetValue<string>());
				}
			} else {
				throw InvalidInputException("Create Secret scope must be of type VARCHAR or LIST(VARCHAR)");
			}
		}

		// Execute all options expressions
		case_insensitive_map_t<Value> bound_options;
		for (auto &option : info.options) {
			auto bound_value = default_binder.Bind(option.second);
			if (bound_value->HasParameter()) {
				throw InvalidInputException("Create Secret expressions can not have parameters!");
			}
			bound_options.insert({option.first, ExpressionExecutor::EvaluateScalar(context, *bound_value, true)});
		}

		CreateSecretInput create_secret_input {Identifier(type_string),
		                                       Identifier(provider_string),
		                                       Identifier(info.storage_type),
		                                       info.GetSecretName(),
		                                       scope_strings,
		                                       bound_options,
		                                       info.on_conflict,
		                                       info.persist_type};

		result = SecretManager::Get(context).BindCreateSecret(transaction, create_secret_input);
		break;
	}
	case CatalogType::TRIGGER_ENTRY: {
		auto &create_trigger_info = stmt.info->Cast<CreateTriggerInfo>();
		auto &schema = BindCreateTriggerInfo(create_trigger_info);
		result.plan =
		    make_uniq<LogicalCreate>(LogicalOperatorType::LOGICAL_CREATE_TRIGGER, std::move(stmt.info), &schema);
		break;
	}
	default:
		throw InternalException("Unrecognized type!");
	}

	properties.return_type = return_type;

	return result;
}

} // namespace duckdb
