#include "duckdb/optimizer/join_filter_pushdown_optimizer.hpp"

#include "duckdb/execution/operator/join/join_filter_pushdown.hpp"
#include "duckdb/execution/operator/join/physical_comparison_join.hpp"
#include "duckdb/function/aggregate/distributive_functions.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/function/builtin_function_lookup.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_any_join.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_distinct.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_set_operation.hpp"
#include "duckdb/planner/operator/logical_unnest.hpp"

namespace duckdb {

JoinFilterPushdownOptimizer::JoinFilterPushdownOptimizer(Optimizer &optimizer) : optimizer(optimizer) {
}

static bool IsJoinFilterPushdownIntegralType(const LogicalType &type) {
	return type.IsIntegral() && GetTypeIdSize(type.InternalType()) <= GetTypeIdSize(PhysicalType::INT64);
}

static bool IsJoinFilterPushdownIntegralCast(const LogicalType &src, const LogicalType &tgt) {
	return IsJoinFilterPushdownIntegralType(src) && IsJoinFilterPushdownIntegralType(tgt);
}

static bool IsJoinFilterPushdownVariantIntegralCast(const LogicalType &src, const LogicalType &tgt) {
	if (src.id() == LogicalTypeId::VARIANT) {
		return IsJoinFilterPushdownIntegralType(tgt);
	}
	if (tgt.id() == LogicalTypeId::VARIANT) {
		return IsJoinFilterPushdownIntegralType(src);
	}
	return false;
}

struct JoinFilterExpressionPath {
	ColumnBinding binding;
	JoinFilterPushdownMode mode = JoinFilterPushdownMode::RECONSTRUCT_EXPRESSION;
	vector<RuntimeFilterCastStep> casts;
};

static bool PushdownJoinFilterExpressionInternal(const Expression &expr, JoinFilterExpressionPath &path) {
	const auto &return_type = expr.GetReturnType();
	if (return_type.IsNested() && return_type.id() != LogicalTypeId::VARIANT) {
		// nested columns are not supported for pushdown
		return false;
	}
	if (return_type.id() == LogicalTypeId::INTERVAL) {
		// interval is not supported for pushdown
		return false;
	}
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_COLUMN_REF: {
		// column-ref - pass through the new column binding
		auto &colref = expr.Cast<BoundColumnRefExpression>();
		path.binding = colref.Binding();
		return true;
	}
	case ExpressionClass::BOUND_FUNCTION: {
		if (!BoundCastExpression::IsCast(expr)) {
			auto &function_expr = expr.Cast<BoundFunctionExpression>();
			if (function_expr.Function().GetName() != "variant_normalize" || function_expr.GetChildren().size() != 1) {
				return false;
			}
			return PushdownJoinFilterExpressionInternal(*function_expr.GetChildren()[0], path);
		}
		// We allow pushing through integral casts and integral/VARIANT casts.
		const auto &bound_cast = expr.Cast<BoundFunctionExpression>();
		const auto &src = BoundCastExpression::Child(bound_cast).GetReturnType();
		const auto &tgt = bound_cast.GetReturnType();
		const bool integral_cast = IsJoinFilterPushdownIntegralCast(src, tgt);
		const bool variant_integral_cast = IsJoinFilterPushdownVariantIntegralCast(src, tgt);
		if (!integral_cast && !variant_integral_cast) {
			return false;
		}
		if (!PushdownJoinFilterExpressionInternal(BoundCastExpression::Child(bound_cast), path)) {
			return false;
		}
		if (variant_integral_cast) {
			if (tgt.id() == LogicalTypeId::VARIANT) {
				path.mode = JoinFilterPushdownMode::STORAGE_ONLY;
				path.casts.clear();
			} else if (path.mode == JoinFilterPushdownMode::RECONSTRUCT_EXPRESSION) {
				path.casts.emplace_back(tgt, BoundCastExpression::IsTryCast(bound_cast)
				                                 ? RuntimeFilterCastMode::TRY_CAST
				                                 : RuntimeFilterCastMode::DEFAULT_CAST);
			}
			return true;
		}
		if (path.mode == JoinFilterPushdownMode::RECONSTRUCT_EXPRESSION) {
			path.casts.emplace_back(tgt, BoundCastExpression::IsTryCast(bound_cast)
			                                 ? RuntimeFilterCastMode::TRY_CAST
			                                 : RuntimeFilterCastMode::DEFAULT_CAST);
		}
		return true;
	}
	default:
		return false;
	}
}

