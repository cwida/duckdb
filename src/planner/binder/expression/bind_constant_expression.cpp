#include "duckdb/common/types/decimal.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression_binder.hpp"

namespace duckdb {

BindResult ExpressionBinder::BindExpression(ConstantExpression &expr, idx_t depth) {
	// the index keys of a database written before wide decimals type their literals as those engines did
	auto value = expr.GetLiteral().ToValue(Decimal::MaxLiteralWidth(context));
	return BindResult(make_uniq<BoundConstantExpression>(std::move(value)));
}

} // namespace duckdb
