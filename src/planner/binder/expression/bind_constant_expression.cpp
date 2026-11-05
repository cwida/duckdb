#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression_binder.hpp"

namespace duckdb {

BindResult ExpressionBinder::BindExpression(ConstantExpression &expr, idx_t depth) {
	return BindResult(make_uniq<BoundConstantExpression>(expr.GetLiteral().ToValue()));
}

} // namespace duckdb
