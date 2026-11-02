#include "duckdb/common/exception/parser_exception.hpp"
#include "duckdb/catalog/catalog.hpp"

#include "duckdb/catalog/catalog_search_path.hpp"
#include "duckdb/catalog/catalog_entry/list.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/duck_schema_entry.hpp"
#include "duckdb/catalog/catalog_set.hpp"
#include "duckdb/catalog/default/default_schemas.hpp"
#include "duckdb/catalog/catalog_entry/type_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/parser/parsed_data/alter_schema_info.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/parsed_data/create_aggregate_function_info.hpp"
#include "duckdb/parser/parsed_data/create_collation_info.hpp"
#include "duckdb/parser/parsed_data/create_coordinate_system_info.hpp"
#include "duckdb/parser/parsed_data/create_copy_function_info.hpp"
#include "duckdb/parser/parsed_data/create_index_info.hpp"
#include "duckdb/parser/parsed_data/create_pragma_function_info.hpp"
#include "duckdb/parser/parsed_data/create_secret_info.hpp"
#include "duckdb/parser/sql_statement.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_sequence_info.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/parser/parsed_data/create_type_info.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression_binder/index_binder.hpp"
#include "duckdb/catalog/default/default_types.hpp"
#include "duckdb/main/extension_entries.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/function/built_in_functions.hpp"
#include "duckdb/catalog/similar_catalog_entry.hpp"
#include "duckdb/catalog/schema_scan_filters.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/storage/database_size.hpp"
#include "duckdb/main/settings.hpp"
#include <algorithm>

