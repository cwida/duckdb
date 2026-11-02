#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/statement/explain_statement.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"

namespace duckdb {

ProfilerPrintFormat ParseProfilerPrintFormat(const Value &val) {
	if (val.type().id() != LogicalTypeId::VARCHAR) {
		throw InvalidInputException("Expected a string as argument to FORMAT");
	}
	// the format name is validated when the renderer is created (needs a ClientContext); only normalize it here
	return ProfilerPrintFormat(StringUtil::Lower(val.GetValue<string>()));
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformExplainStatement(
    PEGTransformer &transformer, const optional<Identifier> &analyze_keyword,
    const optional<vector<GenericCopyOption>> &explain_option_list, unique_ptr<SQLStatement> explainable_statements) {
	auto explain_type = analyze_keyword ? ExplainType::EXPLAIN_ANALYZE : ExplainType::EXPLAIN_STANDARD;
	bool format_is_set = false;
	bool sql_is_set = false;
	auto format = ProfilerPrintFormat::Default();
	if (explain_option_list) {
		for (auto option : *explain_option_list) {
			auto option_name = StringUtil::Lower(option.name.GetIdentifierName());
			if (option_name == "format") {
				if (format_is_set) {
					throw InvalidInputException("FORMAT can not be provided more than once");
				}
				if (option.children.empty()) {
					// no constant/identifier argument: either FORMAT was given nothing at all, or its argument is an
					// expression the parser kept whole. A bare DEFAULT keyword parses as a DefaultExpression.
					if (option.expression && option.expression->GetExpressionType() == ExpressionType::VALUE_DEFAULT) {
						format = ProfilerPrintFormat::Default();
					} else {
						throw InvalidInputException("FORMAT requires a single format name, e.g. FORMAT json");
					}
				} else {
					format = ParseProfilerPrintFormat(option.children[0]);
				}
				format_is_set = true;
			} else if (option_name == "sql") {
				if (sql_is_set || !option.children.empty() || option.expression) {
					throw InvalidInputException("SQL must be provided once without arguments");
				}
				sql_is_set = true;
			} else if (option_name == "analyze") {
				explain_type = ExplainType::EXPLAIN_ANALYZE;
			} else {
				throw NotImplementedException("Unimplemented explain type: %s", option_name);
			}
		}
	}
	if (sql_is_set) {
		if (format_is_set || explain_type == ExplainType::EXPLAIN_ANALYZE) {
			throw InvalidInputException("EXPLAIN (SQL) cannot be combined with ANALYZE or FORMAT");
		}
		transformer.PivotEntryCheck("EXPLAIN (SQL) statement");
		explain_type = ExplainType::EXPLAIN_SQL;
	}
	auto statement = std::move(explainable_statements);
	return make_uniq<ExplainStatement>(std::move(statement), explain_type, format);
}

Identifier PEGTransformerFactory::TransformExplainOptionName(PEGTransformer &transformer, ParseResult &choice_result) {
	return transformer.Transform<Identifier>(choice_result);
}

unique_ptr<SQLStatement>
PEGTransformerFactory::TransformExplainSelectStatement(PEGTransformer &transformer,
                                                       unique_ptr<SelectStatement> select_statement_internal) {
	return std::move(select_statement_internal);
}

vector<GenericCopyOption>
PEGTransformerFactory::TransformExplainOptionList(PEGTransformer &transformer,
                                                  const vector<GenericCopyOption> &explain_option) {
	return explain_option;
}

GenericCopyOption PEGTransformerFactory::TransformExplainOption(PEGTransformer &transformer,
                                                                const Identifier &explain_option_name,
                                                                optional<unique_ptr<ParsedExpression>> expression) {
	GenericCopyOption copy_option;
	copy_option.name = Identifier(StringUtil::Lower(explain_option_name.GetIdentifierName()));
	if (!expression) {
		return copy_option;
	}
	auto &expr = *expression;
	if (expr->GetExpressionType() == ExpressionType::VALUE_CONSTANT) {
		copy_option.children.push_back(expr->Cast<ConstantExpression>().GetLiteral().ToValue());
	} else if (expr->GetExpressionType() == ExpressionType::COLUMN_REF) {
		copy_option.children.push_back(Value(expr->Cast<ColumnRefExpression>().GetColumnName()));
	} else {
		copy_option.expression = std::move(expr);
	}
	return copy_option;
}

} // namespace duckdb
