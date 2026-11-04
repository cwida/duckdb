#include "duckdb/main/relation/value_relation.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/tableref/expressionlistref.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parser.hpp"

namespace duckdb {

ValueRelation::ValueRelation(const shared_ptr<ClientContext> &context, const vector<vector<Value>> &values,
                             vector<string> names_p, string alias_p)
    : Relation(context, RelationType::VALUE_LIST_RELATION), names(std::move(names_p)), alias(std::move(alias_p)) {
	// create constant expressions for the values
	for (idx_t row_idx = 0; row_idx < values.size(); row_idx++) {
		auto &list = values[row_idx];
		vector<unique_ptr<ParsedExpression>> expressions;
		for (idx_t col_idx = 0; col_idx < list.size(); col_idx++) {
			expressions.push_back(ConstantExpression::FromValue(list[col_idx]));
		}
		this->expressions.push_back(std::move(expressions));
	}
	QueryResult::DeduplicateColumns(names);
	TryBindRelation(columns);
}

ValueRelation::ValueRelation(const shared_ptr<ClientContext> &context,
                             vector<vector<unique_ptr<ParsedExpression>>> &&expressions_p, vector<string> names_p,
                             string alias_p)
    : ValueRelation(make_shared_ptr<RelationContextWrapper>(context), std::move(expressions_p), std::move(names_p),
                    std::move(alias_p)) {
}

ValueRelation::ValueRelation(const shared_ptr<ClientContext> &context, const string &values_list,
                             vector<string> names_p, string alias_p)
    : Relation(context, RelationType::VALUE_LIST_RELATION), names(std::move(names_p)), alias(std::move(alias_p)) {
	this->expressions = Parser(*context).ParseValuesList(values_list);
	QueryResult::DeduplicateColumns(names);
	TryBindRelation(columns);
}

ValueRelation::ValueRelation(const shared_ptr<RelationContextWrapper> &context, const vector<vector<Value>> &values,
                             vector<string> names_p, string alias_p)
    : Relation(context, RelationType::VALUE_LIST_RELATION), names(std::move(names_p)), alias(std::move(alias_p)) {
	// create constant expressions for the values
	for (idx_t row_idx = 0; row_idx < values.size(); row_idx++) {
		auto &list = values[row_idx];
		vector<unique_ptr<ParsedExpression>> expressions;
		for (idx_t col_idx = 0; col_idx < list.size(); col_idx++) {
			expressions.push_back(ConstantExpression::FromValue(list[col_idx]));
		}
		this->expressions.push_back(std::move(expressions));
	}
	QueryResult::DeduplicateColumns(names);
	TryBindRelation(columns);
}

ValueRelation::ValueRelation(const shared_ptr<RelationContextWrapper> &context,
                             vector<vector<unique_ptr<ParsedExpression>>> &&expressions_p, vector<string> names_p,
                             string alias_p)
    : Relation(context, RelationType::VALUE_LIST_RELATION), alias(std::move(alias_p)) {
	D_ASSERT(!expressions_p.empty());
	if (names_p.empty()) {
		auto &first_list = expressions_p[0];
		for (auto &expr : first_list) {
			names_p.emplace_back(expr->GetName());
		}
	}
	names = std::move(names_p);
	expressions = std::move(expressions_p);
	QueryResult::DeduplicateColumns(names);
	TryBindRelation(columns);
}

unique_ptr<QueryNode> ValueRelation::GetQueryNode() {
	auto result = make_uniq<SelectNode>();
	result->select_list.push_back(make_uniq<StarExpression>());
	result->from_table = GetTableRef();
	return std::move(result);
}

unique_ptr<TableRef> ValueRelation::GetTableRef() {
	auto table_ref = make_uniq<ExpressionListRef>();
	// set the expected types/names
	if (columns.empty()) {
		// no columns yet: only set up names
		for (idx_t i = 0; i < names.size(); i++) {
			table_ref->expected_names.push_back(Identifier(names[i]));
		}
	} else {
		for (idx_t i = 0; i < columns.size(); i++) {
			table_ref->expected_names.emplace_back(columns[i].Name());
			table_ref->expected_types.push_back(columns[i].Type());
			D_ASSERT(names.size() == 0 || columns[i].Name() == names[i]);
		}
	}
	// copy the expressions
	for (auto &expr_list : expressions) {
		vector<unique_ptr<ParsedExpression>> copied_list;
		copied_list.reserve(expr_list.size());
		for (auto &expr : expr_list) {
			copied_list.push_back(expr->Copy());
		}
		table_ref->values.push_back(std::move(copied_list));
	}
	table_ref->alias = Identifier(GetAlias());
	return std::move(table_ref);
}

Identifier ValueRelation::GetAlias() {
	return Identifier(alias);
}

const vector<ColumnDefinition> &ValueRelation::Columns() {
	return columns;
}

string ValueRelation::ToString(idx_t depth) {
	string str = RenderWhitespace(depth) + "Values ";
	for (idx_t row_idx = 0; row_idx < expressions.size(); row_idx++) {
		auto &list = expressions[row_idx];
		str += row_idx > 0 ? ", (" : "(";
		for (idx_t col_idx = 0; col_idx < list.size(); col_idx++) {
			str += col_idx > 0 ? ", " : "";
			str += list[col_idx]->ToString();
		}
		str += ")";
	}
	str += "\n";
	return str;
}

} // namespace duckdb