bool JoinFilterPushdownUtil::PushdownJoinFilterExpression(const Expression &expr, JoinFilterPushdownColumn &filter) {
	JoinFilterExpressionPath path;
	if (!PushdownJoinFilterExpressionInternal(expr, path)) {
		return false;
	}
	filter.probe_column_index = path.binding;
	if (filter.mode == JoinFilterPushdownMode::STORAGE_ONLY) {
		return true;
	}
	if (path.mode == JoinFilterPushdownMode::STORAGE_ONLY) {
		filter.mode = JoinFilterPushdownMode::STORAGE_ONLY;
		filter.runtime_filter_casts.clear();
		return true;
	}
	filter.runtime_filter_casts.insert(filter.runtime_filter_casts.begin(), path.casts.begin(), path.casts.end());
	return true;
}

bool JoinFilterPushdownUtil::JoinTypeIsSupported(JoinType join_type) {
	switch (join_type) {
	case JoinType::MARK:
	case JoinType::SINGLE:
	case JoinType::LEFT:
	case JoinType::OUTER:
	case JoinType::ANTI:
	case JoinType::RIGHT_ANTI:
		// cannot generate join filters for these join types
		// mark/single - cannot change cardinality of probe side
		// left/outer always need to include every row from probe side
		// FIXME: anti/right_anti - we could do this, but need to invert the join conditions
		return false;
	default:
		return true;
	}
}

static void PushdownProjectionColumns(LogicalProjection &proj, const vector<JoinFilterPushdownColumn> &columns,
                                      vector<JoinFilterPushdownColumn> &projected_columns) {
	for (auto &filter : columns) {
		if (filter.probe_column_index.table_index != proj.table_index) {
			continue;
		}
		auto candidate = filter;
		auto &expr = proj.GetExpression(candidate.probe_column_index);
		if (!JoinFilterPushdownUtil::PushdownJoinFilterExpression(expr, candidate)) {
			continue;
		}
		projected_columns.push_back(std::move(candidate));
	}
}

static void PushdownAggregateColumns(LogicalAggregate &aggr, const vector<JoinFilterPushdownColumn> &columns,
                                     vector<JoinFilterPushdownColumn> &group_columns) {
	for (auto &filter : columns) {
		if (filter.probe_column_index.table_index != aggr.group_index) {
			continue;
		}
		const auto group_index = filter.probe_column_index.column_index;
		bool present_in_all = true;
		for (auto &grouping_set : aggr.grouping_sets) {
			present_in_all &= grouping_set.count(group_index) != 0;
		}
		if (!present_in_all) {
			continue;
		}
		auto candidate = filter;
		auto &expr = aggr.GetGroupExpression(group_index);
		if (!JoinFilterPushdownUtil::PushdownJoinFilterExpression(expr, candidate)) {
			continue;
		}
		group_columns.push_back(std::move(candidate));
	}
}

