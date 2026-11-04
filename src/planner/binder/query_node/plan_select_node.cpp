#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/list.hpp"
#include "duckdb/planner/operator/logical_dummy_scan.hpp"
#include "duckdb/planner/operator/logical_limit.hpp"
#include "duckdb/planner/query_node/bound_select_node.hpp"
#include "duckdb/function/scalar/generic_common.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/function/scalar/system_functions.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"

namespace duckdb {

unique_ptr<LogicalOperator> Binder::PlanFilter(unique_ptr<Expression> condition, unique_ptr<LogicalOperator> root) {
	PlanSubqueries(condition, root);
	auto filter = make_uniq<LogicalFilter>(std::move(condition));
	filter->AddChild(std::move(root));
	return std::move(filter);
}

bool Binder::DebugAggregateStateExportVerify(BoundSelectNode &statement, unique_ptr<LogicalOperator> &root) {
	if (!Settings::Get<DebugVerifyAggregateStateExportSetting>(context)) {
		return false;
	}
	if (IsBindingTrySubquery()) {
		// TRY is lowered onto these aggregates after planning; the verification rewrite would hide
		// them behind an aggregate-state export, so the error capture could no longer reach them
		return false;
	}
	if (statement.groups.grouping_sets.size() > 1 || !statement.grouping_functions.empty()) {
		// verification is not supported with multiple grouping sets / grouping functions (yet?)
		return false;
	}
	for (auto &aggr : statement.aggregates) {
		if (aggr->GetAlias() == "__collated_group") {
			// collated GROUP BY adds first() aggregates to preserve original values - skip verification since
			// ExtractPivotAggregates relies on the __collated_group alias to filter them out
			return false;
		}
		auto &res_type = aggr->GetReturnType();
		if (res_type.IsAggregateState()) {
			// already an aggregate state export - skip verification
			return false;
		}
	}

	// we transform a query like "SELECT grp, SUM(col) FROM tbl GROUP BY grp" into
	// "SELECT grp, first(finalize(sum_state)) FROM (SELECT grp, SUM(col) export_state FROM tbl GROUP BY grp) GROUP BY
	// grp the second grouping is technically not necessary - we add this just so that subsequent binding remains
	// correct first push an aggregate that actually does the state export
	vector<unique_ptr<Expression>> finalize_expressions;
	vector<unique_ptr<Expression>> final_group_expressions;
	vector<unique_ptr<Expression>> final_aggregates;
	auto intermediate_group_index = GenerateTableIndex();
	auto intermediate_aggregate_index = GenerateTableIndex();
	auto intermediate_proj_index = GenerateTableIndex();
	// push references to any groups first
	for (idx_t grp_idx = 0; grp_idx < statement.groups.group_expressions.size(); grp_idx++) {
		auto &group = statement.groups.group_expressions[grp_idx];
		auto group_ref = make_uniq<BoundColumnRefExpression>(
		    group->GetReturnType(), ColumnBinding(intermediate_group_index, ProjectionIndex(grp_idx)));

		auto final_group_ref = make_uniq<BoundColumnRefExpression>(
		    group->GetReturnType(),
		    ColumnBinding(intermediate_proj_index, ProjectionIndex(finalize_expressions.size())));

		finalize_expressions.push_back(std::move(group_ref));
		final_group_expressions.push_back(std::move(final_group_ref));
	}
	auto &system_catalog = Catalog::GetSystemCatalog(context);
	ErrorData error;
	FunctionBinder function_binder(context);
	for (idx_t aggr_idx = 0; aggr_idx < statement.aggregates.size(); aggr_idx++) {
		auto &aggr = statement.aggregates[aggr_idx];
		auto aggr_expr = unique_ptr_cast<Expression, BoundAggregateExpression>(std::move(aggr));
		aggr = ExportAggregateFunction::Bind(std::move(aggr_expr));

		// create a reference to the exported aggregate state
		auto aggr_state_ref = make_uniq<BoundColumnRefExpression>(
		    aggr->GetReturnType(), ColumnBinding(intermediate_aggregate_index, ProjectionIndex(aggr_idx)));

		// create the "finalize" expression
		vector<unique_ptr<Expression>> finalize_children;
		finalize_children.push_back(std::move(aggr_state_ref));
		auto &finalize_function = system_catalog.GetEntry<ScalarFunctionCatalogEntry>(
		    context, QualifiedName(system_catalog.GetName(), DEFAULT_SCHEMA, "finalize"));
		auto result =
		    function_binder.BindScalarFunction(finalize_function, std::move(finalize_children), error, false, *this);
		if (!result) {
			throw InternalException("Failed to bind finalize during debug verification - %s", error.Message());
		}

		// create the "first" expression
		auto &first_function = system_catalog.GetEntry<AggregateFunctionCatalogEntry>(
		    context, QualifiedName(system_catalog.GetName(), DEFAULT_SCHEMA, "first"));
		auto finalize_ref = make_uniq<BoundColumnRefExpression>(
		    result->GetReturnType(),
		    ColumnBinding(intermediate_proj_index, ProjectionIndex(finalize_expressions.size())));

		vector<pair<Identifier, unique_ptr<Expression>>> first_children;
		first_children.emplace_back(Identifier(), std::move(finalize_ref));
		auto first_fun = function_binder.BindAggregateFunction(first_function, std::move(first_children), error);

		finalize_expressions.push_back(std::move(result));
		final_aggregates.push_back(std::move(first_fun));
	}

	// create the initial aggregate that performs the aggregate state export
	auto aggregate = make_uniq<LogicalAggregate>(intermediate_group_index, intermediate_aggregate_index,
	                                             std::move(statement.aggregates));
	aggregate->groups = std::move(statement.groups.group_expressions);
	aggregate->grouping_sets = statement.groups.grouping_sets;
	aggregate->AddChild(std::move(root));

	// now push the projection that finalizes the states
	auto proj = make_uniq<LogicalProjection>(intermediate_proj_index, std::move(finalize_expressions));
	proj->AddChild(std::move(aggregate));

	// in order for bindings after this point to be correct, we need to push another aggregate
	// this just does grp1, grp2, FIRST(finalize_result), etc
	auto final_aggr =
	    make_uniq<LogicalAggregate>(statement.group_index, statement.aggregate_index, std::move(final_aggregates));
	final_aggr->groups = std::move(final_group_expressions);
	final_aggr->grouping_sets = std::move(statement.groups.grouping_sets);
	final_aggr->AddChild(std::move(proj));
	root = std::move(final_aggr);
	return true;
}

unique_ptr<LogicalOperator> Binder::CreatePlan(BoundSelectNode &statement) {
	D_ASSERT(statement.from_table.plan);
	if (IsBindingTrySubquery()) {
		// record the indexes of this node, so TRY can be lowered onto exactly this node's projection
		auto &try_indexes = GetTrySubqueryPlanIndexesMutable();
		try_indexes.recorded = true;
		try_indexes.projection_index = statement.projection_index;
		if (statement.need_prune) {
			try_indexes.prune_index = statement.prune_index;
		}
		try_indexes.aggregate_index = statement.aggregate_index;
		try_indexes.window_index = statement.window_index;
		for (auto &entry : statement.unnests.SelectList()) {
			try_indexes.unnest_indexes.push_back(entry.second.index);
		}
		for (auto &entry : statement.unnests.GroupBy()) {
			try_indexes.unnest_indexes.push_back(entry.second.index);
		}
	}
	auto root = std::move(statement.from_table.plan);

	auto plan_unnests = [&](BoundUnnestMap &unnests) {
		for (idx_t i = unnests.size(); i > 0; i--) {
			auto unnest_depth = i - 1;
			auto entry = unnests.find(unnest_depth);
			if (entry == unnests.end()) {
				throw InternalException("unnests specified at level %d but none were found", unnest_depth);
			}
			auto &unnest_node = entry->second;
			auto unnest = make_uniq<LogicalUnnest>(unnest_node.index);
			unnest->expressions = std::move(unnest_node.expressions);
			// visit the unnest expressions
			for (auto &expr : unnest->expressions) {
				PlanSubqueries(expr, root);
			}
			D_ASSERT(!unnest->expressions.empty());
			unnest->AddChild(std::move(root));
			root = std::move(unnest);
		}
	};

	// GROUP BY expressions containing UNNEST must expand rows before aggregation.
	plan_unnests(statement.unnests.GroupBy());

	if (!statement.aggregates.empty() || !statement.groups.group_expressions.empty() || statement.having) {
		if (!statement.groups.group_expressions.empty()) {
			// visit the groups
			for (auto &group : statement.groups.group_expressions) {
				PlanSubqueries(group, root);
			}
		}
		// now visit all aggregate expressions
		for (auto &expr : statement.aggregates) {
			PlanSubqueries(expr, root);
		}

		if (!DebugAggregateStateExportVerify(statement, root)) {
			// finally create the aggregate node with the group_index and aggregate_index as obtained from the binder
			auto aggregate = make_uniq<LogicalAggregate>(statement.group_index, statement.aggregate_index,
			                                             std::move(statement.aggregates));
			aggregate->groups = std::move(statement.groups.group_expressions);
			aggregate->groupings_index = statement.groupings_index;
			aggregate->grouping_sets = std::move(statement.groups.grouping_sets);
			aggregate->grouping_functions = std::move(statement.grouping_functions);
			aggregate->AddChild(std::move(root));
			root = std::move(aggregate);
		}
	} else if (!statement.groups.grouping_sets.empty()) {
		// edge case: we have grouping sets but no groups or aggregates
		// this can only happen if we have e.g. select 1 from tbl group by ();
		// just output a dummy scan
		root = make_uniq_base<LogicalOperator, LogicalDummyScan>(statement.group_index);
	}

	if (statement.having) {
		PlanSubqueries(statement.having, root);
		auto having = make_uniq<LogicalFilter>(std::move(statement.having));

		having->AddChild(std::move(root));
		root = std::move(having);
	}

	if (!statement.windows.empty()) {
		auto win = make_uniq<LogicalWindow>(statement.window_index);
		win->expressions = std::move(statement.windows);
		// visit the window expressions
		for (auto &expr : win->expressions) {
			PlanSubqueries(expr, root);
		}
		D_ASSERT(!win->expressions.empty());
		win->AddChild(std::move(root));
		root = std::move(win);
	}

	if (statement.qualify) {
		PlanSubqueries(statement.qualify, root);
		auto qualify = make_uniq<LogicalFilter>(std::move(statement.qualify));

		qualify->AddChild(std::move(root));
		root = std::move(qualify);
	}

	plan_unnests(statement.unnests.SelectList());

	for (auto &expr : statement.select_list) {
		PlanSubqueries(expr, root);
	}

	auto proj = make_uniq<LogicalProjection>(statement.projection_index, std::move(statement.select_list));
	auto &projection = *proj;
	proj->AddChild(std::move(root));
	root = std::move(proj);

	// finish the plan by handling the elements of the QueryNode
	root = VisitQueryNode(statement, std::move(root));

	// add a prune node if necessary
	if (statement.need_prune) {
		D_ASSERT(root);
		vector<unique_ptr<Expression>> prune_expressions;
		for (idx_t i = 0; i < statement.column_count; i++) {
			prune_expressions.push_back(
			    make_uniq<BoundColumnRefExpression>(projection.expressions[i]->GetReturnType(),
			                                        ColumnBinding(statement.projection_index, ProjectionIndex(i))));
		}
		auto prune = make_uniq<LogicalProjection>(statement.prune_index, std::move(prune_expressions));
		prune->AddChild(std::move(root));
		root = std::move(prune);
	}
	return root;
}

} // namespace duckdb
