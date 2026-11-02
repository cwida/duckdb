//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/catalog.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/enums/catalog_lookup_behavior.hpp"
#include "duckdb/common/enums/on_entry_not_found.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception/catalog_exception.hpp"
#include "duckdb/common/map.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/parser/query_error_context.hpp"
#include "duckdb/parser/qualified_name.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/common/types/string.hpp"

#include <functional>

namespace duckdb {
struct AttachOptions;
struct CreateSchemaInfo;
struct DropInfo;
struct BoundCreateTableInfo;
struct AlterTableInfo;
struct AlterSchemaInfo;
struct CreateTableFunctionInfo;
struct CreateCopyFunctionInfo;
struct CreatePragmaFunctionInfo;
struct CreateFunctionInfo;
struct CreateViewInfo;
struct CreateSequenceInfo;
struct CreateCollationInfo;
struct CreateCoordinateSystemInfo;
struct CreateIndexInfo;
struct CreateTypeInfo;
struct CreateTableInfo;
struct DatabaseSize;
struct MetadataBlockInfo;
struct SchemaScanFilters;

class AttachedDatabase;
class ClientContext;
class QueryContext;
class Transaction;

class AggregateFunctionCatalogEntry;
class CollateCatalogEntry;
class SchemaCatalogEntry;
class TableCatalogEntry;
class ViewCatalogEntry;
class SequenceCatalogEntry;
class TableFunctionCatalogEntry;
class CopyFunctionCatalogEntry;
class PragmaFunctionCatalogEntry;
class CatalogSet;
class DatabaseInstance;
class DependencyManager;

struct CatalogLookup;
struct CatalogEntryLookup;
struct SimilarCatalogEntry;

class Binder;
class LogicalOperator;
class LogicalMergeInto;
class PhysicalOperator;
class PhysicalPlanGenerator;
class LogicalCreateIndex;
class LogicalCreateTable;
class LogicalInsert;
class LogicalDelete;
class LogicalUpdate;
class CreateStatement;
class CatalogEntryRetriever;
class QueryNode;
class SQLStatement;

//! Per-capability opt-in for remote catalogs. Each value gates a specific dispatch path or
//! engine-wide accommodation:
//!  - IS_REMOTE: this catalog represents data hosted on a remote server. Drives generic
//!    accommodations (e.g. CheckAmbiguousCatalogOrSchema skips schema lookups, database_manager
//!    counts attached remote catalogs).
//!  - EXECUTE_QUERY_NODE: `RemoteExecute(QueryNode)` is implemented; the RemotePushdownOptimizer
//!    may push down structured queries to this catalog.
//!  - EXECUTE_STATEMENT: `RemoteExecute(SQLStatement)` is implemented; the RemotePushdownOptimizer
//!    may push down non-query statements (DDL) to this catalog.
//!  - CONNECT: `RemoteExecute(string)` is implemented; the CONNECT chokepoint may route raw SQL
//!    to this catalog.
enum class RemoteCapability : uint8_t {
	IS_REMOTE,
	EXECUTE_QUERY_NODE,
	EXECUTE_STATEMENT,
	CONNECT,
};

//! The Catalog object represents the catalog of the database.
class Catalog {
public:
	explicit Catalog(AttachedDatabase &db);
	virtual ~Catalog();

public:
	//! Get the SystemCatalog from the ClientContext
	DUCKDB_API static Catalog &GetSystemCatalog(ClientContext &context);
	//! Get the SystemCatalog from the DatabaseInstance
	DUCKDB_API static Catalog &GetSystemCatalog(DatabaseInstance &db);
	//! Get the specified Catalog from the ClientContext
	DUCKDB_API static Catalog &GetCatalog(ClientContext &context, const Identifier &catalog_name);
	//! Get the specified Catalog from the ClientContext
	DUCKDB_API static Catalog &GetCatalog(CatalogEntryRetriever &retriever, const Identifier &catalog_name);
	//! Get the specified Catalog from the DatabaseInstance
	DUCKDB_API static Catalog &GetCatalog(DatabaseInstance &db, const Identifier &catalog_name);
	//! Gets the specified Catalog from the database if it exists
	DUCKDB_API static optional_ptr<Catalog> GetCatalogEntry(ClientContext &context, const Identifier &catalog_name);
	//! Gets the specified Catalog from the database if it exists
	DUCKDB_API static optional_ptr<Catalog> GetCatalogEntry(CatalogEntryRetriever &retriever,
	                                                        const Identifier &catalog_name);
	//! Get the specific Catalog from the AttachedDatabase
	DUCKDB_API static Catalog &GetCatalog(AttachedDatabase &db);

