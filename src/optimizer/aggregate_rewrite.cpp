#include "duckdb/optimizer/aggregate_rewrite.hpp"

#include "duckdb/function/aggregate/distributive_functions.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/function/builtin_function_lookup.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"

namespace duckdb {

AggregateRewriteSource AggregateRewriteSource::Input() {
	return {AggregateRewriteSourceType::INPUT, DConstants::INVALID_INDEX};
}

AggregateRewriteSource AggregateRewriteSource::Stage(idx_t stage_index) {
	return {AggregateRewriteSourceType::STAGE, stage_index};
}

bool AggregateRewriteSource::operator==(const AggregateRewriteSource &other) const {
	return type == other.type && stage_index == other.stage_index;
}

AggregateRewriteStage::AggregateRewriteStage(TableIndex group_index_p, TableIndex aggregate_index_p,
                                             vector<AggregateRewriteSource> sources_p,
                                             vector<unique_ptr<Expression>> groups_p,
                                             vector<unique_ptr<Expression>> aggregates_p)
    : group_index(group_index_p), aggregate_index(aggregate_index_p), sources(std::move(sources_p)),
      groups(std::move(groups_p)), aggregates(std::move(aggregates_p)) {
}

AggregateRewriteInput::AggregateRewriteInput(ClientContext &context_p, const BoundAggregateExpression &aggregate_p)
    : context(context_p), aggregate(aggregate_p) {
}

AggregateRewriteInput::AggregateRewriteInput(Optimizer &optimizer_p, const LogicalAggregate &op_p,
                                             const BoundAggregateExpression &aggregate_p)
    : context(optimizer_p.context), optimizer(optimizer_p), op(op_p), aggregate(aggregate_p) {
}

AggregateRewriteCostInput::AggregateRewriteCostInput(AggregateRewriteInput &rewrite_input_p,
                                                     optional_idx input_cardinality_p,
                                                     vector<optional_ptr<const BaseStatistics>> argument_statistics_p)
    : rewrite_input(rewrite_input_p), input_cardinality(input_cardinality_p),
      argument_statistics(std::move(argument_statistics_p)) {
}

unique_ptr<Expression> TryDirectAggregateRewrite(AggregateRewriteInput &input) {
	if (!input.aggregate.Function().HasDirectRewriteCallback() ||
	    input.aggregate.StateExportMode() == AggregateStateExportMode::STATE_EXPORT) {
		return nullptr;
	}
	auto rewrite = input.aggregate.Function().GetDirectRewriteCallback()(input);
	if (!rewrite) {
		return nullptr;
	}
	D_ASSERT(rewrite->GetReturnType() == input.aggregate.GetReturnType());
	return rewrite;
}

FrequencyAggregateFinalizeInput::FrequencyAggregateFinalizeInput(
    AggregateRewriteInput &rewrite_input_p, TableIndex aggregate_index_p, unique_ptr<Expression> value_p,
    unique_ptr<Expression> frequency_p, unique_ptr<Expression> filter_p, unique_ptr<Expression> order_key_p)
    : rewrite_input(rewrite_input_p), aggregate_index(aggregate_index_p), value(std::move(value_p)),
      frequency(std::move(frequency_p)), filter(std::move(filter_p)), order_key(std::move(order_key_p)) {
}

static unique_ptr<BoundAggregateExpression> BindMinAggregate(ClientContext &context, unique_ptr<Expression> child) {
	auto function = GetBuiltinAggregateFunction(context, MinFun::Name, {child->GetReturnType()});
	FunctionBinder function_binder(context);
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(child));
	return function_binder.BindAggregateFunction(function, std::move(children));
}

static unique_ptr<Expression> CreateAggregateSortKey(ClientContext &context, const BoundOrderModifier &order_bys) {
	vector<unique_ptr<Expression>> sort_children;
	for (auto &order : order_bys.orders) {
		sort_children.push_back(order.expression->Copy());
		sort_children.push_back(make_uniq<BoundConstantExpression>(Value(order.GetOrderModifier())));
	}
	FunctionBinder function_binder(context);
	return function_binder.BindScalarFunction(Identifier::DefaultSchema(), "create_sort_key", std::move(sort_children));
}

