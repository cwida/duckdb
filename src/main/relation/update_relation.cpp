#include "duckdb/main/relation/update_relation.hpp"
#include "duckdb/parser/statement/update_statement.hpp"
#include "duckdb/parser/query_node/update_query_node.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/common/sql_identifier.hpp"

namespace duckdb {

UpdateRelation::UpdateRelation(shared_ptr<ClientContextWrapper> &context, unique_ptr<ParsedExpression> condition_p,
                               Identifier catalog_name_p, Identifier schema_name_p, Identifier table_name_p,
                               vector<Identifier> update_columns_p, vector<unique_ptr<ParsedExpression>> expressions_p)
    : Relation(context, RelationType::UPDATE_RELATION), condition(std::move(condition_p)),
      catalog_name(std::move(catalog_name_p)), schema_name(std::move(schema_name_p)),
      table_name(std::move(table_name_p)), update_columns(std::move(update_columns_p)),
      expressions(std::move(expressions_p)) {
	D_ASSERT(update_columns.size() == expressions.size());
	TryBindRelation(columns);
}

BoundStatement UpdateRelation::Bind(Binder &binder) {
	auto basetable = make_uniq<BaseTableRef>();
	basetable->SetQualifiedName(QualifiedName(catalog_name, schema_name, table_name));

	UpdateStatement stmt;
	auto &node = *stmt.node;

	node.set_info = make_uniq<UpdateSetInfo>();
	node.set_info->condition = condition ? condition->Copy() : nullptr;
	node.table = std::move(basetable);
	node.set_info->columns = update_columns;
	for (auto &expr : expressions) {
		node.set_info->expressions.push_back(expr->Copy());
	}
	return binder.Bind(stmt.Cast<SQLStatement>());
}

unique_ptr<QueryNode> UpdateRelation::GetQueryNode() {
	throw InternalException("Cannot create a query node from an update relation");
}

string UpdateRelation::GetQuery() {
	auto name = QualifiedName(catalog_name, schema_name, table_name);
	string result = "UPDATE " + name.ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA) + " SET ";
	for (idx_t i = 0; i < expressions.size(); i++) {
		if (i > 0) {
			result += ", ";
		}
		result += SQLIdentifier(update_columns[i]) + " = " + expressions[i]->ToString();
	}
	if (condition) {
		result += " WHERE " + condition->ToString();
	}
	return result;
}

const vector<ColumnDefinition> &UpdateRelation::Columns() {
	return columns;
}

string UpdateRelation::ToString(idx_t depth) {
	return RenderWhitespace(depth) + GetQuery();
}

} // namespace duckdb
