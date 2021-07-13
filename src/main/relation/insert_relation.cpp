#include "duckdb/main/relation/insert_relation.hpp"
#include "duckdb/parser/statement/insert_statement.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

InsertRelation::InsertRelation(shared_ptr<Relation> child_p, string schema_name, string table_name)
    : Relation(child_p->context, RelationType::INSERT_RELATION), child(move(child_p)), schema_name(move(schema_name)),
      table_name(move(table_name)) {
	context.TryBindRelation(*this, this->columns);
}

BoundStatement InsertRelation::Bind(Binder &binder) {
	InsertStatement stmt;
	auto select = make_unique<SelectStatement>();
	select->node = child->GetQueryNode();

	stmt.schema = schema_name;
	stmt.table = table_name;
	stmt.select_statement = move(select);
	return binder.Bind((SQLStatement &)stmt);
}

const vector<ColumnDefinition> &InsertRelation::Columns() {
	return columns;
}

string InsertRelation::ToString(idx_t depth) {
	string str = RenderWhitespace(depth) + "Insert\n";
	return str + child->ToString(depth + 1);
}

} // namespace duckdb
