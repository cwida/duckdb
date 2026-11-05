#include "duckdb/optimizer/rule/move_constants.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/value_operations/value_operations.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/optimizer/expression_rewriter.hpp"

namespace duckdb {

MoveConstantsRule::MoveConstantsRule(ExpressionRewriter &rewriter) : Rule(rewriter) {
	auto op = make_uniq<ComparisonExpressionMatcher>();
	op->matchers.push_back(make_uniq<ConstantExpressionMatcher>());
	op->policy = SetMatcher::Policy::UNORDERED;

	auto arithmetic = make_uniq<FunctionExpressionMatcher>();
	// we handle multiplication, addition and subtraction because those are "easy"
	// integer division makes the division case difficult
	// e.g. [x / 2 = 3] means [x = 6 OR x = 7] because of truncation -> no clean rewrite rules
	arithmetic->function =
	    make_uniq<ManyFunctionMatcher>(identifier_set_t {Identifier("+"), Identifier("-"), Identifier("*")});
	// we match only on integral numeric types
	arithmetic->type = make_uniq<IntegerTypeMatcher>();
	auto child_constant_matcher = make_uniq<ConstantExpressionMatcher>();
	auto child_expression_matcher = make_uniq<ExpressionMatcher>();
	child_constant_matcher->type = make_uniq<IntegerTypeMatcher>();
	child_expression_matcher->type = make_uniq<IntegerTypeMatcher>();
	arithmetic->matchers.push_back(std::move(child_constant_matcher));
	arithmetic->matchers.push_back(std::move(child_expression_matcher));
	arithmetic->policy = SetMatcher::Policy::SOME;
	op->matchers.push_back(std::move(arithmetic));
	root = std::move(op);
}

// Result of a comparison against a constant that cannot be represented in the type, if it can be decided
static bool TryOutOfRangeComparisonResult(ExpressionType comparison_type, bool &result) {
	switch (comparison_type) {
	case ExpressionType::COMPARE_EQUAL:
		result = false;
		return true;
	case ExpressionType::COMPARE_NOTEQUAL:
		result = true;
		return true;
	default:
		return false;
	}
}

unique_ptr<Expression> MoveConstantsRule::Apply(LogicalOperator &op, vector<reference<Expression>> &bindings,
                                                bool &changes_made, bool is_root) {
	auto &comparison = bindings[0].get().Cast<BoundFunctionExpression>();
	auto &outer_constant = bindings[1].get().Cast<BoundConstantExpression>();
	auto &arithmetic = bindings[2].get().Cast<BoundFunctionExpression>();
	auto &inner_constant = bindings[3].get().Cast<BoundConstantExpression>();
	D_ASSERT(arithmetic.GetReturnType().IsIntegral());
	D_ASSERT(arithmetic.GetChildren()[0]->GetReturnType().IsIntegral());
	if (inner_constant.GetValue().IsNull() || outer_constant.GetValue().IsNull()) {
		if (comparison.GetExpressionType() == ExpressionType::COMPARE_DISTINCT_FROM ||
		    comparison.GetExpressionType() == ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
			return nullptr;
		}
		return make_uniq<BoundConstantExpression>(Value(comparison.GetReturnType()));
	}
	auto &constant_type = outer_constant.GetReturnType();
	hugeint_t outer_value = IntegralValue::Get(outer_constant.GetValue());
	hugeint_t inner_value = IntegralValue::Get(inner_constant.GetValue());

	idx_t arithmetic_child_index = arithmetic.GetChildrenMutable()[0].get() == &inner_constant ? 1 : 0;
	auto &op_type = arithmetic.Function().GetName();
	if (op_type == "+") {
		// [x + 1 COMP 10] OR [1 + x COMP 10]
		// order does not matter in addition:
		// simply change right side to 10-1 (outer_constant - inner_constant)
		if (!Hugeint::TrySubtractInPlace(outer_value, inner_value)) {
			return nullptr;
		}
		auto result_value = Value::HUGEINT(outer_value).DefaultTryCastAs(constant_type);
		if (!result_value) {
			if (comparison.GetExpressionType() != ExpressionType::COMPARE_EQUAL) {
				return nullptr;
			}
			// if the cast is not possible then the comparison is not possible
			// for example, if we have x + 5 = 3, where x is an unsigned number, we will get x = -2
			// since this is not possible we can remove the entire branch here
			return ExpressionRewriter::ConstantOrNull(
			    GetContext(), std::move(arithmetic.GetChildrenMutable()[arithmetic_child_index]),
			    Value::BOOLEAN(false));
		}
		outer_constant.SetValue(std::move(*result_value));
	} else if (op_type == "-") {
		// [x - 1 COMP 10] O R [1 - x COMP 10]
		// order matters in subtraction:
		if (arithmetic_child_index == 0) {
			// [x - 1 COMP 10]
			// change right side to 10+1 (outer_constant + inner_constant)
			if (!Hugeint::TryAddInPlace(outer_value, inner_value)) {
				return nullptr;
			}
			auto result_value = Value::HUGEINT(outer_value).DefaultTryCastAs(constant_type);
			if (!result_value) {
				// if the cast is not possible then an equality comparison is not possible
				if (comparison.GetExpressionType() != ExpressionType::COMPARE_EQUAL) {
					return nullptr;
				}
				return ExpressionRewriter::ConstantOrNull(
				    GetContext(), std::move(arithmetic.GetChildrenMutable()[arithmetic_child_index]),
				    Value::BOOLEAN(false));
			}
			outer_constant.SetValue(std::move(*result_value));
		} else {
			// [1 - x COMP 10]
			// change right side to 1-10=-9
			if (!Hugeint::TrySubtractInPlace(inner_value, outer_value)) {
				return nullptr;
			}
			auto result_value = Value::HUGEINT(inner_value).DefaultTryCastAs(constant_type);
			if (!result_value) {
				// if the cast is not possible then an equality comparison is not possible
				if (comparison.GetExpressionType() != ExpressionType::COMPARE_EQUAL) {
					return nullptr;
				}
				return ExpressionRewriter::ConstantOrNull(
				    GetContext(), std::move(arithmetic.GetChildrenMutable()[arithmetic_child_index]),
				    Value::BOOLEAN(false));
			}
			outer_constant.SetValue(std::move(*result_value));
			// in this case, we should also flip the comparison
			// e.g. if we have [4 - x < 2] then we should have [x > 2]
			BoundComparisonExpression::FlipType(comparison);
		}
	} else {
		D_ASSERT(op_type == "*");
		// [x * 2 COMP 10] OR [2 * x COMP 10]
		// order does not matter in multiplication:
		// change right side to 10/2 (outer_constant / inner_constant)
		// but ONLY if outer_constant is cleanly divisible by the inner_constant
		if (inner_value == 0) {
			// x * 0, the result is either 0 or NULL
			// we let the arithmetic_simplification rule take care of simplifying this first
			return nullptr;
		}
		// check out of range for HUGEINT or not cleanly divisible
		// HUGEINT is not cleanly divisible when outer_value == minimum and inner value == -1. (modulo overflow)
		if ((outer_value == NumericLimits<hugeint_t>::Minimum() && inner_value == -1) ||
		    outer_value % inner_value != 0) {
			bool is_equality = comparison.GetExpressionType() == ExpressionType::COMPARE_EQUAL;
			bool is_inequality = comparison.GetExpressionType() == ExpressionType::COMPARE_NOTEQUAL;
			if (is_equality || is_inequality) {
				// we know the values are not equal
				// the result will be either FALSE or NULL (if COMPARE_EQUAL)
				// or TRUE or NULL (if COMPARE_NOTEQUAL)
				return ExpressionRewriter::ConstantOrNull(
				    GetContext(), std::move(arithmetic.GetChildrenMutable()[arithmetic_child_index]),
				    Value::BOOLEAN(is_inequality));
			} else {
				// not cleanly divisible and we are doing > >= < <=, skip the simplification for now
				return nullptr;
			}
		}
		// divide the RHS by the LHS
		// we need to do a range check on the cast even though we do a division
		// because e.g. -128 / -1 = 128, which is out of range
		auto result_value = Value::HUGEINT(outer_value / inner_value).DefaultTryCastAs(constant_type);
		if (!result_value) {
			// no representable value satisfies an equality, but ordered comparisons cannot be decided here
			bool comparison_result;
			if (!TryOutOfRangeComparisonResult(comparison.GetExpressionType(), comparison_result)) {
				return nullptr;
			}
			return ExpressionRewriter::ConstantOrNull(
			    GetContext(), std::move(arithmetic.GetChildrenMutable()[arithmetic_child_index]),
			    Value::BOOLEAN(comparison_result));
		}
		if (inner_value < 0) {
			// multiply by negative value, need to flip expression
			BoundComparisonExpression::FlipType(comparison);
		}
		outer_constant.SetValue(std::move(*result_value));
	}
	// replace left side with x
	// first extract x from the arithmetic expression
	auto arithmetic_child = std::move(arithmetic.GetChildrenMutable()[arithmetic_child_index]);
	// then place in the comparison
	if (RefersToSameObject(BoundComparisonExpression::Left(comparison), outer_constant)) {
		BoundComparisonExpression::RightMutable(comparison) = std::move(arithmetic_child);
	} else {
		BoundComparisonExpression::LeftMutable(comparison) = std::move(arithmetic_child);
	}
	changes_made = true;
	return nullptr;
}

static bool IsOrderedComparison(ExpressionType comparison_type) {
	switch (comparison_type) {
	case ExpressionType::COMPARE_LESSTHAN:
	case ExpressionType::COMPARE_GREATERTHAN:
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return true;
	default:
		return false;
	}
}

static bool CanDuplicateForNaNGuard(const Expression &expr) {
	return expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF ||
	       expr.GetExpressionClass() == ExpressionClass::BOUND_REF;
}

static unique_ptr<Expression> CreateIsNanCall(ClientContext &context, const Expression &expr) {
	vector<unique_ptr<Expression>> children;
	children.push_back(expr.Copy());

	FunctionBinder binder(context);
	return binder.BindScalarFunction(Identifier::DefaultSchema(), "isnan", std::move(children));
}

static unique_ptr<Expression> CreateNotIsNanGuard(ClientContext &context, const Expression &expr) {
	auto not_isnan = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_NOT, LogicalType::BOOLEAN);
	not_isnan->GetChildrenMutable().push_back(CreateIsNanCall(context, expr));
	return std::move(not_isnan);
}

