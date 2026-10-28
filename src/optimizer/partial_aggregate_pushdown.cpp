#include "duckdb/optimizer/partial_aggregate_pushdown.hpp"

#include "duckdb/common/extension_type_info.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/aggregate/distributive_functions.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/function/scalar/generic_common.hpp"
#include "duckdb/function/builtin_function_lookup.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/optimizer/relation_statistics/relation_statistics_extractor.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

namespace duckdb {

PartialAggregatePushdown::PartialAggregatePushdown(Optimizer &optimizer_p) : optimizer(optimizer_p) {
}

//===--------------------------------------------------------------------===//
// Shared Pushdown Helpers
//===--------------------------------------------------------------------===//

struct PartialAggregatePushdownHeuristics {
	static constexpr idx_t MIN_AGGREGATE_TO_DIMENSION_RATIO = 4;
	static constexpr idx_t MAX_EXTRA_LOWER_GROUPS = 1;
	// Avoid eager aggregation when the join discards much of the aggregate side.
	static constexpr double MIN_JOIN_RETENTION = 0.7;
};

struct PartialAggregatePushdownInfo {
	idx_t aggregate_side;
	idx_t dimension_side;
	TableIndex lower_group_index;
	TableIndex lower_aggregate_index;
	TableIndex upper_group_index;
	TableIndex upper_aggregate_index;
	idx_t join_key_count;
	unordered_set<TableIndex> side_bindings[2];
	vector<unique_ptr<Expression>> lower_groups;
	column_binding_map_t<ColumnBinding> lower_group_map;
	column_binding_map_t<LogicalType> lower_group_types;
};

static bool IsSubset(const unordered_set<TableIndex> &bindings, const unordered_set<TableIndex> &side_bindings) {
	for (auto &binding : bindings) {
		if (side_bindings.find(binding) == side_bindings.end()) {
			return false;
		}
	}
	return true;
}

static unordered_set<TableIndex> GetExpressionBindings(const Expression &expr) {
	unordered_set<TableIndex> bindings;
	LogicalJoin::GetExpressionBindings(expr, bindings);
	return bindings;
}

static bool GetColumnBinding(const Expression &expr, ColumnBinding &binding) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return false;
	}
	binding = expr.Cast<BoundColumnRefExpression>().Binding();
	return true;
}

static bool IsAggregateStateFromFunction(const LogicalType &type, const string &function_name) {
	if (!type.IsAggregateState()) {
		return false;
	}
	auto ext_info = type.GetExtensionInfo();
	if (!ext_info) {
		return false;
	}
	auto entry = ext_info->properties.find("function_name");
	return entry != ext_info->properties.end() && entry->second.type().id() == LogicalTypeId::VARCHAR &&
	       !entry->second.IsNull() && StringValue::Get(entry->second) == function_name;
}

static void CopyCardinality(LogicalOperator &dst, const LogicalOperator &src) {
	dst.estimated_cardinality = src.estimated_cardinality;
	dst.has_estimated_cardinality = src.has_estimated_cardinality;
}

static bool IsSupportedAggregate(const BoundAggregateExpression &expr) {
	if (expr.IsDistinct() || expr.GetFilter() || expr.GetOrderBys()) {
		return false;
	}
	if (expr.Function().GetName() == "decimal_average") {
		return false;
	}
	for (auto &child : expr.GetChildren()) {
		if (IsAggregateStateFromFunction(child->GetReturnType(), "decimal_average")) {
			return false;
		}
	}
	if (expr.StateExportMode() != AggregateStateExportMode::NONE) {
		return false;
	}
	if (expr.GetChildren().size() > 1) {
		return false;
	}
	if (!expr.Function().HasGetStateTypeCallback()) {
		return false;
	}
	return true;
}

static bool ContainsAggregateInput(const LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		return true;
	}
	for (auto &child : op.children) {
		if (ContainsAggregateInput(*child)) {
			return true;
		}
	}
	return false;
}

static bool JoinContainsDelimGet(const LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_DELIM_GET) {
		return true;
	}
	for (auto &child : op.children) {
		if (JoinContainsDelimGet(*child)) {
			return true;
		}
	}
	return false;
}

static bool IsPushdownJoinType(JoinType join_type) {
	return join_type == JoinType::INNER || join_type == JoinType::LEFT || join_type == JoinType::RIGHT;
}

static bool GetPushdownOperators(LogicalOperator &op, LogicalAggregate *&aggr, LogicalComparisonJoin *&join) {
	if (op.type != LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY || op.children.size() != 1) {
		return false;
	}
	aggr = &op.Cast<LogicalAggregate>();
	if (!aggr->grouping_functions.empty() || aggr->groups.empty() || aggr->expressions.empty()) {
		return false;
	}
	auto &child = *op.children[0];
	if (child.type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		return false;
	}
	join = &child.Cast<LogicalComparisonJoin>();
	if (JoinContainsDelimGet(*join)) {
		return false;
	}
	return IsPushdownJoinType(join->join_type) && !join->HasProjectionMap() && join->children.size() == 2 &&
	       !join->conditions.empty();
}

static bool GetExpressionSide(const Expression &expr, const unordered_set<TableIndex> (&side_bindings)[2],
                              idx_t &side) {
	auto bindings = GetExpressionBindings(expr);
	if (bindings.empty()) {
		return false;
	}
	if (IsSubset(bindings, side_bindings[0])) {
		side = 0;
		return true;
	}
	if (IsSubset(bindings, side_bindings[1])) {
		side = 1;
		return true;
	}
	return false;
}

// Lower aggregate groups are join keys followed by same-side grouping columns.
static idx_t CollectLowerSideGroups(TableIndex group_index, const vector<ColumnBinding> &join_keys,
                                    const vector<LogicalType> &join_key_types,
                                    const vector<unique_ptr<Expression>> &aggr_groups, idx_t side,
                                    const unordered_set<TableIndex> (&side_bindings)[2],
                                    vector<unique_ptr<Expression>> &out_groups,
                                    column_binding_map_t<ColumnBinding> &out_map,
                                    column_binding_map_t<LogicalType> &out_types) {
	auto add = [&](ColumnBinding binding, const LogicalType &type) {
		if (out_map.find(binding) != out_map.end()) {
			return;
		}
		out_map[binding] = ColumnBinding(group_index, ProjectionIndex(out_groups.size()));
		out_types[binding] = type;
		out_groups.push_back(make_uniq<BoundColumnRefExpression>(type, binding));
	};
	for (idx_t k = 0; k < join_keys.size(); k++) {
		add(join_keys[k], join_key_types[k]);
	}
	const idx_t join_key_count = out_groups.size();
	for (auto &group : aggr_groups) {
		auto &ref = group->Cast<BoundColumnRefExpression>();
		idx_t s;
		if (GetExpressionSide(*group, side_bindings, s) && s == side) {
			add(ref.Binding(), ref.GetReturnType());
		}
	}
	return join_key_count;
}