namespace duckdb {

Catalog::Catalog(AttachedDatabase &db) : db(db) {
}

Catalog::~Catalog() {
}

DatabaseInstance &Catalog::GetDatabase() {
	return db.GetDatabase();
}

AttachedDatabase &Catalog::GetAttached() {
	return db;
}

const AttachedDatabase &Catalog::GetAttached() const {
	return db;
}

const Identifier &Catalog::GetName() const {
	return GetAttached().GetName();
}

idx_t Catalog::GetOid() {
	return GetAttached().oid;
}

Catalog &Catalog::GetSystemCatalog(ClientContext &context) {
	return Catalog::GetSystemCatalog(*context.db);
}

//! The default catalog for lookups; empty when no database is attached, in which case the lookup finds nothing there.
Identifier GetDefaultCatalog(CatalogEntryRetriever &retriever) {
	return DatabaseManager::TryGetDefaultDatabase(retriever.GetContext());
}

optional_ptr<Catalog> Catalog::GetCatalogEntry(CatalogEntryRetriever &retriever, const Identifier &catalog_name) {
	auto &context = retriever.GetContext();
	auto &db_manager = DatabaseManager::Get(context);
	if (catalog_name == TEMP_CATALOG) {
		return &ClientData::Get(context).temporary_objects->GetCatalog();
	}
	if (catalog_name == SYSTEM_CATALOG) {
		return &GetSystemCatalog(context);
	}
	auto entry =
	    db_manager.GetDatabase(context, IsInvalidCatalog(catalog_name) ? GetDefaultCatalog(retriever) : catalog_name);
	if (!entry) {
		return nullptr;
	}
	return &entry->GetCatalog();
}

optional_ptr<Catalog> Catalog::GetCatalogEntry(ClientContext &context, const Identifier &catalog_name) {
	CatalogEntryRetriever entry_retriever(context);
	return GetCatalogEntry(entry_retriever, catalog_name);
}

Catalog &Catalog::GetCatalog(CatalogEntryRetriever &retriever, const Identifier &catalog_name) {
	auto catalog = Catalog::GetCatalogEntry(retriever, catalog_name);
	if (!catalog) {
		throw BinderException("Catalog %s does not exist!", catalog_name);
	}
	return *catalog;
}

Catalog &Catalog::GetCatalog(ClientContext &context, const Identifier &catalog_name) {
	CatalogEntryRetriever entry_retriever(context);
	return GetCatalog(entry_retriever, catalog_name);
}

//===--------------------------------------------------------------------===//
// Schema
//===--------------------------------------------------------------------===//
optional_ptr<CatalogEntry> Catalog::CreateSchema(ClientContext &context, CreateSchemaInfo &info) {
	auto supports_create_schema = SupportsCreateSchema(info);
	if (supports_create_schema.HasError()) {
		supports_create_schema.Throw();
	}
	return CreateSchema(GetCatalogTransaction(context), info);
}

CatalogTransaction Catalog::GetCatalogTransaction(ClientContext &context) {
	return CatalogTransaction(*this, context);
}

SchemaCatalogEntry &Catalog::GetEntrySchema(CatalogTransaction transaction, const QualifiedName &name) {
	auto &path = name.Path();
	if (path.size() <= 3) {
		return GetSchema(transaction, name.Schema());
	}
	// nested entry ([catalog, schema_path..., name]): navigate the (nested) schema path
	vector<Identifier> schema_path(path.begin() + 1, path.end() - 1);
	return *GetSchema(transaction, schema_path, OnEntryNotFound::THROW_EXCEPTION);
}

//===--------------------------------------------------------------------===//
// Table
//===--------------------------------------------------------------------===//
optional_ptr<CatalogEntry> Catalog::CreateTable(ClientContext &context, BoundCreateTableInfo &info) {
	return CreateTable(GetCatalogTransaction(context), info);
}

optional_ptr<CatalogEntry> Catalog::CreateTable(ClientContext &context, unique_ptr<CreateTableInfo> info) {
	auto binder = Binder::CreateBinder(context);
	auto bound_info = binder->BindCreateTableInfo(std::move(info));
	return CreateTable(context, *bound_info);
}

optional_ptr<CatalogEntry> Catalog::CreateTable(CatalogTransaction transaction, SchemaCatalogEntry &schema,
                                                BoundCreateTableInfo &info) {
	auto supports_create_table = SupportsCreateTable(info);
	if (supports_create_table.HasError()) {
		supports_create_table.Throw();
	}
	return schema.CreateTable(transaction, info);
}

optional_ptr<CatalogEntry> Catalog::CreateTable(CatalogTransaction transaction, BoundCreateTableInfo &info) {
	auto &schema = GetEntrySchema(transaction, info.base->GetQualifiedName());
	return CreateTable(transaction, schema, info);
}

//===--------------------------------------------------------------------===//
// View
//===--------------------------------------------------------------------===//
optional_ptr<CatalogEntry> Catalog::CreateView(CatalogTransaction transaction, CreateViewInfo &info) {
	auto &schema = GetEntrySchema(transaction, info.GetQualifiedName());
	return CreateView(transaction, schema, info);
}

optional_ptr<CatalogEntry> Catalog::CreateView(ClientContext &context, CreateViewInfo &info) {
	return CreateView(GetCatalogTransaction(context), info);
}

optional_ptr<CatalogEntry> Catalog::CreateView(CatalogTransaction transaction, SchemaCatalogEntry &schema,
                                               CreateViewInfo &info) {
	return schema.CreateView(transaction, info);
}

//===--------------------------------------------------------------------===//
// Sequence
//===--------------------------------------------------------------------===//
optional_ptr<CatalogEntry> Catalog::CreateSequence(CatalogTransaction transaction, CreateSequenceInfo &info) {
	auto &schema = GetEntrySchema(transaction, info.GetQualifiedName());
	return CreateSequence(transaction, schema, info);
}

optional_ptr<CatalogEntry> Catalog::CreateSequence(ClientContext &context, CreateSequenceInfo &info) {
	return CreateSequence(GetCatalogTransaction(context), info);
}

optional_ptr<CatalogEntry> Catalog::CreateSequence(CatalogTransaction transaction, SchemaCatalogEntry &schema,
                                                   CreateSequenceInfo &info) {
	return schema.CreateSequence(transaction, info);
}

//===--------------------------------------------------------------------===//
// Type
//===--------------------------------------------------------------------===//
optional_ptr<CatalogEntry> Catalog::CreateType(CatalogTransaction transaction, CreateTypeInfo &info) {
	auto &schema = GetEntrySchema(transaction, info.GetQualifiedName());
	return CreateType(transaction, schema, info);
}

optional_ptr<CatalogEntry> Catalog::CreateType(ClientContext &context, CreateTypeInfo &info) {
	return CreateType(GetCatalogTransaction(context), info);
}

optional_ptr<CatalogEntry> Catalog::CreateType(CatalogTransaction transaction, SchemaCatalogEntry &schema,
                                               CreateTypeInfo &info) {
	return schema.CreateType(transaction, info);
}

//===--------------------------------------------------------------------===//
// Table Function
//===--------------------------------------------------------------------===//
optional_ptr<CatalogEntry> Catalog::CreateTableFunction(CatalogTransaction transaction, CreateTableFunctionInfo &info) {
	auto &schema = GetEntrySchema(transaction, info.GetQualifiedName());
	return CreateTableFunction(transaction, schema, info);
}

optional_ptr<CatalogEntry> Catalog::CreateTableFunction(ClientContext &context, CreateTableFunctionInfo &info) {
	return CreateTableFunction(GetCatalogTransaction(context), info);
}

optional_ptr<CatalogEntry> Catalog::CreateTableFunction(CatalogTransaction transaction, SchemaCatalogEntry &schema,
                                                        CreateTableFunctionInfo &info) {
	return schema.CreateTableFunction(transaction, info);
}

optional_ptr<CatalogEntry> Catalog::CreateTableFunction(ClientContext &context,
                                                        optional_ptr<CreateTableFunctionInfo> info) {
	return CreateTableFunction(context, *info);
}

//===--------------------------------------------------------------------===//
// Copy Function
//===--------------------------------------------------------------------===//
optional_ptr<CatalogEntry> Catalog::CreateCopyFunction(CatalogTransaction transaction, CreateCopyFunctionInfo &info) {
	auto &schema = GetEntrySchema(transaction, info.GetQualifiedName());
	return CreateCopyFunction(transaction, schema, info);
}

optional_ptr<CatalogEntry> Catalog::CreateCopyFunction(ClientContext &context, CreateCopyFunctionInfo &info) {
	return CreateCopyFunction(GetCatalogTransaction(context), info);
}

optional_ptr<CatalogEntry> Catalog::CreateCopyFunction(CatalogTransaction transaction, SchemaCatalogEntry &schema,
                                                       CreateCopyFunctionInfo &info) {
	return schema.CreateCopyFunction(transaction, info);
}

//===--------------------------------------------------------------------===//
// Pragma Function
//===--------------------------------------------------------------------===//
optional_ptr<CatalogEntry> Catalog::CreatePragmaFunction(CatalogTransaction transaction,
                                                         CreatePragmaFunctionInfo &info) {
	auto &schema = GetEntrySchema(transaction, info.GetQualifiedName());
	return CreatePragmaFunction(transaction, schema, info);
}

optional_ptr<CatalogEntry> Catalog::CreatePragmaFunction(ClientContext &context, CreatePragmaFunctionInfo &info) {
	return CreatePragmaFunction(GetCatalogTransaction(context), info);
}

optional_ptr<CatalogEntry> Catalog::CreatePragmaFunction(CatalogTransaction transaction, SchemaCatalogEntry &schema,
                                                         CreatePragmaFunctionInfo &info) {
	return schema.CreatePragmaFunction(transaction, info);
}

//===--------------------------------------------------------------------===//
// Function
//===--------------------------------------------------------------------===//
optional_ptr<CatalogEntry> Catalog::CreateFunction(CatalogTransaction transaction, CreateFunctionInfo &info) {
	auto &schema = GetEntrySchema(transaction, info.GetQualifiedName());
	return CreateFunction(transaction, schema, info);
}

optional_ptr<CatalogEntry> Catalog::CreateFunction(ClientContext &context, CreateFunctionInfo &info) {
	return CreateFunction(GetCatalogTransaction(context), info);
}

optional_ptr<CatalogEntry> Catalog::CreateFunction(CatalogTransaction transaction, SchemaCatalogEntry &schema,
                                                   CreateFunctionInfo &info) {
	return schema.CreateFunction(transaction, info);
}

optional_ptr<CatalogEntry> Catalog::AddFunction(ClientContext &context, CreateFunctionInfo &info) {
	info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
	return CreateFunction(context, info);
}

//===--------------------------------------------------------------------===//
// Collation
//===--------------------------------------------------------------------===//
optional_ptr<CatalogEntry> Catalog::CreateCollation(CatalogTransaction transaction, CreateCollationInfo &info) {
	auto &schema = GetEntrySchema(transaction, info.GetQualifiedName());
	return CreateCollation(transaction, schema, info);
}

optional_ptr<CatalogEntry> Catalog::CreateCollation(ClientContext &context, CreateCollationInfo &info) {
	return CreateCollation(GetCatalogTransaction(context), info);
}

optional_ptr<CatalogEntry> Catalog::CreateCollation(CatalogTransaction transaction, SchemaCatalogEntry &schema,
                                                    CreateCollationInfo &info) {
	return schema.CreateCollation(transaction, info);
}

//===--------------------------------------------------------------------===//
// Coordinate System
//===--------------------------------------------------------------------===//
optional_ptr<CatalogEntry> Catalog::CreateCoordinateSystem(CatalogTransaction transaction,
                                                           CreateCoordinateSystemInfo &info) {
	auto &schema = GetEntrySchema(transaction, info.GetQualifiedName());
	return CreateCoordinateSystem(transaction, schema, info);
}

optional_ptr<CatalogEntry> Catalog::CreateCoordinateSystem(ClientContext &context, CreateCoordinateSystemInfo &info) {
	return CreateCoordinateSystem(GetCatalogTransaction(context), info);
}

optional_ptr<CatalogEntry> Catalog::CreateCoordinateSystem(CatalogTransaction transaction, SchemaCatalogEntry &schema,
                                                           CreateCoordinateSystemInfo &info) {
	return schema.CreateCoordinateSystem(transaction, info);
}

//===--------------------------------------------------------------------===//
// Index
//===--------------------------------------------------------------------===//
optional_ptr<CatalogEntry> Catalog::CreateIndex(CatalogTransaction transaction, CreateIndexInfo &info) {
	auto &schema = GetEntrySchema(transaction, info.GetQualifiedName());
	auto &table = schema.GetEntry(transaction, CatalogType::TABLE_ENTRY, info.table)->Cast<TableCatalogEntry>();
	return schema.CreateIndex(transaction, info, table);
}

optional_ptr<CatalogEntry> Catalog::CreateIndex(ClientContext &context, CreateIndexInfo &info) {
	return CreateIndex(GetCatalogTransaction(context), info);
}

unique_ptr<LogicalOperator> Catalog::BindCreateIndex(Binder &binder, CreateStatement &stmt, TableCatalogEntry &table,
                                                     unique_ptr<LogicalOperator> plan) {
	D_ASSERT(plan->type == LogicalOperatorType::LOGICAL_GET);
	auto create_index_info = unique_ptr_cast<CreateInfo, CreateIndexInfo>(std::move(stmt.info));
	IndexBinder index_binder(binder, binder.context);
	return index_binder.BindCreateIndex(binder.context, std::move(create_index_info), table, std::move(plan), nullptr);
}

unique_ptr<LogicalOperator> Catalog::BindAlterAddIndex(Binder &binder, TableCatalogEntry &table_entry,
                                                       unique_ptr<LogicalOperator> plan,
                                                       unique_ptr<CreateIndexInfo> create_info,
                                                       unique_ptr<AlterTableInfo> alter_info) {
	throw NotImplementedException("BindAlterAddIndex not supported by this catalog");
}

unique_ptr<TableRef> Catalog::RemoteExecute(ClientContext &context, unique_ptr<QueryNode> node) {
	throw NotImplementedException("RemoteExecute(QueryNode) not supported by this catalog");
}

unique_ptr<TableRef> Catalog::RemoteExecute(ClientContext &context, unique_ptr<SQLStatement> statement) {
	throw NotImplementedException("RemoteExecute(SQLStatement) not supported by this catalog");
}

unique_ptr<TableRef> Catalog::RemoteExecute(ClientContext &context, const string &sql) {
	throw NotImplementedException("RemoteExecute(string) not supported by this catalog");
}

bool Catalog::SupportsPushdown(const ParsedExpression &expression) {
	return true;
}

bool Catalog::SupportsPushdown(const TableRef &ref) {
	return true;
}

bool Catalog::SupportsPushdown(const QueryNode &node) {
	return true;
}

bool Catalog::SupportsPushdown(const SQLStatement &statement) {
	return true;
}

string Catalog::GetConnectDisplay() {
	auto &attached = GetAttached();
	// Ephemeral catalogs (from `CONNECT '<uri>'`) carry a generated internal name; show the URI instead.
	if (attached.IsEphemeral()) {
		auto path = attached.StoredPath();
		if (!path.empty()) {
			return path;
		}
	}
	return attached.GetName().GetIdentifierName();
}

//===--------------------------------------------------------------------===//
// Lookup Structures
//===--------------------------------------------------------------------===//
struct CatalogLookup {
	CatalogLookup(Catalog &catalog, CatalogType catalog_type, Identifier schema_p, Identifier name_p)
	    : catalog(catalog),
	      lookup_info(catalog_type, QualifiedName(catalog.GetName(), std::move(schema_p), std::move(name_p))) {
	}
	//! Look the entry up in the given catalog, keeping the lookup's qualification as-is. Used for nested schema
	//! paths, which cannot be expressed as a single (catalog, schema) pair.
	CatalogLookup(Catalog &catalog, const EntryLookupInfo &lookup_p) : catalog(catalog), lookup_info(lookup_p) {
	}
	//! Look the entry up in the schema of a search path entry, which can be nested
	CatalogLookup(Catalog &catalog, const CatalogSearchEntry &entry, const EntryLookupInfo &lookup_p)
	    : catalog(catalog),
	      lookup_info(lookup_p, QualifiedName::FromCatalogSchema(catalog.GetName(), entry.GetSchemaPath(),
	                                                             lookup_p.GetEntryIdentifier())) {
	}

