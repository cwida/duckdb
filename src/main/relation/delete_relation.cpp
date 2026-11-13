#include "duckdb/main/relation/delete_relation.hpp"
#include "duckdb/parser/statement/delete_statement.hpp"
#include "duckdb/parser/query_node/delete_query_node.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"

namespace duckdb {

DeleteRelation::DeleteRelation(shared_ptr<ClientContextWrapper> &context, unique_ptr<ParsedExpression> condition_p,
                               Identifier catalog_name_p, Identifier schema_name_p, Identifier table_name_p)
    : Relation(context, RelationType::DELETE_RELATION), condition(std::move(condition_p)),
      catalog_name(std::move(catalog_name_p)), schema_name(std::move(schema_name_p)),
      table_name(std::move(table_name_p)) {
	TryBindRelation(columns);
}

BoundStatement DeleteRelation::Bind(Binder &binder) {
	auto basetable = make_uniq<BaseTableRef>();
	basetable->SetQualifiedName(QualifiedName(catalog_name, schema_name, table_name));

	DeleteStatement stmt;
	auto &node = *stmt.node;
	node.condition = condition ? condition->Copy() : nullptr;
	node.table = std::move(basetable);
	return binder.Bind(stmt.Cast<SQLStatement>());
}

unique_ptr<QueryNode> DeleteRelation::GetQueryNode() {
	throw InternalException("Cannot create a query node from a delete relation");
}

string DeleteRelation::GetQuery() {
	auto name = QualifiedName(catalog_name, schema_name, table_name);
	string result = "DELETE FROM " + name.ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	if (condition) {
		result += " WHERE " + condition->ToString();
	}
	return result;
}

const vector<ColumnDefinition> &DeleteRelation::Columns() {
	return columns;
}

string DeleteRelation::ToString(idx_t depth) {
	return RenderWhitespace(depth) + GetQuery();
}

} // namespace duckdb
