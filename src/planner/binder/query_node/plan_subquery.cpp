#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"
#include "duckdb/function/aggregate/distributive_functions.hpp"
#include "duckdb/function/aggregate/distributive_function_utils.hpp"
#include "duckdb/function/builtin_function_lookup.hpp"
#include "duckdb/function/scalar/generic_functions.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_subquery_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/list.hpp"
#include "duckdb/planner/operator/logical_any_join.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_window.hpp"
#include "duckdb/planner/joinside.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/planner/subquery/flatten_dependent_join.hpp"
#include "duckdb/common/enums/logical_operator_type.hpp"
#include "duckdb/planner/operator/logical_dependent_join.hpp"
#include "duckdb/planner/subquery/recursive_dependent_join_planner.hpp"
#include "duckdb/function/scalar/generic_functions.hpp"
#include "duckdb/function/scalar/struct_functions.hpp"
#include "duckdb/main/settings.hpp"

namespace duckdb {

static bool PlanReturnsExactlyOneRow(const LogicalOperator &op) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY: {
		auto &aggr = op.Cast<LogicalAggregate>();
		return aggr.groups.empty() && aggr.grouping_sets.empty() && aggr.grouping_functions.empty();
	}
	case LogicalOperatorType::LOGICAL_DUMMY_SCAN:
		return true;
	case LogicalOperatorType::LOGICAL_LIMIT: {
		auto &limit = op.Cast<LogicalLimit>();
		if (limit.limit_val.Type() != LimitNodeType::CONSTANT_VALUE || limit.limit_val.GetConstantValue() != 1) {
			return false;
		}
		if (limit.offset_val.Type() != LimitNodeType::UNSET &&
		    (limit.offset_val.Type() != LimitNodeType::CONSTANT_VALUE || limit.offset_val.GetConstantValue() != 0)) {
			return false;
		}
		return op.children.size() == 1 && PlanReturnsExactlyOneRow(*op.children[0]);
	}
	case LogicalOperatorType::LOGICAL_PROJECTION:
		return op.children.size() == 1 && PlanReturnsExactlyOneRow(*op.children[0]);
	default:
		return false;
	}
}

static bool IsExtremumRewriteValid(const BoundSubqueryExpression &expr) {
	// TODO/FIXME: Generalize this rewrite to multi-column subqueries
	if (expr.GetChildren().size() != 1 || expr.GetChildTypes().size() != 1 || expr.GetChildTargets().size() != 1) {
		return false;
	}
	auto cmp_type = expr.ComparisonType();
	return cmp_type == ExpressionType::COMPARE_GREATERTHAN ||
	       cmp_type == ExpressionType::COMPARE_GREATERTHANOREQUALTO || cmp_type == ExpressionType::COMPARE_LESSTHAN ||
	       cmp_type == ExpressionType::COMPARE_LESSTHANOREQUALTO;
}