static void FindScanFilterTargets(LogicalGet &get, vector<JoinFilterPushdownColumn> columns,
                                  vector<PushdownFilterTarget> &targets) {
	if (!get.function.filter_pushdown || get.ordinality_idx.IsValid()) {
		return;
	}
	get.ResolveOperatorTypes();
	const auto bindings = get.GetColumnBindings();
	vector<JoinFilterPushdownColumn> accepted_columns;
	for (auto &filter : columns) {
		if (filter.probe_column_index.table_index != get.table_index) {
			continue;
		}
		if (get.function.supports_pushdown_type) {
			auto column_id = get.GetColumnIndex(filter.probe_column_index).GetPrimaryIndex();
			if (!get.function.supports_pushdown_type(*get.bind_data, column_id)) {
				continue;
			}
		}
		for (idx_t i = 0; i < bindings.size(); i++) {
			if (filter.probe_column_index.column_index == bindings[i].column_index) {
				filter.storage_type = get.types[i];
				break;
			}
		}
		D_ASSERT(filter.storage_type != LogicalType::INVALID);
		const auto reconstructed_type =
		    filter.runtime_filter_casts.empty() ? filter.storage_type : filter.runtime_filter_casts.back().target_type;
		if (filter.storage_type.id() == LogicalTypeId::VARIANT &&
		    filter.mode == JoinFilterPushdownMode::RECONSTRUCT_EXPRESSION &&
		    reconstructed_type.id() == LogicalTypeId::VARIANT) {
			continue;
		}
		accepted_columns.push_back(std::move(filter));
	}
	if (!accepted_columns.empty()) {
		targets.emplace_back(get, std::move(accepted_columns));
	}
}

static void FindSetOperationFilterTargets(LogicalSetOperation &setop, const vector<JoinFilterPushdownColumn> &columns,
                                          vector<PushdownFilterTarget> &targets) {
	for (auto &child : setop.children) {
		vector<JoinFilterPushdownColumn> child_columns;
		const auto child_bindings = child->GetColumnBindings();
		for (auto &filter : columns) {
			if (filter.probe_column_index.table_index != setop.table_index) {
				continue;
			}
			auto candidate = filter;
			candidate.probe_column_index = child_bindings[filter.probe_column_index.column_index];
			child_columns.push_back(std::move(candidate));
		}
		JoinFilterPushdownOptimizer::GetPushdownFilterTargets(*child, std::move(child_columns), targets);
		if (setop.type == LogicalOperatorType::LOGICAL_EXCEPT) {
			break;
		}
	}
}

static bool FilterTargetPreservesLeftInput(JoinType type) {
	switch (type) {
	case JoinType::INNER:
	case JoinType::LEFT:
	case JoinType::SEMI:
	case JoinType::ANTI:
	case JoinType::MARK:
	case JoinType::SINGLE:
		return true;
	default:
		// Filtering a NULL-generating input can create new unmatched rows.
		return false;
	}
}

void JoinFilterPushdownOptimizer::GetPushdownFilterTargets(LogicalOperator &op,
                                                           vector<JoinFilterPushdownColumn> columns,
                                                           vector<PushdownFilterTarget> &targets) {
	if (columns.empty()) {
		return;
	}
	bool is_volatile = false;
	LogicalOperatorVisitor::EnumerateExpressions(
	    op, [&](unique_ptr<Expression> *expr) { is_volatile |= (*expr)->IsVolatile(); });
	if (is_volatile) {
		return;
	}
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_GET:
		FindScanFilterTargets(op.Cast<LogicalGet>(), std::move(columns), targets);
		break;
	case LogicalOperatorType::LOGICAL_PROJECTION: {
		vector<JoinFilterPushdownColumn> projected_columns;
		PushdownProjectionColumns(op.Cast<LogicalProjection>(), columns, projected_columns);
		GetPushdownFilterTargets(*op.children[0], std::move(projected_columns), targets);
		break;
	}
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY: {
		vector<JoinFilterPushdownColumn> group_columns;
		PushdownAggregateColumns(op.Cast<LogicalAggregate>(), columns, group_columns);
		GetPushdownFilterTargets(*op.children[0], std::move(group_columns), targets);
		break;
	}
	case LogicalOperatorType::LOGICAL_ANY_JOIN: {
		auto &join = op.Cast<LogicalAnyJoin>();
		// Filtering a NULL-generating input can create new unmatched rows.
		if (join.join_type != JoinType::INNER) {
			break;
		}
		GetPushdownFilterTargets(*join.children[0], std::move(columns), targets);
		break;
	}
	case LogicalOperatorType::LOGICAL_UNNEST: {
		vector<JoinFilterPushdownColumn> input_columns;
		for (auto &filter : columns) {
			if (filter.probe_column_index.table_index != op.Cast<LogicalUnnest>().unnest_index) {
				input_columns.push_back(std::move(filter));
			}
		}
		GetPushdownFilterTargets(*op.children[0], std::move(input_columns), targets);
		break;
	}
	case LogicalOperatorType::LOGICAL_EXCEPT:
	case LogicalOperatorType::LOGICAL_INTERSECT:
	case LogicalOperatorType::LOGICAL_UNION:
		FindSetOperationFilterTargets(op.Cast<LogicalSetOperation>(), columns, targets);
		break;
	case LogicalOperatorType::LOGICAL_DISTINCT:
		if (op.Cast<LogicalDistinct>().distinct_type == DistinctType::DISTINCT) {
			GetPushdownFilterTargets(*op.children[0], std::move(columns), targets);
		}
		break;
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
		if (FilterTargetPreservesLeftInput(op.Cast<LogicalComparisonJoin>().join_type)) {
			GetPushdownFilterTargets(*op.children[0], std::move(columns), targets);
		}
		break;
	case LogicalOperatorType::LOGICAL_ORDER_BY:
	case LogicalOperatorType::LOGICAL_FILTER:
	case LogicalOperatorType::LOGICAL_CROSS_PRODUCT:
		GetPushdownFilterTargets(*op.children[0], std::move(columns), targets);
		break;
	default:
		break;
	}
}

