#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_create_index.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/execution/operator/scan/physical_dummy_scan.hpp"
#include "duckdb/execution/operator/filter/physical_filter.hpp"
#include "duckdb/execution/operator/order/physical_order.hpp"
#include "duckdb/execution/operator/projection/physical_projection.hpp"
#include "duckdb/execution/operator/schema/physical_create_index.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"

namespace duckdb {

static PhysicalOperator &AddCreateIndex(PhysicalPlanGenerator &plan, LogicalCreateIndex &op, PhysicalOperator &prev,
                                        const IndexType &index_type, unique_ptr<IndexBuildBindData> bind_data) {
	auto &cindex = plan.Make<PhysicalCreateIndex>(op, op.table, op.info->column_ids, std::move(op.info),
	                                              std::move(op.unbound_expressions), op.estimated_cardinality,
	                                              index_type, std::move(bind_data), std::move(op.alter_table_info));

	cindex.children.push_back(prev);
	return cindex;
}

static PhysicalOperator &AddFilter(PhysicalPlanGenerator &plan, LogicalCreateIndex &op, PhysicalOperator &prev) {
	auto cardinality = op.estimated_cardinality;

	auto filter_types = vector<LogicalType>();
	auto filter_exprs = vector<unique_ptr<Expression>>();

	for (idx_t i = 0; i < prev.types.size() - 1; i++) {
		filter_types.push_back(prev.types[i]);
		auto is_not_null_expr =
		    make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NOT_NULL, LogicalType::BOOLEAN);
		auto bound_ref = make_uniq<BoundReferenceExpression>(prev.types[i], i);
		is_not_null_expr->GetChildrenMutable().push_back(std::move(bound_ref));

		filter_exprs.push_back(std::move(is_not_null_expr));
	}
	filter_types.push_back(LogicalType::ROW_TYPE);

	// filter operator
	auto &filter = plan.Make<PhysicalFilter>(std::move(filter_types), std::move(filter_exprs), cardinality);

	filter.children.push_back(prev);

	return filter;
}

static PhysicalOperator &AddProjection(PhysicalPlanGenerator &plan, LogicalCreateIndex &op, PhysicalOperator &prev) {
	auto cardinality = op.estimated_cardinality;

	// Create a project on on the indexed columns + rowid
	auto select_types = vector<LogicalType>();
	auto select_exprs = vector<unique_ptr<Expression>>();

	for (auto &expression : op.expressions) {
		select_types.push_back(expression->GetReturnType());
		select_exprs.push_back(std::move(expression));
	}

	auto rowid_column_index = op.info->scan_types.size() - 1;
	select_types.push_back(LogicalType::ROW_TYPE);
	select_exprs.push_back(make_uniq<BoundReferenceExpression>(LogicalType::ROW_TYPE, rowid_column_index));

	auto &select = plan.Make<PhysicalProjection>(std::move(select_types), std::move(select_exprs), cardinality);

	select.children.push_back(prev);

	return select;
}

static PhysicalOperator &AddSort(PhysicalPlanGenerator &plan, LogicalCreateIndex &op, PhysicalOperator &prev) {
	auto cardinality = op.estimated_cardinality;

	vector<BoundOrderByNode> orders;
	vector<idx_t> projections;
	for (idx_t i = 0; i < prev.types.size() - 1; i++) {
		auto col_expr = make_uniq_base<Expression, BoundReferenceExpression>(prev.types[i], i);
		orders.emplace_back(OrderType::ASCENDING, OrderByNullType::NULLS_FIRST, std::move(col_expr));
		projections.emplace_back(i);
	}
	// add row id column to projections
	projections.emplace_back(prev.types.size() - 1);

	auto &sortby = plan.Make<PhysicalOrder>(prev.types, std::move(orders), std::move(projections), cardinality, true);

	sortby.children.push_back(prev);

	return sortby;
}

PhysicalOperator &PhysicalPlanGenerator::CreatePlan(LogicalCreateIndex &op) {
	// Early-out, if the index already exists.
	// ALTER TABLE ADD PRIMARY KEY / UNIQUE creates a constraint index, which has no catalog entry.
	if (!op.alter_table_info) {
		auto &schema = op.table.schema;
		auto entry =
		    schema.GetEntry(schema.GetCatalogTransaction(context), CatalogType::INDEX_ENTRY, op.info->GetIndexName());
		if (entry) {
			if (op.info->on_conflict != OnCreateConflict::IGNORE_ON_CONFLICT) {
				throw CatalogException("Index with name %s already exists!", op.info->GetIndexName());
			}
			return Make<PhysicalDummyScan>(op.types, op.estimated_cardinality);
		}
	}

	if (!op.table.IsDuckTable()) {
		throw BinderException("Indexes can only be created on DuckDB tables.");
	}

	// Ensure that all expressions contain valid scalar functions.
	// E.g., get_current_timestamp(), random(), and sequence values cannot be index keys.
	for (idx_t i = 0; i < op.unbound_expressions.size(); i++) {
		auto &expr = op.unbound_expressions[i];
		if (!expr->IsConsistent() || expr->RequiresRebind()) {
			throw BinderException("Index keys cannot contain expressions with side effects.");
		}
	}

	// If we get here and the index type is not valid index type, we throw an exception.
	const auto index_type = context.db->config.GetIndexTypes().FindByName(op.info->index_type);
	if (!index_type) {
		throw BinderException("Unknown index type: " + op.info->index_type);
	}

	// Add a dependency for the entire table on which we create the index.
	dependencies.AddDependency(op.table);
	D_ASSERT(op.info->scan_types.size() - 1 <= op.info->names.size());
	D_ASSERT(op.info->scan_types.size() - 1 <= op.info->column_ids.size());

	D_ASSERT(op.children.size() == 1);
	auto &scan = CreatePlan(*op.children[0]);

	// Index has a plan replacement function
	if (index_type->create_plan) {
		PlanIndexInput input(context, op, *this, scan, index_type->index_info);
		return index_type->create_plan(input);
	}

	// Fall back to generic index creation plan
	// SCAN -> PROJECTION -> [FILTER] -> [SORT] -> CREATE INDEX

	// "Bind" the index and determine if we need a sort.
	auto &duck_table = op.table.Cast<DuckTableEntry>();
	IndexBuildBindInput bind_input {context, duck_table, *op.info, op.unbound_expressions};
	auto bind_data = index_type->build_bind(bind_input);

	auto need_sort = false;
	if (index_type->build_sort) {
		IndexBuildSortInput sort_input {bind_data.get()};
		need_sort = index_type->build_sort(sort_input);
	}

	// CREATE INDEX and ALTER ADD UNIQUE skip NULL keys. ALTER ADD PRIMARY KEY must see NULLs to reject them.
	const auto is_add_primary_key = op.alter_table_info && op.info->constraint_type == IndexConstraintType::PRIMARY;
	auto need_filter = !is_add_primary_key;

	// Construct the plan
	auto plan = &scan;
	plan = &AddProjection(*this, op, *plan);

	if (need_filter) {
		plan = &AddFilter(*this, op, *plan);
	}
	if (need_sort) {
		plan = &AddSort(*this, op, *plan);
	}
	plan = &AddCreateIndex(*this, op, *plan, *index_type, std::move(bind_data));

	return *plan;
}

} // namespace duckdb
