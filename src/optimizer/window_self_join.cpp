#include "duckdb/optimizer/window_self_join.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/operator/logical_prepare.hpp"
#include "duckdb/planner/operator/logical_window.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_cross_product.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/function/aggregate_state.hpp"
#include "duckdb/planner/expression_binder/base_select_binder.hpp"
#include "duckdb/planner/logical_operator_deep_copy.hpp"

namespace duckdb {

class VolatileExpressionCounter : public LogicalOperatorVisitor {
public:
	static idx_t HasVolatiles(LogicalOperator &op) {
		VolatileExpressionCounter counter;
		counter.VisitOperator(op);
		return counter.volatiles;
	}

	VolatileExpressionCounter() {
	}

	void VisitExpression(unique_ptr<Expression> *expression) override {
		volatiles += (*expression)->IsVolatile();
		LogicalOperatorVisitor::VisitExpression(expression);
	}

	idx_t volatiles = 0;
};

//	Rows that tie on the argument ordering have equal arguments, so the frame ordering cannot change the result
static bool FrameOrderIsRedundant(const BoundWindowExpression &w_expr) {
	if (w_expr.ArgOrders().empty()) {
		return false;
	}

	vector<reference<Expression>> order_refs;
	order_refs.reserve(w_expr.ArgOrders().size());
	for (auto &order : w_expr.ArgOrders()) {
		order_refs.emplace_back(*order.expression);
	}
	for (auto &arg : w_expr.GetChildren()) {
		if (!BaseSelectBinder::IsFunctionallyDependent(arg, order_refs)) {
			return false;
		}
	}
	return true;
}

static bool IsOrderableDistinctAggregate(const BoundWindowExpression &w_expr) {
	//	If the aggregate is order-sensitive and distinct,
	//	then the ORDER BYs need to be functional dependencies of the arguments.
	auto agg_func = *w_expr.AggregateFunction();
	if (agg_func.GetOrderDependent() != AggregateOrderDependent::ORDER_DEPENDENT || !w_expr.Distinct()) {
		return true;
	}

	const auto &arguments = w_expr.GetChildren();
	vector<reference<Expression>> arg_refs;
	arg_refs.reserve(arguments.size());
	for (auto &arg : arguments) {
		arg_refs.emplace_back(*arg);
	}
	bool in_args = true;
	for (auto &order : w_expr.ArgOrders()) {
		in_args &= BaseSelectBinder::IsFunctionallyDependent(order.expression, arg_refs);
	}
	if (FrameOrderIsRedundant(w_expr)) {
		return in_args;
	}
	for (auto &order : w_expr.OrderBy()) {
		in_args &= BaseSelectBinder::IsFunctionallyDependent(order.expression, arg_refs);
	}

	return in_args;
}

static unique_ptr<Expression> TranslateAggregate(ClientContext &client, const BoundWindowExpression &w_expr) {
	auto agg_func = *w_expr.AggregateFunction();
	unique_ptr<FunctionData> bind_info;
	if (w_expr.BindInfo()) {
		bind_info = w_expr.BindInfo()->Copy();
	} else {
		bind_info = nullptr;
	}

	vector<unique_ptr<Expression>> children;
	for (auto &child : w_expr.GetChildren()) {
		auto child_copy = child->Copy();
		children.push_back(std::move(child_copy));
	}

	unique_ptr<Expression> filter;
	if (w_expr.Filter()) {
		filter = w_expr.Filter()->Copy();
	}

	const auto aggr_type = w_expr.Distinct() ? AggregateType::DISTINCT : AggregateType::NON_DISTINCT;
	const auto aggr_ordered = (agg_func.GetOrderDependent() == AggregateOrderDependent::ORDER_DEPENDENT);
	auto result = make_uniq<BoundAggregateExpression>(std::move(agg_func), std::move(children), std::move(filter),
	                                                  std::move(bind_info), aggr_type);

	if (!aggr_ordered) {
		//	ORDER BY is a NOP, so drop it.
		return std::move(result);
	}

	if (w_expr.ArgOrders().empty() && w_expr.OrderBy().empty()) {
		return std::move(result);
	}

	//	The frame ordering breaks ties in the argument ordering
	result->GetOrderBysMutable() = make_uniq<BoundOrderModifier>();
	auto &orders = result->GetOrderBysMutable()->orders;
	for (auto &order : w_expr.ArgOrders()) {
		orders.emplace_back(order.Copy());
	}
	if (FrameOrderIsRedundant(w_expr)) {
		return std::move(result);
	}
	for (auto &order : w_expr.OrderBy()) {
		orders.emplace_back(order.Copy());
	}

	return std::move(result);
}

WindowSelfJoinOptimizer::WindowSelfJoinOptimizer(Optimizer &optimizer) : optimizer(optimizer) {
}

unique_ptr<LogicalOperator> WindowSelfJoinOptimizer::Optimize(unique_ptr<LogicalOperator> op) {
	ColumnBindingReplacer replacer;
	op = OptimizeInternal(std::move(op), replacer);
	if (!replacer.replacement_bindings.empty()) {
		replacer.VisitOperator(*op);
	}
	return op;
}

bool WindowSelfJoinOptimizer::CanOptimize(const BoundWindowExpression &w_expr,
                                          const BoundWindowExpression &w_expr0) const {
	if (w_expr.GetExpressionType() != ExpressionType::WINDOW_AGGREGATE) {
		return false;
	}
	//	We can only accept ORDER BY clauses if the frame is the entire partition
	//	In that case, we will have to move the ordering clauses into the aggregate.
	//	ROWS framing is excluded because the frame depends on physical row position even without ORDER BY.
	switch (w_expr.WindowStart()) {
	case WindowBoundary::UNBOUNDED_PRECEDING:
		break;
	case WindowBoundary::CURRENT_ROW_RANGE:
	case WindowBoundary::CURRENT_ROW_GROUPS:
		if (!w_expr.OrderBy().empty()) {
			return false;
		}
		break;
	default:
		return false;
	}

	switch (w_expr.WindowEnd()) {
	case WindowBoundary::UNBOUNDED_FOLLOWING:
		break;
	case WindowBoundary::CURRENT_ROW_RANGE:
	case WindowBoundary::CURRENT_ROW_GROUPS:
		if (!w_expr.OrderBy().empty()) {
			return false;
		}
		break;
	default:
		return false;
	}
	if (w_expr.WindowExclude() != WindowExcludeMode::NO_OTHER) {
		return false;
	}
	if (!w_expr.PartitionsAreEquivalent(w_expr0)) {
		return false;
	}

	if (!IsOrderableDistinctAggregate(w_expr)) {
		return false;
	}

	return true;
}

bool WindowSelfJoinOptimizer::CanOptimize(const LogicalOperator &op) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_GET:
	case LogicalOperatorType::LOGICAL_EXPRESSION_GET:
	case LogicalOperatorType::LOGICAL_PROJECTION:
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY:
	case LogicalOperatorType::LOGICAL_DUMMY_SCAN:
	case LogicalOperatorType::LOGICAL_FILTER:
		if (!op.children.empty()) {
			return CanOptimize(*op.children[0]);
		}
		return true;
	default:
		break;
	}
	return false;
}

