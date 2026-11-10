#include "duckdb/optimizer/variant_comparison_rewriter.hpp"

#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/cast/cast_statistics.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/expression/bound_between_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/statistics/variant_stats.hpp"

#include <algorithm>

namespace duckdb {

bool IsVariantWrapperFunction(const BoundFunctionExpression &expr) {
	const auto &function_name = expr.Function().GetName();
	return function_name == "variant_comparator" || function_name == "variant_normalize";
}

optional_ptr<const BoundColumnRefExpression> TryGetVariantWrapperColumnRef(const Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return nullptr;
	}
	auto &function_expr = expr.Cast<BoundFunctionExpression>();
	if (!IsVariantWrapperFunction(function_expr) || function_expr.GetChildren().size() != 1) {
		return nullptr;
	}
	auto &child = *function_expr.GetChildren()[0];
	if (child.GetExpressionType() != ExpressionType::BOUND_COLUMN_REF ||
	    child.GetReturnType().id() != LogicalTypeId::VARIANT) {
		return nullptr;
	}
	return child.Cast<BoundColumnRefExpression>();
}

optional_ptr<const Expression> TryGetVariantWrapperChild(const Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return nullptr;
	}
	auto &function_expr = expr.Cast<BoundFunctionExpression>();
	if (!IsVariantWrapperFunction(function_expr) || function_expr.GetChildren().size() != 1) {
		return nullptr;
	}
	return *function_expr.GetChildren()[0];
}

bool TryGetFullyShreddedIntegralType(const BaseStatistics &stats, LogicalType &result) {
	if (stats.GetType().id() != LogicalTypeId::VARIANT || !VariantStats::IsShredded(stats)) {
		return false;
	}
	auto &shredded_stats = VariantStats::GetShreddedStats(stats);
	if (!VariantShreddedStats::IsFullyShredded(shredded_stats)) {
		return false;
	}
	auto shredded_type = VariantStats::GetShreddedStructuredType(stats);
	if (!shredded_type.IsIntegral()) {
		return false;
	}
	auto &typed_stats = VariantStats::GetTypedStats(shredded_stats);
	if (typed_stats.GetType() != shredded_type) { // LCOV_EXCL_START
		return false;
	} // LCOV_EXCL_STOP
	result = std::move(shredded_type);
	return true;
}

unique_ptr<Expression> CreateVariantShreddedCast(ClientContext &context, unique_ptr<Expression> input,
                                                 const LogicalType &shredded_type, const LogicalType &target_type) {
	input = BoundCastExpression::AddCastToType(context, std::move(input), shredded_type);
	if (shredded_type == target_type) {
		return input;
	}
	return BoundCastExpression::AddCastToType(context, std::move(input), target_type);
}

bool TryGetVariantIntegralComparisonType(const LogicalType &shredded_type, const LogicalType &constant_type,
                                         LogicalType &result) {
	if (shredded_type == constant_type) {
		result = shredded_type;
		return true;
	}
	if (!shredded_type.IsIntegral() || !constant_type.IsIntegral()) {
		return false;
	}
	if (!LogicalType::DefaultTryGetMaxLogicalTypeUnchecked(shredded_type, constant_type, result)) {
		return false;
	}
	return result.IsIntegral();
}

//! Evaluates the child of a VARIANT wrapper into a constant, returns false if it is not a constant
static bool TryGetVariantWrapperConstant(ClientContext &context, optional_ptr<const Expression> wrapper_child,
                                         Value &result) {
	if (!wrapper_child || !wrapper_child->IsFoldable()) {
		return false;
	}
	Value evaluated;
	if (!ExpressionExecutor::TryEvaluateScalar(context, *wrapper_child, evaluated)) {
		return false;
	}
	if (evaluated.type().id() == LogicalTypeId::VARIANT) {
		evaluated = VariantValue::GetValue(evaluated);
	}
	if (evaluated.IsNull()) {
		return false;
	}
	result = std::move(evaluated);
	return true;
}

VariantComparisonRewriter::VariantComparisonRewriter(ClientContext &context) : context(context) {
}

void VariantComparisonRewriter::Rewrite(unique_ptr<LogicalOperator> &op) {
	RewriteOperator(op);
}