	Catalog &catalog;
	//! The lookup, with its catalog/schema qualification set to the resolved (catalog, schema) pair
	EntryLookupInfo lookup_info;
};

//===--------------------------------------------------------------------===//
// Generic
//===--------------------------------------------------------------------===//
void Catalog::DropEntry(ClientContext &context, DropInfo &info) {
	if (info.type == CatalogType::SCHEMA_ENTRY) {
		// DROP SCHEMA
		DropSchema(context, info);
		return;
	}

	CatalogEntryRetriever retriever(context);
	EntryLookupInfo lookup_info(info.type, info.GetQualifiedName());
	auto lookup = LookupEntry(retriever, lookup_info, info.if_not_found);
	if (!lookup.Found()) {
		return;
	}

	lookup.schema->DropEntry(context, info);
}

SchemaCatalogEntry &Catalog::GetSchema(ClientContext &context, const Identifier &schema) {
	return GetSchema(GetCatalogTransaction(context), schema);
}

SchemaCatalogEntry &Catalog::GetSchema(CatalogTransaction transaction, const Identifier &schema) {
	EntryLookupInfo schema_lookup(CatalogType::SCHEMA_ENTRY, QualifiedName(schema));
	return GetSchema(transaction, schema_lookup);
}

SchemaCatalogEntry &Catalog::GetSchema(ClientContext &context, const EntryLookupInfo &schema_lookup) {
	return *Catalog::GetSchema(context, schema_lookup, OnEntryNotFound::THROW_EXCEPTION);
}

SchemaCatalogEntry &Catalog::GetSchema(ClientContext &context, const Identifier &catalog_name,
                                       const Identifier &schema) {
	return *GetSchema(context, catalog_name, schema, OnEntryNotFound::THROW_EXCEPTION);
}

optional_ptr<SchemaCatalogEntry> Catalog::GetSchema(ClientContext &context, const Identifier &schema,
                                                    OnEntryNotFound if_not_found) {
	return GetSchema(GetCatalogTransaction(context), schema, if_not_found);
}

optional_ptr<SchemaCatalogEntry> Catalog::GetSchema(CatalogTransaction transaction, const Identifier &schema,
                                                    OnEntryNotFound if_not_found) {
	EntryLookupInfo schema_lookup(CatalogType::SCHEMA_ENTRY, QualifiedName(schema));
	return LookupSchema(transaction, schema_lookup, if_not_found);
}

optional_ptr<SchemaCatalogEntry> Catalog::GetSchema(ClientContext &context, const EntryLookupInfo &schema_lookup,
                                                    OnEntryNotFound if_not_found) {
	return LookupSchema(GetCatalogTransaction(context), schema_lookup, if_not_found);
}

optional_ptr<SchemaCatalogEntry> Catalog::GetSchema(ClientContext &context, const Identifier &catalog_name,
                                                    const Identifier &schema, OnEntryNotFound if_not_found) {
	CatalogEntryRetriever retriever(context);
	EntryLookupInfo schema_lookup(CatalogType::SCHEMA_ENTRY, QualifiedName(catalog_name, Identifier(), schema));
	return GetSchema(retriever, schema_lookup, if_not_found);
}

static bool TryParseSchemaPath(const string &schema_arg, vector<Identifier> &result) {
	try {
		result = QualifiedName::ParseComponents(schema_arg);
	} catch (ParserException &) {
		return false;
	}
	if (schema_arg.empty() || schema_arg.back() == '.') {
		result.emplace_back();
	}
	return true;
}

static string EmptyComponentError(const string &schema_arg) {
	return StringUtil::Format(
	    "Failed to parse schema path \"%s\" - schema paths cannot contain empty components. Schema names that "
	    "contain a '.' or a '\"' must be double-quoted (e.g. '\"my.schema\"')",
	    schema_arg);
}

struct SchemaPathCandidateList {
	vector<vector<Identifier>> candidates;
	string parse_error;
};

static SchemaPathCandidateList SchemaPathCandidates(Catalog &catalog, const string &schema_arg) {
	SchemaPathCandidateList result;
	vector<Identifier> literal_path;
	literal_path.emplace_back(schema_arg);
	vector<Identifier> nested_path;
	if (!catalog.SupportsNestedSchemas() || !TryParseSchemaPath(schema_arg, nested_path)) {
		result.candidates.push_back(std::move(literal_path));
		return result;
	}
	if (std::any_of(nested_path.begin(), nested_path.end(), [](const Identifier &part) { return part.empty(); })) {
		result.parse_error = EmptyComponentError(schema_arg);
		result.candidates.push_back(std::move(literal_path));
		return result;
	}
	bool is_literal = nested_path.size() == 1 && nested_path[0].GetIdentifierName() == schema_arg;
	result.candidates.push_back(std::move(nested_path));
	if (!is_literal) {
		result.candidates.push_back(std::move(literal_path));
	}
	return result;
}

QualifiedName Catalog::ResolveEntryName(ClientContext &context, const string &schema_arg, const string &entry_name,
                                        CatalogType entry_type, optional_ptr<BoundAtClause> at_clause) {
	Identifier name(entry_name);
	if (schema_arg.empty()) {
		return QualifiedName(GetName(), Identifier(), std::move(name));
	}
	auto candidates = SchemaPathCandidates(*this, schema_arg);
	if (candidates.candidates.size() > 1 || !candidates.parse_error.empty()) {
		for (auto &candidate : candidates.candidates) {
			auto qualified_name = QualifiedName::FromCatalogSchema(GetName(), candidate, name);
			EntryLookupInfo lookup(entry_type, qualified_name, at_clause, QueryErrorContext());
			CatalogEntryRetriever retriever(context);
			if (LookupEntry(retriever, lookup, OnEntryNotFound::RETURN_NULL).entry) {
				return qualified_name;
			}
		}
		if (!candidates.parse_error.empty()) {
			throw InvalidInputException("%s", candidates.parse_error);
		}
	}
	return QualifiedName::FromCatalogSchema(GetName(), candidates.candidates[0], name);
}

SchemaCatalogEntry &Catalog::ResolveSchema(ClientContext &context, const string &schema_arg) {
	auto candidates = SchemaPathCandidates(*this, schema_arg);
	if (candidates.candidates.size() > 1 || !candidates.parse_error.empty()) {
		for (auto &candidate : candidates.candidates) {
			auto entry = Catalog::GetSchema(context, GetName(), candidate, OnEntryNotFound::RETURN_NULL);
			if (entry) {
				return *entry;
			}
		}
		if (!candidates.parse_error.empty()) {
			throw InvalidInputException("%s", candidates.parse_error);
		}
	}
	return *Catalog::GetSchema(context, GetName(), candidates.candidates[0], OnEntryNotFound::THROW_EXCEPTION);
}

optional_ptr<SchemaCatalogEntry>
Catalog::LookupSchemaPath(CatalogTransaction transaction, const EntryLookupInfo &schema_lookup,
                          OnEntryNotFound if_not_found,
                          const std::function<optional_ptr<CatalogEntry>(const EntryLookupInfo &)> &lookup_root) {
	auto &path = schema_lookup.GetQualifiedName().Path();
	optional_ptr<SchemaCatalogEntry> schema;
	// Bound schema lookups lead with the catalog; bare lookups contain just the schema name.
	for (idx_t i = path.size() > 1 ? 1 : 0; i < path.size(); i++) {
		if (path[i].empty()) {
			continue;
		}
		EntryLookupInfo lookup(schema_lookup, QualifiedName(path[i]));
		auto entry = schema ? schema->LookupEntry(transaction, lookup) : lookup_root(lookup);
		if (!entry) {
			if (if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
				throw CatalogException::MissingEntry(lookup, string());
			}
			return nullptr;
		}
		schema = entry->Cast<SchemaCatalogEntry>();
	}
	return schema;
}

optional_ptr<SchemaCatalogEntry> Catalog::GetSchema(ClientContext &context, const Identifier &catalog_name,
                                                    const vector<Identifier> &schema_path,
                                                    OnEntryNotFound if_not_found) {
	D_ASSERT(!schema_path.empty());
	auto schema = GetSchema(context, catalog_name, schema_path[0], if_not_found);
	if (!schema || schema_path.size() == 1) {
		return schema;
	}
	auto &catalog = schema->catalog;
	auto path = schema_path;
	path.insert(path.begin(), catalog.GetName());
	EntryLookupInfo lookup(CatalogType::SCHEMA_ENTRY, QualifiedName::FromPath(std::move(path)));
	return catalog.LookupSchemaPath(
	    catalog.GetCatalogTransaction(context), lookup, if_not_found,
	    [&](const EntryLookupInfo &) -> optional_ptr<CatalogEntry> { return schema.get(); });
}

optional_ptr<SchemaCatalogEntry> Catalog::GetSchema(CatalogTransaction transaction,
                                                    const vector<Identifier> &schema_path,
                                                    OnEntryNotFound if_not_found) {
	D_ASSERT(!schema_path.empty());
	auto path = schema_path;
	path.insert(path.begin(), GetName());
	EntryLookupInfo lookup(CatalogType::SCHEMA_ENTRY, QualifiedName::FromPath(std::move(path)));
	return LookupSchemaPath(transaction, lookup, if_not_found,
	                        [&](const EntryLookupInfo &root) -> optional_ptr<CatalogEntry> {
		                        return LookupSchema(transaction, root, if_not_found).get();
	                        });
}

SchemaCatalogEntry &Catalog::GetSchema(ClientContext &context, const Identifier &catalog_name,
                                       const EntryLookupInfo &schema_lookup) {
	CatalogEntryRetriever retriever(context);
	auto result = GetSchema(
	    retriever,
	    EntryLookupInfo(schema_lookup, QualifiedName(catalog_name, Identifier(), schema_lookup.GetEntryIdentifier())),
	    OnEntryNotFound::THROW_EXCEPTION);
	return *result;
}

SchemaCatalogEntry &Catalog::GetSchema(CatalogTransaction transaction, const EntryLookupInfo &schema_lookup) {
	return *LookupSchema(transaction, schema_lookup, OnEntryNotFound::THROW_EXCEPTION);
}

bool Catalog::CheckAmbiguousCatalogOrSchema(ClientContext &context, const Identifier &schema) {
	if (Supports(RemoteCapability::IS_REMOTE)) {
		// skip this check for remote catalogs
		return false;
	}
	EntryLookupInfo schema_lookup(CatalogType::SCHEMA_ENTRY, QualifiedName(schema));
	return !!GetSchema(context, schema_lookup, OnEntryNotFound::RETURN_NULL);
}

//===--------------------------------------------------------------------===//
// Lookup
//===--------------------------------------------------------------------===//
vector<SimilarCatalogEntry> Catalog::SimilarEntriesInSchemas(ClientContext &context, const EntryLookupInfo &lookup_info,
                                                             const reference_set_t<SchemaCatalogEntry> &schemas) {
	vector<SimilarCatalogEntry> results;
	for (auto schema_ref : schemas) {
		auto &schema = schema_ref.get();
		auto transaction = schema.catalog.GetCatalogTransaction(context);
		auto entry = schema.GetSimilarEntry(transaction, lookup_info);
		if (!entry.Found()) {
			// no similar entry found
			continue;
		}
		if (results.empty() || results[0].score <= entry.score) {
			if (!results.empty() && results[0].score < entry.score) {
				results.clear();
			}

			results.push_back(entry);
			results.back().schema = &schema;
		}
	}
	return results;
}

vector<CatalogSearchEntry> GetCatalogEntries(CatalogEntryRetriever &retriever, const Identifier &catalog,
                                             const Identifier &schema, CatalogType lookup_type = CatalogType::INVALID) {
	auto &context = retriever.GetContext();
	vector<CatalogSearchEntry> entries;
	auto &search_path = retriever.GetSearchPath();
	if (IsInvalidCatalog(catalog) && IsInvalidSchema(schema)) {
		// no catalog or schema provided - scan the entire search path.
		if (lookup_type == CatalogType::INVALID || lookup_type == CatalogType::TABLE_ENTRY) {
			entries = search_path.Get();
		} else {
			// for non-table lookups, resolve implicit catalogs that requested precedence to their default schema in
			// place
			entries = search_path.GetWithPrecedenceSchemas(context);
		}
	} else if (IsInvalidCatalog(catalog)) {
		auto catalogs = search_path.GetCatalogsForSchema(schema);
		for (auto &catalog_name : catalogs) {
			entries.emplace_back(catalog_name, schema);
		}
		if (entries.empty()) {
			auto &default_entry = search_path.GetDefault();
			if (!IsInvalidCatalog(default_entry.GetCatalog())) {
				entries.emplace_back(default_entry.GetCatalog(), schema);
			} else if (auto default_database = DatabaseManager::TryGetDefaultDatabase(context);
			           !IsInvalidCatalog(default_database)) {
				entries.emplace_back(std::move(default_database), schema);
			}
		}
	} else if (IsInvalidSchema(schema)) {
		entries = search_path.GetEntriesForCatalog(catalog);
		if (entries.empty()) {
			auto catalog_entry = Catalog::GetCatalogEntry(context, catalog);
			if (!catalog_entry) {
				entries.emplace_back(catalog, DEFAULT_SCHEMA);
			} else {
				auto default_schema = catalog_entry->GetDefaultSchema();
				// a catalog without a default schema has no implicit schema to fall back on
				if (default_schema) {
					entries.emplace_back(catalog, *default_schema);
				}
			}
		}
	} else {
		// specific catalog and schema provided
		entries.emplace_back(catalog, schema);
	}
	return entries;
}

void FindMinimalQualification(CatalogEntryRetriever &retriever, const Identifier &catalog_name,
                              const Identifier &schema_name, bool &qualify_database, bool &qualify_schema) {
	// check if we can we qualify ONLY the schema
	bool found = false;
	auto entries = GetCatalogEntries(retriever, Identifier::InvalidCatalog(), schema_name);
	for (auto &entry : entries) {
		if (entry.GetCatalog() == catalog_name && entry.GetSchema() == schema_name) {
			found = true;
			break;
		}
	}
	if (found) {
		qualify_database = false;
		qualify_schema = true;
		return;
	}
	// check if we can qualify ONLY the catalog
	found = false;
	entries = GetCatalogEntries(retriever, catalog_name, Identifier::InvalidSchema());
	for (auto &entry : entries) {
		if (entry.GetCatalog() == catalog_name && entry.GetSchema() == schema_name && !entry.IsNested()) {
			found = true;
			break;
		}
	}
	if (found) {
		qualify_database = true;
		qualify_schema = false;
		return;
	}
	// need to qualify both catalog and schema
	qualify_database = true;
	qualify_schema = true;
}

bool Catalog::TryAutoLoad(ClientContext &context, const string &original_name) noexcept {
	string extension_name = ExtensionHelper::ApplyExtensionAlias(original_name);
	if (context.db->ExtensionIsLoaded(extension_name)) {
		return true;
	}
	if (!Settings::Get<AutoloadKnownExtensionsSetting>(context)) {
		return false;
	}
	try {
		if (ExtensionHelper::CanAutoloadExtension(*context.db, extension_name)) {
			return ExtensionHelper::TryAutoLoadExtension(context, extension_name);
		}
	} catch (...) {
		return false;
	}
	return false;
}

String Catalog::AutoloadExtensionByConfigName(ClientContext &context, const Identifier &configuration_name) {
	if (Settings::Get<AutoloadKnownExtensionsSetting>(context)) {
		auto extension_name = ExtensionHelper::FindExtensionInEntries(configuration_name, EXTENSION_SETTINGS);
		if (ExtensionHelper::CanAutoloadExtension(*context.db, extension_name)) {
			ExtensionHelper::AutoLoadExtension(context, extension_name);
			return extension_name;
		}
	}

	throw Catalog::UnrecognizedConfigurationError(context, configuration_name);
}

static bool IsAutoloadableFunction(CatalogType type) {
	return (type == CatalogType::TABLE_FUNCTION_ENTRY || type == CatalogType::SCALAR_FUNCTION_ENTRY ||
	        type == CatalogType::AGGREGATE_FUNCTION_ENTRY || type == CatalogType::PRAGMA_FUNCTION_ENTRY);
}

bool IsTableFunction(CatalogType type) {
	switch (type) {
	case CatalogType::TABLE_FUNCTION_ENTRY:
	case CatalogType::TABLE_MACRO_ENTRY:
	case CatalogType::PRAGMA_FUNCTION_ENTRY:
		return true;
	default:
		return false;
	}
}

bool IsScalarFunction(CatalogType type) {
	switch (type) {
	case CatalogType::SCALAR_FUNCTION_ENTRY:
	case CatalogType::AGGREGATE_FUNCTION_ENTRY:
	case CatalogType::MACRO_ENTRY:
		return true;
	default:
		return false;
	}
}

static bool CompareCatalogTypes(CatalogType type_a, CatalogType type_b) {
	if (type_a == type_b) {
		// Types are same
		return true;
	}
	if (IsScalarFunction(type_a) && IsScalarFunction(type_b)) {
		return true;
	}
	if (IsTableFunction(type_a) && IsTableFunction(type_b)) {
		return true;
	}
	return false;
}

bool Catalog::AutoLoadExtensionByCatalogEntry(DatabaseInstance &db, CatalogType type, const Identifier &entry_name) {
	if (Settings::Get<AutoloadKnownExtensionsSetting>(db)) {
		string extension_name;
		if (IsAutoloadableFunction(type)) {
			auto lookup_result = ExtensionHelper::FindExtensionInFunctionEntries(entry_name, EXTENSION_FUNCTIONS);
			if (lookup_result.empty()) {
				return false;
			}
			for (auto &function : lookup_result) {
				auto function_type = function.second;
				// FIXME: what if there are two functions with the same name, from different extensions?
				if (CompareCatalogTypes(type, function_type)) {
					extension_name = function.first;
					break;
				}
			}
		} else if (type == CatalogType::COPY_FUNCTION_ENTRY) {
			extension_name = ExtensionHelper::FindExtensionInEntries(entry_name, EXTENSION_COPY_FUNCTIONS);
		} else if (type == CatalogType::TYPE_ENTRY) {
			extension_name = ExtensionHelper::FindExtensionInEntries(entry_name, EXTENSION_TYPES);
		} else if (type == CatalogType::COLLATION_ENTRY) {
			extension_name = ExtensionHelper::FindExtensionInEntries(entry_name, EXTENSION_COLLATIONS);
		}

		if (!extension_name.empty() && ExtensionHelper::CanAutoloadExtension(db, extension_name)) {
			ExtensionHelper::AutoLoadExtension(db, extension_name);
			return true;
		}
	}

	return false;
}

CatalogException Catalog::UnrecognizedConfigurationError(ClientContext &context, const Identifier &name) {
	// check if the setting exists in any extensions
	auto extension_name = ExtensionHelper::FindExtensionInEntries(name, EXTENSION_SETTINGS);
	if (!extension_name.empty()) {
		auto error_message = StringUtil::Format(
		    "Setting with name %s is not in the catalog, but it exists in the %s extension.", name, extension_name);
		error_message = ExtensionHelper::AddExtensionInstallHintToErrorMsg(context, error_message, extension_name);
		return CatalogException(error_message);
	}
	// the setting is not in an extension
	// get a list of all options
	vector<Identifier> potential_names = DBConfig::GetOptionNames();
	for (auto &entry : DBConfig::GetConfig(context).GetExtensionSettings()) {
		potential_names.push_back(entry.first);
	}
	throw CatalogException::MissingEntry("configuration parameter", name, potential_names);
}

CatalogException Catalog::CreateMissingEntryException(CatalogEntryRetriever &retriever,
                                                      const EntryLookupInfo &lookup_info,
                                                      const reference_set_t<SchemaCatalogEntry> &schemas) {
	auto &context = retriever.GetContext();
	auto entries = SimilarEntriesInSchemas(context, lookup_info, schemas);
	auto max_schema_count = Settings::Get<CatalogErrorMaxSchemasSetting>(context);

	reference_set_t<SchemaCatalogEntry> unseen_schemas;
	auto &db_manager = DatabaseManager::Get(context);
	auto databases = db_manager.GetDatabases(context, max_schema_count);

	for (const auto &database : databases) {
		if (unseen_schemas.size() >= max_schema_count) {
			break;
		}
		if (database->GetVisibility() == AttachVisibility::HIDDEN) {
			continue;
		}
		auto &catalog = database->GetCatalog();
		auto current_schemas = catalog.GetSchemas(context);
		for (auto &current_schema : current_schemas) {
			if (unseen_schemas.size() >= max_schema_count) {
				break;
			}
			unseen_schemas.insert(current_schema.get());
		}
	}

	// Check if the entry exists in any extension.
	string extension_name;
	auto type = lookup_info.GetCatalogType();
	auto &entry_name = lookup_info.GetEntryIdentifier();
	if (type == CatalogType::TABLE_FUNCTION_ENTRY || type == CatalogType::SCALAR_FUNCTION_ENTRY ||
	    type == CatalogType::AGGREGATE_FUNCTION_ENTRY || type == CatalogType::PRAGMA_FUNCTION_ENTRY) {
		auto lookup_result = ExtensionHelper::FindExtensionInFunctionEntries(entry_name, EXTENSION_FUNCTIONS);
		do {
			if (lookup_result.empty()) {
				break;
			}
			vector<string> other_types;
			string extension_for_error;
			for (auto &function : lookup_result) {
				auto function_type = function.second;
				if (CompareCatalogTypes(type, function_type)) {
					extension_name = function.first;
					break;
				}
				extension_for_error = function.first;
				other_types.push_back(CatalogTypeToString(function_type));
			}
			if (!extension_name.empty()) {
				break;
			}
			if (other_types.size() == 1) {
				auto &function_type = other_types[0];
				auto error =
				    CatalogException("%s with name %s is not in the catalog, a function by this name exists "
				                     "in the %s extension, but it's of a different type, namely %s",
				                     CatalogTypeToString(type), entry_name, extension_for_error, function_type);
				return error;
			} else {
				D_ASSERT(!other_types.empty());
				auto list_of_types = StringUtil::Join(other_types, ", ");
				auto error =
				    CatalogException("%s with name %s is not in the catalog, functions with this name exist "
				                     "in the %s extension, but they are of different types, namely %s",
				                     CatalogTypeToString(type), entry_name, extension_for_error, list_of_types);
				return error;
			}
		} while (false);
	} else if (type == CatalogType::TYPE_ENTRY) {
		extension_name = ExtensionHelper::FindExtensionInEntries(entry_name, EXTENSION_TYPES);
	} else if (type == CatalogType::COPY_FUNCTION_ENTRY) {
		extension_name = ExtensionHelper::FindExtensionInEntries(entry_name, EXTENSION_COPY_FUNCTIONS);
	} else if (type == CatalogType::COLLATION_ENTRY) {
		extension_name = ExtensionHelper::FindExtensionInEntries(entry_name, EXTENSION_COLLATIONS);
	}

	// if we found an extension that can handle this catalog entry, create an error hinting the user
	if (!extension_name.empty()) {
		auto error_message =
		    StringUtil::Format("%s with name %s is not in the catalog, but it exists in the %s extension.",
		                       CatalogTypeToString(type), entry_name, extension_name);
		error_message = ExtensionHelper::AddExtensionInstallHintToErrorMsg(context, error_message, extension_name);
		return CatalogException(error_message);
	}

	// entries in other schemas get a penalty
	// however, if there is an exact match in another schema, we will always show it
	static constexpr const double UNSEEN_PENALTY = 0.2;
	auto unseen_entries = SimilarEntriesInSchemas(context, lookup_info, unseen_schemas);
	set<string> suggestions;

	if (!unseen_entries.empty() && (unseen_entries[0].score == 1.0 || unseen_entries[0].score - UNSEEN_PENALTY >
	                                                                      (entries.empty() ? 0.0 : entries[0].score))) {
		// the closest matching entry requires qualification as it is not in the default search path
		// check how to minimally qualify this entry
		for (auto &unseen_entry : unseen_entries) {
			auto catalog_name = unseen_entry.schema->catalog.GetName();
			auto schema_name = unseen_entry.schema->name;
			bool qualify_database;
			bool qualify_schema;
			FindMinimalQualification(retriever, catalog_name, schema_name, qualify_database, qualify_schema);
			auto qualified_name = unseen_entry.GetQualifiedName(qualify_database, qualify_schema);
			suggestions.insert(qualified_name);
		}
	} else if (!entries.empty()) {
		for (auto &entry : entries) {
			suggestions.insert(entry.name.GetIdentifierName());
		}
	}

	string did_you_mean;
	if (suggestions.size() > 2) {
		string last = *suggestions.rbegin();
		suggestions.erase(last);
		did_you_mean = StringUtil::Join(suggestions, ", ") + ", or " + last;
	} else {
		did_you_mean = StringUtil::Join(suggestions, " or ");
	}

	return CatalogException::MissingEntry(lookup_info, did_you_mean);
}

CatalogEntryLookup Catalog::TryLookupEntryInternal(CatalogTransaction transaction, const EntryLookupInfo &lookup_info) {
	if (lookup_info.GetAtClause() && !SupportsTimeTravel()) {
		return {nullptr, nullptr, ErrorData(BinderException("Catalog type does not support time travel"))};
	}
	// the (possibly nested) schema qualification is everything up to the entry name; LookupSchema resolves it
	auto &full_path = lookup_info.GetQualifiedName().Path();
	vector<Identifier> schema_path(full_path.begin(), full_path.end() - 1);
	auto schema_lookup = EntryLookupInfo::SchemaLookup(lookup_info, std::move(schema_path));
	auto schema_entry = LookupSchema(transaction, schema_lookup, OnEntryNotFound::RETURN_NULL);
	if (!schema_entry) {
		return {nullptr, nullptr, ErrorData()};
	}
	auto entry = schema_entry->LookupEntry(transaction, lookup_info);
	if (!entry) {
		return {schema_entry, nullptr, ErrorData()};
	}
	return {schema_entry, entry, ErrorData()};
}

CatalogEntryLookup Catalog::TryLookupEntry(CatalogEntryRetriever &retriever, const EntryLookupInfo &lookup_info,
                                           OnEntryNotFound if_not_found) {
	// Looks the entry up within this catalog only (the lookup_info's catalog qualification is ignored). This does not
	// resolve the catalog by name, so it is safe to use in contexts where the catalog is not yet registered (e.g. WAL
	// replay).
	auto &context = retriever.GetContext();
	auto &schema = lookup_info.GetSchema();
	reference_set_t<SchemaCatalogEntry> schemas;
	if (IsInvalidSchema(schema)) {
		// try all schemas for this catalog
		auto entries = GetCatalogEntries(retriever, GetName(), Identifier::InvalidSchema());
		for (auto &entry : entries) {
			auto transaction = GetCatalogTransaction(context);
			auto result = TryLookupEntryInternal(
			    transaction,
			    EntryLookupInfo(lookup_info, QualifiedName::FromCatalogSchema(GetName(), entry.GetSchemaPath(),
			                                                                  lookup_info.GetEntryIdentifier())));
			if (result.Found()) {
				return result;
			}
			if (result.schema) {
				schemas.insert(*result.schema);
			}
		}
	} else {
		auto transaction = GetCatalogTransaction(context);
		auto result = TryLookupEntryInternal(transaction, lookup_info);
		if (result.Found()) {
			return result;
		}
		if (result.schema) {
			schemas.insert(*result.schema);
		}
	}

	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		return {nullptr, nullptr, ErrorData()};
	}
	// Check if the default database is actually attached. CreateMissingEntryException will throw binder exception
	// otherwise.
	if (!GetCatalogEntry(context, Identifier(GetDefaultCatalog(retriever)))) {
		auto except = CatalogException("%s with name %s does not exist!",
		                               CatalogTypeToString(lookup_info.GetCatalogType()), lookup_info.GetEntryName());
		return {nullptr, nullptr, ErrorData(except)};
	} else {
		auto except = CreateMissingEntryException(retriever, lookup_info, schemas);
		return {nullptr, nullptr, ErrorData(except)};
	}
}

