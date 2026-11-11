#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/statement/delete_statement.hpp"
#include "duckdb/parser/query_node/delete_query_node.hpp"

namespace duckdb {

unique_ptr<SQLStatement> PEGTransformerFactory::TransformDeleteStatement(
    PEGTransformer &transformer, optional<CommonTableExpressionMap> with_clause,
    unique_ptr<BaseTableRef> target_opt_alias, optional<vector<unique_ptr<TableRef>>> delete_using_clause,
    optional<unique_ptr<ParsedExpression>> where_clause,
    optional<vector<unique_ptr<ParsedExpression>>> returning_clause) {
	auto result = make_uniq<DeleteStatement>();
	auto &node = *result->node;
	if (with_clause && !with_clause->map.empty()) {
		node.cte_map = std::move(*with_clause);
	}
	node.table = std::move(target_opt_alias);
	if (delete_using_clause) {
		node.using_clauses = std::move(*delete_using_clause);
	}
	if (where_clause) {
		node.condition = std::move(*where_clause);
	}
	if (returning_clause) {
		node.returning_list = std::move(*returning_clause);
	}
	return std::move(result);
}

unique_ptr<BaseTableRef> PEGTransformerFactory::TransformTargetOptAlias(PEGTransformer &transformer,
                                                                        unique_ptr<BaseTableRef> base_table_name,
                                                                        const optional<Identifier> &target_alias) {
	if (target_alias && !target_alias->empty()) {
		base_table_name->alias = Identifier(*target_alias);
	}
	return base_table_name;
}

Identifier PEGTransformerFactory::TransformTargetAlias(PEGTransformer &transformer, const bool &has_result,
                                                       const Identifier &col_id) {
	return col_id;
}

vector<unique_ptr<TableRef>> PEGTransformerFactory::TransformDeleteUsingClause(PEGTransformer &transformer,
                                                                               vector<unique_ptr<TableRef>> table_ref) {
	return table_ref;
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformTruncateStatement(PEGTransformer &transformer,
                                                                           const bool &has_result,
                                                                           unique_ptr<BaseTableRef> base_table_name,
                                                                           optional<bool> truncate_options) {
	if (truncate_options && *truncate_options) {
		throw NotImplementedException("TRUNCATE ... CASCADE is not implemented");
	}
	auto result = make_uniq<DeleteStatement>();
	result->node->table = std::move(base_table_name);
	return std::move(result);
}

bool PEGTransformerFactory::TransformTruncateAliasOptions(PEGTransformer &transformer, const Identifier &target_alias,
                                                          optional<bool> truncate_drop_behavior) {
	// the alias is ignored, the result is whether CASCADE was given
	return truncate_drop_behavior && *truncate_drop_behavior;
}

bool PEGTransformerFactory::TransformTruncateDropBehavior(PEGTransformer &transformer, const bool &drop_behavior) {
	return drop_behavior;
}

} // namespace duckdb
