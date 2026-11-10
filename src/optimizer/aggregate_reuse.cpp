#include "duckdb/optimizer/aggregate_reuse.hpp"

#include "duckdb/optimizer/aggregate_rewrite_helper.hpp"
#include "duckdb/optimizer/aggregate_reuse_internal.hpp"
#include "duckdb/function/builtin_function_lookup.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/function/aggregate/distributive_functions.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/function/scalar/generic_common.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_materialized_cte.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

namespace duckdb {

optional_idx GetDirectReferenceIndex(const Expression &expression, LogicalOperator &input) {
	return AggregateRewriteHelper::GetDirectReferenceIndex(expression, input);
}

struct AggregateJoinCandidate {
	idx_t owner_child;
	idx_t probe_child;
	idx_t owner_key;
	idx_t probe_key;
	vector<ColumnBinding> owner_groups;
};

optional_idx GetInvertibleReferenceIndex(const Expression &expression, LogicalOperator &input) {
	auto result = GetDirectReferenceIndex(expression, input);
	if (result.IsValid() || !BoundCastExpression::IsCast(expression)) {
		return result;
	}
	auto &cast = expression.Cast<BoundFunctionExpression>();
	if (!BoundCastExpression::CastIsInvertible(BoundCastExpression::SourceType(cast),
	                                           BoundCastExpression::TargetType(cast))) {
		return optional_idx();
	}
	return GetInvertibleReferenceIndex(BoundCastExpression::Child(cast), input);
}

optional<TableColumnOrigin> GetTableColumnOrigin(LogicalOperator &op, idx_t output_idx, bool allow_filtered) {
	if (output_idx >= op.GetColumnBindings().size()) {
		return nullopt;
	}
	if (op.type == LogicalOperatorType::LOGICAL_PROJECTION) {
		auto &projection = op.Cast<LogicalProjection>();
		if (projection.children.size() != 1 || output_idx >= projection.expressions.size()) {
			return nullopt;
		}
		auto child_idx = allow_filtered
		                     ? GetInvertibleReferenceIndex(*projection.expressions[output_idx], *projection.children[0])
		                     : GetDirectReferenceIndex(*projection.expressions[output_idx], *projection.children[0]);
		return child_idx.IsValid() ? GetTableColumnOrigin(*projection.children[0], child_idx.GetIndex(), allow_filtered)
		                           : nullopt;
	}
	if (op.type == LogicalOperatorType::LOGICAL_FILTER) {
		if (!allow_filtered) {
			return nullopt;
		}
		auto &filter = op.Cast<LogicalFilter>();
		if (filter.children.size() != 1) {
			return nullopt;
		}
		if (!filter.projection_map.empty()) {
			if (output_idx >= filter.projection_map.size()) {
				return nullopt;
			}
			output_idx = filter.projection_map[output_idx].GetIndex();
		}
		return GetTableColumnOrigin(*filter.children[0], output_idx, allow_filtered);
	}
	if (op.type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		if (!allow_filtered) {
			return nullopt;
		}
		auto binding = op.GetColumnBindings()[output_idx];
		for (auto &child : op.children) {
			auto bindings = child->GetColumnBindings();
			auto entry = std::find(bindings.begin(), bindings.end(), binding);
			if (entry != bindings.end()) {
				return GetTableColumnOrigin(*child, NumericCast<idx_t>(entry - bindings.begin()), allow_filtered);
			}
		}
		return nullopt;
	}
	if (op.type != LogicalOperatorType::LOGICAL_GET) {
		return nullopt;
	}
	auto &get = op.Cast<LogicalGet>();
	auto table = get.GetTable();
	if (!table || !get.children.empty() || !get.scan_partition_indices.empty() ||
	    (!allow_filtered &&
	     (get.table_filters.HasFilters() || (get.dynamic_filters && get.dynamic_filters->HasFilters())))) {
		return nullopt;
	}
	return TableColumnOrigin {*table, get.GetColumnIndex(ProjectionIndex(output_idx))};
}

optional<TableColumnOrigin> GetTableColumnOrigin(LogicalOperator &op, const Expression &expression,
                                                 bool allow_filtered) {
	auto output_idx = GetDirectReferenceIndex(expression, op);
	return output_idx.IsValid() ? GetTableColumnOrigin(op, output_idx.GetIndex(), allow_filtered) : nullopt;
}

bool SameOrigin(const TableColumnOrigin &left, const TableColumnOrigin &right) {
	return &left.table.get() == &right.table.get() && left.column == right.column;
}

static optional_idx GetJoinOutputReference(const Expression &expression, LogicalComparisonJoin &join,
                                           idx_t &child_index) {
	if (expression.GetExpressionClass() == ExpressionClass::BOUND_REF) {
		auto index = expression.Cast<BoundReferenceExpression>().Index();
		auto left_count = join.children[0]->GetColumnBindings().size();
		if (index < left_count) {
			child_index = 0;
			return optional_idx(index);
		}
		index -= left_count;
		if (index < join.children[1]->GetColumnBindings().size()) {
			child_index = 1;
			return optional_idx(index);
		}
		return optional_idx();
	}
	if (expression.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return optional_idx();
	}
	for (idx_t side = 0; side < 2; side++) {
		auto index = GetDirectReferenceIndex(expression, *join.children[side]);
		if (index.IsValid()) {
			child_index = side;
			return index;
		}
	}
	return optional_idx();
}

bool HasCompleteGrouping(const LogicalAggregate &aggregate) {
	if (aggregate.groups.empty() || !aggregate.grouping_functions.empty() || aggregate.grouping_sets.size() > 1) {
		return false;
	}
	if (aggregate.grouping_sets.empty()) {
		return true;
	}
	auto &grouping_set = aggregate.grouping_sets[0];
	if (grouping_set.size() != aggregate.groups.size()) {
		return false;
	}
	for (idx_t group_idx = 0; group_idx < aggregate.groups.size(); group_idx++) {
		if (grouping_set.find(ProjectionIndex(group_idx)) == grouping_set.end()) {
			return false;
		}
	}
	return true;
}

static optional<AggregateJoinCandidate> GetCandidate(LogicalAggregate &aggregate, LogicalComparisonJoin &join) {
	if (join.join_type != JoinType::INNER || join.children.size() != 2 || join.conditions.size() != 1 ||
	    join.HasProjectionMap() || join.HasArbitraryConditions() || aggregate.expressions.size() != 1 ||
	    !HasCompleteGrouping(aggregate)) {
		return nullopt;
	}
	auto &condition = join.conditions[0];
	if (!condition.IsComparison() || condition.GetComparisonType() != ExpressionType::COMPARE_EQUAL ||
	    condition.GetLHS().GetReturnType() != condition.GetRHS().GetReturnType()) {
		return nullopt;
	}
	auto left_key = GetDirectReferenceIndex(condition.GetLHS(), *join.children[0]);
	auto right_key = GetDirectReferenceIndex(condition.GetRHS(), *join.children[1]);
	if (!left_key.IsValid() || !right_key.IsValid()) {
		return nullopt;
	}
	auto &outer_aggregate = aggregate.expressions[0]->Cast<BoundAggregateExpression>();
	if (outer_aggregate.IsDistinct() || outer_aggregate.IsVolatile() ||
	    outer_aggregate.StateExportMode() != AggregateStateExportMode::NONE || outer_aggregate.GetFilter() ||
	    outer_aggregate.GetOrderBys() || outer_aggregate.GetChildren().size() != 1 ||
	    !outer_aggregate.Function().HasGetStateTypeCallback() ||
	    !outer_aggregate.Function().HasStateCombineCallback() ||
	    !outer_aggregate.Function().HasStateFinalizeCallback()) {
		return nullopt;
	}
	for (idx_t owner_child = 0; owner_child < 2; owner_child++) {
		const auto probe_child = 1 - owner_child;
		if (!GetDirectReferenceIndex(*outer_aggregate.GetChildren()[0], *join.children[probe_child]).IsValid()) {
			continue;
		}
		const auto owner_key = owner_child == 0 ? left_key.GetIndex() : right_key.GetIndex();
		const auto probe_key = probe_child == 0 ? left_key.GetIndex() : right_key.GetIndex();
		vector<ColumnBinding> owner_groups;
		auto owner_bindings = join.children[owner_child]->GetColumnBindings();
		bool valid = true;
		for (auto &group : aggregate.groups) {
			idx_t group_child;
			auto group_index = GetJoinOutputReference(*group, join, group_child);
			if (!group_index.IsValid() || group_child != owner_child) {
				valid = false;
				break;
			}
			owner_groups.push_back(owner_bindings[group_index.GetIndex()]);
		}
		if (valid) {
			return AggregateJoinCandidate {owner_child, probe_child, owner_key, probe_key, std::move(owner_groups)};
		}
	}
	return nullopt;
}

static optional_idx FindTracedOutput(LogicalOperator &op, const ColumnBinding &target) {
	auto bindings = op.GetColumnBindings();
	for (idx_t output_idx = 0; output_idx < bindings.size(); output_idx++) {
		if (bindings[output_idx] == target) {
			return optional_idx(output_idx);
		}
	}
	if (op.children.size() != 1) {
		return optional_idx();
	}
	if (op.type == LogicalOperatorType::LOGICAL_FILTER) {
		auto &filter = op.Cast<LogicalFilter>();
		auto traced = FindTracedOutput(*filter.children[0], target);
		if (!traced.IsValid() || filter.projection_map.empty()) {
			return traced;
		}
		for (idx_t output_idx = 0; output_idx < filter.projection_map.size(); output_idx++) {
			if (filter.projection_map[output_idx].GetIndex() == traced.GetIndex()) {
				return optional_idx(output_idx);
			}
		}
		return optional_idx();
	}
	if (op.type != LogicalOperatorType::LOGICAL_PROJECTION) {
		return optional_idx();
	}
	auto &projection = op.Cast<LogicalProjection>();
	auto traced = FindTracedOutput(*projection.children[0], target);
	if (!traced.IsValid()) {
		return optional_idx();
	}
	for (idx_t output_idx = 0; output_idx < projection.expressions.size(); output_idx++) {
		auto child_idx = GetInvertibleReferenceIndex(*projection.expressions[output_idx], *projection.children[0]);
		if (child_idx == traced) {
			return optional_idx(output_idx);
		}
	}
	return optional_idx();
}

optional_ptr<LogicalAggregate> FindUnaryAggregate(LogicalOperator &op) {
	reference<LogicalOperator> current(op);
	while (current.get().type != LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		if (current.get().children.size() != 1 || (current.get().type != LogicalOperatorType::LOGICAL_PROJECTION &&
		                                           current.get().type != LogicalOperatorType::LOGICAL_FILTER)) {
			return nullptr;
		}
		current = *current.get().children[0];
	}
	return current.get().Cast<LogicalAggregate>();
}

static optional<ColumnBinding> PromoteAggregateBinding(LogicalOperator &op, LogicalAggregate &aggregate,
                                                       const ColumnBinding &binding, const LogicalType &type,
                                                       bool apply) {
	if (&op == &aggregate) {
		return binding;
	}
	if (op.children.size() != 1 ||
	    (op.type != LogicalOperatorType::LOGICAL_PROJECTION && op.type != LogicalOperatorType::LOGICAL_FILTER)) {
		return nullopt;
	}
	auto child_binding = PromoteAggregateBinding(*op.children[0], aggregate, binding, type, apply);
	if (!child_binding) {
		return nullopt;
	}
	if (op.type == LogicalOperatorType::LOGICAL_PROJECTION) {
		auto &projection = op.Cast<LogicalProjection>();
		auto result = ColumnBinding(projection.table_index, ProjectionIndex(projection.expressions.size()));
		if (apply) {
			projection.expressions.push_back(make_uniq<BoundColumnRefExpression>(type, *child_binding));
			projection.ResolveOperatorTypes();
		}
		return result;
	}
	auto &filter = op.Cast<LogicalFilter>();
	if (apply && !filter.projection_map.empty()) {
		auto bindings = filter.children[0]->GetColumnBindings();
		auto entry = std::find(bindings.begin(), bindings.end(), *child_binding);
		if (entry == bindings.end()) {
			throw InternalException("Aggregate state is not exposed by the filter child");
		}
		filter.projection_map.push_back(ProjectionIndex(NumericCast<idx_t>(entry - bindings.begin())));
		filter.ResolveOperatorTypes();
	}
	return child_binding;
}

static void FinalizeAggregateReferences(unique_ptr<Expression> &expression, const ColumnBinding &binding,
                                        const LogicalType &state_type, Optimizer &optimizer) {
	ExpressionIterator::VisitExpressionMutable<BoundColumnRefExpression>(
	    expression, [&](BoundColumnRefExpression &ref, unique_ptr<Expression> &expr) {
		    if (ref.Binding() != binding) {
			    return;
		    }
		    auto state_ref = make_uniq<BoundColumnRefExpression>(ref.GetAlias(), state_type, binding);
		    expr = optimizer.BindScalarFunction("finalize", std::move(state_ref));
	    });
}

static bool FinalizeAggregateConsumers(LogicalOperator &op, LogicalAggregate &aggregate, const ColumnBinding &binding,
                                       const LogicalType &state_type, Optimizer &optimizer) {
	if (&op == &aggregate) {
		return true;
	}
	if (op.children.size() != 1 ||
	    (op.type != LogicalOperatorType::LOGICAL_PROJECTION && op.type != LogicalOperatorType::LOGICAL_FILTER) ||
	    !FinalizeAggregateConsumers(*op.children[0], aggregate, binding, state_type, optimizer)) {
		return false;
	}
	if (op.type == LogicalOperatorType::LOGICAL_PROJECTION) {
		auto &projection = op.Cast<LogicalProjection>();
		for (auto &expression : projection.expressions) {
			FinalizeAggregateReferences(expression, binding, state_type, optimizer);
		}
	} else {
		auto &filter = op.Cast<LogicalFilter>();
		for (auto &expression : filter.expressions) {
			FinalizeAggregateReferences(expression, binding, state_type, optimizer);
		}
	}
	op.ResolveOperatorTypes();
	return true;
}

static bool SameReusableAggregate(const BoundAggregateExpression &outer, LogicalOperator &outer_input,
                                  const BoundAggregateExpression &inner, LogicalOperator &inner_input) {
	if (outer.IsDistinct() || inner.IsDistinct() || outer.Function() != inner.Function() ||
	    outer.GetReturnType() != inner.GetReturnType() || outer.StateExportMode() != inner.StateExportMode() ||
	    outer.GetFilter() || inner.GetFilter() || outer.GetOrderBys() || inner.GetOrderBys() ||
	    outer.GetChildren().size() != 1 || inner.GetChildren().size() != 1 ||
	    !FunctionData::Equals(outer.BindInfo().get(), inner.BindInfo().get())) {
		return false;
	}
	auto outer_origin = GetTableColumnOrigin(outer_input, *outer.GetChildren()[0]);
	auto inner_origin = GetTableColumnOrigin(inner_input, *inner.GetChildren()[0]);
	return outer_origin && inner_origin && SameOrigin(*outer_origin, *inner_origin);
}

static optional<ColumnBinding> PromoteReusableAggregate(Optimizer &optimizer, unique_ptr<LogicalOperator> &op,
                                                        ColumnBinding key_binding, LogicalOperator &outer_probe,
                                                        const TableColumnOrigin &outer_key_origin,
                                                        const BoundAggregateExpression &outer_aggregate,
                                                        const LogicalType &state_type, bool apply) {
	if (op->type == LogicalOperatorType::LOGICAL_PROJECTION) {
		auto &projection = op->Cast<LogicalProjection>();
		auto bindings = projection.GetColumnBindings();
		auto key_entry = std::find(bindings.begin(), bindings.end(), key_binding);
		if (projection.children.size() != 1 || key_entry == bindings.end()) {
			return nullopt;
		}
		auto key_idx = NumericCast<idx_t>(key_entry - bindings.begin());
		auto child_idx = GetDirectReferenceIndex(*projection.expressions[key_idx], *projection.children[0]);
		if (!child_idx.IsValid()) {
			return nullopt;
		}
		auto child_binding = projection.children[0]->GetColumnBindings()[child_idx.GetIndex()];
		auto payload = PromoteReusableAggregate(optimizer, projection.children[0], child_binding, outer_probe,
		                                        outer_key_origin, outer_aggregate, state_type, apply);
		if (!payload) {
			return nullopt;
		}
		const auto result = ColumnBinding(projection.table_index, ProjectionIndex(projection.expressions.size()));
		if (apply) {
			projection.expressions.push_back(make_uniq<BoundColumnRefExpression>(state_type, *payload));
			projection.ResolveOperatorTypes();
		}
		return result;
	}
	if (op->type == LogicalOperatorType::LOGICAL_FILTER) {
		auto &filter = op->Cast<LogicalFilter>();
		auto bindings = filter.GetColumnBindings();
		auto key_entry = std::find(bindings.begin(), bindings.end(), key_binding);
		if (filter.children.size() != 1 || key_entry == bindings.end()) {
			return nullopt;
		}
		auto child_bindings = filter.children[0]->GetColumnBindings();
		idx_t child_key_idx = NumericCast<idx_t>(key_entry - bindings.begin());
		if (!filter.projection_map.empty()) {
			child_key_idx = filter.projection_map[child_key_idx].GetIndex();
		}
		auto payload = PromoteReusableAggregate(optimizer, filter.children[0], child_bindings[child_key_idx],
		                                        outer_probe, outer_key_origin, outer_aggregate, state_type, apply);
		if (!payload) {
			return nullopt;
		}
		if (apply && !filter.projection_map.empty()) {
			child_bindings = filter.children[0]->GetColumnBindings();
			auto payload_entry = std::find(child_bindings.begin(), child_bindings.end(), *payload);
			if (payload_entry == child_bindings.end()) {
				throw InternalException("Validated aggregate payload is not exposed by its child plan");
			}
			filter.projection_map.push_back(
			    ProjectionIndex(NumericCast<idx_t>(payload_entry - child_bindings.begin())));
			filter.ResolveOperatorTypes();
		}
		return payload;
	}
	if (op->type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		return nullopt;
	}
	auto &join = op->Cast<LogicalComparisonJoin>();
	if (join.children.size() != 2 || join.HasProjectionMap()) {
		return nullopt;
	}
	if (join.join_type == JoinType::SEMI) {
		if (join.conditions.size() != 1 || join.conditions[0].GetComparisonType() != ExpressionType::COMPARE_EQUAL ||
		    join.children[1]->type != LogicalOperatorType::LOGICAL_PROJECTION) {
			return nullopt;
		}
		auto left_key = GetDirectReferenceIndex(join.conditions[0].GetLHS(), *join.children[0]);
		auto right_key = GetDirectReferenceIndex(join.conditions[0].GetRHS(), *join.children[1]);
		if (!left_key.IsValid() || !right_key.IsValid() ||
		    join.children[0]->GetColumnBindings()[left_key.GetIndex()] != key_binding) {
			return nullopt;
		}
		auto &right_projection = join.children[1]->Cast<LogicalProjection>();
		auto inner_aggregate = FindUnaryAggregate(*right_projection.children[0]);
		if (!inner_aggregate || inner_aggregate->children.size() != 1 || inner_aggregate->groups.size() != 1 ||
		    inner_aggregate->grouping_sets.size() > 1 || !inner_aggregate->grouping_functions.empty()) {
			return nullopt;
		}
		if (!inner_aggregate->grouping_sets.empty() &&
		    (inner_aggregate->grouping_sets[0].size() != 1 ||
		     inner_aggregate->grouping_sets[0].find(ProjectionIndex(0)) == inner_aggregate->grouping_sets[0].end())) {
			return nullopt;
		}
		auto group_binding = ColumnBinding(inner_aggregate->group_index, ProjectionIndex(0));
		auto traced_group = FindTracedOutput(*right_projection.children[0], group_binding);
		auto projected_key = GetInvertibleReferenceIndex(*right_projection.expressions[right_key.GetIndex()],
		                                                 *right_projection.children[0]);
		if (!traced_group.IsValid() || projected_key != traced_group) {
			return nullopt;
		}
		auto right_key_origin = GetTableColumnOrigin(*inner_aggregate->children[0], *inner_aggregate->groups[0]);
		if (!right_key_origin || !SameOrigin(*right_key_origin, outer_key_origin)) {
			return nullopt;
		}
		idx_t aggregate_idx = DConstants::INVALID_INDEX;
		for (idx_t idx = 0; idx < inner_aggregate->expressions.size(); idx++) {
			auto &inner = inner_aggregate->expressions[idx]->Cast<BoundAggregateExpression>();
			if (!SameReusableAggregate(outer_aggregate, outer_probe, inner, *inner_aggregate->children[0])) {
				continue;
			}
			if (aggregate_idx != DConstants::INVALID_INDEX) {
				return nullopt;
			}
			aggregate_idx = idx;
		}
		if (aggregate_idx == DConstants::INVALID_INDEX) {
			return nullopt;
		}
		auto aggregate_copy =
		    unique_ptr_cast<Expression, BoundAggregateExpression>(inner_aggregate->expressions[aggregate_idx]->Copy());
		auto exported = ExportAggregateFunction::Bind(std::move(aggregate_copy));
		if (exported->GetReturnType() != state_type) {
			return nullopt;
		}
		auto aggregate_binding = ColumnBinding(inner_aggregate->aggregate_index, ProjectionIndex(aggregate_idx));
		if (apply) {
			if (!FinalizeAggregateConsumers(*right_projection.children[0], *inner_aggregate, aggregate_binding,
			                                state_type, optimizer)) {
				throw InternalException("Validated aggregate consumers can no longer be finalized");
			}
			inner_aggregate->expressions[aggregate_idx] = std::move(exported);
			inner_aggregate->ResolveOperatorTypes();
		}
		auto payload_binding = PromoteAggregateBinding(*right_projection.children[0], *inner_aggregate,
		                                               aggregate_binding, state_type, apply);
		if (!payload_binding) {
			return nullopt;
		}
		const auto result =
		    ColumnBinding(right_projection.table_index, ProjectionIndex(right_projection.expressions.size()));
		if (apply) {
			right_projection.expressions.push_back(make_uniq<BoundColumnRefExpression>(state_type, *payload_binding));
			join.join_type = JoinType::INNER;
			right_projection.ResolveOperatorTypes();
			join.ResolveOperatorTypes();
		}
		return result;
	}
	if (join.join_type != JoinType::INNER) {
		return nullopt;
	}
	for (auto &child : join.children) {
		auto child_bindings = child->GetColumnBindings();
		if (std::find(child_bindings.begin(), child_bindings.end(), key_binding) == child_bindings.end()) {
			continue;
		}
		auto payload = PromoteReusableAggregate(optimizer, child, key_binding, outer_probe, outer_key_origin,
		                                        outer_aggregate, state_type, apply);
		if (payload) {
			if (apply) {
				join.ResolveOperatorTypes();
			}
			return payload;
		}
	}
	return nullopt;
}

AggregateReuseOptimizer::AggregateReuseOptimizer(Optimizer &optimizer_p) : optimizer(optimizer_p) {
}

void AggregateReuseOptimizer::CollectCTEs(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE) {
		auto &cte = op.Cast<LogicalMaterializedCTE>();
		cte_definitions.emplace(cte.table_index.index, cte);
	}
	for (auto &child : op.children) {
		CollectCTEs(*child);
	}
}