	DUCKDB_API AttachedDatabase &GetAttached();
	DUCKDB_API const AttachedDatabase &GetAttached() const;
	DUCKDB_API DatabaseInstance &GetDatabase();

	virtual bool IsDuckCatalog() {
		return false;
	}

	virtual void Initialize(bool load_builtin) = 0;
	virtual void Initialize(optional_ptr<ClientContext> context, bool load_builtin);
	virtual void FinalizeLoad(optional_ptr<ClientContext> context);

	bool IsSystemCatalog() const;
	bool IsTemporaryCatalog() const;

	//! Returns a version number that uniquely characterizes the current catalog snapshot.
	//! If there are transaction-local changes, the version returned is >= TRANSACTION_START, o.w. it is a simple number
	//! starting at 0 that is incremented at each commit that has had catalog changes.
	//! If the catalog does not support versioning, no index is returned.
	DUCKDB_API virtual optional_idx GetCatalogVersion(ClientContext &context) {
		return {}; // don't return anything by default
	}

	//! Returns the catalog name - based on how the catalog was attached
	DUCKDB_API const Identifier &GetName() const;
	DUCKDB_API idx_t GetOid();
	DUCKDB_API virtual string GetCatalogType() = 0;

	DUCKDB_API CatalogTransaction GetCatalogTransaction(ClientContext &context);

	//! Creates a schema in the catalog.
	DUCKDB_API virtual optional_ptr<CatalogEntry> CreateSchema(CatalogTransaction transaction,
	                                                           CreateSchemaInfo &info) = 0;
	DUCKDB_API optional_ptr<CatalogEntry> CreateSchema(ClientContext &context, CreateSchemaInfo &info);
	//! Creates a table in the catalog.
	DUCKDB_API optional_ptr<CatalogEntry> CreateTable(CatalogTransaction transaction, BoundCreateTableInfo &info);
	DUCKDB_API optional_ptr<CatalogEntry> CreateTable(ClientContext &context, BoundCreateTableInfo &info);
	//! Creates a table in the catalog.
	DUCKDB_API optional_ptr<CatalogEntry> CreateTable(ClientContext &context, unique_ptr<CreateTableInfo> info);
	//! Create a table function in the catalog
	DUCKDB_API optional_ptr<CatalogEntry> CreateTableFunction(CatalogTransaction transaction,
	                                                          CreateTableFunctionInfo &info);
	DUCKDB_API optional_ptr<CatalogEntry> CreateTableFunction(ClientContext &context, CreateTableFunctionInfo &info);
	// Kept for backwards compatibility
	DUCKDB_API optional_ptr<CatalogEntry> CreateTableFunction(ClientContext &context,
	                                                          optional_ptr<CreateTableFunctionInfo> info);
	//! Create a copy function in the catalog
	DUCKDB_API optional_ptr<CatalogEntry> CreateCopyFunction(CatalogTransaction transaction,
	                                                         CreateCopyFunctionInfo &info);
	DUCKDB_API optional_ptr<CatalogEntry> CreateCopyFunction(ClientContext &context, CreateCopyFunctionInfo &info);
	//! Create a pragma function in the catalog
	DUCKDB_API optional_ptr<CatalogEntry> CreatePragmaFunction(CatalogTransaction transaction,
	                                                           CreatePragmaFunctionInfo &info);
	DUCKDB_API optional_ptr<CatalogEntry> CreatePragmaFunction(ClientContext &context, CreatePragmaFunctionInfo &info);
	//! Create a scalar or aggregate function in the catalog
	DUCKDB_API optional_ptr<CatalogEntry> CreateFunction(CatalogTransaction transaction, CreateFunctionInfo &info);
	DUCKDB_API optional_ptr<CatalogEntry> CreateFunction(ClientContext &context, CreateFunctionInfo &info);
	//! Creates a table in the catalog.
	DUCKDB_API optional_ptr<CatalogEntry> CreateView(CatalogTransaction transaction, CreateViewInfo &info);
	DUCKDB_API optional_ptr<CatalogEntry> CreateView(ClientContext &context, CreateViewInfo &info);
	//! Creates a sequence in the catalog.
	DUCKDB_API optional_ptr<CatalogEntry> CreateSequence(CatalogTransaction transaction, CreateSequenceInfo &info);
	DUCKDB_API optional_ptr<CatalogEntry> CreateSequence(ClientContext &context, CreateSequenceInfo &info);
	//! Creates a Enum in the catalog.
	DUCKDB_API optional_ptr<CatalogEntry> CreateType(CatalogTransaction transaction, CreateTypeInfo &info);
	DUCKDB_API optional_ptr<CatalogEntry> CreateType(ClientContext &context, CreateTypeInfo &info);
	//! Creates a collation in the catalog
	DUCKDB_API optional_ptr<CatalogEntry> CreateCollation(CatalogTransaction transaction, CreateCollationInfo &info);
	DUCKDB_API optional_ptr<CatalogEntry> CreateCollation(ClientContext &context, CreateCollationInfo &info);
	//! Creates a coordinate system in the catalog
	DUCKDB_API optional_ptr<CatalogEntry> CreateCoordinateSystem(CatalogTransaction transaction,
	                                                             CreateCoordinateSystemInfo &info);
	DUCKDB_API optional_ptr<CatalogEntry> CreateCoordinateSystem(ClientContext &context,
	                                                             CreateCoordinateSystemInfo &info);
	//! Creates an index in the catalog
	DUCKDB_API optional_ptr<CatalogEntry> CreateIndex(CatalogTransaction transaction, CreateIndexInfo &info);
	DUCKDB_API optional_ptr<CatalogEntry> CreateIndex(ClientContext &context, CreateIndexInfo &info);

