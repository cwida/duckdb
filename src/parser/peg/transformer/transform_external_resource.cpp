#include "duckdb/common/exception/parser_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/peg/ast/generic_copy_option.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/external_resource_statement.hpp"
#include "duckdb/parser/parsed_data/external_resource_options.hpp"
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

//! The type spelled as words: `A B C` is 'a:b:c'. Lower-cased, since type names are, and '_' read as '-', since an
//! identifier cannot carry one; the string form is there for any name this cannot spell.
static string TypeOfWords(const vector<Identifier> &words) {
	// A truncated or misspelt long form ("SHOW EXTERNAL RESOURCE") must not read as a type named after the noun.
	if (StringUtil::CIEquals(words[0].GetIdentifierName(), "EXTERNAL")) {
		throw ParserException("a resource type spelled as words cannot start with EXTERNAL; the long form, EXTERNAL "
		                      "RESOURCE, takes the type as a string");
	}
	vector<string> parts;
	for (auto &word : words) {
		parts.push_back(StringUtil::Replace(StringUtil::Lower(word.GetIdentifierName()), "_", "-"));
	}
	return StringUtil::Join(parts, ":");
}

//! The grammar splits the words into the first and the rest, so that there are two at least.
static string TypeOfWords(const Identifier &first, const vector<Identifier> &rest) {
	vector<Identifier> words;
	words.push_back(first);
	words.insert(words.end(), rest.begin(), rest.end());
	return TypeOfWords(words);
}

// CREATE RESOURCE <words> ... is CREATE EXTERNAL RESOURCE '<words joined>' ...
unique_ptr<SQLStatement> PEGTransformerFactory::TransformCreateResourceWords(
    PEGTransformer &transformer, const Identifier &col_id, const vector<Identifier> &col_ids,
    const optional<Identifier> &attach_alias,
    const optional<vector<GenericCopyOption>> &external_resource_creation_options) {
	return TransformCreateExternalResourceStmt(transformer, TypeOfWords(col_id, col_ids), attach_alias,
	                                           external_resource_creation_options);
}

// REGISTER RESOURCE <words> ... is REGISTER EXTERNAL RESOURCE '<words joined>' ...
unique_ptr<SQLStatement> PEGTransformerFactory::TransformRegisterResourceWords(
    PEGTransformer &transformer, const Identifier &col_id, const vector<Identifier> &col_ids,
    const optional<Identifier> &attach_alias, unique_ptr<ParsedExpression> expression) {
	return TransformRegisterExternalResourceStmt(transformer, TypeOfWords(col_id, col_ids), attach_alias,
	                                             std::move(expression));
}

// NEW TEMPORARY RESOURCE <words> ... is NEW TEMPORARY EXTERNAL RESOURCE '<words joined>' ...
unique_ptr<ExternalResourceOptions> PEGTransformerFactory::TransformResourceWordsCreateClause(
    PEGTransformer &transformer, const Identifier &col_id, const vector<Identifier> &col_ids,
    const optional<vector<GenericCopyOption>> &external_resource_creation_options) {
	return TransformExternalResourceCreateClause(transformer, TypeOfWords(col_id, col_ids),
	                                             external_resource_creation_options);
}

// RESOURCE <name> is EXTERNAL RESOURCE <name>
unique_ptr<ExternalResourceOptions> PEGTransformerFactory::TransformResourceReferenceClause(PEGTransformer &transformer,
                                                                                            const Identifier &col_id) {
	return TransformExternalResourceReferenceClause(transformer, col_id);
}

} // namespace duckdb
