#include "duckdb/planner/table_binding.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/planner/bind_context.hpp"
#include "duckdb/planner/bound_query_node.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_lambdaref_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"

#include <algorithm>

namespace duckdb {

Binding::Binding(BindingType binding_type, BindingAlias alias_p, vector<LogicalType> coltypes,
                 vector<Identifier> colnames, TableIndex index)
    : binding_type(binding_type), alias(std::move(alias_p)), index(index), types(std::move(coltypes)) {
	names.reserve(colnames.size());
	for (auto &colname : colnames) {
		names.emplace_back(std::move(colname));
	}
	Initialize();
}

void Binding::Initialize() {
	D_ASSERT(types.size() == names.size());
	for (idx_t i = 0; i < names.size(); i++) {
		auto &name = names[i];
		D_ASSERT(!name.empty());
		if (name_map.find(name) != name_map.end()) {
			throw BinderException("table %s has duplicate column name %s", alias.GetAlias(), name);
		}
		name_map[name] = i;
	}
}

BindingType Binding::GetBindingType() {
	return binding_type;
}

const BindingAlias &Binding::GetBindingAlias() {
	return alias;
}

TableIndex Binding::GetIndex() {
	return index;
}

const vector<LogicalType> &Binding::GetColumnTypes() {
	return types;
}

const vector<Identifier> &Binding::GetColumnNames() {
	return names;
}

idx_t Binding::GetColumnCount() {
	return GetColumnNames().size();
}

void Binding::SetColumnType(idx_t col_idx, LogicalType type_p) {
	types[col_idx] = std::move(type_p);
}

void Binding::SetNullExtended() {
	null_extended = true;
}

bool Binding::IsNullExtended() const {
	return null_extended;
}

const Identifier &Binding::GetAlias() const {
	return alias.GetAlias();
}

bool Binding::TryGetBindingIndex(const Identifier &column_name, column_t &result) {
	auto entry = name_map.find(column_name);
	if (entry == name_map.end()) {
		return false;
	}
	auto column_info = entry->second;
	result = column_info;
	return true;
}

column_t Binding::GetBindingIndex(const Identifier &column_name) {
	column_t result;
	if (!TryGetBindingIndex(column_name, result)) {
		throw InternalException("Binding index for column %s not found", column_name);
	}
	return result;
}

bool Binding::HasMatchingBinding(const Identifier &column_name) {
	column_t result;
	return TryGetBindingIndex(column_name, result);
}

void Binding::AddColumnAlias(const Identifier &column_alias, column_t column_index) {
	D_ASSERT(column_index < names.size());
	if (name_map.find(column_alias) != name_map.end()) {
		// a column with this name already exists - the alias is not required
		return;
	}
	name_map[column_alias] = column_index;
}

const Identifier &Binding::GetRegisteredColumnName(const Identifier &column_name) {
	auto entry = name_map.find(column_name);
	return entry == name_map.end() ? column_name : entry->first;
}

void Binding::SetBoundColumnAlias(ColumnRefExpression &colref) {
	if (!colref.GetAlias().empty()) {
		return;
	}
	colref.SetAlias(GetRegisteredColumnName(colref.GetColumnName()));
}

ErrorData Binding::ColumnNotFoundError(const Identifier &column_name) const {
	return ErrorData(ExceptionType::BINDER,
	                 StringUtil::Format("Values list %s does not have a column named %s", GetAlias(), column_name));
}

BindResult Binding::Bind(ColumnRefExpression &colref, idx_t depth) {
	column_t column_index;
	bool success = false;
	success = TryGetBindingIndex(colref.GetColumnName(), column_index);
	if (!success) {
		return BindResult(ColumnNotFoundError(colref.GetColumnName()));
	}
	ColumnBinding binding;
	binding.table_index = index;
	binding.column_index = ProjectionIndex(column_index);
	LogicalType sql_type = types[column_index];
	SetBoundColumnAlias(colref);
	return BindResult(make_uniq<BoundColumnRefExpression>(Identifier(colref.GetName()), sql_type, binding, depth));
}

optional_ptr<StandardEntry> Binding::GetStandardEntry() {
	return nullptr;
}

BindingAlias Binding::GetAlias(const Identifier &explicit_alias, const StandardEntry &entry) {
	if (!explicit_alias.empty()) {
		return BindingAlias(explicit_alias);
	}
	// no explicit alias provided - generate from entry
	return BindingAlias(entry);
}

BindingAlias Binding::GetAlias(const Identifier &explicit_alias, optional_ptr<StandardEntry> entry) {
	if (!explicit_alias.empty()) {
		return BindingAlias(explicit_alias);
	}
	if (!entry) {
		throw InternalException("Binding::GetAlias called - but neither an alias nor an entry was provided");
	}
	// no explicit alias provided - generate from entry
	return BindingAlias(*entry);
}

EntryBinding::EntryBinding(const Identifier &alias, vector<LogicalType> types_p, vector<Identifier> names_p,
                           TableIndex index, StandardEntry &entry)
    : Binding(BindingType::CATALOG_ENTRY, GetAlias(alias, entry), std::move(types_p), std::move(names_p), index),
      entry(entry) {
}

optional_ptr<StandardEntry> EntryBinding::GetStandardEntry() {
	return &entry;
}

TableBinding::TableBinding(const Identifier &alias, vector<LogicalType> types_p, vector<Identifier> names_p,
                           vector<ColumnIndex> &bound_column_ids, optional_ptr<StandardEntry> entry, TableIndex index,
                           virtual_column_map_t virtual_columns_p)
    : Binding(BindingType::TABLE, GetAlias(alias, entry), std::move(types_p), std::move(names_p), index),
      bound_column_ids(bound_column_ids), entry(entry), virtual_columns(std::move(virtual_columns_p)) {
	for (auto &ventry : virtual_columns) {
		auto idx = ventry.first;
		auto &name = ventry.second.name;
		if (idx < VIRTUAL_COLUMN_START) {
			throw BinderException(
			    "Virtual column index must be larger than VIRTUAL_COLUMN_START - found %d for column %s", idx, name);
		}
		if (idx == COLUMN_IDENTIFIER_EMPTY) {
			// the empty column cannot be queried by the user
			continue;
		}
		if (idx == COLUMN_IDENTIFIER_ROW_NUMBER) {
			// the row_number column cannot be queried by the user
			continue;
		}
		if (name_map.find(name) == name_map.end()) {
			name_map[name] = idx;
		}
	}
}

static void ReplaceAliases(ParsedExpression &root_expr, const ColumnList &list,
                           const unordered_map<idx_t, Identifier> &alias_map) {
	ParsedExpressionIterator::VisitExpressionMutable<ColumnRefExpression>(root_expr, [&](ColumnRefExpression &colref) {
		D_ASSERT(!colref.IsQualified());
		auto &col_names = colref.ColumnNamesMutable();
		D_ASSERT(col_names.size() == 1);
		auto idx_entry = list.GetColumnIndex(col_names[0]);
		auto &alias = alias_map.at(idx_entry.index);
		col_names = vector<Identifier> {alias};
	});
}

static void BakeTableName(ParsedExpression &root_expr, const BindingAlias &binding_alias) {
	ParsedExpressionIterator::VisitExpressionMutable<ColumnRefExpression>(root_expr, [&](ColumnRefExpression &colref) {
		D_ASSERT(!colref.IsQualified());
		auto &col_names = colref.ColumnNamesMutable();
		col_names.insert(col_names.begin(), binding_alias.GetAlias());
		if (!binding_alias.GetSchema().empty()) {
			col_names.insert(col_names.begin(), binding_alias.GetSchema());
		}
		if (!binding_alias.GetCatalog().empty()) {
			col_names.insert(col_names.begin(), binding_alias.GetCatalog());
		}
	});
}

unique_ptr<ParsedExpression> TableBinding::ExpandGeneratedColumn(const Identifier &column_name) {
	auto catalog_entry = GetStandardEntry();
	D_ASSERT(catalog_entry); // Should only be called on a TableBinding

	D_ASSERT(catalog_entry->type == CatalogType::TABLE_ENTRY);
	auto &table_entry = catalog_entry->Cast<TableCatalogEntry>();

	// Get the index of the generated column
	auto column_index = GetBindingIndex(column_name);
	D_ASSERT(table_entry.GetColumn(LogicalIndex(column_index)).Generated());
	// Get a copy of the generated column
	auto expression = table_entry.GetColumn(LogicalIndex(column_index)).GeneratedExpression().Copy();
	unordered_map<idx_t, Identifier> alias_map;
	for (auto &entry : name_map) {
		alias_map[entry.second] = entry.first;
	}
	ReplaceAliases(*expression, table_entry.GetColumns(), alias_map);
	BakeTableName(*expression, alias);
	return (expression);
}

const vector<ColumnIndex> &TableBinding::GetBoundColumnIds() const {
#ifdef DEBUG
	unordered_set<idx_t> column_ids;
	for (auto &col_id : bound_column_ids) {
		idx_t id = col_id.IsRowIdColumn() ? DConstants::INVALID_INDEX : col_id.GetPrimaryIndex();
		auto result = column_ids.insert(id);
		// assert that all entries in the bound_column_ids are unique
		D_ASSERT(result.second);
		auto it = std::find_if(name_map.begin(), name_map.end(),
		                       [&](const std::pair<const Identifier, idx_t> &it) { return it.second == id; });
		// assert that every id appears in the name_map
		D_ASSERT(it != name_map.end());
		// the order that they appear in is not guaranteed to be sequential
	}
#endif
	return bound_column_ids;
}

ColumnBinding TableBinding::GetColumnBinding(column_t column_index) {
	auto &column_ids = bound_column_ids;
	ColumnBinding binding;

	// Locate the column_id that matches the 'column_index'
	binding.column_index = ProjectionIndex(column_ids.size());
	for (idx_t i = 0; i < column_ids.size(); ++i) {
		auto &col_id = column_ids[i];
		if (col_id.GetPrimaryIndex() == column_index) {
			binding.column_index = ProjectionIndex(i);
			break;
		}
	}
	// If it wasn't found, add it
	if (binding.column_index == column_ids.size()) {
		column_ids.emplace_back(column_index);
	}

	binding.table_index = index;
	return binding;
}

BindResult TableBinding::Bind(ColumnRefExpression &colref, idx_t depth) {
	auto &column_name = colref.GetColumnName();
	column_t column_index;
	bool success = false;
	success = TryGetBindingIndex(column_name, column_index);
	if (!success) {
		return BindResult(ColumnNotFoundError(column_name));
	}
	auto entry = GetStandardEntry();
	if (entry && !IsVirtualColumn(column_index)) {
		D_ASSERT(entry->type == CatalogType::TABLE_ENTRY);
		// Either there is no table, or the columns category has to be standard
		auto &table_entry = entry->Cast<TableCatalogEntry>();
		auto &column_entry = table_entry.GetColumn(LogicalIndex(column_index));
		(void)table_entry;
		(void)column_entry;
		D_ASSERT(column_entry.Category() == TableColumnType::STANDARD);
	}
	// fetch the type of the column
	LogicalType col_type;
	auto ventry = virtual_columns.find(column_index);
	if (ventry != virtual_columns.end()) {
		// virtual column - fetch type from there
		col_type = ventry->second.type;
	} else {
		// normal column: fetch type from base column
		col_type = types[column_index];
		SetBoundColumnAlias(colref);
	}
	ColumnBinding binding = GetColumnBinding(column_index);
	return BindResult(make_uniq<BoundColumnRefExpression>(Identifier(colref.GetName()), col_type, binding, depth));
}

optional_ptr<StandardEntry> TableBinding::GetStandardEntry() {
	return entry;
}

ErrorData TableBinding::ColumnNotFoundError(const Identifier &column_name) const {
	auto candidate_message = StringUtil::CandidatesErrorMessage(
	    IdentifiersToStrings(names), column_name.GetIdentifierName(), "Candidate bindings: ");
	return ErrorData(ExceptionType::BINDER, StringUtil::Format("Table %s does not have a column named %s\n%s",
	                                                           alias.GetAlias(), column_name, candidate_message));
}

DummyBinding::DummyBinding(vector<LogicalType> types, vector<Identifier> names, string dummy_name)
    : Binding(BindingType::DUMMY, BindingAlias(Identifier(DummyBinding::DUMMY_NAME + dummy_name)), std::move(types),
              std::move(names), TableIndex()),
      dummy_name(std::move(dummy_name)) {
}

BindResult DummyBinding::Bind(ColumnRefExpression &colref, idx_t depth) {
	column_t column_index;
	if (!TryGetBindingIndex(colref.GetColumnName(), column_index)) {
		throw InternalException("Column %s not found in bindings", colref.GetColumnName());
	}
	ColumnBinding binding(index, ProjectionIndex(column_index));

	// we are binding a parameter to create the dummy binding, no arguments are supplied
	return BindResult(
	    make_uniq<BoundColumnRefExpression>(Identifier(colref.GetName()), types[column_index], binding, depth));
}

BindResult DummyBinding::Bind(LambdaRefExpression &lambdaref, idx_t depth) {
	column_t column_index;
	if (!TryGetBindingIndex(Identifier(lambdaref.GetName()), column_index)) {
		throw InternalException("Column %s not found in bindings", lambdaref.GetName());
	}
	ColumnBinding binding(index, ProjectionIndex(column_index));
	return BindResult(make_uniq<BoundLambdaRefExpression>(lambdaref.GetName(), types[column_index], binding,
	                                                      lambdaref.LambdaIndex(), depth));
}

unique_ptr<ParsedExpression> DummyBinding::ParamToArg(ColumnRefExpression &colref) {
	column_t column_index;
	if (!TryGetBindingIndex(colref.GetColumnName(), column_index)) {
		throw InternalException("Column %s not found in macro", colref.GetColumnName());
	}
	auto arg = (*arguments)[column_index]->Copy();
	arg->SetAlias(colref.GetAlias());
	return arg;
}

CTEBinding::CTEBinding(BindingAlias alias, vector<LogicalType> types, vector<Identifier> names, TableIndex index,
                       CTEType cte_type)
    : Binding(BindingType::CTE, std::move(alias), std::move(types), std::move(names), index), cte_type(cte_type),
      reference_count(0) {
}

CTEBinding::CTEBinding(BindingAlias alias_p, shared_ptr<CTEBindState> bind_state_p, TableIndex index)
    : Binding(BindingType::CTE, std::move(alias_p), vector<LogicalType>(), vector<Identifier>(), index),
      cte_type(CTEType::CAN_BE_REFERENCED), reference_count(0), bind_state(std::move(bind_state_p)) {
}

bool CTEBinding::CanBeReferenced() const {
	return cte_type == CTEType::CAN_BE_REFERENCED;
}

bool CTEBinding::IsReferenced() const {
	return reference_count > 0;
}

void CTEBinding::Reference() {
	if (!CanBeReferenced()) {
		throw InternalException("CTE cannot be referenced!");
	}
	if (bind_state) {
		// we have not bound the CTE yet - bind it
		bind_state->Bind(*this);

		// copy over the names / types and initialize the binding
		this->names = bind_state->names;
		this->types = bind_state->types;
		Initialize();

		// finalize binding
		bind_state.reset();
	}
	reference_count++;
}

} // namespace duckdb
