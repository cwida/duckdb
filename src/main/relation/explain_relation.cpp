#include "duckdb/main/relation/explain_relation.hpp"
#include "duckdb/parser/statement/explain_statement.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

ExplainRelation::ExplainRelation(shared_ptr<Relation> child_p, ExplainType type, const ProfilerPrintFormat &format)
    : Relation(child_p->context, RelationType::EXPLAIN_RELATION), child(std::move(child_p)), type(type),
      format(format) {
	TryBindRelation(columns);
}

BoundStatement ExplainRelation::Bind(Binder &binder) {
	auto select = make_uniq<SelectStatement>();
	select->node = child->GetQueryNode();
	ExplainStatement explain(std::move(select), type, format);
	return binder.Bind(explain.Cast<SQLStatement>());
}

unique_ptr<QueryNode> ExplainRelation::GetQueryNode() {
	throw InternalException("Cannot create a query node from an explain relation");
}

string ExplainRelation::GetQuery() {
	// reuse the statement's option rendering so ANALYZE and FORMAT stay in sync
	ExplainStatement explain(nullptr, type, format);
	auto options = explain.OptionsToString();
	string result = "EXPLAIN";
	if (!options.empty()) {
		result += " " + options;
	}
	return result + " " + child->GetQuery();
}

const vector<ColumnDefinition> &ExplainRelation::Columns() {
	return columns;
}

string ExplainRelation::ToString(idx_t depth) {
	string str = RenderWhitespace(depth) + "Explain\n";
	return str + child->ToString(depth + 1);
}

} // namespace duckdb
