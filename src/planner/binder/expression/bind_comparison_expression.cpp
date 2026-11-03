#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_parameter_expression.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/catalog/catalog_entry/collate_catalog_entry.hpp"
#include "duckdb/common/string_util.hpp"

#include "duckdb/function/scalar/string_functions.hpp"

#include "duckdb/common/types/decimal.hpp"
#include "duckdb/planner/collation_binding.hpp"

namespace duckdb {

bool ExpressionBinder::PushCollation(ClientContext &context, unique_ptr<Expression> &source,
                                     const LogicalType &sql_type, CollationType type) {
	auto &collation_binding = CollationBinding::Get(context);
	return collation_binding.PushCollation(context, source, sql_type, type);
}

void ExpressionBinder::TestCollation(ClientContext &context, const string &collation) {
	auto expr = make_uniq_base<Expression, BoundConstantExpression>(Value(""));
	PushCollation(context, expr, LogicalType::VARCHAR_COLLATION(collation));
}

static bool SwitchVarcharComparison(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::INTERVAL:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
	case LogicalTypeId::TIME_TZ:
	case LogicalTypeId::INTEGER_LITERAL:
		return true;
	default:
		return false;
	}
}

bool BoundComparisonExpression::TryBindComparison(ClientContext &context, const LogicalType &left_type,
                                                  const LogicalType &right_type, LogicalType &result_type,
                                                  ExpressionType comparison_type) {
	LogicalType res;
	bool is_equality;
	switch (comparison_type) {
	case ExpressionType::COMPARE_EQUAL:
	case ExpressionType::COMPARE_NOTEQUAL:
	case ExpressionType::COMPARE_IN:
	case ExpressionType::COMPARE_NOT_IN:
	case ExpressionType::COMPARE_DISTINCT_FROM:
	case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
		is_equality = true;
		break;
	default:
		is_equality = false;
		break;
	}
	if (is_equality) {
		res = LogicalType::ForceMaxLogicalType(context, left_type, right_type);
	} else {
		if (!LogicalType::TryGetMaxLogicalType(context, left_type, right_type, res)) {
			return false;
		}
	}
	switch (res.id()) {
	case LogicalTypeId::DECIMAL: {
		// result is a decimal: we need the maximum width and the maximum scale over width
		vector<LogicalType> argument_types = {left_type, right_type};
		uint8_t max_width = 0, max_scale = 0, max_width_over_scale = 0;
		for (idx_t i = 0; i < argument_types.size(); i++) {
			uint8_t width, scale;
			auto can_convert = argument_types[i].GetDecimalProperties(width, scale);
			if (!can_convert) {
				result_type = res;
				return true;
			}
			max_width = MaxValue<uint8_t>(width, max_width);
			max_scale = MaxValue<uint8_t>(scale, max_scale);
			max_width_over_scale = MaxValue<uint8_t>(width - scale, max_width_over_scale);
		}
		max_width = MaxValue<uint8_t>(max_scale + max_width_over_scale, max_width);
		if (max_width > Decimal::MAX_WIDTH_DECIMAL) {
			// target width does not fit in decimal: truncate the scale (if possible) to try and make it fit
			max_width = Decimal::MAX_WIDTH_DECIMAL;
		}
		res = LogicalType::DECIMAL(max_width, max_scale);
		break;
	}
	case LogicalTypeId::VARCHAR:
		// for comparison with strings, we prefer to bind to the numeric types
		if (left_type.id() != LogicalTypeId::VARCHAR && SwitchVarcharComparison(left_type)) {
			res = LogicalType::NormalizeType(left_type);
		} else if (right_type.id() != LogicalTypeId::VARCHAR && SwitchVarcharComparison(right_type)) {
			res = LogicalType::NormalizeType(right_type);
		} else {
			// else: check if collations are compatible
			auto left_collation = StringType::GetCollation(left_type);
			auto right_collation = StringType::GetCollation(right_type);
			if (!left_collation.empty() && !right_collation.empty() && left_collation != right_collation) {
				throw BinderException("Cannot combine types with different collation!");
			}
		}
		break;
	default:
		break;
	}
	result_type = res;
	return true;
}

LogicalType BoundComparisonExpression::BindComparison(ClientContext &context, const LogicalType &left_type,
                                                      const LogicalType &right_type, ExpressionType comparison_type) {
	LogicalType result_type;
	if (!BoundComparisonExpression::TryBindComparison(context, left_type, right_type, result_type, comparison_type)) {
		throw BinderException("Cannot mix values of type %s and %s - an explicit cast is required",
		                      left_type.ToString(), right_type.ToString());
	}
	return result_type;
}

LogicalType ExpressionBinder::GetExpressionReturnType(const Expression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		if (expr.GetReturnType() == LogicalTypeId::VARCHAR && StringType::GetCollation(expr.GetReturnType()).empty()) {
			return LogicalTypeId::STRING_LITERAL;
		}
		if (expr.GetReturnType().IsIntegral()) {
			auto &constant = expr.Cast<BoundConstantExpression>();
			if (!constant.GetValue().IsNull()) {
				return LogicalType::INTEGER_LITERAL(constant.GetValue());
			}
		}
	}
	return expr.GetReturnType();
}

//! Unify the types of two bound operands and build the bound comparison over them. Returns nullptr and
//! sets the error if the two types cannot be compared.
unique_ptr<Expression> ExpressionBinder::CreateBoundComparison(ExpressionType comparison_type,
                                                               unique_ptr<Expression> left,
                                                               unique_ptr<Expression> right, ErrorData &error) {
	try {
		return BoundComparisonExpression::Bind(context, comparison_type, std::move(left), std::move(right));
	} catch (const std::exception &ex) {
		error = ErrorData(ex);
		return nullptr;
	}
}

BindResult ExpressionBinder::BindExpression(ComparisonExpression &expr, idx_t depth) {
	// first try to bind the children of the case expression
	ErrorData error;
	auto left = BindChild(expr.LeftMutable(), depth, error);
	auto right = BindChild(expr.RightMutable(), depth, error);
	if (error.HasError()) {
		return BindResult(std::move(error));
	}

	ErrorData compare_error;
	auto result = CreateBoundComparison(expr.GetExpressionType(), std::move(left), std::move(right), compare_error);
	if (!result) {
		compare_error.AddQueryLocation(expr);
		return BindResult(std::move(compare_error));
	}
	return BindResult(std::move(result));
}

} // namespace duckdb
