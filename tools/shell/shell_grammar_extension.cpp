#include "shell_grammar_extension.hpp"

#include "duckdb/common/exception/parser_exception.hpp"
#include "duckdb/main/table_description.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/emptytableref.hpp"

namespace duckdb {

//! A single bare column reference is read as a table name: "tbl" becomes "SELECT * FROM tbl"
static unique_ptr<TableRef> TableFromColumnRef(const ColumnRefExpression &col_expr) {
	if (!col_expr.IsQualified()) {
		auto base_table = make_uniq<BaseTableRef>();
		base_table->SetTable(col_expr.GetColumnName());
		return std::move(base_table);
	}
	auto &names = col_expr.ColumnNames();
	if (names.size() >= 4) {
		throw ParserException("Too many qualifiers encountered. Expected \"catalog.schema.table\" or \"schema.table\"");
	}
	if (names.size() == 3) {
		return make_uniq<BaseTableRef>(TableDescription(QualifiedName(names[0], names[1], names[2])));
	}
	return make_uniq<BaseTableRef>(TableDescription(QualifiedName({names[0]}, names[1])));
}

static arena_ptr<TransformResultValue> FinalizeExpressionStatement(PEGTransformer &transformer,
                                                                   ParseResult &parse_result) {
	// ExpressionStatement <- List(ExpressionAlias)
	auto &list_pr = parse_result.Cast<ListParseResult>();
	auto items = PEGTransformerFactory::ExtractParseResultsFromList(list_pr.GetChild(0));
	vector<unique_ptr<ParsedExpression>> expressions;
	for (auto &item : items) {
		// ExpressionAlias is a choice - transform whichever alternative matched
		auto &choice = item.get().Cast<ListParseResult>().Child<ChoiceParseResult>(0).GetResult();
		expressions.push_back(transformer.Transform<unique_ptr<ParsedExpression>>(choice));
	}

	bool any_column_ref = false;
	for (auto &expr : expressions) {
		if (expr->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
			any_column_ref = true;
			break;
		}
	}
	if (any_column_ref && expressions.size() > 1) {
		throw ParserException("Mix of table names and expressions is not supported. "
		                      "Use SELECT to explicitly specify what you want to query.");
	}

	auto select_node = make_uniq<SelectNode>();
	if (any_column_ref) {
		select_node->from_table = TableFromColumnRef(expressions[0]->Cast<ColumnRefExpression>());
		select_node->select_list.push_back(make_uniq<StarExpression>());
	} else {
		for (auto &expr : expressions) {
			select_node->select_list.push_back(std::move(expr));
		}
		select_node->from_table = make_uniq<EmptyTableRef>();
	}
	auto select_statement = make_uniq<SelectStatement>();
	select_statement->node = std::move(select_node);
	return transformer.MakeResult<unique_ptr<SQLStatement>>(std::move(select_statement));
}

static arena_ptr<TransformProcess> StartExpressionStatementTransform(PEGTransformer &transformer,
                                                                     ParseResult &parse_result) {
	return transformer.MakeProcess<FinalizeTransformProcess>(transformer, parse_result, FinalizeExpressionStatement);
}

ShellGrammarExtension::ShellGrammarExtension()
    : GrammarExtension("shell", "Run a bare expression list or table name as a statement (CLI shorthand)") {
}

vector<GrammarChange> ShellGrammarExtension::GetChanges() const {
	vector<GrammarChange> changes;
	// suppress suggestions so a bare expression does not pollute statement-level completion
	changes.push_back(GrammarChange::AddRule("ExpressionStatement <- List(ExpressionAlias)",
	                                         StartExpressionStatementTransform, true));
	// no implicit aliases ("1 + 1 foo") - those are too easily confused with a typo'd statement
	changes.push_back(GrammarChange::AddRule("ExpressionAlias <- ColIdExpression / ExpressionAsCollabel / Expression"));
	// last alternative so every real statement is tried first
	changes.push_back(GrammarChange::AddChoice("Statement", "ExpressionStatement"));
	return changes;
}

} // namespace duckdb
