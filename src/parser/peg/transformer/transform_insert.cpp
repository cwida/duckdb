#include "duckdb/parser/peg/ast/insert_values.hpp"
#include "duckdb/parser/peg/ast/on_conflict_expression_target.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/statement/insert_statement.hpp"
#include "duckdb/parser/query_node/insert_query_node.hpp"

namespace duckdb {

unique_ptr<SQLStatement> PEGTransformerFactory::TransformInsertStatement(
    PEGTransformer &transformer, optional<CommonTableExpressionMap> with_clause,
    const optional<OnConflictAction> &or_action, unique_ptr<BaseTableRef> insert_target,
    const optional<InsertColumnOrder> &by_name_or_position, const optional<vector<string>> &insert_column_list,
    bool has_overriding_clause, InsertValues insert_values, optional<unique_ptr<OnConflictInfo>> on_conflict_clause,
    optional<vector<unique_ptr<ParsedExpression>>> returning_clause) {
	auto result = make_uniq<InsertStatement>();
	auto &node = *result->node;
	if (with_clause) {
		node.cte_map = std::move(*with_clause);
	}
	node.qualified_name = insert_target->GetQualifiedName();
	node.column_order = by_name_or_position ? *by_name_or_position : InsertColumnOrder::INSERT_BY_POSITION;
	if (insert_column_list) {
		node.columns = StringsToIdentifiers(*insert_column_list);
	}
	if (!node.columns.empty() && insert_values.default_values) {
		throw ParserException(
		    "You can not provide both a column list and DEFAULT VALUES, please remove one of the two");
	}
	if (has_overriding_clause && insert_values.default_values) {
		throw ParserException("OVERRIDING SYSTEM|USER VALUE can not be combined with DEFAULT VALUES, "
		                      "please remove one of the two");
	}
	if (insert_values.default_values) {
		node.default_values = true;
	}
	if (insert_values.select_statement) {
		node.select_statement = std::move(insert_values.select_statement);
	}
	auto action = or_action.value_or(OnConflictAction::THROW);
	if (on_conflict_clause) {
		if (action != OnConflictAction::THROW) {
			// OR REPLACE | OR IGNORE are shorthands for the ON CONFLICT clause
			throw ParserException("You can not provide both OR REPLACE|IGNORE and an ON CONFLICT clause, please remove "
			                      "the first if you want to have more granular control");
		}
		node.on_conflict_info = std::move(*on_conflict_clause);
		node.table_ref = std::move(insert_target);
	} else if (action != OnConflictAction::THROW) {
		auto on_conflict_info = make_uniq<OnConflictInfo>();
		on_conflict_info->action_type = action;
		node.on_conflict_info = std::move(on_conflict_info);
		node.table_ref = std::move(insert_target);
	}
	if (returning_clause) {
		node.returning_list = std::move(*returning_clause);
	}
	return std::move(result);
}

OnConflictAction PEGTransformerFactory::TransformInsertOrReplace(PEGTransformer &transformer) {
	return OnConflictAction::REPLACE;
}

OnConflictAction PEGTransformerFactory::TransformInsertOrIgnore(PEGTransformer &transformer) {
	return OnConflictAction::NOTHING;
}

unique_ptr<BaseTableRef> PEGTransformerFactory::TransformInsertTarget(PEGTransformer &transformer,
                                                                      unique_ptr<BaseTableRef> base_table_name,
                                                                      const optional<Identifier> &insert_alias) {
	if (insert_alias) {
		base_table_name->alias = *insert_alias;
	}
	return base_table_name;
}

Identifier PEGTransformerFactory::TransformInsertAlias(PEGTransformer &transformer, const Identifier &identifier) {
	return identifier;
}

unique_ptr<OnConflictInfo>
PEGTransformerFactory::TransformOnConflictClause(PEGTransformer &transformer,
                                                 optional<OnConflictExpressionTarget> on_conflict_target,
                                                 unique_ptr<OnConflictInfo> on_conflict_action) {
	if (on_conflict_target) {
		on_conflict_action->indexed_columns = on_conflict_target->indexed_columns;
		if (on_conflict_target->where_clause) {
			on_conflict_action->condition = std::move(on_conflict_target->where_clause);
		}
	}
	return on_conflict_action;
}

OnConflictExpressionTarget
PEGTransformerFactory::TransformOnConflictExpressionTarget(PEGTransformer &transformer,
                                                           const vector<string> &column_id_list,
                                                           optional<unique_ptr<ParsedExpression>> where_clause) {
	OnConflictExpressionTarget result;
	result.indexed_columns = StringsToIdentifiers(column_id_list);
	if (where_clause) {
		result.where_clause = std::move(*where_clause);
	}
	return result;
}

OnConflictExpressionTarget PEGTransformerFactory::TransformOnConflictIndexTarget(PEGTransformer &transformer,
                                                                                 const Identifier &constraint_name) {
	throw NotImplementedException("ON CONSTRAINT conflict target is not supported yet");
}

unique_ptr<OnConflictInfo>
PEGTransformerFactory::TransformOnConflictUpdate(PEGTransformer &transformer,
                                                 unique_ptr<UpdateSetInfo> update_set_clause,
                                                 optional<unique_ptr<ParsedExpression>> where_clause) {
	auto result = make_uniq<OnConflictInfo>();
	result->action_type = OnConflictAction::UPDATE;
	result->set_info = std::move(update_set_clause);
	if (where_clause) {
		result->set_info->condition = std::move(*where_clause);
	}
	return result;
}

unique_ptr<OnConflictInfo> PEGTransformerFactory::TransformOnConflictNothing(PEGTransformer &transformer) {
	auto result = make_uniq<OnConflictInfo>();
	result->action_type = OnConflictAction::NOTHING;
	return result;
}

InsertValues PEGTransformerFactory::TransformSelectInsertValues(PEGTransformer &transformer,
                                                                unique_ptr<SelectStatement> select_statement_internal) {
	InsertValues result;
	result.select_statement = std::move(select_statement_internal);
	return result;
}

InsertValues PEGTransformerFactory::TransformDefaultValues(PEGTransformer &transformer) {
	InsertValues result;
	result.default_values = true;
	return result;
}

InsertColumnOrder PEGTransformerFactory::TransformInsertByName(PEGTransformer &transformer) {
	return InsertColumnOrder::INSERT_BY_NAME;
}

InsertColumnOrder PEGTransformerFactory::TransformInsertByPosition(PEGTransformer &transformer) {
	return InsertColumnOrder::INSERT_BY_POSITION;
}

vector<string> PEGTransformerFactory::TransformInsertColumnList(PEGTransformer &transformer,
                                                                const vector<string> &column_list) {
	return column_list;
}

vector<string> PEGTransformerFactory::TransformColumnList(PEGTransformer &transformer,
                                                          const vector<Identifier> &col_id) {
	return IdentifiersToStrings(col_id);
}

vector<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformReturningClause(PEGTransformer &transformer,
                                                vector<unique_ptr<ParsedExpression>> target_list) {
	return target_list;
}

} // namespace duckdb
