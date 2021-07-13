#include "duckdb/common/exception.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/lambda_expression.hpp"
#include "duckdb/parser/transformer.hpp"

namespace duckdb {

static string ExtractColumnFromLambda(ParsedExpression &expr) {
	if (expr.type != ExpressionType::COLUMN_REF) {
		throw ParserException("Lambda parameter must be a column name");
	}
	auto &colref = (ColumnRefExpression &)expr;
	if (!colref.table_name.empty()) {
		throw ParserException("Lambda parameter must be an unqualified name (e.g. 'x', not 'a.x')");
	}
	return colref.column_name;
}

unique_ptr<ParsedExpression> Transformer::TransformLambda(duckdb_libpgquery::PGLambdaFunction *node, idx_t depth) {
	vector<unique_ptr<ParsedExpression>> parameter_expressions;
	TransformExpressionList(*node->parameters, parameter_expressions, depth + 1);
	vector<string> parameters;
	parameters.reserve(parameter_expressions.size());
	for (auto &expr : parameter_expressions) {
		parameters.push_back(ExtractColumnFromLambda(*expr));
	}

	auto lambda_function = TransformExpression(node->function, depth + 1);
	return make_unique<LambdaExpression>(move(parameters), move(lambda_function));
}

} // namespace duckdb
