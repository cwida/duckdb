//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/binder.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/identifier.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/enums/join_type.hpp"
#include "duckdb/common/enums/merge_action_type.hpp"
#include "duckdb/common/enums/statement_type.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/parser/column_definition.hpp"
#include "duckdb/parser/tableref/match_recognize_ref.hpp"
#include "duckdb/parser/query_node.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/parser/tableref/delimgetref.hpp"
#include "duckdb/parser/tokens.hpp"
#include "duckdb/planner/bind_context.hpp"
#include "duckdb/planner/bound_statement.hpp"
#include "duckdb/planner/bound_tokens.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/planner/joinside.hpp"
#include "duckdb/planner/bound_constraint.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/catalog/catalog_entry/trigger_catalog_entry.hpp"
#include "duckdb/common/enums/copy_option_mode.hpp"
#include "duckdb/common/enums/trigger_type.hpp"

//! fwd declare
namespace duckdb_re2 {
class RE2;
} // namespace duckdb_re2

namespace duckdb {
class BoundResultModifier;
class BoundSelectNode;
class ClientContext;
class ExpressionBinder;
struct ExternalResourceOptions;
class LimitModifier;
class OrderBinder;
class TableCatalogEntry;
class ViewCatalogEntry;
class TableMacroCatalogEntry;
class UpdateSetInfo;
class LogicalProjection;
class LogicalGet;
class LogicalUpdate;
class CopyQueryNode;
class LogicalVacuum;

class ColumnList;
class ExternalDependency;
class FunctionSignature;
class TableFunction;
class BoundTableFunction;
class TableStorageInfo;
class BoundConstraint;
class AtClause;
class BoundAtClause;

struct CreateInfo;
struct CreateSchemaInfo;
struct CreateTriggerInfo;
struct QualifiedName;
struct BoundCreateTableInfo;
struct CreateMacroInfo;
struct BoundOnConflictInfo;
struct CommonTableExpressionInfo;
struct BoundParameterMap;
struct BoundPragmaInfo;
struct BoundLimitNode;
struct EntryLookupInfo;
struct PivotColumnEntry;
struct UnpivotEntry;
struct CopyInfo;
struct CopyOption;
struct BoundSetOpChild;
struct BoundCTEData;
enum class CopyToType : uint8_t;
template <class T, class INDEX_TYPE>
class IndexVector;

//! Table indexes of the top select node of a TRY subquery, recorded while planning that node, so that
//! TRY can be lowered onto exactly that node's projection without crossing into other query levels
struct TrySubqueryPlanIndexes {
	bool recorded = false;
	//! The select list projection
	TableIndex projection_index;
	//! The prune projection above the result modifiers, only set when pruning is needed
	TableIndex prune_index;
	//! The aggregate node of the select node
	TableIndex aggregate_index;
	//! The window node of the select node
	TableIndex window_index;
	//! The unnest nodes of the select node
	vector<TableIndex> unnest_indexes;
	//! Whether the select item contains a nested scalar subquery lowered with an exposed error flag;
	//! its re-raise at the use site relies on this node's lowering to consume it
	bool nested_transport = false;
};

enum class BindingMode : uint8_t {
	STANDARD_BINDING,
	PREPARE,
	EXTRACT_NAMES,
	EXTRACT_REPLACEMENT_SCANS,
	EXTRACT_QUALIFIED_NAMES
};
enum class BinderType : uint8_t { REGULAR_BINDER, VIEW_BINDER };

struct CorrelatedColumnInfo {
	ColumnBinding binding;
	LogicalType type;
	Identifier name;
	idx_t depth;

	// NOLINTNEXTLINE - work-around bug in clang-tidy
	CorrelatedColumnInfo(ColumnBinding binding, LogicalType type_p, Identifier name_p, idx_t depth)
	    : binding(binding), type(std::move(type_p)), name(std::move(name_p)), depth(depth) {
	}
	explicit CorrelatedColumnInfo(BoundColumnRefExpression &expr)
	    : CorrelatedColumnInfo(expr.Binding(), expr.GetReturnType(), expr.GetName(), expr.Depth()) {
	}

	bool operator==(const CorrelatedColumnInfo &rhs) const {
		return binding == rhs.binding;
	}
};

struct CorrelatedColumns {
private:
	using container_type = vector<CorrelatedColumnInfo>;

public:
	CorrelatedColumns() : delim_index(1ULL << 63) {
	}

	void AddColumn(container_type::value_type info) {
		// Add to beginning
		correlated_columns.insert(correlated_columns.begin(), std::move(info));
		delim_index++;
	}
	void AddColumnToBack(container_type::value_type info) {
		// Add to end
		correlated_columns.push_back(std::move(info));
	}

	void SetDelimIndexToZero() {
		delim_index = 0;
	}

	idx_t GetDelimIndex() const {
		return delim_index;
	}

	const container_type::value_type &operator[](const idx_t &index) const {
		return correlated_columns.at(index);
	}

	idx_t size() const { // NOLINT: match stl case
		return correlated_columns.size();
	}

	bool empty() const { // NOLINT: match stl case
		return correlated_columns.empty();
	}

