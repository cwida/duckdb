#include "duckdb/optimizer/late_materialization_helper.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"

namespace duckdb {

unique_ptr<LogicalGet> LateMaterializationHelper::CreateLHSGet(const LogicalGet &rhs, Binder &binder) {
	auto table_index = binder.GenerateTableIndex();
	auto new_get = make_uniq<LogicalGet>(table_index, rhs.function, rhs.bind_data->Copy(), rhs.returned_types,
	                                     rhs.names, rhs.virtual_columns);
	new_get->GetMutableColumnIds() = rhs.GetColumnIds();
	new_get->projection_ids = rhs.projection_ids;
	new_get->parameters = rhs.parameters;
	new_get->named_parameters = rhs.named_parameters;
	new_get->input_table_types = rhs.input_table_types;
	new_get->input_table_names = rhs.input_table_names;
	auto &column_ids = rhs.GetColumnIds();
	for (auto &filter_entry : rhs.table_filters) {
		auto &expr_filter = filter_entry.Filter().Cast<ExpressionFilter>();
		if (ExpressionFilter::ContainsInternalFunction(*expr_filter.expr, DynamicFilterScalarFun::NAME)) {
			continue;
		}
		auto &col_type = rhs.GetColumnType(column_ids[filter_entry.GetIndex()]);
		auto optional_expr = CreateOptionalFilterExpression(expr_filter.expr->Copy(), col_type);
		new_get->table_filters.PushFilter(filter_entry.GetIndex(),
		                                  make_uniq<ExpressionFilter>(std::move(optional_expr)));
	}
	return new_get;
}

vector<ProjectionIndex> LateMaterializationHelper::GetOrInsertRowIds(LogicalGet &get,
                                                                     const vector<column_t> &row_id_column_ids,
                                                                     const vector<TableColumn> &row_id_columns) {
	auto &column_ids = get.GetMutableColumnIds();

	vector<ProjectionIndex> result;
	for (idx_t r_idx = 0; r_idx < row_id_column_ids.size(); ++r_idx) {
		// check if it is already projected
		auto row_id_column_id = row_id_column_ids[r_idx];
		auto &row_id_column = row_id_columns[r_idx];
		ProjectionIndex row_id_index;
		for (idx_t i = 0; i < column_ids.size(); ++i) {
			if (column_ids[i].GetPrimaryIndex() == row_id_column_id) {
				// already projected - return the id
				row_id_index = ProjectionIndex(i);
				break;
			}
		}
		if (row_id_index.IsValid()) {
			result.push_back(row_id_index);
			continue;
		}
		// row id is not yet projected - push it and return the new index
		row_id_index = ProjectionIndex(column_ids.size());
		column_ids.push_back(ColumnIndex(row_id_column_id));
		if (!get.projection_ids.empty()) {
			get.projection_ids.push_back(row_id_index);
		}
		if (!get.types.empty()) {
			get.types.push_back(row_id_column.type);
		}
		result.push_back(row_id_index);
	}
	return result;
}
} // namespace duckdb
