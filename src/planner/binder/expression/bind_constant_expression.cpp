#include "duckdb/common/types/decimal.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression_binder.hpp"

namespace duckdb {

BindResult ExpressionBinder::BindExpression(ConstantExpression &expr, idx_t depth) {
	auto value = expr.GetLiteral().ToValue();
	if (value.type().id() == LogicalTypeId::DECIMAL) {
		// a literal wider than the max_decimal_width setting is refused like a wider type: binding it to another type
		// would change its value with the setting
		const auto width = DecimalType::GetWidth(value.type());
		const auto max_width = Decimal::MaxWidth(context);
		if (width > max_width) {
			return BindResult(
			    BinderException(expr,
			                    "Decimal literal of %u digits exceeds the maximum DECIMAL width of %u (SET "
			                    "max_decimal_width to allow wider decimals)",
			                    width, max_width));
		}
	}
	return BindResult(make_uniq<BoundConstantExpression>(std::move(value)));
}

} // namespace duckdb
