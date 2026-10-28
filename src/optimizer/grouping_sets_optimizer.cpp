#include "duckdb/optimizer/grouping_sets_optimizer.hpp"

#include "duckdb/common/algorithm.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/aggregate/distributive_functions.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/function/scalar/generic_common.hpp"
#include "duckdb/optimizer/aggregate_rewrite_helper.hpp"
#include "duckdb/function/builtin_function_lookup.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"
#include "duckdb/planner/operator/logical_materialized_cte.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_set_operation.hpp"

namespace duckdb {

GroupingSetsOptimizer::GroupingSetsOptimizer(Optimizer &optimizer_p, bool mandatory_aggregate_lowering_p)
    : optimizer(optimizer_p), mandatory_aggregate_lowering(mandatory_aggregate_lowering_p) {
}

namespace {

//! A single grouping set in the cascade, in the order in which it is computed
struct GroupingSetLevel {
	explicit GroupingSetLevel(idx_t grouping_set_idx) : grouping_set_idx(grouping_set_idx) {
	}

	//! The index of the grouping set (within LogicalAggregate::grouping_sets) computed by this level
	idx_t grouping_set_idx;
	//! The level this level aggregates over (invalid for the finest level, which aggregates the base data)
	optional_idx source_level;
	//! Whether this level is the source of another level (and must be materialized as a CTE)
	bool materialized = false;
	//! The table index of the materialized CTE (only set if materialized)
	TableIndex cte_index;
	//! The output types of this level: the group columns (in ascending group order), then the aggregate states
	vector<LogicalType> output_types;
	//! The output names of this level
	vector<Identifier> output_names;
	//! The position of each group within the level output
	map<ProjectionIndex, idx_t> group_positions;
	//! The aggregate computing this level
	unique_ptr<LogicalAggregate> aggregate;
};

} // namespace

//! The maximum number of grouping sets that are rewritten into separate aggregates
static constexpr idx_t MAX_REWRITE_GROUPING_SETS = 1024;

static bool CanRewriteAggregate(const BoundAggregateExpression &aggregate) {
	if (aggregate.IsDistinct() || aggregate.GetOrderBys()) {
		// DISTINCT / ORDER BY aggregates cannot be computed by combining states of a finer aggregation
		return false;
	}
	if (aggregate.StateExportMode() != AggregateStateExportMode::NONE) {
		// the aggregate already exports its state
		return false;
	}
	// mirror the requirements of ExportAggregateFunction::Bind so that binding the export cannot fail
	auto &function = aggregate.Function();
	if (!function.HasStateCombineCallback() || function.HasStateDestructorCallback() ||
	    !function.HasStateSizeCallback() || !function.HasStateFinalizeCallback() ||
	    !function.HasGetStateTypeCallback()) {
		return false;
	}
	return true;
}

static int64_t GetGroupingValue(const GroupingSet &grouping_set,
                                const unsafe_vector<ProjectionIndex> &grouping_function) {
	int64_t grouping_value = 0;
	for (idx_t i = 0; i < grouping_function.size(); i++) {
		if (grouping_set.find(grouping_function[i]) == grouping_set.end()) {
			// we do not group on this column in this grouping set
			grouping_value += 1LL << (grouping_function.size() - (i + 1));
		}
	}
	return grouping_value;
}

//! Order the grouping sets so that each set can be computed by re-aggregating an already-computed superset
static bool FindCascade(const vector<GroupingSet> &grouping_sets, vector<GroupingSetLevel> &levels) {
	// order the grouping sets by size (descending) - supersets must be computed before their subsets
	for (idx_t set_idx = 0; set_idx < grouping_sets.size(); set_idx++) {
		levels.emplace_back(set_idx);
	}
	std::stable_sort(levels.begin(), levels.end(), [&](const GroupingSetLevel &a, const GroupingSetLevel &b) {
		return grouping_sets[a.grouping_set_idx].size() > grouping_sets[b.grouping_set_idx].size();
	});
	// each level is computed from the smallest already-computed level that is a superset of it
	// for ROLLUP this forms a chain, for CUBE a lattice rooted in the complete grouping set
	for (idx_t level_idx = 1; level_idx < levels.size(); level_idx++) {
		auto &grouping_set = grouping_sets[levels[level_idx].grouping_set_idx];
		for (idx_t source_idx = level_idx; source_idx > 0; source_idx--) {
			auto &source_set = grouping_sets[levels[source_idx - 1].grouping_set_idx];
			if (std::includes(source_set.begin(), source_set.end(), grouping_set.begin(), grouping_set.end())) {
				auto source_level = source_idx - 1;
				if (source_set.size() == grouping_set.size() && levels[source_level].source_level.IsValid()) {
					// identical grouping set: share the source instead of chaining, to keep the cascade shallow
					source_level = levels[source_level].source_level.GetIndex();
				}
				levels[level_idx].source_level = source_level;
				levels[source_level].materialized = true;
				break;
			}
		}
		if (!levels[level_idx].source_level.IsValid()) {
			// no computed superset to compute this grouping set from - we cannot cascade
			return false;
		}
	}
	return true;
}

bool GroupingSetsOptimizer::TryExpandGroupingSets(unique_ptr<LogicalOperator> &op) {
	auto &aggr = op->Cast<LogicalAggregate>();
	const idx_t group_count = aggr.groups.size();
	const idx_t aggregate_count = aggr.expressions.size();

	AggregateRewriteHelper::StageVolatileAggregateInputs(optimizer, aggr, op->children[0]);

	op->children[0]->ResolveOperatorTypes();
	auto input_types = op->children[0]->types;
	auto input_names = AggregateRewriteHelper::GenerateColumnNames("__grouping_sets_input", input_types.size());
	auto input_bindings = op->children[0]->GetColumnBindings();
	const auto cte_index = optimizer.binder.GenerateTableIndex();

	vector<unique_ptr<LogicalOperator>> branches(aggr.grouping_sets.size());
	for (idx_t grouping_set_idx = 0; grouping_set_idx < aggr.grouping_sets.size(); grouping_set_idx++) {
		auto &grouping_set = aggr.grouping_sets[grouping_set_idx];

		column_binding_map_t<ColumnBinding> input_replacements;
		auto branch_child = AggregateRewriteHelper::CreateCTERef(optimizer, cte_index, input_types, input_names,
		                                                         input_bindings, input_replacements);

		map<ProjectionIndex, idx_t> group_positions;
		vector<unique_ptr<Expression>> branch_groups;
		branch_groups.reserve(grouping_set.size());
		for (auto &group_idx : grouping_set) {
			group_positions[group_idx] = branch_groups.size();
			branch_groups.push_back(AggregateRewriteHelper::CopyAndRebind(*aggr.groups[group_idx], input_replacements));
		}

		vector<unique_ptr<Expression>> branch_aggregates;
		branch_aggregates.reserve(aggregate_count);
		for (auto &expr : aggr.expressions) {
			branch_aggregates.push_back(AggregateRewriteHelper::CopyAndRebind(*expr, input_replacements));
		}

		auto branch_group_index = optimizer.binder.GenerateTableIndex();
		auto branch_aggregate_index = optimizer.binder.GenerateTableIndex();
		auto branch_aggregate =
		    make_uniq<LogicalAggregate>(branch_group_index, branch_aggregate_index, std::move(branch_aggregates));
		branch_aggregate->groups = std::move(branch_groups);
		branch_aggregate->children.push_back(std::move(branch_child));
		if (aggr.has_estimated_cardinality) {
			branch_aggregate->SetEstimatedCardinality(aggr.estimated_cardinality);
		}

		vector<unique_ptr<Expression>> projection_expressions;
		projection_expressions.reserve(group_count + aggregate_count + aggr.grouping_functions.size());
		for (idx_t group_idx = 0; group_idx < group_count; group_idx++) {
			auto &group_type = aggr.groups[group_idx]->GetReturnType();
			auto entry = group_positions.find(ProjectionIndex(group_idx));
			if (entry == group_positions.end()) {
				projection_expressions.push_back(make_uniq<BoundConstantExpression>(Value(group_type)));
			} else {
				projection_expressions.push_back(make_uniq<BoundColumnRefExpression>(
				    group_type, ColumnBinding(branch_group_index, ProjectionIndex(entry->second))));
			}
		}
		for (idx_t aggregate_idx = 0; aggregate_idx < aggregate_count; aggregate_idx++) {
			projection_expressions.push_back(make_uniq<BoundColumnRefExpression>(
			    aggr.expressions[aggregate_idx]->GetReturnType(),
			    ColumnBinding(branch_aggregate_index, ProjectionIndex(aggregate_idx))));
		}
		for (auto &grouping_function : aggr.grouping_functions) {
			projection_expressions.push_back(
			    make_uniq<BoundConstantExpression>(Value::BIGINT(GetGroupingValue(grouping_set, grouping_function))));
		}

		auto projection =
		    make_uniq<LogicalProjection>(optimizer.binder.GenerateTableIndex(), std::move(projection_expressions));
		projection->children.push_back(std::move(branch_aggregate));
		branches[grouping_set_idx] = std::move(projection);
	}

	const idx_t column_count = group_count + aggregate_count + aggr.grouping_functions.size();
	const auto union_index = optimizer.binder.GenerateTableIndex();
	unique_ptr<LogicalOperator> result = make_uniq<LogicalSetOperation>(union_index, column_count, std::move(branches),
	                                                                    LogicalOperatorType::LOGICAL_UNION, true);
	if (aggr.has_estimated_cardinality) {
		result->SetEstimatedCardinality(aggr.estimated_cardinality);
	}

	auto cte_name = Identifier(StringUtil::Format("__grouping_sets_input_cte_%llu", cte_index.index));
	auto input = LogicalProjection::CreateIdentity(optimizer.binder.GenerateTableIndex(), std::move(op->children[0]));
	result = make_uniq<LogicalMaterializedCTE>(std::move(cte_name), cte_index, input_types.size(), std::move(input),
	                                           std::move(result), CTEMaterialize::CTE_MATERIALIZE_DEFAULT);
	if (aggr.has_estimated_cardinality) {
		result->SetEstimatedCardinality(aggr.estimated_cardinality);
	}

	for (idx_t group_idx = 0; group_idx < group_count; group_idx++) {
		replacement_map[ColumnBinding(aggr.group_index, ProjectionIndex(group_idx))] =
		    ColumnBinding(union_index, ProjectionIndex(group_idx));
	}
	for (idx_t aggregate_idx = 0; aggregate_idx < aggregate_count; aggregate_idx++) {
		replacement_map[ColumnBinding(aggr.aggregate_index, ProjectionIndex(aggregate_idx))] =
		    ColumnBinding(union_index, ProjectionIndex(group_count + aggregate_idx));
	}
	for (idx_t grouping_idx = 0; grouping_idx < aggr.grouping_functions.size(); grouping_idx++) {
		replacement_map[ColumnBinding(aggr.groupings_index, ProjectionIndex(grouping_idx))] =
		    ColumnBinding(union_index, ProjectionIndex(group_count + aggregate_count + grouping_idx));
	}

	result->ResolveOperatorTypes();
	op = std::move(result);
	return true;
}

bool GroupingSetsOptimizer::TryRewriteGroupingSets(unique_ptr<LogicalOperator> &op) {
	if (op->type != LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY || op->children.size() != 1) {
		return false;
	}
	auto &aggr = op->Cast<LogicalAggregate>();
	if (aggr.grouping_sets.size() < 2 || aggr.expressions.empty()) {
		// the rewrite is only beneficial when multiple grouping sets are computed
		return false;
	}
	if (mandatory_aggregate_lowering) {
		for (auto &expr : aggr.expressions) {
			if (expr->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
				return false;
			}
			auto &aggregate = expr->Cast<BoundAggregateExpression>();
			if (aggregate.Function().HasRewriteCallback() &&
			    aggregate.Function().GetRewritePolicy() == AggregateRewritePolicy::MANDATORY &&
			    aggregate.StateExportMode() != AggregateStateExportMode::STATE_EXPORT) {
				return TryExpandGroupingSets(op);
			}
		}
		return false;
	}
	bool can_cascade = true;
	for (auto &expr : aggr.expressions) {
		if (expr->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
			return false;
		}
		if (!CanRewriteAggregate(expr->Cast<BoundAggregateExpression>())) {
			can_cascade = false;
		}
	}
	if (aggr.grouping_sets.size() > MAX_REWRITE_GROUPING_SETS) {
		// the rewritten plan nests a materialized CTE per grouping set - leave large grouping sets to the aggregate
		return false;
	}
	vector<GroupingSetLevel> levels;
	if (!can_cascade || !FindCascade(aggr.grouping_sets, levels)) {
		// Explicit branches support arbitrary aggregates and expose single grouping sets to subsequent rewrites.
		return TryExpandGroupingSets(op);
	}

	const idx_t group_count = aggr.groups.size();
	const idx_t aggregate_count = aggr.expressions.size();

	// build the per-level aggregates
	FunctionBinder function_binder(optimizer.context);
	vector<LogicalType> state_types;
	for (idx_t level_idx = 0; level_idx < levels.size(); level_idx++) {
		auto &level = levels[level_idx];
		auto &grouping_set = aggr.grouping_sets[level.grouping_set_idx];

		vector<unique_ptr<Expression>> level_groups;
		vector<unique_ptr<Expression>> level_aggregates;
		unique_ptr<LogicalOperator> level_child;
		if (level_idx == 0) {
			// the finest level aggregates the base data and exports the aggregate states
			for (auto &group_idx : grouping_set) {
				level_groups.push_back(aggr.groups[group_idx]->Copy());
			}
			// bind the aggregates
			for (auto &expr : aggr.expressions) {
				auto aggregate_copy = unique_ptr_cast<Expression, BoundAggregateExpression>(expr->Copy());
				auto export_aggregate = ExportAggregateFunction::Bind(std::move(aggregate_copy));
				if (!export_aggregate->GetReturnType().IsAggregateState()) {
					return false;
				}
				state_types.push_back(export_aggregate->GetReturnType());
				level_aggregates.push_back(std::move(export_aggregate));
			}
		} else {
			// coarser levels combine the aggregate states of their source level
			auto &source = levels[level.source_level.GetIndex()];
			auto &source_set = aggr.grouping_sets[source.grouping_set_idx];
			const auto cte_ref_index = optimizer.binder.GenerateTableIndex();
			level_child =
			    make_uniq<LogicalCTERef>(cte_ref_index, source.cte_index, source.output_types, source.output_names);

			for (auto &group_idx : grouping_set) {
				const auto group_pos = source.group_positions[group_idx];
				level_groups.push_back(make_uniq<BoundColumnRefExpression>(
				    source.output_types[group_pos], ColumnBinding(cte_ref_index, ProjectionIndex(group_pos))));
			}
			for (idx_t aggr_idx = 0; aggr_idx < aggregate_count; aggr_idx++) {
				const auto state_pos = source_set.size() + aggr_idx;
				vector<unique_ptr<Expression>> arguments;
				arguments.push_back(make_uniq<BoundColumnRefExpression>(
				    state_types[aggr_idx], ColumnBinding(cte_ref_index, ProjectionIndex(state_pos))));
				auto combine_function =
				    GetBuiltinAggregateFunction(optimizer.context, CombineAggrFun::Name, {state_types[aggr_idx]});
				auto combine_aggregate =
				    function_binder.BindAggregateFunction(std::move(combine_function), std::move(arguments));
				if (combine_aggregate->GetReturnType() != state_types[aggr_idx]) {
					return false;
				}
				level_aggregates.push_back(std::move(combine_aggregate));
			}
		}

		// fill in the output layout of this level: the group columns, followed by the aggregate states
		for (auto &group_idx : grouping_set) {
			level.group_positions[group_idx] = level.output_types.size();
			level.output_types.push_back(aggr.groups[group_idx]->GetReturnType());
			level.output_names.push_back(Identifier(StringUtil::Format("group_%llu", group_idx.GetIndex())));
		}
		for (idx_t aggr_idx = 0; aggr_idx < aggregate_count; aggr_idx++) {
			level.output_types.push_back(state_types[aggr_idx]);
			level.output_names.push_back(Identifier(StringUtil::Format("state_%llu", aggr_idx)));
		}

		level.aggregate = make_uniq<LogicalAggregate>(
		    optimizer.binder.GenerateTableIndex(), optimizer.binder.GenerateTableIndex(), std::move(level_aggregates));
		level.aggregate->groups = std::move(level_groups);
		if (aggr.has_estimated_cardinality) {
			level.aggregate->SetEstimatedCardinality(aggr.estimated_cardinality);
		}
		if (level_child) {
			level.aggregate->children.push_back(std::move(level_child));
		}
		if (level.materialized) {
			level.cte_index = optimizer.binder.GenerateTableIndex();
		}
	}

	// build the union branches - one branch per grouping set, in their original order
	vector<unique_ptr<LogicalOperator>> branches(levels.size());
	for (auto &level : levels) {
		auto &grouping_set = aggr.grouping_sets[level.grouping_set_idx];

		// the branch reads from a reference to the CTE if the level is materialized,
		// otherwise the level aggregate is inlined into the branch directly
		unique_ptr<LogicalOperator> branch_child;
		TableIndex cte_ref_index;
		if (level.materialized) {
			cte_ref_index = optimizer.binder.GenerateTableIndex();
			branch_child =
			    make_uniq<LogicalCTERef>(cte_ref_index, level.cte_index, level.output_types, level.output_names);
		} else {
			branch_child = std::move(level.aggregate);
		}
		auto GetBranchBinding = [&](idx_t output_pos) {
			if (level.materialized) {
				return ColumnBinding(cte_ref_index, ProjectionIndex(output_pos));
			}
			auto &branch_aggr = branch_child->Cast<LogicalAggregate>();
			if (output_pos < grouping_set.size()) {
				return ColumnBinding(branch_aggr.group_index, ProjectionIndex(output_pos));
			}
			return ColumnBinding(branch_aggr.aggregate_index, ProjectionIndex(output_pos - grouping_set.size()));
		};

		vector<unique_ptr<Expression>> proj_exprs;
		for (idx_t group_idx = 0; group_idx < group_count; group_idx++) {
			auto &group_type = aggr.groups[group_idx]->GetReturnType();
			auto entry = level.group_positions.find(ProjectionIndex(group_idx));
			if (entry != level.group_positions.end()) {
				proj_exprs.push_back(make_uniq<BoundColumnRefExpression>(group_type, GetBranchBinding(entry->second)));
			} else {
				// this group is not part of the grouping set: emit NULL
				proj_exprs.push_back(make_uniq<BoundConstantExpression>(Value(group_type)));
			}
		}
		for (idx_t aggr_idx = 0; aggr_idx < aggregate_count; aggr_idx++) {
			auto state_ref = make_uniq<BoundColumnRefExpression>(state_types[aggr_idx],
			                                                     GetBranchBinding(grouping_set.size() + aggr_idx));
			auto finalize_expr = optimizer.BindScalarFunction("finalize", std::move(state_ref));
			if (finalize_expr->GetReturnType() != aggr.expressions[aggr_idx]->GetReturnType()) {
				return false;
			}
			proj_exprs.push_back(std::move(finalize_expr));
		}
		// GROUPING() function calls are constant within a grouping set (see RadixPartitionedHashTable)
		for (auto &grouping_function : aggr.grouping_functions) {
			int64_t grouping_value = 0;
			for (idx_t i = 0; i < grouping_function.size(); i++) {
				if (grouping_set.find(grouping_function[i]) == grouping_set.end()) {
					// we do not group on this column in this grouping set
					grouping_value += 1LL << (grouping_function.size() - (i + 1));
				}
			}
			proj_exprs.push_back(make_uniq<BoundConstantExpression>(Value::BIGINT(grouping_value)));
		}

		auto branch = make_uniq<LogicalProjection>(optimizer.binder.GenerateTableIndex(), std::move(proj_exprs));
		branch->children.push_back(std::move(branch_child));
		branches[level.grouping_set_idx] = std::move(branch);
	}

	// from here on the rewrite can no longer fail - we can start modifying the original plan
	// attach the base input to the finest level
	levels[0].aggregate->children.push_back(std::move(aggr.children[0]));

	// union the branches together
	const idx_t column_count = group_count + aggregate_count + aggr.grouping_functions.size();
	const auto union_index = optimizer.binder.GenerateTableIndex();
	unique_ptr<LogicalOperator> result = make_uniq<LogicalSetOperation>(union_index, column_count, std::move(branches),
	                                                                    LogicalOperatorType::LOGICAL_UNION, true);
	if (aggr.has_estimated_cardinality) {
		result->SetEstimatedCardinality(aggr.estimated_cardinality);
	}

	// wrap the result in the materialized CTEs, finest level outermost so that coarser levels can reference it
	for (idx_t level_idx = levels.size(); level_idx > 0; level_idx--) {
		auto &level = levels[level_idx - 1];
		if (!level.materialized) {
			continue;
		}
		auto cte_name = Identifier(StringUtil::Format("__grouping_sets_cte_%llu", level.cte_index.index));
		// cap the aggregate with a projection so later binding remaps cannot change the CTE's column layout
		auto cte_definition =
		    LogicalProjection::CreateIdentity(optimizer.binder.GenerateTableIndex(), std::move(level.aggregate));
		auto cte = make_uniq<LogicalMaterializedCTE>(std::move(cte_name), level.cte_index, level.output_types.size(),
		                                             std::move(cte_definition), std::move(result),
		                                             CTEMaterialize::CTE_MATERIALIZE_DEFAULT);
		if (aggr.has_estimated_cardinality) {
			cte->SetEstimatedCardinality(aggr.estimated_cardinality);
		}
		result = std::move(cte);
	}

	// replace the bindings of the original aggregate with the union output
	for (idx_t group_idx = 0; group_idx < group_count; group_idx++) {
		replacement_map[ColumnBinding(aggr.group_index, ProjectionIndex(group_idx))] =
		    ColumnBinding(union_index, ProjectionIndex(group_idx));
	}
	for (idx_t aggr_idx = 0; aggr_idx < aggregate_count; aggr_idx++) {
		replacement_map[ColumnBinding(aggr.aggregate_index, ProjectionIndex(aggr_idx))] =
		    ColumnBinding(union_index, ProjectionIndex(group_count + aggr_idx));
	}
	for (idx_t grouping_idx = 0; grouping_idx < aggr.grouping_functions.size(); grouping_idx++) {
		replacement_map[ColumnBinding(aggr.groupings_index, ProjectionIndex(grouping_idx))] =
		    ColumnBinding(union_index, ProjectionIndex(group_count + aggregate_count + grouping_idx));
	}

	result->ResolveOperatorTypes();
	op = std::move(result);
	return true;
}

void GroupingSetsOptimizer::VisitOperator(unique_ptr<LogicalOperator> &op) {
	LogicalOperatorVisitor::VisitOperator(op);
	TryRewriteGroupingSets(op);
}

unique_ptr<Expression> GroupingSetsOptimizer::VisitReplace(BoundColumnRefExpression &expr,
                                                           unique_ptr<Expression> *expr_ptr) {
	auto entry = replacement_map.find(expr.Binding());
	if (entry != replacement_map.end()) {
		expr.BindingMutable() = entry->second;
	}
	return nullptr;
}

} // namespace duckdb
