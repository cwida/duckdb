//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/bind_context.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/identifier.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/column_index.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/parser/qualified_name_set.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/planner/table_binding.hpp"

namespace duckdb {
class Binder;
class LogicalGet;
struct BoundStatement;

class StarExpression;

class TableCatalogEntry;
class TableFunctionCatalogEntry;

struct UsingColumnSet {
	BindingAlias primary_binding;
	vector<BindingAlias> bindings;
};

enum class ColumnBindType { EXPAND_GENERATED_COLUMNS, DO_NOT_EXPAND_GENERATED_COLUMNS };

//! What to do when aliasing a set of column names produces the same name twice
enum class DuplicateColumnNames { RENAME, ALLOW };

//! The BindContext object keeps track of all the tables and columns that are
//! encountered during the binding process.
class BindContext {
public:
	explicit BindContext(Binder &binder);

public:
	//! Given a column name, find the matching table it belongs to. Throws an
	//! exception if no table has a column of the given name.
	optional_ptr<Binding> GetMatchingBinding(const Identifier &column_name,
	                                         QueryErrorContext context = QueryErrorContext());
	//! Like GetMatchingBinding, but instead of throwing an error if multiple tables have the same binding it will
	//! return a list of all the matching ones
	vector<reference<Binding>> GetMatchingBindings(const Identifier &column_name);
	//! Like GetMatchingBindings, but returns the top 3 most similar bindings (in levenshtein distance) instead of the
	//! matching ones
	vector<Identifier> GetSimilarBindings(const Identifier &column_name);

	optional_ptr<CTEBinding> GetCTEBinding(const BindingAlias &ctename);
	//! Binds a column expression to the base table. Returns the bound expression
	//! or throws an exception if the column could not be bound.
	BindResult BindColumn(ColumnRefExpression &colref, idx_t depth);
	string BindColumn(PositionalReferenceExpression &ref, Identifier &table_name, Identifier &column_name,
	                  optional_idx &column_index);
	unique_ptr<ColumnRefExpression> PositionToColumn(PositionalReferenceExpression &ref);

	unique_ptr<ParsedExpression> ExpandGeneratedColumn(TableBinding &table_binding, column_t column_index,
	                                                   const Identifier &column_name);

	unique_ptr<ParsedExpression>
	CreateColumnReference(const Identifier &table_name, const Identifier &column_name,
	                      ColumnBindType bind_type = ColumnBindType::EXPAND_GENERATED_COLUMNS);
	unique_ptr<ParsedExpression>
	CreateColumnReference(const Identifier &schema_name, const Identifier &table_name, const Identifier &column_name,
	                      ColumnBindType bind_type = ColumnBindType::EXPAND_GENERATED_COLUMNS);
	unique_ptr<ParsedExpression>
	CreateColumnReference(const Identifier &catalog_name, const Identifier &schema_name, const Identifier &table_name,
	                      const Identifier &column_name,
	                      ColumnBindType bind_type = ColumnBindType::EXPAND_GENERATED_COLUMNS);
	unique_ptr<ParsedExpression>
	CreateColumnReference(const BindingAlias &table_alias, const Identifier &column_name,
	                      ColumnBindType bind_type = ColumnBindType::EXPAND_GENERATED_COLUMNS,
	                      optional_idx resolved_index = optional_idx());

	//! Generate column expressions for all columns that are present in the
	//! referenced tables. This is used to resolve the * expression in a
	//! selection list.
	void GenerateAllColumnExpressions(StarExpression &expr, vector<unique_ptr<ParsedExpression>> &new_select_list);

	const vector<unique_ptr<Binding>> &GetBindingsList() {
		return bindings_list;
	}
	vector<BindingAlias> GetBindingAliases();

	void GetTypesAndNames(vector<Identifier> &result_names, vector<LogicalType> &result_types);