	//! Creates a table in the catalog.
	DUCKDB_API optional_ptr<CatalogEntry> CreateTable(CatalogTransaction transaction, SchemaCatalogEntry &schema,
	                                                  BoundCreateTableInfo &info);
	//! Create a table function in the catalog
	DUCKDB_API optional_ptr<CatalogEntry>
	CreateTableFunction(CatalogTransaction transaction, SchemaCatalogEntry &schema, CreateTableFunctionInfo &info);
	//! Create a copy function in the catalog
	DUCKDB_API optional_ptr<CatalogEntry> CreateCopyFunction(CatalogTransaction transaction, SchemaCatalogEntry &schema,
	                                                         CreateCopyFunctionInfo &info);
	//! Create a pragma function in the catalog
	DUCKDB_API optional_ptr<CatalogEntry>
	CreatePragmaFunction(CatalogTransaction transaction, SchemaCatalogEntry &schema, CreatePragmaFunctionInfo &info);
	//! Create a scalar or aggregate function in the catalog
	DUCKDB_API optional_ptr<CatalogEntry> CreateFunction(CatalogTransaction transaction, SchemaCatalogEntry &schema,
	                                                     CreateFunctionInfo &info);
	//! Creates a view in the catalog
	DUCKDB_API optional_ptr<CatalogEntry> CreateView(CatalogTransaction transaction, SchemaCatalogEntry &schema,
	                                                 CreateViewInfo &info);
	//! Creates a table in the catalog.
	DUCKDB_API optional_ptr<CatalogEntry> CreateSequence(CatalogTransaction transaction, SchemaCatalogEntry &schema,
	                                                     CreateSequenceInfo &info);
	//! Creates a enum in the catalog.
	DUCKDB_API optional_ptr<CatalogEntry> CreateType(CatalogTransaction transaction, SchemaCatalogEntry &schema,
	                                                 CreateTypeInfo &info);
	//! Creates a collation in the catalog
	DUCKDB_API static optional_ptr<CatalogEntry> CreateCollation(CatalogTransaction transaction,
	                                                             SchemaCatalogEntry &schema, CreateCollationInfo &info);
	//! Creates a coordinate system in the catalog
	DUCKDB_API static optional_ptr<CatalogEntry> CreateCoordinateSystem(CatalogTransaction transaction,
	                                                                    SchemaCatalogEntry &schema,
	                                                                    CreateCoordinateSystemInfo &info);

	//! Drops an entry from the catalog
	DUCKDB_API void DropEntry(ClientContext &context, DropInfo &info);

	DUCKDB_API virtual optional_ptr<SchemaCatalogEntry> LookupSchema(CatalogTransaction transaction,
	                                                                 const EntryLookupInfo &schema_lookup,
	                                                                 OnEntryNotFound if_not_found) = 0;

