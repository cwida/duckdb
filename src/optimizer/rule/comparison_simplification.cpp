#include "duckdb/planner/expression/list.hpp"
#include "duckdb/optimizer/rule/comparison_simplification.hpp"

#include "duckdb/common/helper.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/optimizer/expression_rewriter.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"

namespace duckdb {

static bool DateTimestampComparisonIsInvertible(ClientContext &context, BoundFunctionExpression &expr,
                                                BoundFunctionExpression &cast_expression, const Value &constant_value,
                                                Value &cast_constant, bool column_ref_left,
                                                unique_ptr<Expression> &replacement) {
	auto timestamp = constant_value.GetValue<timestamp_t>();
	if (!timestamp.IsFinite()) {
		return false;
	}
	if (Timestamp::GetTime(timestamp) == dtime_t(0)) {
		return true; // it's midnight: no replacement needed
	}
	auto op = expr.GetExpressionType();

	// Non-midnight TIMESTAMPs cannot equal DATE values.
	switch (op) {
	case ExpressionType::COMPARE_EQUAL:
		// d =  T   -> false, preserving NULL
		replacement = ExpressionRewriter::ConstantOrNull(
		    context, std::move(BoundCastExpression::ChildMutable(cast_expression)), Value::BOOLEAN(false));
		return true;
	case ExpressionType::COMPARE_NOTEQUAL:
		// d != T   -> true, preserving NULL
		replacement = ExpressionRewriter::ConstantOrNull(
		    context, std::move(BoundCastExpression::ChildMutable(cast_expression)), Value::BOOLEAN(true));
		return true;
	case ExpressionType::COMPARE_DISTINCT_FROM:
		// d IS DISTINCT FROM T     -> true
		replacement = make_uniq<BoundConstantExpression>(Value::BOOLEAN(true));
		return true;
	case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
		// d IS NOT DISTINCT FROM T -> false
		replacement = make_uniq<BoundConstantExpression>(Value::BOOLEAN(false));
		return true;
	default:
		break;
	}

	// The examples describe the column-left form; column-right comparisons invert the day bump.
	bool add_one_day;
	switch (op) {
	case ExpressionType::COMPARE_LESSTHAN:
		// d <  T   -> d <  DATE '2024-06-16'  -- needs +1
		add_one_day = column_ref_left;
		break;
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		// d >= T   -> d >= DATE '2024-06-16'  -- needs +1
		add_one_day = column_ref_left;
		break;
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		// d <= T   -> d <= DATE '2024-06-15'  -- no +1
		add_one_day = !column_ref_left;
		break;
	case ExpressionType::COMPARE_GREATERTHAN:
		// d >  T   -> d >  DATE '2024-06-15'  -- no +1
		add_one_day = !column_ref_left;
		break;
	default:
		return false;
	}
	if (add_one_day) {
		cast_constant = Value::DATE(date_t(cast_constant.GetValue<date_t>().days + 1));
	}
	return true;
}

static bool ConstantCastIsInvertible(ClientContext &context, BoundFunctionExpression &expr,
                                     BoundFunctionExpression &cast_expression, const Value &constant_value,
                                     Value &cast_constant, const LogicalType &target_type, bool column_ref_left,
                                     unique_ptr<Expression> &replacement) {
	auto &cast_type = cast_expression.GetReturnType();
	if (cast_constant.IsNull() || BoundCastExpression::CastIsInvertible(cast_type, target_type)) {
		return true;
	}
	if (cast_type.IsIntegral() && target_type.IsIntegral()) {
		return true;
	}
	// The constant must round-trip exactly; the column side is checked by the caller.
	if (BoundCastExpression::IsLosslessIntegralToFloatingCast(target_type, cast_type) ||
	    (target_type.id() == LogicalTypeId::DECIMAL && cast_type.id() == LogicalTypeId::DECIMAL)) {
		auto roundtrip = cast_constant.TryCastAs(context, cast_type, nullptr, true);
		return roundtrip && *roundtrip == constant_value;
	}
	if (target_type.id() != LogicalTypeId::DATE || cast_type.id() != LogicalTypeId::TIMESTAMP) {
		return false;
	}
	return DateTimestampComparisonIsInvertible(context, expr, cast_expression, constant_value, cast_constant,
	                                           column_ref_left, replacement);
}

//! Replaces CAST(x) <op> c with x <op> source_constant, keeping the side the cast was on
static void ReplaceCastWithChild(BoundFunctionExpression &expr, BoundFunctionExpression &cast_expression,
                                 Value source_constant, bool column_ref_left) {
	auto child_expression = std::move(BoundCastExpression::ChildMutable(cast_expression));
	auto new_constant_expr = make_uniq<BoundConstantExpression>(std::move(source_constant));
	auto &left = BoundComparisonExpression::LeftMutable(expr);
	auto &right = BoundComparisonExpression::RightMutable(expr);
	if (column_ref_left) {
		left = std::move(child_expression);
		right = std::move(new_constant_expr);
	} else {
		left = std::move(new_constant_expr);
		right = std::move(child_expression);
	}
}

static bool IsEqualityComparisonType(ExpressionType type) {
	switch (type) {
	case ExpressionType::COMPARE_EQUAL:
	case ExpressionType::COMPARE_NOTEQUAL:
	case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
	case ExpressionType::COMPARE_DISTINCT_FROM:
		return true;
	default:
		return false;
	}
}

//! Rewrites an (in)equality comparison of CAST(x) with a constant c, for a cast that is invertible for equality:
//! CAST(x) = c holds for exactly the source value CAST(c), if that value casts back to c, and for no value otherwise
static unique_ptr<Expression> SimplifyEqualityCastComparison(ClientContext &context, BoundFunctionExpression &expr,
                                                             BoundFunctionExpression &cast_expression,
                                                             const Value &constant_value, bool column_ref_left,
                                                             bool &changes_made) {
	D_ASSERT(!constant_value.IsNull());
	auto source_constant = BoundCastExpression::TryInvertConstant(context, cast_expression, constant_value);
	if (!source_constant) {
		auto &child = BoundCastExpression::ChildMutable(cast_expression);
		auto comparison_type = expr.GetExpressionType();
		switch (comparison_type) {
		case ExpressionType::COMPARE_EQUAL:
		case ExpressionType::COMPARE_NOTEQUAL:
			return ExpressionRewriter::ConstantOrNull(
			    context, std::move(child), Value::BOOLEAN(comparison_type == ExpressionType::COMPARE_NOTEQUAL));
		default:
			// folding to a constant drops the child, which could throw or have side effects
			if (child->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
				return nullptr;
			}
			return make_uniq<BoundConstantExpression>(
			    Value::BOOLEAN(comparison_type == ExpressionType::COMPARE_DISTINCT_FROM));
		}
	}
	ReplaceCastWithChild(expr, cast_expression, std::move(*source_constant), column_ref_left);
	changes_made = true;
	return nullptr;
}

static unique_ptr<Expression> CreateNullCheckExpression(ExpressionType expression_type, unique_ptr<Expression> child) {
	D_ASSERT(expression_type == ExpressionType::OPERATOR_IS_NULL ||
	         expression_type == ExpressionType::OPERATOR_IS_NOT_NULL);
	auto result = make_uniq<BoundOperatorExpression>(expression_type, LogicalType::BOOLEAN);
	result->GetChildrenMutable().push_back(std::move(child));
	return std::move(result);
}

ComparisonSimplificationRule::ComparisonSimplificationRule(ExpressionRewriter &rewriter) : Rule(rewriter) {
	// match on a ComparisonExpression that has a ConstantExpression as a check
	auto op = make_uniq<ComparisonExpressionMatcher>();
	op->matchers.push_back(make_uniq<FoldableConstantMatcher>());
	op->policy = SetMatcher::Policy::SOME;
	root = std::move(op);
}

RowComparisonSimplificationRule::RowComparisonSimplificationRule(ExpressionRewriter &rewriter) : Rule(rewriter) {
	auto comparison = make_uniq<ComparisonExpressionMatcher>();
	comparison->expr_type = make_uniq<SpecificExpressionTypeMatcher>(ExpressionType::COMPARE_EQUAL);
	comparison->matchers.push_back(make_uniq<ExpressionMatcher>(ExpressionClass::BOUND_FUNCTION));
	comparison->matchers.push_back(make_uniq<ExpressionMatcher>(ExpressionClass::BOUND_FUNCTION));
	root = std::move(comparison);
}

unique_ptr<Expression> RowComparisonSimplificationRule::Apply(LogicalOperator &op,
                                                              vector<reference<Expression>> &bindings,
                                                              bool &changes_made, bool is_root) {
	if (!is_root || op.type != LogicalOperatorType::LOGICAL_FILTER) {
		return nullptr;
	}
	auto &left = bindings[1].get().Cast<BoundFunctionExpression>();
	auto &right = bindings[2].get().Cast<BoundFunctionExpression>();
	if (left.Function().GetName() != "row" || right.Function().GetName() != "row") {
		return nullptr;
	}
	auto &left_children = left.GetChildrenMutable();
	auto &right_children = right.GetChildrenMutable();
	if (left_children.empty() || left_children.size() != right_children.size()) {
		return nullptr;
	}
	for (idx_t child_idx = 0; child_idx < left_children.size(); child_idx++) {
		auto &left_child = *left_children[child_idx];
		auto &right_child = *right_children[child_idx];
		if (left_child.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF ||
		    right_child.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF ||
		    left_child.GetReturnType().IsNested() || left_child.GetReturnType() != right_child.GetReturnType()) {
			return nullptr;
		}
	}

	auto result = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
	for (idx_t child_idx = 0; child_idx < left_children.size(); child_idx++) {
		result->GetChildrenMutable().push_back(BoundComparisonExpression::Create(
		    ExpressionType::COMPARE_NOT_DISTINCT_FROM, std::move(left_children[child_idx]),
		    std::move(right_children[child_idx])));
	}
	return std::move(result);
}

unique_ptr<Expression> ComparisonSimplificationRule::Apply(LogicalOperator &op, vector<reference<Expression>> &bindings,
                                                           bool &changes_made, bool is_root) {
	auto &expr = bindings[0].get().Cast<BoundFunctionExpression>();
	auto &constant_expr = bindings[1].get();
	auto &left = BoundComparisonExpression::LeftMutable(expr);
	auto &right = BoundComparisonExpression::RightMutable(expr);
	bool column_ref_left = !RefersToSameObject(*left, constant_expr);
	auto &column_ref_expr = column_ref_left ? *left : *right;
	// the constant_expr is a scalar expression that we have to fold
	// use an ExpressionExecutor to execute the expression
	D_ASSERT(constant_expr.IsFoldable());
	Value constant_value;
	if (!ExpressionExecutor::TryEvaluateScalar(GetContext(), constant_expr, constant_value)) {
		return nullptr;
	}
	if (constant_value.IsNull() && column_ref_expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		if (expr.GetExpressionType() == ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
			return CreateNullCheckExpression(ExpressionType::OPERATOR_IS_NULL,
			                                 column_ref_left ? std::move(left) : std::move(right));
		}
		if (expr.GetExpressionType() == ExpressionType::COMPARE_DISTINCT_FROM) {
			return CreateNullCheckExpression(ExpressionType::OPERATOR_IS_NOT_NULL,
			                                 column_ref_left ? std::move(left) : std::move(right));
		}
	}
	if (constant_value.IsNull() && !(expr.GetExpressionType() == ExpressionType::COMPARE_NOT_DISTINCT_FROM ||
	                                 expr.GetExpressionType() == ExpressionType::COMPARE_DISTINCT_FROM)) {
		// comparison with constant NULL, return NULL
		return make_uniq<BoundConstantExpression>(Value(LogicalType::BOOLEAN));
	}
	if (BoundComparisonExpression::IsComparison(column_ref_expr) && !constant_value.IsNull() &&
	    constant_value.type().id() == LogicalTypeId::BOOLEAN) {
		if (expr.GetExpressionType() == ExpressionType::COMPARE_EQUAL ||
		    (expr.GetExpressionType() == ExpressionType::COMPARE_NOT_DISTINCT_FROM && is_root &&
		     op.type == LogicalOperatorType::LOGICAL_FILTER)) {
			if (!BooleanValue::Get(constant_value)) {
				auto &comparison = column_ref_expr.Cast<BoundFunctionExpression>();
				auto negated_type = NegateComparisonExpression(comparison.GetExpressionType());
				BoundComparisonExpression::SetType(comparison, negated_type);
			}
			return column_ref_left ? std::move(left) : std::move(right);
		}
	}
	if (BoundCastExpression::IsCast(column_ref_expr)) {
		//! Here we check if we can apply the expression on the constant side
		//! We can do this if the cast itself is invertible and casting the constant is
		//! invertible in practice.
		auto &cast_expression = column_ref_expr.Cast<BoundFunctionExpression>();
		auto target_type = BoundCastExpression::SourceType(cast_expression);
		if (!constant_value.IsNull() && IsEqualityComparisonType(expr.GetExpressionType()) &&
		    BoundCastExpression::CastIsInvertibleForEquality(target_type, cast_expression.GetReturnType())) {
			return SimplifyEqualityCastComparison(GetContext(), expr, cast_expression, constant_value, column_ref_left,
			                                      changes_made);
		}
		if (!BoundCastExpression::CastIsInvertible(target_type, cast_expression.GetReturnType()) &&
		    !BoundCastExpression::IsLosslessIntegralToFloatingCast(target_type, cast_expression.GetReturnType())) {
			return nullptr;
		}

		// Can we cast the constant at all?
		string error_message;
		auto new_constant = constant_value.TryCastAs(rewriter.context, target_type, &error_message, true);
		if (!new_constant) {
			return nullptr;
		}
		auto &cast_constant = *new_constant;

		// Is the constant cast invertible?
		unique_ptr<Expression> replacement;
		if (!ConstantCastIsInvertible(GetContext(), expr, cast_expression, constant_value, cast_constant, target_type,
		                              column_ref_left, replacement)) {
			return nullptr;
		}
		if (replacement) {
			return replacement;
		}

		//! We can cast, now we change our column_ref_expression from an operator cast to a column reference
		ReplaceCastWithChild(expr, cast_expression, std::move(cast_constant), column_ref_left);
		changes_made = true;
	}
	return nullptr;
}

} // namespace duckdb
