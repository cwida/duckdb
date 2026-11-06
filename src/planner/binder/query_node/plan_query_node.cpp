#include "duckdb/parser/query_node.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_limit.hpp"
#include "duckdb/planner/operator/logical_order.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_set_operation.hpp"
#include "duckdb/planner/bound_result_modifier.hpp"
#include "duckdb/function/aggregate/distributive_functions.hpp"
#include "duckdb/function/builtin_function_lookup.hpp"
#include "duckdb/function/function_binder.hpp"

namespace duckdb {

//! Gives the plan that produces the result of a query node a new table index, returns the old table index
static TableIndex RenameResultIndex(LogicalOperator &root, TableIndex new_index) {
	reference<LogicalOperator> current(root);
	while (true) {
		auto &op = current.get();
		switch (op.type) {
		case LogicalOperatorType::LOGICAL_PROJECTION: {
			auto &projection = op.Cast<LogicalProjection>();
			auto old_index = projection.table_index;
			projection.table_index = new_index;
			return old_index;
		}
		case LogicalOperatorType::LOGICAL_UNION:
		case LogicalOperatorType::LOGICAL_EXCEPT:
		case LogicalOperatorType::LOGICAL_INTERSECT: {
			auto &setop = op.Cast<LogicalSetOperation>();
			auto old_index = setop.table_index;
			setop.table_index = new_index;
			return old_index;
		}
		case LogicalOperatorType::LOGICAL_ORDER_BY:
		case LogicalOperatorType::LOGICAL_LIMIT:
			current = *op.children[0];
			break;
		default:
			throw InternalException("DISTINCT is applied to an unexpected operator");
		}
	}
}

static void ReplaceTableIndex(unique_ptr<Expression> &expr, TableIndex old_index, TableIndex new_index) {
	ExpressionIterator::VisitExpressionMutable<BoundColumnRefExpression>(
	    expr, [&](BoundColumnRefExpression &colref, unique_ptr<Expression> &ref) {
		    auto &binding = colref.Binding();
		    if (binding.table_index != old_index) {
			    return;
		    }
		    ref = make_uniq<BoundColumnRefExpression>(colref.GetAlias(), colref.GetReturnType(),
		                                              ColumnBinding(new_index, binding.column_index), colref.Depth());
	    });
}

static void ReplaceTableIndex(BoundOrderModifier &modifier, TableIndex old_index, TableIndex new_index) {
	for (auto &order : modifier.orders) {
		ReplaceTableIndex(order.expression, old_index, new_index);
	}
}

unique_ptr<LogicalOperator> Binder::PlanDistinct(BoundDistinctModifier &distinct,
                                                 optional_ptr<BoundOrderModifier> order,
                                                 unique_ptr<LogicalOperator> root) {
	// the result of the DISTINCT has the table index of the input so that the remainder of the plan remains valid
	auto child_index = GenerateTableIndex();
	auto result_index = RenameResultIndex(*root, child_index);
	// sorts and limits between the DISTINCT and the renamed operator reference the old index
	for (reference<LogicalOperator> current(*root); current.get().type == LogicalOperatorType::LOGICAL_ORDER_BY ||
	                                                current.get().type == LogicalOperatorType::LOGICAL_LIMIT;
	     current = *current.get().children[0]) {
		LogicalOperatorVisitor::EnumerateExpressions(
		    current.get(), [&](unique_ptr<Expression> *expr) { ReplaceTableIndex(*expr, result_index, child_index); });
	}
	if (IsBindingTrySubquery()) {
		auto &try_indexes = GetTrySubqueryPlanIndexesMutable();
		if (try_indexes.recorded && try_indexes.projection_index == result_index) {
			try_indexes.distinct_index = result_index;
			try_indexes.projection_index = child_index;
		}
	}
	root->ResolveOperatorTypes();
	auto child_bindings = root->GetColumnBindings();
	auto child_types = root->types;
	D_ASSERT(child_bindings.size() == child_types.size());

	// every distinct target becomes a group
	auto group_index = GenerateTableIndex();
	auto aggregate_index = GenerateTableIndex();
	vector<unique_ptr<Expression>> groups;
	unordered_map<idx_t, idx_t> group_references;
	for (auto &target : distinct.target_distincts) {
		ReplaceTableIndex(target, result_index, child_index);
		if (target->GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
			auto &colref = target->Cast<BoundColumnRefExpression>();
			group_references[colref.Binding().column_index.GetIndexUnsafe()] = groups.size();
		}
		groups.push_back(std::move(target));
	}
	distinct.target_distincts.clear();

	unique_ptr<BoundOrderModifier> first_order;
	if (distinct.distinct_type == DistinctType::DISTINCT_ON && order) {
		first_order = make_uniq<BoundOrderModifier>();
		for (auto &order_node : order->orders) {
			first_order->orders.push_back(order_node.Copy());
		}
		ReplaceTableIndex(*first_order, result_index, child_index);
	}

	// all other columns are the first value of the group
	vector<unique_ptr<Expression>> aggregates;
	vector<unique_ptr<Expression>> projections;
	FunctionBinder function_binder(context);
	for (idx_t i = 0; i < child_types.size(); i++) {
		auto &type = child_types[i];
		auto entry = group_references.find(child_bindings[i].column_index.GetIndexUnsafe());
		if (entry != group_references.end()) {
			projections.push_back(
			    make_uniq<BoundColumnRefExpression>(type, ColumnBinding(group_index, ProjectionIndex(entry->second))));
			continue;
		}
		vector<unique_ptr<Expression>> first_children;
		first_children.push_back(make_uniq<BoundColumnRefExpression>(type, child_bindings[i]));
		auto first_aggregate =
		    function_binder.BindAggregateFunction(GetBuiltinAggregateFunction(context, FirstFun::Name, {type}),
		                                          std::move(first_children), nullptr, AggregateType::NON_DISTINCT);
		first_aggregate->GetOrderBysMutable() = first_order ? first_order->Copy() : nullptr;
		projections.push_back(make_uniq<BoundColumnRefExpression>(
		    type, ColumnBinding(aggregate_index, ProjectionIndex(aggregates.size()))));
		aggregates.push_back(std::move(first_aggregate));
	}

	auto aggregate = make_uniq<LogicalAggregate>(group_index, aggregate_index, std::move(aggregates));
	// a single grouping set over all groups, as for GROUP BY, so statistics use the distinct counts of the groups
	GroupingSet all_groups;
	for (idx_t i = 0; i < groups.size(); i++) {
		all_groups.insert(ProjectionIndex(i));
	}
	aggregate->grouping_sets.push_back(std::move(all_groups));
	aggregate->groups = std::move(groups);
	// without an ORDER BY, DISTINCT ON takes the first row of every group in input order
	aggregate->preserve_input_order =
	    distinct.distinct_type == DistinctType::DISTINCT_ON && !first_order && !aggregate->expressions.empty();
	aggregate->AddChild(std::move(root));

	auto projection = make_uniq<LogicalProjection>(result_index, std::move(projections));
	projection->AddChild(std::move(aggregate));
	return std::move(projection);
}

unique_ptr<LogicalOperator> Binder::VisitQueryNode(BoundQueryNode &node, unique_ptr<LogicalOperator> root) {
	D_ASSERT(root);
	for (idx_t mod_idx = 0; mod_idx < node.modifiers.size(); mod_idx++) {
		auto &mod = node.modifiers[mod_idx];
		switch (mod->type) {
		case ResultModifierType::DISTINCT_MODIFIER: {
			auto &bound = mod->Cast<BoundDistinctModifier>();
			if (bound.target_distincts.empty()) {
				break;
			}
			// DISTINCT ON takes the first row according to an ORDER BY that directly follows it
			optional_ptr<BoundOrderModifier> order;
			if (mod_idx + 1 < node.modifiers.size() &&
			    node.modifiers[mod_idx + 1]->type == ResultModifierType::ORDER_MODIFIER) {
				order = node.modifiers[mod_idx + 1]->Cast<BoundOrderModifier>();
			}
			root = PlanDistinct(bound, order, std::move(root));
			break;
		}
		case ResultModifierType::ORDER_MODIFIER: {
			auto &bound = mod->Cast<BoundOrderModifier>();
			auto order = make_uniq<LogicalOrder>(std::move(bound.orders));
			order->AddChild(std::move(root));
			root = std::move(order);
			break;
		}
		case ResultModifierType::LIMIT_MODIFIER: {
			auto &bound = mod->Cast<BoundLimitModifier>();
			auto limit = make_uniq<LogicalLimit>(std::move(bound.limit_val), std::move(bound.offset_val));
			limit->AddChild(std::move(root));
			root = std::move(limit);
			break;
		}
		default:
			throw BinderException("Unimplemented modifier type!");
		}
	}
	return root;
}

} // namespace duckdb
