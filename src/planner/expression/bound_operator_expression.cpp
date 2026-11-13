#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"

namespace duckdb {

BoundOperatorExpression::BoundOperatorExpression(ExpressionType type, LogicalType return_type)
    : Expression(type, ExpressionClass::BOUND_OPERATOR, std::move(return_type)) {
}

BoundOperatorExpression::BoundOperatorExpression(ExpressionType type, LogicalType return_type,
                                                 unique_ptr<Expression> child)
    : BoundOperatorExpression(type, std::move(return_type)) {
	children.push_back(std::move(child));
}

BoundOperatorExpression::BoundOperatorExpression(ExpressionType type, LogicalType return_type,
                                                 unique_ptr<Expression> left, unique_ptr<Expression> right)
    : BoundOperatorExpression(type, std::move(return_type)) {
	children.push_back(std::move(left));
	children.push_back(std::move(right));
}

BoundOperatorExpression::BoundOperatorExpression(ExpressionType type, LogicalType return_type,
                                                 vector<unique_ptr<Expression>> children_p)
    : BoundOperatorExpression(type, std::move(return_type)) {
	children = std::move(children_p);
}

string BoundOperatorExpression::ToString() const {
	return OperatorExpression::ToString<BoundOperatorExpression, Expression>(*this);
}

bool BoundOperatorExpression::Equals(const BaseExpression &other_p) const {
	if (!Expression::Equals(other_p)) {
		return false;
	}
	auto &other = other_p.Cast<BoundOperatorExpression>();
	if (!Expression::ListEquals(children, other.children)) {
		return false;
	}
	return true;
}

unique_ptr<Expression> BoundOperatorExpression::Copy() const {
	auto copy = make_uniq<BoundOperatorExpression>(type, return_type);
	copy->CopyProperties(*this);
	for (auto &child : children) {
		copy->children.push_back(child->Copy());
	}
	return std::move(copy);
}

} // namespace duckdb