static unique_ptr<Expression> PlanExtremumRewrite(Binder &binder, BoundSubqueryExpression &expr,
                                                  unique_ptr<LogicalOperator> &plan) {
	auto &child_type = expr.GetChildTypes()[0];
	auto plan_columns = plan->GetColumnBindings();
	D_ASSERT(plan_columns.size() == 1);
	auto plan_column = plan_columns[0];

	vector<unique_ptr<Expression>> aggregate_list;

	auto bound_colref = make_uniq<BoundColumnRefExpression>(child_type, plan_column);

	FunctionBinder function_binder(binder);
	// 1. MIN or MAX
	auto cmp_type = expr.ComparisonType();
	bool is_min =
	    (cmp_type == ExpressionType::COMPARE_GREATERTHAN || cmp_type == ExpressionType::COMPARE_GREATERTHANOREQUALTO);

	auto &compare_type = expr.GetChildTargets()[0];
	vector<unique_ptr<Expression>> min_max_children;
	auto min_max_child = bound_colref->Copy();
	// The comparison may carry a collation the subquery column does not, and the extremum has to be taken
	// under that collation, so it is pushed here. BIT and VARIANT are the exception: min/max pushes its own
	// collation for those, and doing it first makes that branch a no-op, after which the aggregate falls
	// through to its binary-key implementation whose return type is BLOB.
	if (compare_type.id() != LogicalTypeId::BIT && compare_type.id() != LogicalTypeId::VARIANT) {
		ExpressionBinder::PushCollation(binder.context, min_max_child, compare_type);
	}
	min_max_children.push_back(std::move(min_max_child));

	auto extremum_fun = GetBuiltinAggregateFunction(binder.context, is_min ? MinFun::Name : MaxFun::Name,
	                                                {min_max_children[0]->GetReturnType()});
	auto extremum_aggr = function_binder.BindAggregateFunction(extremum_fun, std::move(min_max_children), nullptr,
	                                                           AggregateType::NON_DISTINCT);
	auto extremum_type = extremum_aggr->GetReturnType();
	aggregate_list.push_back(std::move(extremum_aggr));

	// 2. count_star = COUNT(*)
	auto count_star_aggr = function_binder.BindAggregateFunction(
	    GetBuiltinAggregateFunction(binder.context, CountStarFun::Name, {}), {}, nullptr, AggregateType::NON_DISTINCT);
	aggregate_list.push_back(std::move(count_star_aggr));

	// 3. count_child = COUNT(child)
	vector<unique_ptr<Expression>> count_child_children;
	count_child_children.push_back(bound_colref->Copy());
	auto count_fun =
	    GetBuiltinAggregateFunction(binder.context, CountFun::Name, {count_child_children[0]->GetReturnType()});
	auto count_child_aggr = function_binder.BindAggregateFunction(count_fun, std::move(count_child_children), nullptr,
	                                                              AggregateType::NON_DISTINCT);
	aggregate_list.push_back(std::move(count_child_aggr));

	auto aggr_index = binder.GenerateTableIndex();
	auto aggregate = make_uniq<LogicalAggregate>(binder.GenerateTableIndex(), aggr_index, std::move(aggregate_list));
	aggregate->AddChild(std::move(plan));
	plan = std::move(aggregate);

	auto count_star_ref =
	    make_uniq<BoundColumnRefExpression>(LogicalType::BIGINT, ColumnBinding(aggr_index, ProjectionIndex(1)));
	auto count_child_ref =
	    make_uniq<BoundColumnRefExpression>(LogicalType::BIGINT, ColumnBinding(aggr_index, ProjectionIndex(2)));
	// reference the aggregate by the type it actually produces, not by the subquery column type
	auto extremum_ref =
	    make_uniq<BoundColumnRefExpression>(extremum_type, ColumnBinding(aggr_index, ProjectionIndex(0)));

	auto &x_expr = *expr.GetChildrenMutable()[0];

	auto false_val = make_uniq<BoundConstantExpression>(Value::BOOLEAN(false));
	auto null_val = make_uniq<BoundConstantExpression>(Value(LogicalType::BOOLEAN));
	auto true_val = make_uniq<BoundConstantExpression>(Value::BOOLEAN(true));

	// 5. ELSE FALSE
	unique_ptr<Expression> current_else = false_val->Copy();

	// 4. WHEN count_star > count_child THEN NULL
	unique_ptr<Expression> count_star_gt_count_child = BoundComparisonExpression::Create(
	    ExpressionType::COMPARE_GREATERTHAN, count_star_ref->Copy(), count_child_ref->Copy());
	current_else =
	    make_uniq<BoundCaseExpression>(std::move(count_star_gt_count_child), null_val->Copy(), std::move(current_else));

	// 3. WHEN X > MIN(Y) THEN TRUE
	auto x_cast = BoundCastExpression::AddDefaultCastToType(x_expr.Copy(), compare_type);
	auto extremum_cast = BoundCastExpression::AddDefaultCastToType(std::move(extremum_ref), compare_type);
	unique_ptr<Expression> x_op_min =
	    BoundComparisonExpression::Create(cmp_type, std::move(x_cast), std::move(extremum_cast));

	// Push collations for the comparison
	ExpressionBinder::PushCollation(binder.context,
	                                BoundComparisonExpression::LeftMutable(x_op_min->Cast<BoundFunctionExpression>()),
	                                compare_type);
	ExpressionBinder::PushCollation(binder.context,
	                                BoundComparisonExpression::RightMutable(x_op_min->Cast<BoundFunctionExpression>()),
	                                compare_type);

	current_else = make_uniq<BoundCaseExpression>(std::move(x_op_min), true_val->Copy(), std::move(current_else));

	// 2. WHEN X IS NULL THEN NULL
	unique_ptr<Expression> x_is_null =
	    make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NULL, LogicalType::BOOLEAN);
	x_is_null->Cast<BoundOperatorExpression>().GetChildrenMutable().push_back(x_expr.Copy());
	current_else = make_uniq<BoundCaseExpression>(std::move(x_is_null), null_val->Copy(), std::move(current_else));

	// 1. WHEN count_star == 0 THEN FALSE
	auto zero_val = make_uniq<BoundConstantExpression>(Value::BIGINT(0));
	unique_ptr<Expression> count_star_is_zero =
	    BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL, count_star_ref->Copy(), std::move(zero_val));
	current_else =
	    make_uniq<BoundCaseExpression>(std::move(count_star_is_zero), false_val->Copy(), std::move(current_else));

	// 0. WHEN count_star IS NULL THEN FALSE (for correlated empty groups)
	unique_ptr<Expression> count_star_is_null =
	    make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NULL, LogicalType::BOOLEAN);
	count_star_is_null->Cast<BoundOperatorExpression>().GetChildrenMutable().push_back(count_star_ref->Copy());
	current_else =
	    make_uniq<BoundCaseExpression>(std::move(count_star_is_null), false_val->Copy(), std::move(current_else));

	return current_else;
}

static bool IsMultiColumnTuple(const LogicalType &type) {
	return type.id() == LogicalTypeId::TUPLE && StructType::GetChildCount(type) > 1;
}

static bool IsMultiColumnComparison(const BoundSubqueryExpression &expr) {
	if (expr.GetChildren().size() > 1 || expr.GetChildTypes().size() > 1) {
		return true;
	}
	if (expr.ComparisonType() == ExpressionType::COMPARE_EQUAL) {
		// equality on a TUPLE is a single struct comparison, which is supported
		return false;
	}
	for (auto &child : expr.GetChildren()) {
		if (IsMultiColumnTuple(child->GetReturnType())) {
			return true;
		}
	}
	for (auto &child_type : expr.GetChildTypes()) {
		if (IsMultiColumnTuple(child_type)) {
			return true;
		}
	}
	return false;
}