bool JoinFilterPushdownOptimizer::IsFiltering(const unique_ptr<LogicalOperator> &op) {
	switch (op->type) {
	case LogicalOperatorType::LOGICAL_GET: {
		auto &get = op->Cast<LogicalGet>();
		return get.table_filters.HasFilters();
	}
	case LogicalOperatorType::LOGICAL_FILTER: {
		return true;
	}
	case LogicalOperatorType::LOGICAL_TOP_N: {
		return true;
	}
	default:
		for (const unique_ptr<LogicalOperator> &child : op->children) {
			if (IsFiltering(child)) {
				return true;
			}
		}
		return false;
	}
}

static bool IsDirectScanFilterPath(const LogicalOperator &op) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_GET:
		return true;
	case LogicalOperatorType::LOGICAL_PROJECTION:
	case LogicalOperatorType::LOGICAL_FILTER:
		return IsDirectScanFilterPath(*op.children[0]);
	default:
		return false;
	}
}

void JoinFilterPushdownOptimizer::GenerateJoinFilters(LogicalComparisonJoin &join) {
	if (!JoinFilterPushdownUtil::JoinTypeIsSupported(join.join_type)) {
		return;
	}
	for (auto &cond : join.conditions) {
		auto &expr = cond.IsComparison() ? cond.GetLHS() : cond.GetJoinExpression();
		if (expr.IsVolatile()) {
			return;
		}
	}
	// re-order conditions here - otherwise this will happen later on and invalidate the indexes we generate
	PhysicalComparisonJoin::ReorderConditions(join.conditions);
	auto pushdown_info = make_uniq<JoinFilterPushdownInfo>();

	vector<JoinFilterPushdownColumn> pushdown_columns;
	for (idx_t cond_idx = 0; cond_idx < join.conditions.size(); cond_idx++) {
		auto &cond = join.conditions[cond_idx];
		if (!cond.IsComparison()) {
			continue;
		}
		switch (cond.GetComparisonType()) {
		case ExpressionType::COMPARE_EQUAL:
		case ExpressionType::COMPARE_LESSTHAN:
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		case ExpressionType::COMPARE_GREATERTHAN:
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			break;
		case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
			if (!IsDirectScanFilterPath(*join.children[0])) {
				continue;
			}
			break;
		default:
			continue;
		}

		JoinFilterPushdownColumn pushdown_col;
		if (!JoinFilterPushdownUtil::PushdownJoinFilterExpression(cond.GetLHS(), pushdown_col)) {
			continue;
		}

		pushdown_col.join_filter_idx = pushdown_info->join_condition.size();
		pushdown_columns.push_back(pushdown_col);
		pushdown_info->join_condition.push_back(cond_idx);
	}
	if (pushdown_columns.empty()) {
		// could not generate any filters - bail-out
		return;
	}
	// recurse the query tree to find the LogicalGets in which we can push the filter info
	vector<PushdownFilterTarget> pushdown_filter_targets;
	GetPushdownFilterTargets(*join.children[0], pushdown_columns, pushdown_filter_targets);
	for (auto &target : pushdown_filter_targets) {
		vector<JoinFilterPushdownColumn> get_columns;
		for (auto &column : target.columns) {
			const auto comparison =
			    join.conditions[pushdown_info->join_condition[column.join_filter_idx]].GetComparisonType();
			if (comparison == ExpressionType::COMPARE_NOT_DISTINCT_FROM &&
			    column.mode != JoinFilterPushdownMode::RECONSTRUCT_EXPRESSION) {
				continue;
			}
			get_columns.push_back(std::move(column));
		}
		if (get_columns.empty()) {
			continue;
		}
		auto &get = target.get;
		// pushdown info can be applied to this LogicalGet - push the dynamic table filter set
		if (!get.dynamic_filters) {
			get.dynamic_filters = make_shared_ptr<DynamicTableFilterSet>();
		}

		JoinFilterPushdownFilter get_filter;
		get_filter.dynamic_filters = get.dynamic_filters;
		get_filter.columns = std::move(get_columns);
		if (get.has_estimated_cardinality) {
			get_filter.estimated_scan_cardinality = get.estimated_cardinality;
		}
		pushdown_info->probe_info.push_back(std::move(get_filter));
	}

	// Even if we cannot find any table sources in which we can push down filters,
	// we still initialize the aggregate states so that we have the possibility of doing a perfect hash join
	// TODO: Can ExpressionType::COMPARE_NOT_DISTINCT_FROM be used with perfect hash joins?
	const auto compute_aggregates_anyway =
	    join.join_type == JoinType::INNER && join.conditions.size() == 1 && pushdown_info->join_condition.size() == 1 &&
	    join.conditions[0].IsComparison() && join.conditions[0].GetComparisonType() == ExpressionType::COMPARE_EQUAL &&
	    TypeIsIntegral(join.conditions[0].GetRHS().GetReturnType().InternalType());
	if (pushdown_info->probe_info.empty() && !compute_aggregates_anyway) {
		// no table sources found in which we can push down filters
		return;
	}

	// set up the min/max aggregates for each of the filters
	auto &context = optimizer.GetContext();
	for (auto &join_condition : pushdown_info->join_condition) {
		for (const auto &aggr_name : {MinFun::Name, MaxFun::Name}) {
			FunctionBinder function_binder(context);
			vector<unique_ptr<Expression>> aggr_children;
			aggr_children.push_back(join.conditions[join_condition].GetRHS().Copy());
			// the one-argument overload of the min/max set is the plain aggregate, min(x, n) takes two
			auto aggr = GetBuiltinAggregateFunction(context, aggr_name, {aggr_children[0]->GetReturnType()});
			auto aggr_expr = function_binder.BindAggregateFunction(std::move(aggr), std::move(aggr_children), nullptr,
			                                                       AggregateType::NON_DISTINCT);
			if (aggr_expr->GetChildren().size() != 1) {
				// min/max with collation - not supported
				return;
			}
			pushdown_info->min_max_aggregates.push_back(std::move(aggr_expr));
		}
	}
	if (!pushdown_info->probe_info.empty()) {
		const idx_t child_idx = join.children[1]->type == LogicalOperatorType::LOGICAL_DELIM_GET ? 0 : 1;
		pushdown_info->build_side_has_filter = IsFiltering(join.children[child_idx]);
	}

	// set up the filter pushdown in the join itself
	join.filter_pushdown = std::move(pushdown_info);
}

void JoinFilterPushdownOptimizer::VisitOperator(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		// comparison join - try to generate join filters (if possible)
		GenerateJoinFilters(op.Cast<LogicalComparisonJoin>());
	}
	LogicalOperatorVisitor::VisitOperator(op);
}

} // namespace duckdb