CatalogEntryLookup Catalog::TryLookupEntryAcrossCatalogs(CatalogEntryRetriever &retriever,
                                                         const EntryLookupInfo &lookup_info,
                                                         OnEntryNotFound if_not_found) {
	auto &catalog = lookup_info.GetCatalog();
	auto &schema = lookup_info.GetSchema();
	vector<CatalogLookup> lookups;
	auto &lookup_path = lookup_info.GetQualifiedName().Path();
	if (lookup_path.size() > 3) {
		// the entry is qualified with a nested schema path ([catalog, schema path..., name]) - this is unambiguous,
		// so we look it up directly in the given catalog instead of expanding the search path
		optional_ptr<Catalog> catalog_entry;
		if (if_not_found == OnEntryNotFound::RETURN_NULL) {
			catalog_entry = Catalog::GetCatalogEntry(retriever, lookup_path.front());
		} else {
			catalog_entry = &Catalog::GetCatalog(retriever, lookup_path.front());
		}
		if (!catalog_entry) {
			return {nullptr, nullptr, ErrorData()};
		}
		lookups.emplace_back(*catalog_entry, lookup_info);
		return TryLookupEntry(retriever, lookups, lookup_info, if_not_found, false);
	}
	auto entries = GetCatalogEntries(retriever, catalog, schema, lookup_info.GetCatalogType());
	vector<CatalogLookup> final_lookups;
	lookups.reserve(entries.size());
	for (auto &entry : entries) {
		optional_ptr<Catalog> catalog_entry;
		if (if_not_found == OnEntryNotFound::RETURN_NULL || IsInvalidCatalog(entry.GetCatalog())) {
			catalog_entry = Catalog::GetCatalogEntry(retriever, entry.GetCatalog());
		} else {
			catalog_entry = &Catalog::GetCatalog(retriever, entry.GetCatalog());
		}
		if (!catalog_entry) {
			if (IsInvalidCatalog(entry.GetCatalog())) {
				// the search path's default-database entry, with no database attached: nothing to search there
				continue;
			}
			return {nullptr, nullptr, ErrorData()};
		}
		D_ASSERT(catalog_entry);
		auto lookup_behavior = catalog_entry->CatalogTypeLookupRule(lookup_info.GetCatalogType());
		if (lookup_behavior == CatalogLookupBehavior::STANDARD) {
			lookups.emplace_back(*catalog_entry, entry, lookup_info);
		} else if (lookup_behavior == CatalogLookupBehavior::LOWER_PRIORITY) {
			final_lookups.emplace_back(*catalog_entry, entry, lookup_info);
		}
	}

	for (auto &lookup : final_lookups) {
		lookups.emplace_back(std::move(lookup));
	}

	bool allow_default_lookup = catalog.empty() && schema.empty();

	return TryLookupEntry(retriever, lookups, lookup_info, if_not_found, allow_default_lookup);
}

