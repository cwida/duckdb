#include "duckdb/common/types/decimal.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression_binder.hpp"

namespace duckdb {

BindResult ExpressionBinder::BindExpression(ConstantExpression &expr, idx_t depth) {
	// a literal wider than the max_decimal_width setting binds to DOUBLE
	auto value = expr.GetLiteral().ToValue(Decimal::MaxWidth(context));
	return BindResult(make_uniq<BoundConstantExpression>(std::move(value)));
}

} // namespace duckdb