	//! Whether schema strings can denote nested paths as well as literal schema names.
	virtual bool SupportsNestedSchemas() const {
		return false;
	}
	//! Resolve a string schema argument, preferring an existing nested entry over a literal dotted schema.
	DUCKDB_API QualifiedName ResolveEntryName(ClientContext &context, const string &schema_arg,
	                                          const string &entry_name, CatalogType type = CatalogType::TABLE_ENTRY,
	                                          optional_ptr<BoundAtClause> at_clause = nullptr);
	//! Resolve a string schema argument, with the same literal-name fallback as ResolveEntryName.
	DUCKDB_API SchemaCatalogEntry &ResolveSchema(ClientContext &context, const string &schema_arg);

	//! Returns the schema object with the specified name, or throws an exception if it does not exist
	DUCKDB_API SchemaCatalogEntry &GetSchema(ClientContext &context, const EntryLookupInfo &schema_lookup);
	DUCKDB_API optional_ptr<SchemaCatalogEntry> GetSchema(ClientContext &context, const EntryLookupInfo &schema_lookup,
	                                                      OnEntryNotFound if_not_found);
	//! Overloadable method for giving warnings on ambiguous naming id.tab due to a database and schema with name id
	DUCKDB_API virtual bool CheckAmbiguousCatalogOrSchema(ClientContext &context, const Identifier &schema);

	DUCKDB_API SchemaCatalogEntry &GetSchema(ClientContext &context, const Identifier &schema);
	DUCKDB_API SchemaCatalogEntry &GetSchema(CatalogTransaction transaction, const Identifier &schema);
	DUCKDB_API SchemaCatalogEntry &GetSchema(CatalogTransaction transaction, const EntryLookupInfo &schema_lookup);
	//! Resolves the catalog qualification carried in the schema_lookup and returns the matching schema
	DUCKDB_API static optional_ptr<SchemaCatalogEntry>
	GetSchema(CatalogEntryRetriever &retriever, const EntryLookupInfo &schema_lookup, OnEntryNotFound if_not_found);
	[[deprecated("Fold the catalog into the EntryLookupInfo and use GetSchema(context, "
	             "EntryLookupInfo)")]] DUCKDB_API static SchemaCatalogEntry &
	GetSchema(ClientContext &context, const Identifier &catalog_name, const EntryLookupInfo &schema_lookup);
	DUCKDB_API optional_ptr<SchemaCatalogEntry> GetSchema(ClientContext &context, const Identifier &schema,
	                                                      OnEntryNotFound if_not_found);
	DUCKDB_API optional_ptr<SchemaCatalogEntry> GetSchema(CatalogTransaction transaction, const Identifier &schema,
	                                                      OnEntryNotFound if_not_found);
	//! Look up a (possibly nested) schema by its path (outermost first) in this catalog
	DUCKDB_API optional_ptr<SchemaCatalogEntry>
	GetSchema(CatalogTransaction transaction, const vector<Identifier> &schema_path, OnEntryNotFound if_not_found);
	//! Resolve the (possibly nested) schema an entry lives in from its qualified name ([catalog, schema..., name])
	DUCKDB_API SchemaCatalogEntry &GetEntrySchema(CatalogTransaction transaction, const QualifiedName &name);
	[[deprecated("Fold the catalog into the EntryLookupInfo and use GetSchema(context, "
	             "EntryLookupInfo)")]] DUCKDB_API static optional_ptr<SchemaCatalogEntry>
	GetSchema(ClientContext &context, const Identifier &catalog_name, const EntryLookupInfo &schema_lookup,
	          OnEntryNotFound if_not_found);
	DUCKDB_API static SchemaCatalogEntry &GetSchema(ClientContext &context, const Identifier &catalog_name,
	                                                const Identifier &schema);
	DUCKDB_API static optional_ptr<SchemaCatalogEntry> GetSchema(ClientContext &context, const Identifier &catalog_name,
	                                                             const Identifier &schema,
	                                                             OnEntryNotFound if_not_found);
	//! Look up a (possibly nested) schema by its path (outermost first). A single-element path is the normal top-level
	//! lookup; deeper paths navigate the nested-schema chain.
	DUCKDB_API static optional_ptr<SchemaCatalogEntry> GetSchema(ClientContext &context, const Identifier &catalog_name,
	                                                             const vector<Identifier> &schema_path,
	                                                             OnEntryNotFound if_not_found);
	[[deprecated("Fold the catalog into the EntryLookupInfo and use GetSchema(retriever, "
	             "EntryLookupInfo)")]] DUCKDB_API static optional_ptr<SchemaCatalogEntry>
	GetSchema(CatalogEntryRetriever &retriever, const Identifier &catalog_name, const EntryLookupInfo &schema_lookup,
	          OnEntryNotFound if_not_found);
	//! Scans all the schemas in the system one-by-one, invoking the callback for each entry
	DUCKDB_API virtual void ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) = 0;
	//! The schemas a system catalog scan (duckdb_tables, duckdb_views) visits in this catalog, given the filters
	//! pushed down on the schema identity columns. Lists every schema and keeps those the filters accept. A catalog
	//! may override this to avoid the listing; it must return every schema the filters accept and may return more.
	DUCKDB_API virtual vector<reference<SchemaCatalogEntry>> GetSchemasForScan(ClientContext &context,
	                                                                           const SchemaScanFilters &filters);