static bool FindAggregateSide(const LogicalAggregate &aggr, PartialAggregatePushdownInfo &info) {
	optional_idx aggregate_side;
	for (auto &expr : aggr.expressions) {
		if (expr->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
			return false;
		}
		auto &aggregate = expr->Cast<BoundAggregateExpression>();
		if (!IsSupportedAggregate(aggregate)) {
			return false;
		}
		if (aggregate.GetChildren().empty()) {
			continue;
		}
		idx_t side;
		if (!GetExpressionSide(*aggregate.GetChildren()[0], info.side_bindings, side)) {
			return false;
		}
		if (!aggregate_side.IsValid()) {
			aggregate_side = side;
		} else if (aggregate_side.GetIndex() != side) {
			return false;
		}
	}
	if (!aggregate_side.IsValid()) {
		return false;
	}
	info.aggregate_side = aggregate_side.GetIndex();
	info.dimension_side = 1 - info.aggregate_side;
	return true;
}

static bool PassesCardinalityHeuristic(const LogicalComparisonJoin &join, const PartialAggregatePushdownInfo &info) {
	auto &aggregate_child = *join.children[info.aggregate_side];
	auto &dimension_child = *join.children[info.dimension_side];
	if (!aggregate_child.has_estimated_cardinality || !join.has_estimated_cardinality) {
		return true;
	}
	const auto agg_card = aggregate_child.estimated_cardinality;
	const auto join_card = join.estimated_cardinality;
	if (join_card > agg_card) {
		return true;
	}
	if (static_cast<double>(join_card) <
	    PartialAggregatePushdownHeuristics::MIN_JOIN_RETENTION * static_cast<double>(agg_card)) {
		return false;
	}
	if (!dimension_child.has_estimated_cardinality) {
		return true;
	}
	return static_cast<double>(agg_card) >=
	       static_cast<double>(PartialAggregatePushdownHeuristics::MIN_AGGREGATE_TO_DIMENSION_RATIO) *
	           static_cast<double>(dimension_child.estimated_cardinality);
}

static bool GetJoinSideExpressions(JoinCondition &condition, const PartialAggregatePushdownInfo &info,
                                   unique_ptr<Expression> *&aggregate_expr, unique_ptr<Expression> *&dimension_expr) {
	if (!condition.IsComparison() || condition.GetComparisonType() != ExpressionType::COMPARE_EQUAL) {
		return false;
	}
	auto left_bindings = GetExpressionBindings(condition.GetLHS());
	auto right_bindings = GetExpressionBindings(condition.GetRHS());
	if (left_bindings.empty() || right_bindings.empty()) {
		return false;
	}
	if (IsSubset(left_bindings, info.side_bindings[info.aggregate_side]) &&
	    IsSubset(right_bindings, info.side_bindings[info.dimension_side])) {
		aggregate_expr = &condition.LeftReference();
		dimension_expr = &condition.RightReference();
		return true;
	}
	if (IsSubset(right_bindings, info.side_bindings[info.aggregate_side]) &&
	    IsSubset(left_bindings, info.side_bindings[info.dimension_side])) {
		aggregate_expr = &condition.RightReference();
		dimension_expr = &condition.LeftReference();
		return true;
	}
	return false;
}

static bool ValidateJoinConditions(LogicalComparisonJoin &join, const PartialAggregatePushdownInfo &info) {
	for (auto &condition : join.conditions) {
		unique_ptr<Expression> *aggregate_expr;
		unique_ptr<Expression> *dimension_expr;
		if (!GetJoinSideExpressions(condition, info, aggregate_expr, dimension_expr)) {
			return false;
		}
		ColumnBinding join_key;
		if (!GetColumnBinding(**aggregate_expr, join_key)) {
			return false;
		}
	}
	return true;
}

static bool HasDimensionGroup(const LogicalAggregate &aggr, const PartialAggregatePushdownInfo &info) {
	idx_t dimension_group_count = 0;
	for (auto &group : aggr.groups) {
		ColumnBinding group_binding;
		if (!GetColumnBinding(*group, group_binding)) {
			return false;
		}
		idx_t side;
		if (!GetExpressionSide(*group, info.side_bindings, side)) {
			return false;
		}
		if (side == info.dimension_side) {
			dimension_group_count++;
		}
	}
	return dimension_group_count > 0;
}

static bool JoinPreservesAggregateSide(const LogicalComparisonJoin &join, const PartialAggregatePushdownInfo &info) {
	switch (join.join_type) {
	case JoinType::INNER:
		return true;
	case JoinType::LEFT:
		return info.aggregate_side == 0;
	case JoinType::RIGHT:
		return info.aggregate_side == 1;
	default:
		return false;
	}
}

static bool AnalyzePushdown(LogicalAggregate &aggr, LogicalComparisonJoin &join, PartialAggregatePushdownInfo &info) {
	LogicalJoin::GetTableReferences(*join.children[0], info.side_bindings[0]);
	LogicalJoin::GetTableReferences(*join.children[1], info.side_bindings[1]);
	if (!FindAggregateSide(aggr, info)) {
		return false;
	}
	if (!JoinPreservesAggregateSide(join, info)) {
		return false;
	}
	if (ContainsAggregateInput(*join.children[info.aggregate_side])) {
		return false;
	}
	if (info.side_bindings[info.dimension_side].empty()) {
		return false;
	}
	if (!PassesCardinalityHeuristic(join, info)) {
		return false;
	}
	if (!ValidateJoinConditions(join, info)) {
		return false;
	}
	return HasDimensionGroup(aggr, info);
}

static void BuildLowerGroupMap(LogicalAggregate &aggr, LogicalComparisonJoin &join,
                               PartialAggregatePushdownInfo &info) {
	vector<ColumnBinding> join_keys;
	vector<LogicalType> join_key_types;
	for (auto &condition : join.conditions) {
		unique_ptr<Expression> *aggregate_expr;
		unique_ptr<Expression> *dimension_expr;
		GetJoinSideExpressions(condition, info, aggregate_expr, dimension_expr);
		ColumnBinding binding;
		GetColumnBinding(**aggregate_expr, binding);
		join_keys.push_back(binding);
		join_key_types.push_back((*aggregate_expr)->GetReturnType());
	}
	info.join_key_count =
	    CollectLowerSideGroups(info.lower_group_index, join_keys, join_key_types, aggr.groups, info.aggregate_side,
	                           info.side_bindings, info.lower_groups, info.lower_group_map, info.lower_group_types);
}

