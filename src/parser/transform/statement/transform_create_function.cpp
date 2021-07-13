#include "duckdb/function/macro_function.hpp"
#include "duckdb/parser/parsed_data/create_macro_info.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/transformer.hpp"

namespace duckdb {

unique_ptr<CreateStatement> Transformer::TransformCreateFunction(duckdb_libpgquery::PGNode *node) {
	D_ASSERT(node);
	D_ASSERT(node->type == duckdb_libpgquery::T_PGCreateFunctionStmt);

	auto stmt = reinterpret_cast<duckdb_libpgquery::PGCreateFunctionStmt *>(node);
	D_ASSERT(stmt);

	auto result = make_unique<CreateStatement>();
	auto info = make_unique<CreateMacroInfo>();

	auto qname = TransformQualifiedName(stmt->name);
	info->schema = qname.schema;
	info->name = qname.name;

	auto function = TransformExpression(stmt->function, 0);
	D_ASSERT(function);
	auto macro_func = make_unique<MacroFunction>(move(function));

	if (stmt->params) {
		vector<unique_ptr<ParsedExpression>> parameters;
		TransformExpressionList(*stmt->params, parameters, 0);
		for (auto &param : parameters) {
			if (param->type == ExpressionType::COMPARE_EQUAL) {
				// parameters with default value
				auto &comp_expr = (ComparisonExpression &)*param;
				if (comp_expr.left->GetExpressionClass() != ExpressionClass::COLUMN_REF) {
					throw ParserException("Invalid parameter: '%s'", comp_expr.left->ToString());
				}
				if (comp_expr.right->GetExpressionClass() != ExpressionClass::CONSTANT) {
					throw ParserException("Parameters may only have constants as default value!");
				}
				auto &param_name_expr = (ColumnRefExpression &)*comp_expr.left;
				if (!param_name_expr.table_name.empty()) {
					throw BinderException("Invalid parameter name '%s'", param_name_expr.ToString());
				}
				macro_func->default_parameters[comp_expr.left->ToString()] = move(comp_expr.right);
			} else if (param->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
				// positional parameters
				if (!macro_func->default_parameters.empty()) {
					throw ParserException("Positional parameters cannot come after parameters with a default value!");
				}
				macro_func->parameters.push_back(move(param));
			} else {
				throw ParserException("Invalid parameter: '%s'", param->ToString());
			}
		}
	}

	info->function = move(macro_func);
	result->info = move(info);
	return result;
}

} // namespace duckdb