void AggregateReuseOptimizer::VisitOperator(unique_ptr<LogicalOperator> &op) {
	LogicalOperatorVisitor::VisitOperator(op);
	if (!TryReuseMaterializedAggregate(op)) {
		TryReuseSemiAggregate(op);
	}
}

unique_ptr<Expression> AggregateReuseOptimizer::VisitReplace(BoundColumnRefExpression &expression,
                                                             unique_ptr<Expression> *expression_ptr) {
	auto entry = replacement_map.find(expression.Binding());
	if (entry != replacement_map.end()) {
		expression.BindingMutable() = entry->second;
	}
	return nullptr;
}

bool AggregateReuseOptimizer::TryReuseSemiAggregate(unique_ptr<LogicalOperator> &op) {
	if (op->type != LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		return false;
	}
	auto &aggregate = op->Cast<LogicalAggregate>();
	if (aggregate.children.size() != 1 || aggregate.children[0]->type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		return false;
	}
	auto &join = aggregate.children[0]->Cast<LogicalComparisonJoin>();
	auto candidate = GetCandidate(aggregate, join);
	if (!candidate) {
		return false;
	}
	auto &outer_aggregate = aggregate.expressions[0]->Cast<BoundAggregateExpression>();
	auto exported_outer =
	    ExportAggregateFunction::Bind(unique_ptr_cast<Expression, BoundAggregateExpression>(outer_aggregate.Copy()));
	auto state_type = exported_outer->GetReturnType();
	if (!state_type.IsAggregateState()) {
		return false;
	}
	auto &owner = join.children[candidate->owner_child];
	auto &probe = join.children[candidate->probe_child];
	auto owner_bindings = owner->GetColumnBindings();
	auto outer_key_origin = GetTableColumnOrigin(*probe, candidate->probe_key);
	if (!outer_key_origin || candidate->owner_key >= owner_bindings.size()) {
		return false;
	}
	auto planned_payload = PromoteReusableAggregate(optimizer, owner, owner_bindings[candidate->owner_key], *probe,
	                                                *outer_key_origin, outer_aggregate, state_type, false);
	if (!planned_payload) {
		return false;
	}
	auto payload_binding = PromoteReusableAggregate(optimizer, owner, owner_bindings[candidate->owner_key], *probe,
	                                                *outer_key_origin, outer_aggregate, state_type, true);
	if (!payload_binding || *payload_binding != *planned_payload) {
		throw InternalException("Aggregate payload changed after validating the owner plan");
	}
	owner_bindings = owner->GetColumnBindings();
	vector<unique_ptr<Expression>> upper_groups;
	upper_groups.reserve(aggregate.groups.size());
	for (idx_t group_idx = 0; group_idx < candidate->owner_groups.size(); group_idx++) {
		auto owner_entry = std::find(owner_bindings.begin(), owner_bindings.end(), candidate->owner_groups[group_idx]);
		if (owner_entry == owner_bindings.end()) {
			throw InternalException("Aggregate owner group changed after validation");
		}
		auto owner_idx = NumericCast<idx_t>(owner_entry - owner_bindings.begin());
		upper_groups.push_back(make_uniq<BoundColumnRefExpression>(aggregate.groups[group_idx]->GetAlias(),
		                                                           aggregate.groups[group_idx]->GetReturnType(),
		                                                           owner_bindings[owner_idx]));
	}
	FunctionBinder function_binder(optimizer.context);
	vector<unique_ptr<Expression>> combine_arguments;
	combine_arguments.push_back(make_uniq<BoundColumnRefExpression>(state_type, *planned_payload));
	auto combine_function = GetBuiltinAggregateFunction(optimizer.context, CombineAggrFun::Name, {state_type});
	auto combined_state = function_binder.BindAggregateFunction(combine_function, std::move(combine_arguments));
	if (combined_state->GetReturnType() != state_type) {
		throw InternalException("Aggregate state changed while binding aggregate reuse combination");
	}
	vector<unique_ptr<Expression>> upper_aggregates;
	upper_aggregates.push_back(std::move(combined_state));
	auto upper_group_index = optimizer.binder.GenerateTableIndex();
	auto upper_aggregate_index = optimizer.binder.GenerateTableIndex();
	auto upper = make_uniq<LogicalAggregate>(upper_group_index, upper_aggregate_index, std::move(upper_aggregates));
	upper->groups = std::move(upper_groups);
	upper->grouping_sets = aggregate.grouping_sets;
	upper->estimated_cardinality = aggregate.estimated_cardinality;
	upper->has_estimated_cardinality = aggregate.has_estimated_cardinality;
	upper->children.push_back(std::move(owner));
	upper->ResolveOperatorTypes();

	const auto projection_index = optimizer.binder.GenerateTableIndex();
	vector<unique_ptr<Expression>> expressions;
	expressions.reserve(aggregate.groups.size() + 1);
	for (idx_t group_idx = 0; group_idx < aggregate.groups.size(); group_idx++) {
		expressions.push_back(make_uniq<BoundColumnRefExpression>(
		    aggregate.groups[group_idx]->GetAlias(), aggregate.groups[group_idx]->GetReturnType(),
		    ColumnBinding(upper_group_index, ProjectionIndex(group_idx))));
		replacement_map[ColumnBinding(aggregate.group_index, ProjectionIndex(group_idx))] =
		    ColumnBinding(projection_index, ProjectionIndex(group_idx));
	}
	auto state_ref =
	    make_uniq<BoundColumnRefExpression>(state_type, ColumnBinding(upper_aggregate_index, ProjectionIndex(0)));
	auto finalized = optimizer.BindScalarFunction("finalize", std::move(state_ref));
	if (finalized->GetReturnType() != outer_aggregate.GetReturnType()) {
		throw InternalException("Aggregate return type changed while binding aggregate reuse finalization");
	}
	finalized->SetAlias(outer_aggregate.GetAlias());
	expressions.push_back(std::move(finalized));
	replacement_map[ColumnBinding(aggregate.aggregate_index, ProjectionIndex(0))] =
	    ColumnBinding(projection_index, ProjectionIndex(aggregate.groups.size()));
	auto projection = make_uniq<LogicalProjection>(projection_index, std::move(expressions));
	projection->estimated_cardinality = aggregate.estimated_cardinality;
	projection->has_estimated_cardinality = aggregate.has_estimated_cardinality;
	projection->children.push_back(std::move(upper));
	projection->ResolveOperatorTypes();
	op = std::move(projection);
	return true;
}

} // namespace duckdb
