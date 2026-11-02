#include "duckdb/optimizer/multi_stage_aggregate_rewriter.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/function/aggregate/distributive_function_utils.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/optimizer/aggregate_rewrite_helper.hpp"
#include "duckdb/optimizer/aggregate_rewrite.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/bound_result_modifier.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_cross_product.hpp"
#include "duckdb/planner/operator/logical_materialized_cte.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

namespace duckdb {

MultiStageAggregateRewriter::MultiStageAggregateRewriter(
    Optimizer &optimizer_p, AggregateRewritePolicy policy_p, bool rewrite_distinct_p,
    optional_ptr<const column_binding_map_t<unique_ptr<BaseStatistics>>> statistics_p)
    : optimizer(optimizer_p), policy(policy_p), rewrite_distinct(rewrite_distinct_p), statistics(statistics_p) {
}

namespace {

struct DistinctAggregateSet {
	DistinctAggregateSet(idx_t source_index, vector<unique_ptr<Expression>> keys_p)
	    : source_index(source_index), keys(std::move(keys_p)) {
		aggregate_indices.push_back(source_index);
	}

	idx_t source_index;
	vector<idx_t> aggregate_indices;
	//! Actual deduplication keys, including collations unless the binder already applied them.
	vector<unique_ptr<Expression>> keys;
	vector<unique_ptr<Expression>> order_expressions;
};

struct AggregateRewriteSet {
	AggregateRewriteSet(idx_t source_index, unique_ptr<AggregateRewritePlan> plan_p) : plan(std::move(plan_p)) {
		aggregate_indices.push_back(source_index);
		results.push_back(std::move(plan->result));
	}

