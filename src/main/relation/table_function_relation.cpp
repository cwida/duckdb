#include "duckdb/main/relation/table_function_relation.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/common/shared_ptr.hpp"

namespace duckdb {

void TableFunctionRelation::AddNamedParameter(const string &name, Value argument) {
	named_parameters[Identifier(name)] = std::move(argument);
}

void TableFunctionRelation::RemoveNamedParameterIfExists(const string &name) {
	if (named_parameters.find(Identifier(name)) != named_parameters.end()) {
		named_parameters.erase(Identifier(name));
	}
}

void TableFunctionRelation::SetNamedParameters(named_parameter_map_t &&options) {
	D_ASSERT(named_parameters.empty());
	named_parameters = std::move(options);
}

TableFunctionRelation::TableFunctionRelation(const shared_ptr<ClientContext> &context, string name_p,
                                             vector<Value> parameters_p, named_parameter_map_t named_parameters,
                                             shared_ptr<Relation> input_relation_p, bool auto_init,
                                             shared_ptr<TableFunctionInfo> bind_info_p)
    : Relation(context, RelationType::TABLE_FUNCTION_RELATION), name(std::move(name_p)),
      parameters(std::move(parameters_p)), named_parameters(std::move(named_parameters)),
      input_relation(std::move(input_relation_p)), bind_info(std::move(bind_info_p)), auto_initialize(auto_init) {
	InitializeColumns();
}

TableFunctionRelation::TableFunctionRelation(const shared_ptr<RelationContextWrapper> &context, string name_p,
                                             vector<Value> parameters_p, named_parameter_map_t named_parameters,
                                             shared_ptr<Relation> input_relation_p, bool auto_init,
                                             shared_ptr<TableFunctionInfo> bind_info_p)
    : Relation(context, RelationType::TABLE_FUNCTION_RELATION), name(std::move(name_p)),
      parameters(std::move(parameters_p)), named_parameters(std::move(named_parameters)),
      input_relation(std::move(input_relation_p)), bind_info(std::move(bind_info_p)), auto_initialize(auto_init) {
	InitializeColumns();
}

TableFunctionRelation::TableFunctionRelation(const shared_ptr<ClientContext> &context, string name_p,
                                             vector<Value> parameters_p, shared_ptr<Relation> input_relation_p,
                                             bool auto_init, shared_ptr<TableFunctionInfo> bind_info_p)
    : Relation(context, RelationType::TABLE_FUNCTION_RELATION), name(std::move(name_p)),
      parameters(std::move(parameters_p)), input_relation(std::move(input_relation_p)),
      bind_info(std::move(bind_info_p)), auto_initialize(auto_init) {
	InitializeColumns();
}

void TableFunctionRelation::InitializeColumns() {
	if (!auto_initialize) {
		return;
	}
	TryBindRelation(columns);
}

unique_ptr<QueryNode> TableFunctionRelation::GetQueryNode() {
	auto result = make_uniq<SelectNode>();
	result->select_list.push_back(make_uniq<StarExpression>());
	result->from_table = GetTableRef();
	return std::move(result);
}

unique_ptr<TableRef> TableFunctionRelation::GetTableRef() {
	vector<unique_ptr<ParsedExpression>> children;
	if (input_relation) { // input relation becomes first parameter if present, always
		auto subquery = make_uniq<SubqueryExpression>();
		subquery->SubqueryMutable() = make_uniq<SelectStatement>();
		subquery->SubqueryMutable()->node = input_relation->GetQueryNode();
		subquery->GetSubqueryTypeMutable() = SubqueryType::SCALAR;
		children.push_back(std::move(subquery));
	}
	for (auto &parameter : parameters) {
		children.push_back(ConstantExpression::FromValue(parameter));
	}

	for (auto &parameter : named_parameters) {
		// Hackity-hack some comparisons with column refs
		// This is all but pretty, basically the named parameter is the column, the table is empty because that's what
		// the function binder likes
		auto column_ref = make_uniq<ColumnRefExpression>(parameter.first);
		auto constant_value = ConstantExpression::FromValue(parameter.second);
		auto comparison = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL, std::move(column_ref),
		                                                  std::move(constant_value));
		children.push_back(std::move(comparison));
	}

	auto table_function = make_uniq<TableFunctionRef>();
	auto function = make_uniq<FunctionExpression>(name, std::move(children));
	table_function->function = std::move(function);
	table_function->bind_info = bind_info;
	return std::move(table_function);
}

Identifier TableFunctionRelation::GetAlias() {
	return name;
}

const vector<ColumnDefinition> &TableFunctionRelation::Columns() {
	return columns;
}

string TableFunctionRelation::ToString(idx_t depth) {
	string function_call = name + "(";
	for (idx_t i = 0; i < parameters.size(); i++) {
		if (i > 0) {
			function_call += ", ";
		}
		function_call += parameters[i].ToString();
	}
	function_call += ")";
	return RenderWhitespace(depth) + function_call;
}

} // namespace duckdb
