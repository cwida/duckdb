#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/statement/vacuum_statement.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_vacuum.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"

namespace duckdb {

void Binder::BindVacuumTable(LogicalVacuum &vacuum, unique_ptr<LogicalOperator> &root) {
	auto &info = vacuum.GetInfo();
	if (!info.has_table) {
		return;
	}

	D_ASSERT(vacuum.column_id_map.empty());

	auto bound_table = Bind(*info.ref);
	if (bound_table.plan->type != LogicalOperatorType::LOGICAL_GET) {
		throw BinderException("Can only vacuum or analyze base tables");
	}
	auto table_scan = std::move(bound_table.plan);
	auto &get = table_scan->Cast<LogicalGet>();
	auto table_ptr = get.GetTable();
	if (!table_ptr) {
		throw BinderException("Can only vacuum or analyze base tables");
	}
	auto &table = *table_ptr;
	vacuum.SetTable(table);

	vector<unique_ptr<Expression>> select_list;
	auto &columns = info.columns;
	if (columns.empty()) {
		// Empty means ALL columns should be vacuumed/analyzed
		for (auto &col : table.GetColumns().Physical()) {
			columns.emplace_back(col.GetName());
		}
	}

	identifier_set_t column_name_set;
	vector<Identifier> non_generated_column_names;
	for (auto &col_name : columns) {
		if (column_name_set.count(col_name) > 0) {
			throw BinderException("cannot vacuum or analyze the same column twice, i.e., there is a duplicate entry in "
			                      "the list of column names");
		}
		column_name_set.insert(col_name);
		if (!table.ColumnExists(col_name)) {
			throw BinderException("Column with name %s does not exist", col_name);
		}
		auto &col = table.GetColumn(col_name);
		// ignore generated column
		if (col.Generated()) {
			throw BinderException(
			    "cannot vacuum or analyze generated column %s - specify non-generated columns to vacuum or analyze",
			    col.GetName());
		}
		non_generated_column_names.emplace_back(col_name);
		ColumnRefExpression colref(col_name, table.name);
		auto result = bind_context.BindColumn(colref, 0);
		if (result.HasError()) {
			result.error.Throw();
		}
		select_list.push_back(std::move(result.expression));
	}
	info.columns = non_generated_column_names;

	auto &column_ids = get.GetColumnIds();
	D_ASSERT(select_list.size() == column_ids.size());
	D_ASSERT(info.columns.size() == column_ids.size());
	for (idx_t i = 0; i < column_ids.size(); i++) {
		vacuum.column_id_map[i] = table.GetColumns().LogicalToPhysical(column_ids[i].ToLogical()).index;
	}

	auto projection = make_uniq<LogicalProjection>(GenerateTableIndex(), std::move(select_list));
	projection->children.push_back(std::move(table_scan));

	root = std::move(projection);
}

BoundStatement Binder::Bind(VacuumStatement &stmt) {
	BoundStatement result;

	unique_ptr<LogicalOperator> root;

	auto vacuum = make_uniq<LogicalVacuum>(std::move(stmt.info));
	BindVacuumTable(*vacuum, root);
	if (root) {
		vacuum->children.push_back(std::move(root));
	}

	result.names = {"Success"};
	result.types = {LogicalType::BOOLEAN};
	result.plan = std::move(vacuum);

	auto &properties = GetStatementProperties();
	properties.return_type = StatementReturnType::NOTHING;
	return result;
}

} // namespace duckdb