CatalogEntryLookup Catalog::LookupEntry(CatalogEntryRetriever &retriever, const EntryLookupInfo &lookup_info,
                                        OnEntryNotFound if_not_found) {
	auto res = TryLookupEntry(retriever, lookup_info, if_not_found);

	if (res.error.HasError()) {
		res.error.Throw();
	}

	return res;
}

CatalogEntryLookup Catalog::LookupEntry(CatalogEntryRetriever &retriever, const string &schema,
                                        const EntryLookupInfo &lookup_info, OnEntryNotFound if_not_found) {
	return LookupEntry(
	    retriever,
	    EntryLookupInfo(lookup_info, QualifiedName(GetName(), Identifier(schema), lookup_info.GetEntryIdentifier())),
	    if_not_found);
}

static void ThrowDefaultTableAmbiguityException(CatalogEntryLookup &base_lookup, CatalogEntryLookup &default_table,
                                                const string &name) {
	auto entry_type = CatalogTypeToString(base_lookup.entry->type);
	string fully_qualified_name_hint;
	if (base_lookup.schema) {
		fully_qualified_name_hint =
		    StringUtil::Format(": '%s'", base_lookup.schema->GetQualifiedName(base_lookup.entry->name).ToString());
	}
	string fully_qualified_catalog_name_hint =
	    StringUtil::Format(": '%s'", default_table.schema->GetQualifiedName(default_table.entry->name).ToString());
	throw CatalogException(
	    "Ambiguity detected for '%s': this could either refer to the '%s' '%s', or the "
	    "attached catalog '%s' which has a default table. To avoid this error, either detach the catalog and "
	    "reattach under a different name, or use a fully qualified name for the '%s'%s or for the Catalog "
	    "Default Table%s.",
	    name, entry_type, name, name, entry_type, fully_qualified_name_hint, fully_qualified_catalog_name_hint);
}