void VariantComparisonRewriter::RewriteOperator(unique_ptr<LogicalOperator> &op) {
	if (op->type == LogicalOperatorType::LOGICAL_FILTER) {
		RewriteFilter(*op);
	}
	for (auto &child : op->children) {
		RewriteOperator(child);
	}
}

void VariantComparisonRewriter::RewriteFilter(LogicalOperator &op) {
	auto &filter = op.Cast<LogicalFilter>();
	if (filter.children.empty()) {
		return;
	}
	source = filter.children[0].get();
	for (auto &expr : filter.expressions) {
		RewriteExpression(expr);
	}
	source = nullptr;
}

bool VariantComparisonRewriter::RewriteExpression(unique_ptr<Expression> &expr) {
	switch (expr->GetExpressionClass()) {
	case ExpressionClass::BOUND_CONJUNCTION: {
		auto &conjunction = expr->Cast<BoundConjunctionExpression>();
		switch (conjunction.GetExpressionType()) {
		case ExpressionType::CONJUNCTION_AND:
		case ExpressionType::CONJUNCTION_OR: {
			bool changed = false;
			for (auto &child : conjunction.GetChildrenMutable()) {
				changed |= RewriteExpression(child);
			}
			return changed;
		}
		default:
			return false;
		}
	}
	case ExpressionClass::BOUND_OPERATOR: {
		if (expr->GetExpressionType() != ExpressionType::OPERATOR_NOT) {
			return false;
		}
		// negation is turned into a negated comparison by the expression rewriter, which runs after this pass
		auto &children = expr->Cast<BoundOperatorExpression>().GetChildrenMutable();
		return children.size() == 1 && RewriteExpression(children[0]);
	}
	case ExpressionClass::BOUND_FUNCTION: {
		if (expr->GetExpressionType() == ExpressionType::COMPARE_BETWEEN) {
			return RewriteBetween(expr);
		}
		if (!BoundComparisonExpression::IsComparison(*expr)) {
			return false;
		}
		return RewriteComparison(expr);
	}
	default:
		return false;
	}
}

bool VariantComparisonRewriter::RewriteComparison(unique_ptr<Expression> &expr) {
	auto &comparison = expr->Cast<BoundFunctionExpression>();
	auto comparison_type = comparison.GetExpressionType();
	if (comparison_type == ExpressionType::COMPARE_DISTINCT_FROM ||
	    comparison_type == ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
		return false;
	}

	auto column_ref = TryGetVariantWrapperColumnRef(BoundComparisonExpression::Left(comparison));
	optional_ptr<const Expression> constant_expr;
	if (column_ref) {
		constant_expr = TryGetVariantWrapperChild(BoundComparisonExpression::Right(comparison));
	} else {
		column_ref = TryGetVariantWrapperColumnRef(BoundComparisonExpression::Right(comparison));
		if (!column_ref) {
			return false;
		}
		constant_expr = TryGetVariantWrapperChild(BoundComparisonExpression::Left(comparison));
		comparison_type = FlipComparisonExpression(comparison_type);
	}

	LogicalType shredded_type;
	if (!TryGetIntegralType(*column_ref, shredded_type)) {
		return false;
	}
	Value constant;
	if (!TryGetVariantWrapperConstant(context, constant_expr, constant)) {
		return false;
	}
	LogicalType target_type;
	if (!TryGetVariantIntegralComparisonType(shredded_type, constant.type(), target_type)) {
		return false;
	}
	auto cast_constant = constant.DefaultTryCastAs(target_type);
	if (!cast_constant) {
		return false;
	}

	auto left = CreateVariantShreddedCast(context, column_ref->Copy(), shredded_type, target_type);
	auto right = make_uniq<BoundConstantExpression>(std::move(*cast_constant));
	expr = BoundComparisonExpression::Create(comparison_type, std::move(left), std::move(right));
	return true;
}

