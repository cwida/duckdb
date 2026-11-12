//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression/bound_constant_expression.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/value.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

class BoundConstantExpression : public Expression {
public:
	static constexpr const ExpressionClass TYPE = ExpressionClass::BOUND_CONSTANT;

public:
	explicit BoundConstantExpression(Value value);
	BoundConstantExpression(Value value, Identifier alias, QueryLocation location = QueryLocation());

public:
	const Value &GetValue() const {
		return value;
	}
	//! Create a constant with a different value (and the type of that value)
	unique_ptr<Expression> WithValue(Value new_value) const;
	void SetReturnType(LogicalType type) override;
	string ToString() const override;

	bool Equals(const BaseExpression &other) const override;
	hash_t Hash() const override;

	unique_ptr<Expression> Copy() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<Expression> Deserialize(Deserializer &deserializer);

private:
	Value value;
};
} // namespace duckdb
