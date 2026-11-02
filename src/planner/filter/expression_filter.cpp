#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/function/arg_properties.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/cast/cast_statistics.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/expression/bound_between_expression.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/expression_barrier.hpp"
#include "duckdb/optimizer/statistics_propagator.hpp"
#include "duckdb/planner/filter/bloom_filter.hpp"
#include "duckdb/planner/filter/dynamic_filter.hpp"
#include "duckdb/planner/filter/optional_filter.hpp"
#include "duckdb/planner/filter/prefix_range_filter.hpp"
#include "duckdb/planner/filter/selectivity_optional_filter.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "duckdb/storage/statistics/struct_stats.hpp"
#include "duckdb/storage/statistics/string_stats.hpp"
#include "duckdb/storage/statistics/variant_stats.hpp"
#include "duckdb/function/scalar/struct_utils.hpp"

namespace duckdb {

ExpressionFilter::ExpressionFilter(unique_ptr<Expression> expr_p) : ExpressionFilter(std::move(expr_p), {}) {
}

ExpressionFilter::ExpressionFilter(unique_ptr<Expression> expr_p, vector<ProjectionIndex> column_indexes_p)
    : TableFilter(TableFilterType::EXPRESSION_FILTER), expr(std::move(expr_p)),
      column_indexes(std::move(column_indexes_p)) {
}

const ExpressionFilter &ExpressionFilter::GetExpressionFilter(const TableFilter &filter, const char *context) {
	D_ASSERT(filter.filter_type == TableFilterType::EXPRESSION_FILTER);
	if (filter.filter_type != TableFilterType::EXPRESSION_FILTER) {
		throw InternalException("%s expected ExpressionFilter", context);
	}
	return filter.Cast<ExpressionFilter>();
}

ExpressionFilter &ExpressionFilter::GetExpressionFilter(TableFilter &filter, const char *context) {
	D_ASSERT(filter.filter_type == TableFilterType::EXPRESSION_FILTER);
	if (filter.filter_type != TableFilterType::EXPRESSION_FILTER) {
		throw InternalException("%s expected ExpressionFilter", context);
	}
	return filter.Cast<ExpressionFilter>();
}

unique_ptr<Expression> ExpressionFilter::CreateInExpression(unique_ptr<Expression> column, vector<Value> values) {
	auto result = make_uniq<BoundOperatorExpression>(ExpressionType::COMPARE_IN, LogicalType::BOOLEAN);
	result->GetChildrenMutable().push_back(std::move(column));
	for (auto &value : values) {
		result->GetChildrenMutable().push_back(make_uniq<BoundConstantExpression>(std::move(value)));
	}
	return std::move(result);
}

unique_ptr<Expression> ExpressionFilter::CreateNullCheckExpression(unique_ptr<Expression> column,
                                                                   ExpressionType expression_type) {
	D_ASSERT(expression_type == ExpressionType::OPERATOR_IS_NULL ||
	         expression_type == ExpressionType::OPERATOR_IS_NOT_NULL);
	auto result = make_uniq<BoundOperatorExpression>(expression_type, LogicalType::BOOLEAN);
	result->GetChildrenMutable().push_back(std::move(column));
	return std::move(result);
}

unique_ptr<ExpressionFilter> ExpressionFilter::CreateComparisonFilter(ExpressionType comparison_type, Value constant) {
	auto column = make_uniq<BoundReferenceExpression>(constant.type(), 0ULL);
	auto comparison = BoundComparisonExpression::Create(comparison_type, std::move(column),
	                                                    make_uniq<BoundConstantExpression>(std::move(constant)));
	return make_uniq<ExpressionFilter>(std::move(comparison));
}

bool ExpressionFilter::IsSimpleFilterColumnRef(const Expression &expr) {
	return expr.GetExpressionClass() == ExpressionClass::BOUND_REF ||
	       expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF;
}

optional_ptr<const BoundConstantExpression>
ExpressionFilter::TryGetColumnConstantComparison(const BoundFunctionExpression &comparison,
                                                 ExpressionType &comparison_type) {
	auto &left = BoundComparisonExpression::Left(comparison);
	auto &right = BoundComparisonExpression::Right(comparison);
	comparison_type = comparison.GetExpressionType();
	if (IsSimpleFilterColumnRef(left) && right.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		return right.Cast<BoundConstantExpression>();
	}
	if (IsSimpleFilterColumnRef(right) && left.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		comparison_type = FlipComparisonExpression(comparison_type);
		return left.Cast<BoundConstantExpression>();
	}
	return nullptr;
}

static bool IsOptionalInternalFunction(const BoundFunctionExpression &func) {
	return func.Function().GetName() == OptionalFilterScalarFun::NAME ||
	       func.Function().GetName() == SelectivityOptionalFilterScalarFun::NAME;
}

static bool IsNonSelectivityOptionalInternalFunction(const BoundFunctionExpression &func) {
	return func.Function().GetName() == OptionalFilterScalarFun::NAME;
}

static bool IsOptionalExpressionInternal(const Expression &expr, bool recurse_through_and,
                                         bool include_selectivity_optional) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &func = expr.Cast<BoundFunctionExpression>();
		return include_selectivity_optional ? IsOptionalInternalFunction(func)
		                                    : IsNonSelectivityOptionalInternalFunction(func);
	}
	if (!recurse_through_and || expr.GetExpressionClass() != ExpressionClass::BOUND_CONJUNCTION ||
	    expr.GetExpressionType() != ExpressionType::CONJUNCTION_AND) {
		return false;
	}
	auto &conj = expr.Cast<BoundConjunctionExpression>();
	if (conj.GetChildren().empty()) {
		return false;
	}
	for (auto &child : conj.GetChildren()) {
		if (!IsOptionalExpressionInternal(*child, true, include_selectivity_optional)) {
			return false;
		}
	}
	return true;
}

