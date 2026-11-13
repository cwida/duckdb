#include "duckdb/main/relation/create_view_relation.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"
#include "duckdb/planner/binder.hpp"

namespace duckdb {

CreateViewRelation::CreateViewRelation(shared_ptr<Relation> child_p, Identifier view_name_p, bool replace_p,
                                       bool temporary_p)
    : Relation(child_p->context, RelationType::CREATE_VIEW_RELATION), child(std::move(child_p)),
      view_name(std::move(view_name_p)), replace(replace_p), temporary(temporary_p) {
	TryBindRelation(columns);
}

CreateViewRelation::CreateViewRelation(shared_ptr<Relation> child_p, Identifier schema_name_p, Identifier view_name_p,
                                       bool replace_p, bool temporary_p)
    : Relation(child_p->context, RelationType::CREATE_VIEW_RELATION), child(std::move(child_p)),
      schema_name(std::move(schema_name_p)), view_name(std::move(view_name_p)), replace(replace_p),
      temporary(temporary_p) {
	TryBindRelation(columns);
}

BoundStatement CreateViewRelation::Bind(Binder &binder) {
	auto select = make_uniq<SelectStatement>();
	select->node = child->GetQueryNode();

	CreateStatement stmt;
	auto info = make_uniq<CreateViewInfo>();
	info->query = std::move(select);
	info->temporary = temporary;
	info->SetQualifiedName(QualifiedName(Identifier(), schema_name, view_name));
	info->on_conflict = replace ? OnCreateConflict::REPLACE_ON_CONFLICT : OnCreateConflict::ERROR_ON_CONFLICT;
	stmt.info = std::move(info);
	return binder.Bind(stmt.Cast<SQLStatement>());
}

unique_ptr<QueryNode> CreateViewRelation::GetQueryNode() {
	throw InternalException("Cannot create a query node from an update relation");
}

string CreateViewRelation::GetQuery() {
	// reuse the catalog's prefix rendering for OR REPLACE / TEMP
	CreateViewInfo info;
	info.on_conflict = replace ? OnCreateConflict::REPLACE_ON_CONFLICT : OnCreateConflict::ERROR_ON_CONFLICT;
	info.temporary = temporary;
	info.SetQualifiedName(QualifiedName(Identifier(), schema_name, view_name));
	return info.GetCreatePrefix("VIEW") + info.QualifiedNameToString() + " AS " + child->GetQuery();
}

const vector<ColumnDefinition> &CreateViewRelation::Columns() {
	return columns;
}

string CreateViewRelation::ToString(idx_t depth) {
	string str = RenderWhitespace(depth) + "Create View\n";
	return str + child->ToString(depth + 1);
}

} // namespace duckdb