	//! Gets the entry described by the (optionally catalog/schema-qualified) EntryLookupInfo. If the entry does not
	//! exist behavior depends on OnEntryNotFound
	DUCKDB_API static optional_ptr<CatalogEntry>
	GetEntry(CatalogEntryRetriever &retriever, const EntryLookupInfo &lookup_info, OnEntryNotFound if_not_found);
	DUCKDB_API static optional_ptr<CatalogEntry> GetEntry(ClientContext &context, const EntryLookupInfo &lookup_info,
	                                                      OnEntryNotFound if_not_found);
	DUCKDB_API static CatalogEntry &GetEntry(ClientContext &context, const EntryLookupInfo &lookup_info);
	DUCKDB_API optional_ptr<CatalogEntry> GetEntry(ClientContext &context, CatalogType catalog_type,
	                                               const Identifier &schema, const Identifier &name,
	                                               OnEntryNotFound if_not_found);

	//! Deprecated: the schema/catalog qualification is now carried inside the EntryLookupInfo - fold it in there
	//! (e.g. via QualifiedName) and use the GetEntry(EntryLookupInfo) overloads instead.
	[[deprecated("Fold the schema into the EntryLookupInfo and use GetEntry(context, EntryLookupInfo)")]] DUCKDB_API
	    optional_ptr<CatalogEntry>
	    GetEntry(ClientContext &context, const Identifier &schema, const EntryLookupInfo &lookup_info,
	             OnEntryNotFound if_not_found);
	[[deprecated("Fold the schema into the EntryLookupInfo and use GetEntry(retriever, EntryLookupInfo)")]] DUCKDB_API
	    optional_ptr<CatalogEntry>
	    GetEntry(CatalogEntryRetriever &retriever, const Identifier &schema, const EntryLookupInfo &lookup_info,
	             OnEntryNotFound if_not_found);
	[[deprecated("Fold the schema into the EntryLookupInfo and use GetEntry(context, EntryLookupInfo)")]] DUCKDB_API
	    CatalogEntry &
	    GetEntry(ClientContext &context, const Identifier &schema, const EntryLookupInfo &lookup_info);
	[[deprecated("Fold catalog/schema into the EntryLookupInfo and use GetEntry(context, "
	             "EntryLookupInfo)")]] DUCKDB_API static optional_ptr<CatalogEntry>
	GetEntry(ClientContext &context, const Identifier &catalog, const Identifier &schema,
	         const EntryLookupInfo &lookup_info, OnEntryNotFound if_not_found);
	[[deprecated("Fold catalog/schema into the EntryLookupInfo and use GetEntry(retriever, "
	             "EntryLookupInfo)")]] DUCKDB_API static optional_ptr<CatalogEntry>
	GetEntry(CatalogEntryRetriever &retriever, const Identifier &catalog, const Identifier &schema,
	         const EntryLookupInfo &lookup_info, OnEntryNotFound if_not_found);
	[[deprecated("Fold catalog/schema into the EntryLookupInfo and use GetEntry(context, "
	             "EntryLookupInfo)")]] DUCKDB_API static CatalogEntry &
	GetEntry(ClientContext &context, const Identifier &catalog, const Identifier &schema,
	         const EntryLookupInfo &lookup_info);

	template <class T>
	[[deprecated("Fold the catalog/schema into a QualifiedName and use GetEntry<T>(context, QualifiedName, ...) "
	             "instead")]] optional_ptr<T>
	GetEntry(ClientContext &context, const Identifier &schema_name, const Identifier &name,
	         OnEntryNotFound if_not_found, QueryErrorContext error_context = QueryErrorContext()) {
		return GetEntry<T>(context, QualifiedName(GetName(), schema_name, name), if_not_found, error_context);
	}

