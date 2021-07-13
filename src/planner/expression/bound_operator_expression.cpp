#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

BoundOperatorExpression::BoundOperatorExpression(ExpressionType type, LogicalType return_type)
    : Expression(type, ExpressionClass::BOUND_OPERATOR, move(return_type)) {
}

string BoundOperatorExpression::ToString() const {
	auto op = ExpressionTypeToOperator(type);
	if (!op.empty()) {
		// use the operator string to represent the operator
		if (children.size() == 1) {
			return op + "(" + children[0]->GetName() + ")";
		} else if (children.size() == 2) {
			return children[0]->GetName() + " " + op + " " + children[1]->GetName();
		}
	}
	// if there is no operator we render it as a function
	auto result = ExpressionTypeToString(type) + "(";
	result += StringUtil::Join(children, children.size(), ", ",
	                           [](const unique_ptr<Expression> &child) { return child->GetName(); });
	result += ")";
	return result;
}

bool BoundOperatorExpression::Equals(const BaseExpression *other_p) const {
	if (!Expression::Equals(other_p)) {
		return false;
	}
	auto other = (BoundOperatorExpression *)other_p;
	if (!ExpressionUtil::ListEquals(children, other->children)) {
		return false;
	}
	return true;
}

unique_ptr<Expression> BoundOperatorExpression::Copy() {
	auto copy = make_unique<BoundOperatorExpression>(type, return_type);
	copy->CopyProperties(*this);
	for (auto &child : children) {
		copy->children.push_back(child->Copy());
	}
	return move(copy);
}

} // namespace duckdb