CatalogEntryLookup Catalog::TryLookupEntry(CatalogEntryRetriever &retriever, const vector<CatalogLookup> &lookups,
                                           const EntryLookupInfo &lookup_info, OnEntryNotFound if_not_found,
                                           bool allow_default_lookup) {
	auto &context = retriever.GetContext();
	reference_set_t<SchemaCatalogEntry> schemas;
	bool all_errors = true;
	ErrorData error_data;
	CatalogEntryLookup result;
	for (auto &lookup : lookups) {
		auto transaction = lookup.catalog.GetCatalogTransaction(context);
		result = lookup.catalog.TryLookupEntryInternal(transaction, lookup.lookup_info);
		if (result.Found()) {
			break;
		}
		if (result.schema) {
			schemas.insert(*result.schema);
		}
		if (!result.error.HasError()) {
			all_errors = false;
		} else {
			error_data = std::move(result.error);
		}
	}

	// Special case for tables: we do a second lookup searching for catalogs with default tables that also match this
	// lookup
	if (lookup_info.GetCatalogType() == CatalogType::TABLE_ENTRY && allow_default_lookup) {
		if (!result.Found()) {
			result = TryLookupDefaultTable(retriever, lookup_info, false);
			if (result.error.HasError()) {
				error_data = std::move(result.error);
			}
		} else {
			// allow_ignore_at_clause set to true to ensure `FROM <table_name> AT <version>` is considered
			// ambiguous with a default table lookup in a catalog that does not support time travel
			auto ambiguity_lookup = TryLookupDefaultTable(retriever, lookup_info, true);
			if (ambiguity_lookup.Found()) {
				ThrowDefaultTableAmbiguityException(result, ambiguity_lookup, lookup_info.GetEntryName());
			}
		}
	}

	// Special case for non-table entries (functions, macros, types): Fall back to the default schema of any
	// catalog flagged as an implicit search catalog. Ignored if the schema already had precedence.
	if (lookup_info.GetCatalogType() != CatalogType::TABLE_ENTRY && allow_default_lookup && !result.Found()) {
		result = TryLookupDefaultSchema(retriever, lookup_info);
		if (result.error.HasError()) {
			error_data = std::move(result.error);
		}
	}

	if (result.Found()) {
		return result;
	}

	if (all_errors && error_data.HasError()) {
		error_data.Throw();
	}

	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		return {nullptr, nullptr, ErrorData()};
	}

	// If we have a specific schema name and no schemas were found, the schema doesn't exist.
	// Throw an error about the schema instead of the entry. A nested schema path is unambiguous, so we report it
	// for every entry type; for a single schema level we only do so for tables (the message would otherwise hide
	// the search-path suggestions that are useful for e.g. functions).
	auto &lookup_path =
	    lookups.empty() ? lookup_info.GetQualifiedName().Path() : lookups[0].lookup_info.GetQualifiedName().Path();
	bool report_missing_schema = lookup_info.GetCatalogType() == CatalogType::TABLE_ENTRY || lookup_path.size() > 3;
	if (schemas.empty() && !lookups.empty() && report_missing_schema) {
		// the schema qualification is everything between the catalog and the entry name - for a nested schema this
		// is more than one component
		vector<string> schema_components;
		for (idx_t i = lookup_path.size() > 2 ? 1 : 0; i + 1 < lookup_path.size(); i++) {
			if (!lookup_path[i].empty()) {
				schema_components.push_back(lookup_path[i].GetIdentifierName());
			}
		}
		if (!schema_components.empty()) {
			auto schema_name = StringUtil::Join(schema_components, ".");
			string relation_name = schema_name + "." + lookup_info.GetEntryName();
			auto except =
			    CatalogException(lookup_info.GetErrorContext(),
			                     "%s with name \"%s\" does not exist because schema \"%s\" does not exist.",
			                     CatalogTypeToString(lookup_info.GetCatalogType()), relation_name, schema_name);
			return {nullptr, nullptr, ErrorData(except)};
		}
	}

	// Check if the default database is actually attached. CreateMissingEntryException will throw binder exception
	// otherwise.
	if (!GetCatalogEntry(context, Identifier(GetDefaultCatalog(retriever)))) {
		auto except = CatalogException("%s with name %s does not exist!",
		                               CatalogTypeToString(lookup_info.GetCatalogType()), lookup_info.GetEntryName());
		return {nullptr, nullptr, ErrorData(except)};
	} else {
		auto except = CreateMissingEntryException(retriever, lookup_info, schemas);
		return {nullptr, nullptr, ErrorData(except)};
	}
}

CatalogEntryLookup Catalog::TryLookupDefaultTable(CatalogEntryRetriever &retriever, const EntryLookupInfo &lookup_info,
                                                  bool allow_ignore_at_clause) {
	auto catalog_by_name = GetCatalogEntry(retriever, lookup_info.GetEntryIdentifier());

	if (catalog_by_name && catalog_by_name->HasDefaultTable()) {
		auto transaction = catalog_by_name->GetCatalogTransaction(retriever.GetContext());
		QueryErrorContext context;

		auto table_schema = catalog_by_name->GetDefaultTableSchema();
		auto table_name = catalog_by_name->GetDefaultTable();

		optional_ptr<BoundAtClause> at_clause;
		if (!catalog_by_name->SupportsTimeTravel() && allow_ignore_at_clause) {
			at_clause = nullptr;
		} else {
			at_clause = lookup_info.GetAtClause();
		}

		EntryLookupInfo info(CatalogType::TABLE_ENTRY,
		                     QualifiedName(catalog_by_name->GetName(), table_schema, table_name), at_clause, context);
		return catalog_by_name->TryLookupEntryInternal(transaction, info);
	}

	return {nullptr, nullptr, ErrorData()};
}

