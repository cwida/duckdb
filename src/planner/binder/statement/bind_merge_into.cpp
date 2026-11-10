#include "duckdb/planner/binder.hpp"
#include "duckdb/parser/statement/merge_into_statement.hpp"
#include "duckdb/parser/query_node/merge_query_node.hpp"
#include "duckdb/planner/tableref/bound_joinref.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_join.hpp"
#include "duckdb/planner/expression_binder/where_binder.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/planner/operator/logical_merge_into.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression_binder/insert_binder.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/expression/default_expression.hpp"
#include "duckdb/parser/tableref/bound_ref_wrapper.hpp"
#include "duckdb/planner/operator/logical_update.hpp"
#include "duckdb/planner/expression_binder/projection_binder.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/storage/data_table.hpp"
#include <algorithm>

namespace duckdb {

vector<unique_ptr<ParsedExpression>> GenerateColumnReferences(Binder &binder, const vector<BindingAlias> &aliases,
                                                              const vector<Identifier> &names) {
	vector<unique_ptr<ParsedExpression>> result;
	D_ASSERT(aliases.size() == names.size());

	for (idx_t c = 0; c < aliases.size(); c++) {
		auto colref = binder.bind_context.CreateColumnReference(aliases[c], names[c],
		                                                        ColumnBindType::DO_NOT_EXPAND_GENERATED_COLUMNS);
		result.push_back(std::move(colref));
	}
	return result;
}

unique_ptr<BoundMergeIntoAction>
Binder::BindMergeAction(LogicalMergeInto &merge_into, TableCatalogEntry &table, LogicalGet &get, TableIndex proj_index,
                        vector<unique_ptr<Expression>> &expressions, MergeIntoAction &action,
                        const vector<BindingAlias> &source_aliases, const vector<Identifier> &source_names) {
	auto result = make_uniq<BoundMergeIntoAction>();
	result->action_type = action.action_type;
	if (action.condition) {
		ProjectionBinder proj_binder(*this, context, proj_index, expressions, "WHERE clause");
		proj_binder.target_type = LogicalType::BOOLEAN;
		auto cond = proj_binder.Bind(action.condition);
		result->condition = std::move(cond);
	}
	switch (action.action_type) {
	case MergeActionType::MERGE_UPDATE: {
		if (!action.update_info) {
			// empty update list - generate it
			action.update_info = make_uniq<UpdateSetInfo>();
			if (action.column_order == InsertColumnOrder::INSERT_BY_NAME) {
				for (idx_t i = 0; i < source_names.size(); i++) {
					if (action.exclude_columns.count(source_names[i])) {
						continue;
					}
					action.update_info->columns.push_back(source_names[i]);
					action.update_info->expressions.push_back(bind_context.CreateColumnReference(
					    source_aliases[i], source_names[i], ColumnBindType::DO_NOT_EXPAND_GENERATED_COLUMNS));
				}
			} else {
				// UPDATE BY POSITION - get the name list from the table
				for (auto &col : table.GetColumns().Physical()) {
					action.update_info->columns.push_back(col.Name());
				}
				if (source_names.size() != action.update_info->columns.size()) {
					throw BinderException(
					    "Data provided for UPDATE did not match column count in table - expected %d columns but got %d",
					    action.update_info->columns.size(), source_names.size());
				}
				action.update_info->expressions = GenerateColumnReferences(*this, source_aliases, source_names);
			}
		}
		unique_ptr<LogicalOperator> fake_root;
		BindUpdateSet(proj_index, fake_root, *action.update_info, table, result->columns, merge_into.bound_defaults,
		              result->expressions, expressions);

		// bind any additional columns that need to be bound for update constraints
		// FIXME: this is pretty hacky
		// construct a dummy projection and update
		LogicalProjection proj(proj_index, std::move(expressions));
		LogicalUpdate update(table);
		update.return_chunk = merge_into.return_chunk;
		update.columns = std::move(result->columns);
		update.expressions = std::move(result->expressions);
		update.bound_defaults = std::move(merge_into.bound_defaults);
		update.bound_constraints = std::move(merge_into.bound_constraints);
		update.update_is_del_and_insert = false;

		// call BindUpdateConstraints
		table.BindUpdateConstraints(*this, get, proj, update, context);

		// move all moved values back
		merge_into.bound_defaults = std::move(update.bound_defaults);
		merge_into.bound_constraints = std::move(update.bound_constraints);
		expressions = std::move(proj.expressions);
		result->columns = std::move(update.columns);
		result->expressions = std::move(update.expressions);
		result->update_is_del_and_insert = update.update_is_del_and_insert;
		break;
	}
	case MergeActionType::MERGE_INSERT: {
		if (action.column_order == InsertColumnOrder::INSERT_BY_NAME) {
			// INSERT BY NAME - get the name list from the source binder and push it into the table
			if (!action.insert_columns.empty()) {
				throw InternalException("INSERT BY NAME cannot be combined with a column list");
			}
			action.insert_columns = source_names;
		}
		vector<LogicalIndex> named_column_map;
		vector<LogicalType> expected_types;
		physical_index_vector_t<idx_t> column_index_map;
		BindInsertColumnList(table, action.insert_columns, action.default_values, named_column_map, expected_types,
		                     column_index_map);

		vector<ColumnBinding> insert_bindings;
		vector<LogicalType> insert_types;
		if (!action.default_values && action.expressions.empty()) {
			// no expressions: *
			// expand source bindings
			action.expressions = GenerateColumnReferences(*this, source_aliases, source_names);
		}
		CheckInsertColumnCountMismatch(expected_types.size(), action.expressions.size(), !action.insert_columns.empty(),
		                               table.name);
		// explicit expressions - plan them
		for (idx_t i = 0; i < action.expressions.size(); i++) {
			auto &column = table.GetColumns().GetColumn(named_column_map[i]);

			InsertBinder insert_binder(*this, context);
			insert_binder.target_type = table.GetExpectedTypeForInsert(column);

			TryReplaceDefaultExpression(action.expressions[i], column);
			auto insert_expr = insert_binder.Bind(action.expressions[i]);
			auto insert_type = insert_expr->GetReturnType();
			auto expr_index = ColumnBinding::PushExpression(expressions, std::move(insert_expr));
			insert_bindings.emplace_back(proj_index, expr_index);
			insert_types.push_back(std::move(insert_type));
		}

		for (auto &col : table.GetColumns().Physical()) {
			auto storage_idx = col.StorageOid();
			auto mapped_index = column_index_map.empty() ? storage_idx : column_index_map[col.Physical()];
			if (mapped_index == DConstants::INVALID_INDEX) {
				result->expressions.push_back(merge_into.bound_defaults[storage_idx]->Copy());
			} else {
				result->expressions.push_back(table.GetDefaultExpressionForColumn(
				    context, insert_types[mapped_index], col.Type(), insert_bindings[mapped_index],
				    *merge_into.bound_defaults[storage_idx]));
			}
		}
		break;
	}
	case MergeActionType::MERGE_ERROR: {
		// bind the error message (if any)
		for (auto &expr : action.expressions) {
			ProjectionBinder proj_binder(*this, context, proj_index, expressions, "Error Message");
			proj_binder.target_type = LogicalType::VARCHAR;
			auto error_msg = proj_binder.Bind(expr);
			result->expressions.push_back(std::move(error_msg));
		}
		break;
	}
	case MergeActionType::MERGE_DELETE:
	case MergeActionType::MERGE_DO_NOTHING:
		// DELETE / DO NOTHING have nothing extra to bind
		break;
	default:
		throw InternalException("Unsupported merge action type");
	}
	return result;
}

static bool ContainsOperator(LogicalOperator &op, LogicalOperator &target) {
	if (RefersToSameObject(op, target)) {
		return true;
	}
	for (auto &child : op.children) {
		if (ContainsOperator(*child, target)) {
			return true;
		}
	}
	return false;
}

//! Finds the join between the source and the target, subqueries in the join condition add other joins on top of it
static optional_ptr<LogicalOperator> FindMergeJoin(LogicalOperator &op, LogicalOperator &source,
                                                   LogicalOperator &target, idx_t &source_idx) {
	if (op.children.size() == 2) {
		for (idx_t child_idx = 0; child_idx < 2; child_idx++) {
			if (ContainsOperator(*op.children[child_idx], source) &&
			    ContainsOperator(*op.children[1 - child_idx], target)) {
				source_idx = child_idx;
				return op;
			}
		}
	}
	for (auto &child : op.children) {
		auto result = FindMergeJoin(*child, source, target, source_idx);
		if (result) {
			return result;
		}
	}
	return nullptr;
}

void CheckMergeAction(MergeActionCondition condition, MergeActionType action_type) {
	if (condition == MergeActionCondition::WHEN_NOT_MATCHED_BY_TARGET) {
		switch (action_type) {
		case MergeActionType::MERGE_UPDATE:
		case MergeActionType::MERGE_DELETE:
			throw ParserException("WHEN NOT MATCHED (BY TARGET) cannot be combined with UPDATE or DELETE actions - as "
			                      "there is no corresponding row in the target to update or delete.\nDid you mean to "
			                      "use WHEN MATCHED or WHEN NOT MATCHED BY SOURCE?");
		default:
			break;
		}
	}
}

BoundStatement Binder::Bind(MergeIntoStatement &stmt) {
	return Bind(*stmt.node);
}

BoundStatement Binder::BindNode(MergeQueryNode &node) {
	// modification type depends on actions; the target is looked up with the union of the actions' intents, and
	// every MERGE INTO reads the target to match its rows
	DatabaseModificationType modification;
	CatalogAccessIntent target_intent = CatalogAccessIntent::READ_DATA;
	for (auto &action_condition : node.actions) {
		for (auto &action : action_condition.second) {
			switch (action->action_type) {
			case MergeActionType::MERGE_UPDATE:
				modification |= DatabaseModificationType::UPDATE_DATA;
				target_intent |= CatalogAccessIntent::UPDATE_DATA;
				break;
			case MergeActionType::MERGE_DELETE:
				modification |= DatabaseModificationType::DELETE_DATA;
				target_intent |= CatalogAccessIntent::DELETE_DATA;
				break;
			case MergeActionType::MERGE_INSERT:
				modification |= DatabaseModificationType::INSERT_DATA;
				target_intent |= CatalogAccessIntent::INSERT_DATA;
				break;
			default:
				break;
			}
		}
	}

	// bind the target table
	auto target_binder = Binder::CreateBinder(context, this);
	auto table_alias = node.target->alias;
	auto bound_table = target_binder->BindWriteTarget(*node.target, target_intent);
	if (bound_table.plan->type != LogicalOperatorType::LOGICAL_GET) {
		throw BinderException("Can only merge into base tables!");
	}
	auto table_ptr = bound_table.plan->Cast<LogicalGet>().GetTable();
	if (!table_ptr) {
		throw BinderException("Can only merge into base tables!");
	}
	auto &table = *table_ptr;

	bool has_triggers = false;
	auto transaction = table.ParentCatalog().GetCatalogTransaction(context);
	table.ScanTriggers(transaction, [&](CatalogEntry &) { has_triggers = true; });
	if (has_triggers && !global_binder_state->trigger_expanded_tables.count(table)) {
		// if the table is not in the trigger_expanded tables, it means that we're on a top level MERGE INTO
		throw NotImplementedException("MERGE INTO is not supported on tables with triggers");
	}

	GetStatementProperties().RegisterDBModify(table.catalog, context, modification);

	// bind the source
	auto source_binder = Binder::CreateBinder(context, this);
	auto source_binding = source_binder->Bind(*node.source);

	// get the source names/types and collect source table indices for validation
	vector<BindingAlias> source_aliases;
	vector<Identifier> source_names;
	for (auto &binding_entry : source_binder->bind_context.GetBindingsList()) {
		auto &binding = *binding_entry;
		auto &column_names = binding.GetColumnNames();
		for (idx_t c = 0; c < column_names.size(); c++) {
			source_aliases.push_back(binding.GetBindingAlias());
			source_names.push_back(column_names[c]);
		}
	}
	// bind the WHEN NOT MATCHED BY SOURCE / TARGET merge actions
	auto &get = bound_table.plan->Cast<LogicalGet>();
	auto merge_into = make_uniq<LogicalMergeInto>(table);
	merge_into->return_chunk = !node.returning_list.empty();
	merge_into->table_index = GenerateTableIndex();
	auto proj_index = GenerateTableIndex();
	vector<unique_ptr<Expression>> projection_expressions;

	// bind table constraints/default values in case these are referenced
	auto &catalog_name = table.ParentCatalog().GetName();
	BindDefaultValues(table.GetColumns(), merge_into->bound_defaults, catalog_name.GetIdentifierName(),
	                  table.ParentSchema().GetSchemaPath());

	merge_into->bound_constraints = BindConstraints(table);

	for (auto &entry : node.actions) {
		if (entry.first == MergeActionCondition::WHEN_MATCHED) {
			continue;
		}
		auto &action_binder =
		    entry.first == MergeActionCondition::WHEN_NOT_MATCHED_BY_TARGET ? *source_binder : *target_binder;
		vector<unique_ptr<BoundMergeIntoAction>> bound_actions;
		for (auto &action : entry.second) {
			CheckMergeAction(entry.first, action->action_type);
			bound_actions.push_back(action_binder.BindMergeAction(
			    *merge_into, table, get, proj_index, projection_expressions, *action, source_aliases, source_names));
		}
		merge_into->actions.emplace(entry.first, std::move(bound_actions));
	}

	// bind the join between the source and target
	// our conditions determine the join type we need
	// if we have WHEN NOT MATCHED BY SOURCE we need all source rows -> RIGHT join
	// if we have WHEN NOT MATCHED BY TARGET we need all target rows -> LEFT join
	// if we have both                                               -> FULL join
	// if we only have WHEN MATCHED we only need matches             -> INNER join
	JoinRef join;
	auto has_not_matched_by_source = node.actions.count(MergeActionCondition::WHEN_NOT_MATCHED_BY_SOURCE) > 0;
	auto has_not_matched_by_target = node.actions.count(MergeActionCondition::WHEN_NOT_MATCHED_BY_TARGET) > 0;
	if (has_not_matched_by_source && has_not_matched_by_target) {
		join.type = JoinType::OUTER;
	} else if (has_not_matched_by_source) {
		join.type = JoinType::RIGHT;
	} else if (has_not_matched_by_target) {
		join.type = JoinType::LEFT;
	} else {
		join.type = JoinType::INNER;
	}
	auto &source_op = *source_binding.plan;
	join.left = make_uniq<BoundRefWrapper>(std::move(source_binding), std::move(source_binder));
	join.right = make_uniq<BoundRefWrapper>(std::move(bound_table), std::move(target_binder));
	if (node.join_condition) {
		join.condition = std::move(node.join_condition);
	} else {
		join.using_columns = std::move(node.using_columns);
	}
	auto bound_join_node = Bind(join);

	auto root = std::move(bound_join_node.plan);
	idx_t source_idx = 0;
	auto merge_join = FindMergeJoin(*root, source_op, get, source_idx);
	if (!merge_join) {
		throw NotImplementedException("Expected a join after binding a join operator - but got a %s", root->type);
	}
	auto join_ref = reference<LogicalOperator>(*merge_join);
	// CreatePlan can turn a RIGHT join into a LEFT join - in that case the children are reversed
	bool inverted = source_idx == 1;
	auto &source = join_ref.get().children[inverted ? 1 : 0];

	// bind WHEN_MATCHED merge actions (can contain references to both source and target)
	for (auto &entry : node.actions) {
		if (entry.first != MergeActionCondition::WHEN_MATCHED) {
			continue;
		}
		vector<unique_ptr<BoundMergeIntoAction>> bound_actions;
		for (auto &action : entry.second) {
			CheckMergeAction(entry.first, action->action_type);
			bound_actions.push_back(BindMergeAction(*merge_into, table, get, proj_index, projection_expressions,
			                                        *action, source_aliases, source_names));
		}
		merge_into->actions.emplace(entry.first, std::move(bound_actions));
	}

	// plan merge action subqueries
	for (auto &expr : projection_expressions) {
		PlanSubqueries(expr, root);
	}

	if (has_not_matched_by_source) {
		// if we have "has_not_matched_by_source" we need to push an extra marker into the source
		// this marker tells us if we have found a source match or not
		auto new_proj_index = GenerateTableIndex();
		auto source_bindings = source->GetColumnBindings();
		auto proj = LogicalProjection::CreateIdentity(new_proj_index, std::move(source));

		// insert the source marker
		auto marker = make_uniq<BoundConstantExpression>(Value::INTEGER(42));
		marker->SetAlias("source_marker");
		ColumnBinding source_marker;
		auto source_marker_idx = ColumnBinding::PushExpression(proj->expressions, std::move(marker));
		source_marker = ColumnBinding(new_proj_index, source_marker_idx);
		source = std::move(proj);

		// remap the source references above the marker projection, including those of correlated subqueries
		CorrelatedColumnBindingReplacer replacer;
		replacer.AddReplacements(source_bindings,
		                         LogicalOperator::GenerateColumnBindings(new_proj_index, source_bindings.size()));
		replacer.stop_operator = *source;
		replacer.VisitOperator(*root);
		replacer.VisitOperator(*merge_into);
		for (auto &expr : projection_expressions) {
			replacer.VisitExpression(&expr);
		}

		// push a reference
		merge_into->source_marker = projection_expressions.size();
		auto marker_ref = make_uniq<BoundColumnRefExpression>(LogicalType::INTEGER, source_marker);
		marker_ref->SetAlias("source_marker");
		projection_expressions.push_back(std::move(marker_ref));
	}

	// Check if we have a DELETE action
	bool has_delete_action = false;
	for (auto &entry : merge_into->actions) {
		for (auto &action : entry.second) {
			if (action->action_type == MergeActionType::MERGE_DELETE) {
				has_delete_action = true;
				break;
			}
		}
		if (has_delete_action) {
			break;
		}
	}

	// If RETURNING is present and we have a DELETE action, add all physical columns to the scan
	// so we can pass them through instead of fetching by row ID in PhysicalDelete.
	// Generated columns will be computed in the RETURNING projection by the binder.
	if (has_delete_action) {
		if (!node.returning_list.empty()) {
			// Use the overloaded helper to add physical columns to the scan and build projection expressions
			auto &target_binding = join_ref.get().children[inverted ? 0 : 1];
			BindDeleteReturningColumns(table, get, merge_into->delete_return_columns, projection_expressions,
			                           *target_binding);
		} else if (table.IsDuckTable()) {
			// Only optimize for DuckDB tables (not attached external tables like SQLite)
			auto &storage = table.GetStorage();
			if (storage.HasUniqueIndexes()) {
				auto &target_binding = join_ref.get().children[inverted ? 0 : 1];
				BindDeleteIndexColumns(table, get, merge_into->delete_return_columns, projection_expressions,
				                       *target_binding);
			}
		}
	}

	merge_into->row_id_start = projection_expressions.size();
	// Row ID columns must remain last: PhysicalMergeInto treats the trailing columns as one composite key.
	BindRowIdColumns(table, get, projection_expressions);

	auto proj = make_uniq<LogicalProjection>(proj_index, std::move(projection_expressions));
	proj->AddChild(std::move(root));

	merge_into->AddChild(std::move(proj));

	if (!node.returning_list.empty()) {
		auto merge_table_index = merge_into->table_index;
		unique_ptr<LogicalOperator> index_as_logicaloperator = std::move(merge_into);

		// add the merge_action virtual column
		virtual_column_map_t virtual_columns;
		virtual_columns.insert(make_pair(VIRTUAL_COLUMN_START, TableColumn("merge_action", LogicalType::VARCHAR)));
		return BindReturning(std::move(node.returning_list), table, table_alias, merge_table_index,
		                     std::move(index_as_logicaloperator), std::move(virtual_columns));
	}

	BoundStatement result;
	result.plan = std::move(merge_into);
	result.names = {"Count"};
	result.types = {LogicalType::BIGINT};

	auto &properties = GetStatementProperties();
	properties.return_type = StatementReturnType::CHANGED_ROWS;
	return result;
}

} // namespace duckdb