static bool ContainsInternalTableFilterFunction(const Expression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &func = expr.Cast<BoundFunctionExpression>();
		if (TableFilterFunctions::IsTableFilterFunction(func.Function())) {
			return true;
		}
		auto optional_child = ExpressionFilter::GetOptionalFilterChild(func);
		if (optional_child && ContainsInternalTableFilterFunction(*optional_child)) {
			return true;
		}
	}
	bool found = false;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		if (!found) {
			found = ContainsInternalTableFilterFunction(child);
		}
	});
	return found;
}

static unique_ptr<Expression> UnwrapOptionalFiltersForConstantEvaluation(const Expression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &func = expr.Cast<BoundFunctionExpression>();
		if (IsOptionalInternalFunction(func) && func.BindInfo()) {
			auto optional_child = ExpressionFilter::GetOptionalFilterChild(func);
			return optional_child ? UnwrapOptionalFiltersForConstantEvaluation(*optional_child)
			                      : make_uniq<BoundConstantExpression>(Value::BOOLEAN(true));
		}
	}
	auto result = expr.Copy();
	ExpressionIterator::EnumerateChildren(
	    *result, [&](unique_ptr<Expression> &child) { child = UnwrapOptionalFiltersForConstantEvaluation(*child); });
	return result;
}

bool ExpressionFilter::EvaluateWithConstant(ClientContext &context, const Value &val) const {
	auto constant_eval_expr = UnwrapOptionalFiltersForConstantEvaluation(*expr);
	ExpressionExecutor executor(context, *constant_eval_expr);
	return EvaluateWithConstant(executor, val);
}

bool ExpressionFilter::EvaluateWithConstant(ExpressionExecutor &executor, const Value &val) const {
	DataChunk input;
	input.data.emplace_back(val, count_t(1));

	SelectionVector sel(1);

	idx_t count = executor.SelectExpression(input, sel);
	return count > 0;
}

FilterPropagateResult ExpressionFilter::CheckStatistics(const BaseStatistics &stats) const {
	return CheckExpressionStatistics(*expr, stats);
}

FilterPropagateResult ExpressionFilter::CheckStatistics(ClientContext &context, const BaseStatistics &stats) const {
	return CheckExpressionStatistics(&context, *expr, stats);
}

static FilterPropagateResult CheckZonemapAgainstConstants(const BaseStatistics &stats, ExpressionType comparison_type,
                                                          array_ptr<const Value> values) {
	D_ASSERT(values.size() > 0);
	switch (values[0].type().InternalType()) {
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
		return NumericStats::CheckZonemap(stats, comparison_type, values);
	case PhysicalType::VARCHAR:
		if (stats.GetStatsType() == StatisticsType::STRING_STATS) {
			return StringStats::CheckZonemap(stats, comparison_type, values);
		}
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	default:
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
}

static optional_ptr<const BaseStatistics> TryGetExpressionStats(optional_ptr<ClientContext> context_p,
                                                                const Expression &expr,
                                                                array_ptr<const BaseStatistics> input_stats,
                                                                vector<unique_ptr<BaseStatistics>> &owned_stats) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_REF: {
		auto index = expr.Cast<BoundReferenceExpression>().Index();
		if (index >= input_stats.size()) {
			return nullptr;
		}
		return &input_stats[index];
	}
	case ExpressionClass::BOUND_CONSTANT: {
		auto &constant = expr.Cast<BoundConstantExpression>().GetValue();
		owned_stats.push_back(BaseStatistics::FromConstant(constant).ToUnique());
		return owned_stats.back().get();
	}
	case ExpressionClass::BOUND_FUNCTION: {
		auto &func = expr.Cast<BoundFunctionExpression>();

		if (ExpressionBarrier::IsBarrier(func)) {
			// the barrier returns its argument unchanged
			return TryGetExpressionStats(context_p, *func.GetChildren()[0], input_stats, owned_stats);
		}
		if (BoundCastExpression::IsCast(func)) {
			auto &cast_child = BoundCastExpression::Child(func);
			auto child_stats = TryGetExpressionStats(context_p, cast_child, input_stats, owned_stats);
			if (!child_stats) {
				return nullptr;
			}
			auto expr_copy = func.Copy();
			auto &func_copy = expr_copy->Cast<BoundFunctionExpression>();
			auto cast_stats = BoundCastExpression::PropagateStatistics(func_copy, *child_stats, context_p);
			if (!cast_stats) {
				return nullptr;
			}
			owned_stats.push_back(std::move(cast_stats));
			return owned_stats.back().get();
		}

		if (!context_p) {
			// Since statistics callback need context, so this path is the fallback
			idx_t child_idx;
			if (TryGetStructExtractChildIndex(func, child_idx) && !func.GetChildren().empty()) {
				auto child_stats = TryGetExpressionStats(context_p, *func.GetChildren()[0], input_stats, owned_stats);
				if (!child_stats || child_stats->GetType().id() != LogicalTypeId::STRUCT) {
					return nullptr;
				}
				owned_stats.push_back(StructStats::GetChildStats(*child_stats, child_idx).ToUnique());
				return owned_stats.back().get();
			}
			return nullptr;
		}

		if (func.Function().HasStatisticsCallback()) {
			vector<BaseStatistics> child_stats;
			child_stats.reserve(func.GetChildren().size());
			for (auto &child_expr : func.GetChildren()) {
				auto child_stat = TryGetExpressionStats(context_p, *child_expr, input_stats, owned_stats);
				if (!child_stat) {
					return nullptr;
				}
				child_stats.push_back(child_stat->Copy());
			}

			auto constant_stats = StatisticsPropagator::PropagateConstantInputs(*context_p, func, child_stats);
			if (constant_stats) {
				owned_stats.push_back(std::move(constant_stats));
				return owned_stats.back().get();
			}
			// Use copy to avoid expression rewritten
			auto expr_copy = func.Copy();
			auto &func_copy = expr_copy->Cast<BoundFunctionExpression>();
			FunctionStatisticsInput input(func_copy, func_copy.BindInfo(), child_stats, &expr_copy);
			owned_stats.push_back(func.Function().GetStatisticsCallback()(*context_p, input));
			return owned_stats.back().get();
		}

		// No custom callback: fall back to the declared monotonicity (ArgProperties) and derive output
		// bounds by evaluating the function at the corners of each argument's range. This lets a
		// f(col) OP const filter prune row groups via the zonemap of the base column.
		if (func.Function().GetStability() != FunctionStability::VOLATILE) {
			vector<BaseStatistics> child_stats;
			child_stats.reserve(func.GetChildren().size());
			for (auto &child_expr : func.GetChildren()) {
				auto child_stat = TryGetExpressionStats(context_p, *child_expr, input_stats, owned_stats);
				child_stats.push_back(child_stat ? child_stat->Copy()
				                                 : BaseStatistics::CreateUnknown(child_expr->GetReturnType()));
			}
			auto derived = StatisticsPropagator::PropagateConstantInputs(*context_p, func, child_stats);
			if (!derived) {
				derived = StatisticsPropagator::PropagateMonotoneBounds(*context_p, func, child_stats);
			}
			if (derived) {
				owned_stats.push_back(std::move(derived));
				return owned_stats.back().get();
			}
		}

		return nullptr;
	}
	case ExpressionClass::BOUND_CASE: {
		// Output range is the union of the ELSE and every THEN branch.
		auto &case_expr = expr.Cast<BoundCaseExpression>();
		auto else_stats = TryGetExpressionStats(context_p, case_expr.Else(), input_stats, owned_stats);
		if (!else_stats) {
			return nullptr;
		}
		auto merged = else_stats->ToUnique();
		for (auto &check : case_expr.CaseChecks()) {
			auto then_stats = TryGetExpressionStats(context_p, *check.then_expr, input_stats, owned_stats);
			if (!then_stats) {
				return nullptr;
			}
			merged->Merge(*then_stats);
		}
		owned_stats.push_back(std::move(merged));
		return owned_stats.back().get();
	}
	case ExpressionClass::BOUND_OPERATOR: {
		auto &op = expr.Cast<BoundOperatorExpression>();
		if (op.GetExpressionType() != ExpressionType::OPERATOR_COALESCE || op.GetChildren().empty()) {
			return nullptr;
		}
		// COALESCE returns the first non-NULL child: merge the stats of every reachable child,
		// stopping at the first child that can never be NULL since later children are unreachable
		unique_ptr<BaseStatistics> merged;
		bool last_child_can_be_null = true;
		for (auto &child : op.GetChildren()) {
			auto child_stats = TryGetExpressionStats(context_p, *child, input_stats, owned_stats);
			if (!child_stats) {
				return nullptr;
			}
			if (!merged) {
				merged = child_stats->ToUnique();
			} else {
				merged->Merge(*child_stats);
			}
			last_child_can_be_null = child_stats->CanHaveNull();
			if (!last_child_can_be_null) {
				break;
			}
		}
		if (!last_child_can_be_null) {
			// the result is NULL only if every considered child is NULL
			merged->Set(StatsInfo::CANNOT_HAVE_NULL_VALUES);
		}
		owned_stats.push_back(std::move(merged));
		return owned_stats.back().get();
	}
	default:
		return nullptr;
	}
}

