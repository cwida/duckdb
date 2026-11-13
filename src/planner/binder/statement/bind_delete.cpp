#include "duckdb/parser/statement/delete_statement.hpp"
#include "duckdb/parser/query_node/delete_query_node.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression_binder/where_binder.hpp"
#include "duckdb/planner/expression_binder/returning_binder.hpp"
#include "duckdb/planner/operator/logical_delete.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_cross_product.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/storage/data_table.hpp"

namespace duckdb {

BoundStatement Binder::Bind(DeleteStatement &stmt) {
	return Bind(*stmt.node);
}

BoundStatement Binder::BindNode(DeleteQueryNode &node) {
	// visit the table reference
	auto bound_table = Bind(*node.table);
	auto root = std::move(bound_table.plan);
	if (root->type != LogicalOperatorType::LOGICAL_GET) {
		throw BinderException("Can only delete from base table");
	}
	auto &get = root->Cast<LogicalGet>();
	auto table_ptr = get.GetTable();
	if (!table_ptr) {
		throw BinderException("Can only delete from base table");
	}
	auto &table = *table_ptr;

	if (auto expanded = TryExpandTriggers(node, table, TriggerEventType::DELETE_EVENT)) {
		return std::move(*expanded);
	}
	if (auto expanded = TryExpandRowTriggers(node, node.returning_list, table, TriggerEventType::DELETE_EVENT)) {
		return std::move(*expanded);
	}

	GetStatementProperties().RegisterDBModify(table.catalog, context, DatabaseModificationType::DELETE_DATA);

	// plan any tables from the various using clauses
	if (!node.using_clauses.empty()) {
		unique_ptr<LogicalOperator> child_operator;
		for (auto &using_clause : node.using_clauses) {
			// bind the using clause
			auto using_binder = Binder::CreateBinder(context, this);
			auto op = using_binder->Bind(*using_clause);
			if (child_operator) {
				// already bound a child: create a cross product to unify the two
				child_operator = LogicalCrossProduct::Create(std::move(child_operator), std::move(op.plan));
			} else {
				child_operator = std::move(op.plan);
			}
			bind_context.AddContext(std::move(using_binder->bind_context));
		}
		if (child_operator) {
			root = LogicalCrossProduct::Create(std::move(root), std::move(child_operator));
		}
	}

	// project any additional columns required for the condition
	unique_ptr<Expression> condition;
	if (node.condition) {
		WhereBinder binder(*this, context);
		condition = binder.Bind(node.condition);

		PlanSubqueries(condition, root);
		auto filter = make_uniq<LogicalFilter>(std::move(condition));
		filter->AddChild(std::move(root));
		root = std::move(filter);
	}
	// create the delete node
	auto del = make_uniq<LogicalDelete>(table, GenerateTableIndex());
	del->bound_constraints = BindConstraints(table);

	auto is_duck_table = table.IsDuckTable();
	if (is_duck_table) {
		// Bind the row id before the table columns so it remains the first delete expression.
		BindRowIdColumns(table, get, del->expressions);
	}

	// Add columns to the scan to avoid fetching by row ID in PhysicalDelete:
	// - If RETURNING: add all physical columns (for RETURNING projection)
	// - Else if unique indexes exist: add only indexed columns (for delete index tracking)
	if (!node.returning_list.empty()) {
		// Add all physical columns for RETURNING
		if (is_duck_table) {
			BindDeleteReturningColumns(table, get, del->return_columns, del->expressions, get);
		} else {
			BindDeleteReturningColumns(table, get, del->return_columns);
		}
	} else if (is_duck_table) {
		// Only optimize for DuckDB tables (not attached external tables like SQLite)
		auto &storage = table.GetStorage();
		if (storage.HasUniqueIndexes()) {
			BindDeleteIndexColumns(table, get, del->return_columns, del->expressions, get);
		}
	}

	del->AddChild(std::move(root));

	if (!is_duck_table) {
		// Bind external table row IDs after the returning columns to preserve their scan layout.
		BindRowIdColumns(table, get, del->expressions);
	}

	if (!node.returning_list.empty()) {
		del->return_chunk = true;

		auto update_table_index = GenerateTableIndex();
		del->table_index = update_table_index;

		unique_ptr<LogicalOperator> del_as_logicaloperator = std::move(del);
		// Include virtual columns (like rowid) so they can be referenced in RETURNING
		auto virtual_columns = table.GetDeleteReturningVirtualColumns();
		return BindReturning(std::move(node.returning_list), table, node.table->alias, update_table_index,
		                     std::move(del_as_logicaloperator), std::move(virtual_columns));
	}
	BoundStatement result;
	result.plan = std::move(del);
	result.names = {"Count"};
	result.types = {LogicalType::BIGINT};

	auto &properties = GetStatementProperties();
	properties.return_type = StatementReturnType::CHANGED_ROWS;

	return result;
}

} // namespace duckdb