	void clear() { // NOLINT: match stl case
		correlated_columns.clear();
	}

	container_type::iterator begin() { // NOLINT: match stl case
		return correlated_columns.begin();
	}

	container_type::iterator end() { // NOLINT: match stl case
		return correlated_columns.end();
	}

	container_type::const_iterator begin() const { // NOLINT: match stl case
		return correlated_columns.begin();
	}

	container_type::const_iterator end() const { // NOLINT: match stl case
		return correlated_columns.end();
	}

private:
	container_type correlated_columns;
	idx_t delim_index;
};

//! GlobalBinderState is state shared over the ENTIRE query, including subqueries, views, etc
struct GlobalBinderState {
	//! The count of bound_tables
	idx_t bound_tables = 0;
	//! Statement properties
	StatementProperties prop;
	//! Binding mode
	BindingMode mode = BindingMode::STANDARD_BINDING;
	//! Table names extracted for BindingMode::EXTRACT_NAMES or BindingMode::EXTRACT_QUALIFIED_NAMES.
	unordered_set<string> table_names;
	//! Replacement Scans extracted for BindingMode::EXTRACT_REPLACEMENT_SCANS
	identifier_map_t<unique_ptr<TableRef>> replacement_scans;
	//! Using column sets
	vector<unique_ptr<UsingColumnSet>> using_column_sets;
	//! The set of parameter expressions bound by this binder
	optional_ptr<BoundParameterMap> parameters;
	//! Tables whose triggers have already been expanded in this query (recursion detection)
	reference_set_t<TableCatalogEntry> trigger_expanded_tables;
	//! Generated base-UPDATE nodes that must capture the pre-update (OLD) row image (the capture request),
	//! keyed by node identity. Internal trigger-expansion state; never set by SQL parsing and never serialized.
	reference_set_t<QueryNode> trigger_old_capture;
	//! Optional presentation detail for the statement-trigger CTE consumer: the reserved column names, in physical
	//! table order, under which the captured OLD image is exposed via SQL aliases. Only populated for capture
	//! requests presented through a CTE; a consumer that reads the OLD image directly leaves it absent.
	reference_map_t<QueryNode, vector<Identifier>> trigger_old_capture_cte_names;
	//! Set during CREATE TRIGGER body validation to detect self-recursive writes
	optional_ptr<TableCatalogEntry> trigger_creation_table;
	//! Name of the trigger being created (for error messages)
	Identifier trigger_creation_name;
};

//! Bind the parsed query tree to the actual columns present in the catalog.
/*!
  The binder is responsible for binding tables and columns to actual physical
  tables and columns in the catalog. In the process, it also resolves types of
  all expressions.
*/
class Binder : public enable_shared_from_this<Binder> {
	friend class ColumnQualifier;
	friend class ExpressionBinder;
	friend class RecursiveDependentJoinPlanner;

public:
	DUCKDB_API static shared_ptr<Binder> CreateBinder(ClientContext &context, optional_ptr<Binder> parent = nullptr,
	                                                  BinderType binder_type = BinderType::REGULAR_BINDER);

	//! The client context
	ClientContext &context;
	//! The bind context
	BindContext bind_context;
	//! The set of correlated columns bound by this binder (FIXME: this should probably be an unordered_set and not a
	//! vector)
	CorrelatedColumns correlated_columns;
	//! The alias for the currently processing subquery, if it exists
	Identifier alias;
	//! Macro parameter bindings (if any)
	optional_ptr<DummyBinding> macro_binding;
	//! The intermediate lambda bindings to bind nested lambdas (if any)
	optional_ptr<vector<DummyBinding>> lambda_bindings;

	unordered_map<TableIndex, LogicalOperator *> recursive_ctes;

public:
	//! Whether the expression is COUNT(tbl.*)
	static bool IsQualifiedCountStar(const ParsedExpression &expr);
	//! Rewrites COUNT(tbl.*) into a COUNT that skips the rows NULL-extended by an outer join, or returns nullptr
	unique_ptr<ParsedExpression> TryRewriteQualifiedCountStar(const ParsedExpression &expr);

public:
	DUCKDB_API BoundStatement Bind(SQLStatement &statement);
	DUCKDB_API BoundStatement Bind(QueryNode &node);

	unique_ptr<BoundCreateTableInfo> BindCreateTableInfo(unique_ptr<CreateInfo> info);
	unique_ptr<BoundCreateTableInfo> BindCreateTableInfo(unique_ptr<CreateInfo> info, SchemaCatalogEntry &schema,
	                                                     AlterBindMode bind_mode = AlterBindMode::BIND_ON_ALTER);
	unique_ptr<BoundCreateTableInfo> BindCreateTableInfo(unique_ptr<CreateInfo> info, SchemaCatalogEntry &schema,
	                                                     vector<unique_ptr<Expression>> &bound_defaults,
	                                                     AlterBindMode bind_mode = AlterBindMode::BIND_ON_ALTER);
	static unique_ptr<BoundCreateTableInfo> BindCreateTableCheckpoint(unique_ptr<CreateInfo> info,
	                                                                  SchemaCatalogEntry &schema);