static unique_ptr<Expression> PlanUncorrelatedSubquery(Binder &binder, BoundSubqueryExpression &expr,
                                                       unique_ptr<LogicalOperator> &root,
                                                       unique_ptr<LogicalOperator> plan) {
	D_ASSERT(!expr.IsCorrelated());
	switch (expr.GetSubqueryType()) {
	case SubqueryType::EXISTS: {
		// uncorrelated EXISTS
		// we only care about existence, hence we push a LIMIT 1 operator
		auto limit = make_uniq<LogicalLimit>(BoundLimitNode::ConstantValue(1), BoundLimitNode());
		limit->AddChild(std::move(plan));
		plan = std::move(limit);

		// now we push a COUNT(*) aggregate onto the limit, this will be either 0 or 1 (EXISTS or NOT EXISTS)
		auto count_star_fun = GetBuiltinAggregateFunction(binder.context, CountStarFun::Name, {});

		FunctionBinder function_binder(binder);
		auto count_star =
		    function_binder.BindAggregateFunction(count_star_fun, {}, nullptr, AggregateType::NON_DISTINCT);
		auto idx_type = count_star->GetReturnType();
		vector<unique_ptr<Expression>> aggregate_list;
		aggregate_list.push_back(std::move(count_star));
		auto aggregate_index = binder.GenerateTableIndex();
		auto aggregate =
		    make_uniq<LogicalAggregate>(binder.GenerateTableIndex(), aggregate_index, std::move(aggregate_list));
		aggregate->AddChild(std::move(plan));
		plan = std::move(aggregate);

		// now we push a projection with a comparison to 1
		auto left_child =
		    make_uniq<BoundColumnRefExpression>(idx_type, ColumnBinding(aggregate_index, ProjectionIndex(0)));
		auto right_child = make_uniq<BoundConstantExpression>(Value::Numeric(idx_type, 1));
		auto comparison = BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL, std::move(left_child),
		                                                    std::move(right_child));

		vector<unique_ptr<Expression>> projection_list;
		projection_list.push_back(std::move(comparison));
		auto projection_index = binder.GenerateTableIndex();
		auto projection = make_uniq<LogicalProjection>(projection_index, std::move(projection_list));
		projection->AddChild(std::move(plan));
		plan = std::move(projection);

		// we add it to the main query by adding a cross product
		// FIXME: should use something else besides cross product as we always add only one scalar constant
		root = LogicalCrossProduct::Create(std::move(root), std::move(plan));

		// we replace the original subquery with a ColumnRefExpression referring to the result of the projection (either
		// TRUE or FALSE)
		return make_uniq<BoundColumnRefExpression>(Identifier(expr.GetName()), LogicalType::BOOLEAN,
		                                           ColumnBinding(projection_index, ProjectionIndex(0)));
	}
	case SubqueryType::SCALAR: {
		// uncorrelated scalar, we want to return the first entry
		// figure out the table index of the bound table of the entry which we want to return
		auto bindings = plan->GetColumnBindings();
		D_ASSERT(bindings.size() == 1);
		if (expr.GetReturnType().id() != LogicalTypeId::SQLNULL && PlanReturnsExactlyOneRow(*plan)) {
			auto result = make_uniq<BoundColumnRefExpression>(expr.GetName(), expr.GetReturnType(), bindings[0]);
			root = LogicalCrossProduct::Create(std::move(root), std::move(plan));
			return std::move(result);
		}
		auto table_idx = bindings[0].table_index;

		bool error_on_multiple_rows = Settings::Get<ScalarSubqueryErrorOnMultipleRowsSetting>(binder.context);

		// we push an aggregate that returns the FIRST element
		vector<unique_ptr<Expression>> expressions;
		auto bound =
		    make_uniq<BoundColumnRefExpression>(expr.GetReturnType(), ColumnBinding(table_idx, ProjectionIndex(0)));
		vector<unique_ptr<Expression>> first_children;
		first_children.push_back(std::move(bound));

		FunctionBinder function_binder(binder);
		auto first_fun = GetBuiltinAggregateFunction(binder.context, FirstFun::Name, {expr.GetReturnType()});
		auto first_agg = function_binder.BindAggregateFunction(first_fun, std::move(first_children), nullptr,
		                                                       AggregateType::NON_DISTINCT);

		expressions.push_back(std::move(first_agg));
		if (error_on_multiple_rows) {
			vector<unique_ptr<Expression>> count_children;
			auto count_agg = function_binder.BindAggregateFunction(
			    GetBuiltinAggregateFunction(binder.context, CountStarFun::Name, {}), std::move(count_children), nullptr,
			    AggregateType::NON_DISTINCT);
			expressions.push_back(std::move(count_agg));
		}
		auto aggr_index = binder.GenerateTableIndex();

		auto aggr = make_uniq<LogicalAggregate>(binder.GenerateTableIndex(), aggr_index, std::move(expressions));
		aggr->AddChild(std::move(plan));
		plan = std::move(aggr);

		if (error_on_multiple_rows) {
			// CASE WHEN count > 1 THEN error('Scalar subquery can only return a single row') ELSE first_agg END
			auto proj_index = binder.GenerateTableIndex();

			auto first_ref = make_uniq<BoundColumnRefExpression>(plan->expressions[0]->GetReturnType(),
			                                                     ColumnBinding(aggr_index, ProjectionIndex(0)));
			auto count_ref = make_uniq<BoundColumnRefExpression>(plan->expressions[1]->GetReturnType(),
			                                                     ColumnBinding(aggr_index, ProjectionIndex(1)));

			auto constant_one = make_uniq<BoundConstantExpression>(Value::BIGINT(1));
			auto count_check = BoundComparisonExpression::Create(ExpressionType::COMPARE_GREATERTHAN,
			                                                     std::move(count_ref), std::move(constant_one));

			vector<unique_ptr<Expression>> error_children;
			error_children.push_back(make_uniq<BoundConstantExpression>(
			    Value("More than one row returned by a subquery used as an expression - scalar subqueries can only "
			          "return a single row.\n\nUse \"SET scalar_subquery_error_on_multiple_rows=false\" to revert to "
			          "previous behavior of returning a random row.")));
			auto error_expr = BindBuiltinScalarFunction(binder.context, ErrorFun::Name, std::move(error_children));
			error_expr->SetReturnType(first_ref->GetReturnType());
			auto case_expr =
			    make_uniq<BoundCaseExpression>(std::move(count_check), std::move(error_expr), std::move(first_ref));

			vector<unique_ptr<Expression>> proj_expressions;
			proj_expressions.push_back(std::move(case_expr));

			auto proj = make_uniq<LogicalProjection>(proj_index, std::move(proj_expressions));
			proj->AddChild(std::move(plan));
			plan = std::move(proj);

			aggr_index = proj_index;
		}

		// in the uncorrelated case, we add the value to the main query through a cross product
		// FIXME: should use something else besides cross product as we always add only one scalar constant and cross
		// product is not optimized for this.
		D_ASSERT(root);
		root = LogicalCrossProduct::Create(std::move(root), std::move(plan));

		// we replace the original subquery with a BoundColumnRefExpression referring to the first result of the
		// aggregation
		return make_uniq<BoundColumnRefExpression>(Identifier(expr.GetName()), expr.GetReturnType(),
		                                           ColumnBinding(aggr_index, ProjectionIndex(0)));
	}
	default: {
		D_ASSERT(expr.GetSubqueryType() == SubqueryType::ANY);
		if (IsExtremumRewriteValid(expr)) {
			auto result = PlanExtremumRewrite(binder, expr, plan);
			root = LogicalCrossProduct::Create(std::move(root), std::move(plan));
			return result;
		}

		// we generate a MARK join that results in either (TRUE, FALSE or NULL)
		// subquery has NULL values -> result is (TRUE or NULL)
		// subquery has no NULL values -> result is (TRUE, FALSE or NULL [if input is NULL])
		// fetch the column bindings
		auto plan_columns = plan->GetColumnBindings();

		// then we generate the MARK join with the subquery
		auto mark_index = binder.GenerateTableIndex();
		auto join = make_uniq<LogicalComparisonJoin>(JoinType::MARK);
		join->mark_index = mark_index;
		join->AddChild(std::move(root));
		join->AddChild(std::move(plan));

		// create the JOIN condition
		// Special case: if we have a single struct child and multiple types,
		// this means we kept the struct intact for ordered comparison (e.g., (a,b) < ANY(...))
		// We need to construct a corresponding struct on the RHS from the subquery columns
		if (expr.GetChildren().size() == 1 && expr.GetChildTypes().size() > 1) {
			// Construct a struct on the RHS from the subquery columns
			vector<unique_ptr<Expression>> struct_children;
			struct_children.reserve(expr.GetChildTypes().size());
			for (idx_t i = 0; i < expr.GetChildTypes().size(); i++) {
				auto &child_type = expr.GetChildTypes()[i];
				auto &compare_type = expr.GetChildTargets()[i];
				auto colref = BoundCastExpression::AddDefaultCastToType(
				    make_uniq<BoundColumnRefExpression>(child_type, plan_columns[i]), compare_type);
				struct_children.push_back(std::move(colref));
			}

			// Create a struct expression from the subquery columns using the "row" function
			FunctionBinder function_binder(binder);
			auto struct_expr = BindBuiltinScalarFunction(binder.context, RowFun::Name, std::move(struct_children));

			JoinCondition cond(std::move(expr.GetChildrenMutable()[0]), std::move(struct_expr), expr.ComparisonType());

			// push collations
			ExpressionBinder::PushCollation(binder.context, cond.LeftReference(), cond.GetLHS().GetReturnType());
			ExpressionBinder::PushCollation(binder.context, cond.RightReference(), cond.GetRHS().GetReturnType());

			join->conditions.push_back(std::move(cond));
		} else {
			// Standard case: compare each child separately
			for (idx_t child_idx = 0; child_idx < expr.GetChildren().size(); child_idx++) {
				auto &child_type = expr.GetChildTypes()[child_idx];
				auto &compare_type = expr.GetChildTargets()[child_idx];
				auto right_expr = BoundCastExpression::AddDefaultCastToType(
				    make_uniq<BoundColumnRefExpression>(child_type, plan_columns[child_idx]), compare_type);
				JoinCondition cond(std::move(expr.GetChildrenMutable()[child_idx]), std::move(right_expr),
				                   expr.ComparisonType());

				// push collations
				ExpressionBinder::PushCollation(binder.context, cond.LeftReference(), compare_type);
				ExpressionBinder::PushCollation(binder.context, cond.RightReference(), compare_type);

				join->conditions.push_back(std::move(cond));
			}
		}
		root = std::move(join);

		// we replace the original subquery with a BoundColumnRefExpression referring to the mark column
		return make_uniq<BoundColumnRefExpression>(Identifier(expr.GetName()), expr.GetReturnType(),
		                                           ColumnBinding(mark_index, ProjectionIndex(0)));
	}
	}
}

