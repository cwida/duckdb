#include "duckdb/parser/expression/case_expression.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/logging/logger.hpp"

namespace duckdb {

static bool WarnCaseDecimalScaleReduction(ClientContext &context, const LogicalType &source_type,
                                          const LogicalType &target_type) {
	if (source_type.id() != LogicalTypeId::DECIMAL || target_type.id() != LogicalTypeId::DECIMAL) {
		return false;
	}
	auto source_scale = DecimalType::GetScale(source_type);
	auto target_scale = DecimalType::GetScale(target_type);
	if (source_scale <= target_scale) {
		return false;
	}
	DUCKDB_LOG_WARNING(context,
	                   "Potential loss of decimal precision while resolving a CASE expression: type %s is being cast "
	                   "to %s, reducing the scale from %d to %d. This may cause values to be rounded.",
	                   source_type.ToString(), target_type.ToString(), source_scale, target_scale);
	return true;
}

BindResult ExpressionBinder::BindExpression(CaseExpression &expr, idx_t depth) {
	// first try to bind the children of the case expression
	ErrorData error;
	vector<pair<unique_ptr<Expression>, unique_ptr<Expression>>> checks;
	for (auto &check : expr.CaseChecksMutable()) {
		auto when_expr = BindChild(check.when_expr, depth, error);
		auto then_expr = BindChild(check.then_expr, depth, error);
		checks.emplace_back(std::move(when_expr), std::move(then_expr));
	}
	auto else_expr = BindChild(expr.ElseMutable(), depth, error);
	if (error.HasError()) {
		return BindResult(std::move(error));
	}
	// the children have been successfully resolved
	// figure out the result type of the CASE expression
	auto return_type = ExpressionBinder::GetExpressionReturnType(*else_expr);
	bool warned_about_scale_reduction = false;
	for (auto &check : checks) {
		auto then_type = ExpressionBinder::GetExpressionReturnType(*check.second);
		if (!LogicalType::TryGetMaxLogicalType(context, return_type, then_type, return_type)) {
			throw BinderException(
			    expr, "Cannot mix values of type %s and %s in CASE expression - an explicit cast is required",
			    return_type.ToString(), then_type.ToString());
		}
		if (!warned_about_scale_reduction) {
			warned_about_scale_reduction = WarnCaseDecimalScaleReduction(context, then_type, return_type);
		}
	}
	if (!warned_about_scale_reduction) {
		WarnCaseDecimalScaleReduction(context, ExpressionBinder::GetExpressionReturnType(*else_expr), return_type);
	}

	// bind all the individual components of the CASE statement
	auto result = make_uniq<BoundCaseExpression>(return_type);
	for (auto &check : checks) {
		BoundCaseCheck result_check;
		result_check.when_expr =
		    BoundCastExpression::AddCastToType(context, std::move(check.first), LogicalType::BOOLEAN);
		result_check.then_expr = BoundCastExpression::AddCastToType(context, std::move(check.second), return_type);
		result->CaseChecksMutable().push_back(std::move(result_check));
	}
	result->ElseMutable() = BoundCastExpression::AddCastToType(context, std::move(else_expr), return_type);
	return BindResult(std::move(result));
}
} // namespace duckdb
