#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/statement/update_statement.hpp"
#include "duckdb/parser/query_node/update_query_node.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/tableref/bound_joinref.hpp"
#include "duckdb/planner/constraints/bound_check_constraint.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_default_expression.hpp"
#include "duckdb/planner/expression_binder/update_binder.hpp"
#include "duckdb/planner/expression_binder/where_binder.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_update.hpp"
#include "duckdb/storage/data_table.hpp"

#include <algorithm>

namespace duckdb {

void Binder::BindUpdateSet(TableIndex proj_index, unique_ptr<LogicalOperator> &root, UpdateSetInfo &set_info,
                           TableCatalogEntry &table, vector<PhysicalIndex> &columns,
                           const vector<unique_ptr<Expression>> &bound_defaults,
                           vector<unique_ptr<Expression>> &update_expressions,
                           vector<unique_ptr<Expression>> &projection_expressions, bool prioritize_table_when_binding) {
	D_ASSERT(set_info.columns.size() == set_info.expressions.size());

	Binder *expr_binder_ptr = this;
	shared_ptr<Binder> binder_with_search_path;

	if (prioritize_table_when_binding) {
		binder_with_search_path =
		    CreateBinderWithSearchPath(table.ParentCatalog().GetName(), table.ParentSchema().GetSchemaPath());
		expr_binder_ptr = binder_with_search_path.get();
	}

	for (idx_t i = 0; i < set_info.columns.size(); i++) {
		auto &colname = set_info.columns[i];
		auto &expr = set_info.expressions[i];
		if (!table.ColumnExists(colname)) {
			vector<string> column_names;
			for (auto &col : table.GetColumns().Physical()) {
				column_names.emplace_back(col.Name().GetIdentifierName());
			}
			auto candidates =
			    StringUtil::CandidatesErrorMessage(column_names, colname.GetIdentifierName(), "Did you mean");
			throw BinderException("Referenced update column %s not found in table!\n%s", colname.GetIdentifierName(),
			                      candidates);
		}
		auto &column = table.GetColumn(colname);
		if (column.Generated()) {
			throw BinderException("Cant update column %s because it is a generated column!", column.Name());
		}
		if (std::find(columns.begin(), columns.end(), column.Physical()) != columns.end()) {
			throw BinderException("Multiple assignments to same column %s", colname);
		}
		columns.push_back(column.Physical());
		if (expr->GetExpressionType() == ExpressionType::VALUE_DEFAULT) {
			auto bound_default = bound_defaults[column.StorageOid()]->Copy();
			auto expr_index = ColumnBinding::PushExpression(projection_expressions, std::move(bound_default));
			update_expressions.push_back(
			    make_uniq<BoundColumnRefExpression>(column.Type(), ColumnBinding(proj_index, expr_index)));
		} else {
			UpdateBinder binder(*expr_binder_ptr, context);
			binder.target_type = table.GetExpectedTypeForInsert(column);
			auto bound_expr = binder.Bind(expr);
			if (root) {
				PlanSubqueries(bound_expr, root);
			}

			auto bound_type = bound_expr->GetReturnType();
			auto expr_index = ColumnBinding::PushExpression(projection_expressions, std::move(bound_expr));
			auto source_binding = ColumnBinding(proj_index, expr_index);

			update_expressions.push_back(table.GetDefaultExpressionForColumn(
			    context, bound_type, column.Type(), source_binding, *bound_defaults[column.StorageOid()]));
		}
	}
}

// This creates a LogicalProjection and moves 'root' into it as a child
// unless there are no expressions to project, in which case it just returns 'root'
unique_ptr<LogicalOperator> Binder::BindUpdateSet(LogicalOperator &op, unique_ptr<LogicalOperator> root,
                                                  UpdateSetInfo &set_info, TableCatalogEntry &table,
                                                  const vector<unique_ptr<Expression>> &bound_defaults,
                                                  vector<PhysicalIndex> &columns, bool prioritize_table_when_binding) {
	auto proj_index = GenerateTableIndex();

	vector<unique_ptr<Expression>> projection_expressions;
	BindUpdateSet(proj_index, root, set_info, table, columns, bound_defaults, op.expressions, projection_expressions,
	              prioritize_table_when_binding);
	if (op.type != LogicalOperatorType::LOGICAL_UPDATE && projection_expressions.empty()) {
		return root;
	}
	// now create the projection
	auto proj = make_uniq<LogicalProjection>(proj_index, std::move(projection_expressions));
	proj->AddChild(std::move(root));
	return unique_ptr_cast<LogicalProjection, LogicalOperator>(std::move(proj));
}

void Binder::BindRowIdColumns(TableCatalogEntry &table, LogicalGet &get, vector<unique_ptr<Expression>> &expressions) {
	auto row_id_columns = table.GetRowIdColumns();
	auto virtual_columns = table.GetVirtualColumns();
	auto &column_ids = get.GetColumnIds();
	for (auto &row_id_column : row_id_columns) {
		auto row_id_entry = virtual_columns.find(row_id_column);
		if (row_id_entry == virtual_columns.end()) {
			throw InternalException(
			    "BindRowIdColumns could not find the row id column in the virtual columns list of the table");
		}
		// check if this column has already been projected
		idx_t column_idx;
		for (column_idx = 0; column_idx < column_ids.size(); ++column_idx) {
			if (column_ids[column_idx].GetPrimaryIndex() == row_id_column) {
				// it has! avoid projecting it again
				break;
			}
		}
		auto row_id_expr = make_uniq<BoundColumnRefExpression>(
		    row_id_entry->second.type, ColumnBinding(get.table_index, ProjectionIndex(column_idx)));
		row_id_expr->SetAlias(row_id_entry->second.name);
		expressions.push_back(std::move(row_id_expr));
		if (column_idx == column_ids.size()) {
			get.AddColumnId(row_id_column);
		}
	}
}

void Binder::BindOldRowCapture(TableCatalogEntry &table, LogicalGet &get, LogicalProjection &proj,
                               LogicalUpdate &update) {
	// Append a reference to each physical column's scanned pre-update value, in table order, recording the
	// input-chunk index of each so the operator can locate the OLD image without assuming a contiguous layout.
	// rowid is appended afterwards, so it stays the last input column. A column that is SET to a constant is not
	// otherwise scanned, so we add it to the scan here.
	auto &column_ids = get.GetColumnIds();
	for (auto &column : table.GetColumns().Physical()) {
		auto logical_index = column.Logical().index;
		optional_idx get_pos;
		for (idx_t i = 0; i < column_ids.size(); i++) {
			if (column_ids[i].GetPrimaryIndex() == logical_index) {
				get_pos = i;
				break;
			}
		}
		if (!get_pos.IsValid()) {
			get_pos = column_ids.size();
			get.AddColumnId(logical_index);
		}
		update.old_row_columns.push_back(proj.expressions.size());
		proj.expressions.push_back(make_uniq<BoundColumnRefExpression>(
		    column.Type(), ColumnBinding(get.table_index, ProjectionIndex(get_pos.GetIndex()))));
	}
}

BoundStatement Binder::Bind(UpdateStatement &stmt) {
	return Bind(*stmt.node);
}

BoundStatement Binder::BindNode(UpdateQueryNode &node) {
	unique_ptr<LogicalOperator> root;

	// visit the table reference
	auto bound_table = Bind(*node.table);
	if (bound_table.plan->type != LogicalOperatorType::LOGICAL_GET) {
		throw BinderException("Can only update base table");
	}
	auto &bound_table_get = bound_table.plan->Cast<LogicalGet>();
	auto table_ptr = bound_table_get.GetTable();
	if (!table_ptr) {
		throw BinderException("Can only update base table");
	}
	auto &table = *table_ptr;

	if (auto expanded = TryExpandTriggers(node, table, TriggerEventType::UPDATE_EVENT)) {
		return std::move(*expanded);
	}

	optional_ptr<LogicalGet> get;
	if (node.from_table) {
		auto from_binder = Binder::CreateBinder(context, this);
		BoundJoinRef bound_crossproduct(JoinRefType::CROSS);
		bound_crossproduct.left = std::move(bound_table);
		bound_crossproduct.right = from_binder->Bind(*node.from_table);
		root = CreatePlan(bound_crossproduct);
		get = &root->children[0]->Cast<LogicalGet>();
		bind_context.AddContext(std::move(from_binder->bind_context));
	} else {
		root = std::move(bound_table.plan);
		get = &root->Cast<LogicalGet>();
	}

	GetStatementProperties().RegisterDBModify(table.catalog, context, DatabaseModificationType::UPDATE_DATA);
	auto update = make_uniq<LogicalUpdate>(table);

	// Trigger expansion flags its generated base UPDATE for OLD capture via scoped binder state (keyed by node
	// identity), so the parsed AST carries no trigger-internal state.
	bool capture_old_rows = global_binder_state->trigger_old_capture.count(node) > 0;

	// set return_chunk boolean early because it needs uses update_is_del_and_insert logic
	if (!node.returning_list.empty() || capture_old_rows) {
		update->return_chunk = true;
	}
	update->capture_old_rows = capture_old_rows;
	// UPDATE ... FROM can match a target row via multiple source rows, so deduplicate keeping the first match;
	// a plain UPDATE cannot produce duplicate row-ids, so it keeps the lock-free path.
	update->row_id_handling = node.from_table ? RowIdHandling::KEEP_FIRST : RowIdHandling::ASSUME_UNIQUE;
	// bind the default values
	auto &catalog_name = table.ParentCatalog().GetName();
	BindDefaultValues(table.GetColumns(), update->bound_defaults, catalog_name.GetIdentifierName(),
	                  table.ParentSchema().GetSchemaPath());
	update->bound_constraints = BindConstraints(table);

	// project any additional columns required for the condition/expressions
	if (node.set_info->condition) {
		WhereBinder binder(*this, context);
		auto condition = binder.Bind(node.set_info->condition);

		PlanSubqueries(condition, root);
		auto filter = make_uniq<LogicalFilter>(std::move(condition));
		filter->AddChild(std::move(root));
		root = std::move(filter);
	}

	D_ASSERT(node.set_info);
	D_ASSERT(node.set_info->columns.size() == node.set_info->expressions.size());

	auto proj_tmp = BindUpdateSet(*update, std::move(root), *node.set_info, table, update->bound_defaults,
	                              update->columns, node.prioritize_table_when_binding);
	D_ASSERT(proj_tmp->type == LogicalOperatorType::LOGICAL_PROJECTION);
	auto proj = unique_ptr_cast<LogicalOperator, LogicalProjection>(std::move(proj_tmp));

	// bind any extra columns necessary for CHECK constraints or indexes
	table.BindUpdateConstraints(*this, *get, *proj, *update, context);

	// capture the pre-update (OLD) row image before rowid so rowid stays the final input column
	if (update->capture_old_rows) {
		BindOldRowCapture(table, *get, *proj, *update);
	}

	// finally bind the row id column and add them to the projection list
	BindRowIdColumns(table, *get, proj->expressions);

	// set the projection as child of the update node and finalize the result
	update->AddChild(std::move(proj));

	auto update_table_index = GenerateTableIndex();
	update->table_index = update_table_index;
	if (!node.returning_list.empty()) {
		bool capture_old = update->capture_old_rows;
		unique_ptr<LogicalOperator> update_as_logicaloperator = std::move(update);

		auto returning_result = BindReturning(std::move(node.returning_list), table, node.table->alias,
		                                      update_table_index, std::move(update_as_logicaloperator));
		if (capture_old) {
			// Expose the captured OLD physical columns as extra output columns of the base CTE, under the reserved
			// names computed once by trigger expansion (threaded via binder state), so the trigger's OLD transition
			// alias can rename them. Public RETURNING (bound above) references only the NEW image, so its semantics
			// are unchanged.
			auto &old_capture_names = global_binder_state->trigger_old_capture_cte_names.at(node);
			auto &proj = returning_result.plan->Cast<LogicalProjection>();
			auto new_column_count = table.GetTypes().size();
			idx_t physical_index = 0;
			for (auto &column : table.GetColumns().Physical()) {
				auto &name = old_capture_names[physical_index];
				proj.expressions.push_back(make_uniq<BoundColumnRefExpression>(
				    name, column.Type(),
				    ColumnBinding(update_table_index, ProjectionIndex(new_column_count + physical_index))));
				returning_result.names.push_back(name);
				returning_result.types.push_back(column.Type());
				physical_index++;
			}
		}
		return returning_result;
	}

	BoundStatement result;
	result.names = {"Count"};
	result.types = {LogicalType::BIGINT};
	result.plan = std::move(update);

	auto &properties = GetStatementProperties();
	properties.return_type = StatementReturnType::CHANGED_ROWS;
	return result;
}

} // namespace duckdb