static bool CanDeriveExpressionStatistics(const Expression &expr) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_COLUMN_REF:
	case ExpressionClass::BOUND_REF:
		return true;
	case ExpressionClass::BOUND_CONSTANT:
		return !expr.Cast<BoundConstantExpression>().GetValue().IsNull();
	case ExpressionClass::BOUND_FUNCTION: {
		const auto &func = expr.Cast<BoundFunctionExpression>();
		if (BoundCastExpression::IsCast(func)) {
			return BoundCastExpression::GetBoundCast(func).HasStatisticsCallback() &&
			       CanDeriveExpressionStatistics(BoundCastExpression::Child(func));
		}
		if (func.Function().HasStatisticsCallback()) {
			for (const auto &child : func.GetChildren()) {
				if (!CanDeriveExpressionStatistics(*child)) {
					return false;
				}
			}
			return true;
		}
		if (!func.Function().HasArgProperties() || func.GetChildren().empty() ||
		    BaseStatistics::GetStatsType(func.GetReturnType()) != StatisticsType::NUMERIC_STATS ||
		    func.GetReturnType().InternalType() == PhysicalType::BOOL) {
			return false;
		}
		for (idx_t child_idx = 0; child_idx < func.GetChildren().size(); ++child_idx) {
			const auto &child = *func.GetChildren()[child_idx];
			if (child.IsFoldable()) {
				continue;
			}
			if (!IsKnownMonotonic(func.Function().GetArgProperties(child_idx).monotonicity) ||
			    !CanDeriveExpressionStatistics(child)) {
				return false;
			}
		}
		return true;
	}
	default:
		return false;
	}
}

static bool CanPropagateExpressionStatisticsInternal(const Expression &expr) {
	if (BoundComparisonExpression::IsComparison(expr)) {
		switch (expr.GetExpressionType()) {
		case ExpressionType::COMPARE_EQUAL:
		case ExpressionType::COMPARE_NOTEQUAL:
		case ExpressionType::COMPARE_GREATERTHAN:
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		case ExpressionType::COMPARE_LESSTHAN:
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
			break;
		default:
			return false;
		}
		const auto &comparison = expr.Cast<BoundFunctionExpression>();
		return CanDeriveExpressionStatistics(BoundComparisonExpression::Left(comparison)) &&
		       CanDeriveExpressionStatistics(BoundComparisonExpression::Right(comparison));
	}
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_CONJUNCTION) {
		return false;
	}
	const auto &conjunction = expr.Cast<BoundConjunctionExpression>();
	if ((conjunction.GetExpressionType() != ExpressionType::CONJUNCTION_AND &&
	     conjunction.GetExpressionType() != ExpressionType::CONJUNCTION_OR) ||
	    conjunction.GetChildren().empty()) {
		return false;
	}
	for (const auto &child : conjunction.GetChildren()) {
		if (!CanPropagateExpressionStatisticsInternal(*child)) {
			return false;
		}
	}
	return true;
}

