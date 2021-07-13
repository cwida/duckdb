#include "duckdb/main/relation/create_view_relation.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

CreateViewRelation::CreateViewRelation(shared_ptr<Relation> child_p, string view_name_p, bool replace_p,
                                       bool temporary_p)
    : Relation(child_p->context, RelationType::CREATE_VIEW_RELATION), child(move(child_p)),
      view_name(move(view_name_p)), replace(replace_p), temporary(temporary_p) {
	context.TryBindRelation(*this, this->columns);
}

BoundStatement CreateViewRelation::Bind(Binder &binder) {
	auto select = make_unique<SelectStatement>();
	select->node = child->GetQueryNode();

	CreateStatement stmt;
	auto info = make_unique<CreateViewInfo>();
	info->query = move(select);
	info->view_name = view_name;
	info->temporary = temporary;
	info->schema = "";
	info->on_conflict = replace ? OnCreateConflict::REPLACE_ON_CONFLICT : OnCreateConflict::ERROR_ON_CONFLICT;
	stmt.info = move(info);
	return binder.Bind((SQLStatement &)stmt);
}

const vector<ColumnDefinition> &CreateViewRelation::Columns() {
	return columns;
}

string CreateViewRelation::ToString(idx_t depth) {
	string str = RenderWhitespace(depth) + "Create View\n";
	return str + child->ToString(depth + 1);
}

} // namespace duckdb
