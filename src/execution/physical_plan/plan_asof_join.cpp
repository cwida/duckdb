#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/execution/operator/aggregate/physical_hash_aggregate.hpp"
#include "duckdb/execution/operator/aggregate/physical_streaming_window.hpp"
#include "duckdb/execution/operator/aggregate/physical_window.hpp"
#include "duckdb/execution/operator/join/physical_asof_join.hpp"
#include "duckdb/execution/operator/join/physical_iejoin.hpp"
#include "duckdb/execution/operator/join/physical_nested_loop_join.hpp"
#include "duckdb/execution/operator/projection/physical_projection.hpp"
#include "duckdb/function/aggregate/distributive_function_utils.hpp"
#include "duckdb/function/aggregate/distributive_functions.hpp"
#include "duckdb/function/builtin_function_lookup.hpp"
#include "duckdb/function/window/rows_functions.hpp"
#include "duckdb/function/window/value_functions.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"

namespace duckdb {

//! Create a combined predicate expression from non-comparison conditions
static unique_ptr<Expression> CreatePredicateFromConditions(const vector<JoinCondition> &conditions) {
	unique_ptr<Expression> predicate;
	for (const auto &cond : conditions) {
		if (!cond.IsComparison()) {
			auto expr = cond.GetJoinExpression().Copy();
			if (!predicate) {
				predicate = std::move(expr);
			} else {
				predicate = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(predicate),
				                                                  std::move(expr));
			}
		}
	}
	return predicate;
}

//! PhysicalHashAggregate requires that the arguments to aggregate functions are bound references.
//! Binding an aggregate can add casts or collations to the arguments, which then no longer are.
static bool HasOnlyReferenceChildren(const Expression &aggregate) {
	for (auto &child : aggregate.Cast<BoundAggregateExpression>().GetChildren()) {
		if (child->GetExpressionClass() != ExpressionClass::BOUND_REF) {
			return false;
		}
	}
	return true;
}

