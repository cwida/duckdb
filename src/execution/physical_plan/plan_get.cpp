#include "duckdb/execution/operator/scan/physical_expression_scan.hpp"
#include "duckdb/execution/operator/scan/physical_dummy_scan.hpp"
#include "duckdb/execution/operator/projection/physical_projection.hpp"
#include "duckdb/execution/operator/projection/physical_tableinout_function.hpp"
#include "duckdb/execution/operator/scan/physical_table_scan.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/function/table/table_scan.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/execution/operator/filter/physical_filter.hpp"

namespace duckdb {

unique_ptr<TableFilterSet> MoveTableFilters(TableFilterSet &table_filters) {
	// create the table filter map
	auto table_filter_set = make_uniq<TableFilterSet>();
	for (auto &entry : table_filters) {
		table_filter_set->SetFilterByColumnIndex(entry.GetIndex(), entry.TakeFilter());
	}
	for (const auto &filter : table_filters.GetMultiColumnFilters()) {
		const auto &expression_filter = ExpressionFilter::GetExpressionFilter(*filter, "MoveTableFilters");
		table_filter_set->PushMultiColumnFilter(expression_filter.Copy());
	}
	return table_filter_set;
}

PhysicalOperator &PhysicalPlanGenerator::CreatePlan(LogicalGet &op) {
	auto column_ids = op.GetColumnIds();
	if (!op.children.empty()) {
		reference<PhysicalOperator> child = ResolveAndPlan(std::move(op.children[0]));
		auto &child_types = child.get().types;

		// this is for table producing functions that consume subquery results
		// push a projection node with casts if required
		if (child_types.size() < op.input_table_types.size()) {
			throw InternalException(
			    "Mismatch between input table types and child node types - expected %llu but got %llu",
			    op.input_table_types.size(), child_types.size());
		}

		vector<LogicalType> return_types;
		vector<unique_ptr<Expression>> expressions;
		bool any_cast_required = false;
		for (idx_t proj_idx = 0; proj_idx < child_types.size(); proj_idx++) {
			auto ref = make_uniq<BoundReferenceExpression>(child_types[proj_idx], proj_idx);
			auto &target_type =
			    proj_idx < op.input_table_types.size() ? op.input_table_types[proj_idx] : child_types[proj_idx];
			if (child_types[proj_idx] != target_type) {
				// cast is required - push a cast
				any_cast_required = true;
				auto cast = BoundCastExpression::AddCastToType(context, std::move(ref), target_type);
				expressions.push_back(std::move(cast));
			} else {
				expressions.push_back(std::move(ref));
			}
			return_types.push_back(target_type);
		}

		if (any_cast_required) {
			auto &proj = Make<PhysicalProjection>(std::move(return_types), std::move(expressions),
			                                      child.get().estimated_cardinality);
			proj.children.push_back(child);
			child = proj;
		}

		unique_ptr<TableFilterSet> table_filters;
		if (op.table_filters.HasFilters() || op.table_filters.HasMultiColumnFilters()) {
			table_filters = MoveTableFilters(op.table_filters);
		}
		vector<idx_t> projection_indices;
		for (auto &projection_id : op.projection_ids) {
			projection_indices.push_back(projection_id);
		}
		optional_idx ordinality_idx;
		if (op.ordinality_idx.IsValid()) {
			auto output_count = projection_indices.empty() ? column_ids.size() : projection_indices.size();
			for (idx_t output_idx = 0; output_idx < output_count; output_idx++) {
				auto column_idx = projection_indices.empty() ? output_idx : projection_indices[output_idx];
				if (column_ids[column_idx].GetPrimaryIndex() == op.ordinality_idx.GetIndex()) {
					ordinality_idx = output_idx;
					break;
				}
			}
		}
		auto &table_in_out = Make<PhysicalTableInOutFunction>(
		    op.types, op.function, std::move(op.bind_data), column_ids, std::move(projection_indices),
		    std::move(table_filters), op.estimated_cardinality, std::move(op.projected_input));
		table_in_out.children.push_back(child);
		auto &cast_table_in_out = table_in_out.Cast<PhysicalTableInOutFunction>();
		cast_table_in_out.ordinality_idx = ordinality_idx;
		return table_in_out;
	}

	if (!op.projected_input.empty()) {
		throw InternalException("LogicalGet::project_input can only be set for table-in-out functions");
	}

	unique_ptr<TableFilterSet> table_filters;
	if (op.table_filters.HasFilters() || op.table_filters.HasMultiColumnFilters()) {
		table_filters = MoveTableFilters(op.table_filters);
	}

	if (op.function.dependency) {
		op.function.dependency(dependencies, op.bind_data.get());
	}

	optional_ptr<PhysicalOperator> filter;
	auto &projection_ids = op.projection_ids;
	// A filter re-created above the scan must reference the scan's output layout: every column in order without
	// projection pushdown, column_ids when projection_ids is empty, projection_ids otherwise
	// (see LogicalGet::ResolveTypes).
	const bool scan_emits_all_columns = !op.function.projection_pushdown;
	const bool scan_emits_column_ids = !scan_emits_all_columns && projection_ids.empty();
	// the columns of the get, before those that only a filter reads are added to the scan
	const auto returned_column_count = projection_ids.size();

	if (table_filters && op.function.supports_pushdown_type) {
		vector<unique_ptr<Expression>> select_list;
		unique_ptr<Expression> unsupported_filter;
		unordered_set<ProjectionIndex> to_remove;

		for (auto &entry : *table_filters) {
			auto filter_idx = entry.GetIndex();
			auto &filter_expr = entry.Filter();
			auto &column_idx = op.GetColumnIndex(filter_idx);
			auto column_id = column_idx.GetPrimaryIndex();
			if (!op.function.supports_pushdown_type(*op.bind_data, column_id)) {
				auto &column_type = op.GetColumnType(column_idx);
				idx_t filter_column;
				if (scan_emits_all_columns) {
					filter_column = column_id;
				} else if (scan_emits_column_ids) {
					filter_column = filter_idx.GetIndex();
				} else {
					optional_idx column_id_filter;
					for (idx_t i = 0; i < projection_ids.size(); i++) {
						if (column_ids[projection_ids[i]] == column_idx) {
							column_id_filter = i;
							break;
						}
					}
					if (!column_id_filter.IsValid()) {
						projection_ids.push_back(filter_idx);
						column_id_filter = projection_ids.size() - 1;
					}
					filter_column = column_id_filter.GetIndex();
				}
				auto column = make_uniq<BoundReferenceExpression>(column_type, filter_column);
				select_list.push_back(filter_expr.ToExpression(*column));
				to_remove.insert(filter_idx);
			}
		}
		for (auto &col : to_remove) {
			table_filters->RemoveFilterByColumnIndex(col);
		}

		if (!select_list.empty()) {
			vector<LogicalType> filter_types;
			if (scan_emits_all_columns) {
				filter_types = op.returned_types;
			} else if (scan_emits_column_ids) {
				for (auto &column_index : column_ids) {
					filter_types.push_back(op.GetColumnType(column_index));
				}
			} else {
				for (auto &c : projection_ids) {
					filter_types.push_back(op.GetColumnType(column_ids[c]));
				}
			}
			filter = Make<PhysicalFilter>(filter_types, std::move(select_list), op.estimated_cardinality);
		}
	}
	op.ResolveOperatorTypes();
	// create the table scan node
	if (!op.function.projection_pushdown) {
		// function does not support projection pushdown
		auto &table_scan = Make<PhysicalTableScan>(
		    op.returned_types, op.function, std::move(op.bind_data), op.returned_types, column_ids, vector<column_t>(),
		    IdentifiersToStrings(op.names), std::move(table_filters), op.estimated_cardinality,
		    std::move(op.extra_info), std::move(op.parameters), std::move(op.virtual_columns));
		// first check if an additional projection is necessary
		if (column_ids.size() == op.returned_types.size()) {
			bool projection_necessary = false;
			for (idx_t i = 0; i < column_ids.size(); i++) {
				if (column_ids[i].GetPrimaryIndex() != i) {
					projection_necessary = true;
					break;
				}
			}
			if (!projection_necessary) {
				// a projection is not necessary if all columns have been requested in-order
				// in that case we just return the node
				if (filter) {
					filter->children.push_back(table_scan);
					return *filter;
				}
				return table_scan;
			}
		}
		// push a projection on top that does the projection
		vector<LogicalType> types;
		vector<unique_ptr<Expression>> expressions;
		for (auto &column_id : column_ids) {
			if (column_id.IsVirtualColumn()) {
				throw NotImplementedException("Virtual columns require projection pushdown");
			} else {
				auto col_id = column_id.GetPrimaryIndex();
				auto type = op.returned_types[col_id];
				types.push_back(type);
				expressions.push_back(make_uniq<BoundReferenceExpression>(type, col_id));
			}
		}
		auto &proj = Make<PhysicalProjection>(std::move(types), std::move(expressions), op.estimated_cardinality);
		if (filter) {
			filter->children.push_back(table_scan);
			proj.children.push_back(*filter);
			return proj;
		}
		proj.children.push_back(table_scan);
		return proj;
	}
	vector<idx_t> projection_indices;
	for (auto &proj_id : op.projection_ids) {
		projection_indices.push_back(proj_id);
	}

	auto &table_scan = Make<PhysicalTableScan>(
	    op.types, op.function, std::move(op.bind_data), op.returned_types, column_ids, std::move(projection_indices),
	    IdentifiersToStrings(op.names), std::move(table_filters), op.estimated_cardinality, std::move(op.extra_info),
	    std::move(op.parameters), std::move(op.virtual_columns));
	auto &cast_table_scan = table_scan.Cast<PhysicalTableScan>();
	cast_table_scan.dynamic_filters = op.dynamic_filters;
	if (!filter) {
		return table_scan;
	}
	filter->children.push_back(table_scan);
	if (projection_ids.size() == returned_column_count) {
		return *filter;
	}
	// drop the columns that only the filter reads
	vector<LogicalType> types;
	vector<unique_ptr<Expression>> expressions;
	for (idx_t i = 0; i < returned_column_count; i++) {
		types.push_back(filter->types[i]);
		expressions.push_back(make_uniq<BoundReferenceExpression>(filter->types[i], i));
	}
	auto &proj = Make<PhysicalProjection>(std::move(types), std::move(expressions), op.estimated_cardinality);
	proj.children.push_back(*filter);
	return proj;
}

} // namespace duckdb