unique_ptr<LogicalOperator> WindowSelfJoinOptimizer::OptimizeInternal(unique_ptr<LogicalOperator> op,
                                                                      ColumnBindingReplacer &replacer) {
	if (op->type == LogicalOperatorType::LOGICAL_PREPARE) {
		parameter_data = op->Cast<LogicalPrepare>().prepared->value_map;
	}
	if (op->type == LogicalOperatorType::LOGICAL_WINDOW) {
		auto &window = op->Cast<LogicalWindow>();

		// Check recursively
		window.children[0] = OptimizeInternal(std::move(window.children[0]), replacer);

		//	We cannot perform self-join when there are volatile functions below us.
		if (VolatileExpressionCounter::HasVolatiles(window)) {
			return op;
		}

		if (!CanOptimize(*window.children[0])) {
			return op;
		}

		auto &w_expr0 = window.expressions[0]->Cast<BoundWindowExpression>();
		for (auto &expr : window.expressions) {
			auto &w_expr = expr->Cast<BoundWindowExpression>();
			if (!CanOptimize(w_expr, w_expr0)) {
				return op;
			}
		}
		auto &partitions = w_expr0.Partitions();

		// --- Transformation ---
		// try to copy the LHS
		unique_ptr<LogicalOperator> copy_child;
		// Reuse the LogicalOperatorDeepCopy from CTE inlining, as the copying requirements are similar (copying an
		// operator subtree and replacing table indices)
		LogicalOperatorDeepCopy deep_copy(optimizer.binder, parameter_data);
		try {
			copy_child = deep_copy.DeepCopy(window.children[0]);
		} catch (std::exception &ex) {
			// failed to copy the LHS - cannot run this optimizer
			return op;
		}

		// We statically know that the copy child has the same column bindings as the original child,
		// just with different table indices. So we can prepare a replacer with the correct binding
		// replacements to avoid having to resolve anything again later on.
		ColumnBindingReplacer local_replacer;
		auto old_bindings = window.children[0]->GetColumnBindings();
		auto new_bindings = copy_child->GetColumnBindings();
		vector<ReplacementBinding> binding_replacements;
		for (idx_t i = 0; i < old_bindings.size(); ++i) {
			binding_replacements.emplace_back(old_bindings[i], new_bindings[i]);
		}
		local_replacer.replacement_bindings = std::move(binding_replacements);

		auto original_child = std::move(window.children[0]);

		auto aggregate_index = optimizer.binder.GenerateTableIndex();
		auto group_index = optimizer.binder.GenerateTableIndex();

		vector<unique_ptr<Expression>> groups;
		vector<unique_ptr<Expression>> aggregates;

		// Create Aggregate Operator
		for (auto &part : partitions) {
			auto part_copy = part->Copy();
			groups.push_back(std::move(part_copy));
		}

		for (auto &expr : window.expressions) {
			auto &w_expr = expr->Cast<BoundWindowExpression>();
			aggregates.emplace_back(TranslateAggregate(optimizer.GetContext(), w_expr));
		}

		// args: group_index, aggregate_index, ...
		auto agg_op = make_uniq<LogicalAggregate>(group_index, aggregate_index, std::move(aggregates));

		agg_op->groups = std::move(groups);
		agg_op->children.push_back(std::move(copy_child));
		local_replacer.VisitOperator(*agg_op);
		agg_op->ResolveOperatorTypes();

		if (agg_op->types.size() <= agg_op->groups.size()) {
			throw InternalException("LogicalAggregate types size mismatch");
		}

		// Inner Join on the partition keys, or cross join against the aggregate
		unique_ptr<LogicalOperator> join;
		if (partitions.empty()) {
			join = LogicalCrossProduct::Create(std::move(original_child), std::move(agg_op));
		} else {
			auto cjoin = make_uniq<LogicalComparisonJoin>(JoinType::INNER);
			for (size_t i = 0; i < partitions.size(); ++i) {
				auto left_expr = partitions[i]->Copy();
				auto right_expr = make_uniq<BoundColumnRefExpression>(partitions[i]->GetReturnType(),
				                                                      ColumnBinding(group_index, ProjectionIndex(i)));
				cjoin->conditions.push_back(JoinCondition(std::move(left_expr), std::move(right_expr),
				                                          ExpressionType::COMPARE_NOT_DISTINCT_FROM));
			}
			join = std::move(cjoin);
			join->children.push_back(std::move(original_child));
			join->children.push_back(std::move(agg_op));
		}
		join->ResolveOperatorTypes();

		// Replace aggregate bindings
		// Old window column: (window.window_index, x)
		// New constant column: (aggregate_index, x)
		for (idx_t column_index = 0; column_index < window.expressions.size(); ++column_index) {
			ColumnBinding old_binding(window.window_index, ProjectionIndex(column_index));
			ColumnBinding new_binding(aggregate_index, ProjectionIndex(column_index));
			replacer.replacement_bindings.emplace_back(old_binding, new_binding);
		}

		return join;
	} else if (!op->children.empty()) {
		for (auto &child : op->children) {
			child = OptimizeInternal(std::move(child), replacer);
		}
	}
	return op;
}

} // namespace duckdb