	//! The lone LOGICAL_GET reached through identity projections, or nullptr if `op` is not a bare
	//! table-function passthrough.
	static optional_ptr<LogicalGet> GetPassthroughTableFunctionGet(LogicalOperator &op);

	static vector<unique_ptr<BoundConstraint>> BindConstraints(ClientContext &context,
	                                                           const vector<unique_ptr<Constraint>> &constraints,
	                                                           const Identifier &table_name, const ColumnList &columns);
	vector<unique_ptr<BoundConstraint>> BindConstraints(const vector<unique_ptr<Constraint>> &constraints,
	                                                    const Identifier &table_name, const ColumnList &columns);
	vector<unique_ptr<BoundConstraint>> BindConstraints(const TableCatalogEntry &table);
	vector<unique_ptr<BoundConstraint>> BindNewConstraints(vector<unique_ptr<Constraint>> &constraints,
	                                                       const Identifier &table_name, const ColumnList &columns);
	unique_ptr<BoundConstraint> BindConstraint(const Constraint &constraint, const Identifier &table,
	                                           const ColumnList &columns);
	unique_ptr<BoundConstraint> BindUniqueConstraint(const Constraint &constraint, const Identifier &table,
	                                                 const ColumnList &columns);
	static void VerifyConstraintCheckModeStorageVersion(const Constraint &constraint, Catalog &catalog, bool temporary);

	BoundStatement BindAlterAddIndex(BoundStatement &result, CatalogEntry &entry, unique_ptr<AlterInfo> alter_info);

	void SetCatalogLookupCallback(catalog_entry_callback_t callback);
	void BindCreateViewInfo(CreateViewInfo &base);
	static void BindView(ClientContext &context, const SelectStatement &stmt, const Identifier &catalog_name,
	                     const vector<Identifier> &schema_path, optional_ptr<LogicalDependencyList> dependencies,
	                     const vector<Identifier> &aliases, vector<LogicalType> &result_types,
	                     vector<Identifier> &result_names);

	void SearchSchema(CreateInfo &info);
	//! Resolve the leading component of a (possibly qualified) name into a catalog: if it names an attached database it
	//! becomes the catalog. The result has the catalog as the first path element. When default_catalog is true (the
	//! default) the default catalog is filled in when the name does not specify one; when false the catalog element is
	//! left empty (the caller applies its own default), which lets it distinguish "no catalog given" from an explicit
	//! catalog.
	QualifiedName ResolveCatalog(ClientContext &context, const QualifiedName &name, bool default_catalog = true);
	static QualifiedName ResolveCatalog(CatalogEntryRetriever &retriever, const QualifiedName &name,
	                                    bool default_catalog = true);
	//! Resolve the (possibly nested) name of a table into a name that can be looked up in the catalog. Unqualified
	//! names and names with a single schema level keep the (catalog, schema, name) shape so that the search path
	//! applies; a nested schema path yields a fully resolved [catalog, schema path..., name].
	static QualifiedName BindTableName(CatalogEntryRetriever &retriever, const QualifiedName &name);
	//! Register a read of the catalog owning the entry - required when bind data holds a reference to the entry,
	//! so that cached prepared statement plans are invalidated when the catalog changes
	static void RegisterEntryRead(optional_ptr<Binder> binder, ClientContext &context, CatalogEntry &entry);
	QualifiedName BindTableName(const QualifiedName &name);
	//! Resolve the (possibly nested) name of a CREATE SCHEMA statement into a canonical [catalog, parents..., schema]
	void BindCreateSchema(CreateSchemaInfo &info);
	SchemaCatalogEntry &BindSchema(CreateInfo &info);
	SchemaCatalogEntry &BindCreateFunctionInfo(CreateInfo &info);
	//! Reject macro entries with declared RETURNS types in storage below v2.1.0
	static void CheckMacroStorageVersion(Catalog &catalog, const CreateMacroInfo &info);
	SchemaCatalogEntry &BindCreateTriggerInfo(CreateTriggerInfo &info);

	unique_ptr<BoundPragmaInfo> BindPragma(PragmaInfo &info, QueryErrorContext error_context);

	BoundStatement Bind(TableRef &ref);

	//! Generates an unused index for a table
	TableIndex GenerateTableIndex();

	optional_ptr<CatalogEntry> GetCatalogEntry(const Identifier &catalog, const Identifier &schema,
	                                           const EntryLookupInfo &lookup_info, OnEntryNotFound on_entry_not_found);
	//! Look up an entry using the qualification carried in the lookup info (which can be a nested schema path)
	optional_ptr<CatalogEntry> GetCatalogEntry(const EntryLookupInfo &lookup_info, OnEntryNotFound on_entry_not_found);

	//! Find all candidate common table expression by name; returns empty vector if none exists
	optional_ptr<CTEBinding> GetCTEBinding(const BindingAlias &name);

	//! Add the view to the set of currently bound views - used for detecting recursive view definitions
	void AddBoundView(ViewCatalogEntry &view);
	void AddBoundTableMacro(CatalogEntry &table_macro);

	void BeginSubqueryBind(Binder &parent, ExpressionBinder &binder);
	//! The innermost enclosing scope
	ExpressionBinder &GetInnermostScope();
	bool HasEnclosingScope();
	void FinishSubqueryBind();