	//! Adds a base table with the given alias to the BindContext.
	void AddBaseTable(TableIndex index, const Identifier &alias, const vector<Identifier> &names,
	                  const vector<LogicalType> &types, vector<ColumnIndex> &bound_column_ids, TableCatalogEntry &entry,
	                  bool add_row_id = true);
	void AddBaseTable(TableIndex index, const Identifier &alias, const vector<Identifier> &names,
	                  const vector<LogicalType> &types, vector<ColumnIndex> &bound_column_ids,
	                  const Identifier &table_name);
	void AddBaseTable(TableIndex index, const Identifier &alias, const vector<Identifier> &names,
	                  const vector<LogicalType> &types, vector<ColumnIndex> &bound_column_ids, TableCatalogEntry &entry,
	                  virtual_column_map_t virtual_columns);
	//! Adds a call to a table function with the given alias to the BindContext.
	void AddTableFunction(TableIndex index, const Identifier &alias, const vector<Identifier> &names,
	                      const vector<LogicalType> &types, vector<ColumnIndex> &bound_column_ids,
	                      optional_ptr<StandardEntry> entry, virtual_column_map_t virtual_columns);
	//! Adds a table view with a given alias to the BindContext.
	void AddView(TableIndex index, const Identifier &alias, SubqueryRef &ref, BoundStatement &subquery,
	             ViewCatalogEntry &view);
	//! Adds a subquery with a given alias to the BindContext.
	void AddSubquery(TableIndex index, const Identifier &alias, SubqueryRef &ref, BoundStatement &subquery);
	//! Adds a subquery with a given alias to the BindContext.
	void AddSubquery(TableIndex index, const Identifier &alias, TableFunctionRef &ref, BoundStatement &subquery);
	//! Adds a binding to a catalog entry with a given alias to the BindContext.
	void AddEntryBinding(TableIndex index, const Identifier &alias, const vector<Identifier> &names,
	                     const vector<LogicalType> &types, StandardEntry &entry);
	//! Adds a base table with the given alias to the BindContext.
	void AddGenericBinding(TableIndex index, const Identifier &alias, const vector<Identifier> &names,
	                       const vector<LogicalType> &types);

	//! Registers an alternative name for a column of the binding with the given index
	//! The alias can be bound like a regular column, but is not emitted by *
	void AddColumnAlias(TableIndex index, const Identifier &column_alias, column_t column_index);

	//! Adds a base table with the given alias to the CTE BindContext.
	//! We need this to correctly bind recursive CTEs with multiple references.
	void AddCTEBinding(TableIndex index, BindingAlias alias, const vector<Identifier> &names,
	                   const vector<LogicalType> &types, CTEType cte_type = CTEType::CAN_BE_REFERENCED);
	void AddCTEBinding(unique_ptr<CTEBinding> binding);

	//! Add an implicit join condition (e.g. USING (x))
	void AddUsingBinding(const Identifier &column_name, UsingColumnSet &set);

	void AddUsingBindingSet(unique_ptr<UsingColumnSet> set);

	//! Returns any using column set for the given column name, or nullptr if there is none. On conflict (multiple using
	//! column sets with the same name) throw an exception.
	optional_ptr<UsingColumnSet> GetUsingBinding(const Identifier &column_name);
	//! Returns any using column set for the given column name, or nullptr if there is none
	optional_ptr<UsingColumnSet> GetUsingBinding(const Identifier &column_name, const BindingAlias &binding);
	//! Erase a using binding from the set of using bindings
	void RemoveUsingBinding(const Identifier &column_name, UsingColumnSet &set);
	//! Transfer a using binding from one bind context to this bind context
	void TransferUsingBinding(BindContext &current_context, optional_ptr<UsingColumnSet> current_set,
	                          UsingColumnSet &new_set, const Identifier &using_column);

	//! Fetch the actual column name from the given binding, or throws if none exists
	//! This can be different from "column_name" because of case insensitivity
	//! (e.g. "column_name" might return "COLUMN_NAME")
	string GetActualColumnName(const BindingAlias &binding_alias, const Identifier &column_name);
	string GetActualColumnName(Binding &binding, const Identifier &column_name);

	//! Alias a set of column names for the specified table, using the original names if there are not enough aliases
	//! specified.
	static vector<Identifier> AliasColumnNames(const Identifier &table_name, const vector<Identifier> &names,
	                                           const vector<Identifier> &column_aliases,
	                                           DuplicateColumnNames duplicates = DuplicateColumnNames::RENAME);

	//! Add all the bindings from a BindContext to this BindContext. The other BindContext is destroyed in the process.
	void AddContext(BindContext other);
	//! For semi and anti joins we remove the binding context of the right table after binding the condition.
	void RemoveContext(const vector<BindingAlias> &aliases);

	//! Gets a binding of the specified name. Returns a nullptr and sets the out_error if the binding could not be
	//! found.
	optional_ptr<Binding> GetBinding(const Identifier &name, ErrorData &out_error);

	optional_ptr<Binding> GetBinding(const BindingAlias &alias, ErrorData &out_error);

	optional_ptr<Binding> GetBinding(const BindingAlias &alias, const Identifier &column_name, ErrorData &out_error);

	//! Get all bindings that match a specific binding alias - returns an error if none match
	vector<reference<Binding>> GetBindings(const BindingAlias &alias, ErrorData &out_error);

private:
	void AddBinding(unique_ptr<Binding> binding);
	static string AmbiguityException(const BindingAlias &alias, const vector<reference<Binding>> &bindings);

private:
	Binder &binder;
	//! The list of bindings in insertion order
	vector<unique_ptr<Binding>> bindings_list;
	//! The set of columns used in USING join conditions
	identifier_map_t<reference_set_t<UsingColumnSet>> using_columns;
	//! The set of CTE bindings
	vector<unique_ptr<CTEBinding>> cte_bindings;
};
} // namespace duckdb