bool ExpressionFilter::CanPropagateExpressionStatistics(const Expression &expr) {
	return expr.IsConsistent() && !expr.CanThrow() && CanPropagateExpressionStatisticsInternal(expr);
}

static optional_ptr<const BaseStatistics> TryGetFilterStats(optional_ptr<ClientContext> context_p,
                                                            const Expression &expr,
                                                            array_ptr<const BaseStatistics> input_stats,
                                                            vector<unique_ptr<BaseStatistics>> &owned_stats) {
	return TryGetExpressionStats(context_p, expr, input_stats, owned_stats);
}

unique_ptr<BaseStatistics> ExpressionFilter::TryGetExpressionStatistics(ClientContext &context, const Expression &expr,
                                                                        array_ptr<const BaseStatistics> input_stats) {
	vector<unique_ptr<BaseStatistics>> owned_stats;
	auto result = TryGetExpressionStats(&context, expr, input_stats, owned_stats);
	return result ? result->ToUnique() : nullptr;
}

static bool TryGetVariantComparisonStatsType(const LogicalType &typed_type, const LogicalType &constant_type,
                                             LogicalType &comparison_type) {
	if (typed_type == constant_type) {
		comparison_type = typed_type;
		return true;
	}
	if (!typed_type.IsIntegral() || !constant_type.IsIntegral()) {
		return false;
	}
	if (!LogicalType::DefaultTryGetMaxLogicalTypeUnchecked(typed_type, constant_type, comparison_type)) {
		return false;
	}
	return comparison_type.IsIntegral();
}

static optional_ptr<const BaseStatistics>
TryPrepareVariantComparisonStats(const BaseStatistics &stats, Value &constant,
                                 vector<unique_ptr<BaseStatistics>> &owned_stats) {
	if (stats.GetType().id() != LogicalTypeId::VARIANT) {
		return constant.type().id() == LogicalTypeId::VARIANT ? nullptr : &stats;
	}
	if (!VariantStats::IsShredded(stats)) {
		return nullptr;
	}
	auto &shredded_stats = VariantStats::GetShreddedStats(stats);
	if (!VariantShreddedStats::IsFullyShredded(shredded_stats)) {
		return nullptr;
	}
	auto &typed_stats = VariantStats::GetTypedStats(shredded_stats);
	auto &typed_type = typed_stats.GetType();
	if (typed_type.IsNested() || typed_type.id() == LogicalTypeId::VARIANT) {
		return nullptr;
	}

	if (constant.type().id() == LogicalTypeId::VARIANT) {
		constant = VariantValue::GetValue(constant);
	}
	if (constant.IsNull()) {
		return nullptr;
	}

	LogicalType comparison_type;
	if (!TryGetVariantComparisonStatsType(typed_type, constant.type(), comparison_type)) {
		return nullptr;
	}
	auto cast_constant = constant.DefaultTryCastAs(comparison_type);
	if (!cast_constant) {
		return nullptr;
	}
	constant = std::move(*cast_constant);
	if (typed_type == comparison_type) {
		return &typed_stats;
	}

	auto cast_stats = CastStatistics::TryPropagate(typed_stats, typed_type, comparison_type);
	if (!cast_stats) {
		return nullptr;
	}
	owned_stats.push_back(std::move(cast_stats));
	return owned_stats.back().get();
}