static unique_ptr<LogicalDependentJoin> CreateDuplicateEliminatedJoin(const CorrelatedColumns &correlated_columns,
                                                                      JoinType join_type,
                                                                      unique_ptr<LogicalOperator> original_plan,
                                                                      bool perform_delim) {
	auto delim_join = make_uniq<LogicalDependentJoin>(join_type);
	delim_join->correlated_columns = correlated_columns;
	delim_join->perform_delim = perform_delim;
	delim_join->join_type = join_type;
	delim_join->AddChild(std::move(original_plan));
	return delim_join;
}

static bool PerformDelimOnType(const LogicalType &type) {
	if (type.InternalType() == PhysicalType::LIST) {
		return false;
	}
	if (type.InternalType() == PhysicalType::STRUCT) {
		for (auto &entry : StructType::GetChildTypes(type)) {
			if (!PerformDelimOnType(entry.second)) {
				return false;
			}
		}
	}
	return true;
}

static bool PerformDuplicateElimination(Binder &binder, CorrelatedColumns &correlated_columns,
                                        optional_ptr<LogicalOperator> dependent_plan = nullptr) {
	bool perform_delim = !dependent_plan || !dependent_plan->HasVolatileExpressions();
	if (perform_delim && !Settings::Get<EnableOptimizerSetting>(binder.context)) {
		// if optimizations are disabled we always do a delim join
		return true;
	}
	if (perform_delim) {
		for (auto &col : correlated_columns) {
			if (!PerformDelimOnType(col.type)) {
				perform_delim = false;
				break;
			}
		}
	}
	if (perform_delim) {
		return true;
	}
	auto binding = ColumnBinding(binder.GenerateTableIndex(), ProjectionIndex(0));
	auto type = LogicalType::BIGINT;
	auto name = "delim_index";
	CorrelatedColumnInfo info(binding, type, name, 0);
	correlated_columns.AddColumn(std::move(info));
	correlated_columns.SetDelimIndexToZero();
	return false;
}