static bool PassesLowerGroupHeuristic(const LogicalAggregate &aggr, const PartialAggregatePushdownInfo &info) {
	return info.lower_groups.size() < aggr.groups.size() &&
	       info.lower_groups.size() <= info.join_key_count + PartialAggregatePushdownHeuristics::MAX_EXTRA_LOWER_GROUPS;
}

static bool BindPushdownAggregates(ClientContext &context, LogicalAggregate &aggr, TableIndex lower_aggregate_index,
                                   vector<unique_ptr<Expression>> &lower_aggregates,
                                   vector<unique_ptr<Expression>> &upper_aggregates) {
	FunctionBinder function_binder(context);

	for (idx_t i = 0; i < aggr.expressions.size(); i++) {
		auto aggregate_copy = unique_ptr_cast<Expression, BoundAggregateExpression>(aggr.expressions[i]->Copy());
		auto lower_aggregate = ExportAggregateFunction::Bind(std::move(aggregate_copy));
		auto lower_type = lower_aggregate->GetReturnType();
		if (!lower_type.IsAggregateState()) {
			return false;
		}

		vector<unique_ptr<Expression>> arguments;
		auto lower_binding = ColumnBinding(lower_aggregate_index, ProjectionIndex(i));
		arguments.push_back(make_uniq<BoundColumnRefExpression>(lower_type, lower_binding));
		auto combine_function = GetBuiltinAggregateFunction(context, CombineAggrFun::Name, {lower_type});
		auto upper_aggregate = function_binder.BindAggregateFunction(std::move(combine_function), std::move(arguments));
		if (!upper_aggregate->GetReturnType().IsAggregateState()) {
			return false;
		}
		lower_aggregates.push_back(std::move(lower_aggregate));
		upper_aggregates.push_back(std::move(upper_aggregate));
	}
	return true;
}

static unique_ptr<LogicalAggregate> CreateLowerAggregate(LogicalAggregate &aggr, LogicalComparisonJoin &join,
                                                         PartialAggregatePushdownInfo &info,
                                                         vector<unique_ptr<Expression>> lower_aggregates) {
	auto lower_aggr =
	    make_uniq<LogicalAggregate>(info.lower_group_index, info.lower_aggregate_index, std::move(lower_aggregates));
	lower_aggr->groups = std::move(info.lower_groups);
	lower_aggr->children.push_back(std::move(join.children[info.aggregate_side]));
	lower_aggr->ResolveOperatorTypes();
	CopyCardinality(*lower_aggr, aggr);
	return lower_aggr;
}

static unique_ptr<LogicalComparisonJoin> CreateJoin(LogicalComparisonJoin &join, PartialAggregatePushdownInfo &info,
                                                    unique_ptr<LogicalAggregate> lower_aggr) {
	auto new_join = make_uniq<LogicalComparisonJoin>(join.join_type);
	if (info.aggregate_side == 0) {
		new_join->children.push_back(std::move(lower_aggr));
		new_join->children.push_back(std::move(join.children[info.dimension_side]));
	} else {
		new_join->children.push_back(std::move(join.children[info.dimension_side]));
		new_join->children.push_back(std::move(lower_aggr));
	}

	for (auto &condition : join.conditions) {
		unique_ptr<Expression> *aggregate_expr;
		unique_ptr<Expression> *dimension_expr;
		GetJoinSideExpressions(condition, info, aggregate_expr, dimension_expr);
		ColumnBinding join_key;
		GetColumnBinding(**aggregate_expr, join_key);
		auto lower_binding = info.lower_group_map[join_key];
		auto lower_type = new_join->children[info.aggregate_side]->types[lower_binding.column_index.GetIndex()];
		auto lower_expr = make_uniq<BoundColumnRefExpression>(lower_type, lower_binding);
		if (info.aggregate_side == 0) {
			new_join->conditions.emplace_back(std::move(lower_expr), (*dimension_expr)->Copy(),
			                                  ExpressionType::COMPARE_EQUAL);
		} else {
			new_join->conditions.emplace_back((*dimension_expr)->Copy(), std::move(lower_expr),
			                                  ExpressionType::COMPARE_EQUAL);
		}
	}
	new_join->ResolveOperatorTypes();
	CopyCardinality(*new_join, join);
	return new_join;
}

static vector<unique_ptr<Expression>> CreateUpperGroups(LogicalAggregate &aggr, LogicalComparisonJoin &new_join,
                                                        const PartialAggregatePushdownInfo &info) {
	vector<unique_ptr<Expression>> upper_groups;
	for (auto &group : aggr.groups) {
		auto &group_ref = group->Cast<BoundColumnRefExpression>();
		auto entry = info.lower_group_map.find(group_ref.Binding());
		if (entry == info.lower_group_map.end()) {
			upper_groups.push_back(group->Copy());
			continue;
		}
		auto type = new_join.children[info.aggregate_side]->types[entry->second.column_index.GetIndex()];
		upper_groups.push_back(make_uniq<BoundColumnRefExpression>(type, entry->second));
	}
	return upper_groups;
}

static unique_ptr<LogicalAggregate> CreateUpperAggregate(LogicalAggregate &aggr,
                                                         unique_ptr<LogicalComparisonJoin> new_join,
                                                         const PartialAggregatePushdownInfo &info,
                                                         vector<unique_ptr<Expression>> upper_aggregates) {
	// Keep upper outputs distinct from the original aggregate during re-traversal.
	auto upper_aggr =
	    make_uniq<LogicalAggregate>(info.upper_group_index, info.upper_aggregate_index, std::move(upper_aggregates));
	upper_aggr->groups = CreateUpperGroups(aggr, *new_join, info);
	upper_aggr->grouping_sets = aggr.grouping_sets;
	upper_aggr->grouping_functions = aggr.grouping_functions;
	upper_aggr->children.push_back(std::move(new_join));
	upper_aggr->ResolveOperatorTypes();
	CopyCardinality(*upper_aggr, aggr);
	return upper_aggr;
}