optional_ptr<PhysicalOperator> PhysicalPlanGenerator::PlanAsOfLoopJoin(LogicalComparisonJoin &op,
                                                                       PhysicalOperator &probe, PhysicalOperator &build,
                                                                       const idx_t join_cardinality) {
	// Plan a inverse nested loop join, then aggregate the values to choose the optimal match for each probe row.
	// Use a row number primary key to handle duplicate probe values.
	// aggregate the fields to produce at most one match per probe row,
	// then project the columns back into the correct order and drop the primary key.
	//
	//		 ∏ * \ pk
	//		 |
	//		 Γ pk;first(P),arg_xxx_null(B,inequality)
	//		 |
	//		 ∏ *,inequality
	//		 |
	//       ⨝ swapped
	//     /   \ 
	//    B     W pk:row_number
	//          |
	//          P

	LogicalComparisonJoin join_op(InverseJoinType(op.join_type));

	// Use the types of the planned children: planning a child can change the types of the logical operator
	// (EXCEPT ALL/INTERSECT ALL append a ROW_NUMBER column to it)
	const auto &probe_types = probe.GetTypes();
	const auto &build_types = build.GetTypes();

	join_op.types = build_types;
	join_op.types.insert(join_op.types.end(), probe_types.begin(), probe_types.end());

	// Project pk
	LogicalType pk_type = LogicalType::BIGINT;
	join_op.types.emplace_back(pk_type);

	//	Fill in the projection maps to simplify the code below
	//	Since NLJ doesn't support projection, but ASOF does,
	//	we have to track this carefully...
	join_op.left_projection_map = op.right_projection_map;
	if (join_op.left_projection_map.empty()) {
		for (idx_t i = 0; i < build_types.size(); ++i) {
			join_op.left_projection_map.emplace_back(i);
		}
	}

	join_op.right_projection_map = op.left_projection_map;
	if (join_op.right_projection_map.empty()) {
		for (idx_t i = 0; i < probe_types.size(); ++i) {
			join_op.right_projection_map.emplace_back(i);
		}
	}

	// Remap predicate column references.
	auto predicate = CreatePredicateFromConditions(op.conditions);
	if (predicate) {
		const auto lhs_width = probe_types.size();
		const auto rhs_width = build_types.size();

		ExpressionIterator::EnumerateExpression(predicate, [&](Expression &child) {
			if (child.GetExpressionClass() == ExpressionClass::BOUND_REF) {
				auto &ref = child.Cast<BoundReferenceExpression>();
				if (ref.Index() < lhs_width) {
					ref.IndexMutable() = ref.Index() + rhs_width;
				} else {
					ref.IndexMutable() = ref.Index() - lhs_width;
				}
			}
		});

		join_op.conditions.emplace_back(std::move(predicate));
	}

	auto binder = Binder::CreateBinder(context);
	FunctionBinder function_binder(*binder);
	auto asof_idx = op.conditions.size();
	string arg_min_max;
	for (idx_t i = 0; i < op.conditions.size(); ++i) {
		const auto &cond = op.conditions[i];
		if (!cond.IsComparison()) {
			continue;
		}
		auto left_expr = cond.GetRHS().Copy();
		auto right_expr = cond.GetLHS().Copy();
		if (!left_expr || !right_expr) {
			return nullptr;
		}
		auto comparison = FlipComparisonExpression(cond.GetComparisonType());
		JoinCondition nested_cond(std::move(left_expr), std::move(right_expr), comparison);
		join_op.conditions.emplace_back(std::move(nested_cond));
		switch (cond.GetComparisonType()) {
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		case ExpressionType::COMPARE_GREATERTHAN:
			D_ASSERT(asof_idx == op.conditions.size());
			asof_idx = i;
			arg_min_max = "arg_max_null";
			break;
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		case ExpressionType::COMPARE_LESSTHAN:
			D_ASSERT(asof_idx == op.conditions.size());
			asof_idx = i;
			arg_min_max = "arg_min_null";
			break;
		case ExpressionType::COMPARE_EQUAL:
		case ExpressionType::COMPARE_NOTEQUAL:
		case ExpressionType::COMPARE_DISTINCT_FROM:
		case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
			break;
		default:
			//	Unsupported NLJ comparison
			return nullptr;
		}
	}

	//	NLJ does not support some join types
	switch (join_op.join_type) {
	case JoinType::SEMI:
	case JoinType::ANTI:
	case JoinType::MARK:
	case JoinType::INNER:
	case JoinType::RIGHT:
		// Unfortunately, this does not check all the join types...
		if (!PhysicalNestedLoopJoin::IsSupported(op.conditions, op.join_type)) {
			return nullptr;
		}
		break;
	case JoinType::OUTER:
	case JoinType::LEFT:
		//	RIGHT ASOF JOINs produce the entire build table and would require grouping on all build rows,
		//	which defeats the purpose of this optimisation.
	default:
		return nullptr;
	}

	EntryLookupInfo function_lookup(CatalogType::SCALAR_FUNCTION_ENTRY, QualifiedName(Identifier(arg_min_max)));
	auto arg_min_max_func = binder->GetCatalogEntry(Identifier::SystemCatalog(), Identifier::DefaultSchema(),
	                                                function_lookup, OnEntryNotFound::RETURN_NULL);
	//	Can't find the arg_min/max aggregate we need, so give up before we break anything.
	if (!arg_min_max_func || arg_min_max_func->type != CatalogType::AGGREGATE_FUNCTION_ENTRY) {
		return nullptr;
	}
	auto &arg_min_max_entry = arg_min_max_func->Cast<AggregateFunctionCatalogEntry>();

	// PhysicalHashAggregate requires that the arguments to aggregate functions be bound references,
	// so we Project the (shared) ordering argument on the end of the join results.
	vector<unique_ptr<Expression>> comp_list;
	for (const auto &col_type : join_op.types) {
		const auto col_idx = comp_list.size();
		comp_list.emplace_back(make_uniq<BoundReferenceExpression>(col_type, col_idx));
	}
	vector<LogicalType> comp_types = join_op.types;
	auto comp_expr = op.conditions[asof_idx].GetRHS().Copy();
	comp_types.emplace_back(comp_expr->GetReturnType());
	comp_list.emplace_back(std::move(comp_expr));

	//	Bind the aggregates first so we can abort safely if we can't find one.
	vector<LogicalType> aggr_types(1, pk_type);

	// Wrap all the projected non-pk probe fields in `first` aggregates;
	vector<unique_ptr<Expression>> aggregates;
	for (const auto &right_proj : join_op.right_projection_map) {
		const auto col_idx = build_types.size() + right_proj;
		const auto col_type = join_op.types[col_idx];
		aggr_types.emplace_back(col_type);

		vector<unique_ptr<Expression>> aggr_children;
		auto col_ref = make_uniq<BoundReferenceExpression>(col_type, col_idx);
		aggr_children.push_back(std::move(col_ref));

		auto aggr_expr =
		    GetBuiltinAggregateFunction(context, FirstFun::Name, {col_type})->Bind(context, std::move(aggr_children));

		D_ASSERT(col_type == aggr_expr->GetReturnType());
		if (!HasOnlyReferenceChildren(*aggr_expr)) {
			return nullptr;
		}
		aggregates.emplace_back(std::move(aggr_expr));
	}

	// Wrap all the projected build fields in `arg_max/min` aggregates using the inequality ordering;
	// We are doing all this first in case we can't find a matching function.
	for (const auto &left_proj : join_op.left_projection_map) {
		auto col_idx = left_proj;
		const auto col_type = join_op.types[col_idx];
		aggr_types.emplace_back(col_type);

		vector<unique_ptr<Expression>> aggr_children;
		auto col_ref = make_uniq<BoundReferenceExpression>(col_type, col_idx);
		aggr_children.push_back(std::move(col_ref));
		auto comp_expr = make_uniq<BoundReferenceExpression>(comp_types.back(), comp_types.size() - 1);
		aggr_children.push_back(std::move(comp_expr));

		ErrorData error;
		auto aggr_expr = function_binder.BindAggregateFunction(arg_min_max_entry, std::move(aggr_children), error);
		if (!aggr_expr) {
			return nullptr;
		}
		D_ASSERT(col_type == aggr_expr->GetReturnType());
		if (!HasOnlyReferenceChildren(*aggr_expr)) {
			return nullptr;
		}
		aggregates.emplace_back(std::move(aggr_expr));
	}

	// Add a synthetic primary integer key to the probe relation using streaming windowing.
	vector<unique_ptr<Expression>> window_select;

	auto pk = GetBuiltinWindowFunction(context, RowNumberFun::Name, {})->Bind(context);
	D_ASSERT(pk->GetReturnType() == pk_type);

	pk->WindowStartMutable() = WindowBoundary::UNBOUNDED_PRECEDING;
	pk->WindowEndMutable() = WindowBoundary::CURRENT_ROW_ROWS;
	pk->SetAlias("row_number");
	window_select.emplace_back(std::move(pk));

	auto window_types = probe.GetTypes();
	window_types.emplace_back(pk_type);

	auto &window = Make<PhysicalStreamingWindow>(window_types, std::move(window_select), join_cardinality);
	window.children.emplace_back(probe);

	auto &join = Make<PhysicalNestedLoopJoin>(join_op, build, window, std::move(join_op.conditions), join_op.join_type,
	                                          join_cardinality);

	// Plan a projection of the compare column
	auto &comp_proj = Make<PhysicalProjection>(std::move(comp_types), std::move(comp_list), join_cardinality);
	comp_proj.children.emplace_back(join);

	// Plan an aggregation on the output of the join, grouping by key;
	// TODO: Can we make it perfect?
	// Note that the NLJ produced all fields, but only the projected ones were aggregated
	vector<unique_ptr<Expression>> groups;
	auto pk_ref = make_uniq<BoundReferenceExpression>(pk_type, join_op.types.size() - 1);
	groups.emplace_back(std::move(pk_ref));

	auto &aggr =
	    Make<PhysicalHashAggregate>(context, aggr_types, std::move(aggregates), std::move(groups), join_cardinality);
	aggr.children.emplace_back(comp_proj);

	// Project away primary/grouping key
	// The aggregates were generated in the output order of the original ASOF,
	// so we just have to shift away the pk
	vector<unique_ptr<Expression>> project_list;
	for (column_t i = 1; i < aggr.GetTypes().size(); ++i) {
		auto col_ref = make_uniq<BoundReferenceExpression>(aggr.GetTypes()[i], i);
		project_list.emplace_back(std::move(col_ref));
	}

	auto &proj = Make<PhysicalProjection>(op.types, std::move(project_list), join_cardinality);
	proj.children.emplace_back(aggr);
	return proj;
}