	//! The scopes enclosing this binder, stored outermost first
	const vector<reference<ExpressionBinder>> &GetEnclosingScopes() const;
	//! Add an enclosing scope that expressions bound by this binder can resolve against
	void PushScope(ExpressionBinder &binder);
	//! Remove the innermost enclosing scope
	void PopScope();
	//! Remove and return the scopes added since the chain had `count` entries
	vector<reference<ExpressionBinder>> SaveScopesAfter(idx_t count);
	//! Restore scopes previously removed by SaveScopesAfter
	void RestoreScopes(const vector<reference<ExpressionBinder>> &scopes);

	void MergeCorrelatedColumns(CorrelatedColumns &other);
	//! Add a correlated column to this binder (if it does not exist)
	void AddCorrelatedColumn(const CorrelatedColumnInfo &info);

	unique_ptr<LogicalOperator> BindUpdateSet(LogicalOperator &op, unique_ptr<LogicalOperator> root,
	                                          UpdateSetInfo &set_info, TableCatalogEntry &table,
	                                          const vector<unique_ptr<Expression>> &bound_defaults,
	                                          vector<PhysicalIndex> &columns,
	                                          bool prioritize_table_when_binding = false);
	void BindUpdateSet(TableIndex proj_index, unique_ptr<LogicalOperator> &root, UpdateSetInfo &set_info,
	                   TableCatalogEntry &table, vector<PhysicalIndex> &columns,
	                   const vector<unique_ptr<Expression>> &bound_defaults,
	                   vector<unique_ptr<Expression>> &update_expressions,
	                   vector<unique_ptr<Expression>> &projection_expressions,
	                   bool prioritize_table_when_binding = false);

	void BindVacuumTable(LogicalVacuum &vacuum, unique_ptr<LogicalOperator> &root);

	static void BindSchemaOrCatalog(ClientContext &context, Identifier &catalog, Identifier &schema);
	static void BindSchemaOrCatalog(ClientContext &context, QualifiedName &qualified_name);

	void BindLogicalType(LogicalType &type);
	//! Resolve a type expression into a concrete type
	LogicalType BindLogicalType(const ParsedExpression &type_expr);

	optional_ptr<Binding> GetMatchingBinding(const Identifier &table_name, const Identifier &column_name,
	                                         ErrorData &error);
	optional_ptr<Binding> GetMatchingBinding(const Identifier &schema_name, const Identifier &table_name,
	                                         const Identifier &column_name, ErrorData &error);
	optional_ptr<Binding> GetMatchingBinding(const Identifier &catalog_name, const Identifier &schema_name,
	                                         const Identifier &table_name, const Identifier &column_name,
	                                         ErrorData &error);
	//! Look up a binding for a (possibly nested) table qualification
	optional_ptr<Binding> GetMatchingBinding(const BindingAlias &alias, const Identifier &column_name,
	                                         ErrorData &error);

	void SetBindingMode(BindingMode mode);
	BindingMode GetBindingMode();
	void AddTableName(string table_name);
	void AddReplacementScan(const Identifier &table_name, unique_ptr<TableRef> replacement);
	const unordered_set<string> &GetTableNames();
	identifier_map_t<unique_ptr<TableRef>> &GetReplacementScans();
	CatalogEntryRetriever &EntryRetriever() {
		return entry_retriever;
	}
	optional_ptr<BoundParameterMap> GetParameters();
	void SetParameters(BoundParameterMap &parameters);
	//! Returns a ColumnRefExpression after it was resolved (i.e. past the STAR expression/USING clauses)
	static optional_ptr<ParsedExpression> GetResolvedColumnExpression(ParsedExpression &root_expr);

	void SetCanContainNulls(bool can_contain_nulls);
	bool CanContainNulls() const;
	void SetAlwaysRequireRebind();
	void SetInsideSubquery();
	bool IsInsideSubquery() const;
	//! Set while binding the key expressions of an index
	void SetBindingIndexKey(bool binding_index_key);
	bool IsBindingIndexKey() const;
	void SetBindingTrySubquery();
	bool IsBindingTrySubquery() const;
	const TrySubqueryPlanIndexes &GetTrySubqueryPlanIndexes() const;
	TrySubqueryPlanIndexes &GetTrySubqueryPlanIndexesMutable();

	StatementProperties &GetStatementProperties();
	static void ReplaceStarExpression(unique_ptr<ParsedExpression> &expr, unique_ptr<ParsedExpression> &replacement);
	static string ReplaceColumnsAlias(const string &alias, const string &column_name,
	                                  optional_ptr<duckdb_re2::RE2> regex);

	unique_ptr<LogicalOperator> UnionOperators(vector<unique_ptr<LogicalOperator>> nodes, idx_t column_count = 1,
	                                           TableIndex table_index = TableIndex());

	void SetSearchPath(Catalog &catalog, const vector<Identifier> &schema_path);
	void SetSearchPath(Catalog &catalog, const Identifier &schema);