static void AddReplacement(column_binding_map_t<ColumnBinding> &replacement_map, ColumnBinding key,
                           ColumnBinding value) {
#ifdef D_ASSERT_IS_ENABLED
	D_ASSERT(replacement_map.find(key) == replacement_map.end());
	for (auto &entry : replacement_map) {
		D_ASSERT(entry.second != key);  // the new key must not already be a value
		D_ASSERT(entry.first != value); // the new value must not already be a key
	}
#endif
	replacement_map[key] = value;
}

template <class BUILD_VALUE>
static unique_ptr<LogicalProjection>
BuildFinalProjection(Optimizer &optimizer, LogicalAggregate &aggr, unique_ptr<LogicalAggregate> upper_aggr,
                     column_binding_map_t<ColumnBinding> &replacement_map, BUILD_VALUE &&build_value) {
	const auto proj_index = optimizer.binder.GenerateTableIndex();
	const auto group_count = aggr.groups.size();
	vector<unique_ptr<Expression>> expressions;
	expressions.reserve(group_count + aggr.expressions.size());

	for (idx_t i = 0; i < group_count; i++) {
		AddReplacement(replacement_map, ColumnBinding(aggr.group_index, ProjectionIndex(i)),
		               ColumnBinding(proj_index, ProjectionIndex(i)));
		expressions.push_back(make_uniq<BoundColumnRefExpression>(
		    upper_aggr->types[i], ColumnBinding(upper_aggr->group_index, ProjectionIndex(i))));
	}

	for (idx_t j = 0; j < aggr.expressions.size(); j++) {
		auto ref = make_uniq<BoundColumnRefExpression>(upper_aggr->types[group_count + j],
		                                               ColumnBinding(upper_aggr->aggregate_index, ProjectionIndex(j)));
		auto value = build_value(j, std::move(ref));
		if (!value) {
			return nullptr;
		}
		AddReplacement(replacement_map, ColumnBinding(aggr.aggregate_index, ProjectionIndex(j)),
		               ColumnBinding(proj_index, ProjectionIndex(group_count + j)));
		expressions.push_back(std::move(value));
	}

	auto projection = make_uniq<LogicalProjection>(proj_index, std::move(expressions));
	if (upper_aggr->has_estimated_cardinality) {
		projection->SetEstimatedCardinality(upper_aggr->estimated_cardinality);
	}
	projection->children.push_back(std::move(upper_aggr));
	projection->ResolveOperatorTypes();
	return projection;
}

//===--------------------------------------------------------------------===//
// Double-Eager Aggregation
//===--------------------------------------------------------------------===//

struct DoubleEagerAggregate {
	idx_t aggregate_pos = 0;
	idx_t side = 0;          // side that owns the aggregate input; COUNT_STAR uses side 0
	LogicalType return_type; // original aggregate return type (final projection casts back to this)
	idx_t partial_pos = 0;   // position of this aggregate's partial within its side's lower aggregate
};

struct DoubleEagerSide {
	TableIndex group_index;
	TableIndex aggregate_index;
	vector<unique_ptr<Expression>> groups;
	vector<unique_ptr<Expression>> aggregates;     // always starts with count_star() at position 0
	column_binding_map_t<ColumnBinding> group_map; // original group/key binding -> lower group binding
	column_binding_map_t<LogicalType> group_types;
};

static unique_ptr<BoundAggregateExpression> DEBindAggregate(ClientContext &context, const string &name,
                                                            vector<unique_ptr<Expression>> children) {
	vector<LogicalType> types;
	for (auto &child : children) {
		types.push_back(child->GetReturnType());
	}
	auto func = TryGetBuiltinAggregateFunction(context, Identifier(name), types);
	if (!func) {
		return nullptr;
	}
	FunctionBinder function_binder(context);
	return function_binder.BindAggregateFunction(std::move(func), std::move(children));
}

static unique_ptr<BoundAggregateExpression> DEBindCombineAggr(ClientContext &context,
                                                              vector<unique_ptr<Expression>> children) {
	return DEBindAggregate(context, CombineAggrFun::Name, std::move(children));
}

struct DoubleEagerHeuristics {
	static constexpr idx_t MAX_SMALL_GROUP_COUNT = 2 * static_cast<idx_t>(STANDARD_VECTOR_SIZE);
	static constexpr idx_t MIN_SMALL_COLLAPSE = 2;
	static constexpr idx_t MIN_LARGE_COLLAPSE = 8;
};

static bool DEEstimateCollapse(const LogicalComparisonJoin &join, idx_t &effective_ndv) {
	if (!join.has_estimated_cardinality || !join.children[0]->has_estimated_cardinality ||
	    !join.children[1]->has_estimated_cardinality) {
		return false;
	}
	const auto c0 = static_cast<double>(MaxValue<idx_t>(join.children[0]->estimated_cardinality, 1));
	const auto c1 = static_cast<double>(MaxValue<idx_t>(join.children[1]->estimated_cardinality, 1));
	const auto cj = static_cast<double>(MaxValue<idx_t>(join.estimated_cardinality, 1));
	double ndv = MaxValue<double>(1.0, (c0 * c1) / cj);
	ndv = MinValue<double>(ndv, MinValue<double>(c0, c1));
	effective_ndv = MaxValue<idx_t>(static_cast<idx_t>(ndv), 1);
	const auto min_collapse = effective_ndv <= DoubleEagerHeuristics::MAX_SMALL_GROUP_COUNT
	                              ? DoubleEagerHeuristics::MIN_SMALL_COLLAPSE
	                              : DoubleEagerHeuristics::MIN_LARGE_COLLAPSE;
	return c0 >= static_cast<double>(min_collapse) * ndv && c1 >= static_cast<double>(min_collapse) * ndv;
}

static bool DECanRepeatAggregateState(const BoundAggregateExpression &aggr) {
	auto &name = aggr.Function().GetName();
	if (name == "decimal_average") {
		return false;
	}
	for (auto &child : aggr.GetChildren()) {
		if (IsAggregateStateFromFunction(child->GetReturnType(), "decimal_average")) {
			return false;
		}
	}
	if ((name != "sum" && name != "avg") || aggr.GetChildren().empty()) {
		return true;
	}
	auto &type = aggr.GetChildren()[0]->GetReturnType();
	return type.InternalType() != PhysicalType::INT128;
}

