#include "duckdb/optimizer/statistics_propagator.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/optimizer/expression_rewriter.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/storage/statistics/string_stats.hpp"

namespace duckdb {

static Value GetStringStatsMin(const BaseStatistics &stats) {
	return Value::BLOB_RAW(StringStats::Min(stats));
}

static Value GetStringStatsMax(const BaseStatistics &stats) {
	auto max = StringStats::Max(stats);
	if (StringStats::GetMaxType(stats) == StringStatsType::EXACT_STATS) {
		return Value::BLOB_RAW(max);
	}
	// A truncated max stores a prefix. Use the next prefix as its upper bound.
	if (StringUtil::FindNextPrefix(max)) {
		return Value::BLOB_RAW(max);
	}
	return Value();
}

static bool RangesDoNotOverlap(const Value &lmin, const Value &lmax, const Value &rmin, const Value &rmax) {
	return (!lmin.IsNull() && !rmax.IsNull() && lmin > rmax) || (!rmin.IsNull() && !lmax.IsNull() && rmin > lmax);
}

static bool RangesAreEqualConstants(const Value &lmin, const Value &lmax, const Value &rmin, const Value &rmax) {
	return !lmin.IsNull() && !lmax.IsNull() && !rmin.IsNull() && !rmax.IsNull() && lmin == lmax && rmin == rmax &&
	       lmin == rmin;
}

static FilterPropagateResult PropagateValueComparison(const Value &lmin, const Value &lmax, const Value &rmin,
                                                      const Value &rmax, ExpressionType comparison, bool has_null) {
	const auto always_true =
	    has_null ? FilterPropagateResult::FILTER_TRUE_OR_NULL : FilterPropagateResult::FILTER_ALWAYS_TRUE;
	const auto always_false =
	    has_null ? FilterPropagateResult::FILTER_FALSE_OR_NULL : FilterPropagateResult::FILTER_ALWAYS_FALSE;
	switch (comparison) {
	case ExpressionType::COMPARE_EQUAL:
		if (RangesDoNotOverlap(lmin, lmax, rmin, rmax)) {
			return always_false;
		}
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	case ExpressionType::COMPARE_NOTEQUAL:
		if (RangesAreEqualConstants(lmin, lmax, rmin, rmax)) {
			return always_false;
		}
		if (RangesDoNotOverlap(lmin, lmax, rmin, rmax)) {
			return always_true;
		}
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	case ExpressionType::COMPARE_GREATERTHAN:
		if (!lmin.IsNull() && !rmax.IsNull() && lmin > rmax) {
			return always_true;
		}
		if (!rmin.IsNull() && !lmax.IsNull() && rmin >= lmax) {
			return always_false;
		}
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		if (!lmin.IsNull() && !rmax.IsNull() && lmin >= rmax) {
			return always_true;
		}
		if (!rmin.IsNull() && !lmax.IsNull() && rmin > lmax) {
			return always_false;
		}
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	case ExpressionType::COMPARE_LESSTHAN:
		if (!lmax.IsNull() && !rmin.IsNull() && lmax < rmin) {
			return always_true;
		}
		if (!lmin.IsNull() && !rmax.IsNull() && lmin >= rmax) {
			return always_false;
		}
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		if (!lmax.IsNull() && !rmin.IsNull() && lmax <= rmin) {
			return always_true;
		}
		if (!lmin.IsNull() && !rmax.IsNull() && lmin > rmax) {
			return always_false;
		}
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	default:
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
}

FilterPropagateResult StatisticsPropagator::PropagateComparison(const BaseStatistics &lstats,
                                                                const BaseStatistics &rstats,
                                                                ExpressionType comparison) {
	if (lstats.GetStatsType() == StatisticsType::STRING_STATS &&
	    rstats.GetStatsType() == StatisticsType::STRING_STATS) {
		if (lstats.GetType() != rstats.GetType()) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		Value lmin;
		Value lmax;
		Value rmin;
		Value rmax;
		if (StringStats::HasMin(lstats)) {
			lmin = GetStringStatsMin(lstats);
		}
		if (StringStats::HasMax(lstats)) {
			lmax = GetStringStatsMax(lstats);
		}
		if (StringStats::HasMin(rstats)) {
			rmin = GetStringStatsMin(rstats);
		}
		if (StringStats::HasMax(rstats)) {
			rmax = GetStringStatsMax(rstats);
		}
		bool has_null = lstats.CanHaveNull() || rstats.CanHaveNull();
		return PropagateValueComparison(lmin, lmax, rmin, rmax, comparison, has_null);
	}

	switch (lstats.GetType().InternalType()) {
	case PhysicalType::BOOL:
	case PhysicalType::UINT8:
	case PhysicalType::UINT16:
	case PhysicalType::UINT32:
	case PhysicalType::UINT64:
	case PhysicalType::UINT128:
	case PhysicalType::INT8:
	case PhysicalType::INT16:
	case PhysicalType::INT32:
	case PhysicalType::INT64:
	case PhysicalType::INT128:
	case PhysicalType::FLOAT:
	case PhysicalType::DOUBLE:
	case PhysicalType::INTERVAL:
		break;
	default:
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	if (!NumericStats::HasMinMax(lstats) || !NumericStats::HasMinMax(rstats)) {
		// no stats available: nothing to prune
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	bool has_null = lstats.CanHaveNull() || rstats.CanHaveNull();
	return PropagateValueComparison(NumericStats::Min(lstats), NumericStats::Max(lstats), NumericStats::Min(rstats),
	                                NumericStats::Max(rstats), comparison, has_null);
}

ExpressionType StatisticsPropagator::SimplifyDistinctComparison(ExpressionType comparison,
                                                                optional_ptr<const BaseStatistics> left,
                                                                optional_ptr<const BaseStatistics> right,
                                                                ComparisonContext context) {
	if (comparison != ExpressionType::COMPARE_NOT_DISTINCT_FROM &&
	    comparison != ExpressionType::COMPARE_DISTINCT_FROM) {
		return comparison;
	}
	for (auto stats : {left, right}) {
		if (stats && (stats->GetType().id() == LogicalTypeId::UNION || stats->GetType().id() == LogicalTypeId::TUPLE)) {
			return comparison;
		}
	}
	const bool left_not_null = left && !left->CanHaveNull();
	const bool right_not_null = right && !right->CanHaveNull();
	switch (comparison) {
	case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
		if ((left_not_null && right_not_null) ||
		    (context == ComparisonContext::JOIN && (left_not_null || right_not_null))) {
			return ExpressionType::COMPARE_EQUAL;
		}
		break;
	case ExpressionType::COMPARE_DISTINCT_FROM:
		if (left_not_null && right_not_null) {
			return ExpressionType::COMPARE_NOTEQUAL;
		}
		break;
	default:
		break;
	}
	return comparison;
}

unique_ptr<BaseStatistics> StatisticsPropagator::PropagateComparison(BoundFunctionExpression &expr,
                                                                     unique_ptr<Expression> &expr_ptr) {
	auto &left = BoundComparisonExpression::LeftMutable(expr);
	auto &right = BoundComparisonExpression::RightMutable(expr);
	auto left_stats = PropagateExpression(left);
	auto right_stats = PropagateExpression(right);
	auto comparison = SimplifyDistinctComparison(expr.GetExpressionType(), left_stats.get(), right_stats.get(),
	                                             ComparisonContext::SCALAR);
	if (comparison != expr.GetExpressionType()) {
		BoundComparisonExpression::SetType(expr, comparison);
	}
	if (!left_stats || !right_stats) {
		return nullptr;
	}
	// propagate the statistics of the comparison operator
	auto propagate_result = PropagateComparison(*left_stats, *right_stats, expr.GetExpressionType());
	switch (propagate_result) {
	case FilterPropagateResult::FILTER_ALWAYS_TRUE:
		return FoldToConstant(expr_ptr, Value::BOOLEAN(true));
	case FilterPropagateResult::FILTER_ALWAYS_FALSE:
		return FoldToConstant(expr_ptr, Value::BOOLEAN(false));
	case FilterPropagateResult::FILTER_TRUE_OR_NULL: {
		if (SuppressPlanFolds()) {
			return nullptr;
		}
		vector<unique_ptr<Expression>> children;
		children.push_back(std::move(left));
		children.push_back(std::move(right));
		expr_ptr = ExpressionRewriter::ConstantOrNull(context, std::move(children), Value::BOOLEAN(true));
		return nullptr;
	}
	case FilterPropagateResult::FILTER_FALSE_OR_NULL: {
		if (SuppressPlanFolds()) {
			return nullptr;
		}
		vector<unique_ptr<Expression>> children;
		children.push_back(std::move(left));
		children.push_back(std::move(right));
		expr_ptr = ExpressionRewriter::ConstantOrNull(context, std::move(children), Value::BOOLEAN(false));
		return nullptr;
	}
	default:
		// FIXME: we can propagate nulls here, i.e. this expression will have nulls only if left and right has nulls
		return nullptr;
	}
}

} // namespace duckdb