CatalogEntryLookup Catalog::TryLookupDefaultSchema(CatalogEntryRetriever &retriever,
                                                   const EntryLookupInfo &lookup_info) {
	// look for the entry in the default schema of every catalog that was flagged as an implicit search catalog
	auto &search_path = retriever.GetSearchPath();
	for (auto &implicit_catalog : search_path.GetImplicitSearchCatalogs()) {
		auto catalog_entry = GetCatalogEntry(retriever, implicit_catalog.GetCatalog());
		if (!catalog_entry) {
			continue;
		}
		auto default_schema = catalog_entry->GetDefaultSchema();
		if (!default_schema) {
			continue;
		}
		auto transaction = catalog_entry->GetCatalogTransaction(retriever.GetContext());
		EntryLookupInfo default_schema_lookup(
		    lookup_info, QualifiedName(catalog_entry->GetName(), *default_schema, lookup_info.GetEntryIdentifier()));
		auto result = catalog_entry->TryLookupEntryInternal(transaction, default_schema_lookup);
		if (result.Found() || result.error.HasError()) {
			return result;
		}
	}

	return {nullptr, nullptr, ErrorData()};
}

optional_ptr<CatalogEntry> Catalog::GetEntry(CatalogEntryRetriever &retriever, const EntryLookupInfo &lookup_info,
                                             OnEntryNotFound if_not_found) {
	auto result = TryLookupEntryAcrossCatalogs(retriever, lookup_info, if_not_found);

	// Try autoloading extension to resolve lookup
	if (!result.Found()) {
		if (AutoLoadExtensionByCatalogEntry(*retriever.GetContext().db, lookup_info.GetCatalogType(),
		                                    lookup_info.GetEntryIdentifier())) {
			result = TryLookupEntryAcrossCatalogs(retriever, lookup_info, if_not_found);
		}
	}

	if (result.error.HasError()) {
		result.error.Throw();
	}

	if (!result.Found()) {
		D_ASSERT(if_not_found == OnEntryNotFound::RETURN_NULL);
		return nullptr;
	}
	return result.entry.get();
}

optional_ptr<CatalogEntry> Catalog::GetEntry(ClientContext &context, const EntryLookupInfo &lookup_info,
                                             OnEntryNotFound if_not_found) {
	CatalogEntryRetriever retriever(context);
	return GetEntry(retriever, lookup_info, if_not_found);
}

CatalogEntry &Catalog::GetEntry(ClientContext &context, const EntryLookupInfo &lookup_info) {
	return *GetEntry(context, lookup_info, OnEntryNotFound::THROW_EXCEPTION);
}

CatalogEntry &Catalog::GetEntry(ClientContext &context, CatalogType catalog_type, const QualifiedName &name) {
	EntryLookupInfo lookup_info(catalog_type, name);
	return GetEntry(context, lookup_info);
}

CatalogEntry &Catalog::GetEntry(ClientContext &context, CatalogType catalog_type, const Identifier &catalog_name,
                                const Identifier &schema_name, const Identifier &name) {
	return GetEntry(context, catalog_type, QualifiedName(catalog_name, schema_name, name));
}

optional_ptr<CatalogEntry> Catalog::GetEntry(ClientContext &context, CatalogType catalog_type, const Identifier &schema,
                                             const Identifier &name, OnEntryNotFound if_not_found) {
	EntryLookupInfo lookup_info(catalog_type, QualifiedName(GetName(), schema, name));
	return GetEntry(context, lookup_info, if_not_found);
}

CatalogEntry &Catalog::GetEntry(ClientContext &context, CatalogType catalog_type, const Identifier &schema_name,
                                const Identifier &name) {
	EntryLookupInfo lookup_info(catalog_type, QualifiedName(GetName(), schema_name, name));
	return GetEntry(context, lookup_info);
}

optional_ptr<CatalogEntry> Catalog::GetEntry(CatalogEntryRetriever &retriever, const Identifier &schema_name,
                                             const EntryLookupInfo &lookup_info, OnEntryNotFound if_not_found) {
	// Instance lookup: search within this catalog only (do not resolve the catalog by name)
	EntryLookupInfo qualified(lookup_info, QualifiedName(GetName(), schema_name, lookup_info.GetEntryIdentifier()));
	return GetEntryInCatalog(retriever, qualified, if_not_found);
}

optional_ptr<CatalogEntry> Catalog::GetEntryInCatalog(CatalogEntryRetriever &retriever,
                                                      const EntryLookupInfo &lookup_info,
                                                      OnEntryNotFound if_not_found) {
	auto result = TryLookupEntry(retriever, lookup_info, if_not_found);

	// Try autoloading extension to resolve lookup
	if (!result.Found()) {
		if (AutoLoadExtensionByCatalogEntry(*retriever.GetContext().db, lookup_info.GetCatalogType(),
		                                    lookup_info.GetEntryIdentifier())) {
			result = TryLookupEntry(retriever, lookup_info, if_not_found);
		}
	}

	if (result.error.HasError()) {
		result.error.Throw();
	}

	return result.entry.get();
}

optional_ptr<CatalogEntry> Catalog::GetEntry(ClientContext &context, const Identifier &schema_name,
                                             const EntryLookupInfo &lookup_info, OnEntryNotFound if_not_found) {
	CatalogEntryRetriever retriever(context);
	return GetEntryInCatalog(
	    retriever,
	    EntryLookupInfo(lookup_info, QualifiedName(GetName(), schema_name, lookup_info.GetEntryIdentifier())),
	    if_not_found);
}

CatalogEntry &Catalog::GetEntry(ClientContext &context, const Identifier &schema, const EntryLookupInfo &lookup_info) {
	CatalogEntryRetriever retriever(context);
	return *GetEntryInCatalog(
	    retriever, EntryLookupInfo(lookup_info, QualifiedName(GetName(), schema, lookup_info.GetEntryIdentifier())),
	    OnEntryNotFound::THROW_EXCEPTION);
}

optional_ptr<CatalogEntry> Catalog::GetEntry(CatalogEntryRetriever &retriever, const Identifier &catalog,
                                             const Identifier &schema, const EntryLookupInfo &lookup_info,
                                             OnEntryNotFound if_not_found) {
	return GetEntry(retriever,
	                EntryLookupInfo(lookup_info, QualifiedName(catalog, schema, lookup_info.GetEntryIdentifier())),
	                if_not_found);
}

optional_ptr<CatalogEntry> Catalog::GetEntry(ClientContext &context, const Identifier &catalog,
                                             const Identifier &schema, const EntryLookupInfo &lookup_info,
                                             OnEntryNotFound if_not_found) {
	CatalogEntryRetriever retriever(context);
	return GetEntry(retriever,
	                EntryLookupInfo(lookup_info, QualifiedName(catalog, schema, lookup_info.GetEntryIdentifier())),
	                if_not_found);
}

CatalogEntry &Catalog::GetEntry(ClientContext &context, const Identifier &catalog, const Identifier &schema,
                                const EntryLookupInfo &lookup_info) {
	return *GetEntry(context,
	                 EntryLookupInfo(lookup_info, QualifiedName(catalog, schema, lookup_info.GetEntryIdentifier())),
	                 OnEntryNotFound::THROW_EXCEPTION);
}

optional_ptr<SchemaCatalogEntry> Catalog::GetSchema(CatalogEntryRetriever &retriever,
                                                    const EntryLookupInfo &schema_lookup,
                                                    OnEntryNotFound if_not_found) {
	auto &catalog_name = schema_lookup.GetCatalog();
	auto entries = GetCatalogEntries(retriever, catalog_name, schema_lookup.GetEntryIdentifier());
	for (idx_t i = 0; i < entries.size(); i++) {
		auto catalog = Catalog::GetCatalogEntry(retriever, entries[i].GetCatalog());
		if (!catalog) {
			// skip if it is not an attached database
			continue;
		}
		const auto on_not_found = i + 1 == entries.size() ? if_not_found : OnEntryNotFound::RETURN_NULL;
		auto result = catalog->GetSchema(retriever.GetContext(), schema_lookup, on_not_found);
		if (result) {
			return result;
		}
	}
	// Catalog has not been found.
	if (if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
		throw CatalogException(schema_lookup.GetErrorContext(), "Catalog with name %s does not exist!",
		                       catalog_name.GetIdentifierName());
	}
	return nullptr;
}

optional_ptr<SchemaCatalogEntry> Catalog::GetSchema(CatalogEntryRetriever &retriever, const Identifier &catalog_name,
                                                    const EntryLookupInfo &schema_lookup,
                                                    OnEntryNotFound if_not_found) {
	return GetSchema(
	    retriever,
	    EntryLookupInfo(schema_lookup, QualifiedName(catalog_name, Identifier(), schema_lookup.GetEntryIdentifier())),
	    if_not_found);
}

