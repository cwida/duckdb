#include "duckdb/function/scalar/generic_functions.hpp"
#include "duckdb/optimizer/statistics_propagator.hpp"
#include "duckdb/planner/expression/bound_between_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/storage/statistics/numeric_statistics.hpp"

namespace duckdb {

bool StatisticsPropagator::ExpressionIsConstant(Expression &expr, const Value &val) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		return false;
	}
	auto &bound_constant = (BoundConstantExpression &)expr;
	D_ASSERT(bound_constant.value.type() == val.type());
	return bound_constant.value == val;
}

bool StatisticsPropagator::ExpressionIsConstantOrNull(Expression &expr, const Value &val) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	auto &bound_function = (BoundFunctionExpression &)expr;
	return ConstantOrNull::IsConstantOrNull(bound_function, val);
}

void StatisticsPropagator::SetStatisticsNotNull(ColumnBinding binding) {
	auto entry = statistics_map.find(binding);
	if (entry == statistics_map.end()) {
		return;
	}
	entry->second->validity_stats = make_unique<ValidityStatistics>(false);
}

void StatisticsPropagator::UpdateFilterStatistics(BaseStatistics &stats, ExpressionType comparison_type,
                                                  const Value &constant) {
	// any comparison filter removes all null values
	stats.validity_stats = make_unique<ValidityStatistics>(false);
	if (!stats.type.IsNumeric()) {
		// don't handle non-numeric columns here (yet)
		return;
	}
	auto &numeric_stats = (NumericStatistics &)stats;
	if (numeric_stats.min.is_null || numeric_stats.max.is_null) {
		// no stats available: skip this
		return;
	}
	switch (comparison_type) {
	case ExpressionType::COMPARE_LESSTHAN:
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		// X < constant OR X <= constant
		// max becomes the constant
		numeric_stats.max = constant;
		break;
	case ExpressionType::COMPARE_GREATERTHAN:
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		// X > constant OR X >= constant
		// min becomes the constant
		numeric_stats.min = constant;
		break;
	case ExpressionType::COMPARE_EQUAL:
		// X = constant
		// both min and max become the constant
		numeric_stats.min = constant;
		numeric_stats.max = constant;
		break;
	default:
		break;
	}
}

void StatisticsPropagator::UpdateFilterStatistics(BaseStatistics &lstats, BaseStatistics &rstats,
                                                  ExpressionType comparison_type) {
	// any comparison filter removes all null values
	lstats.validity_stats = make_unique<ValidityStatistics>(false);
	rstats.validity_stats = make_unique<ValidityStatistics>(false);
	D_ASSERT(lstats.type == rstats.type);
	if (!lstats.type.IsNumeric()) {
		// don't handle non-numeric columns here (yet)
		return;
	}
	auto &left_stats = (NumericStatistics &)lstats;
	auto &right_stats = (NumericStatistics &)rstats;
	if (left_stats.min.is_null || left_stats.max.is_null || right_stats.min.is_null || right_stats.max.is_null) {
		// no stats available: skip this
		return;
	}
	switch (comparison_type) {
	case ExpressionType::COMPARE_LESSTHAN:
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		// LEFT < RIGHT OR LEFT <= RIGHT
		// we know that every value of left is smaller (or equal to) every value in right
		// i.e. if we have left = [-50, 250] and right = [-100, 100]

		// we know that left.max is AT MOST equal to right.max
		// because any value in left that is BIGGER than right.max will not pass the filter
		if (left_stats.max > right_stats.max) {
			left_stats.max = right_stats.max;
		}

		// we also know that right.min is AT MOST equal to left.min
		// because any value in right that is SMALLER than left.min will not pass the filter
		if (right_stats.min < left_stats.min) {
			right_stats.min = left_stats.min;
		}
		// so in our example, the bounds get updated as follows:
		// left: [-50, 100], right: [-50, 100]
		break;
	case ExpressionType::COMPARE_GREATERTHAN:
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		// LEFT > RIGHT OR LEFT >= RIGHT
		// we know that every value of left is bigger (or equal to) every value in right
		// this is essentially the inverse of the less than (or equal to) scenario
		if (right_stats.max > left_stats.max) {
			right_stats.max = left_stats.max;
		}
		if (left_stats.min < right_stats.min) {
			left_stats.min = right_stats.min;
		}
		break;
	case ExpressionType::COMPARE_EQUAL:
		// LEFT = RIGHT
		// only the tightest bounds pass
		// so if we have e.g. left = [-50, 250] and right = [-100, 100]
		// the tighest bounds are [-50, 100]
		// select the highest min
		if (left_stats.min > right_stats.min) {
			right_stats.min = left_stats.min;
		} else {
			left_stats.min = right_stats.min;
		}
		// select the lowest max
		if (left_stats.max < right_stats.max) {
			right_stats.max = left_stats.max;
		} else {
			left_stats.max = right_stats.max;
		}
		break;
	default:
		break;
	}
}