static unique_ptr<Expression> PlanCorrelatedSubquery(Binder &binder, BoundSubqueryExpression &expr,
                                                     unique_ptr<LogicalOperator> &root,
                                                     unique_ptr<LogicalOperator> plan) {
	auto &correlated_columns = expr.GetBinder()->correlated_columns;
	// Preserve the existing duplicate-elimination path for deterministic ANY queries. Volatile ANY queries require
	// row identity for the same reason as other correlated subqueries: sharing one result between equal outer values
	// changes their evaluation cardinality.
	bool perform_delim = expr.GetSubqueryType() == SubqueryType::ANY && !plan->HasVolatileExpressions()
	                         ? true
	                         : PerformDuplicateElimination(binder, correlated_columns, plan);
	D_ASSERT(expr.IsCorrelated());
	// correlated subquery
	// for a more in-depth explanation of this code, read the paper "Unnesting Arbitrary Subqueries"
	// also read "Improving Unnesting of Complex Queries"
	// we handle three types of correlated subqueries: Scalar, EXISTS and ANY
	// all three cases are very similar with some minor changes (mainly the type of join performed at the end)
	switch (expr.GetSubqueryType()) {
	case SubqueryType::SCALAR: {
		// correlated SCALAR query
		// first push a DUPLICATE ELIMINATED join
		// a duplicate eliminated join creates a duplicate eliminated copy of the LHS
		// and pushes it into any DUPLICATE_ELIMINATED SCAN operators on the RHS

		// in the SCALAR case, we create a SINGLE join (because we are only interested in obtaining the value)
		// NULL values are equal in this join because we join on the correlated columns ONLY
		// and e.g. in the query: SELECT (SELECT 42 FROM integers WHERE i1.i IS NULL LIMIT 1) FROM integers i1;
		// the input value NULL will generate the value 42, and we need to join NULL on the LHS with NULL on the RHS
		// the left side is the original plan
		// this is the side that will be duplicate eliminated and pushed into the RHS
		auto delim_join =
		    CreateDuplicateEliminatedJoin(correlated_columns, JoinType::SINGLE, std::move(root), perform_delim);

		// We have to store all information required to perform UNNESTING later.
		delim_join->any_join = false;

		auto plan_column = plan->GetColumnBindings().back();
		delim_join->AddChild(std::move(plan));
		root = std::move(delim_join);
		// finally push the BoundColumnRefExpression referring to the data element returned by the join
		return make_uniq<BoundColumnRefExpression>(Identifier(expr.GetName()), expr.GetReturnType(), plan_column);
	}
	case SubqueryType::EXISTS: {
		// correlated EXISTS query
		// this query is similar to the correlated SCALAR query, except we use a MARK join here
		auto mark_index = binder.GenerateTableIndex();
		auto delim_join =
		    CreateDuplicateEliminatedJoin(correlated_columns, JoinType::MARK, std::move(root), perform_delim);

		delim_join->mark_index = mark_index;
		delim_join->any_join = true;
		delim_join->AddChild(std::move(plan));
		root = std::move(delim_join);
		// finally push the BoundColumnRefExpression referring to the marker
		return make_uniq<BoundColumnRefExpression>(Identifier(expr.GetName()), expr.GetReturnType(),
		                                           ColumnBinding(mark_index, ProjectionIndex(0)));
	}
	default: {
		D_ASSERT(expr.GetSubqueryType() == SubqueryType::ANY);

		if (IsExtremumRewriteValid(expr)) {
			auto result = PlanExtremumRewrite(binder, expr, plan);
			// For the extremum rewrite, we treat the correlated subquery just like a SCALAR query.
			// It produces a single aggregated result per RHS execution.
			auto delim_join =
			    CreateDuplicateEliminatedJoin(correlated_columns, JoinType::SINGLE, std::move(root), perform_delim);
			delim_join->any_join = false;
			delim_join->AddChild(std::move(plan));
			root = std::move(delim_join);
			return result;
		}

		// correlated ANY query
		// this query is similar to the correlated SCALAR query
		// however, in this case we push a correlated MARK join
		// note that in this join null values are NOT equal for ALL columns, but ONLY for the correlated columns
		// the correlated mark join handles this case by itself
		// as the MARK join has one extra join condition (the original condition, of the ANY expression, e.g.
		// [i=ANY(...)])
		auto mark_index = binder.GenerateTableIndex();
		auto delim_join =
		    CreateDuplicateEliminatedJoin(correlated_columns, JoinType::MARK, std::move(root), perform_delim);

		delim_join->mark_index = mark_index;
		delim_join->any_join = true;
		auto &dependent_join = plan;

		if (IsMultiColumnComparison(expr)) {
			// FIXME: the code to generate the plan here is actually correct
			// the problem is in the hash join - specifically PhysicalHashJoin::InitializeHashTable
			// this contains code that is hard-coded for a single comparison
			// -> (delim_types.size() + 1 == conditions.size())
			// this needs to be generalized to get this to work
			throw NotImplementedException("Correlated IN/ANY/ALL with multiple columns not yet supported");
		}

		auto plan_columns = dependent_join->GetColumnBindings();
		D_ASSERT(expr.GetChildren().size() == 1);
		auto left_expr = std::move(expr.GetChildrenMutable()[0]);
		auto &child_type = expr.GetChildTypes()[0];
		auto &compare_type = expr.GetChildTargets()[0];
		auto right_expr = BoundCastExpression::AddDefaultCastToType(
		    make_uniq<BoundColumnRefExpression>(child_type, plan_columns[0]), compare_type);
		ExpressionBinder::PushCollation(binder.context, left_expr, compare_type);
		ExpressionBinder::PushCollation(binder.context, right_expr, compare_type);
		delim_join->condition =
		    BoundComparisonExpression::Create(expr.ComparisonType(), std::move(left_expr), std::move(right_expr));

		delim_join->AddChild(std::move(dependent_join));
		root = std::move(delim_join);
		// finally push the BoundColumnRefExpression referring to the marker
		return make_uniq<BoundColumnRefExpression>(Identifier(expr.GetName()), expr.GetReturnType(),
		                                           ColumnBinding(mark_index, ProjectionIndex(0)));
	}
	}
}