	template <class T>
	[[deprecated("Fold the catalog/schema into a QualifiedName and use GetEntry<T>(context, QualifiedName, ...) "
	             "instead")]] T &
	GetEntry(ClientContext &context, const Identifier &schema_name, const Identifier &name,
	         QueryErrorContext error_context = QueryErrorContext()) {
		return GetEntry<T>(context, QualifiedName(GetName(), schema_name, name), error_context);
	}

	//! Gets the "catalog.schema.name" entry of the specified type from a QualifiedName
	static CatalogEntry &GetEntry(ClientContext &context, CatalogType catalog_type, const QualifiedName &name);
	[[deprecated("Use GetEntry(context, catalog_type, QualifiedName) instead")]] static CatalogEntry &
	GetEntry(ClientContext &context, CatalogType catalog_type, const Identifier &catalog_name,
	         const Identifier &schema_name, const Identifier &name);
	CatalogEntry &GetEntry(ClientContext &context, CatalogType catalog_type, const Identifier &schema_name,
	                       const Identifier &name);

	//! Append a scalar or aggregate function to the catalog
	DUCKDB_API optional_ptr<CatalogEntry> AddFunction(ClientContext &context, CreateFunctionInfo &info);

	//! Alter an existing entry in the catalog.
	DUCKDB_API void Alter(CatalogTransaction transaction, AlterInfo &info);
	DUCKDB_API void Alter(ClientContext &context, AlterInfo &info);

	virtual PhysicalOperator &PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner,
	                                            LogicalCreateTable &op, PhysicalOperator &plan) = 0;
	virtual PhysicalOperator &PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
	                                     optional_ptr<PhysicalOperator> plan) = 0;
	virtual PhysicalOperator &PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
	                                     PhysicalOperator &plan) = 0;
	virtual PhysicalOperator &PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op);
	virtual PhysicalOperator &PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
	                                     PhysicalOperator &plan) = 0;
	virtual PhysicalOperator &PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op);
	virtual PhysicalOperator &PlanMergeInto(ClientContext &context, PhysicalPlanGenerator &planner,
	                                        LogicalMergeInto &op, PhysicalOperator &plan);
	virtual unique_ptr<LogicalOperator> BindCreateIndex(Binder &binder, CreateStatement &stmt, TableCatalogEntry &table,
	                                                    unique_ptr<LogicalOperator> plan);
	virtual unique_ptr<LogicalOperator> BindAlterAddIndex(Binder &binder, TableCatalogEntry &table_entry,
	                                                      unique_ptr<LogicalOperator> plan,
	                                                      unique_ptr<CreateIndexInfo> create_info,
	                                                      unique_ptr<AlterTableInfo> alter_info);

	virtual DatabaseSize GetDatabaseSize(ClientContext &context) = 0;
	virtual vector<MetadataBlockInfo> GetMetadataInfo(ClientContext &context);

	virtual bool InMemory() = 0;
	virtual string GetDBPath() = 0;
	virtual bool SupportsTimeTravel() const {
		return false;
	}
	virtual bool SupportsMultipleDMLCTEs() const {
		return false;
	}
	virtual bool IsEncrypted() const {
		return false;
	}
	virtual string GetEncryptionCipher() const {
		return string();
	}
	virtual ErrorData SupportsCreateTable(BoundCreateTableInfo &info);
	virtual ErrorData SupportsCreateSchema(CreateSchemaInfo &info);
	//! Alter a schema of this catalog (e.g. ALTER SCHEMA ... SET/RESET (<options>))
	virtual void AlterSchema(CatalogTransaction transaction, SchemaCatalogEntry &schema, AlterSchemaInfo &info);

	virtual bool Supports(RemoteCapability capability) const {
		return false;
	}
	virtual unique_ptr<TableRef> RemoteExecute(ClientContext &context, unique_ptr<QueryNode> node);
	//! Execute a full (non-query) statement remotely - the returned table ref yields the statement's result
	virtual unique_ptr<TableRef> RemoteExecute(ClientContext &context, unique_ptr<SQLStatement> statement);
	virtual unique_ptr<TableRef> RemoteExecute(ClientContext &context, const string &sql);
	virtual bool SupportsPushdown(const ParsedExpression &expression);
	virtual bool SupportsPushdown(const TableRef &ref);
	virtual bool SupportsPushdown(const QueryNode &node);
	virtual bool SupportsPushdown(const SQLStatement &statement);
	//! User-facing short identifier for this catalog (e.g. shown in the CLI prompt when CONNECT-ed).
	//! Defaults to the AttachedDatabase name (the AS alias). Remote catalogs override to expose
	//! backend-specific information — the URI for quack, host:port/dbname for postgres, etc.
	DUCKDB_API virtual string GetConnectDisplay();

	//! Whether or not this catalog should search a specific type with the standard priority
	DUCKDB_API virtual CatalogLookupBehavior CatalogTypeLookupRule(CatalogType type) const {
		return CatalogLookupBehavior::STANDARD;
	}

	//! Returns the default schema of the catalog, or nullopt if the catalog has no default schema.
	//! Catalogs without a default schema are never probed with an implicit schema for unqualified lookups.
	//! A returned value is always non-empty - an empty Identifier means "unspecified" elsewhere in the catalog.
	virtual optional<Identifier> GetDefaultSchema() const;

	//! The default table is used for `SELECT * FROM <catalog_name>;`
	//! FIXME: these should be virtual methods
	DUCKDB_API bool HasDefaultTable() const;
	DUCKDB_API void SetDefaultTable(const Identifier &schema, const Identifier &name);
	DUCKDB_API Identifier GetDefaultTable() const;
	DUCKDB_API Identifier GetDefaultTableSchema() const;

	//! Returns the dependency manager of this catalog - if the catalog has any
	virtual optional_ptr<DependencyManager> GetDependencyManager();

	//! Whether attaching a catalog with the given path and attach options would be considered a conflict
	virtual bool HasConflictingAttachOptions(const string &path, const AttachOptions &options);