static FilterPropagateResult CheckComparisonStatistics(optional_ptr<ClientContext> context_p,
                                                       const BoundFunctionExpression &comp_expr,
                                                       array_ptr<const BaseStatistics> input_stats) {
	vector<unique_ptr<BaseStatistics>> owned_stats;
	optional_ptr<const BaseStatistics> filter_stats;
	optional_ptr<const BoundConstantExpression> constant_expr;
	auto comparison_type = comp_expr.GetExpressionType();
	auto &left = BoundComparisonExpression::Left(comp_expr);
	auto &right = BoundComparisonExpression::Right(comp_expr);
	if (right.GetExpressionType() == ExpressionType::VALUE_CONSTANT) {
		filter_stats = TryGetFilterStats(context_p, left, input_stats, owned_stats);
		constant_expr = &right.Cast<BoundConstantExpression>();
	} else if (left.GetExpressionType() == ExpressionType::VALUE_CONSTANT) {
		filter_stats = TryGetFilterStats(context_p, right, input_stats, owned_stats);
		constant_expr = &left.Cast<BoundConstantExpression>();
		comparison_type = FlipComparisonExpression(comparison_type);
	} else {
		auto left_stats = TryGetFilterStats(context_p, left, input_stats, owned_stats);
		auto right_stats = TryGetFilterStats(context_p, right, input_stats, owned_stats);
		if (!left_stats || !right_stats) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		return StatisticsPropagator::PropagateComparison(*left_stats, *right_stats, comparison_type);
	}
	if (!filter_stats || !constant_expr) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	auto &constant = constant_expr->GetValue();
	if (constant.IsNull()) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	auto comparison_constant = constant;
	const bool variant_comparison = filter_stats->GetType().id() == LogicalTypeId::VARIANT ||
	                                comparison_constant.type().id() == LogicalTypeId::VARIANT;
	if (variant_comparison) {
		filter_stats = TryPrepareVariantComparisonStats(*filter_stats, comparison_constant, owned_stats);
		if (!filter_stats) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
	}
	if (!filter_stats->CanHaveNoNull()) {
		if (variant_comparison) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		return comparison_type == ExpressionType::COMPARE_DISTINCT_FROM ? FilterPropagateResult::FILTER_ALWAYS_TRUE
		                                                                : FilterPropagateResult::FILTER_ALWAYS_FALSE;
	}
	auto result =
	    CheckZonemapAgainstConstants(*filter_stats, comparison_type, array_ptr<const Value>(&comparison_constant, 1));
	if (filter_stats->CanHaveNull()) {
		if (result == FilterPropagateResult::FILTER_ALWAYS_FALSE &&
		    comparison_type != ExpressionType::COMPARE_DISTINCT_FROM) {
			return result;
		}
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	return result;
}

//! `input BETWEEN lower AND upper` prunes when the input's derived range is entirely outside
//! [lower, upper] (always-false) or entirely inside it (always-true). Mirrors the null-handling of
//! CheckComparisonStatistics. `input` may itself be a monotone function whose stats are derived.
static FilterPropagateResult CheckBetweenStatistics(optional_ptr<ClientContext> context_p,
                                                    const BoundFunctionExpression &between,
                                                    array_ptr<const BaseStatistics> input_stats) {
	auto &lower = BoundBetweenExpression::LowerBound(between);
	auto &upper = BoundBetweenExpression::UpperBound(between);
	if (lower.GetExpressionType() != ExpressionType::VALUE_CONSTANT ||
	    upper.GetExpressionType() != ExpressionType::VALUE_CONSTANT) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	auto &lower_val = lower.Cast<BoundConstantExpression>().GetValue();
	auto &upper_val = upper.Cast<BoundConstantExpression>().GetValue();
	if (lower_val.IsNull() || upper_val.IsNull()) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	vector<unique_ptr<BaseStatistics>> owned_stats;
	auto between_stats = TryGetFilterStats(context_p, BoundBetweenExpression::Input(between), input_stats, owned_stats);
	if (!between_stats || between_stats->GetType().id() == LogicalTypeId::VARIANT) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	if (!between_stats->CanHaveNoNull()) {
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	}
	auto lower_res = CheckZonemapAgainstConstants(*between_stats, BoundBetweenExpression::LowerComparisonType(between),
	                                              array_ptr<const Value>(&lower_val, 1));
	auto upper_res = CheckZonemapAgainstConstants(*between_stats, BoundBetweenExpression::UpperComparisonType(between),
	                                              array_ptr<const Value>(&upper_val, 1));
	if (between_stats->CanHaveNull()) {
		if (lower_res == FilterPropagateResult::FILTER_ALWAYS_FALSE ||
		    upper_res == FilterPropagateResult::FILTER_ALWAYS_FALSE) {
			return FilterPropagateResult::FILTER_ALWAYS_FALSE;
		}
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	if (lower_res == FilterPropagateResult::FILTER_ALWAYS_FALSE ||
	    upper_res == FilterPropagateResult::FILTER_ALWAYS_FALSE) {
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	}
	if (lower_res == FilterPropagateResult::FILTER_ALWAYS_TRUE &&
	    upper_res == FilterPropagateResult::FILTER_ALWAYS_TRUE) {
		return FilterPropagateResult::FILTER_ALWAYS_TRUE;
	}
	return FilterPropagateResult::NO_PRUNING_POSSIBLE;
}

static FilterPropagateResult CheckBoolStatistics(const BaseStatistics &stats, bool negated) {
	if (stats.GetType().id() != LogicalTypeId::BOOLEAN) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	if (!stats.CanHaveNoNull()) {
		return FilterPropagateResult::FILTER_FALSE_OR_NULL;
	}
	if (!NumericStats::HasMinMax(stats)) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	const auto min_v = NumericStats::Min(stats).GetValue<bool>();
	const auto max_v = NumericStats::Max(stats).GetValue<bool>();
	if (min_v != max_v) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	const bool value = negated ? !min_v : min_v;
	if (!value) {
		return stats.CanHaveNull() ? FilterPropagateResult::FILTER_FALSE_OR_NULL
		                           : FilterPropagateResult::FILTER_ALWAYS_FALSE;
	}
	return stats.CanHaveNull() ? FilterPropagateResult::FILTER_TRUE_OR_NULL : FilterPropagateResult::FILTER_ALWAYS_TRUE;
}

static FilterPropagateResult CheckFunctionStatistics(optional_ptr<ClientContext> context_p,
                                                     const BoundFunctionExpression &func_expr,
                                                     array_ptr<const BaseStatistics> input_stats) {
	if (ExpressionBarrier::IsBarrier(func_expr)) {
		// pruning never evaluates the expression, and only ever removes rows - the barrier can be seen through
		return ExpressionFilter::CheckExpressionStatistics(context_p, *func_expr.GetChildren()[0], input_stats);
	}
	if (func_expr.GetExpressionType() == ExpressionType::COMPARE_BETWEEN) {
		return CheckBetweenStatistics(context_p, func_expr, input_stats);
	}
	if (BoundComparisonExpression::IsComparison(func_expr.GetExpressionType())) {
		return CheckComparisonStatistics(context_p, func_expr, input_stats);
	}
	if (!func_expr.Function().HasFilterPruneCallback()) {
		if (func_expr.GetReturnType().id() != LogicalTypeId::BOOLEAN) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		vector<unique_ptr<BaseStatistics>> owned_stats;
		const auto function_stats = TryGetFilterStats(context_p, func_expr, input_stats, owned_stats);
		if (!function_stats) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		return CheckBoolStatistics(*function_stats, /*negated=*/false);
	}
	// Derive the statistics of each argument. This lets a callback prune regardless of which argument is the column and
	// which is the constant (e.g. `foo(col, const)` vs `foo(const, col`).
	// Entries are null when an argument's stats can't be derived.
	vector<unique_ptr<BaseStatistics>> owned_stats;
	vector<optional_ptr<const BaseStatistics>> child_stats;
	child_stats.reserve(func_expr.GetChildren().size());
	for (auto &child : func_expr.GetChildren()) {
		child_stats.push_back(TryGetFilterStats(context_p, *child, input_stats, owned_stats));
	}
	FunctionStatisticsPruneInput input(func_expr, func_expr.BindInfo().get(), child_stats);
	return func_expr.Function().GetFilterPruneCallback()(input);
}

static FilterPropagateResult CheckNullOperatorStatistics(optional_ptr<ClientContext> context_p,
                                                         const BoundOperatorExpression &op_expr,
                                                         array_ptr<const BaseStatistics> input_stats,
                                                         ExpressionType operator_type) {
	if (op_expr.GetChildren().empty()) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	vector<unique_ptr<BaseStatistics>> owned_stats;
	auto filter_stats = TryGetFilterStats(context_p, *op_expr.GetChildren()[0], input_stats, owned_stats);
	if (!filter_stats) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	if (operator_type == ExpressionType::OPERATOR_IS_NULL) {
		if (!filter_stats->CanHaveNull()) {
			return FilterPropagateResult::FILTER_ALWAYS_FALSE;
		}
		if (!filter_stats->CanHaveNoNull()) {
			return FilterPropagateResult::FILTER_ALWAYS_TRUE;
		}
	} else {
		if (!filter_stats->CanHaveNoNull()) {
			return FilterPropagateResult::FILTER_ALWAYS_FALSE;
		}
		if (!filter_stats->CanHaveNull()) {
			return FilterPropagateResult::FILTER_ALWAYS_TRUE;
		}
	}
	return FilterPropagateResult::NO_PRUNING_POSSIBLE;
}

static FilterPropagateResult CheckInOperatorStatistics(optional_ptr<ClientContext> context_p,
                                                       const BoundOperatorExpression &op_expr,
                                                       array_ptr<const BaseStatistics> input_stats) {
	if (op_expr.GetChildren().size() <= 1) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	vector<unique_ptr<BaseStatistics>> owned_stats;
	auto filter_stats = TryGetFilterStats(context_p, *op_expr.GetChildren()[0], input_stats, owned_stats);
	if (!filter_stats) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	vector<Value> values;
	values.reserve(op_expr.GetChildren().size() - 1);
	for (idx_t i = 1; i < op_expr.GetChildren().size(); i++) {
		if (op_expr.GetChildren()[i]->GetExpressionType() != ExpressionType::VALUE_CONSTANT) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		auto &value = op_expr.GetChildren()[i]->Cast<BoundConstantExpression>().GetValue();
		if (!value.IsNull()) {
			values.push_back(value);
		}
	}
	if (values.empty()) {
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	}
	if (!filter_stats->CanHaveNoNull()) {
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	}
	array_ptr<const Value> constants(values.data(), values.size());
	auto result = CheckZonemapAgainstConstants(*filter_stats, ExpressionType::COMPARE_EQUAL, constants);
	if (result == FilterPropagateResult::NO_PRUNING_POSSIBLE &&
	    NumericStats::ConstantsCoverRange(*filter_stats, constants)) {
		// an integral column whose whole [min, max] range is listed always matches
		result = FilterPropagateResult::FILTER_ALWAYS_TRUE;
	}
	if (result == FilterPropagateResult::FILTER_ALWAYS_TRUE && filter_stats->CanHaveNull()) {
		return FilterPropagateResult::FILTER_TRUE_OR_NULL;
	}
	return result;
}

static FilterPropagateResult CheckBoolRefStatistics(const Expression &expr, array_ptr<const BaseStatistics> input_stats,
                                                    bool negated) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_REF ||
	    expr.GetReturnType().id() != LogicalTypeId::BOOLEAN) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	auto index = expr.Cast<BoundReferenceExpression>().Index();
	if (index >= input_stats.size()) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	return CheckBoolStatistics(input_stats[index], negated);
}