bool VariantComparisonRewriter::RewriteBetween(unique_ptr<Expression> &expr) {
	auto &between = expr->Cast<BoundFunctionExpression>();
	auto column_ref = TryGetVariantWrapperColumnRef(BoundBetweenExpression::Input(between));
	if (!column_ref) {
		return false;
	}

	LogicalType shredded_type;
	if (!TryGetIntegralType(*column_ref, shredded_type)) {
		return false;
	}
	Value lower;
	Value upper;
	if (!TryGetVariantWrapperConstant(context, TryGetVariantWrapperChild(BoundBetweenExpression::LowerBound(between)),
	                                  lower) ||
	    !TryGetVariantWrapperConstant(context, TryGetVariantWrapperChild(BoundBetweenExpression::UpperBound(between)),
	                                  upper)) {
		return false;
	}
	LogicalType lower_type;
	LogicalType upper_type;
	LogicalType target_type;
	if (!TryGetVariantIntegralComparisonType(shredded_type, lower.type(), lower_type) ||
	    !TryGetVariantIntegralComparisonType(shredded_type, upper.type(), upper_type) ||
	    !TryGetVariantIntegralComparisonType(lower_type, upper_type, target_type)) {
		return false;
	}
	auto cast_lower = lower.DefaultTryCastAs(target_type);
	auto cast_upper = upper.DefaultTryCastAs(target_type);
	if (!cast_lower || !cast_upper) {
		return false;
	}

	auto input = CreateVariantShreddedCast(context, column_ref->Copy(), shredded_type, target_type);
	auto lower_expr = make_uniq<BoundConstantExpression>(std::move(*cast_lower));
	auto upper_expr = make_uniq<BoundConstantExpression>(std::move(*cast_upper));
	expr = BoundBetweenExpression::Create(std::move(input), std::move(lower_expr), std::move(upper_expr),
	                                      BoundBetweenExpression::LowerInclusive(between),
	                                      BoundBetweenExpression::UpperInclusive(between));
	return true;
}

optional_ptr<LogicalGet> VariantComparisonRewriter::ResolveSourceColumn(LogicalOperator &op, ColumnBinding &binding) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_GET: {
		auto &get = op.Cast<LogicalGet>();
		return binding.table_index == get.table_index ? &get : nullptr;
	}
	case LogicalOperatorType::LOGICAL_PROJECTION: {
		auto &proj = op.Cast<LogicalProjection>();
		if (binding.table_index != proj.table_index || binding.column_index >= proj.expressions.size()) {
			return nullptr;
		}
		auto &proj_expr = *proj.expressions[binding.column_index];
		if (proj_expr.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			return nullptr;
		}
		binding = proj_expr.Cast<BoundColumnRefExpression>().Binding();
		break;
	}
	case LogicalOperatorType::LOGICAL_FILTER:
		break;
	default:
		return nullptr;
	}
	if (op.children.empty()) {
		return nullptr;
	}
	return ResolveSourceColumn(*op.children[0], binding);
}

bool VariantComparisonRewriter::TryGetIntegralType(const BoundColumnRefExpression &colref, LogicalType &result) {
	if (!source) {
		return false;
	}
	auto binding = colref.Binding();
	auto get_ptr = ResolveSourceColumn(*source, binding);
	if (!get_ptr) {
		return false;
	}
	auto &get = *get_ptr;
	idx_t column_index = binding.column_index;
	if (!get.projection_ids.empty()) {
		auto entry = std::find(get.projection_ids.begin(), get.projection_ids.end(), column_index);
		if (entry == get.projection_ids.end()) { // LCOV_EXCL_START
			return false;
		} // LCOV_EXCL_STOP
		column_index = NumericCast<idx_t>(entry - get.projection_ids.begin());
	}
	auto &column_ids = get.GetColumnIds();
	if (column_index >= column_ids.size()) { // LCOV_EXCL_START
		return false;
	} // LCOV_EXCL_STOP
	if (!get.function.statistics && !get.function.statistics_extended) {
		return false;
	}

	unique_ptr<BaseStatistics> stats;
	if (get.function.statistics_extended) {
		TableFunctionGetStatisticsInput input(get.bind_data.get(), column_ids[column_index]);
		stats = get.function.statistics_extended(context, input);
	} else {
		stats = get.function.statistics(context, get.bind_data.get(), column_ids[column_index].GetPrimaryIndex());
	}
	if (!stats) {
		return false;
	}
	return TryGetFullyShreddedIntegralType(*stats, result);
}

} // namespace duckdb