public:
	template <class T>
	static optional_ptr<T> GetEntry(ClientContext &context, const QualifiedName &name, OnEntryNotFound if_not_found,
	                                QueryErrorContext error_context = QueryErrorContext()) {
		EntryLookupInfo lookup_info(T::Type, name, error_context);
		auto entry = GetEntry(context, lookup_info, if_not_found);
		if (!entry) {
			return nullptr;
		}
		if (entry->type != T::Type) {
			throw CatalogException(error_context, "%s is not an %s", name.Name().GetIdentifierName(), T::Name);
		}
		return &entry->template Cast<T>();
	}
	template <class T>
	static T &GetEntry(ClientContext &context, const QualifiedName &name,
	                   QueryErrorContext error_context = QueryErrorContext()) {
		auto entry = GetEntry<T>(context, name, OnEntryNotFound::THROW_EXCEPTION, error_context);
		return *entry;
	}

	template <class T>
	[[deprecated("Use GetEntry<T>(context, QualifiedName, ...) instead")]] static optional_ptr<T>
	GetEntry(ClientContext &context, const Identifier &catalog_name, const Identifier &schema_name,
	         const Identifier &name, OnEntryNotFound if_not_found,
	         QueryErrorContext error_context = QueryErrorContext()) {
		return GetEntry<T>(context, QualifiedName(catalog_name, schema_name, name), if_not_found, error_context);
	}
	template <class T>
	[[deprecated("Use GetEntry<T>(context, QualifiedName, ...) instead")]] static T &
	GetEntry(ClientContext &context, const Identifier &catalog_name, const Identifier &schema_name,
	         const Identifier &name, QueryErrorContext error_context = QueryErrorContext()) {
		return GetEntry<T>(context, QualifiedName(catalog_name, schema_name, name), error_context);
	}

	DUCKDB_API vector<reference<SchemaCatalogEntry>> GetSchemas(ClientContext &context);
	DUCKDB_API static vector<reference<SchemaCatalogEntry>> GetSchemas(ClientContext &context,
	                                                                   const string &catalog_name);
	DUCKDB_API static vector<reference<SchemaCatalogEntry>> GetSchemas(CatalogEntryRetriever &retriever,
	                                                                   const string &catalog_name);
	DUCKDB_API static vector<reference<SchemaCatalogEntry>> GetAllSchemas(ClientContext &context);

	static vector<reference<CatalogEntry>> GetAllEntries(ClientContext &context, CatalogType catalog_type);

	virtual void Verify();

	static CatalogException UnrecognizedConfigurationError(ClientContext &context, const Identifier &name);

	//! Autoload the extension required for `configuration_name` or throw a CatalogException
	static String AutoloadExtensionByConfigName(ClientContext &context, const Identifier &configuration_name);
	//! Autoload the extension required for `function_name` or throw a CatalogException
	static bool AutoLoadExtensionByCatalogEntry(DatabaseInstance &db, CatalogType type, const Identifier &entry_name);
	DUCKDB_API static bool TryAutoLoad(ClientContext &context, const string &extension_name) noexcept;

	//! Called when the catalog is detached
	DUCKDB_API virtual void OnDetach(ClientContext &context);

