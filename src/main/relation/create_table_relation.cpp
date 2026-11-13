#include "duckdb/main/relation/create_table_relation.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/planner/binder.hpp"

namespace duckdb {

CreateTableRelation::CreateTableRelation(shared_ptr<Relation> child_p, Identifier schema_name, Identifier table_name,
                                         bool temporary_p, OnCreateConflict on_conflict)
    : Relation(child_p->context, RelationType::CREATE_TABLE_RELATION), child(std::move(child_p)),
      schema_name(std::move(schema_name)), table_name(std::move(table_name)), temporary(temporary_p),
      on_conflict(on_conflict) {
	TryBindRelation(columns);
}

CreateTableRelation::CreateTableRelation(shared_ptr<Relation> child_p, Identifier catalog_name, Identifier schema_name,
                                         Identifier table_name, bool temporary_p, OnCreateConflict on_conflict)
    : Relation(child_p->context, RelationType::CREATE_TABLE_RELATION), child(std::move(child_p)),
      catalog_name(std::move(catalog_name)), schema_name(std::move(schema_name)), table_name(std::move(table_name)),
      temporary(temporary_p), on_conflict(on_conflict) {
	TryBindRelation(columns);
}

BoundStatement CreateTableRelation::Bind(Binder &binder) {
	auto select = make_uniq<SelectStatement>();
	select->node = child->GetQueryNode();

	CreateStatement stmt;
	auto info = make_uniq<CreateTableInfo>();
	info->SetQualifiedName(QualifiedName(catalog_name, schema_name, table_name));
	info->query = std::move(select);
	info->on_conflict = on_conflict;
	info->temporary = temporary;
	stmt.info = std::move(info);
	return binder.Bind(stmt.Cast<SQLStatement>());
}

unique_ptr<QueryNode> CreateTableRelation::GetQueryNode() {
	throw InternalException("Cannot create a query node from a create table relation");
}

string CreateTableRelation::GetQuery() {
	// reuse the catalog's prefix rendering for OR REPLACE / TEMP / IF NOT EXISTS
	CreateTableInfo info;
	info.on_conflict = on_conflict;
	info.temporary = temporary;
	info.SetQualifiedName(QualifiedName(catalog_name, schema_name, table_name));
	return info.GetCreatePrefix("TABLE") + info.QualifiedNameToString() + " AS " + child->GetQuery();
}

const vector<ColumnDefinition> &CreateTableRelation::Columns() {
	return columns;
}

string CreateTableRelation::ToString(idx_t depth) {
	string str = RenderWhitespace(depth) + "Create Table\n";
	return str + child->ToString(depth + 1);
}

} // namespace duckdb