unique_ptr<AggregateRewritePlan> FrequencyAggregateRewrite::Create(AggregateRewriteInput &input, bool ignore_nulls,
                                                                   bool retain_order,
                                                                   frequency_aggregate_finalize_t finalize) {
	if (!input.optimizer || !input.op || input.aggregate.GetChildren().size() != 1) {
		return nullptr;
	}
	auto &optimizer = *input.optimizer;
	auto &op = *input.op;

	auto plan = make_uniq<AggregateRewritePlan>();
	const auto group_count = op.groups.size();
	vector<unique_ptr<Expression>> frequency_groups;
	frequency_groups.reserve(group_count + 2);
	for (auto &group : op.groups) {
		frequency_groups.push_back(group->Copy());
	}
	auto frequency_value = input.aggregate.GetChildren()[0]->Copy();
	if (input.aggregate.GetFilter() && frequency_value->CanThrow()) {
		// The frequency stage evaluates the argument for every input row, before the FILTER is applied.
		// Guard arguments that can throw: child -> CASE WHEN <filter> THEN child ELSE NULL END
		auto null_value = make_uniq<BoundConstantExpression>(Value(frequency_value->GetReturnType()));
		frequency_value = make_uniq<BoundCaseExpression>(input.aggregate.GetFilter()->Copy(), std::move(frequency_value),
		                                                 std::move(null_value));
	}
	frequency_groups.push_back(std::move(frequency_value));

	optional_idx filter_column;
	if (input.aggregate.GetFilter()) {
		filter_column = frequency_groups.size();
		frequency_groups.push_back(input.aggregate.GetFilter()->Copy());
	}

	vector<unique_ptr<Expression>> frequency_aggregates;
	optional_idx count_column;
	if (!input.aggregate.IsDistinct()) {
		count_column = frequency_aggregates.size();
		FunctionBinder function_binder(input.context);
		auto count_star = GetBuiltinAggregateFunction(input.context, CountStarFun::Name, {});
		frequency_aggregates.push_back(function_binder.BindAggregateFunction(count_star, {}));
	}

	optional_idx order_column;
	LogicalType order_type = LogicalType::SQLNULL;
	if (retain_order && input.aggregate.GetOrderBys()) {
		order_column = frequency_aggregates.size();
		auto sort_key = CreateAggregateSortKey(input.context, *input.aggregate.GetOrderBys());
		auto min_sort_key = BindMinAggregate(input.context, std::move(sort_key));
		order_type = min_sort_key->GetReturnType();
		frequency_aggregates.push_back(std::move(min_sort_key));
	}

	auto frequency_group_index = optimizer.binder.GenerateTableIndex();
	auto frequency_aggregate_index = optimizer.binder.GenerateTableIndex();
	plan->stages.emplace_back(frequency_group_index, frequency_aggregate_index,
	                          vector<AggregateRewriteSource> {AggregateRewriteSource::Input()},
	                          std::move(frequency_groups), std::move(frequency_aggregates));

	vector<unique_ptr<Expression>> final_groups;
	final_groups.reserve(group_count);
	for (idx_t group_idx = 0; group_idx < group_count; group_idx++) {
		final_groups.push_back(make_uniq<BoundColumnRefExpression>(
		    op.groups[group_idx]->GetReturnType(), ColumnBinding(frequency_group_index, ProjectionIndex(group_idx))));
	}

	auto value =
	    make_uniq<BoundColumnRefExpression>(input.aggregate.GetChildren()[0]->GetReturnType(),
	                                        ColumnBinding(frequency_group_index, ProjectionIndex(group_count)));
	unique_ptr<Expression> frequency;
	if (count_column.IsValid()) {
		frequency = make_uniq<BoundColumnRefExpression>(
		    LogicalType::BIGINT, ColumnBinding(frequency_aggregate_index, ProjectionIndex(count_column.GetIndex())));
	} else {
		frequency = make_uniq<BoundConstantExpression>(Value::BIGINT(1));
	}

	unique_ptr<Expression> filter;
	if (filter_column.IsValid()) {
		filter = make_uniq<BoundColumnRefExpression>(
		    LogicalType::BOOLEAN, ColumnBinding(frequency_group_index, ProjectionIndex(filter_column.GetIndex())));
	}
	if (ignore_nulls) {
		auto not_null = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NOT_NULL, LogicalType::BOOLEAN);
		not_null->GetChildrenMutable().push_back(value->Copy());
		if (filter) {
			filter = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(filter),
			                                               std::move(not_null));
		} else {
			filter = std::move(not_null);
		}
	}

	unique_ptr<Expression> order_key;
	if (order_column.IsValid()) {
		order_key = make_uniq<BoundColumnRefExpression>(
		    order_type, ColumnBinding(frequency_aggregate_index, ProjectionIndex(order_column.GetIndex())));
	}

	auto final_group_index = optimizer.binder.GenerateTableIndex();
	auto final_aggregate_index = optimizer.binder.GenerateTableIndex();
	FrequencyAggregateFinalizeInput finalize_input(input, final_aggregate_index, std::move(value), std::move(frequency),
	                                               std::move(filter), std::move(order_key));
	auto final_result = finalize(finalize_input);
	D_ASSERT(final_result.result);
	plan->stages.emplace_back(final_group_index, final_aggregate_index,
	                          vector<AggregateRewriteSource> {AggregateRewriteSource::Stage(0)},
	                          std::move(final_groups), std::move(final_result.aggregates));
	plan->result_stage = 1;
	plan->result = std::move(final_result.result);
	return plan;
}

} // namespace duckdb