	void BindDefaultValue(const ColumnDefinition &column, vector<unique_ptr<Expression>> &bound_defaults,
	                      const string &catalog = "", const vector<Identifier> &schema_path = {});
	unique_ptr<ParsedExpression> GetSQLValueFunction(const Identifier &column_name);
	Identifier GetExpressionName(const ParsedExpression &expr);

private:
	//! The parent binder (if any)
	shared_ptr<Binder> parent;
	//! What kind of node we are binding using this binder
	BinderType binder_type = BinderType::REGULAR_BINDER;
	//! Global binder state
	shared_ptr<GlobalBinderState> global_binder_state;
	//! Active binders
	vector<reference<ExpressionBinder>> active_binders;
	//! Whether or not outside dependent joins have been planned and flattened
	bool is_outside_flattened = true;
	//! LEGACY: Whether or not the binder can contain NULLs as the root of expressions
	bool legacy_can_contain_nulls = false;
	//! Whether this binder is inside a subquery boundary
	bool inside_subquery = false;
	//! Whether the key expressions of an index are being bound
	bool binding_index_key = false;
	//! Whether this binder binds a TRY subquery, which lowers TRY onto the bound plan after binding
	bool binding_try_subquery = false;
	//! The plan indexes of the TRY subquery's top select node, recorded while planning it
	TrySubqueryPlanIndexes try_subquery_plan_indexes;
	//! The set of bound views
	reference_set_t<ViewCatalogEntry> bound_views;
	//! The table macros that are being expanded by this binder
	reference_set_t<CatalogEntry> bound_table_macros;
	//! Used to retrieve CatalogEntry's
	CatalogEntryRetriever entry_retriever;
	//! Unnamed subquery index
	idx_t unnamed_subquery_index = 1;
	//! Binder depth
	idx_t depth;

private:
	//! Determine the depth of the binder
	idx_t GetBinderDepth() const;
	//! Increase the depth of the binder
	void IncreaseDepth();
	//! Bind the expressions of generated columns to check for errors
	void BindGeneratedColumns(BoundCreateTableInfo &info);
	//! Bind the default values of the columns of a table
	void BindDefaultValues(const ColumnList &columns, vector<unique_ptr<Expression>> &bound_defaults,
	                       const string &catalog = "", const vector<Identifier> &schema_path = {});
	//! Bind a limit value (LIMIT or OFFSET)
	BoundLimitNode BindLimitValue(OrderBinder &order_binder, unique_ptr<ParsedExpression> limit_val,
	                              LimitValueType value_type, bool is_offset);

	//! Move correlated expressions from the child binder to this binder
	void MoveCorrelatedExpressions(Binder &other);

	//! Tries to bind the table name with replacement scans
	BoundStatement BindWithReplacementScan(ClientContext &context, BaseTableRef &ref);

	BoundStatement Bind(SelectStatement &stmt);
	BoundStatement Bind(InsertStatement &stmt);
	BoundStatement Bind(CopyStatement &stmt, CopyToType copy_to_type);
	BoundStatement Bind(DeleteStatement &stmt);
	BoundStatement Bind(UpdateStatement &stmt);
	BoundStatement Bind(CreateStatement &stmt);
	BoundStatement Bind(DropStatement &stmt);
	BoundStatement Bind(AlterStatement &stmt);
	BoundStatement Bind(PrepareStatement &stmt);
	BoundStatement Bind(ExecuteStatement &stmt);
	BoundStatement Bind(TransactionStatement &stmt);
	BoundStatement Bind(PragmaStatement &stmt);
	BoundStatement Bind(ExplainStatement &stmt);
	BoundStatement Bind(VacuumStatement &stmt);
	BoundStatement Bind(RelationStatement &stmt);
	BoundStatement Bind(CallStatement &stmt);
	BoundStatement Bind(ExportStatement &stmt);
	BoundStatement Bind(ExtensionStatement &stmt);
	BoundStatement Bind(SetStatement &stmt);
	BoundStatement Bind(SetVariableStatement &stmt);
	BoundStatement Bind(ResetVariableStatement &stmt);
	BoundStatement Bind(LoadStatement &stmt);
	BoundStatement Bind(LogicalPlanStatement &stmt);
	BoundStatement Bind(AttachStatement &stmt);
	BoundStatement Bind(DetachStatement &stmt);
	BoundStatement Bind(CopyDatabaseStatement &stmt);
	BoundStatement Bind(UpdateExtensionsStatement &stmt);
	BoundStatement Bind(MergeIntoStatement &stmt);
	BoundStatement Bind(ConnectStatement &stmt);
	BoundStatement Bind(DisconnectStatement &stmt);
	BoundStatement Bind(ExternalResourceStatement &stmt);
	//! Bind + constant-fold a single EXTERNAL RESOURCE expression (a create param, or the REGISTER handle).
	Value BindExternalResourceValue(unique_ptr<ParsedExpression> &expr);
	//! Bind + constant-fold the create params of an EXTERNAL RESOURCE statement or clause.
	void BindExternalResourceParams(case_insensitive_map_t<unique_ptr<ParsedExpression>> &parsed_params,
	                                unordered_map<string, Value> &params);
	//! Bind the `ATTACH/CONNECT TO EXTERNAL RESOURCE ...` clause. Shared by ATTACH and CONNECT.
	void BindExternalResource(ExternalResourceOptions &external_resource);

