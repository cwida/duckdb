#include "duckdb/main/relation/table_relation.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/main/relation/delete_relation.hpp"
#include "duckdb/main/relation/value_relation.hpp"
#include "duckdb/main/relation/update_relation.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

TableRelation::TableRelation(const shared_ptr<ClientContext> &context, unique_ptr<TableDescription> description)
    : Relation(context, RelationType::TABLE_RELATION), description(std::move(description)) {
}

TableRelation::TableRelation(const shared_ptr<RelationContextWrapper> &context,
                             unique_ptr<TableDescription> description)
    : Relation(context, RelationType::TABLE_RELATION), description(std::move(description)) {
}

unique_ptr<QueryNode> TableRelation::GetQueryNode() {
	auto result = make_uniq<SelectNode>();
	result->select_list.push_back(make_uniq<StarExpression>());
	result->from_table = GetTableRef();
	return std::move(result);
}

unique_ptr<TableRef> TableRelation::GetTableRef() {
	auto table_ref = make_uniq<BaseTableRef>();
	table_ref->SetQualifiedName(description->qualified_name);
	return std::move(table_ref);
}

Identifier TableRelation::GetAlias() {
	return description->qualified_name.Name();
}

const vector<ColumnDefinition> &TableRelation::Columns() {
	return description->columns;
}

string TableRelation::ToString(idx_t depth) {
	return RenderWhitespace(depth) + "Scan Table [" +
	       description->qualified_name.ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA) + "]";
}

static unique_ptr<ParsedExpression> ParseCondition(ClientContext &context, const string &condition) {
	if (!condition.empty()) {
		auto expression_list = Parser(context).ParseExpressionList(condition);
		if (expression_list.size() != 1) {
			throw ParserException("Expected a single expression as filter condition");
		}
		return std::move(expression_list[0]);
	} else {
		return nullptr;
	}
}

void TableRelation::Update(vector<string> names, vector<unique_ptr<ParsedExpression>> &&update,
                           unique_ptr<ParsedExpression> condition) {
	vector<Identifier> update_columns = StringsToIdentifiers(names);
	vector<unique_ptr<ParsedExpression>> expressions = std::move(update);

	auto update_relation = make_shared_ptr<UpdateRelation>(
	    context, std::move(condition), description->qualified_name.Catalog(), description->qualified_name.Schema(),
	    description->qualified_name.Name(), std::move(update_columns), std::move(expressions));
	update_relation->Execute();
}

void TableRelation::Update(const string &update_list, const string &condition) {
	vector<Identifier> update_columns;
	vector<unique_ptr<ParsedExpression>> expressions;
	auto cond = ParseCondition(*context->GetContext(), condition);
	Parser(*context->GetContext()).ParseUpdateList(update_list, update_columns, expressions);
	auto update = make_shared_ptr<UpdateRelation>(
	    context, std::move(cond), description->qualified_name.Catalog(), description->qualified_name.Schema(),
	    description->qualified_name.Name(), std::move(update_columns), std::move(expressions));
	update->Execute();
}

void TableRelation::Delete(const string &condition) {
	auto cond = ParseCondition(*context->GetContext(), condition);
	auto del =
	    make_shared_ptr<DeleteRelation>(context, std::move(cond), description->qualified_name.Catalog(),
	                                    description->qualified_name.Schema(), description->qualified_name.Name());
	del->Execute();
}

void TableRelation::Insert(const vector<vector<Value>> &values) {
	vector<string> column_names;
	auto rel = make_shared_ptr<ValueRelation>(context->GetContext(), values, std::move(column_names), "values");
	rel->Insert(description->qualified_name.Catalog(), description->qualified_name.Schema(),
	            description->qualified_name.Name());
}

void TableRelation::Insert(vector<vector<unique_ptr<ParsedExpression>>> &&expressions) {
	vector<string> column_names;
	auto rel = make_shared_ptr<ValueRelation>(context->GetContext(), std::move(expressions), std::move(column_names),
	                                          "values");
	rel->Insert(description->qualified_name.Catalog(), description->qualified_name.Schema(),
	            description->qualified_name.Name());
}

} // namespace duckdb
