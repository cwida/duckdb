#include "execution/expression_executor.hpp"
#include "optimizer/filter_pushdown.hpp"
#include "optimizer/optimizer.hpp"
#include "planner/expression/bound_columnref_expression.hpp"
#include "planner/expression/bound_comparison_expression.hpp"
#include "planner/expression/bound_constant_expression.hpp"
#include "planner/expression_iterator.hpp"
#include "planner/operator/logical_comparison_join.hpp"
#include "planner/operator/logical_filter.hpp"

using namespace duckdb;
using namespace std;

using Filter = FilterPushdown::Filter;

static unique_ptr<Expression> ReplaceColRefWithNull(unique_ptr<Expression> expr,
                                                    unordered_set<uint64_t> &right_bindings) {
	if (expr->type == ExpressionType::BOUND_COLUMN_REF) {
		auto &bound_colref = (BoundColumnRefExpression &)*expr;
		if (right_bindings.find(bound_colref.binding.table_index) != right_bindings.end()) {
			// bound colref belongs to RHS
			// replace it with a constant NULL
			return make_unique<BoundConstantExpression>(Value(expr->return_type));
		}
		return expr;
	}
	ExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<Expression> child) -> unique_ptr<Expression> {
		return ReplaceColRefWithNull(move(child), right_bindings);
	});
	return expr;
}

static bool FilterRemovesNull(ExpressionRewriter &rewriter, Expression *expr, unordered_set<uint64_t> &right_bindings) {
	// make a copy of the expression
	auto copy = expr->Copy();
	// replace all BoundColumnRef expressions frmo the RHS with NULL constants in the copied expression
	copy = ReplaceColRefWithNull(move(copy), right_bindings);

	// attempt to flatten the expression by running the expression rewriter on it
	auto filter = make_unique<LogicalFilter>();
	filter->expressions.push_back(move(copy));
	rewriter.Apply(*filter);
	assert(filter->expressions.size() == 1);

	if (filter->expressions[0]->type != ExpressionType::VALUE_CONSTANT) {
		// could not flatten the result
		assert(!filter->expressions[0]->IsFoldable());
		return false;
	}
	// we flattened the result into a scalar, check if it is FALSE or NULL
	auto val = ((BoundConstantExpression &)*filter->expressions[0]).value.CastAs(TypeId::BOOLEAN);
	// if the result of the expression with all expressions replaced with NULL is "NULL" or "false"
	// then any extra entries generated by the LEFT OUTER JOIN will be filtered out!
	// hence the LEFT OUTER JOIN is equivalent to an inner join
	return val.is_null || !val.value_.boolean;
}

unique_ptr<LogicalOperator> FilterPushdown::PushdownLeftJoin(unique_ptr<LogicalOperator> op,
                                                             unordered_set<uint64_t> &left_bindings,
                                                             unordered_set<uint64_t> &right_bindings) {
	auto &join = (LogicalJoin &)*op;
	assert(join.type == JoinType::LEFT);
	assert(op->type != LogicalOperatorType::DELIM_JOIN);
	FilterPushdown left_pushdown(optimizer), right_pushdown(optimizer);
	// for a comparison join we create a FilterCombiner that checks if we can push conditions on LHS join conditions
	// into the RHS of the join
	FilterCombiner filter_combiner;
	if (op->type == LogicalOperatorType::COMPARISON_JOIN) {
		// add all comparison conditions
		auto &comparison_join = (LogicalComparisonJoin &)*op;
		for (auto &cond : comparison_join.conditions) {
			filter_combiner.AddFilter(
			    make_unique<BoundComparisonExpression>(cond.comparison, cond.left->Copy(), cond.right->Copy()));
		}
	}
	// now check the set of filters
	for (index_t i = 0; i < filters.size(); i++) {
		auto side = JoinSide::GetJoinSide(filters[i]->bindings, left_bindings, right_bindings);
		if (side == JoinSide::LEFT) {
			// bindings match left side
			// we can push the filter into the left side
			if (op->type == LogicalOperatorType::COMPARISON_JOIN) {
				// we MIGHT be able to push it down the RHS as well, but only if it is a comparison that matches the
				// join predicates we use the FilterCombiner to figure this out add the expression to the FilterCombiner
				filter_combiner.AddFilter(filters[i]->filter->Copy());
			}
			left_pushdown.filters.push_back(move(filters[i]));
			// erase the filter from the list of filters
			filters.erase(filters.begin() + i);
			i--;
		} else {
			// bindings match right side or both sides: we cannot directly push it into the right
			// however, if the filter removes rows with null values from the RHS we can turn the left outer join
			// in an inner join, and then push down as we would push down an inner join
			if (FilterRemovesNull(optimizer.rewriter, filters[i]->filter.get(), right_bindings)) {
				// the filter removes NULL values, turn it into an inner join
				join.type = JoinType::INNER;
				// now we can do more pushdown
				// move all filters we added to the left_pushdown back into the filter list
				for (auto &left_filter : left_pushdown.filters) {
					filters.push_back(move(left_filter));
				}
				// now push down the inner join
				return PushdownInnerJoin(move(op), left_bindings, right_bindings);
			}
		}
	}
	// finally we check the FilterCombiner to see if there are any predicates we can push into the RHS
	// we only added (1) predicates that have JoinSide::BOTH from the conditions, and
	// (2) predicates that have JoinSide::LEFT from the filters
	// we check now if this combination generated any new filters that are only on JoinSide::RIGHT
	// this happens if, e.g. a join condition is (i=a) and there is a filter (i=500), we can then push the filter
	// (a=500) into the RHS
	filter_combiner.GenerateFilters([&](unique_ptr<Expression> filter) {
		if (JoinSide::GetJoinSide(*filter, left_bindings, right_bindings) == JoinSide::RIGHT) {
			right_pushdown.AddFilter(move(filter));
		}
	});
	right_pushdown.GenerateFilters();
	op->children[0] = left_pushdown.Rewrite(move(op->children[0]));
	op->children[1] = right_pushdown.Rewrite(move(op->children[1]));
	return FinishPushdown(move(op));
}