	//! Resolves the base table for DROP TRIGGER, stamps catalog/schema onto stmt.info,
	//! and registers the catalog modification. IF EXISTS only guards the trigger, not the table.
	void BindDropTrigger(DropStatement &stmt, StatementProperties &properties);
	void BindRowIdColumns(TableCatalogEntry &table, LogicalGet &get, vector<unique_ptr<Expression>> &expressions);
	//! Append references to the scanned pre-update (OLD) value of every physical column, in table order, to the
	//! UPDATE child projection (before rowid). Records the input-chunk offset of the OLD block on the update.
	void BindOldRowCapture(TableCatalogEntry &table, LogicalGet &get, LogicalProjection &proj, LogicalUpdate &update);
	//! Build a mapping from storage column index to scan chunk index for RETURNING.
	//! Ensures all physical columns are present in the scan.
	//! return_columns[storage_idx] = scan_chunk_idx
	void BindDeleteReturningColumns(TableCatalogEntry &table, LogicalGet &get, vector<idx_t> &return_columns);
	//! Overload for MERGE INTO: also creates projection expressions and maps to projection indices
	//! return_columns[storage_idx] = projection_expr_idx
	void BindDeleteReturningColumns(TableCatalogEntry &table, LogicalGet &get, vector<idx_t> &return_columns,
	                                vector<unique_ptr<Expression>> &projection_expressions,
	                                LogicalOperator &target_binding);
	//! Build a sparse mapping for unique index columns only (for DELETE without RETURNING)
	//! return_columns[storage_idx] = scan_chunk_idx (only for indexed columns)
	void BindDeleteIndexColumns(TableCatalogEntry &table, LogicalGet &get, vector<idx_t> &return_columns);
	//! Overload for MERGE INTO: builds projection expressions and maps storage_idx -> projection_expr_idx
	void BindDeleteIndexColumns(TableCatalogEntry &table, LogicalGet &get, vector<idx_t> &return_columns,
	                            vector<unique_ptr<Expression>> &projection_expressions,
	                            LogicalOperator &target_binding);
	BoundStatement BindReturning(vector<unique_ptr<ParsedExpression>> returning_list, TableCatalogEntry &table,
	                             const Identifier &alias, TableIndex update_table_index,
	                             unique_ptr<LogicalOperator> child_operator,
	                             virtual_column_map_t virtual_columns = virtual_column_map_t());

	unique_ptr<QueryNode> BindTableMacro(FunctionExpression &function, TableMacroCatalogEntry &macro_func, idx_t depth);

	BoundStatement BindCTE(const Identifier &ctename, CommonTableExpressionInfo &info);

	BoundStatement BindNode(SelectNode &node);
	BoundStatement BindNode(SetOperationNode &node);
	BoundStatement BindNode(RecursiveCTENode &node);
	BoundStatement BindNode(QueryNode &node);
	BoundStatement BindNode(StatementNode &node);
	BoundStatement BindNode(InsertQueryNode &node);
	unique_ptr<BoundStatement> TryExpandTriggers(QueryNode &node, TableCatalogEntry &table,
	                                             TriggerEventType event_type);
	BoundStatement ExpandTriggers(QueryNode &node, TableCatalogEntry &table,
	                              const vector<const_reference<TriggerCatalogEntry>> &before_triggers,
	                              const vector<const_reference<TriggerCatalogEntry>> &after_triggers);
	unique_ptr<BoundStatement> TryExpandRowTriggers(QueryNode &node,
	                                                vector<unique_ptr<ParsedExpression>> &returning_list,
	                                                TableCatalogEntry &table, TriggerEventType event_type);
	BoundStatement ExpandRowTriggers(QueryNode &node, vector<unique_ptr<ParsedExpression>> &returning_list,
	                                 const TableCatalogEntry &table,
	                                 const vector<const_reference<TriggerCatalogEntry>> &triggers,
	                                 TriggerEventType event_type);
	//! Registers a row scope binding (named "new" for INSERT, "old" for DELETE) so child binders resolve
	//! NEW.col / OLD.col at depth=1. The returned binder is pushed as an enclosing scope.
	//! The caller must keep it alive until the matching pop_back().
	unique_ptr<ExpressionBinder> SetupRowScope(TableIndex table_index, const vector<Identifier> &col_names,
	                                           const vector<LogicalType> &col_types, const string &scope_name);
	//! Returns the correlated-column scope name for a given event type ("new" for INSERT, "old" for DELETE).
	static string RowScopeName(TriggerEventType event_type);
	BoundStatement BindNode(UpdateQueryNode &node);
	BoundStatement BindNode(DeleteQueryNode &node);
	BoundStatement BindNode(MergeQueryNode &node);
	BoundStatement BindNode(CopyQueryNode &node);

	unique_ptr<LogicalOperator> VisitQueryNode(BoundQueryNode &node, unique_ptr<LogicalOperator> root);
	unique_ptr<LogicalOperator> CreatePlan(BoundSelectNode &statement);
	unique_ptr<LogicalOperator> CreatePlan(BoundSetOperationNode &node);
	unique_ptr<LogicalOperator> CreatePlan(BoundQueryNode &node);