optional_ptr<SchemaCatalogEntry> Catalog::GetSchema(ClientContext &context, const Identifier &catalog_name,
                                                    const EntryLookupInfo &schema_lookup,
                                                    OnEntryNotFound if_not_found) {
	CatalogEntryRetriever retriever(context);
	return GetSchema(
	    retriever,
	    EntryLookupInfo(schema_lookup, QualifiedName(catalog_name, Identifier(), schema_lookup.GetEntryIdentifier())),
	    if_not_found);
}

vector<reference<SchemaCatalogEntry>> Catalog::GetSchemas(ClientContext &context) {
	vector<reference<SchemaCatalogEntry>> schemas;
	ScanSchemas(context, [&](SchemaCatalogEntry &entry) { schemas.push_back(entry); });
	return schemas;
}

bool SchemaScanFilters::Satisfies(ClientContext &context, optional_ptr<const TableFilter> filter, const Value &value) {
	if (!filter) {
		return true;
	}
	return ExpressionFilter::GetExpressionFilter(*filter, "SchemaScanFilters").EvaluateWithConstant(context, value);
}

bool SchemaScanFilters::Accepts(ClientContext &context, const SchemaCatalogEntry &schema) const {
	return Satisfies(context, schema_name, Value(schema.name.GetIdentifierName())) &&
	       Satisfies(context, schema_oid, Value::BIGINT(NumericCast<int64_t>(schema.oid)));
}

vector<reference<SchemaCatalogEntry>> Catalog::GetSchemasForScan(ClientContext &context,
                                                                 const SchemaScanFilters &filters) {
	vector<reference<SchemaCatalogEntry>> schemas;
	ScanSchemas(context, [&](SchemaCatalogEntry &entry) {
		if (filters.Accepts(context, entry)) {
			schemas.push_back(entry);
		}
	});
	return schemas;
}

vector<reference<SchemaCatalogEntry>> Catalog::GetSchemas(CatalogEntryRetriever &retriever,
                                                          const string &catalog_name) {
	vector<reference<Catalog>> catalogs;
	if (IsInvalidCatalog(catalog_name)) {
		reference_set_t<Catalog> inserted_catalogs;

		auto &search_path = retriever.GetSearchPath();
		for (auto &entry : search_path.Get()) {
			auto catalog = Catalog::GetCatalogEntry(retriever, entry.GetCatalog());
			if (!catalog) {
				if (IsInvalidCatalog(entry.GetCatalog())) {
					// the search path's default-database entry, with no database attached
					continue;
				}
				throw BinderException("Catalog %s does not exist!", entry.GetCatalog());
			}
			if (inserted_catalogs.find(*catalog) != inserted_catalogs.end()) {
				continue;
			}
			inserted_catalogs.insert(*catalog);
			catalogs.push_back(*catalog);
		}
	} else {
		catalogs.push_back(Catalog::GetCatalog(retriever, Identifier(catalog_name)));
	}
	vector<reference<SchemaCatalogEntry>> result;
	for (auto catalog : catalogs) {
		auto schemas = catalog.get().GetSchemas(retriever.GetContext());
		result.insert(result.end(), schemas.begin(), schemas.end());
	}
	return result;
}

vector<reference<SchemaCatalogEntry>> Catalog::GetSchemas(ClientContext &context, const string &catalog_name) {
	CatalogEntryRetriever retriever(context);
	return GetSchemas(retriever, catalog_name);
}

vector<reference<SchemaCatalogEntry>> Catalog::GetAllSchemas(ClientContext &context) {
	vector<reference<SchemaCatalogEntry>> result;

	auto &db_manager = DatabaseManager::Get(context);
	auto databases = db_manager.GetDatabases(context);
	for (auto &database : databases) {
		if (database->GetVisibility() == AttachVisibility::HIDDEN) {
			continue;
		}
		auto &catalog = database->GetCatalog();
		auto new_schemas = catalog.GetSchemas(context);
		result.insert(result.end(), new_schemas.begin(), new_schemas.end());
	}
	sort(result.begin(), result.end(),
	     [&](reference<SchemaCatalogEntry> left_p, reference<SchemaCatalogEntry> right_p) {
		     auto &left = left_p.get();
		     auto &right = right_p.get();
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

vector<reference<CatalogEntry>> Catalog::GetAllEntries(ClientContext &context, CatalogType catalog_type) {
	vector<reference<CatalogEntry>> result;
	auto schemas = GetAllSchemas(context);
	for (const auto &schema_ref : schemas) {
		auto &schema = schema_ref.get();
		schema.Scan(context, catalog_type, [&](CatalogEntry &entry) { result.push_back(entry); });
	}
	return result;
}

void Catalog::Alter(CatalogTransaction transaction, AlterInfo &info) {
	if (info.type == AlterType::ALTER_SCHEMA) {
		auto &schema_info = info.Cast<AlterSchemaInfo>();
		auto schema = GetSchema(transaction, schema_info.SchemaPath(), info.if_not_found);
		if (!schema) {
			return;
		}
		return AlterSchema(transaction, *schema, schema_info);
	}
	if (transaction.HasContext()) {
		CatalogEntryRetriever retriever(transaction.GetContext());
		EntryLookupInfo lookup_info(info.GetCatalogType(), info.GetQualifiedName());
		auto lookup = LookupEntry(retriever, lookup_info, info.if_not_found);
		if (!lookup.Found()) {
			return;
		}
		return lookup.schema->Alter(transaction, info);
	}
	D_ASSERT(info.if_not_found == OnEntryNotFound::THROW_EXCEPTION);
	auto &schema = GetEntrySchema(transaction, info.GetQualifiedName());
	return schema.Alter(transaction, info);
}

void Catalog::Alter(ClientContext &context, AlterInfo &info) {
	Alter(GetCatalogTransaction(context), info);
}

vector<MetadataBlockInfo> Catalog::GetMetadataInfo(ClientContext &context) {
	return vector<MetadataBlockInfo>();
}

optional_ptr<DependencyManager> Catalog::GetDependencyManager() {
	return nullptr;
}

ErrorData Catalog::SupportsCreateTable(BoundCreateTableInfo &info) {
	auto &base = info.Base().Cast<CreateTableInfo>();
	if (!base.partition_keys.empty()) {
		return ErrorData(
		    ExceptionType::CATALOG,
		    StringUtil::Format("PARTITIONED BY is not supported for tables in a %s catalog", GetCatalogType()));
	}
	if (!base.sort_keys.empty()) {
		return ErrorData(ExceptionType::CATALOG,
		                 StringUtil::Format("SORTED BY is not supported for tables in a %s catalog", GetCatalogType()));
	}
	if (!base.options.empty()) {
		return ErrorData(
		    ExceptionType::CATALOG,
		    StringUtil::Format("WITH clause is not supported for tables in a %s catalog", GetCatalogType()));
	}
	return ErrorData();
}

ErrorData Catalog::SupportsCreateSchema(CreateSchemaInfo &info) {
	if (!info.options.empty()) {
		return ErrorData(
		    ExceptionType::CATALOG,
		    StringUtil::Format("WITH clause is not supported for schemas in a %s catalog", GetCatalogType()));
	}
	return ErrorData();
}

void Catalog::AlterSchema(CatalogTransaction transaction, SchemaCatalogEntry &schema, AlterSchemaInfo &info) {
	switch (info.alter_schema_type) {
	case AlterSchemaType::SET_SCHEMA_OPTIONS:
		throw NotImplementedException("SET (<options>) is not supported for schemas in a %s catalog", GetCatalogType());
	case AlterSchemaType::RESET_SCHEMA_OPTIONS:
		throw NotImplementedException("RESET (<options>) is not supported for schemas in a %s catalog",
		                              GetCatalogType());
	default:
		throw InternalException("Unrecognized alter schema type!");
	}
}

optional<Identifier> Catalog::GetDefaultSchema() const {
	return Identifier(DEFAULT_SCHEMA);
}

//! Whether this catalog has a default table. Catalogs with a default table can be queries by their catalog name
bool Catalog::HasDefaultTable() const {
	return !default_table.empty();
}

void Catalog::SetDefaultTable(const Identifier &schema, const Identifier &name) {
	default_table = name;
	default_table_schema = schema;
}

Identifier Catalog::GetDefaultTable() const {
	return default_table;
}

Identifier Catalog::GetDefaultTableSchema() const {
	return !default_table_schema.empty() ? default_table_schema : DEFAULT_SCHEMA;
}

void Catalog::Verify() {
}

bool Catalog::IsSystemCatalog() const {
	return db.IsSystem();
}

bool Catalog::IsTemporaryCatalog() const {
	return db.IsTemporary();
}

void Catalog::Initialize(optional_ptr<ClientContext> context, bool load_builtin) {
	Initialize(load_builtin);
}

void Catalog::FinalizeLoad(optional_ptr<ClientContext> context) {
}

void Catalog::OnDetach(ClientContext &context) {
}

bool Catalog::HasConflictingAttachOptions(const string &path, const AttachOptions &options) {
	auto const db_type = options.db_type.empty() ? "duckdb" : options.db_type;
	// Normalize through the extension alias table so that equivalent forms
	auto canonical_actual = ExtensionHelper::ApplyExtensionAlias(GetCatalogType());
	auto canonical_requested = ExtensionHelper::ApplyExtensionAlias(db_type);
	return GetDBPath() != path || !StringUtil::CIEquals(canonical_actual, canonical_requested);
}

} // namespace duckdb