static JoinSide GetCurrentJoinSide(LogicalJoin &join, const Expression &expr) {
	unordered_set<TableIndex> left_bindings;
	unordered_set<TableIndex> right_bindings;
	LogicalJoin::GetTableReferences(*join.children[0], left_bindings);
	LogicalJoin::GetTableReferences(*join.children[1], right_bindings);
	return JoinSide::GetCurrentJoinSide(expr, left_bindings, right_bindings);
}

static unique_ptr<LogicalOperator> &GetJoinSideRoot(LogicalJoin &join, JoinSide side, JoinSide uncorrelated_side) {
	if (side == JoinSide::NONE) {
		side = uncorrelated_side;
	}
	if (side == JoinSide::LEFT) {
		return join.children[0];
	}
	if (side == JoinSide::RIGHT) {
		return join.children[1];
	}
	throw NotImplementedException("Correlated subquery in a non-inner join condition cannot reference both sides of "
	                              "the join");
}

static bool HasJoinConditionExpressions(LogicalOperatorType type) {
	switch (type) {
	case LogicalOperatorType::LOGICAL_ASOF_JOIN:
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
	case LogicalOperatorType::LOGICAL_ANY_JOIN:
		return true;
	default:
		return false;
	}
}

void RecursiveDependentJoinPlanner::PlanJoinSubqueries(LogicalJoin &join, unique_ptr<Expression> &expr,
                                                       JoinSide uncorrelated_side) {
	if (!expr) {
		return;
	}
	ExpressionIterator::EnumerateChildren(
	    *expr, [&](unique_ptr<Expression> &child) { PlanJoinSubqueries(join, child, uncorrelated_side); });
	if (expr->GetExpressionClass() != ExpressionClass::BOUND_SUBQUERY) {
		return;
	}

	auto side = GetCurrentJoinSide(join, *expr);
	auto &root = GetJoinSideRoot(join, side, uncorrelated_side);
	auto &subquery = expr->Cast<BoundSubqueryExpression>();
	expr = binder.PlanSubquery(subquery, root);
}

