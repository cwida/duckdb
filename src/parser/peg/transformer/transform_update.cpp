#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/query_node/update_query_node.hpp"
#include "duckdb/parser/statement/update_statement.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"

namespace duckdb {

unique_ptr<SQLStatement> PEGTransformerFactory::TransformUpdateStatement(
    PEGTransformer &transformer, optional<CommonTableExpressionMap> with_clause, unique_ptr<TableRef> update_target,
    unique_ptr<UpdateSetInfo> update_set_clause, optional<unique_ptr<TableRef>> from_clause,
    optional<unique_ptr<ParsedExpression>> where_clause,
    optional<vector<unique_ptr<ParsedExpression>>> returning_clause) {
	auto result = make_uniq<UpdateStatement>();
	auto &node = *result->node;
	if (with_clause) {
		node.cte_map = std::move(*with_clause);
	}
	node.table = std::move(update_target);
	node.set_info = std::move(update_set_clause);
	if (from_clause) {
		node.from_table = std::move(*from_clause);
	}
	if (where_clause) {
		node.set_info->condition = std::move(*where_clause);
	}
	if (returning_clause) {
		node.returning_list = std::move(*returning_clause);
	}
	return std::move(result);
}

unique_ptr<TableRef> PEGTransformerFactory::TransformBaseTableSet(PEGTransformer &transformer,
                                                                  unique_ptr<BaseTableRef> base_table_name) {
	return std::move(base_table_name);
}

unique_ptr<TableRef> PEGTransformerFactory::TransformBaseTableAliasSet(PEGTransformer &transformer,
                                                                       unique_ptr<BaseTableRef> base_table_name,
                                                                       const optional<Identifier> &update_alias) {
	if (update_alias) {
		base_table_name->alias = *update_alias;
	}
	return std::move(base_table_name);
}

Identifier PEGTransformerFactory::TransformUpdateAlias(PEGTransformer &transformer, const bool &has_result,
                                                       const Identifier &col_id) {
	return Identifier(col_id);
}

unique_ptr<UpdateSetInfo> PEGTransformerFactory::TransformUpdateSetTuple(PEGTransformer &transformer,
                                                                         const vector<Identifier> &column_name,
                                                                         unique_ptr<ParsedExpression> expression) {
	auto result = make_uniq<UpdateSetInfo>();
	result->columns = column_name;

	bool is_row_assignment = false;
	if (expression->GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &func_ref = expression->Cast<FunctionExpression>();
		if (func_ref.FunctionName() == "row") {
			is_row_assignment = true;
		}
	}

	if (is_row_assignment) {
		auto &func_expr = expression->Cast<FunctionExpression>();
		if (func_expr.GetArguments().size() != result->columns.size()) {
			throw ParserException("Could not perform assignment, expected %d values, got %d", result->columns.size(),
			                      func_expr.GetArguments().size());
		}
		for (auto &arg : func_expr.GetArgumentsMutable()) {
			result->expressions.push_back(std::move(arg.GetExpressionMutable()));
		}
	} else {
		result->expressions.reserve(result->columns.size());
		for (idx_t i = 0; i < result->columns.size(); i++) {
			result->expressions.push_back(expression->Copy());
		}
	}

	return result;
}

unique_ptr<UpdateSetInfo>
PEGTransformerFactory::TransformUpdateSetClauseList(PEGTransformer &transformer,
                                                    vector<unique_ptr<UpdateSetInfo>> update_set_info) {
	auto result = make_uniq<UpdateSetInfo>();
	for (auto &info : update_set_info) {
		for (idx_t i = 0; i < info->columns.size(); i++) {
			result->columns.emplace_back(std::move(info->columns[i]));
			result->expressions.push_back(std::move(info->expressions[i]));
		}
	}
	return result;
}

unique_ptr<UpdateSetInfo> PEGTransformerFactory::TransformUpdateSetElement(PEGTransformer &transformer,
                                                                           const string &update_set_column_target,
                                                                           unique_ptr<ParsedExpression> expression) {
	auto result = make_uniq<UpdateSetInfo>();
	result->columns.emplace_back(update_set_column_target);
	result->expressions.push_back(std::move(expression));
	return result;
}

string PEGTransformerFactory::TransformUpdateSetColumnTarget(PEGTransformer &transformer, const Identifier &column_name,
                                                             const optional<vector<Identifier>> &dot_identifier) {
	if (dot_identifier) {
		throw ParserException("Qualified column names in UPDATE .. SET not supported");
	}
	return column_name.GetIdentifierName();
}

} // namespace duckdb