static idx_t ValidateAsOfConditions(LogicalComparisonJoin &op, vector<idx_t> &equi_indexes) {
	equi_indexes.clear();
	auto asof_idx = op.conditions.size();
	for (size_t c = 0; c < op.conditions.size(); ++c) {
		auto &cond = op.conditions[c];
		if (!cond.IsComparison()) {
			continue;
		}
		switch (cond.GetComparisonType()) {
		case ExpressionType::COMPARE_EQUAL:
		case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
			equi_indexes.emplace_back(c);
			break;
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		case ExpressionType::COMPARE_GREATERTHAN:
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		case ExpressionType::COMPARE_LESSTHAN:
			D_ASSERT(asof_idx == op.conditions.size());
			asof_idx = c;
			break;
		default:
			throw InternalException("Invalid ASOF JOIN comparison");
		}
	}
	D_ASSERT(asof_idx < op.conditions.size());

	return asof_idx;
}

PhysicalOperator &PhysicalPlanGenerator::PlanAsOfJoin(LogicalComparisonJoin &op) {
	// If we have a predicate and its a "simple" join, then we can just plan a regular join
	switch (op.join_type) {
	case JoinType::SEMI:
	case JoinType::ANTI:
	case JoinType::MARK:
		for (const auto &cond : op.conditions) {
			if (!cond.IsComparison()) {
				return PhysicalPlanGenerator::PlanComparisonJoin(op);
			}
		}
		break;
	default:
		break;
	}

	// now visit the children
	D_ASSERT(op.children.size() == 2);
	idx_t lhs_cardinality = op.children[0]->EstimateCardinality(context);
	idx_t rhs_cardinality = op.children[1]->EstimateCardinality(context);
	// Estimate the join cardinality before planning the children - planning consumes the bind data of their scans
	const idx_t join_cardinality = op.EstimateCardinality(context);
	auto &left = CreatePlan(*op.children[0]);
	auto &right = CreatePlan(*op.children[1]);

	//	Validate
	vector<idx_t> equi_indexes;
	(void)ValidateAsOfConditions(op, equi_indexes);

	// If there is a non-comparison predicate, we have to use NLJ.
	const bool has_predicate = op.HasArbitraryConditions();
	const bool force_asof_join = Settings::Get<DebugAsofIejoinSetting>(context);
	if (!force_asof_join || has_predicate) {
		const idx_t asof_join_threshold = Settings::Get<AsofLoopJoinThresholdSetting>(context);
		if (has_predicate || (op.children[0]->has_estimated_cardinality && lhs_cardinality < asof_join_threshold)) {
			auto result = PlanAsOfLoopJoin(op, left, right, join_cardinality);
			if (result) {
				return *result;
			}
			if (has_predicate) {
				//	PhysicalAsOfJoin only evaluates the comparisons
				throw NotImplementedException("Unsupported ASOF JOIN: the join predicate cannot be evaluated together "
				                              "with these join conditions");
			}
		}

		//	Check to see if we can leverage partitioning

		//	TODO: HasSingleValuePartitions takes expressions instead of pointers.
		//	Which is convenient for the other clients, but not for us...
		vector<unique_ptr<Expression>> lhs_equalities;
		vector<unique_ptr<Expression>> rhs_equalities;
		for (auto equi_idx : equi_indexes) {
			lhs_equalities.emplace_back(op.conditions[equi_idx].GetLHS().Copy());
			rhs_equalities.emplace_back(op.conditions[equi_idx].GetRHS().Copy());
		}
		vector<column_t> lhs_partition_cols;
		vector<column_t> rhs_partition_cols;
		if (!HasSingleValuePartitions(context, lhs_equalities, left, lhs_partition_cols) ||
		    !HasSingleValuePartitions(context, rhs_equalities, right, rhs_partition_cols)) {
			lhs_partition_cols.clear();
			rhs_partition_cols.clear();
		}

		return Make<PhysicalAsOfJoin>(op, left, right, lhs_partition_cols, rhs_partition_cols);
	}

	return *PlanAsOfInequalityJoin(op, left, right, lhs_cardinality, rhs_cardinality);
}