void RecursiveDependentJoinPlanner::PlanJoinExpressions(LogicalOperator &op) {
	if (!HasJoinConditionExpressions(op.type)) {
		return;
	}
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_ASOF_JOIN:
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN: {
		auto &join = op.Cast<LogicalComparisonJoin>();
		for (auto &cond : join.conditions) {
			if (cond.IsComparison()) {
				PlanJoinSubqueries(join, cond.LeftReference(), JoinSide::LEFT);
				PlanJoinSubqueries(join, cond.RightReference(), JoinSide::RIGHT);
			} else {
				PlanJoinSubqueries(join, cond.JoinExpressionReference(), JoinSide::LEFT);
			}
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_ANY_JOIN: {
		auto &join = op.Cast<LogicalAnyJoin>();
		PlanJoinSubqueries(join, join.condition, JoinSide::LEFT);
		break;
	}
	default:
		break;
	}
}

void RecursiveDependentJoinPlanner::PlanJoinChildFilters(LogicalOperator &op) {
	if (!HasJoinConditionExpressions(op.type)) {
		return;
	}

	auto &join = op.Cast<LogicalJoin>();
	for (auto &child : join.children) {
		if (child->type != LogicalOperatorType::LOGICAL_FILTER) {
			continue;
		}
		auto &filter = child->Cast<LogicalFilter>();
		D_ASSERT(filter.children.size() == 1);
		for (auto &expr : filter.expressions) {
			binder.PlanSubqueries(expr, filter.children[0]);
		}
	}
}

BindingReplacementGraph RecursiveDependentJoinPlanner::PlanOperator(unique_ptr<LogicalOperator> &op_ptr) {
	PlanJoinChildFilters(*op_ptr);
	if (op_ptr->type == LogicalOperatorType::LOGICAL_ANY_JOIN &&
	    op_ptr->Cast<LogicalAnyJoin>().condition->HasSubquery()) {
		return PlanAnyJoinCondition(op_ptr);
	}
	auto old_output = op_ptr->GetColumnBindings();
	BindingReplacementGraph operator_replacements;
	if (!op_ptr->children.empty()) {
		// Collect all recursive CTEs during recursive descend
		if (op_ptr->type == LogicalOperatorType::LOGICAL_RECURSIVE_CTE ||
		    op_ptr->type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE) {
			auto &rec_cte = op_ptr->Cast<LogicalCTE>();
			binder.recursive_ctes[rec_cte.table_index] = op_ptr.get();
		}
		if (HasJoinConditionExpressions(op_ptr->type)) {
			PlanJoinExpressions(*op_ptr);
		} else {
			for (idx_t i = 0; i < op_ptr->children.size(); i++) {
				root = std::move(op_ptr->children[i]);
				D_ASSERT(root);
				VisitOperatorExpressions(*op_ptr);
				op_ptr->children[i] = std::move(root);
			}
		}

		for (idx_t i = 0; i < op_ptr->children.size(); i++) {
			D_ASSERT(op_ptr->children[i]);
			ColumnBindingRewrite::RewriteChild(
			    op_ptr, i, [&](unique_ptr<LogicalOperator> &child, BindingReplacementGraph &child_replacements) {
				    child_replacements = PlanOperator(child);
				    operator_replacements.Merge(child_replacements);
			    });
		}
	}
	ColumnBindingRewrite::ValidateOutput(old_output, op_ptr->GetColumnBindings(), operator_replacements);
	return operator_replacements;
}

BindingReplacementGraph RecursiveDependentJoinPlanner::PlanAnyJoinCondition(unique_ptr<LogicalOperator> &op_ptr) {
	auto old_output = op_ptr->GetColumnBindings();
	BindingReplacementGraph operator_replacements;

	for (idx_t child_index = 0; child_index < op_ptr->children.size(); child_index++) {
		ColumnBindingRewrite::RewriteChild(
		    op_ptr, child_index, [&](unique_ptr<LogicalOperator> &child, BindingReplacementGraph &child_replacements) {
			    child_replacements = PlanOperator(child);
			    operator_replacements.Merge(child_replacements);
		    });
	}

	BindingReplacementGraph pair_replacements;
	if (TryRewritePairDependentJoinCondition(binder, op_ptr, pair_replacements)) {
		operator_replacements.Merge(pair_replacements);
		auto recursive_replacements = PlanOperator(op_ptr);
		operator_replacements.Merge(recursive_replacements);
		ColumnBindingRewrite::ValidateOutput(old_output, op_ptr->GetColumnBindings(), operator_replacements);
		return operator_replacements;
	}

	auto &join = op_ptr->Cast<LogicalAnyJoin>();
	D_ASSERT(join.condition);
	PlanJoinSubqueries(join, join.condition, JoinSide::LEFT);
	for (idx_t child_index = 0; child_index < join.children.size(); child_index++) {
		ColumnBindingRewrite::RewriteChild(
		    op_ptr, child_index, [&](unique_ptr<LogicalOperator> &child, BindingReplacementGraph &child_replacements) {
			    child_replacements = PlanOperator(child);
			    operator_replacements.Merge(child_replacements);
		    });
	}
	ColumnBindingRewrite::ValidateOutput(old_output, op_ptr->GetColumnBindings(), operator_replacements);
	return operator_replacements;
}

void RecursiveDependentJoinPlanner::Plan(Binder &binder, unique_ptr<LogicalOperator> &op) {
	RecursiveDependentJoinPlanner planner(binder);
	planner.PlanOperator(op);
}

unique_ptr<Expression> RecursiveDependentJoinPlanner::VisitReplace(BoundSubqueryExpression &expr,
                                                                   unique_ptr<Expression> *expr_ptr) {
	return binder.PlanSubquery(expr, root);
}

static unique_ptr<Expression> BindTryRethrowFunction(Binder &binder, const char *name,
                                                     vector<unique_ptr<Expression>> children) {
	auto &system_catalog = Catalog::GetSystemCatalog(binder.context);
	auto &entry = system_catalog.GetEntry<ScalarFunctionCatalogEntry>(
	    binder.context, QualifiedName(system_catalog.GetName(), DEFAULT_SCHEMA, name));
	vector<pair<Identifier, unique_ptr<Expression>>> arguments;
	for (auto &child : children) {
		arguments.emplace_back(Identifier(), std::move(child));
	}
	ErrorData error;
	FunctionBinder function_binder(binder);
	auto result = function_binder.BindScalarFunction(entry, std::move(arguments), error, false, &binder);
	if (!result) {
		error.Throw();
	}
	return result;
}

//! Replace a TRY subquery that was lowered with an exposed error flag by
//! __internal_try_rethrow(<error flag>, <masked value>), so that errors captured inside the subquery
//! are re-raised at the use site, where the enclosing TRY, HAS_ERROR or subquery lowering consumes them.
//! Only a re-raise in a conditionally evaluated position of an aggregate select item escapes to the
//! user; the original message is not available here, since HAS_ERROR captures only the fact that an
//! error occurred, so the fixed message states that.
static unique_ptr<Expression> CreateTryErrorRethrow(Binder &binder, unique_ptr<Expression> subquery_result) {
	auto flag_input = subquery_result->Copy();

	vector<unique_ptr<Expression>> value_children;
	value_children.push_back(std::move(subquery_result));
	value_children.push_back(make_uniq<BoundConstantExpression>(Value("value")));
	auto value = BindTryRethrowFunction(binder, "struct_extract", std::move(value_children));

	vector<unique_ptr<Expression>> flag_children;
	flag_children.push_back(std::move(flag_input));
	flag_children.push_back(make_uniq<BoundConstantExpression>(Value("error")));
	auto flag = BindTryRethrowFunction(binder, "struct_extract", std::move(flag_children));

	vector<unique_ptr<Expression>> rethrow_children;
	rethrow_children.push_back(std::move(flag));
	rethrow_children.push_back(std::move(value));
	return BindTryRethrowFunction(binder, TryRethrowFun::Name, std::move(rethrow_children));
}

unique_ptr<Expression> Binder::PlanSubquery(BoundSubqueryExpression &expr, unique_ptr<LogicalOperator> &root) {
	D_ASSERT(root);
	// first we translate the QueryNode of the subquery into a logical plan
	auto subquery_root = std::move(expr.SubqueryMutable().plan);
	D_ASSERT(subquery_root);

	if (expr.RethrowsTryErrors()) {
		// the plan outputs a struct of the masked value and the error flag; the flattened result column
		// carries that struct until the rethrow wrapper unpacks it below
		expr.SetReturnType(expr.Subquery().types[0]);
	}

	// now we actually flatten the subquery
	auto plan = std::move(subquery_root);

	unique_ptr<Expression> result_expression;
	if (!expr.IsCorrelated()) {
		result_expression = PlanUncorrelatedSubquery(*this, expr, root, std::move(plan));
	} else {
		result_expression = PlanCorrelatedSubquery(*this, expr, root, std::move(plan));
	}
	if (expr.RethrowsTryErrors()) {
		result_expression = CreateTryErrorRethrow(*this, std::move(result_expression));
	}
	IncreaseDepth();
	return result_expression;
}

void Binder::PlanSubqueries(unique_ptr<Expression> &expr_ptr, unique_ptr<LogicalOperator> &root) {
	if (!expr_ptr) {
		return;
	}
	auto &expr = *expr_ptr;
	// first visit the children of the node, if any
	ExpressionIterator::EnumerateChildren(expr, [&](unique_ptr<Expression> &expr) { PlanSubqueries(expr, root); });

	// check if this is a subquery node
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_SUBQUERY) {
		auto &subquery = expr.Cast<BoundSubqueryExpression>();
		// subquery node! plan it
		expr_ptr = PlanSubquery(subquery, root);
	}
}

unique_ptr<LogicalOperator> Binder::PlanLateralJoin(unique_ptr<LogicalOperator> left, unique_ptr<LogicalOperator> right,
                                                    CorrelatedColumns &correlated, JoinType join_type,
                                                    unique_ptr<Expression> condition) {
	// scan the right operator for correlated columns
	// correlated LATERAL JOIN
	if (condition) {
		if (condition->HasSubquery()) {
			throw BinderException(*condition, "Subqueries are not supported in LATERAL join conditions");
		}
	}

	auto perform_delim = PerformDuplicateElimination(*this, correlated, right);
	auto delim_join = CreateDuplicateEliminatedJoin(correlated, join_type, std::move(left), perform_delim);

	// Store all information required to perform UNNESTING later.
	delim_join->perform_delim = perform_delim;
	delim_join->any_join = false;
	delim_join->propagate_null_values = join_type != JoinType::INNER;
	delim_join->condition = std::move(condition);
	delim_join->AddChild(std::move(right));
	return std::move(delim_join);
}

} // namespace duckdb
