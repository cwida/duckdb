#include "duckdb/planner/binder.hpp"

#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/common/enums/cte_materialize.hpp"
#include "duckdb/parser/common_table_expression_info.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/query_node/list.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/query_node/delete_query_node.hpp"
#include "duckdb/parser/statement/list.hpp"
#include "duckdb/parser/tableref/list.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/planner/bound_query_node.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression_binder/returning_binder.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_sample.hpp"
#include "duckdb/planner/query_node/list.hpp"
#include "duckdb/planner/tableref/list.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/common/types/uuid.hpp"

#include <algorithm>

namespace duckdb {

idx_t Binder::GetBinderDepth() const {
	return depth;
}

void Binder::IncreaseDepth() {
	depth++;
	auto max_expression_depth = Settings::Get<MaxExpressionDepthSetting>(context);
	if (depth > max_expression_depth) {
		throw BinderException("Max expression depth limit of %lld exceeded. Use \"SET max_expression_depth TO x\" to "
		                      "increase the maximum expression depth.",
		                      max_expression_depth);
	}
}

shared_ptr<Binder> Binder::CreateBinder(ClientContext &context, optional_ptr<Binder> parent, BinderType binder_type) {
	return shared_ptr<Binder>(new Binder(context, parent ? parent->shared_from_this() : nullptr, binder_type));
}

Binder::Binder(ClientContext &context, shared_ptr<Binder> parent_p, BinderType binder_type)
    : context(context), bind_context(*this), parent(std::move(parent_p)), binder_type(binder_type),
      global_binder_state(parent ? parent->global_binder_state : make_shared_ptr<GlobalBinderState>()),
      entry_retriever(context), depth(parent ? parent->GetBinderDepth() : 1) {
	IncreaseDepth();
	if (parent) {
		entry_retriever.Inherit(parent->entry_retriever);
		inside_subquery = parent->inside_subquery;

		// We have to inherit macro and lambda parameter bindings and from the parent binder, if there is a parent.
		macro_binding = parent->macro_binding;
		lambda_bindings = parent->lambda_bindings;
		if (binder_type != BinderType::VIEW_BINDER) {
			// inherit expression binders from parent
			active_binders = parent->active_binders;
		}
	}
}

BoundStatement Binder::Bind(SQLStatement &statement) {
	switch (statement.type) {
	case StatementType::SELECT_STATEMENT:
		return Bind(statement.Cast<SelectStatement>());
	case StatementType::COPY_STATEMENT:
		return Bind(statement.Cast<CopyStatement>(), CopyToType::COPY_TO_FILE);
	case StatementType::INSERT_STATEMENT:
		return Bind(statement.Cast<InsertStatement>());
	case StatementType::DELETE_STATEMENT:
		return Bind(statement.Cast<DeleteStatement>());
	case StatementType::UPDATE_STATEMENT:
		return Bind(statement.Cast<UpdateStatement>());
	case StatementType::RELATION_STATEMENT:
		return Bind(statement.Cast<RelationStatement>());
	case StatementType::CREATE_STATEMENT:
		return Bind(statement.Cast<CreateStatement>());
	case StatementType::DROP_STATEMENT:
		return Bind(statement.Cast<DropStatement>());
	case StatementType::ALTER_STATEMENT:
		return Bind(statement.Cast<AlterStatement>());
	case StatementType::TRANSACTION_STATEMENT:
		return Bind(statement.Cast<TransactionStatement>());
	case StatementType::PRAGMA_STATEMENT:
		return Bind(statement.Cast<PragmaStatement>());
	case StatementType::EXPLAIN_STATEMENT:
		return Bind(statement.Cast<ExplainStatement>());
	case StatementType::VACUUM_STATEMENT:
		return Bind(statement.Cast<VacuumStatement>());
	case StatementType::CALL_STATEMENT:
		return Bind(statement.Cast<CallStatement>());
	case StatementType::EXPORT_STATEMENT:
		return Bind(statement.Cast<ExportStatement>());
	case StatementType::SET_STATEMENT:
		return Bind(statement.Cast<SetStatement>());
	case StatementType::LOAD_STATEMENT:
		return Bind(statement.Cast<LoadStatement>());
	case StatementType::EXTENSION_STATEMENT:
		return Bind(statement.Cast<ExtensionStatement>());
	case StatementType::PREPARE_STATEMENT:
		return Bind(statement.Cast<PrepareStatement>());
	case StatementType::EXECUTE_STATEMENT:
		return Bind(statement.Cast<ExecuteStatement>());
	case StatementType::LOGICAL_PLAN_STATEMENT:
		return Bind(statement.Cast<LogicalPlanStatement>());
	case StatementType::ATTACH_STATEMENT:
		return Bind(statement.Cast<AttachStatement>());
	case StatementType::DETACH_STATEMENT:
		return Bind(statement.Cast<DetachStatement>());
	case StatementType::COPY_DATABASE_STATEMENT:
		return Bind(statement.Cast<CopyDatabaseStatement>());
	case StatementType::UPDATE_EXTENSIONS_STATEMENT:
		return Bind(statement.Cast<UpdateExtensionsStatement>());
	case StatementType::MERGE_INTO_STATEMENT:
		return Bind(statement.Cast<MergeIntoStatement>());
	case StatementType::CONNECT_STATEMENT:
		return Bind(statement.Cast<ConnectStatement>());
	case StatementType::DISCONNECT_STATEMENT:
		return Bind(statement.Cast<DisconnectStatement>());
	case StatementType::EXTERNAL_RESOURCE_STATEMENT:
		return Bind(statement.Cast<ExternalResourceStatement>());
	default: // LCOV_EXCL_START
		throw NotImplementedException("Unimplemented statement type \"%s\" for Bind",
		                              StatementTypeToString(statement.type));
	} // LCOV_EXCL_STOP
}

BoundStatement Binder::Bind(QueryNode &node) {
	return BindNode(node);
}

BoundStatement Binder::Bind(TableRef &ref) {
	for (auto &column_alias : ref.column_name_alias) {
		if (column_alias.empty()) {
			throw BinderException(ref, "column alias cannot be empty");
		}
	}
	BoundStatement result;
	switch (ref.type) {
	case TableReferenceType::BASE_TABLE:
		result = Bind(ref.Cast<BaseTableRef>());
		break;
	case TableReferenceType::JOIN:
		result = Bind(ref.Cast<JoinRef>());
		break;
	case TableReferenceType::SUBQUERY:
		result = Bind(ref.Cast<SubqueryRef>());
		break;
	case TableReferenceType::EMPTY_FROM:
		result = Bind(ref.Cast<EmptyTableRef>());
		break;
	case TableReferenceType::TABLE_FUNCTION:
		result = Bind(ref.Cast<TableFunctionRef>());
		break;
	case TableReferenceType::EXPRESSION_LIST:
		result = Bind(ref.Cast<ExpressionListRef>());
		break;
	case TableReferenceType::COLUMN_DATA:
		result = Bind(ref.Cast<ColumnDataRef>());
		break;
	case TableReferenceType::PIVOT:
		result = Bind(ref.Cast<PivotRef>());
		break;
	case TableReferenceType::SHOW_REF:
		result = Bind(ref.Cast<ShowRef>());
		break;
	case TableReferenceType::DELIM_GET:
		result = Bind(ref.Cast<DelimGetRef>());
		break;
	case TableReferenceType::MATCH_RECOGNIZE:
		result = Bind(ref.Cast<MatchRecognizeRef>());
		break;
	case TableReferenceType::BOUND_TABLE_REF:
		result = Bind(ref.Cast<BoundRefWrapper>());
		break;
	case TableReferenceType::CTE:
	case TableReferenceType::INVALID:
	default:
		throw InternalException("Unknown table ref type (%s)", EnumUtil::ToString(ref.type));
	}
	if (ref.sample) {
		result.plan = make_uniq<LogicalSample>(std::move(ref.sample), std::move(result.plan));
	}
	return result;
}

optional_ptr<CTEBinding> Binder::GetCTEBinding(const BindingAlias &name) {
	reference<Binder> current_binder(*this);
	optional_ptr<CTEBinding> result;
	while (true) {
		auto &current = current_binder.get();
		auto entry = current.bind_context.GetCTEBinding(name);
		if (entry) {
			// we only directly return the CTE if it can be referenced
			// if it cannot be referenced (circular reference) we keep going up the stack
			// to look for a CTE that can be referenced
			if (entry->CanBeReferenced()) {
				return entry;
			}
			result = entry;
		}
		if (!current.parent || current.binder_type != BinderType::REGULAR_BINDER) {
			break;
		}
		current_binder = *current.parent;
	}
	return result;
}

void Binder::AddBoundView(ViewCatalogEntry &view) {
	// check if the view is already bound
	auto current = this;
	while (current) {
		if (current->bound_views.find(view) != current->bound_views.end()) {
			throw BinderException("infinite recursion detected: attempting to recursively bind view %s", view.name);
		}
		current = current->parent.get();
	}
	bound_views.insert(view);
}

void Binder::AddBoundTableMacro(CatalogEntry &table_macro) {
	// check if the table macro is already being expanded
	auto current = this;
	while (current) {
		if (current->bound_table_macros.find(table_macro) != current->bound_table_macros.end()) {
			throw BinderException("infinite recursion detected: attempting to recursively expand table macro %s",
			                      table_macro.name);
		}
		current = current->parent.get();
	}
	bound_table_macros.insert(table_macro);
}

TableIndex Binder::GenerateTableIndex() {
	return TableIndex(global_binder_state->bound_tables++);
}

StatementProperties &Binder::GetStatementProperties() {
	return global_binder_state->prop;
}

optional_ptr<LogicalGet> Binder::GetPassthroughTableFunctionGet(LogicalOperator &op) {
	// Follow single-child projections down to a lone LOGICAL_GET; anything else is not a passthrough.
	auto *current = &op;
	while (true) {
		if (current->type == LogicalOperatorType::LOGICAL_GET) {
			return current->Cast<LogicalGet>();
		}
		if (current->type != LogicalOperatorType::LOGICAL_PROJECTION || current->children.size() != 1) {
			return nullptr;
		}
		current = current->children[0].get();
	}
}

optional_ptr<BoundParameterMap> Binder::GetParameters() {
	return global_binder_state->parameters;
}

void Binder::SetParameters(BoundParameterMap &parameters) {
	global_binder_state->parameters = parameters;
}

void Binder::SetInsideSubquery() {
	inside_subquery = true;
}

bool Binder::IsInsideSubquery() const {
	return inside_subquery;
}

void Binder::SetBindingTrySubquery() {
	binding_try_subquery = true;
}

bool Binder::IsBindingTrySubquery() const {
	return binding_try_subquery;
}

const TrySubqueryPlanIndexes &Binder::GetTrySubqueryPlanIndexes() const {
	return try_subquery_plan_indexes;
}

TrySubqueryPlanIndexes &Binder::GetTrySubqueryPlanIndexesMutable() {
	return try_subquery_plan_indexes;
}

void Binder::BeginSubqueryBind(Binder &parent, ExpressionBinder &binder) {
	// this binder already inherited a copy of the enclosing scopes when it was created, so replace them
	// rather than appending the parent's on top: appending would list every scope twice per level of
	// nesting, growing the chain as 2^depth and making the index of a scope no longer unique
	active_binders = parent.active_binders;
	// the scope that is binding the subquery encloses it
	active_binders.push_back(binder);
}

void Binder::FinishSubqueryBind() {
	active_binders.clear();
}

ExpressionBinder &Binder::GetInnermostScope() {
	return active_binders.back();
}

bool Binder::HasEnclosingScope() {
	return !active_binders.empty();
}

const vector<reference<ExpressionBinder>> &Binder::GetEnclosingScopes() const {
	return active_binders;
}

void Binder::PushScope(ExpressionBinder &binder) {
	active_binders.push_back(binder);
}

void Binder::PopScope() {
	active_binders.pop_back();
}

vector<reference<ExpressionBinder>> Binder::SaveScopesAfter(idx_t count) {
	vector<reference<ExpressionBinder>> result;
	for (idx_t i = count; i < active_binders.size(); i++) {
		result.push_back(active_binders[i]);
	}
	active_binders.erase(active_binders.begin() + UnsafeNumericCast<int64_t>(count), active_binders.end());
	return result;
}

void Binder::RestoreScopes(const vector<reference<ExpressionBinder>> &scopes) {
	for (auto &scope : scopes) {
		active_binders.push_back(scope);
	}
}

void Binder::AddUsingBindingSet(unique_ptr<UsingColumnSet> set) {
	global_binder_state->using_column_sets.push_back(std::move(set));
}

void Binder::MoveCorrelatedExpressions(Binder &other) {
	MergeCorrelatedColumns(other.correlated_columns);
	other.correlated_columns.clear();
}

void Binder::MergeCorrelatedColumns(CorrelatedColumns &other) {
	for (idx_t i = 0; i < other.size(); i++) {
		AddCorrelatedColumn(other[i]);
	}
}

void Binder::AddCorrelatedColumn(const CorrelatedColumnInfo &info) {
	// we only add correlated columns to the list if they are not already there
	if (std::find(correlated_columns.begin(), correlated_columns.end(), info) == correlated_columns.end()) {
		correlated_columns.AddColumn(info);
	}
}

optional_ptr<Binding> Binder::GetMatchingBinding(const Identifier &table_name, const Identifier &column_name,
                                                 ErrorData &error) {
	Identifier empty_schema;
	return GetMatchingBinding(empty_schema, table_name, column_name, error);
}

optional_ptr<Binding> Binder::GetMatchingBinding(const Identifier &schema_name, const Identifier &table_name,
                                                 const Identifier &column_name, ErrorData &error) {
	Identifier empty_catalog;
	return GetMatchingBinding(empty_catalog, schema_name, table_name, column_name, error);
}

optional_ptr<Binding> Binder::GetMatchingBinding(const Identifier &catalog_name, const Identifier &schema_name,
                                                 const Identifier &table_name, const Identifier &column_name,
                                                 ErrorData &error) {
	BindingAlias alias(catalog_name, schema_name, table_name);
	return GetMatchingBinding(alias, column_name, error);
}

optional_ptr<Binding> Binder::GetMatchingBinding(const BindingAlias &alias, const Identifier &column_name,
                                                 ErrorData &error) {
	if (macro_binding && alias.GetSchemaPath().empty() && alias.GetCatalog().empty() &&
	    alias.GetAlias() == macro_binding->GetAlias()) {
		return optional_ptr<Binding>(macro_binding.get());
	}
	return bind_context.GetBinding(alias, column_name, error);
}

void Binder::SetBindingMode(BindingMode mode) {
	global_binder_state->mode = mode;
}

BindingMode Binder::GetBindingMode() {
	return global_binder_state->mode;
}

void Binder::SetCanContainNulls(bool can_contain_nulls_p) {
	legacy_can_contain_nulls = can_contain_nulls_p;
}

bool Binder::CanContainNulls() const {
	if (!Settings::Get<LegacyDisableNullTypeSetting>(context)) {
		// if this legacy setting is not enabled nulls are not special - they can just occur anywhere
		return true;
	}
	return legacy_can_contain_nulls;
}

void Binder::SetAlwaysRequireRebind() {
	auto &properties = GetStatementProperties();
	properties.always_require_rebind = true;
}

void Binder::AddTableName(string table_name) {
	global_binder_state->table_names.insert(std::move(table_name));
}

void Binder::AddReplacementScan(const Identifier &table_name, unique_ptr<TableRef> replacement) {
	auto it = global_binder_state->replacement_scans.find(table_name);
	replacement->column_name_alias.clear();
	replacement->alias.clear();
	if (it == global_binder_state->replacement_scans.end()) {
		global_binder_state->replacement_scans[table_name] = std::move(replacement);
	} else {
		// A replacement scan by this name was previously registered, we can just use it
	}
}

const unordered_set<string> &Binder::GetTableNames() {
	return global_binder_state->table_names;
}

identifier_map_t<unique_ptr<TableRef>> &Binder::GetReplacementScans() {
	return global_binder_state->replacement_scans;
}

// FIXME: this is extremely naive
void VerifyNotExcluded(const ParsedExpression &root_expr) {
	ParsedExpressionIterator::VisitExpression<ColumnRefExpression>(
	    root_expr, [&](const ColumnRefExpression &column_ref) {
		    if (!column_ref.IsQualified()) {
			    return;
		    }
		    // the table qualifier is the component directly before the column name
		    auto &names = column_ref.ColumnNames();
		    if (names[names.size() - 2] == "excluded") {
			    throw NotImplementedException(
			        "'excluded' qualified columns are not supported in the RETURNING clause yet");
		    }
	    });
}

void Binder::BindDeleteReturningColumns(TableCatalogEntry &table, LogicalGet &get, vector<idx_t> &return_columns) {
	// Build a mapping from storage column index to scan chunk index for RETURNING.
	// This allows PhysicalDelete to pass columns through from the scan instead of
	// fetching them by row ID. Generated columns will be computed by the RETURNING projection.
	auto &column_ids = get.GetColumnIds();
	auto &columns = table.GetColumns();
	auto physical_count = columns.PhysicalColumnCount();

	// Initialize the mapping with INVALID_INDEX
	return_columns.resize(physical_count, DConstants::INVALID_INDEX);

	// First, map columns already in the scan to their storage indices
	for (idx_t chunk_idx = 0; chunk_idx < column_ids.size(); chunk_idx++) {
		auto &col_id = column_ids[chunk_idx];
		if (col_id.IsVirtualColumn()) {
			continue;
		}
		// Get the column by logical index, then get its storage index
		auto logical_idx = col_id.GetPrimaryIndex();
		auto &col = columns.GetColumn(LogicalIndex(logical_idx));
		if (!col.Generated()) {
			auto storage_idx = col.StorageOid();
			return_columns[storage_idx] = chunk_idx;
		}
	}

	// Add any missing physical columns to the scan
	for (auto &col : columns.Physical()) {
		auto storage_idx = col.StorageOid();
		if (return_columns[storage_idx] == DConstants::INVALID_INDEX) {
			return_columns[storage_idx] = column_ids.size();
			get.AddColumnId(col.Logical().index);
		}
	}
}

//! Helper: convert scan column mapping to expression mapping
static void ConvertScanToProjectionMapping(TableCatalogEntry &table, const vector<idx_t> &scan_return_columns,
                                           vector<idx_t> &return_columns,
                                           vector<unique_ptr<Expression>> &projection_expressions,
                                           LogicalOperator &target_binding) {
	target_binding.ResolveOperatorTypes();
	auto target_bindings = target_binding.GetColumnBindings();
	auto &target_types = target_binding.types;

	auto physical_count = table.GetColumns().PhysicalColumnCount();
	return_columns.resize(physical_count, DConstants::INVALID_INDEX);

	for (idx_t storage_idx = 0; storage_idx < scan_return_columns.size(); storage_idx++) {
		auto scan_idx = scan_return_columns[storage_idx];
		if (scan_idx != DConstants::INVALID_INDEX && scan_idx < target_bindings.size()) {
			return_columns[storage_idx] = projection_expressions.size();
			projection_expressions.push_back(
			    make_uniq<BoundColumnRefExpression>(target_types[scan_idx], target_bindings[scan_idx]));
		}
	}
}

void Binder::BindDeleteReturningColumns(TableCatalogEntry &table, LogicalGet &get, vector<idx_t> &return_columns,
                                        vector<unique_ptr<Expression>> &projection_expressions,
                                        LogicalOperator &target_binding) {
	vector<idx_t> scan_return_columns;
	BindDeleteReturningColumns(table, get, scan_return_columns);
	ConvertScanToProjectionMapping(table, scan_return_columns, return_columns, projection_expressions, target_binding);
}

void Binder::BindDeleteIndexColumns(TableCatalogEntry &table, LogicalGet &get, vector<idx_t> &return_columns) {
	// Build a mapping from storage column index to scan chunk index for unique index tracking.
	// This is a sparse mapping - only indexed columns have valid indices.
	// Used when DELETE has no RETURNING but table has unique indexes.
	auto &storage = table.GetStorage();
	auto &info = storage.GetDataTableInfo();
	auto &indexes = info->GetIndexes();

	// Collect column IDs from unique indexes
	auto indexed_column_ids = indexes.GetUniqueIndexColumns();

	if (indexed_column_ids.empty()) {
		return;
	}

	auto &column_ids = get.GetColumnIds();
	auto &columns = table.GetColumns();
	auto physical_count = columns.PhysicalColumnCount();

	// Initialize the mapping with INVALID_INDEX
	return_columns.resize(physical_count, DConstants::INVALID_INDEX);

	// First, map columns already in the scan to their storage indices
	for (idx_t chunk_idx = 0; chunk_idx < column_ids.size(); chunk_idx++) {
		auto &col_id = column_ids[chunk_idx];
		if (col_id.IsVirtualColumn()) {
			continue;
		}
		auto logical_idx = col_id.GetPrimaryIndex();
		auto &col = columns.GetColumn(LogicalIndex(logical_idx));
		if (!col.Generated()) {
			auto storage_idx = col.StorageOid();
			// Only map if this column is in a unique index
			if (indexed_column_ids.count(storage_idx)) {
				return_columns[storage_idx] = chunk_idx;
			}
		}
	}

	// Add any missing indexed columns to the scan
	for (auto col_idx : indexed_column_ids) {
		if (return_columns[col_idx] == DConstants::INVALID_INDEX) {
			return_columns[col_idx] = column_ids.size();
			// Find the logical index for this storage index
			for (auto &col : columns.Physical()) {
				if (col.StorageOid() == col_idx) {
					get.AddColumnId(col.Logical().index);
					break;
				}
			}
		}
	}
}

void Binder::BindDeleteIndexColumns(TableCatalogEntry &table, LogicalGet &get, vector<idx_t> &return_columns,
                                    vector<unique_ptr<Expression>> &projection_expressions,
                                    LogicalOperator &target_binding) {
	vector<idx_t> scan_return_columns;
	BindDeleteIndexColumns(table, get, scan_return_columns);
	if (!scan_return_columns.empty()) {
		ConvertScanToProjectionMapping(table, scan_return_columns, return_columns, projection_expressions,
		                               target_binding);
	}
}

BoundStatement Binder::BindReturning(vector<unique_ptr<ParsedExpression>> returning_list, TableCatalogEntry &table,
                                     const Identifier &alias, TableIndex update_table_index,
                                     unique_ptr<LogicalOperator> child_operator, virtual_column_map_t virtual_columns) {
	vector<LogicalType> types;
	vector<Identifier> names;

	auto binder = Binder::CreateBinder(context);

	vector<ColumnIndex> bound_columns;
	idx_t column_count = 0;
	for (auto &col : table.GetColumns().Logical()) {
		names.emplace_back(col.Name());
		types.push_back(col.Type());
		if (!col.Generated()) {
			bound_columns.emplace_back(column_count);
		}
		column_count++;
	}

	binder->bind_context.AddBaseTable(update_table_index, alias, names, types, bound_columns, table,
	                                  std::move(virtual_columns));
	ReturningBinder returning_binder(*binder, context);

	vector<unique_ptr<Expression>> projection_expressions;
	LogicalType result_type;
	vector<unique_ptr<ParsedExpression>> new_returning_list;
	BoundStatement result;
	binder->ExpandStarExpressions(returning_list, new_returning_list);
	for (auto &returning_expr : new_returning_list) {
		VerifyNotExcluded(*returning_expr);
		auto expr = returning_binder.Bind(returning_expr, &result_type);
		result.names.push_back(expr->GetName());
		result.types.push_back(result_type);
		projection_expressions.push_back(std::move(expr));
	}
	if (new_returning_list.empty()) {
		throw BinderException("RETURNING list is empty!");
	}
	auto projection = make_uniq<LogicalProjection>(GenerateTableIndex(), std::move(projection_expressions));
	projection->AddChild(std::move(child_operator));
	D_ASSERT(result.types.size() == result.names.size());
	result.plan = std::move(projection);
	auto &properties = GetStatementProperties();
	properties.return_type = StatementReturnType::QUERY_RESULT;
	return result;
}

optional_ptr<CatalogEntry> Binder::GetCatalogEntry(const Identifier &catalog, const Identifier &schema,
                                                   const EntryLookupInfo &lookup_info,
                                                   OnEntryNotFound on_entry_not_found) {
	return entry_retriever.GetEntry(
	    EntryLookupInfo(lookup_info, QualifiedName(catalog, schema, lookup_info.GetEntryIdentifier())),
	    on_entry_not_found);
}

optional_ptr<CatalogEntry> Binder::GetCatalogEntry(const EntryLookupInfo &lookup_info,
                                                   OnEntryNotFound on_entry_not_found) {
	return entry_retriever.GetEntry(lookup_info, on_entry_not_found);
}

//! Create a binder whose catalog search path is anchored to the table's catalog+schema
shared_ptr<Binder> Binder::CreateBinderWithSearchPath(const Identifier &catalog_name,
                                                      const vector<Identifier> &schema_path) {
	shared_ptr<Binder> new_binder = Binder::CreateBinder(context, this);

	vector<CatalogSearchEntry> search_path;

	search_path.push_back(CatalogSearchEntry::FromSchemaPath(catalog_name, schema_path));
	if (schema_path.size() != 1 || schema_path[0] != DEFAULT_SCHEMA) {
		search_path.emplace_back(catalog_name, DEFAULT_SCHEMA);
	}
	new_binder->entry_retriever.SetSearchPath(std::move(search_path));
	return new_binder;
}

} // namespace duckdb