	void BuildUnionByNameInfo(BoundSetOperationNode &result);

	BoundStatement BindJoin(Binder &parent, TableRef &ref);
	BoundStatement Bind(BaseTableRef &ref);
	BoundStatement Bind(BoundRefWrapper &ref);
	BoundStatement Bind(JoinRef &ref);
	BoundStatement Bind(MatchRecognizeRef &ref);
	//! Rewrites a NEAREST BY join into a lateral join over a top-k subquery and binds the result
	BoundStatement BindNearestJoin(JoinRef &ref);
	BoundStatement Bind(SubqueryRef &ref);
	BoundStatement Bind(TableFunctionRef &ref);
	BoundStatement Bind(EmptyTableRef &ref);
	BoundStatement Bind(DelimGetRef &ref);
	BoundStatement Bind(ExpressionListRef &ref);
	BoundStatement Bind(ColumnDataRef &ref);
	BoundStatement Bind(PivotRef &expr);
	BoundStatement Bind(ShowRef &ref);

	unique_ptr<SelectNode> BindPivot(PivotRef &expr, vector<unique_ptr<ParsedExpression>> all_columns);
	unique_ptr<SelectNode> BindUnpivot(Binder &child_binder, PivotRef &expr,
	                                   vector<unique_ptr<ParsedExpression>> all_columns,
	                                   unique_ptr<ParsedExpression> &where_clause);
	BoundStatement BindBoundPivot(PivotRef &expr);
	void ExtractUnpivotEntries(Binder &child_binder, PivotColumnEntry &entry, vector<UnpivotEntry> &unpivot_entries);
	void ExtractUnpivotColumnName(ParsedExpression &expr, vector<Identifier> &result);

	unique_ptr<BoundAtClause> BindAtClause(optional_ptr<AtClause> at_clause);

	bool BindTableFunctionParameters(TableFunctionCatalogEntry &table_function,
	                                 vector<unique_ptr<ParsedExpression>> &expressions,
	                                 vector<unique_ptr<Expression>> &positional_arguments,
	                                 vector<pair<Identifier, unique_ptr<Expression>>> &named_arguments,
	                                 BoundStatement &subquery, bool &table_in_out, ErrorData &error);
	void BindTableInTableOutFunction(vector<unique_ptr<ParsedExpression>> &expressions, BoundStatement &subquery);
	BoundStatement BindTableFunction(TableFunction &function, vector<Value> parameters);
	BoundStatement BindTableFunctionInternal(BoundTableFunction &table_function, const TableFunctionRef &ref,
	                                         vector<Value> parameters, named_argument_map_t named_parameters,
	                                         vector<LogicalType> input_table_types,
	                                         vector<Identifier> input_table_names,
	                                         optional_ptr<unique_ptr<LogicalOperator>> input_plan);

	unique_ptr<LogicalOperator> CreatePlan(BoundJoinRef &ref);

	BoundStatement BindCopyTo(CopyStatement &stmt, const CopyFunction &function, CopyToType copy_to_type);
	BoundStatement BindCopyFrom(CopyStatement &stmt, const CopyFunction &function);
	void BindCopyFormat(CopyInfo &info);
	void BindCopyOptions(CopyInfo &info, const CopyFunction &function);
	identifier_map_t<CopyOption> GetFullCopyOptionsList(const CopyFunction &function, CopyOptionMode mode);

	void PrepareModifiers(OrderBinder &order_binder, QueryNode &statement, BoundQueryNode &result);
	void BindModifiers(BoundQueryNode &result, TableIndex table_index, const vector<Identifier> &names,
	                   const vector<LogicalType> &sql_types, const SelectBindState &bind_state);

	unique_ptr<BoundResultModifier> BindLimit(OrderBinder &order_binder, LimitModifier &limit_mod);
	unique_ptr<Expression> BindOrderExpression(OrderBinder &order_binder, unique_ptr<ParsedExpression> expr);

	unique_ptr<LogicalOperator> PlanFilter(unique_ptr<Expression> condition, unique_ptr<LogicalOperator> root);

	void PlanSubqueries(unique_ptr<Expression> &expr, unique_ptr<LogicalOperator> &root);
	unique_ptr<Expression> PlanSubquery(BoundSubqueryExpression &expr, unique_ptr<LogicalOperator> &root);
	unique_ptr<LogicalOperator> PlanLateralJoin(unique_ptr<LogicalOperator> left, unique_ptr<LogicalOperator> right,
	                                            CorrelatedColumns &correlated_columns,
	                                            JoinType join_type = JoinType::INNER,
	                                            unique_ptr<Expression> condition = nullptr);

	unique_ptr<LogicalOperator> CastLogicalOperatorToTypes(const vector<LogicalType> &source_types,
	                                                       const vector<LogicalType> &target_types,
	                                                       unique_ptr<LogicalOperator> op);

	BindingAlias FindBinding(const Identifier &using_column, const string &join_side);
	bool TryFindBinding(const Identifier &using_column, const string &join_side, BindingAlias &result);

	void AddUsingBindingSet(unique_ptr<UsingColumnSet> set);
	BindingAlias RetrieveUsingBinding(Binder &current_binder, optional_ptr<UsingColumnSet> current_set,
	                                  const Identifier &column_name, const string &join_side);

