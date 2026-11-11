#include "duckdb/logging/logger.hpp"
#include "duckdb/parser/statement/pragma_statement.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"

namespace duckdb {
unique_ptr<SQLStatement>
PEGTransformerFactory::TransformPragmaStatement(PEGTransformer &transformer,
                                                unique_ptr<SQLStatement> pragma_assign_or_function) {
	return pragma_assign_or_function;
}

unique_ptr<SQLStatement>
PEGTransformerFactory::TransformPragmaAssign(PEGTransformer &transformer, const Identifier &setting_name,
                                             vector<unique_ptr<ParsedExpression>> variable_list) {
	// Rule: PragmaAssign <- SettingName '=' Expression
	auto result = make_uniq<PragmaStatement>();
	auto &info = *result->info;
	info.name = setting_name;
	if (variable_list.size() != 1) {
		throw ParserException("PRAGMA statement with assignment should contain exactly one parameter");
	}
	auto &expr = variable_list[0];
	if (expr->GetExpressionType() == ExpressionType::COLUMN_REF) {
		auto &colref = expr->Cast<ColumnRefExpression>();
		if (!colref.IsQualified()) {
			info.parameters.emplace_back(ConstantExpression::String(colref.GetColumnName().GetIdentifierName()));
		} else {
			info.parameters.emplace_back(ConstantExpression::String(expr->ToString()));
		}
	} else {
		info.parameters.emplace_back(std::move(expr));
	}
	// SQLite does not distinguish between:
	// "PRAGMA table_info='integers'"
	// "PRAGMA table_info('integers')"
	// for compatibility, any pragmas that match the SQLite ones are parsed as calls
	identifier_set_t sqlite_compat_pragmas {"table_info"};
	if (sqlite_compat_pragmas.find(info.name) != sqlite_compat_pragmas.end()) {
		return std::move(result);
	}
	if (transformer.options.context) {
		DUCKDB_LOG_WARNING(*transformer.options.context,
		                   "Using \"PRAGMA %s=...\" to set a setting is deprecated and will be removed in a future "
		                   "release, use \"SET %s=...\" instead.",
		                   info.name.GetIdentifierName(), info.name.GetIdentifierName());
	}
	auto set_statement = make_uniq<SetVariableStatement>(info.name, std::move(info.parameters[0]), SetScope::AUTOMATIC);
	return std::move(set_statement);
}

unique_ptr<SQLStatement>
PEGTransformerFactory::TransformPragmaFunction(PEGTransformer &transformer, const Identifier &pragma_name,
                                               optional<vector<unique_ptr<ParsedExpression>>> pragma_parameters) {
	// Rule: PragmaFunction <- PragmaName PragmaParameters?
	auto result = make_uniq<PragmaStatement>();
	result->info->name = pragma_name;
	if (!pragma_parameters) {
		return std::move(result);
	}
	for (auto &parameter : *pragma_parameters) {
		if (parameter->GetExpressionType() == ExpressionType::COMPARE_EQUAL) {
			auto &comp = parameter->Cast<ComparisonExpression>();
			if (comp.Left().GetExpressionType() != ExpressionType::COLUMN_REF) {
				throw ParserException("Named parameter requires a column reference on the LHS");
			}
			auto &columnref = comp.Left().Cast<ColumnRefExpression>();
			result->info->named_parameters.insert(make_pair(columnref.GetName(), std::move(comp.RightMutable())));
		} else if (parameter->GetExpressionType() == ExpressionType::COLUMN_REF) {
			auto &colref = parameter->Cast<ColumnRefExpression>();
			if (!colref.IsQualified()) {
				result->info->parameters.emplace_back(
				    ConstantExpression::String(colref.GetColumnName().GetIdentifierName()));
			} else {
				result->info->parameters.emplace_back(ConstantExpression::String(parameter->ToString()));
			}
		} else {
			result->info->parameters.emplace_back(std::move(parameter));
		}
	}
	return std::move(result);
}

vector<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformPragmaParameters(PEGTransformer &transformer,
                                                 vector<unique_ptr<ParsedExpression>> expression) {
	return expression;
}

} // namespace duckdb
