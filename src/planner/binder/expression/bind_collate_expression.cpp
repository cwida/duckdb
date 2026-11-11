#include "duckdb/parser/expression/collate_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_parameter_expression.hpp"
#include "duckdb/planner/expression_binder.hpp"

namespace duckdb {

BindResult ExpressionBinder::BindExpression(CollateExpression &expr, idx_t depth) {
	// first try to bind the child of the cast expression
	auto result = Bind(expr.ChildMutable(), depth);
	if (result.HasError()) {
		return result;
	}
	auto child = std::move(result.expression);
	if (child->HasParameter()) {
		throw ParameterNotResolvedException();
	}
	if (child->GetReturnType().id() != LogicalTypeId::VARCHAR || child->GetReturnType().IsJSONType()) {
		throw BinderException(child->GetQueryLocation(), "collations are only supported for type varchar");
	}
	// Validate the collation, but don't use it
	auto collation_test = make_uniq_base<Expression, BoundConstantExpression>(Value(child->GetReturnType()));
	auto collation_type = LogicalType::VARCHAR_COLLATION(expr.Collation());
	PushCollation(context, collation_test, collation_type);
	child->SetReturnType(collation_type);
	return BindResult(std::move(child));
}

} // namespace duckdb
