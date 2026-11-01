#include "duckdb/optimizer/statistics_propagator.hpp"

#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/arg_properties.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "duckdb/storage/statistics/string_stats.hpp"

namespace duckdb {

namespace {
bool TryEvaluateAtConstants(ClientContext &context, const BoundFunctionExpression &func,
                            const vector<Value> &arg_values, Value &result) {
	vector<unique_ptr<Expression>> children;
	children.reserve(arg_values.size());
	for (auto &v : arg_values) {
		children.push_back(make_uniq<BoundConstantExpression>(v));
	}
	// Functions such as alias() inspect expression metadata during execution.
	auto clone = func.Copy();
	clone->Cast<BoundFunctionExpression>().GetChildrenMutable() = std::move(children);
	return ExpressionExecutor::TryEvaluateScalar(context, *clone, result);
}

// Whether [lo, hi] holds -infinity and another value. The smallest finite DATE or TIMESTAMP sorts below -infinity.
bool HoldsNegativeInfinity(const Value &lo, const Value &hi) {
	switch (lo.type().id()) {
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_TZ_NS: {
		auto ninfinity = Value::NegativeInfinity(lo.type());
		return lo < hi && lo <= ninfinity && ninfinity <= hi;
	}
	default:
		return false;
	}
}

// Equal bounds need not imply identical inputs for certain types, so we skip this optimization for those values.
// Floating-point bounds only lose information for the sign of zero.
bool CanInferConstantFromNumericBounds(const Value &value) {
	switch (value.type().InternalType()) {
	case PhysicalType::FLOAT:
		return value.GetValue<float>() != 0.0F;
	case PhysicalType::DOUBLE:
		return value.GetValue<double>() != 0.0;
	case PhysicalType::INTERVAL:
		return false;
	default:
		return true;
	}
}

bool TryInferConstantBounds(const BaseStatistics &stats, Value &constant) {
	if (stats.CanHaveNull() && !stats.CanHaveNoNull()) {
		constant = Value(stats.GetType());
		return true;
	}
	// bounds of empty stats do not describe any actual value
	if (stats.CanHaveNull() || !stats.CanHaveNoNull()) {
		return false;
	}
	if (stats.GetStatsType() == StatisticsType::NUMERIC_STATS && NumericStats::HasMinMax(stats)) {
		auto min = NumericStats::Min(stats);
		auto max = NumericStats::Max(stats);
		if (min != max || !CanInferConstantFromNumericBounds(min)) {
			return false;
		}
		constant = std::move(min);
		return true;
	}
	if ((stats.GetType().id() == LogicalTypeId::VARCHAR || stats.GetType().id() == LogicalTypeId::BLOB) &&
	    StringStats::HasMinMax(stats) && StringStats::GetMinType(stats) == StringStatsType::EXACT_STATS &&
	    StringStats::GetMaxType(stats) == StringStatsType::EXACT_STATS &&
	    StringStats::Min(stats) == StringStats::Max(stats)) {
		constant = Value::BLOB_RAW(StringStats::Min(stats)).WithType(stats.GetType());
		return true;
	}
	return false;
}
} // namespace

unique_ptr<BaseStatistics> StatisticsPropagator::PropagateConstantInputs(ClientContext &context,
                                                                         const BoundFunctionExpression &func,
                                                                         const vector<BaseStatistics> &child_stats) {
	if (func.Function().GetStability() == FunctionStability::VOLATILE || func.GetChildren().empty()) {
		return nullptr;
	}
	// Lambda arguments are placeholders, not SQL values that can be evaluated independently.
	if (func.Function().HasBindLambdaCallback()) {
		return nullptr;
	}
	vector<Value> values;
	values.reserve(func.GetChildren().size());
	for (idx_t idx = 0; idx < func.GetChildren().size(); ++idx) {
		auto &child = *func.GetChildren()[idx];
		auto &stats = child_stats[idx];
		Value value;
		if (child.IsFoldable()) {
			if (!ExpressionExecutor::TryEvaluateScalar(context, child, value)) {
				return nullptr;
			}
		} else if (!TryInferConstantBounds(stats, value)) {
			return nullptr;
		}
		values.emplace_back(std::move(value));
	}
	Value result;
	if (!TryEvaluateAtConstants(context, func, values, result)) {
		return nullptr;
	}
	return BaseStatistics::FromConstant(result).ToUnique();
}

//! Evaluate `func` at the lo/hi corner of each child's value range to derive output min/max.
//! Decreasing args are swapped so f(lo_args) and f(hi_args) bracket the output range.
unique_ptr<BaseStatistics> StatisticsPropagator::PropagateMonotoneBounds(ClientContext &context,
                                                                         const BoundFunctionExpression &func,
                                                                         const vector<BaseStatistics> &child_stats) {
	if (!func.Function().HasArgProperties() || func.GetChildren().empty()) {
		return nullptr;
	}
	if (func.Function().GetStability() == FunctionStability::VOLATILE) {
		return nullptr;
	}
	if (BaseStatistics::GetStatsType(func.GetReturnType()) != StatisticsType::NUMERIC_STATS ||
	    func.GetReturnType().InternalType() == PhysicalType::BOOL) {
		return nullptr;
	}

	vector<Value> lo_args(func.GetChildren().size());
	vector<Value> hi_args(func.GetChildren().size());
	// SPECIAL_HANDLING means the function may produce nulls on non-null inputs (e.g. try_cast).
	bool output_can_have_null = (func.Function().GetNullHandling() != FunctionNullHandling::DEFAULT_NULL_HANDLING);

	for (idx_t i = 0; i < func.GetChildren().size(); i++) {
		auto &child = *func.GetChildren()[i];
		auto &cs = child_stats[i];
		auto &props = func.Function().GetArgProperties(i);

		if (child.IsFoldable()) {
			Value v;
			if (!ExpressionExecutor::TryEvaluateScalar(context, child, v) || v.IsNull()) {
				return nullptr;
			}
			lo_args[i] = v;
			hi_args[i] = std::move(v);
			continue;
		}

		auto m = props.monotonicity;
		if (cs.CanHaveNull()) {
			output_can_have_null = true;
		}
		if (cs.GetStatsType() != StatisticsType::NUMERIC_STATS || !NumericStats::HasMinMax(cs)) {
			return nullptr;
		}
		Value lo = NumericStats::Min(cs);
		Value hi = NumericStats::Max(cs);

		if (!IsKnownMonotonic(m)) {
			return nullptr;
		}

		if (m == Monotonicity::CONSTANT) {
			lo_args[i] = lo;
			hi_args[i] = std::move(lo);
			continue;
		}
		if (IsMonotonicIncreasing(m)) {
			lo_args[i] = std::move(lo);
			hi_args[i] = std::move(hi);
		} else {
			D_ASSERT(IsMonotonicDecreasing(m));
			lo_args[i] = std::move(hi);
			hi_args[i] = std::move(lo);
		}
	}

	Value out_lo, out_hi;
	if (!TryEvaluateAtConstants(context, func, lo_args, out_lo) ||
	    !TryEvaluateAtConstants(context, func, hi_args, out_hi)) {
		return nullptr;
	}
	// NaN/NULL at a corner means the input was NaN/NULL (column contains NaN, or year(±infinity)).
	// NaN is excluded even though DuckDB orders it above all other values: negate(NaN) = NaN, which
	// would violate NON_INCREASING if NaN were treated as a valid corner. Bail instead.
	const auto is_unusable = [](const Value &v) {
		if (v.IsNull()) {
			return true;
		}
		switch (v.type().id()) {
		case LogicalTypeId::DOUBLE:
			return Value::IsNan(v.GetValue<double>());
		case LogicalTypeId::FLOAT:
			return Value::IsNan(v.GetValue<float>());
		default:
			return false;
		}
	};
	if (is_unusable(out_lo) || is_unusable(out_hi)) {
		return nullptr;
	}
	if (out_hi < out_lo) {
		throw InternalException("Monotonic arg annotation violated for %s: output min exceeds output max",
		                        func.Function().GetName());
	}

	auto result = NumericStats::CreateEmpty(func.GetReturnType());
	NumericStats::SetMin(result, out_lo);
	NumericStats::SetMax(result, out_hi);

	result.Set(StatsInfo::CAN_HAVE_VALID_VALUES);
	if (output_can_have_null) {
		result.Set(StatsInfo::CAN_HAVE_NULL_VALUES);
	}
	return result.ToUnique();
}

unique_ptr<BaseStatistics> StatisticsPropagator::PropagateFiniteMonotoneBounds(ClientContext &context,
                                                                               FunctionStatisticsInput &input) {
	// -infinity gives NULL, and it can lie between the bounds of an argument
	for (auto &stats : input.child_stats) {
		if (stats.GetStatsType() == StatisticsType::NUMERIC_STATS && NumericStats::HasMinMax(stats) &&
		    HoldsNegativeInfinity(NumericStats::Min(stats), NumericStats::Max(stats))) {
			return nullptr;
		}
	}
	return PropagateMonotoneBounds(context, input.expr, input.child_stats);
}

unique_ptr<BaseStatistics> StatisticsPropagator::PropagateExpression(BoundFunctionExpression &func,
                                                                     unique_ptr<Expression> &expr_ptr) {
	if (func.GetExpressionType() == ExpressionType::COMPARE_BETWEEN) {
		return PropagateBetween(func, expr_ptr);
	}
	if (BoundComparisonExpression::IsComparison(func)) {
		return PropagateComparison(func, expr_ptr);
	}
	vector<BaseStatistics> stats;
	stats.reserve(func.GetChildrenMutable().size());
	bool has_invalidated_parameter = false;
	for (auto &child : func.GetChildrenMutable()) {
		auto stat = PropagateExpression(child);
		// an invalidated parameter has no statistics until it is rebound at execution
		if (child->GetExpressionType() == ExpressionType::VALUE_PARAMETER &&
		    child->GetReturnType().id() == LogicalTypeId::SQLNULL) {
			has_invalidated_parameter = true;
		}
		if (!stat) {
			stats.push_back(BaseStatistics::CreateUnknown(child->GetReturnType()));
		} else {
			stats.push_back(stat->Copy());
		}
	}
	if (has_invalidated_parameter) {
		return nullptr;
	}
	auto constant_stats = PropagateConstantInputs(context, func, stats);
	if (constant_stats) {
		return constant_stats;
	}
	if (func.Function().HasStatisticsCallback()) {
		FunctionStatisticsInput input(func, func.BindInfo().get(), stats, &expr_ptr);
		return func.Function().GetStatisticsCallback()(context, input);
	}
	return PropagateMonotoneBounds(context, func, stats);
}

} // namespace duckdb