void StatisticsPropagator::UpdateFilterStatistics(Expression &left, Expression &right, ExpressionType comparison_type) {
	// first check if either side is a bound column ref
	// any column ref involved in a comparison will not be null after the comparison
	if (left.type == ExpressionType::BOUND_COLUMN_REF) {
		SetStatisticsNotNull(((BoundColumnRefExpression &)left).binding);
	}
	if (right.type == ExpressionType::BOUND_COLUMN_REF) {
		SetStatisticsNotNull(((BoundColumnRefExpression &)right).binding);
	}
	// check if this is a comparison between a constant and a column ref
	BoundConstantExpression *constant = nullptr;
	BoundColumnRefExpression *columnref = nullptr;
	if (left.type == ExpressionType::VALUE_CONSTANT && right.type == ExpressionType::BOUND_COLUMN_REF) {
		constant = (BoundConstantExpression *)&left;
		columnref = (BoundColumnRefExpression *)&right;
		comparison_type = FlipComparisionExpression(comparison_type);
	} else if (left.type == ExpressionType::BOUND_COLUMN_REF && right.type == ExpressionType::VALUE_CONSTANT) {
		columnref = (BoundColumnRefExpression *)&left;
		constant = (BoundConstantExpression *)&right;
	} else if (left.type == ExpressionType::BOUND_COLUMN_REF && right.type == ExpressionType::BOUND_COLUMN_REF) {
		// comparison between two column refs
		auto &left_column_ref = (BoundColumnRefExpression &)left;
		auto &right_column_ref = (BoundColumnRefExpression &)right;
		auto lentry = statistics_map.find(left_column_ref.binding);
		auto rentry = statistics_map.find(right_column_ref.binding);
		if (lentry == statistics_map.end() || rentry == statistics_map.end()) {
			return;
		}
		UpdateFilterStatistics(*lentry->second, *rentry->second, comparison_type);
	} else {
		// unsupported filter
		return;
	}
	if (constant && columnref) {
		// comparison between columnref
		auto entry = statistics_map.find(columnref->binding);
		if (entry == statistics_map.end()) {
			return;
		}
		UpdateFilterStatistics(*entry->second, comparison_type, constant->value);
	}
}

void StatisticsPropagator::UpdateFilterStatistics(Expression &condition) {
	// in filters, we check for constant comparisons with bound columns
	// if we find a comparison in the form of e.g. "i=3", we can update our statistics for that column
	switch (condition.GetExpressionClass()) {
	case ExpressionClass::BOUND_BETWEEN: {
		auto &between = (BoundBetweenExpression &)condition;
		UpdateFilterStatistics(*between.input, *between.lower, between.LowerComparisonType());
		UpdateFilterStatistics(*between.input, *between.upper, between.UpperComparisonType());
		break;
	}
	case ExpressionClass::BOUND_COMPARISON: {
		auto &comparison = (BoundComparisonExpression &)condition;
		UpdateFilterStatistics(*comparison.left, *comparison.right, comparison.type);
		break;
	}
	default:
		break;
	}
}

unique_ptr<NodeStatistics> StatisticsPropagator::PropagateStatistics(LogicalFilter &filter,
                                                                     unique_ptr<LogicalOperator> *node_ptr) {
	// first propagate to the child
	node_stats = PropagateStatistics(filter.children[0]);

	// then propagate to each of the expressions
	for (idx_t i = 0; i < filter.expressions.size(); i++) {
		auto &condition = filter.expressions[i];
		PropagateExpression(condition);

		if (ExpressionIsConstant(*condition, Value::BOOLEAN(true))) {
			// filter is always true; it is useless to execute it
			// erase this condition
			filter.expressions.erase(filter.expressions.begin() + i);
			i--;
			if (filter.expressions.empty()) {
				// all conditions have been erased: remove the entire filter
				*node_ptr = move(filter.children[0]);
				break;
			}
		} else if (ExpressionIsConstant(*condition, Value::BOOLEAN(false)) ||
		           ExpressionIsConstantOrNull(*condition, Value::BOOLEAN(false))) {
			// filter is always false or null; this entire filter should be replaced by an empty result block
			ReplaceWithEmptyResult(*node_ptr);
			return make_unique<NodeStatistics>(0, 0);
		} else {
			// cannot prune this filter: propagate statistics from the filter
			UpdateFilterStatistics(*condition);
		}
	}
	// the max cardinality of a filter is the cardinality of the input (i.e. no tuples get filtered)
	return move(node_stats);
}

} // namespace duckdb