protected:
	//! Resolve a bound schema path, preserving time travel and error context at every level.
	DUCKDB_API optional_ptr<SchemaCatalogEntry>
	LookupSchemaPath(CatalogTransaction transaction, const EntryLookupInfo &schema_lookup, OnEntryNotFound if_not_found,
	                 const std::function<optional_ptr<CatalogEntry>(const EntryLookupInfo &)> &lookup_root);

	//! Reference to the database
	AttachedDatabase &db;

	//! (optionally) a default table to query for `SELECT * FROM <catalog_name>;`
	Identifier default_table;
	Identifier default_table_schema;

public:
	//! Lookup an entry using TryLookupEntry, throws if entry not found and if_not_found == THROW_EXCEPTION
	CatalogEntryLookup LookupEntry(CatalogEntryRetriever &retriever, const EntryLookupInfo &lookup_info,
	                               OnEntryNotFound if_not_found);
	[[deprecated(
	    "Fold the schema into the EntryLookupInfo and use LookupEntry(retriever, EntryLookupInfo)")]] DUCKDB_API
	    CatalogEntryLookup
	    LookupEntry(CatalogEntryRetriever &retriever, const string &schema, const EntryLookupInfo &lookup_info,
	                OnEntryNotFound if_not_found);

private:
	//! Look up an entry within this catalog and handle autoloading and errors
	optional_ptr<CatalogEntry> GetEntryInCatalog(CatalogEntryRetriever &retriever, const EntryLookupInfo &lookup_info,
	                                             OnEntryNotFound if_not_found);
	//! Lookup an entry in the schema (taken from the lookup_info), returning the entry and schema if they exist
	virtual CatalogEntryLookup TryLookupEntryInternal(CatalogTransaction transaction,
	                                                  const EntryLookupInfo &lookup_info);
	//! Looks the entry up within this catalog only, using the schema carried in the lookup_info. Does not resolve the
	//! catalog by name, so it is safe during WAL replay where the catalog may not yet be registered by name.
	CatalogEntryLookup TryLookupEntry(CatalogEntryRetriever &retriever, const EntryLookupInfo &lookup_info,
	                                  OnEntryNotFound if_not_found);
	//! Resolves the catalog/schema qualification carried in the lookup_info against the search path and looks the
	//! entry up. Sets CatalogEntryLookup->error depending on if_not_found when no entry is found
	static CatalogEntryLookup TryLookupEntryAcrossCatalogs(CatalogEntryRetriever &retriever,
	                                                       const EntryLookupInfo &lookup_info,
	                                                       OnEntryNotFound if_not_found);
	static CatalogEntryLookup TryLookupEntry(CatalogEntryRetriever &retriever, const vector<CatalogLookup> &lookups,
	                                         const EntryLookupInfo &lookup_info, OnEntryNotFound if_not_found,
	                                         bool allow_default_lookup);

	//! Looks for a Catalog with a DefaultTable that matches the lookup
	static CatalogEntryLookup TryLookupDefaultTable(CatalogEntryRetriever &retriever,
	                                                const EntryLookupInfo &lookup_info,
	                                                bool allow_ignore_at_clause = false);

	//! Looks for a non-table entry in the default schema of any implicit search catalog
	static CatalogEntryLookup TryLookupDefaultSchema(CatalogEntryRetriever &retriever,
	                                                 const EntryLookupInfo &lookup_info);

	//! Return an exception with did-you-mean suggestion.
	static CatalogException CreateMissingEntryException(CatalogEntryRetriever &retriever,
	                                                    const EntryLookupInfo &lookup_info,
	                                                    const reference_set_t<SchemaCatalogEntry> &schemas);

	//! Return the close entry name, the distance and the belonging schema.
	static vector<SimilarCatalogEntry> SimilarEntriesInSchemas(ClientContext &context,
	                                                           const EntryLookupInfo &lookup_info,
	                                                           const reference_set_t<SchemaCatalogEntry> &schemas);

	virtual void DropSchema(ClientContext &context, DropInfo &info) = 0;

public:
	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}

	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}
};

} // namespace duckdb