static bool DEClassifyAggregates(const LogicalAggregate &aggr, const unordered_set<TableIndex> (&side_bindings)[2],
                                 vector<DoubleEagerAggregate> &aggregates) {
	for (idx_t i = 0; i < aggr.expressions.size(); i++) {
		auto &expr = aggr.expressions[i];
		if (expr->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
			return false;
		}
		auto &bound = expr->Cast<BoundAggregateExpression>();
		if (!IsSupportedAggregate(bound) || !bound.Function().HasStateCombineCallback() ||
		    !bound.Function().HasStateFinalizeCallback() || !DECanRepeatAggregateState(bound)) {
			return false;
		}
		DoubleEagerAggregate de;
		de.aggregate_pos = i;
		de.return_type = bound.GetReturnType();
		if (!bound.GetChildren().empty()) {
			if (!GetExpressionSide(*bound.GetChildren()[0], side_bindings, de.side)) {
				return false;
			}
		}
		aggregates.push_back(std::move(de));
	}
	return true;
}

static bool DECollectJoinKeys(LogicalComparisonJoin &join, const unordered_set<TableIndex> (&side_bindings)[2],
                              vector<ColumnBinding> (&join_keys)[2], vector<LogicalType> (&join_key_types)[2]) {
	for (auto &cond : join.conditions) {
		if (!cond.IsComparison() || cond.GetComparisonType() != ExpressionType::COMPARE_EQUAL) {
			return false;
		}
		idx_t lhs_side, rhs_side;
		if (!GetExpressionSide(cond.GetLHS(), side_bindings, lhs_side) ||
		    !GetExpressionSide(cond.GetRHS(), side_bindings, rhs_side) || lhs_side == rhs_side) {
			return false;
		}
		auto &lhs_expr = lhs_side == 0 ? cond.GetLHS() : cond.GetRHS();
		auto &rhs_expr = lhs_side == 0 ? cond.GetRHS() : cond.GetLHS();
		ColumnBinding lkey, rkey;
		if (!GetColumnBinding(lhs_expr, lkey) || !GetColumnBinding(rhs_expr, rkey)) {
			return false;
		}
		join_keys[0].push_back(lkey);
		join_key_types[0].push_back(lhs_expr.GetReturnType());
		join_keys[1].push_back(rkey);
		join_key_types[1].push_back(rhs_expr.GetReturnType());
	}
	return true;
}

static bool DEValidateGroups(const LogicalAggregate &aggr, const unordered_set<TableIndex> (&side_bindings)[2]) {
	for (auto &group : aggr.groups) {
		ColumnBinding b;
		idx_t s;
		if (!GetColumnBinding(*group, b) || !GetExpressionSide(*group, side_bindings, s)) {
			return false;
		}
	}
	return true;
}

static bool DEBuildLowerSides(Optimizer &optimizer, const LogicalAggregate &aggr,
                              vector<DoubleEagerAggregate> &aggregates,
                              const unordered_set<TableIndex> (&side_bindings)[2],
                              const vector<ColumnBinding> (&join_keys)[2],
                              const vector<LogicalType> (&join_key_types)[2], DoubleEagerSide (&sides)[2]) {
	for (idx_t s = 0; s < 2; s++) {
		sides[s].group_index = optimizer.binder.GenerateTableIndex();
		sides[s].aggregate_index = optimizer.binder.GenerateTableIndex();
		CollectLowerSideGroups(sides[s].group_index, join_keys[s], join_key_types[s], aggr.groups, s, side_bindings,
		                       sides[s].groups, sides[s].group_map, sides[s].group_types);
		auto count_star = DEBindAggregate(optimizer.context, "count_star", {});
		if (!count_star) {
			return false;
		}
		sides[s].aggregates.push_back(std::move(count_star));
	}

	// Per-side exported states for each measured aggregate.
	for (auto &de : aggregates) {
		auto aggregate_copy =
		    unique_ptr_cast<Expression, BoundAggregateExpression>(aggr.expressions[de.aggregate_pos]->Copy());
		auto partial = ExportAggregateFunction::Bind(std::move(aggregate_copy));
		if (!partial->GetReturnType().IsAggregateState()) {
			return false;
		}
		de.partial_pos = sides[de.side].aggregates.size();
		sides[de.side].aggregates.push_back(std::move(partial));
	}
	return true;
}

static void DECreateLowerAggregates(LogicalComparisonJoin &join, DoubleEagerSide (&sides)[2], idx_t effective_ndv,
                                    unique_ptr<LogicalAggregate> (&lower)[2]) {
	for (idx_t s = 0; s < 2; s++) {
		lower[s] =
		    make_uniq<LogicalAggregate>(sides[s].group_index, sides[s].aggregate_index, std::move(sides[s].aggregates));
		lower[s]->groups = std::move(sides[s].groups);
		lower[s]->children.push_back(std::move(join.children[s]));
		lower[s]->ResolveOperatorTypes();
		lower[s]->SetEstimatedCardinality(effective_ndv);
	}
}

static void DEGetCountBindings(const DoubleEagerSide (&sides)[2], const unique_ptr<LogicalAggregate> (&lower)[2],
                               ColumnBinding (&cnt_binding)[2], LogicalType (&cnt_type)[2],
                               idx_t (&lower_group_count)[2]) {
	for (idx_t s = 0; s < 2; s++) {
		lower_group_count[s] = lower[s]->groups.size();
		cnt_binding[s] = ColumnBinding(sides[s].aggregate_index, ProjectionIndex(0));
		cnt_type[s] = lower[s]->types[lower_group_count[s]];
	}
}

static unique_ptr<LogicalComparisonJoin> DECreateJoin(DoubleEagerSide (&sides)[2],
                                                      const vector<ColumnBinding> (&join_keys)[2],
                                                      const vector<LogicalType> (&join_key_types)[2],
                                                      unique_ptr<LogicalAggregate> (&lower)[2], idx_t effective_ndv) {
	auto new_join = make_uniq<LogicalComparisonJoin>(JoinType::INNER);
	auto lower_key_expr = [&](idx_t s, idx_t k) {
		auto binding = sides[s].group_map.at(join_keys[s][k]);
		return make_uniq<BoundColumnRefExpression>(join_key_types[s][k], binding);
	};
	for (idx_t k = 0; k < join_keys[0].size(); k++) {
		new_join->conditions.emplace_back(lower_key_expr(0, k), lower_key_expr(1, k), ExpressionType::COMPARE_EQUAL);
	}
	new_join->children.push_back(std::move(lower[0]));
	new_join->children.push_back(std::move(lower[1]));
	new_join->ResolveOperatorTypes();
	new_join->SetEstimatedCardinality(effective_ndv);
	return new_join;
}