//! -NaN is still NaN, and DuckDB orders NaN above every other value, so [-x > c] and [-x >= c] are TRUE for a
//! NaN input while the flipped [x < -c] / [x <= -c] are FALSE. Those two need [OR isnan(x)]; [-x < c] and
//! [-x <= c] have the mirrored problem and need [AND NOT isnan(x)]. The comparison type passed here must
//! already be oriented with the negation on the left.
static bool NaNGuardIsDisjunctive(ExpressionType comparison_type) {
	return comparison_type == ExpressionType::COMPARE_GREATERTHAN ||
	       comparison_type == ExpressionType::COMPARE_GREATERTHANOREQUALTO;
}

MoveUnaryMinusRule::MoveUnaryMinusRule(ExpressionRewriter &rewriter) : Rule(rewriter) {
	auto op = make_uniq<ComparisonExpressionMatcher>();
	op->matchers.push_back(make_uniq<ConstantExpressionMatcher>());
	op->policy = SetMatcher::Policy::UNORDERED;

	// match a unary minus over a numeric expression, e.g. [-x = -5] or [-x < -3.14]
	auto negation = make_uniq<FunctionExpressionMatcher>();
	negation->function = make_uniq<SpecificFunctionMatcher>("-");
	negation->type = make_uniq<NumericTypeMatcher>();
	negation->matchers.push_back(make_uniq<ExpressionMatcher>());
	negation->policy = SetMatcher::Policy::ORDERED;
	op->matchers.push_back(std::move(negation));
	root = std::move(op);
}

