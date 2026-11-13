#include "duckdb/parser/expression/constant_expression.hpp"

#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/value.hpp"

namespace duckdb {

ConstantExpression::ConstantExpression() : ParsedExpression(ExpressionType::VALUE_CONSTANT, ExpressionClass::CONSTANT) {
}

ConstantExpression::ConstantExpression(Literal literal_p)
    : ParsedExpression(ExpressionType::VALUE_CONSTANT, ExpressionClass::CONSTANT), literal(std::move(literal_p)) {
}

string ConstantExpression::ToString() const {
	return literal.ToString();
}

Value ConstantExpression::GetValueForSerialization() const {
	// the Value form is written for storage versions before v2.0.0, whose engines type a wider literal as DOUBLE
	return literal.ToValue(Decimal::MAX_WIDTH_INT128);
}

unique_ptr<ParsedExpression> ConstantExpression::DeserializeConstant(const Value &value, Literal literal) {
	if (literal.kind != LiteralKind::INVALID) {
		return make_uniq<ConstantExpression>(std::move(literal));
	}
	return FromValue(value);
}

} // namespace duckdb