static bool DECreateUpperAggregates(Optimizer &optimizer, const vector<DoubleEagerAggregate> &aggregates,
                                    const DoubleEagerSide (&sides)[2], LogicalComparisonJoin &new_join,
                                    const ColumnBinding (&cnt_binding)[2], const LogicalType (&cnt_type)[2],
                                    const idx_t (&lower_group_count)[2],
                                    vector<unique_ptr<Expression>> &upper_aggregates) {
	auto partial_ref = [&](const DoubleEagerAggregate &de) {
		auto binding = ColumnBinding(sides[de.side].aggregate_index, ProjectionIndex(de.partial_pos));
		auto type = new_join.children[de.side]->types[lower_group_count[de.side] + de.partial_pos];
		return make_uniq<BoundColumnRefExpression>(type, binding);
	};
	auto cnt_ref = [&](idx_t s) -> unique_ptr<Expression> {
		return make_uniq<BoundColumnRefExpression>(cnt_type[s], cnt_binding[s]);
	};
	for (auto &de : aggregates) {
		vector<unique_ptr<Expression>> arg;
		arg.push_back(partial_ref(de));
		arg.push_back(cnt_ref(1 - de.side));
		auto upper = DEBindCombineAggr(optimizer.context, std::move(arg));
		if (!upper) {
			return false;
		}
		if (!upper->GetReturnType().IsAggregateState()) {
			return false;
		}
		upper_aggregates.push_back(std::move(upper));
	}
	return true;
}

static unique_ptr<LogicalAggregate> DECreateUpperAggregate(Optimizer &optimizer, LogicalAggregate &aggr,
                                                           unique_ptr<LogicalComparisonJoin> new_join,
                                                           const unordered_set<TableIndex> (&side_bindings)[2],
                                                           const DoubleEagerSide (&sides)[2],
                                                           vector<unique_ptr<Expression>> upper_aggregates) {
	auto upper_group_index = optimizer.binder.GenerateTableIndex();
	auto upper_aggregate_index = optimizer.binder.GenerateTableIndex();
	auto upper_aggr =
	    make_uniq<LogicalAggregate>(upper_group_index, upper_aggregate_index, std::move(upper_aggregates));
	for (auto &group : aggr.groups) {
		auto &ref = group->Cast<BoundColumnRefExpression>();
		idx_t gs;
#ifdef D_ASSERT_IS_ENABLED
		auto has_side = GetExpressionSide(*group, side_bindings, gs);
		D_ASSERT(has_side);
#else
		GetExpressionSide(*group, side_bindings, gs);
#endif
		auto binding = sides[gs].group_map.at(ref.Binding());
		upper_aggr->groups.push_back(make_uniq<BoundColumnRefExpression>(ref.GetReturnType(), binding));
	}
	upper_aggr->grouping_sets = aggr.grouping_sets;
	upper_aggr->children.push_back(std::move(new_join));
	upper_aggr->ResolveOperatorTypes();
	CopyCardinality(*upper_aggr, aggr);
	return upper_aggr;
}

bool PartialAggregatePushdown::TryDoubleEagerPushdown(unique_ptr<LogicalOperator> &op) {
	LogicalAggregate *aggr_ptr;
	LogicalComparisonJoin *join_ptr;
	if (!GetPushdownOperators(*op, aggr_ptr, join_ptr)) {
		return false;
	}
	auto &aggr = *aggr_ptr;
	auto &join = *join_ptr;
	if (join.join_type != JoinType::INNER) {
		return false;
	}
	if (aggr.grouping_sets.size() > 1) {
		return false;
	}

	idx_t effective_ndv;
	if (!DEEstimateCollapse(join, effective_ndv)) {
		return false;
	}

	unordered_set<TableIndex> side_bindings[2];
	LogicalJoin::GetTableReferences(*join.children[0], side_bindings[0]);
	LogicalJoin::GetTableReferences(*join.children[1], side_bindings[1]);

	vector<DoubleEagerAggregate> aggregates;
	if (!DEClassifyAggregates(aggr, side_bindings, aggregates)) {
		return false;
	}

	vector<ColumnBinding> join_keys[2];
	vector<LogicalType> join_key_types[2];
	if (!DECollectJoinKeys(join, side_bindings, join_keys, join_key_types) || !DEValidateGroups(aggr, side_bindings)) {
		return false;
	}

	DoubleEagerSide sides[2];
	if (!DEBuildLowerSides(optimizer, aggr, aggregates, side_bindings, join_keys, join_key_types, sides)) {
		return false;
	}

	unique_ptr<LogicalAggregate> lower[2];
	DECreateLowerAggregates(join, sides, effective_ndv, lower);

	ColumnBinding cnt_binding[2];
	LogicalType cnt_type[2];
	idx_t lower_group_count[2];
	DEGetCountBindings(sides, lower, cnt_binding, cnt_type, lower_group_count);

	auto new_join = DECreateJoin(sides, join_keys, join_key_types, lower, effective_ndv);

	vector<unique_ptr<Expression>> upper_aggregates;
	if (!DECreateUpperAggregates(optimizer, aggregates, sides, *new_join, cnt_binding, cnt_type, lower_group_count,
	                             upper_aggregates)) {
		return false;
	}

	auto upper_aggr =
	    DECreateUpperAggregate(optimizer, aggr, std::move(new_join), side_bindings, sides, std::move(upper_aggregates));

	auto projection =
	    BuildFinalProjection(optimizer, aggr, std::move(upper_aggr), replacement_map,
	                         [&](idx_t j, unique_ptr<Expression> ref) -> unique_ptr<Expression> {
		                         auto finalized = optimizer.BindScalarFunction("finalize", std::move(ref));
		                         if (finalized->GetReturnType() != aggregates[j].return_type) {
			                         return BoundCastExpression::AddCastToType(optimizer.context, std::move(finalized),
			                                                                   aggregates[j].return_type);
		                         }
		                         return finalized;
	                         });
	if (!projection) {
		return false;
	}
	op = std::move(projection);
	return true;
}

//===--------------------------------------------------------------------===//
// Key-Preserving Owner-Outer Count Pushdown
//===--------------------------------------------------------------------===//