unique_ptr<Expression> MoveUnaryMinusRule::Apply(LogicalOperator &op, vector<reference<Expression>> &bindings,
                                                 bool &changes_made, bool is_root) {
	auto &comparison = bindings[0].get().Cast<BoundFunctionExpression>();
	auto &outer_constant = bindings[1].get().Cast<BoundConstantExpression>();
	auto &negation = bindings[2].get().Cast<BoundFunctionExpression>();

	// Unsigned values cannot be negated
	if (negation.GetReturnType().IsUnsigned()) {
		return nullptr;
	}

	// Handle NULL values
	if (outer_constant.GetValue().IsNull()) {
		if (comparison.GetExpressionType() == ExpressionType::COMPARE_DISTINCT_FROM ||
		    comparison.GetExpressionType() == ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
			return nullptr;
		}
		return make_uniq<BoundConstantExpression>(Value(comparison.GetReturnType()));
	}

	auto &constant_type = outer_constant.GetReturnType();
	unique_ptr<Expression> nan_guard;
	bool nan_guard_disjunctive = false;
	if (constant_type.IsFloating()) {
		if (IsOrderedComparison(comparison.GetExpressionType())) {
			auto constant_value = outer_constant.GetValue();
			bool constant_is_nan = constant_type.id() == LogicalTypeId::FLOAT
			                           ? Value::IsNan(FloatValue::Get(constant_value))
			                           : Value::IsNan(DoubleValue::Get(constant_value));
			if (constant_is_nan) {
				return nullptr;
			}
			auto &input = *negation.GetChildren()[0];
			if (!CanDuplicateForNaNGuard(input)) {
				return nullptr;
			}
			// the guard depends on the direction of the comparison AS SEEN BY the negation, so [c > -x]
			// must be read as [-x < c] first
			auto negation_type = comparison.GetExpressionType();
			if (!RefersToSameObject(BoundComparisonExpression::Left(comparison), negation)) {
				negation_type = FlipComparisonExpression(negation_type);
			}
			nan_guard_disjunctive = NaNGuardIsDisjunctive(negation_type);
			nan_guard =
			    nan_guard_disjunctive ? CreateIsNanCall(GetContext(), input) : CreateNotIsNanGuard(GetContext(), input);
		}
		double val = 0.0;
		if (constant_type.id() == LogicalTypeId::FLOAT) {
			val = static_cast<double>(FloatValue::Get(outer_constant.GetValue()));
		} else {
			val = DoubleValue::Get(outer_constant.GetValue());
		}
		auto result_value = Value::DOUBLE(-val).DefaultTryCastAs(constant_type);
		if (!result_value) {
			return nullptr;
		}
		outer_constant.SetValue(std::move(*result_value));
	} else if (constant_type.id() == LogicalTypeId::DECIMAL) {
		// negate the unscaled integer and reconstruct with the same width/scale
		hugeint_t negated_value;
		if (!Hugeint::TryNegate(IntegralValue::Get(outer_constant.GetValue()), negated_value)) {
			return nullptr;
		}
		auto width = DecimalType::GetWidth(constant_type);
		auto scale = DecimalType::GetScale(constant_type);
		switch (constant_type.InternalType()) {
		case PhysicalType::INT16:
			outer_constant.SetValue(Value::DECIMAL(Cast::Operation<hugeint_t, int16_t>(negated_value), width, scale));
			break;
		case PhysicalType::INT32:
			outer_constant.SetValue(Value::DECIMAL(Cast::Operation<hugeint_t, int32_t>(negated_value), width, scale));
			break;
		case PhysicalType::INT64:
			outer_constant.SetValue(Value::DECIMAL(Cast::Operation<hugeint_t, int64_t>(negated_value), width, scale));
			break;
		case PhysicalType::INT128:
			outer_constant.SetValue(Value::DECIMAL(negated_value, width, scale));
			break;
		default:
			throw InternalException("Unknown DECIMAL physical type");
		}
	} else {
		// integer path
		hugeint_t negated_value;
		if (!Hugeint::TryNegate(IntegralValue::Get(outer_constant.GetValue()), negated_value)) {
			return nullptr;
		}
		auto result_value = Value::HUGEINT(negated_value).DefaultTryCastAs(constant_type);
		if (!result_value) {
			bool comparison_result;
			if (!TryOutOfRangeComparisonResult(comparison.GetExpressionType(), comparison_result)) {
				return nullptr;
			}
			return ExpressionRewriter::ConstantOrNull(GetContext(), std::move(negation.GetChildrenMutable()[0]),
			                                          Value::BOOLEAN(comparison_result));
		}
		outer_constant.SetValue(std::move(*result_value));
	}
	// [-x COMP c] => [x FLIPPED_COMP -c]
	auto inner_expr = std::move(negation.GetChildrenMutable()[0]);
	if (RefersToSameObject(BoundComparisonExpression::Left(comparison), negation)) {
		BoundComparisonExpression::LeftMutable(comparison) = std::move(inner_expr);
	} else {
		BoundComparisonExpression::RightMutable(comparison) = std::move(inner_expr);
	}
	BoundComparisonExpression::FlipType(comparison);
	if (nan_guard) {
		auto conjunction_type =
		    nan_guard_disjunctive ? ExpressionType::CONJUNCTION_OR : ExpressionType::CONJUNCTION_AND;
		auto result = make_uniq<BoundConjunctionExpression>(conjunction_type, comparison.Copy(), std::move(nan_guard));
		return std::move(result);
	}
	changes_made = true;
	return nullptr;
}

} // namespace duckdb
