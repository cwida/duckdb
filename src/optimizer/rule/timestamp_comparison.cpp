
#include "duckdb/optimizer/rule/timestamp_comparison.hpp"
#include "duckdb/optimizer/matcher/expression_matcher.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/optimizer/matcher/type_matcher_id.hpp"
#include "duckdb/optimizer/expression_rewriter.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/operator/add.hpp"
#include "duckdb/common/types/timestamp.hpp"

namespace duckdb {

TimeStampComparison::TimeStampComparison(ExpressionRewriter &rewriter) : Rule(rewriter), context(rewriter.context) {
	// match on an equality comparison between a timestamp column cast to DATE and a foldable DATE constant
	auto op = make_uniq<ComparisonExpressionMatcher>();
	op->policy = SetMatcher::Policy::UNORDERED;
	op->expr_type = make_uniq<SpecificExpressionTypeMatcher>(ExpressionType::COMPARE_EQUAL);

	// one side is timestamp cast to date
	auto left = make_uniq<CastExpressionMatcher>();
	left->type = make_uniq<TypeMatcherId>(LogicalTypeId::DATE);
	left->matcher = make_uniq<ExpressionMatcher>();
	left->matcher->expr_class = ExpressionClass::BOUND_COLUMN_REF;
	left->matcher->type = make_uniq<TypeMatcherId>(LogicalTypeId::TIMESTAMP);
	op->matchers.push_back(std::move(left));

	// other side is any foldable DATE constant expression; bottom-up rewriting can fold CAST(VARCHAR AS DATE)
	auto right = make_uniq<FoldableConstantMatcher>();
	right->type = make_uniq<TypeMatcherId>(LogicalTypeId::DATE);
	op->matchers.push_back(std::move(right));

	root = std::move(op);
}

static optional_ptr<BoundFunctionExpression> GetTimestampCast(Expression &expr) {
	if (!BoundCastExpression::IsCast(expr)) {
		return nullptr;
	}
	auto &cast_expr = expr.Cast<BoundFunctionExpression>();
	if (cast_expr.GetReturnType().id() != LogicalTypeId::DATE) {
		return nullptr;
	}
	if (BoundCastExpression::Child(cast_expr).GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF ||
	    BoundCastExpression::Child(cast_expr).GetReturnType().id() != LogicalTypeId::TIMESTAMP) {
		return nullptr;
	}
	return &cast_expr;
}

static Value DateToTimestampValue(date_t date, dtime_t time) {
	if (date == date_t::infinity()) {
		return Value::TIMESTAMP(timestamp_t::infinity());
	} else if (date == date_t::ninfinity()) {
		return Value::TIMESTAMP(timestamp_t::ninfinity());
	}
	return Value::TIMESTAMP(date, time);
}

unique_ptr<Expression> TimeStampComparison::Apply(LogicalOperator &op, vector<reference<Expression>> &bindings,
                                                  bool &changes_made, bool is_root) {
	auto &comparison = bindings[0].get().Cast<BoundFunctionExpression>();
	D_ASSERT(comparison.GetChildren().size() == 2);

	optional_ptr<BoundFunctionExpression> cast_expr;
	Expression *constant_expr = nullptr;
	for (auto &child : comparison.GetChildren()) {
		if (auto timestamp_cast = GetTimestampCast(*child)) {
			cast_expr = timestamp_cast;
		} else if (child->IsFoldable() && child->GetReturnType().id() == LogicalTypeId::DATE) {
			constant_expr = child.get();
		}
	}
	if (!cast_expr || !constant_expr) {
		return nullptr;
	}

	auto cast_constant = constant_expr->Copy();
	auto cast_columnref = BoundCastExpression::Child(*cast_expr).Copy();
	auto new_expr = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);

	Value result;
	if (ExpressionExecutor::TryEvaluateScalar(context, *cast_constant, result)) {
		D_ASSERT(result.type() == LogicalType::DATE);
		auto original_val = result.GetValue<duckdb::date_t>();
		auto no_seconds = dtime_t(0);

		if (!original_val.IsFinite()) {
			auto original_val_for_comparison =
			    make_uniq<BoundConstantExpression>(DateToTimestampValue(original_val, no_seconds));
			auto column_copy = cast_columnref->Copy();
			auto eq_expr = BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL, std::move(column_copy),
			                                                 std::move(original_val_for_comparison));
			new_expr->GetChildrenMutable().push_back(std::move(eq_expr));
			return std::move(new_expr);
		}

		// the range [date, date + 1 day) must be representable as timestamps, otherwise keep the cast comparison
		date_t next_day;
		timestamp_t lower_bound, upper_bound;
		if (!TryAddOperator::Operation<date_t, int32_t, date_t>(original_val, 1, next_day) ||
		    !Timestamp::TryFromDatetime(original_val, no_seconds, lower_bound) ||
		    !Timestamp::TryFromDatetime(next_day, no_seconds, upper_bound)) {
			return nullptr;
		}
		auto original_val_for_comparison = make_uniq<BoundConstantExpression>(Value::TIMESTAMP(lower_bound));
		auto val_for_comparison = make_uniq<BoundConstantExpression>(Value::TIMESTAMP(upper_bound));

		auto left_copy = cast_columnref->Copy();
		auto right_copy = cast_columnref->Copy();
		auto lt_eq_expr = BoundComparisonExpression::Create(ExpressionType::COMPARE_LESSTHAN, std::move(right_copy),
		                                                    std::move(val_for_comparison));
		auto gt_eq_expr = BoundComparisonExpression::Create(
		    ExpressionType::COMPARE_GREATERTHANOREQUALTO, std::move(left_copy), std::move(original_val_for_comparison));
		new_expr->GetChildrenMutable().push_back(std::move(gt_eq_expr));
		new_expr->GetChildrenMutable().push_back(std::move(lt_eq_expr));
		return std::move(new_expr);
	}
	return nullptr;
}

} // namespace duckdb