optional_ptr<PhysicalOperator> PhysicalPlanGenerator::PlanAsOfInequalityJoin(LogicalComparisonJoin &op,
                                                                             PhysicalOperator &left,
                                                                             PhysicalOperator &right,
                                                                             const idx_t lhs_cardinality,
                                                                             const idx_t rhs_cardinality) {
	//	Caller already did this but it is cheap...
	vector<idx_t> equi_indexes;
	const auto asof_idx = ValidateAsOfConditions(op, equi_indexes);

	//	Strip extra column from rhs projections
	auto &right_projection_map = op.right_projection_map;
	if (right_projection_map.empty()) {
		const auto right_count = right.GetTypes().size();
		right_projection_map.reserve(right_count);
		for (column_t i = 0; i < right_count; ++i) {
			right_projection_map.emplace_back(i);
		}
	}

	//	Debug implementation: IEJoin of Window
	//	LEAD(asof_column, 1, infinity) OVER (PARTITION BY equi_column... ORDER BY asof_column) AS asof_end
	auto &asof_comp = op.conditions[asof_idx];
	auto &asof_column = asof_comp.RightReference();
	auto asof_type = asof_column->GetReturnType();

	vector<unique_ptr<Expression>> children;
	vector<unique_ptr<Expression>> partitions;
	vector<BoundOrderByNode> orders;

	children.emplace_back(asof_column->Copy());

	// TODO: If infinities are not supported for a type, fake them by looking at LHS statistics?
	children.emplace_back(make_uniq<BoundConstantExpression>(Value::BIGINT(1)));
	for (auto equi_idx : equi_indexes) {
		partitions.emplace_back(op.conditions[equi_idx].GetRHS().Copy());
	}
	switch (asof_comp.GetComparisonType()) {
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
	case ExpressionType::COMPARE_GREATERTHAN:
		orders.emplace_back(OrderType::ASCENDING, OrderByNullType::NULLS_FIRST, asof_column->Copy());
		children.emplace_back(make_uniq<BoundConstantExpression>(Value::Infinity(asof_type)));
		break;
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
	case ExpressionType::COMPARE_LESSTHAN:
		orders.emplace_back(OrderType::DESCENDING, OrderByNullType::NULLS_FIRST, asof_column->Copy());
		children.emplace_back(make_uniq<BoundConstantExpression>(Value::NegativeInfinity(asof_type)));
		break;
	default:
		throw InternalException("Invalid ASOF JOIN ordering for WINDOW");
	}

	auto asof_end = LeadFun::GetTypedFunction(asof_type, 3).Bind(context, std::move(children));

	D_ASSERT(asof_end->GetReturnType() == asof_type);

	asof_end->PartitionsMutable() = std::move(partitions);
	asof_end->OrderByMutable() = std::move(orders);
	asof_end->WindowStartMutable() = WindowBoundary::UNBOUNDED_PRECEDING;
	asof_end->WindowEndMutable() = WindowBoundary::CURRENT_ROW_ROWS;

	vector<unique_ptr<Expression>> window_select;
	window_select.emplace_back(std::move(asof_end));

	// Copy the types of the planned child instead of mutating the logical operator's types.
	auto window_types = right.GetTypes();
	window_types.emplace_back(asof_type);

	auto &window = Make<PhysicalWindow>(window_types, std::move(window_select), rhs_cardinality);
	window.children.emplace_back(right);

	// IEJoin(left, window, conditions || asof_comp ~op asof_end)
	auto left_expr = asof_comp.GetLHS().Copy();
	auto right_expr = make_uniq<BoundReferenceExpression>(asof_type, window_types.size() - 1);
	ExpressionType comparison;
	switch (asof_comp.GetComparisonType()) {
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		comparison = ExpressionType::COMPARE_LESSTHAN;
		break;
	case ExpressionType::COMPARE_GREATERTHAN:
		comparison = ExpressionType::COMPARE_LESSTHANOREQUALTO;
		break;
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		comparison = ExpressionType::COMPARE_GREATERTHAN;
		break;
	case ExpressionType::COMPARE_LESSTHAN:
		comparison = ExpressionType::COMPARE_GREATERTHANOREQUALTO;
		break;
	default:
		throw InternalException("Invalid ASOF JOIN comparison for IEJoin");
	}
	JoinCondition asof_upper(std::move(left_expr), std::move(right_expr), comparison);

	op.conditions.emplace_back(std::move(asof_upper));
	return Make<PhysicalIEJoin>(op, left, window, std::move(op.conditions), op.join_type, lhs_cardinality);
}

} // namespace duckdb