	unique_ptr<AggregateRewritePlan> plan;
	vector<idx_t> aggregate_indices;
	vector<unique_ptr<Expression>> results;
};

struct BranchResult {
	unique_ptr<LogicalOperator> plan;
	TableIndex table_index;
	vector<idx_t> aggregate_indices;
};

static optional_idx FindExpression(const vector<unique_ptr<Expression>> &expressions, const Expression &needle) {
	for (idx_t expr_idx = 0; expr_idx < expressions.size(); expr_idx++) {
		if (Expression::Equals(*expressions[expr_idx], needle)) {
			return expr_idx;
		}
	}
	return optional_idx();
}

//! Expression equality ignores the return type, but a collation lives in the return type and decides which
//! argument values are duplicates of each other. Two distinct aggregates can only share a deduplication branch
//! if their arguments agree on that as well.
static bool ArgumentsEqualIncludingCollation(const BoundAggregateExpression &lhs, const BoundAggregateExpression &rhs) {
	auto &left = lhs.GetChildren();
	auto &right = rhs.GetChildren();
	if (left.size() != right.size()) {
		return false;
	}
	for (idx_t i = 0; i < left.size(); i++) {
		if (!Expression::Equals(*left[i], *right[i]) ||
		    !left[i]->GetReturnType().EqualsIncludingCollation(right[i]->GetReturnType())) {
			return false;
		}
	}
	return true;
}

static void AddOrderExpressions(DistinctAggregateSet &set, const BoundAggregateExpression &aggregate) {
	if (!aggregate.GetOrderBys()) {
		return;
	}
	for (auto &order : aggregate.GetOrderBys()->orders) {
		if (FindExpression(aggregate.GetChildren(), *order.expression).IsValid()) {
			continue;
		}
		if (!FindExpression(set.order_expressions, *order.expression).IsValid()) {
			set.order_expressions.push_back(order.expression->Copy());
		}
	}
}

static unique_ptr<BoundAggregateExpression> CreateFinalAggregate(const BoundAggregateExpression &source,
                                                                 const DistinctAggregateSet &set,
                                                                 TableIndex input_table, idx_t order_column_offset,
                                                                 const vector<ColumnBinding> &value_columns,
                                                                 optional_idx filter_column_offset = optional_idx()) {
	auto result = unique_ptr_cast<Expression, BoundAggregateExpression>(source.Copy());
	result->GetAggregateTypeMutable() = AggregateType::NON_DISTINCT;
	result->GetChildrenMutable().clear();
	for (idx_t child_idx = 0; child_idx < source.GetChildren().size(); child_idx++) {
		auto &child = source.GetChildren()[child_idx];
		auto &value_column = value_columns[child_idx];
		result->GetChildrenMutable().push_back(
		    make_uniq<BoundColumnRefExpression>(child->GetReturnType(), value_column));
	}
	if (source.GetOrderBys()) {
		result->GetOrderBysMutable() = make_uniq<BoundOrderModifier>();
		for (auto &order : source.GetOrderBys()->orders) {
			auto order_idx = FindExpression(source.GetChildren(), *order.expression);
			ColumnBinding order_binding;
			if (order_idx.IsValid()) {
				// the ORDER BY is one of the arguments - sort on the value of that argument
				order_binding = value_columns[order_idx.GetIndex()];
			} else {
				order_idx = FindExpression(set.order_expressions, *order.expression);
				D_ASSERT(order_idx.IsValid());
				order_binding = ColumnBinding(input_table, ProjectionIndex(order_column_offset + order_idx.GetIndex()));
			}
			result->GetOrderBysMutable()->orders.emplace_back(
			    order.type, order.null_order,
			    make_uniq<BoundColumnRefExpression>(order.expression->GetReturnType(), order_binding));
		}
	} else {
		result->GetOrderBysMutable().reset();
	}
	if (filter_column_offset.IsValid()) {
		result->GetFilterMutable() = make_uniq<BoundColumnRefExpression>(
		    LogicalType::BOOLEAN, ColumnBinding(input_table, ProjectionIndex(filter_column_offset.GetIndex())));
	} else {
		result->GetFilterMutable().reset();
	}
	return result;
}

static unique_ptr<LogicalOperator> CreateProjection(Optimizer &optimizer, unique_ptr<LogicalOperator> child,
                                                    TableIndex child_group_index, TableIndex child_aggregate_index,
                                                    const vector<unique_ptr<Expression>> &groups,
                                                    const vector<unique_ptr<Expression>> &aggregates,
                                                    const vector<idx_t> &aggregate_indices,
                                                    TableIndex &projection_index) {
	vector<unique_ptr<Expression>> projection_expressions;
	projection_expressions.reserve(groups.size() + aggregate_indices.size());
	for (idx_t group_idx = 0; group_idx < groups.size(); group_idx++) {
		projection_expressions.push_back(make_uniq<BoundColumnRefExpression>(
		    groups[group_idx]->GetReturnType(), ColumnBinding(child_group_index, ProjectionIndex(group_idx))));
	}
	for (idx_t aggregate_idx = 0; aggregate_idx < aggregate_indices.size(); aggregate_idx++) {
		auto &source = aggregates[aggregate_indices[aggregate_idx]];
		projection_expressions.push_back(make_uniq<BoundColumnRefExpression>(
		    source->GetReturnType(), ColumnBinding(child_aggregate_index, ProjectionIndex(aggregate_idx))));
	}
	projection_index = optimizer.binder.GenerateTableIndex();
	auto projection = make_uniq<LogicalProjection>(projection_index, std::move(projection_expressions));
	projection->children.push_back(std::move(child));
	return std::move(projection);
}

static BranchResult CreateDistinctBranch(Optimizer &optimizer, LogicalAggregate &aggr, const DistinctAggregateSet &set,
                                         unique_ptr<LogicalOperator> input,
                                         const column_binding_map_t<ColumnBinding> &input_replacements) {
	const idx_t group_count = aggr.groups.size();
	auto &source_aggregate = aggr.expressions[set.source_index]->Cast<BoundAggregateExpression>();

	vector<unique_ptr<Expression>> distinct_groups;
	distinct_groups.reserve(group_count + source_aggregate.GetChildren().size() + set.order_expressions.size() +
	                        (source_aggregate.GetFilter() ? 1 : 0));
	for (auto &group : aggr.groups) {
		distinct_groups.push_back(AggregateRewriteHelper::CopyAndRebind(*group, input_replacements));
	}
	auto distinct_group_index = optimizer.binder.GenerateTableIndex();
	auto distinct_aggregate_index = optimizer.binder.GenerateTableIndex();

	// Group on comparison keys, but pass original values to the final aggregate. When these differ,
	// FIRST carries a representative, just as for collated GROUP BY.
	vector<unique_ptr<Expression>> distinct_aggregates;
	vector<ColumnBinding> value_columns;
	value_columns.reserve(source_aggregate.GetChildren().size());
	FunctionBinder function_binder(optimizer.GetContext());
	for (idx_t child_idx = 0; child_idx < source_aggregate.GetChildren().size(); child_idx++) {
		auto &child = source_aggregate.GetChildren()[child_idx];
		auto &key = set.keys[child_idx];
		auto value_binding = ColumnBinding(distinct_group_index, ProjectionIndex(distinct_groups.size()));
		if (!Expression::Equals(*child, *key)) {
			vector<unique_ptr<Expression>> first_children;
			first_children.push_back(AggregateRewriteHelper::CopyAndRebind(*child, input_replacements));
			auto first_aggregate =
			    function_binder.BindAggregateFunction(FirstFunctionGetter::GetFunction(child->GetReturnType()),
			                                          std::move(first_children), nullptr, AggregateType::NON_DISTINCT);
			value_binding = ColumnBinding(distinct_aggregate_index, ProjectionIndex(distinct_aggregates.size()));
			distinct_aggregates.push_back(std::move(first_aggregate));
		}
		value_columns.push_back(value_binding);
		distinct_groups.push_back(AggregateRewriteHelper::CopyAndRebind(*key, input_replacements));
	}
	for (auto &order_expr : set.order_expressions) {
		distinct_groups.push_back(AggregateRewriteHelper::CopyAndRebind(*order_expr, input_replacements));
	}
	optional_idx filter_column_offset;
	if (source_aggregate.GetFilter()) {
		// Keeping FILTER as a deduplication key preserves argument evaluation and groups with no qualifying rows.
		filter_column_offset = distinct_groups.size();
		distinct_groups.push_back(
		    AggregateRewriteHelper::CopyAndRebind(*source_aggregate.GetFilter(), input_replacements));
	}

	auto distinct =
	    make_uniq<LogicalAggregate>(distinct_group_index, distinct_aggregate_index, std::move(distinct_aggregates));
	distinct->groups = std::move(distinct_groups);
	distinct->children.push_back(std::move(input));

	vector<unique_ptr<Expression>> final_groups;
	final_groups.reserve(group_count);
	for (idx_t group_idx = 0; group_idx < group_count; group_idx++) {
		final_groups.push_back(make_uniq<BoundColumnRefExpression>(
		    aggr.groups[group_idx]->GetReturnType(), ColumnBinding(distinct_group_index, ProjectionIndex(group_idx))));
	}

	vector<unique_ptr<Expression>> final_aggregates;
	final_aggregates.reserve(set.aggregate_indices.size());
	const auto order_column_offset = group_count + source_aggregate.GetChildren().size();
	for (auto aggregate_idx : set.aggregate_indices) {
		auto &aggregate = aggr.expressions[aggregate_idx]->Cast<BoundAggregateExpression>();
		final_aggregates.push_back(CreateFinalAggregate(aggregate, set, distinct_group_index, order_column_offset,
		                                                value_columns, filter_column_offset));
	}

	auto final_group_index = optimizer.binder.GenerateTableIndex();
	auto final_aggregate_index = optimizer.binder.GenerateTableIndex();
	auto final = make_uniq<LogicalAggregate>(final_group_index, final_aggregate_index, std::move(final_aggregates));
	final->groups = std::move(final_groups);
	final->children.push_back(std::move(distinct));
	if (aggr.has_estimated_cardinality) {
		final->SetEstimatedCardinality(aggr.estimated_cardinality);
	}

	BranchResult result;
	result.aggregate_indices = set.aggregate_indices;
	result.plan = CreateProjection(optimizer, std::move(final), final_group_index, final_aggregate_index, aggr.groups,
	                               aggr.expressions, result.aggregate_indices, result.table_index);
	return result;
}

static void RebindRewriteExpression(unique_ptr<Expression> &expr,
                                    const column_binding_map_t<ColumnBinding> &replacement_map) {
	if (!expr || replacement_map.empty()) {
		return;
	}
	ExpressionIterator::VisitExpressionMutable<BoundColumnRefExpression>(
	    expr, [&](BoundColumnRefExpression &colref, unique_ptr<Expression> &) {
		    auto entry = replacement_map.find(colref.Binding());
		    if (entry != replacement_map.end()) {
			    colref.BindingMutable() = entry->second;
		    }
	    });
}

static optional_idx FindExpression(const vector<unique_ptr<Expression>> &expressions,
                                   const vector<unique_ptr<Expression>> &additional, const Expression &needle) {
	auto result = FindExpression(expressions, needle);
	if (result.IsValid()) {
		return result;
	}
	for (idx_t expr_idx = 0; expr_idx < additional.size(); expr_idx++) {
		if (Expression::Equals(*additional[expr_idx], needle)) {
			return optional_idx(expressions.size() + expr_idx);
		}
	}
	return optional_idx();
}

static bool TryMergeRewrite(AggregateRewriteSet &target, idx_t source_index, AggregateRewritePlan &candidate) {
	if (target.plan->stages.size() != candidate.stages.size() || target.plan->result_stage != candidate.result_stage) {
		return false;
	}

	column_binding_map_t<ColumnBinding> replacements;
	vector<vector<unique_ptr<Expression>>> additions(candidate.stages.size());
	for (idx_t stage_idx = 0; stage_idx < candidate.stages.size(); stage_idx++) {
		auto &target_stage = target.plan->stages[stage_idx];
		auto &candidate_stage = candidate.stages[stage_idx];
		if (target_stage.groups.size() != candidate_stage.groups.size() ||
		    target_stage.sources != candidate_stage.sources) {
			return false;
		}
		for (idx_t group_idx = 0; group_idx < candidate_stage.groups.size(); group_idx++) {
			auto group = candidate_stage.groups[group_idx]->Copy();
			RebindRewriteExpression(group, replacements);
			if (!Expression::Equals(*target_stage.groups[group_idx], *group)) {
				return false;
			}
			replacements[ColumnBinding(candidate_stage.group_index, ProjectionIndex(group_idx))] =
			    ColumnBinding(target_stage.group_index, ProjectionIndex(group_idx));
		}
		for (idx_t aggregate_idx = 0; aggregate_idx < candidate_stage.aggregates.size(); aggregate_idx++) {
			auto aggregate = candidate_stage.aggregates[aggregate_idx]->Copy();
			RebindRewriteExpression(aggregate, replacements);
			auto target_idx = FindExpression(target_stage.aggregates, additions[stage_idx], *aggregate);
			if (!target_idx.IsValid()) {
				target_idx = target_stage.aggregates.size() + additions[stage_idx].size();
				additions[stage_idx].push_back(std::move(aggregate));
			}
			replacements[ColumnBinding(candidate_stage.aggregate_index, ProjectionIndex(aggregate_idx))] =
			    ColumnBinding(target_stage.aggregate_index, ProjectionIndex(target_idx.GetIndex()));
		}
	}

	auto result = std::move(candidate.result);
	RebindRewriteExpression(result, replacements);
	for (idx_t stage_idx = 0; stage_idx < additions.size(); stage_idx++) {
		for (auto &aggregate : additions[stage_idx]) {
			target.plan->stages[stage_idx].aggregates.push_back(std::move(aggregate));
		}
	}
	target.aggregate_indices.push_back(source_index);
	target.results.push_back(std::move(result));
	return true;
}

static BranchResult CreateRewriteBranch(Optimizer &optimizer, LogicalAggregate &aggr, AggregateRewriteSet &set,
                                        unique_ptr<LogicalOperator> input,
                                        const column_binding_map_t<ColumnBinding> &input_replacements) {
	if (set.plan->stages.empty() || set.plan->result_stage >= set.plan->stages.size()) {
		throw InternalException("Invalid aggregate rewrite plan");
	}

	for (auto &stage : set.plan->stages) {
		for (auto &group : stage.groups) {
			RebindRewriteExpression(group, input_replacements);
		}
		for (auto &aggregate : stage.aggregates) {
			RebindRewriteExpression(aggregate, input_replacements);
		}
	}
	for (auto &result : set.results) {
		if (!result) {
			throw InternalException("Aggregate rewrite plan has no result expression");
		}
		RebindRewriteExpression(result, input_replacements);
	}

	idx_t input_consumers = 0;
	vector<idx_t> stage_consumers(set.plan->stages.size(), 0);
	for (idx_t stage_idx = 0; stage_idx < set.plan->stages.size(); stage_idx++) {
		auto &stage = set.plan->stages[stage_idx];
		if (stage.sources.empty()) {
			throw InternalException("Aggregate rewrite stage has no source");
		}
		if (stage.groups.size() < aggr.groups.size()) {
			throw InternalException("Aggregate rewrite stage does not preserve the original groups");
		}
		for (idx_t group_idx = 0; group_idx < aggr.groups.size(); group_idx++) {
			if (stage.groups[group_idx]->GetReturnType() != aggr.groups[group_idx]->GetReturnType()) {
				throw InternalException("Aggregate rewrite stage changed an original group type");
			}
		}
		for (auto &source : stage.sources) {
			if (source.type == AggregateRewriteSourceType::INPUT) {
				input_consumers++;
				if (stage.sources.size() != 1) {
					throw InternalException("Aggregate rewrite input cannot be joined directly to another source");
				}
			} else if (source.stage_index >= stage_idx) {
				throw InternalException("Aggregate rewrite plan is not topologically ordered");
			} else {
				stage_consumers[source.stage_index]++;
			}
		}
	}
	stage_consumers[set.plan->result_stage]++;
	for (auto count : stage_consumers) {
		if (count == 0) {
			throw InternalException("Aggregate rewrite plan contains an unused stage");
		}
	}

	input->ResolveOperatorTypes();
	auto input_types = input->types;
	auto input_bindings = input->GetColumnBindings();
	auto input_names = AggregateRewriteHelper::GenerateColumnNames("__aggregate_rewrite_input", input_types.size());
	optional_idx input_cte_index;
	if (input_consumers > 1) {
		input_cte_index = optimizer.binder.GenerateTableIndex().index;
		input = LogicalProjection::CreateIdentity(optimizer.binder.GenerateTableIndex(), std::move(input));
	}

	struct StageOutput {
		unique_ptr<LogicalOperator> plan;
		vector<LogicalType> types;
		vector<Identifier> names;
		vector<ColumnBinding> bindings;
		optional_idx cte_index;
	};
	vector<StageOutput> outputs(set.plan->stages.size());

	for (idx_t stage_idx = 0; stage_idx < set.plan->stages.size(); stage_idx++) {
		auto &stage = set.plan->stages[stage_idx];
		vector<unique_ptr<LogicalOperator>> sources;
		vector<vector<ColumnBinding>> source_groups;
		for (auto &source : stage.sources) {
			column_binding_map_t<ColumnBinding> replacements;
			if (source.type == AggregateRewriteSourceType::INPUT) {
				if (input_cte_index.IsValid()) {
					sources.push_back(
					    AggregateRewriteHelper::CreateCTERef(optimizer, TableIndex(input_cte_index.GetIndex()),
					                                         input_types, input_names, input_bindings, replacements));
				} else {
					// The validation above guarantees that the input is moved exactly once when no CTE is needed.
					sources.push_back(std::move(input)); // NOLINT(bugprone-use-after-move)
				}
			} else {
				auto &source_stage = set.plan->stages[source.stage_index];
				auto &source_output = outputs[source.stage_index];
				if (source_output.cte_index.IsValid()) {
					sources.push_back(AggregateRewriteHelper::CreateCTERef(
					    optimizer, TableIndex(source_output.cte_index.GetIndex()), source_output.types,
					    source_output.names, source_output.bindings, replacements));
					auto bindings = sources.back()->GetColumnBindings();
					source_groups.emplace_back(
					    bindings.begin(),
					    bindings.begin() + NumericCast<vector<ColumnBinding>::difference_type>(aggr.groups.size()));
				} else {
					sources.push_back(std::move(source_output.plan));
					vector<ColumnBinding> groups;
					groups.reserve(aggr.groups.size());
					for (idx_t group_idx = 0; group_idx < aggr.groups.size(); group_idx++) {
						groups.emplace_back(source_stage.group_index, ProjectionIndex(group_idx));
					}
					source_groups.push_back(std::move(groups));
				}
			}
			for (auto &group : stage.groups) {
				RebindRewriteExpression(group, replacements);
			}
			for (auto &aggregate : stage.aggregates) {
				RebindRewriteExpression(aggregate, replacements);
			}
		}

		unique_ptr<LogicalOperator> current = std::move(sources[0]);
		if (sources.size() > 1) {
			D_ASSERT(source_groups.size() == sources.size());
			if (aggr.groups.empty()) {
				for (idx_t source_idx = 1; source_idx < sources.size(); source_idx++) {
					current = LogicalCrossProduct::Create(std::move(current), std::move(sources[source_idx]));
				}
			} else {
				for (idx_t source_idx = 1; source_idx < sources.size(); source_idx++) {
					auto join = make_uniq<LogicalComparisonJoin>(JoinType::INNER);
					for (idx_t group_idx = 0; group_idx < aggr.groups.size(); group_idx++) {
						auto left = make_uniq<BoundColumnRefExpression>(aggr.groups[group_idx]->GetReturnType(),
						                                                source_groups[0][group_idx]);
						auto right = make_uniq<BoundColumnRefExpression>(aggr.groups[group_idx]->GetReturnType(),
						                                                 source_groups[source_idx][group_idx]);
						join->conditions.emplace_back(std::move(left), std::move(right),
						                              ExpressionType::COMPARE_NOT_DISTINCT_FROM);
					}
					join->children.push_back(std::move(current));
					join->children.push_back(std::move(sources[source_idx]));
					current = std::move(join);
				}
			}
		}

		auto &output = outputs[stage_idx];
		output.types.reserve(stage.groups.size() + stage.aggregates.size());
		output.bindings.reserve(stage.groups.size() + stage.aggregates.size());
		for (idx_t group_idx = 0; group_idx < stage.groups.size(); group_idx++) {
			output.types.push_back(stage.groups[group_idx]->GetReturnType());
			output.bindings.emplace_back(stage.group_index, ProjectionIndex(group_idx));
		}
		for (idx_t aggregate_idx = 0; aggregate_idx < stage.aggregates.size(); aggregate_idx++) {
			output.types.push_back(stage.aggregates[aggregate_idx]->GetReturnType());
			output.bindings.emplace_back(stage.aggregate_index, ProjectionIndex(aggregate_idx));
		}
		output.names = AggregateRewriteHelper::GenerateColumnNames("__aggregate_rewrite_stage", output.types.size());
		if (stage_consumers[stage_idx] > 1) {
			output.cte_index = optimizer.binder.GenerateTableIndex().index;
		}

		auto aggregate =
		    make_uniq<LogicalAggregate>(stage.group_index, stage.aggregate_index, std::move(stage.aggregates));
		aggregate->groups = std::move(stage.groups);
		aggregate->children.push_back(std::move(current));
		if (stage_idx == set.plan->result_stage && aggr.has_estimated_cardinality) {
			aggregate->SetEstimatedCardinality(aggr.estimated_cardinality);
		}
		output.plan = std::move(aggregate);
	}

	auto &final_output = outputs[set.plan->result_stage];
	column_binding_map_t<ColumnBinding> final_replacements;
	unique_ptr<LogicalOperator> current;
	vector<ColumnBinding> final_bindings;
	if (final_output.cte_index.IsValid()) {
		current = AggregateRewriteHelper::CreateCTERef(optimizer, TableIndex(final_output.cte_index.GetIndex()),
		                                               final_output.types, final_output.names, final_output.bindings,
		                                               final_replacements);
		final_bindings = current->GetColumnBindings();
		for (auto &result : set.results) {
			RebindRewriteExpression(result, final_replacements);
		}
	} else {
		current = std::move(final_output.plan);
		final_bindings = final_output.bindings;
	}

	vector<unique_ptr<Expression>> projection_expressions;
	projection_expressions.reserve(aggr.groups.size() + set.results.size());
	for (idx_t group_idx = 0; group_idx < aggr.groups.size(); group_idx++) {
		projection_expressions.push_back(
		    make_uniq<BoundColumnRefExpression>(aggr.groups[group_idx]->GetReturnType(), final_bindings[group_idx]));
	}
	for (idx_t result_idx = 0; result_idx < set.results.size(); result_idx++) {
		D_ASSERT(set.results[result_idx]->GetReturnType() ==
		         aggr.expressions[set.aggregate_indices[result_idx]]->GetReturnType());
		projection_expressions.push_back(std::move(set.results[result_idx]));
	}

	BranchResult result;
	result.aggregate_indices = set.aggregate_indices;
	result.table_index = optimizer.binder.GenerateTableIndex();
	auto projection = make_uniq<LogicalProjection>(result.table_index, std::move(projection_expressions));
	projection->children.push_back(std::move(current));
	result.plan = std::move(projection);

	for (idx_t stage_idx = outputs.size(); stage_idx > 0; stage_idx--) {
		auto &output = outputs[stage_idx - 1];
		if (!output.cte_index.IsValid()) {
			continue;
		}
		auto cte_name = Identifier(StringUtil::Format("__aggregate_rewrite_stage_%llu", output.cte_index.GetIndex()));
		result.plan = make_uniq<LogicalMaterializedCTE>(
		    std::move(cte_name), TableIndex(output.cte_index.GetIndex()), output.types.size(), std::move(output.plan),
		    std::move(result.plan), CTEMaterialize::CTE_MATERIALIZE_DEFAULT);
	}
	if (input_cte_index.IsValid()) {
		auto cte_name = Identifier(StringUtil::Format("__aggregate_rewrite_input_%llu", input_cte_index.GetIndex()));
		result.plan = make_uniq<LogicalMaterializedCTE>(std::move(cte_name), TableIndex(input_cte_index.GetIndex()),
		                                                input_types.size(), std::move(input), std::move(result.plan),
		                                                CTEMaterialize::CTE_MATERIALIZE_DEFAULT);
	}
	return result;
}

static BranchResult CreateRegularBranch(Optimizer &optimizer, LogicalAggregate &aggr,
                                        const vector<idx_t> &aggregate_indices, unique_ptr<LogicalOperator> input,
                                        const column_binding_map_t<ColumnBinding> &input_replacements) {
	vector<unique_ptr<Expression>> regular_groups;
	regular_groups.reserve(aggr.groups.size());
	for (auto &group : aggr.groups) {
		regular_groups.push_back(AggregateRewriteHelper::CopyAndRebind(*group, input_replacements));
	}

	vector<unique_ptr<Expression>> regular_aggregates;
	regular_aggregates.reserve(aggregate_indices.size());
	for (auto aggregate_idx : aggregate_indices) {
		regular_aggregates.push_back(
		    AggregateRewriteHelper::CopyAndRebind(*aggr.expressions[aggregate_idx], input_replacements));
	}

	auto regular_group_index = optimizer.binder.GenerateTableIndex();
	auto regular_aggregate_index = optimizer.binder.GenerateTableIndex();
	auto regular =
	    make_uniq<LogicalAggregate>(regular_group_index, regular_aggregate_index, std::move(regular_aggregates));
	regular->groups = std::move(regular_groups);
	regular->children.push_back(std::move(input));
	if (aggr.has_estimated_cardinality) {
		regular->SetEstimatedCardinality(aggr.estimated_cardinality);
	}

	BranchResult result;
	result.aggregate_indices = aggregate_indices;
	result.plan = CreateProjection(optimizer, std::move(regular), regular_group_index, regular_aggregate_index,
	                               aggr.groups, aggr.expressions, result.aggregate_indices, result.table_index);
	return result;
}

static unique_ptr<LogicalOperator> JoinBranches(const vector<BranchResult> &branches,
                                                vector<unique_ptr<LogicalOperator>> branch_plans,
                                                const vector<unique_ptr<Expression>> &groups) {
	D_ASSERT(!branch_plans.empty());
	if (groups.empty()) {
		// Ungrouped aggregate branches each produce one row, including for empty inputs.
		auto result = std::move(branch_plans[0]);
		for (idx_t branch_idx = 1; branch_idx < branch_plans.size(); branch_idx++) {
			result = LogicalCrossProduct::Create(std::move(result), std::move(branch_plans[branch_idx]));
		}
		return result;
	}

	// Every branch consumes the same rows and retains every original group; null-safe inner joins preserve NULL keys.
	auto result = std::move(branch_plans[0]);
	const auto anchor_table = branches[0].table_index;
	for (idx_t branch_idx = 1; branch_idx < branch_plans.size(); branch_idx++) {
		auto join = make_uniq<LogicalComparisonJoin>(JoinType::INNER);
		for (idx_t group_idx = 0; group_idx < groups.size(); group_idx++) {
			auto left = make_uniq<BoundColumnRefExpression>(groups[group_idx]->GetReturnType(),
			                                                ColumnBinding(anchor_table, ProjectionIndex(group_idx)));
			auto right = make_uniq<BoundColumnRefExpression>(
			    groups[group_idx]->GetReturnType(),
			    ColumnBinding(branches[branch_idx].table_index, ProjectionIndex(group_idx)));
			join->conditions.emplace_back(std::move(left), std::move(right), ExpressionType::COMPARE_NOT_DISTINCT_FROM);
		}
		join->children.push_back(std::move(result));
		join->children.push_back(std::move(branch_plans[branch_idx]));
		result = std::move(join);
	}
	return result;
}

} // namespace

bool MultiStageAggregateRewriter::ShouldRewrite(const BoundAggregateExpression &aggregate,
                                                const LogicalAggregate &op) const {
	if (!aggregate.Function().HasRewriteCallback() ||
	    aggregate.StateExportMode() == AggregateStateExportMode::STATE_EXPORT ||
	    aggregate.Function().GetRewritePolicy() != policy) {
		return false;
	}
	if (policy == AggregateRewritePolicy::MANDATORY) {
		return true;
	}
	const auto optimizer_type = aggregate.Function().GetRewriteOptimizerType();
	if (optimizer_type == OptimizerType::INVALID || optimizer.OptimizerDisabled(optimizer_type)) {
		return false;
	}
	if (policy != AggregateRewritePolicy::COST_BASED) {
		return true;
	}
	if (!statistics || !aggregate.Function().GetRewriteCostCallback()) {
		return false;
	}

	optional_idx input_cardinality;
	if (!op.children.empty() && op.children[0]->has_estimated_cardinality) {
		input_cardinality = op.children[0]->estimated_cardinality;
	}
	vector<optional_ptr<const BaseStatistics>> argument_statistics;
	argument_statistics.reserve(aggregate.GetChildren().size());
	for (auto &child : aggregate.GetChildren()) {
		optional_ptr<const BaseStatistics> child_statistics;
		if (child->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
			auto entry = statistics->find(child->Cast<BoundColumnRefExpression>().Binding());
			if (entry != statistics->end()) {
				child_statistics = entry->second.get();
			}
		}
		argument_statistics.push_back(child_statistics);
	}
	AggregateRewriteInput rewrite_input(optimizer, op, aggregate);
	AggregateRewriteCostInput cost_input(rewrite_input, input_cardinality, std::move(argument_statistics));
	return aggregate.Function().GetRewriteCostCallback()(cost_input);
}

bool MultiStageAggregateRewriter::TryRewrite(unique_ptr<LogicalOperator> &op) {
	if (op->type != LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY || op->children.size() != 1) {
		return false;
	}
	auto &aggr = op->Cast<LogicalAggregate>();
	if (aggr.grouping_sets.size() > 1 || aggr.expressions.empty()) {
		return false;
	}
	if (aggr.grouping_sets.size() == 1) {
		if (aggr.grouping_sets[0].size() != aggr.groups.size()) {
			return false;
		}
		for (idx_t group_idx = 0; group_idx < aggr.groups.size(); group_idx++) {
			if (aggr.grouping_sets[0].find(ProjectionIndex(group_idx)) == aggr.grouping_sets[0].end()) {
				return false;
			}
		}
	}

	bool has_rewrite = false;
	vector<bool> rewrite_candidates(aggr.expressions.size(), false);
	for (idx_t aggregate_idx = 0; aggregate_idx < aggr.expressions.size(); aggregate_idx++) {
		auto &expr = aggr.expressions[aggregate_idx];
		if (expr->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
			return false;
		}
		auto &aggregate = expr->Cast<BoundAggregateExpression>();
		if (rewrite_distinct && aggregate.IsDistinct()) {
			has_rewrite = true;
			continue;
		}
		if (ShouldRewrite(aggregate, aggr)) {
			rewrite_candidates[aggregate_idx] = true;
			has_rewrite = true;
		}
	}
	if (!has_rewrite) {
		return false;
	}

	AggregateRewriteHelper::StageVolatileAggregateInputs(optimizer, aggr, op->children[0]);

	vector<DistinctAggregateSet> distinct_sets;
	vector<AggregateRewriteSet> rewrite_sets;
	vector<idx_t> regular_aggregates;
	for (idx_t aggregate_idx = 0; aggregate_idx < aggr.expressions.size(); aggregate_idx++) {
		auto &expr = aggr.expressions[aggregate_idx];
		auto &aggregate = expr->Cast<BoundAggregateExpression>();
		if (rewrite_candidates[aggregate_idx]) {
			AggregateRewriteInput rewrite_input(optimizer, aggr, aggregate);
			auto rewrite = aggregate.Function().GetRewriteCallback()(rewrite_input);
			if (!rewrite) {
				throw InternalException("Selected aggregate rewrite did not return a plan");
			}
			bool merged = false;
			for (auto &set : rewrite_sets) {
				if (TryMergeRewrite(set, aggregate_idx, *rewrite)) {
					merged = true;
					break;
				}
			}
			if (!merged) {
				rewrite_sets.emplace_back(aggregate_idx, std::move(rewrite));
			}
			continue;
		}
		if (!rewrite_distinct || !aggregate.IsDistinct()) {
			regular_aggregates.push_back(aggregate_idx);
			continue;
		}
		// Resolve keys before sharing branches: COUNT may already consume keys from the binder, while a
		// value-returning aggregate with the same argument still needs its collation applied.
		vector<unique_ptr<Expression>> keys;
		for (auto &child : aggregate.GetChildren()) {
			auto key = child->Copy();
			if (aggregate.Function().GetCollationHandling() != FunctionCollationHandling::PUSH_DISTINCT_COLLATIONS) {
				ExpressionBinder::PushCollation(optimizer.GetContext(), key, key->GetReturnType());
			}
			keys.push_back(std::move(key));
		}
		bool found_match = false;
		for (auto &set : distinct_sets) {
			auto &other = aggr.expressions[set.source_index]->Cast<BoundAggregateExpression>();
			if (ArgumentsEqualIncludingCollation(aggregate, other) && Expression::ListEquals(keys, set.keys) &&
			    Expression::Equals(aggregate.GetFilter(), other.GetFilter())) {
				set.aggregate_indices.push_back(aggregate_idx);
				AddOrderExpressions(set, aggregate);
				found_match = true;
				break;
			}
		}
		if (!found_match) {
			distinct_sets.emplace_back(aggregate_idx, std::move(keys));
			AddOrderExpressions(distinct_sets.back(), aggregate);
		}
	}
	if (distinct_sets.empty() && rewrite_sets.empty()) {
		return false;
	}

	const bool needs_cte = distinct_sets.size() + rewrite_sets.size() + (regular_aggregates.empty() ? 0 : 1) > 1;
	vector<LogicalType> input_types;
	vector<Identifier> input_names;
	vector<ColumnBinding> input_bindings;
	TableIndex cte_index;
	if (needs_cte) {
		op->children[0]->ResolveOperatorTypes();
		input_types = op->children[0]->types;
		input_names = AggregateRewriteHelper::GenerateColumnNames("__aggregate_input", input_types.size());
		input_bindings = op->children[0]->GetColumnBindings();
		cte_index = optimizer.binder.GenerateTableIndex();
		op->children[0] =
		    LogicalProjection::CreateIdentity(optimizer.binder.GenerateTableIndex(), std::move(op->children[0]));
	}

	vector<BranchResult> branches;
	vector<unique_ptr<LogicalOperator>> branch_plans;
	auto CreateBranchInput = [&](column_binding_map_t<ColumnBinding> &input_replacements) {
		if (!needs_cte) {
			return std::move(op->children[0]);
		}
		return AggregateRewriteHelper::CreateCTERef(optimizer, cte_index, input_types, input_names, input_bindings,
		                                            input_replacements);
	};

	for (auto &set : distinct_sets) {
		column_binding_map_t<ColumnBinding> input_replacements;
		auto branch =
		    CreateDistinctBranch(optimizer, aggr, set, CreateBranchInput(input_replacements), input_replacements);
		branch_plans.push_back(std::move(branch.plan));
		branches.push_back(std::move(branch));
	}
	for (auto &set : rewrite_sets) {
		column_binding_map_t<ColumnBinding> input_replacements;
		auto branch =
		    CreateRewriteBranch(optimizer, aggr, set, CreateBranchInput(input_replacements), input_replacements);
		branch_plans.push_back(std::move(branch.plan));
		branches.push_back(std::move(branch));
	}
	if (!regular_aggregates.empty()) {
		column_binding_map_t<ColumnBinding> input_replacements;
		auto branch = CreateRegularBranch(optimizer, aggr, regular_aggregates, CreateBranchInput(input_replacements),
		                                  input_replacements);
		branch_plans.push_back(std::move(branch.plan));
		branches.push_back(std::move(branch));
	}

	vector<ColumnBinding> aggregate_bindings(aggr.expressions.size());
	for (auto &branch : branches) {
		for (idx_t local_idx = 0; local_idx < branch.aggregate_indices.size(); local_idx++) {
			aggregate_bindings[branch.aggregate_indices[local_idx]] =
			    ColumnBinding(branch.table_index, ProjectionIndex(aggr.groups.size() + local_idx));
		}
	}

	auto joined = JoinBranches(branches, std::move(branch_plans), aggr.groups);

	vector<unique_ptr<Expression>> projection_expressions;
	projection_expressions.reserve(aggr.groups.size() + aggr.expressions.size() + aggr.grouping_functions.size());
	const auto final_projection_index = optimizer.binder.GenerateTableIndex();
	for (idx_t group_idx = 0; group_idx < aggr.groups.size(); group_idx++) {
		projection_expressions.push_back(
		    make_uniq<BoundColumnRefExpression>(aggr.groups[group_idx]->GetReturnType(),
		                                        ColumnBinding(branches[0].table_index, ProjectionIndex(group_idx))));
		replacement_map[ColumnBinding(aggr.group_index, ProjectionIndex(group_idx))] =
		    ColumnBinding(final_projection_index, ProjectionIndex(group_idx));
	}
	for (idx_t aggregate_idx = 0; aggregate_idx < aggr.expressions.size(); aggregate_idx++) {
		projection_expressions.push_back(make_uniq<BoundColumnRefExpression>(
		    aggr.expressions[aggregate_idx]->GetReturnType(), aggregate_bindings[aggregate_idx]));
		replacement_map[ColumnBinding(aggr.aggregate_index, ProjectionIndex(aggregate_idx))] =
		    ColumnBinding(final_projection_index, ProjectionIndex(aggr.groups.size() + aggregate_idx));
	}
	for (idx_t grouping_idx = 0; grouping_idx < aggr.grouping_functions.size(); grouping_idx++) {
		projection_expressions.push_back(make_uniq<BoundConstantExpression>(Value::BIGINT(0)));
		replacement_map[ColumnBinding(aggr.groupings_index, ProjectionIndex(grouping_idx))] = ColumnBinding(
		    final_projection_index, ProjectionIndex(aggr.groups.size() + aggr.expressions.size() + grouping_idx));
	}

	unique_ptr<LogicalOperator> result =
	    make_uniq<LogicalProjection>(final_projection_index, std::move(projection_expressions));
	result->children.push_back(std::move(joined));
	if (aggr.has_estimated_cardinality) {
		result->SetEstimatedCardinality(aggr.estimated_cardinality);
	}

	if (needs_cte) {
		// DEFAULT keeps the shared input eligible for direct streaming fan-out during CTE planning.
		auto cte_name = Identifier(StringUtil::Format("__aggregate_cte_%llu", cte_index.index));
		result = make_uniq<LogicalMaterializedCTE>(std::move(cte_name), cte_index, input_types.size(),
		                                           std::move(op->children[0]), std::move(result),
		                                           CTEMaterialize::CTE_MATERIALIZE_DEFAULT);
		if (aggr.has_estimated_cardinality) {
			result->SetEstimatedCardinality(aggr.estimated_cardinality);
		}
	}

	result->ResolveOperatorTypes();
	op = std::move(result);
	changed = true;
	return true;
}

void MultiStageAggregateRewriter::VisitOperator(unique_ptr<LogicalOperator> &op) {
	LogicalOperatorVisitor::VisitOperator(op);
	TryRewrite(op);
}

bool MultiStageAggregateRewriter::WasChanged() const {
	return changed;
}

unique_ptr<Expression> MultiStageAggregateRewriter::VisitReplace(BoundColumnRefExpression &expr,
                                                                 unique_ptr<Expression> *expr_ptr) {
	auto entry = replacement_map.find(expr.Binding());
	if (entry != replacement_map.end()) {
		expr.BindingMutable() = entry->second;
	}
	return nullptr;
}

} // namespace duckdb