bool PartialAggregatePushdown::TryOwnerOuterCountPushdown(unique_ptr<LogicalOperator> &op) {
	LogicalAggregate *aggr_ptr;
	LogicalComparisonJoin *join_ptr;
	if (!GetPushdownOperators(*op, aggr_ptr, join_ptr)) {
		return false;
	}
	auto &aggr = *aggr_ptr;
	auto &join = *join_ptr;
	if ((join.join_type != JoinType::LEFT && join.join_type != JoinType::RIGHT) || join.conditions.size() != 1 ||
	    join.HasProjectionMap() || join.HasArbitraryConditions() || aggr.groups.size() != 1 ||
	    aggr.expressions.size() != 1 || aggr.grouping_sets.size() > 1 || !aggr.grouping_functions.empty() ||
	    (!aggr.grouping_sets.empty() &&
	     aggr.grouping_sets[0].find(ProjectionIndex(0)) == aggr.grouping_sets[0].end())) {
		return false;
	}
	auto &condition = join.conditions[0];
	if (!condition.IsComparison() || condition.GetComparisonType() != ExpressionType::COMPARE_EQUAL) {
		return false;
	}
	PartialAggregatePushdownInfo info;
	LogicalJoin::GetTableReferences(*join.children[0], info.side_bindings[0]);
	LogicalJoin::GetTableReferences(*join.children[1], info.side_bindings[1]);
	info.dimension_side = join.join_type == JoinType::LEFT ? 0 : 1;
	info.aggregate_side = 1 - info.dimension_side;
	unique_ptr<Expression> *aggregate_key_expr;
	unique_ptr<Expression> *owner_key_expr;
	if (!GetJoinSideExpressions(condition, info, aggregate_key_expr, owner_key_expr)) {
		return false;
	}
	ColumnBinding owner_key;
	ColumnBinding group_key;
	if (!GetColumnBinding(**owner_key_expr, owner_key) || !GetColumnBinding(*aggr.groups[0], group_key) ||
	    group_key != owner_key) {
		return false;
	}
	ColumnBinding aggregate_key;
	if (!GetColumnBinding(**aggregate_key_expr, aggregate_key)) {
		return false;
	}
	auto &aggregate = aggr.expressions[0]->Cast<BoundAggregateExpression>();
	auto &aggregate_name = aggregate.Function().GetName();
	const bool count_star = aggregate_name == "count_star";
	if ((!count_star && aggregate_name != "count") || aggregate.IsDistinct() || aggregate.GetFilter() ||
	    aggregate.GetOrderBys() || (!count_star && aggregate.GetChildren().size() != 1) ||
	    (count_star && !aggregate.GetChildren().empty()) || aggregate.IsVolatile() ||
	    aggregate.StateExportMode() != AggregateStateExportMode::NONE) {
		return false;
	}
	if (!count_star && !GetColumnBinding(*aggregate.GetChildren()[0], group_key)) {
		return false;
	}
	if (!count_star && info.side_bindings[info.aggregate_side].find(group_key.table_index) ==
	                       info.side_bindings[info.aggregate_side].end()) {
		return false;
	}
	RelationStatsExtractor stats_extractor(optimizer.context);
	auto aggregate_stats = stats_extractor.Extract(*join.children[info.aggregate_side]);
	auto key_stats = aggregate_stats ? aggregate_stats->GetColumnStats(aggregate_key) : nullptr;
	if (!aggregate_stats || !key_stats ||
	    (key_stats->total_domain.source != DistinctCountSource::EXACT &&
	     key_stats->total_domain.source != DistinctCountSource::HLL)) {
		return false;
	}
	const auto aggregate_cardinality = aggregate_stats->cardinality;
	const auto key_count = MinValue(key_stats->total_domain.distinct_count, aggregate_cardinality);
	if (aggregate_cardinality == 0 || key_count == 0 ||
	    static_cast<double>(aggregate_cardinality) <
	        static_cast<double>(PartialAggregatePushdownHeuristics::MIN_AGGREGATE_TO_DIMENSION_RATIO) *
	            static_cast<double>(key_count) ||
	    !join.children[info.dimension_side]->has_estimated_cardinality || !join.has_estimated_cardinality ||
	    static_cast<double>(join.estimated_cardinality) <
	        PartialAggregatePushdownHeuristics::MIN_JOIN_RETENTION * static_cast<double>(aggregate_cardinality)) {
		return false;
	}

	auto lower_group_index = optimizer.binder.GenerateTableIndex();
	auto lower_aggregate_index = optimizer.binder.GenerateTableIndex();
	vector<unique_ptr<Expression>> lower_aggregates;
	lower_aggregates.push_back(aggregate.Copy());
	auto lower = make_uniq<LogicalAggregate>(lower_group_index, lower_aggregate_index, std::move(lower_aggregates));
	lower->groups.push_back((*aggregate_key_expr)->Copy());
	lower->children.push_back(std::move(join.children[info.aggregate_side]));
	lower->ResolveOperatorTypes();
	lower->SetEstimatedCardinality(key_count);

	auto aggregate_key_type = (*aggregate_key_expr)->GetReturnType();
	*aggregate_key_expr =
	    make_uniq<BoundColumnRefExpression>(aggregate_key_type, ColumnBinding(lower_group_index, ProjectionIndex(0)));
	join.children[info.aggregate_side] = std::move(lower);
	join.ResolveOperatorTypes();
	CopyCardinality(join, *join.children[info.dimension_side]);

	auto count_type = aggregate.GetReturnType();
	auto count_alias = aggregate.GetAlias();
	auto count_ref =
	    make_uniq<BoundColumnRefExpression>(count_type, ColumnBinding(lower_aggregate_index, ProjectionIndex(0)));
	auto count_value = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_COALESCE, count_type);
	count_value->GetChildrenMutable().push_back(std::move(count_ref));
	count_value->GetChildrenMutable().push_back(make_uniq<BoundConstantExpression>(Value::BIGINT(count_star ? 1 : 0)));
	vector<unique_ptr<Expression>> sum_children;
	sum_children.push_back(std::move(count_value));
	auto upper_sum = DEBindAggregate(optimizer.context, "sum", std::move(sum_children));
	if (!upper_sum) {
		return false;
	}
	upper_sum->SetAlias(count_alias);
	vector<unique_ptr<Expression>> upper_aggregates;
	upper_aggregates.push_back(std::move(upper_sum));
	auto upper_group_index = optimizer.binder.GenerateTableIndex();
	auto upper_aggregate_index = optimizer.binder.GenerateTableIndex();
	auto upper = make_uniq<LogicalAggregate>(upper_group_index, upper_aggregate_index, std::move(upper_aggregates));
	for (auto &group : aggr.groups) {
		upper->groups.push_back(group->Copy());
	}
	upper->grouping_sets = aggr.grouping_sets;
	upper->estimated_cardinality = aggr.estimated_cardinality;
	upper->has_estimated_cardinality = aggr.has_estimated_cardinality;
	upper->children.push_back(std::move(aggr.children[0]));
	upper->ResolveOperatorTypes();

	auto projection_index = optimizer.binder.GenerateTableIndex();
	vector<unique_ptr<Expression>> expressions;
	expressions.push_back(
	    make_uniq<BoundColumnRefExpression>(upper->types[0], ColumnBinding(upper_group_index, ProjectionIndex(0))));
	auto sum_ref =
	    make_uniq<BoundColumnRefExpression>(upper->types[1], ColumnBinding(upper_aggregate_index, ProjectionIndex(0)));
	auto count_result = BoundCastExpression::AddCastToType(optimizer.context, std::move(sum_ref), count_type);
	count_result->SetAlias(count_alias);
	expressions.push_back(std::move(count_result));

	auto projection = make_uniq<LogicalProjection>(projection_index, std::move(expressions));
	projection->children.push_back(std::move(upper));
	projection->ResolveOperatorTypes();
	CopyCardinality(*projection, aggr);
	AddReplacement(replacement_map, ColumnBinding(aggr.group_index, ProjectionIndex(0)),
	               ColumnBinding(projection_index, ProjectionIndex(0)));
	AddReplacement(replacement_map, ColumnBinding(aggr.aggregate_index, ProjectionIndex(0)),
	               ColumnBinding(projection_index, ProjectionIndex(1)));
	op = std::move(projection);
	return true;
}