static FilterPropagateResult CheckNotOperatorStatistics(optional_ptr<ClientContext> context_p,
                                                        const BoundOperatorExpression &op_expr,
                                                        array_ptr<const BaseStatistics> input_stats) {
	if (op_expr.GetChildren().size() != 1) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	auto &child = *op_expr.GetChildren()[0];
	if (child.GetExpressionType() == ExpressionType::COMPARE_IN) {
		auto &children = child.Cast<BoundOperatorExpression>().GetChildren();
		if (children.size() == 2 && children[1]->GetExpressionType() == ExpressionType::VALUE_CONSTANT &&
		    children[1]->Cast<BoundConstantExpression>().GetValue().IsNull()) {
			return FilterPropagateResult::FILTER_FALSE_OR_NULL;
		}
	}
	auto bool_ref = CheckBoolRefStatistics(child, input_stats, true);
	if (bool_ref != FilterPropagateResult::NO_PRUNING_POSSIBLE) {
		return bool_ref;
	}
	// a child matching every row makes NOT match no row - the converse does not hold, since a child
	// that matches no row may be NULL rather than false, and NOT NULL does not match either
	auto child_result = ExpressionFilter::CheckExpressionStatistics(context_p, child, input_stats);
	if (child_result == FilterPropagateResult::FILTER_ALWAYS_TRUE) {
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	}
	if (child_result == FilterPropagateResult::FILTER_TRUE_OR_NULL) {
		return FilterPropagateResult::FILTER_FALSE_OR_NULL;
	}
	return FilterPropagateResult::NO_PRUNING_POSSIBLE;
}

