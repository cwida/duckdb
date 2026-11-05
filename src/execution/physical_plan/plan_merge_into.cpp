#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/execution/operator/persistent/physical_merge_into.hpp"
#include "duckdb/execution/operator/persistent/physical_delete.hpp"
#include "duckdb/execution/operator/persistent/physical_insert.hpp"
#include "duckdb/execution/operator/persistent/physical_update.hpp"
#include "duckdb/execution/operator/projection/physical_projection.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_delete.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"
#include "duckdb/planner/operator/logical_merge_into.hpp"
#include "duckdb/planner/operator/logical_update.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"

namespace duckdb {

PhysicalOperator &PlanMergeActionSource(PhysicalPlanGenerator &planner, PhysicalOperator &merge_input,
                                        MergeActionCondition condition, MergeIntoOperator &action) {
	auto &source = planner.Make<PhysicalMergeActionSource>(merge_input.types, merge_input.estimated_cardinality,
	                                                       condition, action.action_type, true);
	action.source = source.Cast<PhysicalMergeActionSource>();
	action.source->merge_input = merge_input;
	if (action.expressions.empty()) {
		return source;
	}
	// the action has expressions (e.g. the values of an INSERT) - execute them in a projection
	vector<LogicalType> projection_types;
	for (auto &expr : action.expressions) {
		projection_types.push_back(expr->GetReturnType());
	}
	auto &projection = planner.Make<PhysicalProjection>(std::move(projection_types), std::move(action.expressions),
	                                                    merge_input.estimated_cardinality);
	projection.children.push_back(source);
	action.expressions.clear();
	return projection;
}

//===--------------------------------------------------------------------===//
// DuckDB merge into planning
//===--------------------------------------------------------------------===//
unique_ptr<MergeIntoOperator> PlanMergeIntoAction(ClientContext &context, LogicalMergeInto &op,
                                                  PhysicalPlanGenerator &planner, PhysicalOperator &plan,
                                                  MergeActionCondition condition, BoundMergeIntoAction &action) {
	auto result = make_uniq<MergeIntoOperator>();

	result->action_type = action.action_type;
	result->condition = std::move(action.condition);
	vector<unique_ptr<BoundConstraint>> bound_constraints;
	for (auto &constraint : op.bound_constraints) {
		bound_constraints.push_back(constraint->Copy());
	}
	auto return_types = op.types;
	if (op.return_chunk) {
		// for RETURNING, the last column is the merge_action - this is added in the merge itself
		return_types.pop_back();
	}

	auto cardinality = op.estimated_cardinality;
	switch (action.action_type) {
	case MergeActionType::MERGE_UPDATE: {
		vector<unique_ptr<Expression>> defaults;
		for (auto &def : op.bound_defaults) {
			defaults.push_back(def->Copy());
		}
		auto &action_input = PlanMergeActionSource(planner, plan, condition, *result);
		result->op = planner.Make<PhysicalUpdate>(std::move(return_types), op.table.Cast<DuckTableEntry>(),
		                                          op.table.GetStorage(), std::move(action.columns),
		                                          std::move(action.expressions), std::move(defaults),
		                                          std::move(bound_constraints), cardinality, op.return_chunk,
		                                          /*capture_old_rows=*/false, /*old_row_columns=*/vector<idx_t>(),
		                                          /*row_id_handling=*/RowIdHandling::ASSUME_UNIQUE);
		auto &cast_update = result->op->Cast<PhysicalUpdate>();
		cast_update.update_is_del_and_insert = action.update_is_del_and_insert;
		result->op->children.push_back(action_input);
		break;
	}
	case MergeActionType::MERGE_DELETE: {
		// Use delete_return_columns if available (for optimized RETURNING path)
		vector<idx_t> return_columns = op.delete_return_columns;
		auto &action_input = PlanMergeActionSource(planner, plan, condition, *result);
		result->op = planner.Make<PhysicalDelete>(std::move(return_types), op.table.Cast<DuckTableEntry>(),
		                                          op.table.GetStorage(), std::move(bound_constraints), op.row_id_start,
		                                          cardinality, op.return_chunk, std::move(return_columns));
		result->op->children.push_back(action_input);
		break;
	}
	case MergeActionType::MERGE_INSERT: {
		vector<unique_ptr<Expression>> set_expressions;
		vector<PhysicalIndex> set_columns;
		vector<LogicalType> set_types;
		unordered_set<column_t> on_conflict_filter;
		vector<column_t> columns_to_fetch;

		// transform expressions if required
		if (!action.column_index_map.empty()) {
			//! Deprecated: plan expressions for default expressions, now set at bind time
			vector<unique_ptr<Expression>> new_expressions;
			for (auto &col : op.table.GetColumns().Physical()) {
				auto storage_idx = col.StorageOid();
				auto mapped_index = action.column_index_map[col.Physical()];
				if (mapped_index == DConstants::INVALID_INDEX) {
					// push default value
					new_expressions.push_back(op.bound_defaults[storage_idx]->Copy());
				} else {
					// push reference
					new_expressions.push_back(std::move(action.expressions[mapped_index]));
				}
			}
			action.expressions = std::move(new_expressions);
		}
		result->expressions = std::move(action.expressions);
		auto &action_input = PlanMergeActionSource(planner, plan, condition, *result);

		result->op = planner.Make<PhysicalInsert>(
		    std::move(return_types), op.table.Cast<DuckTableEntry>(), std::move(bound_constraints),
		    std::move(set_expressions), std::move(set_columns), std::move(set_types), cardinality, op.return_chunk,
		    !op.return_chunk, OnConflictAction::THROW, nullptr, nullptr, std::move(on_conflict_filter),
		    std::move(columns_to_fetch), false);
		result->op->children.push_back(action_input);
		break;
	}
	case MergeActionType::MERGE_ERROR:
		result->expressions = std::move(action.expressions);
		break;
	case MergeActionType::MERGE_DO_NOTHING:
		break;
	default:
		throw InternalException("Unsupported merge action");
	}
	return result;
}

PhysicalOperator &DuckCatalog::PlanMergeInto(ClientContext &context, PhysicalPlanGenerator &planner,
                                             LogicalMergeInto &op, PhysicalOperator &plan) {
	map<MergeActionCondition, vector<unique_ptr<MergeIntoOperator>>> actions;

	// plan the merge into clauses
	// FIXME: disable parallelism when we have multiple INSERTs because they do not work nicely together currently
	idx_t append_count = 0;
	for (auto &entry : op.actions) {
		vector<unique_ptr<MergeIntoOperator>> planned_actions;
		for (auto &action : entry.second) {
			if (action->action_type == MergeActionType::MERGE_INSERT) {
				append_count++;
			}
			if (action->action_type == MergeActionType::MERGE_UPDATE && action->update_is_del_and_insert) {
				append_count++;
			}
			planned_actions.push_back(PlanMergeIntoAction(context, op, planner, plan, entry.first, *action));
		}
		actions.emplace(entry.first, std::move(planned_actions));
	}

	bool parallel = append_count <= 1 && !op.return_chunk;
	// multiple operators appending to the same table cannot run concurrently - run their actions one after the other
	bool serialize_actions = append_count > 1;

	return planner.Make<PhysicalMergeInto>(op.types, plan, std::move(actions), op.row_id_start, op.source_marker,
	                                       parallel, op.return_chunk, serialize_actions);
}

//===--------------------------------------------------------------------===//
// Generic merge into planning
//===--------------------------------------------------------------------===//
//! The row id columns are the trailing columns of the merge input - the merge binder appends them with the same
//! helper that a regular DELETE/UPDATE uses, so they are in the order that the catalog expects them in
static vector<unique_ptr<Expression>> PlanMergeRowIdReferences(LogicalMergeInto &op, PhysicalOperator &plan) {
	vector<unique_ptr<Expression>> result;
	for (idx_t i = op.row_id_start; i < plan.types.size(); i++) {
		result.push_back(make_uniq<BoundReferenceExpression>(plan.types[i], i));
	}
	return result;
}

//! The operators of a regular INSERT/UPDATE read the values of a row in the types of the table - cast the values that
//! the action generates, so that the catalog gets the same input that it gets for a regular INSERT/UPDATE
static void CastActionExpressionsToColumnTypes(ClientContext &context, TableCatalogEntry &table,
                                               vector<unique_ptr<Expression>> &expressions,
                                               const vector<PhysicalIndex> &columns) {
	auto &table_columns = table.GetColumns();
	for (idx_t i = 0; i < expressions.size(); i++) {
		auto &target_type = columns.empty() ? table_columns.GetColumn(PhysicalIndex(i)).Type()
		                                    : table_columns.GetColumn(columns[i]).Type();
		if (expressions[i]->GetReturnType() == target_type) {
			continue;
		}
		expressions[i] = BoundCastExpression::AddCastToType(context, std::move(expressions[i]), target_type);
	}
}

static vector<unique_ptr<BoundConstraint>> CopyBoundConstraints(LogicalMergeInto &op) {
	vector<unique_ptr<BoundConstraint>> result;
	for (auto &constraint : op.bound_constraints) {
		result.push_back(constraint->Copy());
	}
	return result;
}

//! Plans a merge action through the regular INSERT/UPDATE/DELETE planning of the catalog - the source of the action
//! takes the place of the child plan that the operators of a regular INSERT/UPDATE/DELETE read from
static unique_ptr<MergeIntoOperator> PlanGenericMergeIntoAction(ClientContext &context, LogicalMergeInto &op,
                                                                PhysicalPlanGenerator &planner, PhysicalOperator &plan,
                                                                MergeActionCondition condition,
                                                                BoundMergeIntoAction &action) {
	auto result = make_uniq<MergeIntoOperator>();
	result->action_type = action.action_type;
	result->condition = std::move(action.condition);

	auto &catalog = op.table.catalog;
	// the operators of an action emit the same rows as those of a regular INSERT/UPDATE/DELETE - resolve the types
	// and cardinality of the logical operators in the same manner
	auto cardinality = op.estimated_cardinality;
	switch (action.action_type) {
	case MergeActionType::MERGE_UPDATE: {
		// a regular UPDATE reads the new values of a row followed by its row id - project the rows of the action into
		// that layout so that we can plan the update exactly like a regular update
		vector<unique_ptr<Expression>> select_list;
		vector<unique_ptr<Expression>> update_expressions;
		CastActionExpressionsToColumnTypes(context, op.table, action.expressions, action.columns);
		for (auto &expr : action.expressions) {
			update_expressions.push_back(
			    make_uniq<BoundReferenceExpression>(expr->GetReturnType(), select_list.size()));
			select_list.push_back(std::move(expr));
		}
		for (auto &row_id_reference : PlanMergeRowIdReferences(op, plan)) {
			select_list.push_back(std::move(row_id_reference));
		}
		result->expressions = std::move(select_list);
		auto &action_input = PlanMergeActionSource(planner, plan, condition, *result);

		LogicalUpdate update(op.table);
		update.columns = std::move(action.columns);
		update.expressions = std::move(update_expressions);
		update.update_is_del_and_insert = action.update_is_del_and_insert;
		update.bound_constraints = CopyBoundConstraints(op);
		for (auto &def : op.bound_defaults) {
			update.bound_defaults.push_back(def->Copy());
		}
		update.estimated_cardinality = cardinality;
		update.ResolveOperatorTypes();
		result->op = catalog.PlanUpdate(context, planner, update, action_input);
		break;
	}
	case MergeActionType::MERGE_DELETE: {
		// a regular DELETE reads the row ids out of its child plan - reference them within the rows of the action
		auto &action_input = PlanMergeActionSource(planner, plan, condition, *result);

		LogicalDelete delete_op(op.table, TableIndex(0));
		delete_op.expressions = PlanMergeRowIdReferences(op, plan);
		delete_op.bound_constraints = CopyBoundConstraints(op);
		delete_op.estimated_cardinality = cardinality;
		delete_op.ResolveOperatorTypes();
		result->op = catalog.PlanDelete(context, planner, delete_op, action_input);
		break;
	}
	case MergeActionType::MERGE_INSERT: {
		// the expressions of the action generate the values to insert - the projection on top of the action source
		// emits exactly the rows that a regular INSERT reads
		if (!action.column_index_map.empty()) {
			//! Deprecated: plan expressions for default expressions, now set at bind time
			vector<unique_ptr<Expression>> new_expressions;
			for (auto &col : op.table.GetColumns().Physical()) {
				auto storage_idx = col.StorageOid();
				auto mapped_index = action.column_index_map[col.Physical()];
				if (mapped_index == DConstants::INVALID_INDEX) {
					new_expressions.push_back(op.bound_defaults[storage_idx]->Copy());
				} else {
					new_expressions.push_back(std::move(action.expressions[mapped_index]));
				}
			}
			action.expressions = std::move(new_expressions);
		}
		CastActionExpressionsToColumnTypes(context, op.table, action.expressions, vector<PhysicalIndex>());
		result->expressions = std::move(action.expressions);
		auto &action_input = PlanMergeActionSource(planner, plan, condition, *result);

		LogicalInsert insert(op.table, TableIndex(0));
		insert.bound_constraints = CopyBoundConstraints(op);
		for (auto &def : op.bound_defaults) {
			insert.bound_defaults.push_back(def->Copy());
		}
		insert.estimated_cardinality = cardinality;
		insert.ResolveOperatorTypes();
		result->op = catalog.PlanInsert(context, planner, insert, action_input);
		break;
	}
	case MergeActionType::MERGE_ERROR:
		result->expressions = std::move(action.expressions);
		break;
	case MergeActionType::MERGE_DO_NOTHING:
		break;
	default:
		throw InternalException("Unsupported merge action");
	}
	return result;
}

PhysicalOperator &Catalog::PlanMergeInto(ClientContext &context, PhysicalPlanGenerator &planner, LogicalMergeInto &op,
                                         PhysicalOperator &plan) {
	if (op.return_chunk) {
		throw NotImplementedException("RETURNING clause not yet supported for MERGE INTO for database type \"%s\"",
		                              GetCatalogType());
	}
	map<MergeActionCondition, vector<unique_ptr<MergeIntoOperator>>> actions;
	idx_t append_count = 0;
	for (auto &entry : op.actions) {
		vector<unique_ptr<MergeIntoOperator>> planned_actions;
		for (auto &action : entry.second) {
			if (action->action_type == MergeActionType::MERGE_INSERT) {
				append_count++;
			}
			if (action->action_type == MergeActionType::MERGE_UPDATE && action->update_is_del_and_insert) {
				append_count++;
			}
			planned_actions.push_back(PlanGenericMergeIntoAction(context, op, planner, plan, entry.first, *action));
		}
		actions.emplace(entry.first, std::move(planned_actions));
	}
	// multiple operators appending to the same table cannot run concurrently - run their actions one after the other
	bool serialize_actions = append_count > 1;
	return planner.Make<PhysicalMergeInto>(op.types, plan, std::move(actions), op.row_id_start, op.source_marker,
	                                       !serialize_actions, op.return_chunk, serialize_actions);
}

PhysicalOperator &PhysicalPlanGenerator::CreatePlan(LogicalMergeInto &op) {
	// planning the child consumes its logical operators - estimate the cardinality beforehand
	op.estimated_cardinality = op.EstimateCardinality(context);
	auto &plan = CreatePlan(*op.children[0]);
	D_ASSERT(op.children.size() == 1);
	dependencies.AddDependency(op.table);
	return op.table.catalog.PlanMergeInto(context, *this, op, plan);
}

} // namespace duckdb
