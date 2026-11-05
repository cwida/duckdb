#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/peg/ast/generic_copy_option.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/external_resource_statement.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"

namespace duckdb {

//! Fold `(k v, ...)` create params onto the statement; they are resolved at bind time.
static void ApplyOptions(const optional<vector<GenericCopyOption>> &options, ExternalResourceStatement &stmt) {
	if (!options) {
		return;
	}
	PEGTransformerFactory::CollectGenericOptions(*options, stmt.options, "EXTERNAL RESOURCE");
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformCreateExternalResourceStmt(
    PEGTransformer &transformer, const string &string_literal, const optional<Identifier> &attach_alias,
    const optional<vector<GenericCopyOption>> &external_resource_creation_options) {
	auto result = make_uniq<ExternalResourceStatement>(ExternalResourceOperation::CREATE);
	result->type = string_literal;
	if (attach_alias) {
		result->name = Identifier(*attach_alias);
	}
	ApplyOptions(external_resource_creation_options, *result);
	return std::move(result);
}

unique_ptr<SQLStatement>
PEGTransformerFactory::TransformRegisterExternalResourceStmt(PEGTransformer &transformer, const string &string_literal,
                                                             const optional<Identifier> &attach_alias,
                                                             unique_ptr<ParsedExpression> expression) {
	auto result = make_uniq<ExternalResourceStatement>(ExternalResourceOperation::REGISTER);
	result->type = string_literal;
	if (attach_alias) {
		result->name = Identifier(*attach_alias);
	}
	result->handle = std::move(expression);
	return std::move(result);
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformDestroyExternalResourceStmt(PEGTransformer &transformer,
                                                                                     const Identifier &col_id) {
	auto result = make_uniq<ExternalResourceStatement>(ExternalResourceOperation::DESTROY);
	result->name = col_id;
	return std::move(result);
}

//! A named argument of a table function call: the name goes on the argument and, for the binder, on the expression.
static void AddNamedArgument(vector<FunctionArgument> &children, const string &name,
                             unique_ptr<ParsedExpression> value) {
	value->SetAlias(Identifier(name));
	children.emplace_back(Identifier(name), std::move(value));
}

// SHOW [ALL] EXTERNAL RESOURCES [WITH TYPE t] is the query
// `SELECT * FROM duckdb_external_resources(discover := <all>, type := t)`, so it can be used wherever a query can,
// such as a subquery.
unique_ptr<QueryNode>
PEGTransformerFactory::TransformShowExternalResources(PEGTransformer &transformer, const ShowType &show_rule,
                                                      const bool &has_result,
                                                      optional<Identifier> show_external_resources_type) {
	vector<FunctionArgument> children;
	if (has_result) {
		AddNamedArgument(children, "discover", ConstantExpression::Boolean(true));
	}
	if (show_external_resources_type) {
		auto type = ConstantExpression::String(show_external_resources_type->GetIdentifierName());
		AddNamedArgument(children, "type", std::move(type));
	}
	auto table_function = make_uniq<TableFunctionRef>();
	table_function->function = make_uniq<FunctionExpression>("duckdb_external_resources", std::move(children));

	auto select_node = make_uniq<SelectNode>();
	select_node->select_list.push_back(make_uniq<StarExpression>());
	select_node->from_table = std::move(table_function);
	return std::move(select_node);
}

} // namespace duckdb