static FilterPropagateResult CheckOperatorStatistics(optional_ptr<ClientContext> context_p,
                                                     const BoundOperatorExpression &op_expr,
                                                     array_ptr<const BaseStatistics> input_stats) {
	switch (op_expr.GetExpressionType()) {
	case ExpressionType::OPERATOR_IS_NULL:
	case ExpressionType::OPERATOR_IS_NOT_NULL:
		return CheckNullOperatorStatistics(context_p, op_expr, input_stats, op_expr.GetExpressionType());
	case ExpressionType::COMPARE_IN:
		return CheckInOperatorStatistics(context_p, op_expr, input_stats);
	case ExpressionType::OPERATOR_NOT:
		return CheckNotOperatorStatistics(context_p, op_expr, input_stats);
	default:
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
}

static FilterPropagateResult CheckConjunctionStatistics(optional_ptr<ClientContext> context_p,
                                                        const BoundConjunctionExpression &conj,
                                                        array_ptr<const BaseStatistics> input_stats) {
	switch (conj.GetExpressionType()) {
	case ExpressionType::CONJUNCTION_AND: {
		auto result = FilterPropagateResult::FILTER_ALWAYS_TRUE;
		for (auto &child : conj.GetChildren()) {
			auto prune_result = ExpressionFilter::CheckExpressionStatistics(context_p, *child, input_stats);
			if (prune_result == FilterPropagateResult::FILTER_ALWAYS_FALSE) {
				return FilterPropagateResult::FILTER_ALWAYS_FALSE;
			}
			if (prune_result == FilterPropagateResult::FILTER_FALSE_OR_NULL) {
				return FilterPropagateResult::FILTER_FALSE_OR_NULL;
			}
			if (prune_result != result) {
				result = FilterPropagateResult::NO_PRUNING_POSSIBLE;
			}
		}
		return result;
	}
	case ExpressionType::CONJUNCTION_OR: {
		D_ASSERT(!conj.GetChildren().empty());
		auto result = FilterPropagateResult::FILTER_ALWAYS_FALSE;
		for (auto &child : conj.GetChildren()) {
			auto prune_result = ExpressionFilter::CheckExpressionStatistics(context_p, *child, input_stats);
			if (prune_result == FilterPropagateResult::NO_PRUNING_POSSIBLE) {
				return FilterPropagateResult::NO_PRUNING_POSSIBLE;
			}
			if (prune_result == FilterPropagateResult::FILTER_ALWAYS_TRUE) {
				return FilterPropagateResult::FILTER_ALWAYS_TRUE;
			}
			if (prune_result == FilterPropagateResult::FILTER_TRUE_OR_NULL) {
				result = FilterPropagateResult::FILTER_TRUE_OR_NULL;
			} else if (prune_result == FilterPropagateResult::FILTER_FALSE_OR_NULL &&
			           result == FilterPropagateResult::FILTER_ALWAYS_FALSE) {
				result = FilterPropagateResult::FILTER_FALSE_OR_NULL;
			}
		}
		return result;
	}
	default:
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
}

FilterPropagateResult ExpressionFilter::CheckExpressionStatistics(const Expression &expr, const BaseStatistics &stats) {
	return CheckExpressionStatistics(nullptr, expr, array_ptr<const BaseStatistics>(stats));
}

FilterPropagateResult ExpressionFilter::CheckExpressionStatistics(optional_ptr<ClientContext> context_p,
                                                                  const Expression &expr, const BaseStatistics &stats) {
	return CheckExpressionStatistics(context_p, expr, array_ptr<const BaseStatistics>(stats));
}

FilterPropagateResult ExpressionFilter::CheckExpressionStatistics(const Expression &expr,
                                                                  array_ptr<const BaseStatistics> input_stats) {
	return CheckExpressionStatistics(nullptr, expr, input_stats);
}

FilterPropagateResult ExpressionFilter::CheckExpressionStatistics(optional_ptr<ClientContext> context_p,
                                                                  const Expression &expr,
                                                                  array_ptr<const BaseStatistics> input_stats) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_FUNCTION:
		return CheckFunctionStatistics(context_p, expr.Cast<BoundFunctionExpression>(), input_stats);
	case ExpressionClass::BOUND_OPERATOR:
		return CheckOperatorStatistics(context_p, expr.Cast<BoundOperatorExpression>(), input_stats);
	case ExpressionClass::BOUND_CONJUNCTION:
		return CheckConjunctionStatistics(context_p, expr.Cast<BoundConjunctionExpression>(), input_stats);
	case ExpressionClass::BOUND_REF:
		return CheckBoolRefStatistics(expr, input_stats, false);
	default:
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
}

bool ExpressionFilter::ContainsInternalFunction(const Expression &expr, const string &func_name) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &func = expr.Cast<BoundFunctionExpression>();
		if (func.Function().GetName() == func_name) {
			return true;
		}
		auto optional_child = GetOptionalFilterChild(func);
		if (optional_child && ContainsInternalFunction(*optional_child, func_name)) {
			return true;
		}
	}
	bool found = false;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		if (!found) {
			found = ContainsInternalFunction(child, func_name);
		}
	});
	return found;
}

bool ExpressionFilter::IsOptionalExpression(const Expression &expr) {
	return IsOptionalExpressionInternal(expr, true, true);
}

bool ExpressionFilter::IsRootOptionalExpression(const Expression &expr) {
	return IsOptionalExpressionInternal(expr, false, true);
}

optional_ptr<const Expression> ExpressionFilter::GetOptionalFilterChild(const Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return nullptr;
	}
	auto &func = expr.Cast<BoundFunctionExpression>();
	if (!func.BindInfo()) {
		return nullptr;
	}
	if (func.Function().GetName() == OptionalFilterScalarFun::NAME) {
		return func.BindInfo()->Cast<OptionalFilterFunctionData>().child_filter_expr.get();
	}
	if (func.Function().GetName() == SelectivityOptionalFilterScalarFun::NAME) {
		return func.BindInfo()->Cast<SelectivityOptionalFilterFunctionData>().child_filter_expr.get();
	}
	return nullptr;
}

bool ExpressionFilter::IsOptionalFilter(const TableFilter &filter) {
	auto &expr_filter = GetExpressionFilter(filter, "ExpressionFilter::IsOptionalFilter");
	return IsOptionalExpression(*expr_filter.expr);
}

bool ExpressionFilter::IsRootOptionalFilter(const TableFilter &filter) {
	auto &expr_filter = GetExpressionFilter(filter, "ExpressionFilter::IsRootOptionalFilter");
	return IsRootOptionalExpression(*expr_filter.expr);
}

bool ExpressionFilter::IsRootNonSelectivityOptionalFilter(const TableFilter &filter) {
	auto &expr_filter = GetExpressionFilter(filter, "ExpressionFilter::IsRootNonSelectivityOptionalFilter");
	return IsOptionalExpressionInternal(*expr_filter.expr, false, false);
}

static shared_ptr<DynamicFilterData> TryGetRootDynamicFilterData(const Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return nullptr;
	}
	auto &func = expr.Cast<BoundFunctionExpression>();
	if (func.Function().GetName() != DynamicFilterScalarFun::NAME || !func.BindInfo()) {
		return nullptr;
	}
	return func.BindInfo()->Cast<DynamicFilterFunctionData>().filter_data;
}

