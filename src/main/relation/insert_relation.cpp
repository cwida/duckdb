#include "duckdb/main/relation/insert_relation.hpp"
#include "duckdb/parser/statement/insert_statement.hpp"
#include "duckdb/parser/query_node/insert_query_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

InsertRelation::InsertRelation(shared_ptr<Relation> child_p, Identifier schema_name, Identifier table_name)
    : Relation(child_p->context, RelationType::INSERT_RELATION), child(std::move(child_p)),
      schema_name(std::move(schema_name)), table_name(std::move(table_name)) {
	TryBindRelation(columns);
}

InsertRelation::InsertRelation(shared_ptr<Relation> child_p, Identifier catalog_name, Identifier schema_name,
                               Identifier table_name)
    : Relation(child_p->context, RelationType::INSERT_RELATION), child(std::move(child_p)),
      catalog_name(std::move(catalog_name)), schema_name(std::move(schema_name)), table_name(std::move(table_name)) {
	TryBindRelation(columns);
}

BoundStatement InsertRelation::Bind(Binder &binder) {
	InsertStatement stmt;
	auto &node = *stmt.node;
	auto select = make_uniq<SelectStatement>();
	select->node = child->GetQueryNode();

	node.qualified_name = QualifiedName(catalog_name, schema_name, table_name);
	node.select_statement = std::move(select);
	return binder.Bind(stmt.Cast<SQLStatement>());
}

unique_ptr<QueryNode> InsertRelation::GetQueryNode() {
	throw InternalException("Cannot create a query node from an insert relation");
}

string InsertRelation::GetQuery() {
	auto name = QualifiedName(catalog_name, schema_name, table_name);
	return "INSERT INTO " + name.ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA) + " " + child->GetQuery();
}

const vector<ColumnDefinition> &InsertRelation::Columns() {
	return columns;
}

string InsertRelation::ToString(idx_t depth) {
	string str = RenderWhitespace(depth) + "Insert\n";
	return str + child->ToString(depth + 1);
}

} // namespace duckdb
