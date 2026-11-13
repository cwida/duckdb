//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression/bound_operator_expression.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/expression.hpp"

namespace duckdb {

class BoundOperatorExpression : public Expression {
public:
	static constexpr const ExpressionClass TYPE = ExpressionClass::BOUND_OPERATOR;

public:
	BoundOperatorExpression(ExpressionType type, LogicalType return_type);
	BoundOperatorExpression(ExpressionType type, LogicalType return_type, unique_ptr<Expression> child);
	BoundOperatorExpression(ExpressionType type, LogicalType return_type, unique_ptr<Expression> left,
	                        unique_ptr<Expression> right);
	BoundOperatorExpression(ExpressionType type, LogicalType return_type, vector<unique_ptr<Expression>> children);

public:
	const vector<unique_ptr<Expression>> &GetChildren() const {
		return children;
	}
	vector<unique_ptr<Expression>> &GetChildrenMutable() {
		return children;
	}
	string ToString() const override;

	bool Equals(const BaseExpression &other) const override;

	unique_ptr<Expression> Copy() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<Expression> Deserialize(Deserializer &deserializer);

private:
	vector<unique_ptr<Expression>> children;
};
} // namespace duckdb