static shared_ptr<DynamicFilterData> TryGetOptionalDynamicFilterData(const Expression &expr) {
	if (expr.GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
		for (auto &child : expr.Cast<BoundConjunctionExpression>().GetChildren()) {
			auto filter_data = TryGetOptionalDynamicFilterData(*child);
			if (filter_data) {
				return filter_data;
			}
		}
		return nullptr;
	}
	auto optional_child = ExpressionFilter::GetOptionalFilterChild(expr);
	return optional_child ? TryGetRootDynamicFilterData(*optional_child) : nullptr;
}

shared_ptr<DynamicFilterData> ExpressionFilter::GetRootOptionalDynamicFilterData(const TableFilter &filter) {
	auto &expr_filter = GetExpressionFilter(filter, "ExpressionFilter::GetRootOptionalDynamicFilterData");
	if (expr_filter.expr->GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
		return nullptr;
	}
	return TryGetOptionalDynamicFilterData(*expr_filter.expr);
}

shared_ptr<DynamicFilterData> ExpressionFilter::GetOptionalDynamicFilterData(const TableFilter &filter) {
	auto &expr_filter = GetExpressionFilter(filter, "ExpressionFilter::GetOptionalDynamicFilterData");
	return TryGetOptionalDynamicFilterData(*expr_filter.expr);
}

unique_ptr<ExpressionFilter> ExpressionFilter::FromTableFilter(const TableFilter &filter, const LogicalType &col_type) {
	if (filter.filter_type == TableFilterType::EXPRESSION_FILTER) {
		auto &expr_filter = filter.Cast<ExpressionFilter>();
		return expr_filter.Copy();
	}
	if (col_type == LogicalType::ANY) {
		throw InternalException("ExpressionFilter::FromTableFilter requires the actual column type");
	}
	storage_t col_idx = 0;
	auto col_ref = make_uniq<BoundReferenceExpression>(col_type, col_idx);
	auto expr = filter.ToExpression(*col_ref);
	return make_uniq<ExpressionFilter>(std::move(expr));
}

string ExpressionFilter::InternalFunctionToString(const BoundFunctionExpression &func_expr, const string &column_name) {
	auto &func_name = func_expr.Function().GetName();
	if (func_name == BloomFilterScalarFun::NAME) {
		auto &data = func_expr.BindInfo()->Cast<BloomFilterFunctionData>();
		return BloomFilterScalarFun::ToString(column_name, data.key_column_name);
	} else if (func_name == PrefixRangeScalarFun::NAME) {
		auto &data = func_expr.BindInfo()->Cast<PrefixRangeFunctionData>();
		return PrefixRangeScalarFun::ToString(column_name, data.key_column_name);
	} else if (func_name == DynamicFilterScalarFun::NAME) {
		const auto has_filter_data =
		    func_expr.BindInfo() && func_expr.BindInfo()->Cast<DynamicFilterFunctionData>().filter_data;
		return DynamicFilterScalarFun::ToString(column_name, has_filter_data);
	} else if (IsOptionalInternalFunction(func_expr)) {
		auto optional_child = GetOptionalFilterChild(func_expr);
		auto child_filter_string = optional_child ? ExpressionToFriendlyString(*optional_child, column_name) : string();
		if (func_name == OptionalFilterScalarFun::NAME) {
			return OptionalFilterScalarFun::ToString(child_filter_string);
		}
		return SelectivityOptionalFilterScalarFun::ToString(child_filter_string);
	}
	return string();
}

string ExpressionFilter::ExpressionToFriendlyString(const Expression &expression, const string &column_name) {
	if (expression.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &func_expr = expression.Cast<BoundFunctionExpression>();
		auto result = InternalFunctionToString(func_expr, column_name);
		if (!result.empty()) {
			return result;
		}
	}
	if (expression.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = expression.Cast<BoundConjunctionExpression>();
		if (ContainsInternalTableFilterFunction(expression)) {
			string result = "(";
			for (idx_t i = 0; i < conj.GetChildren().size(); i++) {
				if (i > 0) {
					result += conj.GetExpressionType() == ExpressionType::CONJUNCTION_AND ? " AND " : " OR ";
				}
				result += ExpressionToFriendlyString(*conj.GetChildren()[i], column_name);
			}
			result += ")";
			return result;
		}
	}
	// Default: use standard expression ToString with column name substitution
	auto expr_copy = expression.Copy();
	auto name_expr = make_uniq<BoundReferenceExpression>(Identifier(column_name), LogicalType::INVALID, 0ULL);
	ReplaceExpressionRecursive(expr_copy, *name_expr, ExpressionType::BOUND_REF);
	return expr_copy->ToString();
}

string ExpressionFilter::ToString(const string &column_name) const {
	return ExpressionToFriendlyString(*expr, column_name);
}

string ExpressionFilter::DebugToString() const {
	return ExpressionToFriendlyString(*expr, "c1");
}

void ExpressionFilter::ReplaceExpressionRecursive(unique_ptr<Expression> &expr, const Expression &column,
                                                  ExpressionType replace_type) {
	if (expr->GetExpressionType() == replace_type) {
		expr = column.Copy();
		return;
	}
	ExpressionIterator::EnumerateChildren(
	    *expr, [&](unique_ptr<Expression> &child) { ReplaceExpressionRecursive(child, column, replace_type); });
}

unique_ptr<Expression> ExpressionFilter::ToExpression(const Expression &column) const {
	D_ASSERT(column_indexes.empty());
	auto expr_copy = expr->Copy();
	ReplaceExpressionRecursive(expr_copy, column);
	return expr_copy;
}

bool ExpressionFilter::Equals(const ExpressionFilter &other_p) const {
	auto &other = other_p.Cast<ExpressionFilter>();
	return column_indexes == other.column_indexes && other.expr->Equals(*expr);
}

unique_ptr<ExpressionFilter> ExpressionFilter::Copy() const {
	return make_uniq<ExpressionFilter>(expr->Copy(), column_indexes);
}

} // namespace duckdb