	void ExpandStarExpressions(vector<unique_ptr<ParsedExpression>> &select_list,
	                           vector<unique_ptr<ParsedExpression>> &new_select_list);
	void ExpandStarExpression(unique_ptr<ParsedExpression> expr, vector<unique_ptr<ParsedExpression>> &new_select_list);
	StarExpressionType FindStarExpression(unique_ptr<ParsedExpression> &expr, StarExpression **star, bool is_root,
	                                      bool in_columns);
	void ReplaceUnpackedStarExpression(unique_ptr<ParsedExpression> &expr,
	                                   vector<unique_ptr<ParsedExpression>> &replacements, StarExpression &star,
	                                   optional_ptr<duckdb_re2::RE2> regex);
	void BindWhereStarExpression(unique_ptr<ParsedExpression> &expr);
	void NormalizeFilterStarExpression(ParsedExpression &expr);
	void NormalizeFilterStarExpressions(SelectNode &statement);

	//! If only a schema name is provided (e.g. "a.b") then figure out if "a" is a schema or a catalog name
	void BindSchemaOrCatalog(Identifier &catalog_name, Identifier &schema_name);
	static void BindSchemaOrCatalog(CatalogEntryRetriever &retriever, Identifier &catalog, Identifier &schema);
	//! Resolve the (optional) schema/catalog of a qualified name in-place, overwriting it with the resolved name
	void BindSchemaOrCatalog(QualifiedName &qualified_name);
	static void BindSchemaOrCatalog(CatalogEntryRetriever &retriever, QualifiedName &qualified_name);
	Identifier BindCatalog(const Identifier &catalog_name);
	SchemaCatalogEntry &BindCreateSchema(CreateInfo &info);

	vector<CatalogSearchEntry> GetSearchPath(Catalog &catalog, const vector<Identifier> &schema_path,
	                                         bool default_schema_precedence = false);

	BoundStatement BindSelectNode(SelectNode &statement, BoundStatement from_table);

	unique_ptr<LogicalOperator> BindCopyDatabaseSchema(Catalog &source_catalog, const Identifier &target_database_name);
	unique_ptr<LogicalOperator> BindCopyDatabaseData(Catalog &source_catalog, const Identifier &target_database_name);

	BoundStatement BindDescribeQuery(ShowRef &ref);
	BoundStatement BindDescribeTable(ShowRef &ref);
	//! Describes a ShowRef target: its query if it has one, otherwise the named table
	BoundStatement BindDescribe(ShowRef &ref);
	//! Binds the Postgres-style "SHOW name" (settings-first, with a deprecated table/query describe fallback)
	BoundStatement BindShow(ShowRef &ref);
	//! Binds "SHOW name" to the value of the setting "name", returns false if no such setting exists
	bool TryBindShowSetting(ShowRef &ref, BoundStatement &result);
	BoundStatement BindSummarize(ShowRef &ref);

	void BindInsertColumnList(TableCatalogEntry &table, vector<Identifier> &columns, bool default_values,
	                          vector<LogicalIndex> &named_column_map, vector<LogicalType> &expected_types,
	                          IndexVector<idx_t, PhysicalIndex> &column_index_map);
	void TryReplaceDefaultExpression(unique_ptr<ParsedExpression> &expr, const ColumnDefinition &column);
	void ExpandDefaultInValuesList(InsertQueryNode &node, TableCatalogEntry &table,
	                               optional_ptr<ExpressionListRef> values_list,
	                               const vector<LogicalIndex> &named_column_map);

	unique_ptr<LogicalOperator> ResolveInputProjection(LogicalInsert &insert,
	                                                   const IndexVector<idx_t, PhysicalIndex> &column_index_map,
	                                                   unique_ptr<LogicalOperator> root,
	                                                   const vector<LogicalType> &source_types);

	unique_ptr<BoundMergeIntoAction>
	BindMergeAction(LogicalMergeInto &merge_into, TableCatalogEntry &table, LogicalGet &get, TableIndex proj_index,
	                vector<unique_ptr<Expression>> &expressions, MergeIntoAction &action,
	                const vector<BindingAlias> &source_aliases, const vector<Identifier> &source_names);

	unique_ptr<MergeIntoStatement> GenerateMergeInto(InsertQueryNode &node, TableCatalogEntry &table);

	static void CheckInsertColumnCountMismatch(idx_t expected_columns, idx_t result_columns, bool columns_provided,
	                                           const Identifier &tname);

	BoundCTEData PrepareCTE(const Identifier &ctename, CommonTableExpressionInfo &statement);
	BoundStatement FinishCTE(BoundCTEData &bound_cte, BoundStatement child_data);

	shared_ptr<Binder> CreateBinderWithSearchPath(const Identifier &catalog_name,
	                                              const vector<Identifier> &schema_path);

	bool DebugAggregateStateExportVerify(BoundSelectNode &statement, unique_ptr<LogicalOperator> &root);

private:
	Binder(ClientContext &context, shared_ptr<Binder> parent, BinderType binder_type);
};

} // namespace duckdb