//===--------------------------------------------------------------------===//
// Projection Fusion And Entry Points
//===--------------------------------------------------------------------===//

static void DEInlineProjection(unique_ptr<Expression> &expr, const LogicalProjection &proj) {
	if (expr->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &ref = expr->Cast<BoundColumnRefExpression>();
		if (ref.Binding().table_index == proj.table_index) {
			expr = proj.expressions[ref.Binding().column_index.GetIndex()]->Copy();
			return;
		}
	}
	ExpressionIterator::EnumerateChildren(*expr,
	                                      [&](unique_ptr<Expression> &child) { DEInlineProjection(child, proj); });
}

bool PartialAggregatePushdown::FuseInterveningProjections(LogicalOperator &op) {
	if (op.type != LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY || op.children.size() != 1) {
		return false;
	}
	if (op.children[0]->type != LogicalOperatorType::LOGICAL_PROJECTION) {
		return false;
	}
	// Only absorb projection chains that expose a comparison join underneath.
	vector<reference<LogicalProjection>> projections;
	reference<LogicalOperator> cur = *op.children[0];
	while (cur.get().type == LogicalOperatorType::LOGICAL_PROJECTION && cur.get().children.size() == 1) {
		projections.push_back(cur.get().Cast<LogicalProjection>());
		cur = *cur.get().children[0];
	}
	if (cur.get().type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		return false;
	}
	// Projection inlining would duplicate volatile expressions.
	for (auto &proj : projections) {
		for (auto &expr : proj.get().expressions) {
			if (expr->IsVolatile()) {
				return false;
			}
		}
	}
	auto &aggr = op.Cast<LogicalAggregate>();
	auto inline_all = [&](unique_ptr<Expression> &e) {
		for (auto &proj : projections) {
			DEInlineProjection(e, proj.get());
		}
	};
	for (auto &group : aggr.groups) {
		inline_all(group);
	}
	for (auto &expr : aggr.expressions) {
		inline_all(expr);
	}
	op.children[0] = std::move(projections.back().get().children[0]);
	return true;
}

void PartialAggregatePushdown::VisitOperator(unique_ptr<LogicalOperator> &op) {
	LogicalOperatorVisitor::VisitOperator(op);
	FuseInterveningProjections(*op);
	if (TryDoubleEagerPushdown(op) || TryOwnerOuterCountPushdown(op) || TryPushdownAggregate(op)) {
		// Revisit rewritten subtrees so nested aggregate-over-join shapes can be pushed too.
		LogicalOperatorVisitor::VisitOperator(op);
	}
}

unique_ptr<Expression> PartialAggregatePushdown::VisitReplace(BoundColumnRefExpression &expr,
                                                              unique_ptr<Expression> *expr_ptr) {
	auto entry = replacement_map.find(expr.Binding());
	if (entry != replacement_map.end()) {
		expr.BindingMutable() = entry->second;
	}
	return nullptr;
}

bool PartialAggregatePushdown::TryPushdownAggregate(unique_ptr<LogicalOperator> &op) {
	LogicalAggregate *aggr;
	LogicalComparisonJoin *join;
	if (!GetPushdownOperators(*op, aggr, join)) {
		return false;
	}

	PartialAggregatePushdownInfo info;
	if (!AnalyzePushdown(*aggr, *join, info)) {
		return false;
	}
	info.lower_group_index = optimizer.binder.GenerateTableIndex();
	info.lower_aggregate_index = optimizer.binder.GenerateTableIndex();
	info.upper_group_index = optimizer.binder.GenerateTableIndex();
	info.upper_aggregate_index = optimizer.binder.GenerateTableIndex();
	BuildLowerGroupMap(*aggr, *join, info);
	if (!PassesLowerGroupHeuristic(*aggr, info)) {
		return false;
	}

	vector<unique_ptr<Expression>> lower_aggregates;
	vector<unique_ptr<Expression>> upper_aggregates;
	if (!BindPushdownAggregates(optimizer.context, *aggr, info.lower_aggregate_index, lower_aggregates,
	                            upper_aggregates)) {
		return false;
	}

	auto lower_aggr = CreateLowerAggregate(*aggr, *join, info, std::move(lower_aggregates));
	auto new_join = CreateJoin(*join, info, std::move(lower_aggr));
	auto upper_aggr = CreateUpperAggregate(*aggr, std::move(new_join), info, std::move(upper_aggregates));
	auto final_projection =
	    BuildFinalProjection(optimizer, *aggr, std::move(upper_aggr), replacement_map,
	                         [&](idx_t j, unique_ptr<Expression> ref) -> unique_ptr<Expression> {
		                         auto finalized = optimizer.BindScalarFunction("finalize", std::move(ref));
		                         if (finalized->GetReturnType() != aggr->expressions[j]->GetReturnType()) {
			                         return nullptr; // state does not round-trip back to the original type
		                         }
		                         return finalized;
	                         });
	if (!final_projection) {
		return false;
	}
	op = std::move(final_projection);
	return true;
}

} // namespace duckdb
