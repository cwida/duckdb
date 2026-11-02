//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/table_binding.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/parser/column_definition.hpp"
#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/catalog/catalog_entry/table_column_type.hpp"
#include "duckdb/planner/binding_alias.hpp"
#include "duckdb/common/column_index.hpp"
#include "duckdb/common/table_column.hpp"
#include "duckdb/planner/bound_statement.hpp"

namespace duckdb {
class BindContext;
class BoundQueryNode;
class ColumnRefExpression;
class SubqueryRef;
class LogicalGet;
class TableCatalogEntry;
class TableFunctionCatalogEntry;
class StandardEntry;
struct ColumnBinding;

enum class BindingType { BASE, TABLE, DUMMY, CATALOG_ENTRY, CTE };

//! A Binding represents a binding to a table, table-producing function or subquery with a specified table index.
struct Binding {
	Binding(BindingType binding_type, BindingAlias alias, vector<LogicalType> types, vector<Identifier> names,
	        TableIndex index);
	virtual ~Binding() = default;

public:
	bool TryGetBindingIndex(const Identifier &column_name, column_t &column_index);
	column_t GetBindingIndex(const Identifier &column_name);
	bool HasMatchingBinding(const Identifier &column_name);
	//! Register an alternative name for an existing column - the alias can be bound, but is hidden from *
	void AddColumnAlias(const Identifier &column_alias, column_t column_index);
	//! Returns the name under which a column is registered in this binding (this can differ from the provided name
	//! because of case insensitivity, or because the column is referenced through a column alias)
	const Identifier &GetRegisteredColumnName(const Identifier &column_name);
	virtual ErrorData ColumnNotFoundError(const Identifier &column_name) const;
	virtual BindResult Bind(ColumnRefExpression &colref, idx_t depth);
	virtual optional_ptr<StandardEntry> GetStandardEntry();
	const Identifier &GetAlias() const;

	BindingType GetBindingType();
	const BindingAlias &GetBindingAlias();
	TableIndex GetIndex();
	const vector<LogicalType> &GetColumnTypes();
	const vector<Identifier> &GetColumnNames();
	idx_t GetColumnCount();
	void SetColumnType(idx_t col_idx, LogicalType type);
	//! Marks this binding as the NULL-producing side of an outer join
	void SetNullExtended();
	bool IsNullExtended() const;

	static BindingAlias GetAlias(const Identifier &explicit_alias, const StandardEntry &entry);
	static BindingAlias GetAlias(const Identifier &explicit_alias, optional_ptr<StandardEntry> entry);

public:
	template <class TARGET>
	TARGET &Cast() {
		if (binding_type != TARGET::TYPE) {
			InternalException::Throw("Failed to cast binding to type - binding type mismatch");
		}
		return reinterpret_cast<TARGET &>(*this);
	}

	template <class TARGET>
	const TARGET &Cast() const {
		if (binding_type != TARGET::TYPE) {
			InternalException::Throw("Failed to cast binding to type - binding type mismatch");
		}
		return reinterpret_cast<const TARGET &>(*this);
	}

protected:
	void Initialize();
	//! Set the alias of the column reference to the name under which the column is registered in this binding
	void SetBoundColumnAlias(ColumnRefExpression &colref);

protected:
	//! The type of Binding
	BindingType binding_type;
	//! The alias of the binding
	BindingAlias alias;
	//! The table index of the binding
	TableIndex index;
	//! The types of the bound columns
	vector<LogicalType> types;
	//! Column names of the subquery
	vector<Identifier> names;
	//! Name -> index for the names
	identifier_map_t<column_t> name_map;
	//! Whether rows of this binding can be NULL-extended by an outer join
	bool null_extended = false;
};

struct EntryBinding : public Binding {
public:
	static constexpr const BindingType TYPE = BindingType::CATALOG_ENTRY;

public:
	EntryBinding(const Identifier &alias, vector<LogicalType> types, vector<Identifier> names, TableIndex index,
	             StandardEntry &entry);
	StandardEntry &entry;

public:
	optional_ptr<StandardEntry> GetStandardEntry() override;
};

//! TableBinding is exactly like the Binding, except it keeps track of which columns were bound in the linked LogicalGet
//! node for projection pushdown purposes.
struct TableBinding : public Binding {
public:
	static constexpr const BindingType TYPE = BindingType::TABLE;

public:
	TableBinding(const Identifier &alias, vector<LogicalType> types, vector<Identifier> names,
	             vector<ColumnIndex> &bound_column_ids, optional_ptr<StandardEntry> entry, TableIndex index,
	             virtual_column_map_t virtual_columns);

	//! A reference to the set of bound column ids
	vector<ColumnIndex> &bound_column_ids;
	//! The underlying catalog entry (if any)
	optional_ptr<StandardEntry> entry;
	//! Virtual columns
	virtual_column_map_t virtual_columns;

public:
	unique_ptr<ParsedExpression> ExpandGeneratedColumn(const Identifier &column_name);
	BindResult Bind(ColumnRefExpression &colref, idx_t depth) override;
	optional_ptr<StandardEntry> GetStandardEntry() override;
	ErrorData ColumnNotFoundError(const Identifier &column_name) const override;
	// These are columns that are present in the name_map, appearing in the order that they're bound
	const vector<ColumnIndex> &GetBoundColumnIds() const;

protected:
	ColumnBinding GetColumnBinding(column_t column_index);
};

//! DummyBinding is like the Binding, except the alias and index are set by default.
//! Used for binding lambdas and macro parameters.
struct DummyBinding : public Binding {
public:
	static constexpr const BindingType TYPE = BindingType::DUMMY;
	// NOTE: changing this string conflicts with the storage version
	static constexpr const char *DUMMY_NAME = "0_macro_parameters";

public:
	DummyBinding(vector<LogicalType> types, vector<Identifier> names, string dummy_name);

	//! Arguments (for macros)
	vector<unique_ptr<ParsedExpression>> *arguments;
	//! The name of the dummy binding
	Identifier dummy_name;

public:
	//! Binding macros
	BindResult Bind(ColumnRefExpression &col_ref, idx_t depth) override;
	//! Binding lambdas
	BindResult Bind(LambdaRefExpression &lambda_ref, idx_t depth);

	//! Returns a copy of the col_ref parameter as a parsed expression
	unique_ptr<ParsedExpression> ParamToArg(ColumnRefExpression &col_ref);
};

enum class CTEType { CAN_BE_REFERENCED, CANNOT_BE_REFERENCED };
struct CTEBinding;

struct CTEBindState {
	CTEBindState(Binder &parent_binder, QueryNode &cte_def, const vector<Identifier> &aliases);
	~CTEBindState();

	Binder &parent_binder;
	QueryNode &cte_def;
	const vector<Identifier> &aliases;
	idx_t active_binder_count;
	shared_ptr<Binder> query_binder;
	BoundStatement query;
	vector<Identifier> names;
	vector<LogicalType> types;

public:
	bool IsBound() const;
	void Bind(CTEBinding &binding);
};

struct CTEBinding : public Binding {
public:
	static constexpr const BindingType TYPE = BindingType::CTE;

public:
	CTEBinding(BindingAlias alias, vector<LogicalType> types, vector<Identifier> names, TableIndex index, CTEType type);
	CTEBinding(BindingAlias alias, shared_ptr<CTEBindState> bind_state, TableIndex index);

public:
	bool CanBeReferenced() const;
	bool IsReferenced() const;
	void Reference();

private:
	CTEType cte_type;
	idx_t reference_count;
	shared_ptr<CTEBindState> bind_state;
};

} // namespace duckdb
